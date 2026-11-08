/**
 * @file memguard.cpp
 * @brief The PC short of free memory, or decoded textures piling up: large
 *        texture decodes wait and the textures go down, so the viewer does not
 *        take the PC's last memory (igpu-safe).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "memguard.h"

#include "llimage.h"     // MAX_DISCARD_LEVEL
#include "llrhi.h"
#include "llviewercontrol.h"

#include <atomic>

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace
{
    constexpr char SETTING_ON[] = "PlusMemGuard";
    constexpr char SETTING_FREE_MB[] = "PlusMemGuardFreeMB";
    constexpr char SETTING_SOFT_ONLY[] = "PlusMemGuardSoftOnly";

    constexpr U64 MB = 1024ull * 1024ull;
    constexpr U64 SHARED_LINE_MIN = 768 * MB;       // a GPU without VRAM of its own: a sixteenth of the PC's memory, 768 MB to 2 GB
    constexpr U64 SHARED_LINE_MAX = 2048 * MB;      // (research/309 section 8: it was a tenth, 768 MB to 3 GB: on a 32 GB PC with other
    constexpr U64 SHARED_LINE_DIVISOR = 16;         // heavy programs running, the free memory sat at 2.8 to 4.2 GB, always under it)
    constexpr U64 SET_LINE_MIN = 256 * MB;          // PlusMemGuardFreeMB, when set: up to half the PC's memory
    constexpr U64 LOW_END_MIN = 256 * MB;           // low ends above the line and a quarter more, at least this much more
    constexpr U64 ROOM_MIN = 512 * MB;              // the bias comes down above the line and a half more, at least this much more,
    constexpr U64 ROOM_MAX = 1024 * MB;             // ... and at most this much more (reachable on an 8 GB PC: 768 + 512 MB)
    constexpr U64 ROOM_WAIT_MS = 10000;             // ... and only once it has stayed above that this long
    constexpr U64 QUIET_WAIT_MS = 60000;            // or once not low this long (in the band)
    constexpr U64 SOFT_QUIET_WAIT_MS = 10000;       // ... 10 s with PlusMemGuardSoftOnly (research/309 M3)
    constexpr U64 EMERGENCY_MIN = 512 * MB;         // the emergency line H: max(this, L/3)
    constexpr U64 EMERGENCY_DIVISOR = 3;
    constexpr U64 EMERGENCY_END_MIN = 128 * MB;     // ends above H and a quarter more, at least this much more
    constexpr U64 SOFT_DEBOUNCE_MS = 3000;          // low this long on end before the Soft state raises the bias (M4)
    constexpr F32 SOFT_FALL_FACTOR = 3.f;           // a Soft-only raise comes down at three times the base step (M3)
    constexpr U64 PURGE_MIN_MS = 10000;             // the pass over every texture, at most this often (M4)
    constexpr F32 BIAS_BACK = 0.01f;                // the guard's raise is over once the bias is back this close
    constexpr U64 WAITING_CAP = 256 * MB;           // decoded images the main thread has not taken
    constexpr U64 SMALL_DECODE = 128 * 128 * 4;     // a decode this small never waits
    constexpr U64 UNKNOWN_DECODE = 1024 * 1024 * 4; // the size not known and not the smallest level: taken as 1024 x 1024
    constexpr U64 CHECK_MS = 250;                   // the PC's free memory read at most this often
    constexpr U64 LOW_PASS_MS = 250;                // one large decode passes this often while low
    constexpr U64 SOFT_PASS_MS = 100;               // ... while Soft (PlusMemGuardSoftOnly)
    constexpr U64 PILED_PASS_MS = 2000;             // ... and while the decoded images pass WAITING_CAP
    constexpr U64 PILED_LOG_MS = 60000;

    // Set on the main thread (0: no line: off, a discrete GPU unless set, or before the window), read by any.
    std::atomic<U64> sLine{ 0 };
    std::atomic<U64> sLowEnd{ 0 };
    std::atomic<U64> sRoom{ 0 };
    std::atomic<U64> sEmergency{ 0 };       // H (0: no line)
    std::atomic<U64> sEmergencyEnd{ 0 };
    std::atomic<bool> sGuardDecodes{ false };
    // Updated by whichever thread reads the free memory.
    std::atomic<bool> sLow{ false };
    std::atomic<bool> sEmergencyNow{ false };   // under H (until H's end)
    std::atomic<U64> sLastEmergencyMs{ 0 };     // the last time it was in Emergency (0: not since the settings were applied)
    std::atomic<U64> sLowSinceMs{ 0 };          // since when it has been low on end (0: not low)
    std::atomic<bool> sRoomy{ true };
    std::atomic<U64> sRoomSinceMs{ 0 };    // since when the free memory has been over the room line (0: not)
    std::atomic<U64> sQuietSinceMs{ 0 };   // since when it has not been low (0: low, or not known yet)
    std::atomic<U64> sNextCheckMs{ 0 };
    // research/309 (diagnostics): the interval statistics, written by the thread that reads the free memory.
    std::atomic<U32> sStatEdges{ 0 };
    std::atomic<U32> sStatEmergencyEdges{ 0 };
    std::atomic<U64> sStatEmergencyMs{ 0 };
    std::atomic<U64> sStatLowMs{ 0 };
    std::atomic<U64> sStatFreeMin{ ~0ull };     // MB
    std::atomic<U64> sStatFreeMax{ 0 };
    std::atomic<U64> sStatFreeNow{ 0 };
    std::atomic<U64> sStatLastMs{ 0 };          // the previous reading (0: none yet)
    U64 sStatSinceMs = 0;                       // main thread: the interval's start
    // Decodes (the fetch thread, and the main thread taking images).
    std::atomic<S64> sWaitingBytes{ 0 };
    std::atomic<U64> sNextPassMs{ 0 };
    std::atomic<U64> sNextPiledLogMs{ 0 };
    // Main thread (LLViewerTexture::updateClass): the discard bias when the guard first raised it (< 0: no raise
    // of the guard's in effect).
    F32 sBiasBefore = -1.f;
    U64 sLastPurgeMs = 0;       // main thread: the last pass over every texture (0: none)
    std::atomic<bool> sSoftOnlyOn{ false };   // the setting (applySettings, main thread; the decode hold reads it on the fetch thread)
    std::atomic<bool> sSplitOk{ true };       // plus/texprio's split is available (noteSplit, main thread)
    bool softActive()
    {
        return sSoftOnlyOn.load(std::memory_order_relaxed) && sSplitOk.load(std::memory_order_relaxed);
    }
    bool sPurgeOwed = false;    // main thread: a pass was held back and is still to run

    U64 nowMs()
    {
#if LL_WINDOWS
        return GetTickCount64();
#else
        return 0;
#endif
    }

    U64 totalMemory()
    {
#if LL_WINDOWS
        MEMORYSTATUSEX memory = { sizeof(memory) };
        return GlobalMemoryStatusEx(&memory) ? (U64)memory.ullTotalPhys : 0;
#else
        return 0;
#endif
    }

    // Main thread: the lines from the settings and the GPU.
    void applySettings()
    {
        U64 line = 0;
        bool guard_decodes = false;
#if LL_WINDOWS
        if (gSavedSettings.getBOOL(SETTING_ON))
        {
            const U64 total = totalMemory();
            const U64 set = (U64)gSavedSettings.getU32(SETTING_FREE_MB) * MB;
            LLRHI::DeviceInfo device;
            if (set > 0)
            {
                line = llclamp(set, SET_LINE_MIN, llmax(total / 2, SET_LINE_MIN));
                guard_decodes = true;
            }
            else if (LLRHI::deviceInfo(device) && device.shared_memory)
            {
                line = llclamp(total / SHARED_LINE_DIVISOR, SHARED_LINE_MIN, SHARED_LINE_MAX);
                guard_decodes = true;
            }
        }
#endif
        const U64 room_band = llclamp(line / 2, ROOM_MIN, ROOM_MAX);
        sLowEnd.store(line + llmin(llmax(line / 4, LOW_END_MIN), room_band), std::memory_order_relaxed);
        // research/309 M1: the emergency line (never over the line itself)
        const U64 emergency = line ? llmin(llmax(EMERGENCY_MIN, line / EMERGENCY_DIVISOR), line) : 0;
        sEmergency.store(emergency, std::memory_order_relaxed);
        sEmergencyEnd.store(emergency + llmax(emergency / 4, EMERGENCY_END_MIN), std::memory_order_relaxed);
        sEmergencyNow.store(false, std::memory_order_relaxed);
        sLastEmergencyMs.store(0, std::memory_order_relaxed);
        sLowSinceMs.store(0, std::memory_order_relaxed);
        sSoftOnlyOn = line != 0 && gSavedSettings.getBOOL(SETTING_SOFT_ONLY);
        sRoom.store(line + room_band, std::memory_order_relaxed);
        sGuardDecodes.store(line && guard_decodes, std::memory_order_relaxed);
        sLine.store(line, std::memory_order_relaxed);
        sNextCheckMs.store(0, std::memory_order_relaxed);   // read again at the next ask
        sRoomSinceMs.store(0, std::memory_order_relaxed);
        sQuietSinceMs.store(0, std::memory_order_relaxed);
        if (!line)
        {
            sBiasBefore = -1.f;
            sLow.store(false, std::memory_order_relaxed);
            sRoomy.store(false, std::memory_order_relaxed);   // roomy() is true without a line anyway
            LL_INFOS("Plus") << "Memory guard: off (a discrete GPU, or PlusMemGuard off: the official reaction alone; igpu-safe)" << LL_ENDL;
            return;
        }
        LL_INFOS("Plus") << "Memory guard: textures go down under " << (line / MB) << " MB free (until "
                         << (sLowEnd.load(std::memory_order_relaxed) / MB) << " MB; Emergency under "
                         << (sEmergency.load(std::memory_order_relaxed) / MB) << " MB, PlusMemGuardSoftOnly "
                         << (sSoftOnlyOn.load(std::memory_order_relaxed) ? "on" : "off") << "), come back over "
                         << (sRoom.load(std::memory_order_relaxed) / MB) << " MB (after " << ROOM_WAIT_MS / 1000
                         << " s, slowly); large decodes wait then and while decoded images not taken pass " << (WAITING_CAP / MB)
                         << " MB (igpu-safe)" << LL_ENDL;
    }

    // Any thread: the free memory again when CHECK_MS have passed (one thread does it).
    void refresh()
    {
#if LL_WINDOWS
        const U64 line = sLine.load(std::memory_order_relaxed);
        if (!line)
        {
            return;
        }
        const U64 now = nowMs();
        U64 next = sNextCheckMs.load(std::memory_order_relaxed);
        if (now < next || !sNextCheckMs.compare_exchange_strong(next, now + CHECK_MS, std::memory_order_relaxed))
        {
            return;
        }
        MEMORYSTATUSEX memory = { sizeof(memory) };
        if (!GlobalMemoryStatusEx(&memory))
        {
            return;
        }
        const U64 free_bytes = memory.ullAvailPhys;
        const U64 low_end = sLowEnd.load(std::memory_order_relaxed);
        const bool was_low = sLow.load(std::memory_order_relaxed);
        const bool is_low = was_low ? free_bytes < low_end : free_bytes < line;
        // research/309: the emergency state (under H, until H's end) and when this low began
        {
            const bool was_emergency = sEmergencyNow.load(std::memory_order_relaxed);
            const bool is_emergency = was_emergency ? free_bytes < sEmergencyEnd.load(std::memory_order_relaxed)
                                                    : free_bytes < sEmergency.load(std::memory_order_relaxed);
            sEmergencyNow.store(is_emergency, std::memory_order_relaxed);
            if (is_emergency)
            {
                sLastEmergencyMs.store(now, std::memory_order_relaxed);
                if (!was_emergency)
                {
                    sStatEmergencyEdges.fetch_add(1, std::memory_order_relaxed);    // (diagnostics: counted, nothing decided)
                }
            }
            {
                const U64 last = sStatLastMs.load(std::memory_order_relaxed);    // (the interval statistics: the previous reading)
                if (last && was_emergency)
                {
                    sStatEmergencyMs.fetch_add(llmin(now - last, (U64)2000), std::memory_order_relaxed);
                }
            }
            if (is_emergency != was_emergency)
            {
                const U64 mb = free_bytes / MB;
                if (is_emergency)
                {
                    LL_WARNS("Plus") << "Memory guard: Emergency: the PC has " << mb << " MB free (under " << (sEmergency.load(std::memory_order_relaxed) / MB)
                                     << " MB): " << (softActive() ? "what is on screen goes down too" : "(PlusMemGuardSoftOnly off or no split: as the guard has always been)")
                                     << LL_ENDL;
                }
                else
                {
                    LL_INFOS("Plus") << "Memory guard: out of Emergency (" << mb << " MB free, over "
                                     << (sEmergencyEnd.load(std::memory_order_relaxed) / MB) << " MB)" << LL_ENDL;
                }
            }
            if (is_low)
            {
                if (!sLowSinceMs.load(std::memory_order_relaxed))
                {
                    sLowSinceMs.store(now, std::memory_order_relaxed);
                }
            }
            else
            {
                sLowSinceMs.store(0, std::memory_order_relaxed);
            }
        }
        // research/309 (diagnostics): counted, nothing decided
        {
            const U64 free_mb = free_bytes / MB;
            const U64 last = sStatLastMs.exchange(now, std::memory_order_relaxed);
            if (last && was_low)
            {
                sStatLowMs.fetch_add(llmin(now - last, (U64)2000), std::memory_order_relaxed);
            }
            if (is_low && !was_low)
            {
                sStatEdges.fetch_add(1, std::memory_order_relaxed);
            }
            sStatFreeNow.store(free_mb, std::memory_order_relaxed);
            U64 seen = sStatFreeMin.load(std::memory_order_relaxed);
            while (free_mb < seen && !sStatFreeMin.compare_exchange_weak(seen, free_mb, std::memory_order_relaxed)) {}
            seen = sStatFreeMax.load(std::memory_order_relaxed);
            while (free_mb > seen && !sStatFreeMax.compare_exchange_weak(seen, free_mb, std::memory_order_relaxed)) {}
        }
        if (is_low)
        {
            sQuietSinceMs.store(0, std::memory_order_relaxed);
        }
        else if (!sQuietSinceMs.load(std::memory_order_relaxed))
        {
            sQuietSinceMs.store(now, std::memory_order_relaxed);
        }
        if (free_bytes >= sRoom.load(std::memory_order_relaxed))
        {
            U64 since = sRoomSinceMs.load(std::memory_order_relaxed);
            if (!since)
            {
                since = now;
                sRoomSinceMs.store(now, std::memory_order_relaxed);
            }
            sRoomy.store(now - since >= ROOM_WAIT_MS, std::memory_order_relaxed);
        }
        else
        {
            sRoomSinceMs.store(0, std::memory_order_relaxed);
            sRoomy.store(false, std::memory_order_relaxed);
        }
        if (is_low != was_low)
        {
            sLow.store(is_low, std::memory_order_relaxed);
            if (is_low)
            {
                LL_WARNS("Plus") << "Memory guard: the PC has " << (free_bytes / MB) << " MB free (under " << (line / MB)
                                 << " MB): " << (softActive() ? "Soft: what is off screen goes down (after 3 s), large decodes wait (one every "
                                                                 : "textures go down and large decodes wait (one every ")
                                 << (softActive() ? SOFT_PASS_MS : LOW_PASS_MS) << " ms) until " << (low_end / MB) << " MB are free"
                                 << (softActive() ? "; Emergency under " : "") << (softActive() ? std::to_string(sEmergency.load(std::memory_order_relaxed) / MB) + " MB" : std::string())
                                 << LL_ENDL;
            }
            else
            {
                LL_INFOS("Plus") << "Memory guard: " << (free_bytes / MB) << " MB free again: decodes go on; textures come back over "
                                 << (sRoom.load(std::memory_order_relaxed) / MB) << " MB free (after " << ROOM_WAIT_MS / 1000 << " s)" << LL_ENDL;
            }
        }
#endif
    }

    void onUIReady()
    {
        applySettings();
        for (const char* name : { SETTING_ON, SETTING_FREE_MB, SETTING_SOFT_ONLY })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(name).get())
            {
                control->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { applySettings(); });
            }
        }
    }

    plus::FeatureRegistration sRegistration({ "memguard", {}, &onUIReady });
}

namespace plus
{
namespace memguard
{
    bool low()
    {
        refresh();
        // The line read too: one turned off as another thread updated the state is not left low.
        return sLine.load(std::memory_order_relaxed) && sLow.load(std::memory_order_relaxed);
    }

    bool roomy()
    {
        refresh();
        return !sLine.load(std::memory_order_relaxed) || sRoomy.load(std::memory_order_relaxed);
    }

    void takeStats(Stats& stats)
    {
        stats = Stats();
        const U64 line = sLine.load(std::memory_order_relaxed);
        const U64 now = nowMs();
        const U64 since = sStatSinceMs;
        sStatSinceMs = now;
        stats.has_line = line != 0;
        stats.low_edges = sStatEdges.exchange(0, std::memory_order_relaxed);
        stats.emergency_edges = sStatEmergencyEdges.exchange(0, std::memory_order_relaxed);
        stats.emergency_mb = (U32)(sEmergency.load(std::memory_order_relaxed) / MB);
        stats.emergency = line && sEmergencyNow.load(std::memory_order_relaxed);
        stats.soft_only = line && softActive();
        stats.emergency_seconds = (F32)sStatEmergencyMs.exchange(0, std::memory_order_relaxed) / 1000.f;
        stats.low_seconds = (F32)sStatLowMs.exchange(0, std::memory_order_relaxed) / 1000.f;
        stats.interval_seconds = since ? (F32)(now - since) / 1000.f : 0.f;
        const U64 free_min = sStatFreeMin.exchange(~0ull, std::memory_order_relaxed);
        stats.free_min_mb = free_min == ~0ull ? 0 : (U32)free_min;
        stats.free_max_mb = (U32)sStatFreeMax.exchange(0, std::memory_order_relaxed);
        stats.free_now_mb = (U32)sStatFreeNow.load(std::memory_order_relaxed);
        stats.line_mb = (U32)(line / MB);
        stats.low_end_mb = (U32)(sLowEnd.load(std::memory_order_relaxed) / MB);
        stats.room_mb = (U32)(sRoom.load(std::memory_order_relaxed) / MB);
        stats.low = line && sLow.load(std::memory_order_relaxed);
        stats.roomy = !line || sRoomy.load(std::memory_order_relaxed);
        stats.bias_held = biasHeld();
        const U64 quiet = sQuietSinceMs.load(std::memory_order_relaxed);
        stats.quiet_seconds = quiet && now > quiet ? (F32)(now - quiet) / 1000.f : 0.f;    // (another thread may have written a later time)
        const U64 room_since = sRoomSinceMs.load(std::memory_order_relaxed);
        stats.room_seconds = room_since && now > room_since ? (F32)(now - room_since) / 1000.f : 0.f;
    }

    void noteBias(bool raising, F32 bias)
    {
        if (raising)
        {
            if (sBiasBefore < 0.f)
            {
                sBiasBefore = bias;
            }
        }
        else if (sBiasBefore >= 0.f && (bias <= sBiasBefore + BIAS_BACK || !sLine.load(std::memory_order_relaxed)))
        {
            sBiasBefore = -1.f;     // back where the guard found it (or no line now): as before from here
        }
    }

    bool biasHeld()
    {
        return sBiasBefore >= 0.f;
    }

    // The answer from the state as it stands (no refresh): biasMayFall's rule, once.
    bool biasMayFallPeek()
    {
        if (!biasHeld())
        {
            return true;    // raised by something else (the VRAM budget): the official rules alone
        }
        if (!sLine.load(std::memory_order_relaxed) || sRoomy.load(std::memory_order_relaxed))
        {
            return true;    // roomy(): true without a line
        }
        const U64 quiet = sQuietSinceMs.load(std::memory_order_relaxed);
        return quiet && nowMs() - quiet >= (softActive() ? SOFT_QUIET_WAIT_MS : QUIET_WAIT_MS);
    }

    void noteSplit(bool available)
    {
        sSplitOk.store(available, std::memory_order_relaxed);
    }

    bool softOnly()
    {
        return softActive() && sLine.load(std::memory_order_relaxed) != 0;
    }

    GuardState guardState()
    {
        refresh();
        // One reading of each, the emergency first (an Emergency is low)
        const bool emergency = sEmergencyNow.load(std::memory_order_relaxed);
        const bool low = emergency || sLow.load(std::memory_order_relaxed);
        if (!sLine.load(std::memory_order_relaxed) || !low)
        {
            return GuardState::Normal;
        }
        if (emergency)
        {
            return GuardState::Emergency;
        }
        const U64 since = sLowSinceMs.load(std::memory_order_relaxed);
        return since && nowMs() - since >= SOFT_DEBOUNCE_MS ? GuardState::Soft : GuardState::SoftWait;
    }

    bool biasHeldHard()
    {
        // With PlusMemGuardSoftOnly a held raise is never bias_all by itself: an Emergency in it is what sys_raised holds
        // (its 60 s). Off: every held raise, as before
        return biasHeld() && !softActive();
    }

    F32 fallStepCap(F32 base)
    {
        // Held, with the setting, and no Emergency in the last 60 s: the off-screen textures alone came down
        const U64 last = sLastEmergencyMs.load(std::memory_order_relaxed);
        const bool emergency_lately = last && nowMs() - last < 60000;
        return softActive() && biasHeld() && !emergency_lately ? base * SOFT_FALL_FACTOR : base;
    }

    bool emergencyPurgeDue(bool guard_only)
    {
        if (!guard_only || !softActive())
        {
            sPurgeOwed = false;
            return true;    // as before: at every edge
        }
        const U64 now = nowMs();
        if (sLastPurgeMs && now - sLastPurgeMs < PURGE_MIN_MS)
        {
            sPurgeOwed = true;      // held back: owed until it may run
            return false;
        }
        sLastPurgeMs = now;
        sPurgeOwed = false;
        return true;
    }

    bool purgeOwed()
    {
        return sPurgeOwed;
    }

    void dropPurgeOwed()
    {
        sPurgeOwed = false;
    }

    bool biasMayFall()
    {
        if (!biasHeld())
        {
            return true;    // (as biasMayFallPeek: no need to read the free memory)
        }
        refresh();
        return biasMayFallPeek();
    }

    U64 decodedBytes(S32 width, S32 height, S32 components, S32 discard)
    {
        if (width <= 0 || height <= 0)
        {
            // A texture's first request, when nothing is known of it, is at the smallest level (createRequest).
            return discard >= MAX_DISCARD_LEVEL ? 0 : UNKNOWN_DECODE;
        }
        discard = llclamp(discard, 0, 15);
        const U64 w = (U64)llmax(width >> discard, 1);
        const U64 h = (U64)llmax(height >> discard, 1);
        return w * h * (U64)(components > 0 ? llmin(components, 4) : 4);
    }

    U32 decodeHoldMs(bool exempt, U64 bytes)
    {
        if (exempt || bytes <= SMALL_DECODE || !sGuardDecodes.load(std::memory_order_relaxed))
        {
            return 0;
        }
        const S64 waiting = sWaitingBytes.load(std::memory_order_relaxed);
        if (waiting > (S64)WAITING_CAP)
        {
            // The main thread is not taking them (a slow frame, a long step of the login).
            const U64 now = nowMs();
            U64 next_log = sNextPiledLogMs.load(std::memory_order_relaxed);
            if (now >= next_log && sNextPiledLogMs.compare_exchange_strong(next_log, now + PILED_LOG_MS, std::memory_order_relaxed))
            {
                LL_INFOS("Plus") << "Memory guard: " << (waiting / (S64)MB) << " MB of decoded textures not taken yet (over "
                                 << (WAITING_CAP / MB) << " MB): large decodes wait, one every " << PILED_PASS_MS << " ms" << LL_ENDL;
            }
            return (U32)PILED_PASS_MS;
        }
        if (!low())
        {
            return 0;
        }
        // Soft (research/309): large decodes pass faster than in an Emergency (the off-screen textures go down for it)
        return softActive() && !sEmergencyNow.load(std::memory_order_relaxed) ? (U32)SOFT_PASS_MS : (U32)LOW_PASS_MS;
    }

    bool takeDecodePass(U32 period_ms)
    {
        const U64 now = nowMs();
        U64 next = sNextPassMs.load(std::memory_order_relaxed);
        return now >= next && sNextPassMs.compare_exchange_strong(next, now + period_ms, std::memory_order_relaxed);
    }

    void addWaiting(S64 delta)
    {
        if (delta)
        {
            sWaitingBytes.fetch_add(delta, std::memory_order_relaxed);
        }
    }
}
}
