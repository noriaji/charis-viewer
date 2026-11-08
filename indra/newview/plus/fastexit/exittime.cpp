/**
 * @file exittime.cpp
 * @brief Exit time log (research/276 stage 1).
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

#include "plus/fastexit/exittime.h"

#include "llrhi.h"
#include "llviewercontrol.h"

#include <chrono>
#include <string>

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace plus::exittime
{
namespace
{
    using Clock = std::chrono::steady_clock;

    enum class State
    {
        UNDECIDED,      // no exit under way (or the last one was cancelled)
        OFF,            // this exit is not measured
        ON
    };

    // Main thread only (the quit, the frame loop, cleanup, wWinMain).
    State sState = State::UNDECIDED;
    Clock::time_point sStart;
    Clock::time_point sLast;

    F64 ms_between(const Clock::time_point& from, const Clock::time_point& to)
    {
        return std::chrono::duration<F64, std::milli>(to - from).count();
    }

    // The PC's local time to the millisecond, for lining the log up with
    // tools/exit-timer.ps1 (which sees the window go and the process end from
    // the outside). The log's own time stamps stop at the second.
    std::string local_time()
    {
#if LL_WINDOWS
        SYSTEMTIME t;
        GetLocalTime(&t);
        return llformat(" (local time %02u:%02u:%02u.%03u)", (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond,
                       (unsigned)t.wMilliseconds);
#else
        return std::string();
#endif
    }
}

void begin(const char* stage)
{
    // research/285 (compact-hysteresis): every quit, measured or not: the
    // backend's VRAM compactions stop while the viewer waits to close.
    LLRHI::sQuitting.store(true, std::memory_order_relaxed);
    if (sState == State::UNDECIDED)
    {
        // Read here, never in a frame: begin() runs once per exit, while
        // gSavedSettings is still whole (cleanup() empties it near its end).
        static LLCachedControl<bool> wanted(gSavedSettings, "PlusExitTimeLog", false);
        if (!wanted)
        {
            sState = State::OFF;
            return;
        }
        sState = State::ON;
        sStart = Clock::now();
        sLast = sStart;
        // llrhi cannot read the setting: LLRHIVKMirror::onGLShutdown writes its
        // own steps while this is set, timed from the same start.
        LLRHI::sExitTimeStartNs = (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(sStart.time_since_epoch()).count();
        LLRHI::sExitTimeLog = true;
        LL_INFOS("Plus") << "ExitTime [start] the clock starts at " << stage << local_time() << " (PlusExitTimeLog)" << LL_ENDL;
    }
    mark(stage);
}

void mark(const char* stage)
{
    if (sState != State::ON)
    {
        return;
    }
    const Clock::time_point now = Clock::now();
    LL_INFOS("Plus") << "ExitTime [" << stage << "] +" << llformat("%.1f", ms_between(sStart, now)) << " ms (step "
                     << llformat("%.1f", ms_between(sLast, now)) << " ms)" << LL_ENDL;
    sLast = now;
}

void cancel()
{
    LLRHI::sQuitting.store(false, std::memory_order_relaxed);   // research/285: the compactions may run again
    if (sState == State::ON)
    {
        mark("quit cancelled");
        LLRHI::sExitTimeLog = false;
    }
    sState = State::UNDECIDED;
}

} // namespace plus::exittime
