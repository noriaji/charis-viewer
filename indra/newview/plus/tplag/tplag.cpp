/**
 * @file tplag.cpp
 * @brief After a teleport, the frame time, VRAM and what the old region still
 *        holds, on a timeline; the GL buffer compaction's switches (research/245).
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
#include "plus/rlv/rlvhooks.h"

#include "tplag.h"

#include "plus/memguard/memguard.h"
#include "plus/texprio/texprio.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llimagegl.h"
#include "llregionhandle.h"
#include "llappviewer.h"
#include "llmemory.h"
#include "llmeshrepository.h"
#include "llrhi.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llworld.h"

#include <array>
#include <string>

namespace
{
    constexpr std::array<F32, 8> REPORT_AT = { 1.f, 3.f, 10.f, 30.f, 60.f, 120.f, 180.f, 300.f };
    constexpr F32 MB = 1024.f * 1024.f;
    constexpr F32 IDLE_MIN = 0.f;
    constexpr F32 IDLE_MAX = 60.f;      // llvkgl_buffer.cpp IDLE_SECONDS_MAX (its frame history)

    LLCachedControl<bool>& log_on()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusTPLagLog", true);
        return on;
    }

    F32 now()
    {
        return llmax((F32)gFrameTimeSeconds, 0.001f);
    }

    // research/296: a timeline is open from an arrival's first line until its last (main thread).
    bool sOpen = false;

    // research/296: the discard bias, frame by frame, since the line before (main thread; BiasFrame has the terms).
    // Each frame is counted once in its kind; the reasons are counted besides (a frame can have several).
    struct BiasCounts
    {
        U32 frames = 0;
        // kind: raised, fixed at the top by the window in the background, held up by a reason, lowered, at 1
        U32 raised = 0, raised_in_background = 0, fixed_background = 0, held = 0, lowered = 0, at_one = 0;
        // reasons raised (system memory short; plus/memguard's line, part of it; over the target)
        U32 up_sys_low = 0, up_guard = 0, up_over = 0;
        // reasons held (bias above 1, not raised, not lowered): each of those that applied, and the first of them
        U32 hold_sys_free = 0, hold_over = 0, hold_over_above = 0, hold_room = 0, hold_pressure = 0, hold_guard = 0, hold_none = 0;
        U32 held_while_raising = 0;     // the frame is a rise (capped at 4, or waiting the second): no reason of the fall applies
        // bias_all: why the bias may be up for another reason than VRAM (frames, not exclusive)
        U32 ball_sys = 0, ball_guard = 0, ball_manual = 0;
        U32 first_sys_free = 0, first_over = 0, first_room = 0, first_pressure = 0, first_guard = 0;
        // the window
        U32 not_visible = 0, no_focus = 0, kept_in_front = 0, held_budget = 0;
        // research/309: how much the bias rose (while system memory was short, while over the target) and fell, and
        // how often the frame went from not low to low (a low edge: the bias is slammed to 1.5 and the emergency
        // pass over every texture runs when it is system memory)
        // (split as the counts of raises: the memory guard's low, the official system memory short, over the target;
        // the window's background fix and its return are not in these)
        F32 raised_guard_amount = 0.f, raised_sys_amount = 0.f, raised_over_amount = 0.f, lowered_amount = 0.f;
        F32 background_fixed_amount = 0.f;
        U32 low_edges = 0, low_edges_sys = 0;
    };
    // One counting of the bias frames: a TPLag timeline's, and the bias watch's (research/309).
    struct BiasTrack
    {
        BiasCounts counts;
        plus::tplag::BiasFrame last;    // the last frame's figures
        F32 prev = -1.f;                // the bias at the frame before (below 0: none since it began)
        bool prev_background = false;
        bool prev_low = false;
    };
    BiasTrack sTimelineTrack;
    BiasTrack sWatchTrack;

    constexpr F32 BIAS_EPS = 1.0e-4f;
    constexpr F32 BIAS_FALL_OVER_PCT = -0.1f;           // llviewertexture.cpp FREE_PERCENTAGE_TRESHOLD
    constexpr F32 BIAS_ROOM_SECONDS = 5.f;              // REAL_BUDGET_ROOM_WAIT

    std::string bias_line(const BiasTrack& track)
    {
        const BiasCounts& c = track.counts;
        const plus::tplag::BiasFrame& l = track.last;
        std::string line = llformat("bias frames %u: raised %u (sys mem short %u, memguard %u, over target %u; fixed with the window in the background %u), "
                                    "fixed by the background %u, held up %u (of them %u while it was being raised), lowered %u, at 1 %u",
                                    c.frames, c.raised, c.up_sys_low, c.up_guard, c.up_over, c.raised_in_background, c.fixed_background,
                                    c.held, c.held_while_raising, c.lowered, c.at_one);
        line += llformat("; held up by (each that applied / the first): free sys mem at or under its line %u/%u, not 10%% under the target %u/%u "
                         "(of them over the target %u), room under %.0f s %u/%u, texture pressure >= 2 %u/%u, memguard %u/%u, none %u",
                         c.hold_sys_free, c.first_sys_free, c.hold_over, c.first_over, c.hold_over_above, BIAS_ROOM_SECONDS, c.hold_room,
                         c.first_room, c.hold_pressure, c.first_pressure, c.hold_guard, c.first_guard, c.hold_none);
        line += llformat("; bias_all frames: system memory short within 60 s %u, memguard holds %u, RenderMaxVRAMBudget set %u",
                         c.ball_sys, c.ball_guard, c.ball_manual);
        line += llformat("; low edges %u (system memory %u); bias raised by %.2f while the memory guard was low, %.2f while system memory was short "
                         "(official), %.2f while over the target; lowered by %.2f; fixed by the window in the background by %.2f",
                         c.low_edges, c.low_edges_sys, c.raised_guard_amount, c.raised_sys_amount, c.raised_over_amount, c.lowered_amount,
                         c.background_fixed_amount);
        line += llformat("; window: not visible %u frames, no focus %u (of them with the textures kept as in front by PlusBackgroundKeepVisible, no bias fixed %u), "
                         "budget held by a snapshot %u", c.not_visible, c.no_focus, c.kept_in_front, c.held_budget);
        if (c.frames == 0)
        {
            return line + "; last frame: no frame counted";
        }
        line += llformat("; last frame: bias %.2f, textures+vertex %.0f MB (vertex %.0f), settings target %.0f MB, driver-side target %.0f MB (%s limits), "
                         "over %.1f%%, free sys mem %.0f MB (the fall needs more than %.0f), RenderMaxVRAMBudget %u, pressure %d, bias_all %d",
                         l.bias, l.used_mb, l.vertex_mb, l.settings_target_mb, l.real_target_mb,
                         !l.real_budget ? "settings" : (l.real_target_mb < l.settings_target_mb ? "driver" : "settings"),
                         l.over_pct * 100.f, l.sys_free_mb, l.fall_free_mb, l.max_vram_budget_mb, l.pressure, l.bias_all ? 1 : 0);
        return line;
    }

    // research/296: where the fetch waits, from the fetcher's own counts (tex-fetch-flow has the stages) and the mesh's.
    std::string fetch_line()
    {
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        LLTextureCache* cache = LLAppViewer::getTextureCache();
        std::string line = "; fetch";
        if (fetch)
        {
            line += llformat(": %d requests, %d in flight on HTTP, %d waiting for a slot", fetch->getNumRequests(), fetch->getNumHTTPRequests(),
                             fetch->getHttpWaitersCount());
        }
        if (cache)
        {
            cache->lockWorkers();   // as LLTextureFetch does for these two counts
            const S32 reads = cache->getNumReads();
            const S32 writes = cache->getNumWrites();
            cache->unlockWorkers();
            line += llformat(", cache reads %d writes %d", reads, writes);
        }
        line += llformat("; mesh: %u LODs pending, %d LOD / %d header / %d skin requests active", LLMeshRepository::sLODPending,
                         (S32)LLMeshRepoThread::sActiveLODRequests.load(), (S32)LLMeshRepoThread::sActiveHeaderRequests.load(),
                         (S32)LLMeshRepoThread::sActiveSkinRequests.load());
        return line;
    }

    // The arrival being reported (main thread).
    struct Timeline
    {
        U32 generation = 0;
        U64 handle = 0;
        U64 left = 0;           // the region left (0: none known)
        F32 at = 0.f;
        size_t next = 0;        // into REPORT_AT
        // Frames since the line before.
        U32 frames = 0;
        F64 frame_seconds = 0.0;
        F32 longest = 0.f;
        LLRHI::BufferCompactCounts compact_at_arrival;
    };
    Timeline sTimeline;
    U64 sLastRegion = 0;

    std::string region_label(const LLViewerRegion* regionp)
    {
        if (!regionp)
        {
            return "(none)";
        }
        if (plus::rlv::enabled() && !plus::rlv::canShowMenuLocation())
        {
            // RLVa @showloc: neither the name nor where on the grid (plus/rlv, research/248 3.4)
            return plus::rlv::regionNameForPlus(regionp->getName());
        }
        U32 x = 0, y = 0;
        from_region_handle(regionp->getHandle(), &x, &y);
        return llformat("%s (%u,%u)", regionp->getName().c_str(), x / 256, y / 256);
    }

    LLRHI::BufferCompactCounts compact_counts()
    {
        LLRHI::BufferCompactCounts counts;
        if (LLRHI::sBufferCompactCounts)
        {
            LLRHI::sBufferCompactCounts(counts);
        }
        return counts;
    }

    // The regions open: the one arrived in and the rest (the old one stays
    // until the simulator closes it, about 50 s), with their objects alive.
    std::string regions_line()
    {
        if (!LLWorld::instanceExists())
        {
            return "no world";
        }
        const LLViewerRegion* here = gAgent.getRegion();
        std::string line = llformat("here %s: %u objects alive", region_label(here).c_str(),
                                    here ? here->getNumOfActiveCachedObjects() : 0);
        U32 others = 0, others_alive = 0;
        bool left_open = false;
        for (const LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            if (!regionp || regionp == here)
            {
                continue;
            }
            ++others;
            others_alive += regionp->getNumOfActiveCachedObjects();
            left_open = left_open || (sTimeline.left && regionp->getHandle() == sTimeline.left);
        }
        line += llformat("; %u other regions open (%u objects alive)%s", others, others_alive,
                         sTimeline.left ? (left_open ? ", the region left still open" : ", the region left closed") : "");
        line += llformat("; objects %d (%d active), avatars %d", gObjectList.getNumObjects(), gObjectList.getNumActiveObjects(),
                         gObjectList.getAvatarCount());
        return line;
    }

    std::string memory_line()
    {
        std::string line;
        U64 texture_bytes = 0, usage = 0, budget = 0;
        if (LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(texture_bytes, usage, budget))
        {
            line = llformat("VRAM %.0f of %.0f MB (driver budget), texture images %.0f MB; ", (F32)usage / MB, (F32)budget / MB,
                            (F32)texture_bytes / MB);
        }
        line += llformat("GL textures %.0f MB, vertex buffers %.0f MB, %d textures in the list, discard bias %.2f, texture pressure %d",
                         (F32)(LLImageGL::getTextureBytesAllocated() >> 20), (F32)(LLVertexBuffer::getBytesAllocated() >> 20),
                         gTextureList.getNumImages(), LLViewerTexture::sDesiredDiscardBias, LLViewerTexture::sRhiTexPressure);
        const LLRHI::BufferCompactCounts pools = compact_counts();  // the pool lock only
        if (pools.gl_pool_blocks || pools.texture_pool_blocks)
        {
            line += llformat("; GL buffer pools %.0f MB in %.0f MB, texture pools %.0f MB in %.0f MB", (F32)pools.gl_pool_used / MB,
                             (F32)pools.gl_pool_blocks / MB, (F32)pools.texture_pool_used / MB, (F32)pools.texture_pool_blocks / MB);
        }
        // The backend's full report walks every buffer and texture under its lock:
        // only in development starts (LLRHI::sPeriodicLogs); TextureMemory has it once a minute.
        if (LLRHI::sPeriodicLogs && LLRHI::sMemoryReport)
        {
            line += "; " + LLRHI::sMemoryReport();
        }
        return line;
    }

    // ---- research/309: the bias watch (main thread). It opens once the bias has stayed over WATCH_OPEN_BIAS for
    // WATCH_OPEN_SECONDS, whatever started it (a teleport or not), says every WATCH_REPORT_SECONDS what the bias frames
    // and the memory guard did since the line before, and closes after WATCH_CLOSE_SECONDS at 1.
    constexpr F32 WATCH_OPEN_BIAS = 1.5f;
    constexpr F32 WATCH_OPEN_SECONDS = 20.f;
    constexpr F32 WATCH_CLOSE_BIAS = 1.05f;
    constexpr F32 WATCH_CLOSE_SECONDS = 30.f;
    constexpr F32 WATCH_REPORT_SECONDS = 60.f;
    bool sWatchOpen = false;
    F32 sWatchHighSince = -1.f;     // the bias has been over WATCH_OPEN_BIAS since (below 0: not)
    F32 sWatchLowSince = -1.f;      // ... and, while open, at or under WATCH_CLOSE_BIAS since
    F32 sWatchOpenedAt = 0.f;
    F32 sWatchLastLine = 0.f;

    void report(F32 at)
    {
        Timeline& t = sTimeline;
        const LLRHI::BufferCompactCounts c = compact_counts();
        const LLRHI::BufferCompactCounts& a = t.compact_at_arrival;
        std::string line = llformat("+%.0f s after the teleport (plus/tplag): ", at);
        line += t.frames ? llformat("%u frames since the line before, %.0f ms a frame, longest %.0f ms", t.frames,
                                    t.frame_seconds * 1000.0 / t.frames, t.longest * 1000.f)
                         : std::string("no frame since the line before");
        line += "; " + regions_line();
        line += "; " + memory_line();
        line += "; " + bias_line(sTimelineTrack);
        line += fetch_line();
        const std::string far_diag = plus::texprio::farDiagLine();
        if (!far_diag.empty())
        {
            line += "; " + far_diag;
        }
        line += llformat("; GL buffer compaction %s: %llu passes, %llu buffers given back (%.0f MB drawn lately, %.0f MB not) since the arrival",
                         LLRHI::sBufferCompact.load(std::memory_order_relaxed) ? "on" : "off",
                         (unsigned long long)(c.passes - a.passes), (unsigned long long)(c.buffers - a.buffers),
                         (F32)(c.drawn_bytes - a.drawn_bytes) / MB, (F32)(c.other_bytes - a.other_bytes) / MB);
        LL_INFOS("TPLag") << line << LL_ENDL;
        sTimelineTrack.counts = BiasCounts();       // the next line counts from here
        t.frames = 0;
        t.frame_seconds = 0.0;
        t.longest = 0.f;
    }

    // Main thread, from LLAgent::setRegion(): a teleport (state not NONE) to another region.
    void on_region_changed()
    {
        LLViewerRegion* regionp = gAgent.getRegion();
        const U64 before = sLastRegion;
        const U64 handle = regionp ? regionp->getHandle() : 0;
        sLastRegion = handle;
        if (!log_on() || !handle || before == handle || gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
        {
            return;     // off, the same region, or a walking crossing / the login
        }
        Timeline& t = sTimeline;
        t = Timeline();
        static U32 sGeneration = 0;
        t.generation = ++sGeneration;
        t.handle = handle;
        t.left = before;
        t.at = now();
        t.compact_at_arrival = compact_counts();
        sOpen = true;
        sTimelineTrack = BiasTrack();
        LL_INFOS("TPLag") << "teleport arrival in " << region_label(regionp) << " (plus/tplag, research/245): " << regions_line() << "; "
                          << memory_line() << LL_ENDL;

        const U32 generation = t.generation;
        doOnIdleRepeating([generation]()
        {
            Timeline& t = sTimeline;
            if (generation != t.generation)
            {
                return true;    // a later arrival reports for itself (and keeps the timeline open)
            }
            if (!log_on())
            {
                sOpen = false;
                return true;    // the log was turned off
            }
            const F32 frame = gFrameIntervalSeconds.value();
            ++t.frames;
            t.frame_seconds += frame;
            t.longest = llmax(t.longest, frame);
            const F32 since = now() - t.at;
            if (since < REPORT_AT[t.next])
            {
                return false;
            }
            report(REPORT_AT[t.next]);
            if (++t.next >= REPORT_AT.size())
            {
                sOpen = false;
                return true;
            }
            return false;
        });
    }

    // The compaction's switches go to the backend when they change.
    void push_compact_settings()
    {
        static LLCachedControl<bool> compact(gSavedSettings, "PlusBufferCompact", true);
        static LLCachedControl<F32> idle(gSavedSettings, "PlusBufferCompactIdle", 20.f);
        LLRHI::sBufferCompact.store(compact(), std::memory_order_relaxed);
        LLRHI::sBufferCompactIdleSeconds.store(llclamp((F32)idle(), IDLE_MIN, IDLE_MAX), std::memory_order_relaxed);
    }

    void watch(const char* name)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name).get())
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { push_compact_settings(); });
        }
    }

    void init()
    {
        gAgent.addRegionChangedCallback(&on_region_changed);
        push_compact_settings();
        watch("PlusBufferCompact");
        watch("PlusBufferCompactIdle");
    }

    void countFrame(BiasTrack& t, const plus::tplag::BiasFrame& f)
    {
        BiasCounts& c = t.counts;
        ++c.frames;
        t.last = f;
        c.not_visible += f.not_visible ? 1 : 0;
        c.no_focus += f.no_focus ? 1 : 0;
        c.kept_in_front += f.kept_in_front ? 1 : 0;
        c.held_budget += f.budget_held ? 1 : 0;
        c.ball_sys += f.bias_all && f.sys_raised ? 1 : 0;
        c.ball_guard += f.bias_all && f.guard_held ? 1 : 0;
        c.ball_manual += f.bias_all && f.max_vram_budget_mb > 0 ? 1 : 0;
        const bool first = t.prev < 0.f;
        const F32 before = first ? f.bias : t.prev;
        const bool was_bg_before = t.prev_background;
        t.prev = f.bias;
        t.prev_background = f.was_backgrounded;
        if (!first && f.is_low && !t.prev_low)
        {
            ++c.low_edges;
            c.low_edges_sys += f.sys_low ? 1 : 0;
        }
        t.prev_low = f.is_low;
        if (f.was_backgrounded)
        {
            if (!was_bg_before && f.bias > before + BIAS_EPS)
            {
                ++c.raised;
                ++c.raised_in_background;   // the frame it was fixed
                c.background_fixed_amount += f.bias - before;
            }
            else
            {
                ++c.fixed_background;
            }
            return;
        }
        if (f.bias > before + BIAS_EPS)
        {
            ++c.raised;
            (f.guard_low ? c.raised_guard_amount : f.sys_low ? c.raised_sys_amount : c.raised_over_amount) += f.bias - before;
            c.up_sys_low += f.sys_low && !f.guard_low ? 1 : 0;
            c.up_guard += f.guard_low ? 1 : 0;
            c.up_over += f.over_pct > 0.f ? 1 : 0;
            return;
        }
        if (f.bias < before - BIAS_EPS)
        {
            ++c.lowered;
            if (!was_bg_before)
            {
                c.lowered_amount += before - f.bias;    // (the frame the window came back from the background is not a fall)
            }
            return;
        }
        if (f.bias <= 1.f + BIAS_EPS)
        {
            ++c.at_one;
            return;
        }
        // Above 1 and not moving: held up. Each condition of the fall (updateClass), as it stands, from the values it
        // worked them out with (a frame that is a rise has none of them: it is counted apart)
        ++c.held;
        if (f.is_low)
        {
            ++c.held_while_raising;
            return;
        }
        const bool sys = f.sys_free_mb <= f.fall_free_mb;
        const bool over = f.over_pct >= BIAS_FALL_OVER_PCT;
        c.hold_over_above += f.over_pct > 0.f ? 1 : 0;
        const bool room = f.real_budget && f.room_s <= BIAS_ROOM_SECONDS;
        const bool pressure = f.pressure >= 2;
        const bool guard = !f.guard_may_fall;
        c.hold_sys_free += sys ? 1 : 0;
        c.hold_over += over ? 1 : 0;
        c.hold_room += room ? 1 : 0;
        c.hold_pressure += pressure ? 1 : 0;
        c.hold_guard += guard ? 1 : 0;
        if (sys)
        {
            ++c.first_sys_free;
        }
        else if (over)
        {
            ++c.first_over;
        }
        else if (room)
        {
            ++c.first_room;
        }
        else if (pressure)
        {
            ++c.first_pressure;
        }
        else if (guard)
        {
            ++c.first_guard;
        }
        else
        {
            ++c.hold_none;
        }
    }

    std::string memguard_line(const plus::memguard::Stats& g)
    {
        if (!g.has_line)
        {
            return "memory guard: no line (a discrete GPU, or off)";
        }
        return llformat("memory guard (line %u, low until %u, roomy over %u MB): free %u MB now (%u to %u in the %.0f s), low %u times (from under the line "
                        "until over its end) for %.1f s in all; "
                        "%s, %s, quiet %.0f s, over the room line %.0f s, a raise held %s; "
                        "soft/emergency states %s: emergency under %u MB, %s, %u times for %.1f s in the interval",
                        g.line_mb, g.low_end_mb, g.room_mb, g.free_now_mb, g.free_min_mb, g.free_max_mb, g.interval_seconds, g.low_edges,
                        g.low_seconds, g.low ? "low now" : "not low", g.roomy ? "roomy" : "not roomy", g.quiet_seconds, g.room_seconds,
                        g.bias_held ? "yes" : "no", g.soft_only ? "on" : "off (PlusMemGuardSoftOnly off, or no view split)",
                        g.emergency_mb, g.emergency ? "in Emergency now" : "not in Emergency", g.emergency_edges, g.emergency_seconds);
    }

    void watch_report(F32 at, const char* what)
    {
        plus::memguard::Stats guard;
        plus::memguard::takeStats(guard);
        std::string line = llformat("bias watch %s, +%.0f s (plus/tplag, research/309): ", what, at - sWatchOpenedAt);
        line += bias_line(sWatchTrack);
        line += "; " + memguard_line(guard);
        line += llformat("; the viewer's working set %.0f MB; ", (F32)LLMemory::getCurrentRSS() / MB);
        line += memory_line();
        LL_INFOS("BiasWatch") << line << LL_ENDL;
        sWatchTrack.counts = BiasCounts();
        sWatchLastLine = at;
    }

    // One frame of the watch (main thread; the frame is counted when open).
    void watch_frame(const plus::tplag::BiasFrame& f)
    {
        const F32 t = now();
        if (!sWatchOpen)
        {
            if (f.bias <= WATCH_OPEN_BIAS)
            {
                sWatchHighSince = -1.f;
                return;
            }
            if (sWatchHighSince < 0.f)
            {
                // The bias went over the line: what comes in the next 20 s is counted (and the memory guard's interval
                // begins), and said in the line when the watch opens (the build-up before it is what is looked for)
                sWatchHighSince = t;
                sWatchTrack = BiasTrack();
                plus::memguard::Stats begins;
                plus::memguard::takeStats(begins);
            }
            countFrame(sWatchTrack, f);
            if (t - sWatchHighSince < WATCH_OPEN_SECONDS)
            {
                return;
            }
            sWatchOpen = true;
            sWatchLowSince = -1.f;
            sWatchOpenedAt = t - WATCH_OPEN_SECONDS;    // (the line says how long the bias has been up)
            watch_report(t, "opened");
            return;
        }
        countFrame(sWatchTrack, f);
        if (f.bias <= WATCH_CLOSE_BIAS)
        {
            if (sWatchLowSince < 0.f)
            {
                sWatchLowSince = t;
            }
            if (t - sWatchLowSince >= WATCH_CLOSE_SECONDS)
            {
                watch_report(t, "closed");
                sWatchOpen = false;
                sWatchHighSince = -1.f;
                return;
            }
        }
        else
        {
            sWatchLowSince = -1.f;
        }
        if (t - sWatchLastLine >= WATCH_REPORT_SECONDS)
        {
            watch_report(t, "open");
        }
    }

    plus::FeatureRegistration sRegistration({ "tplag", &init });
}

namespace plus::tplag
{
    bool timelineOpen()
    {
        return sOpen;
    }

    bool biasFrameWanted(F32 bias)
    {
        if (!log_on())
        {
            sWatchOpen = false;     // PlusTPLagLog off: no watch (a timeline ends at its next frame, as before)
            sWatchHighSince = -1.f;
            return sOpen;
        }
        if (sOpen || sWatchOpen || bias > WATCH_OPEN_BIAS)
        {
            return true;
        }
        sWatchHighSince = -1.f;     // no longer high: the 20 s start again
        return false;
    }

    void noteBiasFrame(const BiasFrame& f)
    {
        if (sOpen)
        {
            countFrame(sTimelineTrack, f);
        }
        else
        {
            sTimelineTrack.prev = -1.f;     // the next timeline starts clean
        }
        watch_frame(f);
    }
}
