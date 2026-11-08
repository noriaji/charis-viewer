/**
 * @file hitchlog.cpp
 * @brief Hitch log: samples the main thread's call stack while a frame is stuck (research/274).
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

#include "plus/hitchlog/hitchlog.h"

#if LL_WINDOWS

#include "llapp.h"
#include "llrhiprofiler.h"
#include "llviewercontrol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plus::hitchlog
{
namespace
{
    constexpr U32 MAX_SAMPLES = 12;     // samples of one hitch
    constexpr U64 SAMPLE_GAP_MS = 100;  // between two samples of one hitch
    constexpr U64 POLL_MS = 10;
    constexpr U64 IDLE_POLL_MS = 250;   // while the setting is 0
    constexpr int INNER_DEPTH = 8;      // frames written for a sample from the inside
    constexpr int OUTER_DEPTH = 12;     // then this many more of those with symbols (the viewer's own: a driver without symbols can be deep)
    constexpr U32 MAX_REPORTS_PER_MINUTE = 6;

    constexpr int MAX_FRAMES = LLRHIProfiler::StackSampler::MAX_FRAMES;

    struct Sample
    {
        U64 at_ms = 0;                  // since the frame began
        int depth = 0;
        U64 pc[MAX_FRAMES] = {};        // function start addresses
    };

    LLRHIProfiler::StackSampler sStack;
    std::atomic<U64> sTickNs{ 0 };      // when the frame began
    std::atomic<U64> sFrame{ 0 };       // frames begun
    std::atomic<U32> sThresholdMs{ 0 };
    std::atomic<bool> sStop{ false };
    std::thread sThread;
    std::mutex sWakeLock;
    std::condition_variable sWake;      // ends the watchdog's sleep at exit
    bool sStarted = false;              // main thread only

    U64 now_ns()
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool same_stack(const Sample& a, const Sample& b)
    {
        if (a.depth != b.depth)
        {
            return false;
        }
        for (int i = 0; i < a.depth; ++i)
        {
            if (a.pc[i] != b.pc[i])
            {
                return false;
            }
        }
        return true;
    }

    // At most MAX_REPORTS_PER_MINUTE hitches a minute are written; the rest are counted.
    bool report_allowed(U64 now, U64& window_start, U32& written, U32& dropped)
    {
        if (now - window_start >= 60ull * 1000000000ull)
        {
            if (dropped)
            {
                LL_INFOS("HitchLog") << dropped << " more hitches in the last minute were not written" << LL_ENDL;
            }
            window_start = now;
            written = 0;
            dropped = 0;
        }
        if (written >= MAX_REPORTS_PER_MINUTE)
        {
            ++dropped;
            return false;
        }
        ++written;
        return true;
    }

    // Equal stacks are written once with their count.
    void report(const std::vector<Sample>& samples, U64 frame_ms, U32 threshold, bool running)
    {
        LL_INFOS("HitchLog") << (running ? "frame still running after " : "frame took ") << frame_ms << " ms (log from " << threshold << " ms), " << samples.size()
                             << " stack samples of the main thread" << LL_ENDL;
        std::vector<bool> done(samples.size(), false);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            if (done[i])
            {
                continue;
            }
            U32 same = 0;
            for (size_t j = i; j < samples.size(); ++j)
            {
                if (!done[j] && same_stack(samples[i], samples[j]))
                {
                    done[j] = true;
                    ++same;
                }
            }
            LL_INFOS("HitchLog") << "  " << same << " sample(s), the first at +" << samples[i].at_ms << " ms:" << LL_ENDL;
            int outer = 0;
            for (int d = 0; d < samples[i].depth; ++d)
            {
                const std::string name = LLRHIProfiler::symbolName(samples[i].pc[d]);
                if (d >= INNER_DEPTH)
                {
                    if (outer >= OUTER_DEPTH)
                    {
                        break;
                    }
                    if (name.find("<no symbols") != std::string::npos)
                    {
                        continue;
                    }
                    ++outer;
                }
                LL_INFOS("HitchLog") << "    " << name << LL_ENDL;
            }
        }
    }

    void watchdog()
    {
        std::vector<Sample> samples;
        samples.reserve(MAX_SAMPLES);
        U64 episode_frame = 0, episode_start_ns = 0, last_sample_ns = 0;
        U64 window_start = now_ns();
        U32 written = 0, dropped = 0;
        bool in_episode = false;
        U32 episode_threshold = 0;
        bool written_early = false;     // the samples were dealt with before the frame ended
        bool early_out = false;         // and were written (not over the limit)
        while (!sStop.load(std::memory_order_relaxed))
        {
            const U32 before = sThresholdMs.load(std::memory_order_relaxed);
            {
                std::unique_lock<std::mutex> lock(sWakeLock);
                sWake.wait_for(lock, std::chrono::milliseconds(before ? POLL_MS : IDLE_POLL_MS), []() { return sStop.load(); });
            }
            const U32 threshold = sThresholdMs.load(std::memory_order_relaxed);
            const U64 frame = sFrame.load(std::memory_order_acquire);
            const U64 tick = sTickNs.load(std::memory_order_acquire);
            if (in_episode && frame != episode_frame)
            {
                // The stuck frame has ended; `tick` is the next frame's start.
                const U64 frame_ms = (tick - episode_start_ns) / 1000000;
                if (written_early)
                {
                    if (early_out)
                    {
                        LL_INFOS("HitchLog") << "that frame ended after " << frame_ms << " ms" << LL_ENDL;
                    }
                }
                else if (!samples.empty() && report_allowed(now_ns(), window_start, written, dropped))
                {
                    report(samples, frame_ms, episode_threshold, false);
                }
                samples.clear();
                in_episode = false;
                written_early = false;
                early_out = false;
            }
            if (!threshold || sStop.load(std::memory_order_relaxed) || !LLApp::isRunning())
            {
                continue;   // not while the viewer shuts down: DLLs are being unloaded
            }
            const U64 now = now_ns();
            if (now - tick < (U64)threshold * 1000000)
            {
                continue;
            }
            if (!in_episode)
            {
                in_episode = true;
                episode_frame = frame;
                episode_start_ns = tick;
                episode_threshold = threshold;
                last_sample_ns = 0;
            }
            if (samples.size() < MAX_SAMPLES && now - last_sample_ns >= SAMPLE_GAP_MS * 1000000)
            {
                Sample s;
                s.depth = sStack.sample(s.pc);
                last_sample_ns = now;
                if (s.depth < 0)
                {
                    return;     // the main thread is gone
                }
                // The frame may have ended while the stack was taken: that stack is the next frame's.
                if (s.depth > 0 && sFrame.load(std::memory_order_acquire) == episode_frame)
                {
                    s.at_ms = (now - tick) / 1000000;
                    samples.push_back(s);
                }
            }
            // A frame that never ends (a real freeze) would leave nothing: with all the samples taken,
            // write them now, here, with the main thread running again.
            if (in_episode && !written_early && samples.size() >= MAX_SAMPLES)
            {
                written_early = true;
                if (report_allowed(now_ns(), window_start, written, dropped))
                {
                    early_out = true;
                    report(samples, (now_ns() - tick) / 1000000, episode_threshold, true);
                }
            }
        }
        if (dropped)
        {
            LL_INFOS("HitchLog") << dropped << " more hitches in the last minute were not written" << LL_ENDL;
        }
    }
} // namespace

    void frameTick()
    {
        static LLCachedControl<S32> setting(gSavedSettings, "PlusHitchLogMs", 0);
        const U32 threshold = (U32)llmax(0, (S32)setting);
        if (!sStarted)
        {
            if (!threshold)
            {
                return;     // off: no thread until it is turned on
            }
            sStarted = true;
            if (!sStack.init())
            {
                LL_WARNS("HitchLog") << "no handle for the main thread; the hitch log is off" << LL_ENDL;
                return;
            }
            sTickNs.store(now_ns());
            sThread = std::thread(watchdog);
            // Stop and join before the process exits: a thread killed by ExitProcess while
            // it holds the heap or the log lock deadlocks whatever runs after it.
            std::atexit([]()
            {
                {
                    std::lock_guard<std::mutex> lock(sWakeLock);
                    sStop = true;
                }
                sWake.notify_all();
                if (sThread.joinable())
                {
                    sThread.join();
                }
            });
            LL_INFOS("HitchLog") << "on: a frame longer than PlusHitchLogMs gets the main thread's stack written here" << LL_ENDL;
        }
        if (!sThread.joinable())
        {
            return;
        }
        sThresholdMs.store(threshold, std::memory_order_relaxed);
        sTickNs.store(now_ns(), std::memory_order_release);
        sFrame.fetch_add(1, std::memory_order_release);
    }
}

#else

namespace plus::hitchlog
{
    void frameTick() {}
}

#endif
