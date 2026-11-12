/**
 * @file detachwin32.h
 * @brief The OS window of a detached floater (Windows): borderless, owned by
 *        the main window, its input handled as the main window's.
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

#ifndef PLUS_DETACH_DETACHWIN32_H
#define PLUS_DETACH_DETACHWIN32_H

#include "stdtypes.h"

#include <memory>
#include <string>

namespace plus::detach
{
    // A rectangle on the desktop, in pixels (top-left origin, right and bottom exclusive).
    struct ScreenRect
    {
        S32 left = 0;
        S32 top = 0;
        S32 right = 0;
        S32 bottom = 0;

        S32 width() const { return right - left; }
        S32 height() const { return bottom - top; }
        bool empty() const { return width() <= 0 || height() <= 0; }
        bool operator==(const ScreenRect& other) const
        {
            return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
        }
        bool operator!=(const ScreenRect& other) const { return !(*this == other); }

        // Moved (and shrunk if need be) to lie inside area.
        ScreenRect fittedInto(const ScreenRect& area) const;
        // The same size, centred on area (shrunk to fit it).
        ScreenRect centredOn(const ScreenRect& area) const;
    };

    // A monitor of the desktop.
    struct Monitor
    {
        std::string device;     // the display's device name (\\.\DISPLAY2), stable while it stays plugged in
        ScreenRect bounds;      // the whole monitor
        ScreenRect work;        // without the taskbar
    };

    // A borderless top-level window owned by the main window: always in front
    // of it, minimized with it, no taskbar button, not in Alt+Tab (the tool
    // windows of an IDE). It is made on the viewer's window thread, whose
    // message loop serves every window of that thread. Its input is handled by
    // the main window's own message handling (LLWindowWin32::mainWindowProc),
    // as if it happened there: mouse points are moved into the main window's
    // client coordinates (outside its client area, where the UI goes on past
    // the main window), keys, characters and IME messages go as they are.
    // Focus moving between it and the main window is not the viewer losing
    // focus (the <plus> hooks in mainWindowProc). Main thread.
    class OsWindow
    {
    public:
        // Starts making the window at rect; handle() is null until the window
        // thread has made it (a frame or so). take_mouse: a drag going on (a
        // floater dragged off the main window) goes on on this window: it
        // takes the mouse capture once made, as if the drag had started on it
        // (or, the button up by then, the drag ends).
        OsWindow(const ScreenRect& rect, const std::wstring& title, bool take_mouse = false);
        // Destroys the window. The renderer must have let it go first
        // (LLRHI::sForgetWindow(handle())).
        ~OsWindow();

        OsWindow(const OsWindow&) = delete;
        OsWindow& operator=(const OsWindow&) = delete;

        // The HWND, or null while it is being made.
        void* handle() const;
        // The window could not be made (logged): handle() stays null for good.
        bool failed() const;

        // Moves and sizes the window (asynchronously; nothing when unchanged).
        void setRect(const ScreenRect& rect);

        // For the log (LLRHI_DETACH_LOG=3, research/105): what Windows makes
        // of the window (visible, styles, cloaked, DPI, the window under its
        // middle) and the colour the screen shows there. Slow (reads the
        // screen): every few seconds at most.
        std::string describe() const;

        // The windows' corners are round, as the floater's background drawn
        // in them. Windows 11 and later: DWM rounds them (anti-aliased, with
        // its own radius; no border of its own, the floater has one). Before:
        // each window's shape is cut to a rounded region of this radius, the
        // floaters' corner radius in pixels at the UI's scale, kept through
        // every resize (and the corners' outside no longer takes the mouse).
        // Main thread; nothing when unchanged (cheap: call it every idle).
        static void setCornerRadius(S32 pixels);

        // Sets the <plus> hooks of llwindowwin32.cpp (which windows are these,
        // where the IME's windows go). Once, before the main window exists.
        static void installHooks();

        // A mouse button went down on one of these windows and is still held
        // (the window has the mouse capture).
        static bool hasMouseCapture();

        // While the UI of window (handle()) is drawn with its corner as the
        // UI's origin (plus/detach drawWindows), text fields report the IME's
        // place measured from that corner: the IME hook takes it from there.
        // Main thread; height: the window's client height in pixels.
        static void beginDrawing(void* window, S32 height);
        static void endDrawing();

        // The desktop's monitors.
        // The work area of one that does not show the main window (the first
        // such), else of the main window's own.
        static ScreenRect otherMonitorWorkArea();
        // The monitor showing the main window (the most of it).
        static Monitor mainMonitor();
        // The monitor showing the most of rect (the nearest when none does).
        static Monitor monitorOf(const ScreenRect& rect);
        // The monitor with that device name, if it is plugged in.
        static bool findMonitor(const std::string& device, Monitor& out);

        // The mouse cursor is off the main window (its whole frame, grown by
        // margin pixels on each side). False when that cannot be told.
        static bool cursorOffMainWindow(S32 margin);

        // A mouse button (left, right, middle; as the hardware has them) is
        // down now. True where that cannot be told.
        static bool anyMouseButtonDown();

        // Messages handed to the main window's handling that came back to a
        // detached window's own procedure too deep (a loop broken; logged once).
        static U32 forwardLoopsBroken();

    private:
        struct Shared;
        std::shared_ptr<Shared> mShared;    // with the window thread's work
        ScreenRect mRect;                   // last asked for
    };
}

#endif // PLUS_DETACH_DETACHWIN32_H
