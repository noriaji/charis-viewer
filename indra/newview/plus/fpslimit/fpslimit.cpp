/**
 * @file fpslimit.cpp
 * @brief Frame rate limit: PlusFpsLimit (Graphics > Advanced, under VSync) is the most frames per second the
 *        main loop makes; 0 = no limit. The wait uses a high resolution timer and ends with a short yield
 *        loop, so the rate holds to a fraction of a millisecond. The "match monitor" button sets the limit
 *        to the refresh rate of the monitor the window is on.
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
#include "plus/fpslimit/fpslimit.h"

#include "llrhi.h"
#include "llperfstats.h"
#include "llui.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <algorithm>
#include <chrono>
#include <iterator>

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace plus::fpslimit
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr char SETTING_LIMIT[] = "PlusFpsLimit";
        constexpr S32 MIN_LIMIT = 10;       // below this the viewer is hard to use: taken as this
        constexpr S32 MAX_LIMIT = 1000;
        // The last part of the wait is a yield loop (the timer can end a little late).
        constexpr std::chrono::microseconds SPIN_MARGIN(1500);

        Clock::time_point sNext;            // when the next frame may start
        bool sHaveNext = false;

#if LL_WINDOWS
        // A timer that ends within about a millisecond (Windows 10 1803+), or an ordinary one (15 ms at
        // worst: then most of the wait is spun, with a yield, rather than slept).
        HANDLE sTimer = nullptr;
        bool sTimerFine = false;

        void makeTimer()
        {
            sTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
            sTimerFine = (sTimer != nullptr);
            if (!sTimer)
            {
                sTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            }
        }

        void sleepUntil(Clock::time_point until)
        {
            if (!sTimer)
            {
                makeTimer();
            }
            const auto spin_from = until - (sTimerFine ? SPIN_MARGIN : std::chrono::microseconds(16000));
            const auto now = Clock::now();
            if (sTimer && spin_from > now)
            {
                // Negative = relative, in 100 ns.
                LARGE_INTEGER due;
                due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(spin_from - now).count() / 100;
                if (SetWaitableTimer(sTimer, &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject(sTimer, INFINITE);
                }
            }
            while (Clock::now() < until)
            {
                SwitchToThread();
            }
        }
#else
        void sleepUntil(Clock::time_point until)
        {
            const auto now = Clock::now();
            if (until > now)
            {
                ms_sleep((U32)std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count());
            }
        }
#endif

        // Windows gives the refresh rate as a whole number, rounded down: a
        // 59.94 Hz mode (NTSC-derived: 60, 120, 144, 240 x 1000/1001) reads 59.
        // With VSync on, a limit of 59 under VSync's 59.94 shows a frame twice
        // about once a second (a hitch): the limit goes one up there (60), so
        // that VSync, not the limit, sets the pace. Those rates only (were the
        // mode a true 59 Hz, one above it still leaves VSync the pace).
        constexpr int FRACTIONAL_RATES_FLOORED[] = { 59, 119, 143, 239 };

        void matchMonitor(LLUICtrl*, const LLSD&)
        {
            const int hz = monitorRefreshRate();
            if (hz > 1)
            {
                int limit = hz;
                if (gSavedSettings.getBOOL("RenderVSyncEnable") &&
                    std::find(std::begin(FRACTIONAL_RATES_FLOORED), std::end(FRACTIONAL_RATES_FLOORED), hz) !=
                        std::end(FRACTIONAL_RATES_FLOORED))
                {
                    limit = hz + 1;
                }
                gSavedSettings.setS32(SETTING_LIMIT, llclamp((S32)limit, MIN_LIMIT, MAX_LIMIT));
                LL_INFOS("Window") << "Frame rate limit set to " << limit << " for the monitor's " << hz << " Hz"
                                   << (limit != hz ? " (a fractional rate, VSync on: one up, VSync sets the pace)" : "")
                                   << LL_ENDL;
            }
        }

        void init()
        {
            // Setting: app_settings/settings_plus.xml (PlusFpsLimit). The row and button:
            // floater_preferences_graphics_advanced.xml (en, ja), the Rendering tab, above "Enable VSync".
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.FpsLimit.MatchMonitor", &matchMonitor);
        }

        plus::FeatureRegistration sRegistration({ "fpslimit", &init });
    }

    int monitorRefreshRate()
    {
#if LL_WINDOWS
        HWND window = (gViewerWindow && gViewerWindow->getWindow())
                          ? (HWND)gViewerWindow->getWindow()->getPlatformWindow() : nullptr;
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
        {
            return 0;
        }
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        {
            return 0;
        }
        // 0 and 1 mean "the hardware default": not a rate.
        return mode.dmDisplayFrequency > 1 ? (int)mode.dmDisplayFrequency : 0;
#else
        return 0;
#endif
    }

    void wait()
    {
        static LLCachedControl<S32> limit(gSavedSettings, SETTING_LIMIT, 0);
        const S32 fps = limit;
        if (fps <= 0)
        {
            sHaveNext = false;
            return;
        }

        const auto interval = std::chrono::nanoseconds(1000000000LL / llclamp(fps, MIN_LIMIT, MAX_LIMIT));
        const auto now = Clock::now();
        if (!sHaveNext)
        {
            sHaveNext = true;
            sNext = now + interval;
            return;
        }
        if (now < sNext)
        {
            LLPerfStats::RecordSceneTime T(LLPerfStats::StatType_t::RENDER_SLEEP);
            LLRHI::cpuMark("fps limit");
            sleepUntil(sNext);
            LLRHI::cpuMark("loop");
            sNext += interval;
        }
        else
        {
            // Late: the next frame is not rushed to make up for it.
            sNext = now + interval;
        }
    }
}
