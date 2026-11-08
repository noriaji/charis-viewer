/**
 * @file httpautotune.cpp
 * @brief Fetch connections tuned to the line (plus/httpautotune)
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
#include "plus/httpautotune/httpautotune.h"

#include "httprequest.h"
#include "llapp.h"
#include "llappcorehttp.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "llframetimer.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <atomic>

namespace plus::httpautotune
{
    namespace
    {
        constexpr char SETTING_SWITCH[] = "PlusFetchAutoTune";

        constexpr U32 MIN_LIMIT = 24;           // the floor (Aurora Viewer's value)
        constexpr U32 START_LIMIT = 64;         // the start: a line that cannot take it answers slower and steps down
        constexpr U32 MAX_LIMIT = 192;
        constexpr U32 MESH_MAX_LIMIT = 64;      // the mesh fetch is a bulk load beside the textures that show the world: it must not take the line (a login with the mesh at 192 slowed the textures' first byte from 17 to 500 ms)
        constexpr U32 STEP_UP_MIN = 4;          // up by an eighth, at least this many
        constexpr F64 LOOK_SECONDS = 3.0;       // how often the limits are looked at

        constexpr U32 RING = 64;                // the latest times to the first byte kept per class
        constexpr U32 MIN_SAMPLES = 8;          // fewer finished in a look: too little to judge by
        constexpr F32 SLOW_RATIO = 2.f;         // the median this many times the best: a quarter down
        constexpr F32 SLOW_MARGIN = 0.05f;      // ... and at least this many seconds over it (noise on a fast CDN)
        constexpr F32 NEAR_RATIO = 1.5f;        // up only while the median stays within this of the best
        constexpr F32 QUICK_RATIO = 1.25f;      // ... and within this: twice as many (the line is nowhere near full)
        constexpr F32 BEST_DRIFT = 1.01f;       // the best seen creeps up 1 % a look (a line that changed for good)
        constexpr S32 COOLDOWN_SLOW = 2;        // looks with no step up after a step down
        constexpr S32 COOLDOWN_THROTTLED = 3;

        // The fetchers' requests in flight follow the tuned limit: this many a connection, up to the most.
        constexpr S32 WATER_PER_CONNECTION = 3;
        constexpr S32 WATER_MAX = 400;

        constexpr U32 NO_POLICY = ~0U;

        struct Slot
        {
            Slot(int app_policy, const char* name, const char* user_key, U32 max_limit, bool quick_steps)
                : appPolicy(app_policy), name(name), userKey(user_key), maxLimit(max_limit), quickSteps(quick_steps)
            {
            }

            const int appPolicy;                // LLAppCoreHttp::EAppPolicy
            const char* const name;             // in log lines
            const char* const userKey;          // the user's own limit (0 or left at its default = ours)
            const U32 maxLimit;                 // the most connections this class takes
            const bool quickSteps;              // doubles while the line answers as fast as ever (the textures only)

            // Written by the main thread, read by the HTTP worker and the fetchers' threads.
            std::atomic<U32> policy{ NO_POLICY };
            std::atomic<bool> tuned{ false };
            std::atomic<U32> limit{ START_LIMIT };

            // Written by the HTTP worker (onCompletion()), taken by the main thread (look()).
            std::atomic<U32> written{ 0 };
            std::atomic<F32> ttfb[RING] = {};
            std::atomic<U32> throttled{ 0 };
            std::atomic<S32> peakActive{ 0 };

            // Main thread only.
            U32 read = 0;
            F32 best = 0.f;
            S32 cooldown = 0;
        };

        Slot sSlots[] = {
            { LLAppCoreHttp::AP_TEXTURE, "texture", "TextureFetchConcurrency", MAX_LIMIT, true },
            { LLAppCoreHttp::AP_MESH2, "mesh2", "Mesh2MaxConcurrentRequests", MESH_MAX_LIMIT, false },
        };

        Slot* slotFor(int app_policy)
        {
            for (Slot& slot : sSlots)
            {
                if (slot.appPolicy == app_policy)
                {
                    return &slot;
                }
            }
            return nullptr;
        }

        // The HTTP worker thread, once per finished attempt: atomics only.
        // The clock count of the latest 429 / 503 of the texture class (research/299 reads it: the fetch retries pause).
        std::atomic<U64> sLastThrottledClock{ 0 };

        void onCompletion(LLCore::HttpRequest::policy_t pclass, int http_status, double ttfb, int active)
        {
            // (the texture class only: a 503 of the mesh or any other class is not a reason for the textures' retries to wait)
            if ((http_status == 429 || http_status == 503) && sSlots[0].appPolicy == LLAppCoreHttp::AP_TEXTURE &&
                sSlots[0].policy.load(std::memory_order_relaxed) == pclass)
            {
                sLastThrottledClock.store(LLTimer::getCurrentClockCount(), std::memory_order_relaxed);
            }
            for (Slot& slot : sSlots)
            {
                if (slot.policy.load(std::memory_order_relaxed) != pclass)
                {
                    continue;
                }
                if (!slot.tuned.load(std::memory_order_relaxed))
                {
                    return;
                }
                if (http_status == 429 || http_status == 503)
                {
                    slot.throttled.fetch_add(1, std::memory_order_relaxed);
                }
                else if (ttfb >= 0.0 && http_status >= 200 && http_status < 300)
                {
                    const U32 i = slot.written.fetch_add(1, std::memory_order_relaxed);
                    slot.ttfb[i % RING].store(F32(ttfb), std::memory_order_relaxed);
                }
                S32 peak = slot.peakActive.load(std::memory_order_relaxed);
                while (active > peak &&
                       !slot.peakActive.compare_exchange_weak(peak, active, std::memory_order_relaxed))
                {
                }
                return;
            }
        }

        void restart(Slot& slot)
        {
            slot.limit.store(START_LIMIT);
            slot.read = slot.written.load();
            slot.throttled.store(0);
            slot.peakActive.store(0);
            slot.best = 0.f;
            slot.cooldown = 0;
        }

        // The user set a limit of their own (not 0, not left at the shipped default).
        bool userSet(const Slot& slot)
        {
            LLControlVariablePtr control = gSavedSettings.getControl(slot.userKey);
            return control && control->getValue().asInteger() != 0 && !control->isDefault();
        }

        // One look at a tuned class: true when its limit changed.
        bool look(Slot& slot)
        {
            const U32 written = slot.written.load(std::memory_order_relaxed);
            const U32 fresh = std::min(written - slot.read, RING);
            slot.read = written;
            const U32 throttled = slot.throttled.exchange(0, std::memory_order_relaxed);
            const S32 peak = slot.peakActive.exchange(0, std::memory_order_relaxed);

            const U32 limit = slot.limit.load(std::memory_order_relaxed);
            U32 next = limit;
            const char* why = nullptr;
            F32 median = 0.f;

            if (throttled)
            {
                // The server asks us to slow down: half.
                next = std::max(MIN_LIMIT, limit / 2);
                slot.cooldown = COOLDOWN_THROTTLED;
                why = "HTTP 429/503";
            }
            else if (fresh >= MIN_SAMPLES)
            {
                F32 times[RING];
                for (U32 k = 0; k < fresh; ++k)
                {
                    times[k] = slot.ttfb[(written - 1 - k) % RING].load(std::memory_order_relaxed);
                }
                std::nth_element(times, times + fresh / 2, times + fresh);
                median = times[fresh / 2];
                if (slot.best <= 0.f)
                {
                    slot.best = median;
                }

                // Only while every connection was in use: with fewer requests than connections the limit is
                // not what holds the fetch back, and a slower first byte is noise (or the server's own).
                // Requests in flight, not connections, also with pipelining: the libcurl shipped (7.54) opens
                // a new connection rather than queue on a busy one until the host's limit is reached, so L
                // requests out means L connections in use (research/275 L081, left as it is).
                if (peak >= S32(limit) && median > slot.best * SLOW_RATIO && median - slot.best > SLOW_MARGIN)
                {
                    next = std::max(MIN_LIMIT, limit - limit / 4);
                    slot.cooldown = COOLDOWN_SLOW;
                    why = "first byte twice as slow";
                }
                else if (slot.cooldown > 0)
                {
                    --slot.cooldown;
                }
                else if (peak >= S32(limit) && median <= slot.best * NEAR_RATIO)
                {
                    // Every connection busy and the server answers as fast as ever: more.
                    const U32 step = slot.quickSteps && median <= slot.best * QUICK_RATIO ? limit : std::max(STEP_UP_MIN, limit / 8);
                    next = std::min(slot.maxLimit, limit + step);
                    why = "all connections busy";
                }
                slot.best = std::min(median, slot.best * BEST_DRIFT);
            }
            else if (slot.cooldown > 0)
            {
                --slot.cooldown;
            }

            if (next == limit)
            {
                return false;
            }
            slot.limit.store(next, std::memory_order_relaxed);
            LL_INFOS("HttpAutoTune") << slot.name << " connections " << limit << " -> " << next << " (" << why
                                     << "; first byte median " << S32(median * 1000.f) << " ms, best "
                                     << S32(slot.best * 1000.f) << " ms, " << fresh << " finished, "
                                     << peak << " active at most)" << LL_ENDL;
            return true;
        }

        // The idle loop, every frame: a clock compare until the next look is due.
        void idle(void*)
        {
            static F64 next_look = 0.0;
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now < next_look)
            {
                return;
            }
            next_look = now + LOOK_SECONDS;
            if (LLApp::isExiting())
            {
                return;
            }

            bool changed = false;
            for (Slot& slot : sSlots)
            {
                if (slot.tuned.load(std::memory_order_relaxed))
                {
                    changed |= look(slot);
                }
            }
            if (changed)
            {
                // Sets the tuned limits through connectionLimit() (only the classes that changed are sent).
                LLAppViewer::instance()->getAppCoreHttp().refreshSettings(false);
            }
        }

        void onSwitch()
        {
            if (!LLApp::isExiting())
            {
                LLAppViewer::instance()->getAppCoreHttp().refreshSettings(false);
            }
        }

        void init()
        {
            // Setting: app_settings/settings_plus.xml (PlusFetchAutoTune). The hooks: llappcorehttp.cpp
            // (refreshSettings()), lltexturefetch.cpp (commonUpdate()), llmeshrepository.cpp (the request
            // water levels), llcorehttp (_httplibcurl.cpp: the observer, and a new connection limit set
            // without waiting for the class to go idle).
            LLCore::HttpRequest::setCompletionObserver(&onCompletion);
            gIdleCallbacks.addFunction(&idle);
            if (LLControlVariablePtr control = gSavedSettings.getControl(SETTING_SWITCH))
            {
                control->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { onSwitch(); });
            }
        }

        plus::FeatureRegistration sRegistration({ "httpautotune", &init });
    }

    U32 connectionLimit(int app_policy, U32 policy, U32 setting)
    {
        Slot* slot = slotFor(app_policy);
        if (!slot)
        {
            return setting;
        }
        slot->policy.store(policy);

        static LLCachedControl<bool> on(gSavedSettings, SETTING_SWITCH, true);
        const bool tuned = on && !userSet(*slot);
        if (tuned != slot->tuned.load())
        {
            restart(*slot);
            slot->tuned.store(tuned);
            LL_INFOS("HttpAutoTune") << slot->name << " connections "
                                     << (tuned ? "tuned to the line, from " : "fixed at ")
                                     << (tuned ? START_LIMIT : setting) << LL_ENDL;
        }
        return tuned ? slot->limit.load() : setting;
    }

    bool throttledWithin(F64 seconds)
    {
        const U64 at = sLastThrottledClock.load(std::memory_order_relaxed);
        const U64 now = LLTimer::getCurrentClockCount();
        return at != 0 && now >= at && (F64)(now - at) < seconds * (F64)get_timer_info().mClockFrequency;
    }

    void raiseWater(int app_policy, S32& high_water, S32& low_water)
    {
        const Slot* slot = slotFor(app_policy);
        if (!slot || !slot->tuned.load(std::memory_order_relaxed))
        {
            return;
        }
        const S32 want = std::min(S32(slot->limit.load(std::memory_order_relaxed)) * WATER_PER_CONNECTION, WATER_MAX);
        if (want > high_water)
        {
            high_water = want;
            low_water = want / 2;
        }
    }
}
