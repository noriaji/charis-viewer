/**
 * @file regionpark.cpp
 * @brief Teleport and revisit speed: the textures of the regions just left
 *        stay while VRAM allows, and after an arrival the cached objects in
 *        reach are made before they come into view.
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

// The idea (keep what was just left, and make what the cache has at once) is the
// one Aurora's parked world and object cache have (research/212); this is written
// from that description and upstream's code, not from Aurora's code.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "regionpark.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llmemory.h"
#include "llprogressview.h"
#include "llregionhandle.h"
#include "llrhi.h"
#include "llstartup.h"
#include "lltexturefetch.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llworld.h"

#include <array>
#include <string>
#include <vector>

namespace
{
    // Visits remembered (a visit: from a teleport or the login to the next
    // teleport). PlusRegionParkMax is clamped below this.
    constexpr size_t VISITS = 9;
    constexpr U32 PARK_MAX_LIMIT = (U32)VISITS - 1;
    // The time a texture was last seen moves only when the time-sliced scan
    // visits it (about a second): seen this close to an arrival counts as seen
    // after it (as in plus/tprelease).
    constexpr F32 GRACE_SECONDS = 0.5f;
    constexpr F32 HOLD_MIN = 30.f;          // upstream's lazy flush
    constexpr F32 HOLD_MAX = 600.f;
    constexpr F32 EAGER_WINDOW_SECONDS = 30.f;
    constexpr F32 EAGER_MIN_SECONDS = 5.f;  // the cache probes come in over the first seconds
    constexpr U32 EAGER_SCREEN_FACTOR = 2;  // under the teleport screen (a longer frame is hidden; more once measured)
    constexpr U32 EAGER_PER_FRAME_MAX = 2048;   // the hook looks at 2048 cache entries a frame at most
    constexpr U32 EAGER_EMPTY_SWEEPS = 2;   // sweeps in a row that make nothing close the window (eagerDone())
    constexpr F32 PARK_VRAM_SHARE = 0.8f;   // parked only below this share of the VRAM budget (research/97: room before it runs over)
    constexpr std::array<F32, 4> REPORT_AT = { 1.f, 3.f, 10.f, 30.f };
    constexpr F32 MB = 1024.f * 1024.f;

    LLCachedControl<bool>& park_on()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusRegionPark", false);
        return on;
    }

    LLCachedControl<bool>& eager_on()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusRegionParkEager", false);
        return on;
    }

    U32 park_max()
    {
        static LLCachedControl<U32> max_visits(gSavedSettings, "PlusRegionParkMax", 2);
        return llclamp((U32)max_visits, 1U, PARK_MAX_LIMIT);
    }

    F32 hold_seconds()
    {
        static LLCachedControl<F32> hold(gSavedSettings, "PlusRegionParkTextureHold", 90.f);
        return llclamp((F32)hold, HOLD_MIN, HOLD_MAX);
    }

    U32 eager_per_frame()
    {
        static LLCachedControl<U32> per_frame(gSavedSettings, "PlusRegionParkEagerPerFrame", 64);
        return llclamp((U32)per_frame, 1U, EAGER_PER_FRAME_MAX);
    }

    F32 now()
    {
        return llmax((F32)gFrameTimeSeconds, 0.001f);
    }

    // No texture pressure (research/79 stages), nothing biased down, VRAM in use below
    // PARK_VRAM_SHARE of the budget (read once a frame; without the backend's figures:
    // no room), and system memory not short.
    bool vram_has_room()
    {
        if (LLViewerTexture::sRhiTexPressure != 0 || LLViewerTexture::sDesiredDiscardBias > 1.f)
        {
            return false;
        }
        static U32 sFrame = U32_MAX;
        static bool sRoom = false;
        if (sFrame != gFrameCount)
        {
            sFrame = gFrameCount;
            U64 texture_bytes = 0, usage = 0, budget = 0;
            sRoom = LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(texture_bytes, usage, budget) && budget > 0
                    && (F64)usage < (F64)budget * PARK_VRAM_SHARE
                    && LLMemory::getSystemMemoryBudgetFactor() <= 1.f;
        }
        return sRoom;
    }

    bool screen_up()
    {
        return gViewerWindow && gViewerWindow->getProgressView() && gViewerWindow->getProgressView()->getVisible();
    }

    // regionp: the region when it is no longer in the world's list (being removed).
    std::string region_label(U64 handle, const LLViewerRegion* regionp = nullptr)
    {
        U32 x = 0;
        U32 y = 0;
        from_region_handle(handle, &x, &y);
        std::string label = "(" + std::to_string(x / 256) + "," + std::to_string(y / 256) + ")";
        if (!regionp && LLWorld::instanceExists())
        {
            regionp = LLWorld::getInstance()->getRegionFromHandle(handle);
        }
        if (regionp && !regionp->getName().empty())
        {
            label += " " + regionp->getName();
        }
        return label;
    }

    // ---------------------------------------------------------------- visits

    // Start of each visit, oldest first; the last one is the current visit.
    std::array<F32, VISITS> sVisitStart{};
    size_t sVisits = 0;

    void add_visit(F32 at)
    {
        if (sVisits == VISITS)
        {
            for (size_t i = 1; i < VISITS; ++i)
            {
                sVisitStart[i - 1] = sVisitStart[i];
            }
            --sVisits;
        }
        sVisitStart[sVisits++] = at;
    }

    // Seen during one of the last PlusRegionParkMax visits before this one.
    bool seen_on_recent_visit(const LLViewerFetchedTexture* imagep)
    {
        if (sVisits < 2)
        {
            return false;   // still on the first visit: nothing has been left
        }
        const F32 seen = imagep->mRhiSeenTime;
        const F32 current = sVisitStart[sVisits - 1];
        if (seen <= 0.f || seen >= current - GRACE_SECONDS)
        {
            return false;   // never seen, or seen on this visit (tprelease leaves it alone)
        }
        const size_t back = llmin((size_t)park_max(), sVisits - 1);
        return seen >= sVisitStart[sVisits - 1 - back] - GRACE_SECONDS;
    }

    // ---------------------------------------------------------------- regions left

    struct Parked
    {
        U64 handle = 0;
        F32 left = 0.f;
        U32 alive = 0;      // objects alive when left
        U32 cached = 0;     // object cache entries then
    };
    std::vector<Parked> sParked;   // the regions left by a teleport, still open; at most VISITS

    Parked* find_parked(U64 handle)
    {
        for (Parked& p : sParked)
        {
            if (p.handle == handle)
            {
                return &p;
            }
        }
        return nullptr;
    }

    void forget_parked(U64 handle)
    {
        for (auto it = sParked.begin(); it != sParked.end(); ++it)
        {
            if (it->handle == handle)
            {
                sParked.erase(it);
                return;
            }
        }
    }

    // ---------------------------------------------------------------- the arrival

    struct Counters
    {
        U64 skipped = 0;    // texture visits tprelease would have sent down
        U64 held = 0;       // texture visits kept past the lazy flush they would have had
    };

    struct Arrival
    {
        U64 handle = 0;
        F32 at = -1.f;
        U32 generation = 0;
        bool kept = false;          // back to a region still open, its objects alive
        F32 screen_gone = -1.f;     // since the arrival; below 0: not yet
        Counters at_arrival;
        // early creation
        bool eager_open = false;
        U64 eager_pushed = 0;
        U32 eager_sweeps = 0;
        U32 eager_empty_sweeps = 0;     // in a row
        U32 eager_sweep_active = 0;     // the region's live cached objects when the last sweep ended
    };
    Arrival sArrival;
    Counters sCounters;
    boost::signals2::connection sRemovedConnection;

    std::string memory_line()
    {
        std::string line;
        U64 texture_bytes = 0, usage = 0, budget = 0;
        if (LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(texture_bytes, usage, budget))
        {
            line = llformat("VRAM %.0f of %.0f MB, texture images %.0f MB, ",
                            (F32)usage / MB, (F32)budget / MB, (F32)texture_bytes / MB);
        }
        LLMemory::updateMemoryInfo();
        line += llformat("%d textures in the list, viewer memory %.0f MB, texture pressure %d, discard bias %.2f",
                         gTextureList.getNumImages(), (F32)LLMemory::getAllocatedMemKB().value() / 1024.f,
                         LLViewerTexture::sRhiTexPressure, LLViewerTexture::sDesiredDiscardBias);
        return line;
    }

    std::string region_objects(U64 handle)
    {
        LLViewerRegion* regionp = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegionFromHandle(handle) : nullptr;
        if (!regionp)
        {
            return "region gone";
        }
        return llformat("%u objects alive of %u cached", regionp->getNumOfActiveCachedObjects(),
                        (U32)regionp->getVOCacheMap().size());
    }

    void report(F32 at)
    {
        const S32 fetches = LLAppViewer::getTextureFetch() ? LLAppViewer::getTextureFetch()->getNumRequests() : -1;
        std::string line = llformat("+%.0f s after the arrival in %s (%s, plus/regionpark): ", at,
                                    region_label(sArrival.handle).c_str(),
                                    sArrival.kept ? "back to the region kept" : "new or closed since");
        line += region_objects(sArrival.handle);
        line += llformat(", %d texture fetches waiting", fetches);
        line += sArrival.screen_gone >= 0.f ? llformat(", teleport screen gone at +%.2f s", sArrival.screen_gone)
                                            : std::string(", teleport screen still up");
        line += llformat("; parked textures: %llu scale-downs left out, %llu kept past the flush (texture visits since the arrival)",
                         (unsigned long long)(sCounters.skipped - sArrival.at_arrival.skipped),
                         (unsigned long long)(sCounters.held - sArrival.at_arrival.held));
        if (eager_on())
        {
            line += llformat("; made early: %llu queued in %u sweeps%s", (unsigned long long)sArrival.eager_pushed,
                             sArrival.eager_sweeps, sArrival.eager_open ? "" : " (done)");
        }
        line += "; " + memory_line();
        LL_INFOS("RegionPark") << line << LL_ENDL;
    }

    void on_region_removed(LLViewerRegion* regionp)
    {
        if (!regionp)
        {
            return;
        }
        const U64 handle = regionp->getHandle();
        Parked* parked = find_parked(handle);
        if (!parked)
        {
            return;
        }
        // LLWorld::resetClass() removes every region at logout: not the simulator's doing.
        const bool leaving = LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent();
        if ((park_on() || eager_on()) && !leaving)
        {
            LL_INFOS("RegionPark") << "parked " << region_label(handle, regionp)
                                   << llformat(" closed by the simulator %.0f s after it was left (%u objects alive go, %u cached)",
                                               now() - parked->left, regionp->getNumOfActiveCachedObjects(),
                                               (U32)regionp->getVOCacheMap().size())
                                   << LL_ENDL;
        }
        forget_parked(handle);
    }

    U64 sLastRegion = 0;

    // Main thread, from LLAgent::setRegion(): a teleport (state not NONE), the
    // login (startup not done) or a walking crossing (neither: not a visit).
    void on_region_changed()
    {
        LLViewerRegion* regionp = gAgent.getRegion();
        const U64 before = sLastRegion;
        const U64 handle = regionp ? regionp->getHandle() : 0;
        sLastRegion = handle;
        if (before == handle)
        {
            // The same region again: the login sets it twice (at the start and on the arrival), and a
            // teleport within the region sets it too (process_agent_movement_complete()). Neither is a
            // new arrival, so the eager window does not open for a teleport within the region
            // (research/242 §3.2).
            return;
        }

        const bool teleport = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
        const bool login = LLStartUp::getStartupState() < STATE_STARTED;
        if (!handle || (!teleport && !login))
        {
            return;
        }
        if (!sRemovedConnection.connected() && LLWorld::instanceExists())
        {
            sRemovedConnection = LLWorld::getInstance()->setRegionRemovedCallback(&on_region_removed);
        }

        const F32 at = now();
        add_visit(at);
        const bool log = park_on() || eager_on();

        // The region left: upstream keeps it, with its objects, until the simulator closes it.
        LLViewerRegion* leftp = (before && before != handle && LLWorld::instanceExists())
                                    ? LLWorld::getInstance()->getRegionFromHandle(before) : nullptr;
        if (leftp)
        {
            forget_parked(before);
            if (sParked.size() >= VISITS)
            {
                sParked.erase(sParked.begin());
            }
            Parked parked;
            parked.handle = before;
            parked.left = at;
            parked.alive = leftp->getNumOfActiveCachedObjects();
            parked.cached = (U32)leftp->getVOCacheMap().size();
            sParked.push_back(parked);
            if (log)
            {
                LL_INFOS("RegionPark") << "left " << region_label(before)
                                       << llformat(": %u objects alive of %u cached stay until the simulator closes it; ",
                                                   parked.alive, parked.cached)
                                       << (park_on() ? llformat("its textures are parked while VRAM has room (the last %u visits)", park_max())
                                                     : std::string("textures not parked (PlusRegionPark off)"))
                                       << LL_ENDL;
            }
        }

        // The arrival.
        const Parked* back = find_parked(handle);
        sArrival = Arrival();
        sArrival.handle = handle;
        sArrival.at = at;
        sArrival.kept = back != nullptr;
        sArrival.at_arrival = sCounters;
        sArrival.eager_open = eager_on();
        static U32 sGeneration = 0;
        sArrival.generation = ++sGeneration;
        if (back)
        {
            if (log)
            {
                LL_INFOS("RegionPark") << "back to " << region_label(handle)
                                       << llformat(" %.0f s after it was left, the region kept: ", at - back->left)
                                       << region_objects(handle)
                                       << llformat(" (%u when left); %u regions parked; ", back->alive, (U32)sParked.size() - 1)
                                       << memory_line() << LL_ENDL;
            }
            forget_parked(handle);
        }
        else if (log)
        {
            LL_INFOS("RegionPark") << "arrival in " << region_label(handle) << (login ? " (login)" : "")
                                   << ", new or closed since: " << region_objects(handle)
                                   << llformat("; %u regions parked; ", (U32)sParked.size()) << memory_line() << LL_ENDL;
        }
        if (!log)
        {
            return;
        }

        const U32 generation = sArrival.generation;
        doOnIdleRepeating([generation, next = (size_t)0]() mutable
        {
            if (generation != sArrival.generation)
            {
                return true;    // a later arrival reports for itself
            }
            const F32 since = now() - sArrival.at;
            if (sArrival.screen_gone < 0.f && !screen_up())
            {
                sArrival.screen_gone = since;
            }
            if (sArrival.eager_open && since >= EAGER_WINDOW_SECONDS)
            {
                sArrival.eager_open = false;
            }
            if (since < REPORT_AT[next])
            {
                return false;
            }
            report(REPORT_AT[next]);
            return ++next >= REPORT_AT.size();
        });
    }

    void init()
    {
        gAgent.addRegionChangedCallback(&on_region_changed);
    }

    plus::FeatureRegistration sRegistration({ "regionpark", &init });
}

namespace plus::regionpark
{
    bool skipScaleDown(const LLViewerFetchedTexture* imagep)
    {
        if (!park_on() || !vram_has_room() || !seen_on_recent_visit(imagep))
        {
            return false;
        }
        ++sCounters.skipped;
        return true;
    }

    bool holdInList(const LLViewerFetchedTexture* imagep, F32& timeout)
    {
        // The kind tprelease sends down: a region's own textures, not the UI, avatars or terrain.
        if (!park_on() || imagep->getType() != LLViewerTexture::LOD_TEXTURE
            || const_cast<LLViewerFetchedTexture*>(imagep)->getBoostLevel() != LLGLTexture::BOOST_NONE  // read only
            || !vram_has_room() || !seen_on_recent_visit(imagep))
        {
            return false;
        }
        const F32 hold = hold_seconds();
        if (hold > timeout)
        {
            // Counted when the hold is what keeps it on this visit (read only: the
            // accessor is not const).
            if (const_cast<LLViewerFetchedTexture*>(imagep)->getLastReferencedTimer()->getElapsedTimeF32() > timeout)
            {
                ++sCounters.held;
            }
            timeout = hold;
        }
        return true;
    }

    U32 eagerBudget(const LLViewerRegion* regionp, U32& arrival)
    {
        arrival = sArrival.generation;
        if (!sArrival.eager_open || !regionp || regionp->getHandle() != sArrival.handle)
        {
            return 0;
        }
        if (!eager_on() || now() - sArrival.at >= EAGER_WINDOW_SECONDS)
        {
            sArrival.eager_open = false;
            return 0;
        }
        if (LLViewerTexture::sRhiTexPressure > 0 || LLMemory::getSystemMemoryBudgetFactor() > 1.f)
        {
            return 0;   // VRAM or memory short: only what comes into view, as upstream
        }
        const U32 per_frame = eager_per_frame();
        return screen_up() ? per_frame * EAGER_SCREEN_FACTOR : per_frame;
    }

    void eagerDone(const LLViewerRegion* regionp, U32 pushed, bool sweep_done, U32 sweep_pushed)
    {
        if (!regionp || regionp->getHandle() != sArrival.handle)
        {
            return;
        }
        sArrival.eager_pushed += pushed;
        if (!sweep_done)
        {
            return;
        }
        ++sArrival.eager_sweeps;
        // Empty: nothing queued, or nothing made of what was queued (the live cached objects did not
        // grow since the last sweep: entries the cache cannot make an object of are queued again
        // every sweep, and would keep the window open until its end). What is queued is made over
        // the next frames, so the growth is seen a sweep later.
        const U32 active = regionp->getNumOfActiveCachedObjects();
        const bool made = active > sArrival.eager_sweep_active;
        sArrival.eager_sweep_active = active;
        sArrival.eager_empty_sweeps = sweep_pushed == 0 || !made ? sArrival.eager_empty_sweeps + 1 : 0;
        const F32 since = now() - sArrival.at;
        // The probes of what is farther come in later (the draw distance widens after the
        // arrival): a single empty sweep does not close it.
        if (sArrival.eager_empty_sweeps >= EAGER_EMPTY_SWEEPS && since >= EAGER_MIN_SECONDS && !screen_up()
            && sArrival.eager_open)
        {
            sArrival.eager_open = false;
            LL_INFOS("RegionPark") << llformat("made early in %s: %llu cached objects (with their children) queued out of view in %.1f s (%u sweeps); ",
                                               region_label(sArrival.handle).c_str(),
                                               (unsigned long long)sArrival.eager_pushed, since, sArrival.eager_sweeps)
                                   << region_objects(sArrival.handle) << LL_ENDL;
        }
    }
}
