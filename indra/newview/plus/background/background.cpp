/**
 * @file background.cpp
 * @brief A window that lost focus but can still be seen keeps working as in front (plus/background)
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

#include "plus/background/background.h"

#include "llappviewer.h"
#include "llframetimer.h"
#include "llrhi.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <chrono>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <dwmapi.h>     // DwmGetWindowAttribute (dwmapi.lib is linked by llwindowwin32.cpp)
#endif

namespace plus::background
{
    namespace
    {
        constexpr F64 LOG_MIN_SECONDS = 5.0;    // between two log lines
        constexpr F64 EPISODE_GAP_SECONDS = 2.0;
        constexpr std::chrono::milliseconds SEEN_RECHECK(500);  // the slower questions are asked again after this

        // The callers ask only while the window has no focus (a call every frame, more with media), so a gap
        // between calls is the focus coming back: the next call starts a new stay without focus.
        F64 sLastCall = -1000.0;
        F64 sLastLog = -LOG_MIN_SECONDS;

        // What stops a window that is on and not minimized from being seen (None: it can be).
        enum class Hidden : U8
        {
            None,
            NoDisplayOrDesktop,     // on no display, or the session has no input desktop (locked, remote desktop disconnected, user switched)
            OtherDesktop,           // cloaked by the desktop manager (another virtual desktop)
            ScreenSaver,
            Covered                 // all nine points of the window are another program's windows (research/303 case C)
        };

        const char* hiddenName(Hidden why)
        {
            switch (why)
            {
            case Hidden::NoDisplayOrDesktop: return "on no display, or the computer is locked or its remote desktop is disconnected";
            case Hidden::OtherDesktop: return "on another virtual desktop";
            case Hidden::ScreenSaver: return "a screen saver is running";
            case Hidden::Covered: return "other programs' windows cover it";
            default: return "";
            }
        }

#if LL_WINDOWS
        // Case C (research/303): whether other programs' windows cover the window. Nine points of its client area (the
        // four corners a little in, the middle of each side, the middle) are asked of WindowFromPoint, once a second
        // of real time. One point on a window of this process (the viewer's own, plus/detach's windows too) means
        // seen. So does anything that cannot be judged: a point off every monitor, no window answered, an answer
        // that is layered or transparent for the mouse (overlays: OBS, Discord, the snap helpers) or whose process
        // is not told: the answer is always "seen" then, a wrong "covered" being the worst that can happen (the
        // picture lowered for a window that is seen). Covered only when all nine points are judged other programs'
        // windows, and on two checks in a row (a click-through window of a moment does not flip it); seen at once.
        constexpr std::chrono::milliseconds COVER_RECHECK(1000);
        constexpr int COVER_INSET_PIXELS = 3;
        constexpr U32 COVER_CHECKS_IN_A_ROW = 2;

        // True: the point is on another program's window. False: it is on this process's window, or it cannot be told.
        bool pointIsOthers(const POINT& point, DWORD own_process)
        {
            if (!MonitorFromPoint(point, MONITOR_DEFAULTTONULL))
            {
                return false;
            }
            const HWND hit = WindowFromPoint(point);
            if (!hit)
            {
                return false;
            }
            const HWND root_window = GetAncestor(hit, GA_ROOT);
            const HWND top = root_window ? root_window : hit;
            DWORD process = 0;
            if (!GetWindowThreadProcessId(top, &process) || process == 0 || process == own_process)
            {
                return false;
            }
            DWORD cloaked = 0;
            if (FAILED(DwmGetWindowAttribute(top, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked != 0)
            {
                return false;   // a window the desktop manager hides (another virtual desktop) covers nothing; cannot be told
            }
            const LONG_PTR ex_style = GetWindowLongPtrW(top, GWL_EXSTYLE);
            return (ex_style & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) == 0;
        }

        bool allNinePointsAreOthers(HWND hwnd)
        {
            RECT client;
            POINT origin = { 0, 0 };
            if (!hwnd || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
            {
                return false;
            }
            const LONG width = client.right - client.left;
            const LONG height = client.bottom - client.top;
            if (width < 4 * COVER_INSET_PIXELS || height < 4 * COVER_INSET_PIXELS)
            {
                return false;
            }
            const LONG xs[3] = { origin.x + COVER_INSET_PIXELS, origin.x + width / 2, origin.x + width - 1 - COVER_INSET_PIXELS };
            const LONG ys[3] = { origin.y + COVER_INSET_PIXELS, origin.y + height / 2, origin.y + height - 1 - COVER_INSET_PIXELS };
            const DWORD own_process = GetCurrentProcessId();
            for (const LONG y : ys)
            {
                for (const LONG x : xs)
                {
                    const POINT point = { x, y };
                    if (!pointIsOthers(point, own_process))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // Whether a window covered by other programs' windows counts as covered (PlusBackgroundHiddenStop, under
        // PlusBackgroundKeepVisible; LLRHI_BACKGROUND_HIDDEN=0 turns it off whatever the setting says).
        bool hiddenStopOn()
        {
            static LLCachedControl<bool> setting(gSavedSettings, "PlusBackgroundHiddenStop", true);
            static const bool env_on = LLRHI::envDefaultOn("LLRHI_BACKGROUND_HIDDEN");
            return setting() && env_on;
        }

        // The settled answer, asked again every COVER_RECHECK.
        bool coveredByOthers(HWND hwnd)
        {
            static bool sCovered = false;
            static U32 sInARow = 0;
            static std::chrono::steady_clock::time_point sChecked;
            static bool sHaveChecked = false;
            const auto now = std::chrono::steady_clock::now();
            if (sHaveChecked && now - sChecked < COVER_RECHECK)
            {
                return sCovered;
            }
            if (!sHaveChecked || now - sChecked >= 2 * COVER_RECHECK)
            {
                // Nobody asked for a while (the window had focus, or was minimized): what was found then is old
                sInARow = 0;
                sCovered = false;
            }
            sChecked = now;
            sHaveChecked = true;
            if (allNinePointsAreOthers(hwnd))
            {
                sInARow = llmin(sInARow + 1, COVER_CHECKS_IN_A_ROW);
                sCovered = sInARow >= COVER_CHECKS_IN_A_ROW;
            }
            else
            {
                sInARow = 0;
                sCovered = false;
            }
            return sCovered;
        }
#endif

        // The questions that are slower than a window query (an input desktop is opened, the screen saver and the
        // desktop manager asked): at most every SEEN_RECHECK of real time, not a frame count (a count stands still
        // while minimized), and each change of the answer is logged.
        Hidden whyHidden(void* window)
        {
            static Hidden sWhy = Hidden::None;
            static std::chrono::steady_clock::time_point sChecked;
            static bool sHaveChecked = false;
            const auto now = std::chrono::steady_clock::now();
            if (sHaveChecked && now - sChecked < SEEN_RECHECK)
            {
                return sWhy;
            }
            sChecked = now;
            sHaveChecked = true;
            Hidden why = Hidden::None;
#if LL_WINDOWS
            const HWND hwnd = (HWND)window;
            DWORD cloaked = 0;
            BOOL screen_saver = FALSE;
            if (!LLRHI::windowOnADisplay(window, false))
            {
                why = Hidden::NoDisplayOrDesktop;
            }
            else if (hwnd && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
            {
                why = Hidden::OtherDesktop;
            }
            else if (SystemParametersInfoW(SPI_GETSCREENSAVERRUNNING, 0, &screen_saver, 0) && screen_saver)
            {
                why = Hidden::ScreenSaver;
            }
            else if (hiddenStopOn() && coveredByOthers(hwnd))
            {
                why = Hidden::Covered;
            }
#else
            (void)window;
#endif
            if (why != sWhy)
            {
                sWhy = why;
                if (why == Hidden::None)
                {
                    LL_INFOS("Background") << "the window can be seen again: it keeps working as in front while without focus" << LL_ENDL;
                }
                else
                {
                    LL_INFOS("Background") << "the window cannot be seen (" << hiddenName(why) << "): as upstream" << LL_ENDL;
                }
            }
            return sWhy;
        }

        // One line when a stay without focus begins and the window is kept (at most one line every LOG_MIN_SECONDS).
        void noteCall(bool kept)
        {
            const F64 now = LLFrameTimer::getElapsedSeconds();
            const bool begins = now - sLastCall > EPISODE_GAP_SECONDS;
            sLastCall = now;
            if (!kept || !begins || now - sLastLog < LOG_MIN_SECONDS)
            {
                return;
            }
            sLastLog = now;
            LL_INFOS("Background") << "the window has no focus but can be seen: it keeps working as in front "
                                      "(plus/background, PlusBackgroundKeepVisible)"
                                   << LL_ENDL;
        }
    }

    namespace
    {
        // Two window queries and a setting read at each call (a call at most per frame and per media in the scene;
        // no frame-count cache: the count stands still while minimized and a stale answer would stay), and the
        // slower questions of whyHidden every half second. The window is kept (visible, not minimized, the setting
        // on) with why: None, or Covered (other programs' windows over it), or another reason it cannot be seen.
        bool kept(Hidden& why)
        {
            static LLCachedControl<bool> setting(gSavedSettings, "PlusBackgroundKeepVisible", true);
            static const bool env_on = LLRHI::envDefaultOn("LLRHI_BACKGROUND_KEEP");
            why = Hidden::None;
            if (!setting() || !env_on || !gViewerWindow || !gViewerWindow->getWindow())
            {
                return false;
            }
            LLWindow* window = gViewerWindow->getWindow();
            if (!window->getVisible() || window->getMinimized())
            {
                return false;
            }
            why = whyHidden(window->getPlatformWindow());
            return true;
        }
    }

    bool treatAsForeground()
    {
        Hidden why;
        const bool keep = kept(why) && why == Hidden::None;
        noteCall(keep);
        return keep;
    }

    bool keepTextures()
    {
        // A covered window is the one case that can be wrong while the window is seen (a window capture of it, a window
        // that is see-through without being layered): there the textures stay, and the sleep and the media's priority
        // are the official ones (a lower frame rate is all a wrong verdict costs).
        Hidden why;
        return kept(why) && (why == Hidden::None || why == Hidden::Covered);
    }
}
