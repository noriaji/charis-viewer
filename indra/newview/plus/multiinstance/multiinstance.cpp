/**
 * @file multiinstance.cpp
 * @brief Charis viewers of one user on one GPU make room for the one in front (plus/multiinstance, research/337)
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
#include "plus/multiinstance/multiinstance.h"
#include "plus/background/background.h"

#include "hbxxh.h"
#include "llagent.h"
#include "llapp.h"
#include "lldir.h"
#include "llfocusmgr.h"
#include "llrhi.h"
#include "llstring.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <aclapi.h>     // GetSecurityInfo
#include <psapi.h>      // GetMappedFileNameW (kernel32's K32GetMappedFileNameW)
#include <sddl.h>       // ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW
#endif

namespace plus::multiinstance
{
#if LL_WINDOWS
    namespace
    {
        // One clock for all of it: GetTickCount64, milliseconds since the computer started. It is the system's, so the
        // others' heartbeats and front times compare with ours.
        constexpr U64 TICK_MS = 1000;               // the slot's write and the others' reading
        constexpr U64 HEARTBEAT_STALE_MS = 5000;    // a slot not written for this long does not count (five writes missed)
        constexpr U64 CLOCK_SKEW_MS = 1000;         // a time this far ahead of ours is still taken (the reads are not at one instant)
        constexpr U64 YIELD_AFTER_MS = 2000;        // another viewer in front this long: this one yields (not on a passing Alt+Tab)
        constexpr U64 SHARE_LOWER_AFTER_MS = 5000;  // a smaller VRAM share applies once it has held this long
        constexpr U64 LOG_MIN_MS = 5000;            // between two state lines
        constexpr U64 ASK_AGAIN_MS = 30000;         // a process that could not be asked is asked again after this
        // A slot not written for this long may be taken even when its process cannot be asked (review low-1): far longer
        // than a viewer of ours is held up (a large snapshot, a load: seconds), and a viewer that was would only leave the
        // registry at its next tick (its slot is gone): the worst is "as without this feature"
        constexpr U64 ABANDONED_MS = 10 * 60 * 1000;

        constexpr F32 WEIGHT_FRONT = 2.f;
        constexpr F32 WEIGHT_SEEN = 1.f;
        constexpr F32 WEIGHT_HIDDEN = 0.5f;
        constexpr U32 MAX_COUNTED = 4;              // at most this many others count in the weights (research/337 section 7)
        constexpr F32 SHARE_FLOOR_MB = 1024.f;      // the share is never under this ...
        constexpr F32 SHARE_FLOOR_OF_BUDGET = 0.25f;    // ... nor under this part of the driver's budget
        constexpr S32 FPS_CAP_MIN = 10;             // PlusMultiBackgroundFps above 0 is held to this range
        constexpr S32 FPS_CAP_MAX = 120;
        constexpr U32 MB_MAX = 1u << 20;            // a figure over 1 TB is taken as not known

        // The registry's layout. Version 1 is in the name too: another layout gets a registry of its own.
        constexpr U32 MAGIC = 0x494D4843;           // "CHMI"
        constexpr U32 LAYOUT_VERSION = 1;
        constexpr U32 SLOT_COUNT = 8;
        constexpr size_t REGISTRY_BYTES = 4096;
        constexpr int READ_ATTEMPTS = 4;            // a slot read while its owner writes it is read again this many times

        struct Header
        {
            U32 magic;          // 0 until set; set (compare-exchange) after the fields below, by whoever finds it 0
            U32 version;
            U32 header_bytes;
            U32 slot_bytes;
            U32 slot_count;
            U32 reserved[11];
        };

        // Numbers only: no command, handle, path or name.
        struct Slot
        {
            U32 seq;            // odd while its owner writes the fields below (the owner alone changes it)
            U32 pid;            // 0: free. Taken with a compare-exchange, given back last
            U64 start_100ns;    // the process's creation time (FILETIME): a process id used again is told apart. 0: not filled
            U64 luid;           // the GPU that renders (LLRHI::DeviceInfo::luid); 0: not known
            U64 beat_ms;        // at its last write
            U64 focus_since_ms; // since when it has been in front; 0: not in front
            U64 cache_hash;     // XXH64 of its cache folder: which caches it shares (the log; research/337 stage 2)
            U32 textures_mb;    // the figures below are for the log only, never for a decision; 0: not known
            U32 usage_mb;
            U32 budget_mb;
            U8 focused;         // 1: in front (it has the focus and is not minimized)
            U8 seen;            // 1: in front, or without focus but seen (plus::background::treatAsForeground)
            U8 reserved[2];
        };

        static_assert(sizeof(Header) == 64 && sizeof(Slot) == 64, "the registry's layout is version 1's");
        static_assert(sizeof(Header) + SLOT_COUNT * sizeof(Slot) <= REGISTRY_BYTES, "the registry fits its 4 KB");
        static_assert(sizeof(LONG) == sizeof(U32), "the Interlocked functions work on the 32-bit fields");

        volatile LONG* asLong(U32& field)
        {
            return reinterpret_cast<volatile LONG*>(&field);
        }

        const volatile LONG* asLong(const U32& field)
        {
            return reinterpret_cast<const volatile LONG*>(&field);
        }

        // What is known of another slot's process, asked once for each (process id, start time) seen in the slot.
        enum class Peer : U8
        {
            NotAsked,
            Ours,       // alive, with that start time and this program's file name: it counts
            Gone,       // no such process, it ended, or another process now has that id: its slot may be taken
            NotOurs,    // alive with that start time, another program: never counted, its slot left alone
            Unknown     // it could not be asked (no access, a call failed): not counted, its slot left alone
        };

        struct PeerCheck
        {
            U32 pid = 0;
            U64 start_100ns = 0;
            Peer verdict = Peer::NotAsked;
            U64 asked_ms = 0;
        };

        enum class View : U8
        {
            Hidden,     // minimized, or cannot be seen
            Seen,       // without focus but seen
            Front       // in front
        };

        struct PeerSeen
        {
            U32 pid = 0;
            View view = View::Hidden;
            U64 focus_since_ms = 0;
            U32 textures_mb = 0;
            U32 usage_mb = 0;
            U32 budget_mb = 0;
            bool same_cache = false;
        };

        // What the state line shows; a line comes when it changes.
        struct Shown
        {
            U32 others = 0;
            U32 front_pid = 0;      // the viewer in front (this one's id or another's); 0: none of them
            bool yielding = false;
            bool lowered = false;   // the priority is below normal (this feature lowered it)
            S32 fps_cap = 0;
            S32 share_pct = 100;    // 100: no share
            bool share_floored = false; // the share applied is the floor
            bool operator==(const Shown&) const = default;
        };

        // This process.
        bool sInitDone = false;
        U32 sPid = 0;
        U64 sStart100ns = 0;
        U64 sLuid = 0;
        U64 sCacheHash = 0;
        std::wstring sExeName;      // this program's file name, without the folder

        // The registry. sSlot < 0: not in it (then nothing below is used).
        HANDLE sMapping = nullptr;
        U8* sView = nullptr;
        S32 sSlot = -1;
        U64 sLastTickMs = 0;
        U64 sFocusSinceMs = 0;      // 0: not in front
        PeerCheck sChecks[SLOT_COUNT];
        PeerSeen sOthers[SLOT_COUNT];
        U32 sOtherCount = 0;        // live viewers of ours on this GPU, as of the last tick

        // B: yielding.
        bool sYielding = false;
        S32 sFpsCap = 0;            // 0: none
        bool sPriorityLowered = false;

        // A: the VRAM share. 1: no share.
        F32 sShareFraction = 1.f;
        U64 sShareLowerSinceMs = 0; // 0: no smaller share waiting
        F32 sShareLowerTo = 1.f;
        VRAMReading sReading;       // the last frame's, with the driver's budget

        Shown sShown;
        U64 sLastLogMs = 0;

        Header* header()
        {
            return reinterpret_cast<Header*>(sView);
        }

        Slot* slotAt(U32 index)
        {
            return reinterpret_cast<Slot*>(sView + sizeof(Header)) + index;
        }

        S32 fpsCapSetting()
        {
            static LLCachedControl<S32> setting(gSavedSettings, "PlusMultiBackgroundFps", 30);
            const S32 fps = setting;
            return fps <= 0 ? 0 : llclamp(fps, FPS_CAP_MIN, FPS_CAP_MAX);
        }

        bool lowPrioritySetting()
        {
            static LLCachedControl<bool> setting(gSavedSettings, "PlusMultiBackgroundPriority", true);
            return setting;
        }

        bool shareSetting()
        {
            static LLCachedControl<bool> setting(gSavedSettings, "PlusMultiShareVRAM", false);
            return setting;
        }

        U64 fileTimeValue(const FILETIME& time)
        {
            return ((U64)time.dwHighDateTime << 32) | time.dwLowDateTime;
        }

        U32 toMB(F32 mb)
        {
            return std::isfinite(mb) && mb > 0.f ? (U32)llmin(mb, (F32)MB_MAX) : 0;
        }

        U32 checkedMB(U32 mb)
        {
            return mb <= MB_MAX ? mb : 0;
        }

        // The share (A) for a reading and a part of the weights (1: none). The one calculation, for the frame's texture
        // target (vramShare) and for the state line (with the reading of the last frame and the part as of now: review 3
        // low-1, the line of the second the part changes used to show the frame's share from before it). limit_mb 0 when
        // the part is 1 or the reading has no driver's budget.
        // This viewer's part of the budget less the margin, less what else of VRAM it takes (render targets, buffers, free
        // space in blocks: updateClass's "other"), for its textures (review mid-2: without it the viewers' parts add up past
        // the budget by each one's other). The floor can still put the sum over the budget (on an 8 GB GPU the one behind
        // is at its floor: research/337 M2): the driver's own limit (real_target) and the texture pressure are still there
        // for that.
        VRAMShare shareFor(const VRAMReading& reading, F32 fraction)
        {
            VRAMShare share;
            if (fraction >= 1.f || !std::isfinite(reading.budget_mb) || !std::isfinite(reading.margin_mb) || reading.budget_mb <= 0.f)
            {
                return share;
            }
            share.other_mb = std::isfinite(reading.usage_mb) && std::isfinite(reading.textures_mb) ? llmax(reading.usage_mb - reading.textures_mb, 0.f)
                                                                                                    : 0.f;
            const F32 floor_mb = llmax(SHARE_FLOOR_MB, reading.budget_mb * SHARE_FLOOR_OF_BUDGET);
            share.part_mb = (reading.budget_mb - reading.margin_mb) * fraction;
            share.floored = share.part_mb - share.other_mb <= floor_mb;
            share.limit_mb = share.floored ? floor_mb : share.part_mb - share.other_mb;
            return share;
        }

        // A heartbeat under HEARTBEAT_STALE_MS old, and not ahead of ours by more than the skew.
        bool freshBeat(U64 beat_ms, U64 now_ms)
        {
            return beat_ms != 0 && beat_ms <= now_ms + CLOCK_SKEW_MS && now_ms - llmin(beat_ms, now_ms) < HEARTBEAT_STALE_MS;
        }

        // A heartbeat ABANDONED_MS old or older (0 does not count: a slot between its taking and its first write, or being
        // given back; nor does one from the future).
        bool abandonedBeat(U64 beat_ms, U64 now_ms)
        {
            return beat_ms != 0 && beat_ms <= now_ms && now_ms - beat_ms >= ABANDONED_MS;
        }

        const wchar_t* fileNamePart(const wchar_t* path)
        {
            const wchar_t* name = path;
            for (const wchar_t* c = path; *c; ++c)
            {
                if (*c == L'\\' || *c == L'/')
                {
                    name = c + 1;
                }
            }
            return name;
        }

        // In front: the focus, and not minimized.
        bool appInFront()
        {
            LLWindow* window = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
            return window && gFocusMgr.getAppHasFocus() && !window->getMinimized();
        }

        // This window: in front, seen without focus, or neither. Keeps since when it has been in front.
        View ownView(U64 now_ms)
        {
            const bool front = appInFront();
            sFocusSinceMs = front ? (sFocusSinceMs ? sFocusSinceMs : llmax(now_ms, (U64)1)) : 0;
            return front ? View::Front : plus::background::treatAsForeground() ? View::Seen : View::Hidden;
        }

        // Asks Windows about another slot's process. start_100ns 0 (its slot could not be read whole): only "gone" or
        // "not known" can be told.
        Peer askProcess(U32 pid, U64 start_100ns)
        {
            HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process)
            {
                return GetLastError() == ERROR_INVALID_PARAMETER ? Peer::Gone : Peer::Unknown;
            }
            Peer verdict = Peer::Unknown;
            FILETIME created, ended, kernel, user;
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
            {
                verdict = Peer::Gone;
            }
            else if (start_100ns != 0 && GetProcessTimes(process, &created, &ended, &kernel, &user))
            {
                if (fileTimeValue(created) != start_100ns)
                {
                    verdict = Peer::Gone;
                }
                else
                {
                    wchar_t path[1024];
                    DWORD length = (DWORD)LL_ARRAY_SIZE(path);
                    if (QueryFullProcessImageNameW(process, 0, path, &length) && length < LL_ARRAY_SIZE(path))
                    {
                        path[length] = L'\0';
                        verdict = CompareStringOrdinal(fileNamePart(path), -1, sExeName.c_str(), -1, TRUE) == CSTR_EQUAL ? Peer::Ours
                                                                                                                     : Peer::NotOurs;
                    }
                }
            }
            CloseHandle(process);
            return verdict;
        }

        // The verdict for the process in slot index, asked only when the slot shows a (process id, start time) not
        // asked about before, or ASK_AGAIN_MS after it could not be asked.
        Peer checkPeer(U32 index, U32 pid, U64 start_100ns, U64 now_ms)
        {
            PeerCheck& check = sChecks[index];
            if (check.verdict == Peer::NotAsked || check.pid != pid || check.start_100ns != start_100ns ||
                (check.verdict == Peer::Unknown && now_ms - check.asked_ms >= ASK_AGAIN_MS))
            {
                check.pid = pid;
                check.start_100ns = start_100ns;
                check.verdict = askProcess(pid, start_100ns);
                check.asked_ms = now_ms;
            }
            return check.verdict;
        }

        // A copy of a slot as its owner last wrote it whole (the sequence count even and the same before and after).
        // False: its owner was writing it at each attempt (or it was left half written).
        bool readSlot(const Slot* slot, Slot& out)
        {
            for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt)
            {
                const LONG before = ReadAcquire(asLong(slot->seq));
                if ((before & 1) == 0)
                {
                    std::memcpy(&out, slot, sizeof(Slot));
                    std::atomic_thread_fence(std::memory_order_acquire);    // the copy is read before the count again
                    if (ReadAcquire(asLong(slot->seq)) == before)
                    {
                        return true;
                    }
                }
                YieldProcessor();
            }
            return false;
        }

        // The owner's write of its own slot: the count odd, the fields, the count even (each a full barrier).
        U32 beginWrite(Slot* slot)
        {
            const U32 now = (U32)ReadAcquire(asLong(slot->seq));
            const U32 odd = (now & 1) ? now + 2 : now + 1;     // odd, and not the value a reader may have seen
            InterlockedExchange(asLong(slot->seq), (LONG)odd);
            return odd;
        }

        void endWrite(Slot* slot, U32 odd)
        {
            InterlockedExchange(asLong(slot->seq), (LONG)(odd + 1));
        }

        void writeOwnSlot(U64 now_ms, View view)
        {
            Slot* slot = slotAt((U32)sSlot);
            const U32 odd = beginWrite(slot);
            slot->start_100ns = sStart100ns;
            slot->luid = sLuid;
            slot->beat_ms = now_ms;
            slot->focus_since_ms = sFocusSinceMs;
            slot->cache_hash = sCacheHash;
            slot->textures_mb = toMB(sReading.textures_mb);
            slot->usage_mb = toMB(sReading.usage_mb);
            slot->budget_mb = toMB(sReading.budget_mb);
            slot->focused = view == View::Front ? 1 : 0;
            slot->seen = view != View::Hidden ? 1 : 0;
            endWrite(slot, odd);
        }

        // The slot emptied, then given back (the id last: after that another viewer may take it and write it).
        void giveBackSlot()
        {
            Slot* slot = slotAt((U32)sSlot);
            if ((U32)ReadAcquire(asLong(slot->pid)) != sPid)
            {
                return;     // not ours any more: not touched
            }
            const U32 odd = beginWrite(slot);
            slot->start_100ns = 0;
            slot->luid = 0;
            slot->beat_ms = 0;
            slot->focus_since_ms = 0;
            slot->cache_hash = 0;
            slot->textures_mb = slot->usage_mb = slot->budget_mb = 0;
            slot->focused = slot->seen = 0;
            endWrite(slot, odd);
            InterlockedCompareExchange(asLong(slot->pid), 0, (LONG)sPid);
        }

        // Which slots a pass of takeSlot may take.
        enum class TakePass : U8
        {
            Free,       // free, ours (a viewer that ended had this process's id), or its process gone
            Abandoned   // its process cannot be asked and it was not written for ABANDONED_MS (it may be alive: last)
        };

        // Whether slot (held by holder, not 0 and not this process) may be taken in this pass. Taken only when its process
        // is gone: a stale heartbeat alone may be a viewer held up for seconds (a large snapshot), which would write its
        // slot again. A process that cannot be asked (its id now a service's or an elevated program's) is taken for gone
        // only after ABANDONED_MS without a write, and only once no slot is free (re-review low-4).
        bool mayTake(const Slot* slot, U32 holder, U64 now_ms, TakePass pass)
        {
            Slot copy;
            const bool whole = readSlot(slot, copy);
            if (whole && freshBeat(copy.beat_ms, now_ms))
            {
                return false;
            }
            const Peer verdict = askProcess(holder, whole ? copy.start_100ns : 0);
            return pass == TakePass::Free ? verdict == Peer::Gone
                                          : verdict == Peer::Unknown && whole && abandonedBeat(copy.beat_ms, now_ms);
        }

        // Takes a free slot or one whose process is gone; only when there is none, one abandoned (TakePass). False: all
        // are taken.
        bool takeSlot(U64 now_ms)
        {
            for (const TakePass pass : { TakePass::Free, TakePass::Abandoned })
            {
                for (U32 index = 0; index < SLOT_COUNT; ++index)
                {
                    Slot* slot = slotAt(index);
                    const U32 holder = (U32)ReadAcquire(asLong(slot->pid));
                    bool taken = false;
                    if (holder == 0)
                    {
                        taken = InterlockedCompareExchange(asLong(slot->pid), (LONG)sPid, 0) == 0;
                    }
                    else if (holder == sPid)
                    {
                        // A viewer that ended had this id before this process: its slot is ours to write. Not taken with
                        // a compare-exchange: another viewer starting at this moment may judge it gone (this id alive,
                        // another start time) and take it in the microseconds before our first write; then this one finds
                        // its slot written over at its next tick and leaves the registry (as without this feature). Left
                        // so (review low-5): the window is microseconds and the outcome harmless.
                        taken = true;
                    }
                    else if (mayTake(slot, holder, now_ms, pass))
                    {
                        taken = InterlockedCompareExchange(asLong(slot->pid), (LONG)sPid, (LONG)holder) == (LONG)holder;
                    }
                    if (taken)
                    {
                        sSlot = (S32)index;
                        writeOwnSlot(now_ms, ownView(now_ms));
                        return true;
                    }
                }
            }
            return false;
        }

        // Whoever finds the header empty fills it (the same values whoever it is), the magic last. Then it must be
        // version 1's.
        bool headerReady()
        {
            Header* head = header();
            if (ReadAcquire(asLong(head->magic)) == 0)
            {
                head->version = LAYOUT_VERSION;
                head->header_bytes = (U32)sizeof(Header);
                head->slot_bytes = (U32)sizeof(Slot);
                head->slot_count = SLOT_COUNT;
                InterlockedCompareExchange(asLong(head->magic), (LONG)MAGIC, 0);
            }
            return (U32)ReadAcquire(asLong(head->magic)) == MAGIC && head->version == LAYOUT_VERSION &&
                   head->header_bytes == sizeof(Header) && head->slot_bytes == sizeof(Slot) && head->slot_count == SLOT_COUNT;
        }

        void closeRegistry()
        {
            if (sView)
            {
                UnmapViewOfFile(sView);
                sView = nullptr;
            }
            if (sMapping)
            {
                CloseHandle(sMapping);
                sMapping = nullptr;
            }
        }

        // sid as an SID string is text (plus::userSidText()).
        bool sidIs(PSID sid, const std::wstring& text)
        {
            LPWSTR sid_text = nullptr;
            const bool same = sid && IsValidSid(sid) && ConvertSidToStringSidW(sid, &sid_text) && text == sid_text;
            if (sid_text)
            {
                LocalFree(sid_text);
            }
            return same;
        }

        // The access list as makeMapping makes it: one entry, not inherited, letting sid in. (Its "protected" bit is not
        // asked: what lets a process in is the entries, and an object of the kernel's namespace inherits nothing after it
        // is made; whether the bit is kept on such an object is not something to rely on.)
        bool daclIsOnly(PACL dacl, const std::wstring& sid)
        {
            void* ace = nullptr;
            return dacl && dacl->AceCount == 1 && GetAce(dacl, 0, &ace) && static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                   (static_cast<ACE_HEADER*>(ace)->AceFlags & INHERITED_ACE) == 0 && sidIs(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, sid);
        }

        // No integrity label (the creator was of medium integrity or higher: a lower one gets its own label), or labels
        // of medium or higher only. A process of low integrity cannot write such an object.
        bool labelMediumOrHigher(PACL sacl)
        {
            for (DWORD i = 0; sacl && i < sacl->AceCount; ++i)
            {
                void* ace = nullptr;
                if (!GetAce(sacl, i, &ace))
                {
                    return false;
                }
                if (static_cast<ACE_HEADER*>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                {
                    continue;
                }
                PSID label = &static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace)->SidStart;
                if (!IsValidSid(label) || *GetSidSubAuthorityCount(label) == 0 ||
                    *GetSidSubAuthority(label, *GetSidSubAuthorityCount(label) - 1) < SECURITY_MANDATORY_MEDIUM_RID)
                {
                    return false;
                }
            }
            return true;
        }

        // A registry that was there already is used only when it is as makeMapping makes it (review mid-1): this user's
        // SID its owner, an access list whose one entry lets that SID in, an integrity label of medium or higher
        // (or none). Else another user of this session (runas) or a process of this user at low integrity may have made it
        // and may write it. False with why, for the log. (A program of this user at medium integrity or higher is not
        // kept out: it can change the viewer's settings files as it is.)
        bool registryTrusted(HANDLE mapping, const std::wstring& sid, const char*& why)
        {
            PSID owner = nullptr;
            PACL dacl = nullptr;
            PACL sacl = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (GetSecurityInfo(mapping, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                                &owner, nullptr, &dacl, &sacl, &descriptor) != ERROR_SUCCESS)
            {
                why = "the security of the one there could not be read";
                return false;
            }
            why = !sidIs(owner, sid)                    ? "the one there has another owner"
                  : !daclIsOnly(dacl, sid)              ? "the one there lets others in"
                  : !labelMediumOrHigher(sacl)          ? "the one there has a low integrity label"
                                                        : nullptr;
            LocalFree(descriptor);
            return why == nullptr;
        }

        // The view is backed by a file (a name comes back, or one too long for the longest path Windows has: re-review
        // low-5), not by the paging file. A section of the paging file has no name, and the call fails then; that failure's
        // code is not relied on (unverified): any other failure is taken as the paging file's. If a Windows gave a name for
        // the paging file's sections, every viewer (the maker too) would stay out of the registry: as without this feature.
        bool backedByFile(const void* view)
        {
            constexpr DWORD LONGEST_PATH = 32768;
            std::vector<wchar_t> name(LONGEST_PATH);    // once, at the start
            if (GetMappedFileNameW(GetCurrentProcess(), const_cast<void*>(view), name.data(), LONGEST_PATH) != 0)
            {
                return true;
            }
            const DWORD error = GetLastError();
            return error == ERROR_INSUFFICIENT_BUFFER || error == ERROR_MORE_DATA || error == ERROR_BUFFER_OVERFLOW;
        }

        // The mapped view is the registry's whole page, committed and read-write, of the paging file (review mid-1: one made
        // with SEC_RESERVE would fault at the first read, one backed by a file on a drive that goes away too). Checked
        // before anything in it is read. Against this user's programs at medium integrity or higher (the only ones that get
        // here: registryTrusted) it is a second guard, not a complete one: they can change the viewer's files as it is.
        bool viewUsable()
        {
            MEMORY_BASIC_INFORMATION region = {};
            return sView && VirtualQuery(sView, &region, sizeof(region)) == sizeof(region) && region.RegionSize >= REGISTRY_BYTES &&
                   region.State == MEM_COMMIT && region.Protect == PAGE_READWRITE && region.Type == MEM_MAPPED && !backedByFile(sView);
        }

        // Makes the registry, or opens the one there (then error is ERROR_ALREADY_EXISTS): owned by this user, whose SID
        // alone is in the access list, of medium integrity with no read or write up (a process of low integrity can neither
        // read nor write it; one of high integrity, a viewer run as administrator, makes it medium too, so the two share
        // it). The descriptor counts only when it is made: one there already is checked (registryTrusted). Null on failure,
        // error saying why.
        HANDLE makeMapping(const std::wstring& sid, DWORD& error)
        {
            const std::wstring descriptor_text = L"O:" + sid + L"D:P(A;;GA;;;" + sid + L")S:(ML;;NWNR;;;ME)";
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor_text.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            {
                error = GetLastError();
                return nullptr;
            }
            // The SID is not in the name (a hash of it is): the name is the same for this user's every process in this
            // logon session (Local\), and another user's viewers have names of their own.
            const std::string name = llformat("Local\\CharisViewer.Instances.v%u.%016llx", LAYOUT_VERSION,
                                              (unsigned long long)HBXXH64::digest(ll_convert_wide_to_string(sid)));
            SECURITY_ATTRIBUTES attributes = { sizeof(attributes), descriptor, FALSE };
            SetLastError(ERROR_SUCCESS);    // only ERROR_ALREADY_EXISTS after a success is read
            HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, (DWORD)REGISTRY_BYTES,
                                                ll_convert_string_to_wide(name).c_str());
            error = GetLastError();
            LocalFree(descriptor);
            return mapping;
        }

        void notJoining(const std::string& why)
        {
            LL_INFOS("MultiInstance") << "not joining the registry of Charis viewers: " << why << LL_ENDL;
        }

        // Opens the registry of this user's viewers, making it when there is none (makeMapping). One that is there
        // already must be as makeMapping makes it (registryTrusted). The view must be committed paging-file memory before
        // its first read (viewUsable), and the layout version 1's. False (logged): not in the registry this session.
        bool openRegistry()
        {
            const std::wstring sid = plus::userSidText();
            if (sid.empty())
            {
                notJoining("the user's SID could not be read");
                return false;
            }
            DWORD error = 0;
            sMapping = makeMapping(sid, error);
            if (!sMapping)
            {
                notJoining(llformat("it could not be made or opened (Windows error %lu)", error));
                return false;
            }
            const char* why = nullptr;
            if (error == ERROR_ALREADY_EXISTS && !registryTrusted(sMapping, sid, why))
            {
                notJoining(why);
                closeRegistry();
                return false;
            }
            sView = static_cast<U8*>(MapViewOfFile(sMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, REGISTRY_BYTES));
            if (!viewUsable())
            {
                notJoining("it could not be mapped whole as committed read-write memory of the paging file");
                closeRegistry();
                return false;
            }
            if (!headerReady())
            {
                notJoining(llformat("its layout is not version %u's", LAYOUT_VERSION));
                closeRegistry();
                return false;
            }
            return true;
        }

        // This viewer can speak on voice now (WebRTC runs in this process: its threads would go below normal too; review
        // low-2): the speak button's own test, LLAgent::isActionAllowed("speak"): connected (gAgent's flag, set when a channel
        // is joined, cleared when voice is turned off) and the channel allows it (nearby voice: the parcel's allowAgentVoice;
        // a call: the channel is active). The flag alone stays set in a parcel or region without voice (review 3 low-2), and
        // isVoiceWorking() stays true for the session once voice was on (re-review low-1). The flag is asked first (a bool):
        // until voice is connected the voice client and the parcel manager are not asked at all. Main thread (tick).
        bool voiceOn()
        {
            return gAgent.isVoiceConnected() && LLAgent::isActionAllowed(LLSD("speak"));
        }

        // B on or off: the frame rate cap and the priority. Below normal only from normal (a priority the user set is
        // left alone) and not while it can speak on voice (voiceOn), and back to normal only from the below normal this set. A failed return
        // to normal keeps the mark, so the next call (the next tick) tries again (review low-4).
        void applyYield(bool yield)
        {
            sYielding = yield;
            sFpsCap = yield ? fpsCapSetting() : 0;
            const bool lower = yield && lowPrioritySetting() && !voiceOn();
            HANDLE self = GetCurrentProcess();
            if (lower && !sPriorityLowered)
            {
                sPriorityLowered = GetPriorityClass(self) == NORMAL_PRIORITY_CLASS && SetPriorityClass(self, BELOW_NORMAL_PRIORITY_CLASS);
            }
            else if (!lower && sPriorityLowered)
            {
                if (GetPriorityClass(self) != BELOW_NORMAL_PRIORITY_CLASS || SetPriorityClass(self, NORMAL_PRIORITY_CLASS))
                {
                    sPriorityLowered = false;
                }
            }
        }

        // A larger share (or none) at once; a smaller one once it has held SHARE_LOWER_AFTER_MS (the largest of the
        // smaller ones asked for meanwhile).
        void updateShare(U64 now_ms, F32 wanted)
        {
            if (wanted >= sShareFraction)
            {
                sShareFraction = wanted;
                sShareLowerSinceMs = 0;
                return;
            }
            if (sShareLowerSinceMs == 0)
            {
                sShareLowerSinceMs = now_ms;
                sShareLowerTo = wanted;
            }
            else
            {
                sShareLowerTo = llmax(sShareLowerTo, wanted);
            }
            if (now_ms - sShareLowerSinceMs >= SHARE_LOWER_AFTER_MS)
            {
                sShareFraction = sShareLowerTo;
                sShareLowerSinceMs = 0;
            }
        }

        F32 weightOf(View view)
        {
            return view == View::Front ? WEIGHT_FRONT : view == View::Seen ? WEIGHT_SEEN : WEIGHT_HIDDEN;
        }

        const char* viewName(View view)
        {
            return view == View::Front ? "in front" : view == View::Seen ? "seen" : "minimized or hidden";
        }

        // Another slot, read whole, that is filled, written under HEARTBEAT_STALE_MS ago, on this GPU, with values in range
        // (its process is asked about after this: checkPeer).
        bool onThisGpu(const Slot& copy, U64 now_ms)
        {
            const bool front_time_ok = !copy.focused || (copy.focus_since_ms != 0 && copy.focus_since_ms <= now_ms + CLOCK_SKEW_MS);
            return copy.pid != 0 && copy.pid != sPid && copy.start_100ns != 0 && freshBeat(copy.beat_ms, now_ms) && copy.luid == sLuid &&
                   copy.focused <= 1 && copy.seen <= 1 && front_time_ok;
        }

        // The others of ours on this GPU, alive and checked, into sOthers.
        void readOthers(U64 now_ms)
        {
            sOtherCount = 0;
            if (sLuid == 0)
            {
                return;     // this GPU is not known: no other viewer is on it
            }
            for (U32 index = 0; index < SLOT_COUNT; ++index)
            {
                Slot copy;
                if ((S32)index == sSlot || !readSlot(slotAt(index), copy) || !onThisGpu(copy, now_ms) ||
                    checkPeer(index, copy.pid, copy.start_100ns, now_ms) != Peer::Ours)
                {
                    continue;
                }
                PeerSeen& other = sOthers[sOtherCount++];
                other.pid = copy.pid;
                other.view = copy.focused ? View::Front : copy.seen ? View::Seen : View::Hidden;
                other.focus_since_ms = copy.focused ? copy.focus_since_ms : 0;
                other.textures_mb = checkedMB(copy.textures_mb);
                other.usage_mb = checkedMB(copy.usage_mb);
                other.budget_mb = checkedMB(copy.budget_mb);
                other.same_cache = copy.cache_hash != 0 && copy.cache_hash == sCacheHash;
            }
        }

        // The state line's parts (0 MB: not known).
        void appendFigures(std::ostream& line, const PeerSeen& viewer)
        {
            line << "pid " << viewer.pid << " " << viewName(viewer.view) << ", VRAM " << viewer.usage_mb << " of " << viewer.budget_mb
                 << " MB driver budget, textures " << viewer.textures_mb << " MB";
        }

        void appendYield(std::ostream& line, const Shown& shown)
        {
            if (!shown.yielding)
            {
                line << "no";
                return;
            }
            line << "yes (frame rate cap ";
            if (shown.fps_cap > 0)
            {
                line << shown.fps_cap;
            }
            else
            {
                line << "none: PlusMultiBackgroundFps 0";
            }
            line << ", priority "
                 << (shown.lowered              ? "below normal"
                     : !lowPrioritySetting()    ? "kept: PlusMultiBackgroundPriority off"
                     : voiceOn()                ? "kept: on voice"
                                                : "kept: it was not normal")
                 << ")";
        }

        // The share as of now: the part of the weights this tick set, with the last frame's reading (vramShare applies the
        // same at the next frame). No share: not in the registry or the setting off.
        VRAMShare shareNow()
        {
            return sSlot >= 0 && shareSetting() ? shareFor(sReading, sShareFraction) : VRAMShare();
        }

        void appendShare(std::ostream& line, S32 share_pct, const VRAMShare& share)
        {
            if (!shareSetting())
            {
                line << "off (PlusMultiShareVRAM)";
            }
            else if (share_pct >= 100)
            {
                line << (sShareLowerSinceMs ? "none yet (a smaller one waits)" : "none");
            }
            else if (share.limit_mb <= 0.f)
            {
                line << "a part of " << share_pct << "% (no driver budget: not applied)";
            }
            else if (share.floored)
            {
                // The floor, not the formula (research/337 M2: on an 8 GB GPU the one behind is there)
                line << "a part of " << share_pct << "%: " << (S32)share.limit_mb << " MB, the floor max(1 GB, a quarter of the budget) ("
                     << (S32)share.part_mb << " MB of the budget less the margin, less its other VRAM " << (S32)share.other_mb
                     << " MB, is under it)";
            }
            else
            {
                line << "a part of " << share_pct << "%: " << (S32)share.limit_mb << " MB for textures ("
                     << (S32)share.part_mb << " MB of the budget less the margin, less its other VRAM " << (S32)share.other_mb << " MB)";
            }
        }

        // One line when what it shows changed, at most every LOG_MIN_MS (a change within that is shown when it is over).
        void logState(U64 now_ms, View own_view)
        {
            Shown now;
            now.others = sOtherCount;
            // Alone, who is in front is not shown: a start without others logs nothing after joining
            now.front_pid = own_view == View::Front && sOtherCount > 0 ? sPid : 0;
            for (U32 i = 0; i < sOtherCount && now.front_pid == 0; ++i)
            {
                if (sOthers[i].view == View::Front)
                {
                    now.front_pid = sOthers[i].pid;
                }
            }
            now.yielding = sYielding;
            now.lowered = sPriorityLowered;
            now.fps_cap = sFpsCap;
            now.share_pct = (S32)ll_round(sShareFraction * 100.f);
            const VRAMShare share = shareNow();
            now.share_floored = share.limit_mb > 0.f && share.floored;
            if (now == sShown || now_ms - sLastLogMs < LOG_MIN_MS)
            {
                return;
            }
            sShown = now;
            sLastLogMs = now_ms;

            std::ostringstream line;
            line << now.others << " other Charis viewer(s) on this GPU";
            for (U32 i = 0; i < sOtherCount; ++i)
            {
                line << (i == 0 ? ": " : "; ");
                appendFigures(line, sOthers[i]);
                line << ", " << (sOthers[i].same_cache ? "the same cache" : "another cache");
            }
            PeerSeen self;
            self.pid = sPid;
            self.view = own_view;
            self.usage_mb = toMB(sReading.usage_mb);
            self.budget_mb = toMB(sReading.budget_mb);
            self.textures_mb = toMB(sReading.textures_mb);
            line << ". This one: ";
            appendFigures(line, self);
            line << "; yielding: ";
            appendYield(line, now);
            line << "; VRAM share: ";
            appendShare(line, now.share_pct, share);
            LL_INFOS("MultiInstance") << line.str() << LL_ENDL;
        }

        // Out of the registry for the rest of the session: no yield, no share; the slot given back when it is ours.
        void leave(const char* why, bool give_back)
        {
            applyYield(false);
            sShareFraction = 1.f;
            sShareLowerSinceMs = 0;
            sOtherCount = 0;
            if (give_back && sSlot >= 0)
            {
                giveBackSlot();
            }
            sSlot = -1;
            closeRegistry();
            LL_INFOS("MultiInstance") << why << LL_ENDL;
        }

        // Once a second: this one's slot, the others', the yield and the share, the log.
        void tick(U64 now_ms)
        {
            if ((U32)ReadAcquire(asLong(header()->magic)) != MAGIC)
            {
                leave("the registry of Charis viewers no longer has version 1's layout: out of it for this session", false);
                return;
            }
            if ((U32)ReadAcquire(asLong(slotAt((U32)sSlot)->pid)) != sPid)
            {
                leave("another process wrote over this viewer's slot in the registry of Charis viewers: out of it for this session", false);
                return;
            }
            const View own_view = ownView(now_ms);
            writeOwnSlot(now_ms, own_view);
            readOthers(now_ms);

            bool other_in_front = false;
            F32 total_weight = weightOf(own_view);
            for (U32 i = 0; i < sOtherCount; ++i)
            {
                const PeerSeen& other = sOthers[i];
                const U64 since = llmin(other.focus_since_ms, now_ms);
                other_in_front = other_in_front || (other.view == View::Front && now_ms - since >= YIELD_AFTER_MS);
                if (i < MAX_COUNTED)
                {
                    total_weight += weightOf(other.view);
                }
            }
            applyYield(other_in_front && own_view != View::Front);
            updateShare(now_ms, shareSetting() && sOtherCount > 0 ? weightOf(own_view) / total_weight : 1.f);
            logState(now_ms, own_view);
        }
    }

    void init()
    {
        if (sInitDone)
        {
            return;
        }
        sInitDone = true;
        if (!LLRHI::envDefaultOn("LLRHI_MULTI_INSTANCE"))
        {
            notJoining("LLRHI_MULTI_INSTANCE=0 (a safe start sets it)");
            return;
        }
        FILETIME created, ended, kernel, user;
        if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        {
            notJoining("this process's start time could not be read");
            return;
        }
        sPid = GetCurrentProcessId();
        sStart100ns = fileTimeValue(created);
        const std::wstring exe = plus::ownExePath();
        sExeName = fileNamePart(exe.c_str());
        if (sExeName.empty())
        {
            notJoining("this program's file name could not be read");
            return;
        }
        LLRHI::DeviceInfo device;
        sLuid = LLRHI::deviceInfo(device) ? device.luid : 0;
        std::string cache = gDirUtilp->getCacheDir();
        LLStringUtil::toLower(cache);
        sCacheHash = cache.empty() ? 0 : HBXXH64::digest(cache);
        if (!openRegistry())
        {
            return;
        }
        const U64 now_ms = GetTickCount64();
        if (!takeSlot(now_ms))
        {
            closeRegistry();
            notJoining(llformat("all %u slots are taken", SLOT_COUNT));
            return;
        }
        sLastTickMs = now_ms;
        sLastLogMs = now_ms;
        const S32 fps_cap = fpsCapSetting();
        LL_INFOS("MultiInstance") << "joined the registry of Charis viewers (slot " << sSlot + 1 << " of " << SLOT_COUNT << ", pid " << sPid
                                  << (sLuid ? "" : "; the GPU is not known: no other viewer counts as on this GPU")
                                  << "). While another one on this GPU is in front: frame rate cap "
                                  << (fps_cap > 0 ? std::to_string(fps_cap) : std::string("none")) << " (PlusMultiBackgroundFps), priority "
                                  << (lowPrioritySetting() ? "below normal" : "kept") << " (PlusMultiBackgroundPriority); VRAM share "
                                  << (shareSetting() ? "on" : "off") << " (PlusMultiShareVRAM)" << LL_ENDL;
    }

    void update()
    {
        if (sSlot < 0)
        {
            // Out of the registry with the priority still below normal (leave's return to normal failed): tried again once
            // a second (re-review low-6). Otherwise a bool's read.
            if (sPriorityLowered)
            {
                const U64 now_ms = GetTickCount64();
                if (now_ms - sLastTickMs >= TICK_MS)
                {
                    sLastTickMs = now_ms;
                    applyYield(false);
                }
            }
            return;
        }
        // In front again, or quitting (the quit's frames at full rate and priority; the slot goes in shutdown()): at once,
        // not at the next tick. LLApp::isRunning() takes a lock (LLScalarCond): asked only while yielding and once a second.
        if (sYielding && (appInFront() || !LLApp::isRunning()))
        {
            applyYield(false);
        }
        const U64 now_ms = GetTickCount64();
        if (now_ms - sLastTickMs < TICK_MS)
        {
            return;
        }
        sLastTickMs = now_ms;
        if (LLApp::isRunning())     // no tick while quitting: the heartbeat stops, the others stop counting this one
        {
            tick(now_ms);
        }
    }

    void shutdown()
    {
        if (sSlot >= 0)
        {
            leave("left the registry of Charis viewers (the viewer is closing)", true);
        }
    }

    S32 fpsCap()
    {
        return sFpsCap;
    }

    VRAMShare vramShare(const VRAMReading& reading)
    {
        sReading = reading;
        return sSlot >= 0 && shareSetting() ? shareFor(reading, sShareFraction) : VRAMShare();
    }

#else // !LL_WINDOWS: no registry (the viewer is built for Windows)

    void init() {}
    void update() {}
    void shutdown() {}
    S32 fpsCap() { return 0; }
    VRAMShare vramShare(const VRAMReading&) { return VRAMShare(); }

#endif
}
