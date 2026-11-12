/**
 * @file detachwin32.cpp
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

#include "llviewerprecompiledheaders.h"

#include "detachwin32.h"

#include "llviewerwindow.h"

#if LL_WINDOWS

#include "llwin32headers.h"

#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

// llwindowwin32.cpp, the <plus> hooks of its window procedure.
extern bool (*gPlusIsDetachedWindow)(HWND window);
extern void (*gPlusMapImePoints)(HWND main_window, POINT* points, int count);
bool plus_post_to_window_thread(HWND main_window, std::function<void()> func);

namespace
{
    constexpr wchar_t WINDOW_CLASS[] = L"CharisDetachedFloater";

    // How deep a message handed to the main window's handling may come back
    // to a detached window's own procedure (DefWindowProc sends some messages
    // on: a few levels are normal). Deeper is a loop, broken here.
    constexpr int MAX_FORWARD_DEPTH = 8;

    // The main window and its window procedure: set before the first window is
    // made, on the main thread and the window thread respectively.
    HWND sMainWindow = NULL;
    std::atomic<WNDPROC> sMainWindowProc{ nullptr };

    // The windows that exist (the window thread adds and removes them), the one
    // with the keyboard focus and the one with the mouse capture.
    std::mutex sWindowsMutex;
    std::vector<HWND> sWindows;
    std::atomic<HWND> sFocused{ NULL };
    std::atomic<HWND> sCaptured{ NULL };

    // LLRHI_DETACH_STYLE (research/105, a test for windows that stay black):
    // words, comma separated, each changing how the windows are made.
    //   noredirect  WS_EX_NOREDIRECTIONBITMAP (no GDI surface behind Vulkan's)
    //   unowned     no owner window (a top-level window of its own)
    //   caption     a normal window with a title bar and frame
    //   erase       the background is painted grey (GDI): a window that then
    //               shows grey gets none of Vulkan's pictures on the screen
    constexpr char ENV_STYLE[] = "LLRHI_DETACH_STYLE";

    struct StyleTest
    {
        bool no_redirect = false;
        bool unowned = false;
        bool caption = false;
        bool erase = false;
    };

    const StyleTest& styleTest()
    {
        static const StyleTest test = []()
        {
            StyleTest out;
            const char* env = getenv(ENV_STYLE);
            const std::string words = env ? env : "";
            auto has = [&words](const char* word) { return words.find(word) != std::string::npos; };
            out.no_redirect = has("noredirect");
            out.unowned = has("unowned");
            out.caption = has("caption");
            out.erase = has("erase");
            if (env && *env)
            {
                LL_INFOS("Detach") << "Detached windows made with " << ENV_STYLE << "=" << words << LL_ENDL;
            }
            return out;
        }();
        return test;
    }

    // How the windows' corners are rounded (roundCorners()): set on the
    // window thread as each window is made (the system's answer, the same
    // for every window), read on both threads.
    enum class Corners : U8
    {
        Unknown,    // no window made yet (or only test windows with a frame)
        Dwm,        // Windows 11 and later: DWM rounds them
        Region,     // before: each window's shape is a rounded region (cutCorners())
    };
    std::atomic<Corners> sCorners{ Corners::Unknown };
    // OsWindow::setCornerRadius(): written on the main thread, read on the
    // window thread (cutCorners()). Pixels.
    std::atomic<S32> sCornerRadius{ 0 };
    constexpr S32 MAX_CORNER_RADIUS = 64;   // pixels: the floaters' radius at any UI scale is less

    // Window thread: window's shape, a rectangle whose corners are rounded
    // by sCornerRadius (square while that is 0, or while the window is
    // maximized or minimized). Windows owns the region once it is set.
    void cutCorners(HWND window)
    {
        RECT rect = {};
        GetWindowRect(window, &rect);
        const S32 width = rect.right - rect.left;
        const S32 height = rect.bottom - rect.top;
        const S32 radius = sCornerRadius.load();
        HRGN region = NULL;
        if (radius > 0 && width > 0 && height > 0 && !IsZoomed(window) && !IsIconic(window))
        {
            // The corners' ellipse is 2 radii across; the region leaves out
            // its right column and bottom row, hence one more.
            region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 2 * radius, 2 * radius);
        }
        if (!SetWindowRgn(window, region, TRUE) && region)
        {
            DeleteObject(region);
        }
    }

    // Window thread, as the window is made: its corners rounded (as the
    // floater's background drawn in it is). DWM does it where it can
    // (Windows 11 and later), keeping it through every size and drawing the
    // edge anti-aliased; its own thin border is left out (the floater draws
    // one). Before Windows 11 the attribute is unknown (the call fails, no
    // harm done): the window's shape is cut instead, again at each WM_SIZE.
    void roundCorners(HWND window)
    {
        if (styleTest().caption)
        {
            return;     // a test window with a frame: Windows' own corners
        }
        const DWM_WINDOW_CORNER_PREFERENCE round = DWMWCP_ROUND;
        if (SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round))))
        {
            const COLORREF no_border = DWMWA_COLOR_NONE;
            DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &no_border, sizeof(no_border));
            if (sCorners.exchange(Corners::Dwm) != Corners::Dwm)
            {
                LL_INFOS("Detach") << "Detached windows: corners rounded by DWM" << LL_ENDL;
            }
            return;
        }
        if (sCorners.exchange(Corners::Region) != Corners::Region)
        {
            LL_INFOS("Detach") << "Detached windows: corners rounded by a window region (no DWM corner preference)" << LL_ENDL;
        }
        cutCorners(window);
    }

    // Window thread: how deep toMainWindow() is nested; loops broken.
    thread_local int tForwardDepth = 0;
    std::atomic<U32> sForwardLoopsBroken{ 0 };

    // Main thread: the window whose UI is being drawn (OsWindow::beginDrawing).
    struct Drawing
    {
        HWND window = NULL;
        S32 height = 0;
    };
    thread_local Drawing tDrawing;

    bool isDetachedWindow(HWND window)
    {
        if (!window)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(sWindowsMutex);
        return std::find(sWindows.begin(), sWindows.end(), window) != sWindows.end();
    }

    // gPlusMapImePoints: the IME's points, given in the main window's client
    // area, to the client area of the window with the keyboard focus. While a
    // detached window's UI is drawn (main thread), text fields measure from
    // that window's corner but the main window converts with its own height:
    // the points are that window's, moved by the difference in height.
    void mapImePoints(HWND main_window, POINT* points, int count)
    {
        HWND from = main_window;
        if (tDrawing.window)
        {
            RECT main_client = {};
            GetClientRect(main_window, &main_client);
            const LONG dy = tDrawing.height - (main_client.bottom - main_client.top);
            for (int i = 0; i < count; ++i)
            {
                points[i].y += dy;
            }
            from = tDrawing.window;
        }
        HWND focused = sFocused.load();
        HWND to = focused ? focused : main_window;
        if (from != to)
        {
            MapWindowPoints(from, to, points, count);
        }
    }

    bool isViewerWindow(HWND window)
    {
        return window && (window == sMainWindow || isDetachedWindow(window));
    }

    // The main window's message handling, as if the message had come to it.
    // h_wnd stays this window's: the IME state is read from its input context
    // (the thread's, shared with the main window) and DefWindowProc acts on it.
    LRESULT toMainWindow(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
    {
        const WNDPROC main_proc = sMainWindowProc.load();
        if (!main_proc || tForwardDepth >= MAX_FORWARD_DEPTH)
        {
            if (main_proc && sForwardLoopsBroken.fetch_add(1) == 0)
            {
                LL_WARNS("Detach") << "A message to a detached window looped through the main window's handling "
                                   << "(message 0x" << std::hex << message << std::dec << "); broken" << LL_ENDL;
            }
            return DefWindowProc(window, message, w_param, l_param);
        }
        ++tForwardDepth;
        const LRESULT result = CallWindowProc(main_proc, window, message, w_param, l_param);
        --tForwardDepth;
        return result;
    }

    // The mouse message's point as a client point of the main window (outside
    // its client area: the UI goes on past the main window). Taken from the
    // message's screen position, not its client point: while a drag moves the
    // window (asynchronously), a client point would be measured from where
    // the window was.
    // screen: a screen point packed as GetMessagePos() returns it.
    LPARAM toMainClient(DWORD screen)
    {
        POINT point = { (S16)LOWORD(screen), (S16)HIWORD(screen) };
        ScreenToClient(sMainWindow, &point);
        return MAKELPARAM((WORD)(S16)point.x, (WORD)(S16)point.y);
    }

    // The cursor's screen point now, packed as GetMessagePos() returns it
    // (WM_MOUSELEAVE is posted: its message point may be old).
    DWORD cursorNow()
    {
        POINT point = {};
        GetCursorPos(&point);
        return MAKELONG((WORD)(S16)point.x, (WORD)(S16)point.y);
    }

    // Window thread: the window asked to send WM_MOUSELEAVE (Windows drops
    // the request once it has sent it).
    thread_local HWND tLeaveTracked = NULL;

    // Asks for WM_MOUSELEAVE when the mouse leaves window, once per entry.
    void trackLeave(HWND window)
    {
        if (tLeaveTracked == window)
        {
            return;
        }
        TRACKMOUSEEVENT track = { sizeof(track) };
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        if (TrackMouseEvent(&track))
        {
            tLeaveTracked = window;
        }
    }

    bool isButtonDown(UINT message)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            return true;
        default:
            return false;
        }
    }

    bool isButtonUp(UINT message)
    {
        return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
    }

    constexpr WPARAM ANY_BUTTON = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;

    // Window thread: window takes the mouse capture (followCapture()).
    void capture(HWND window)
    {
        SetCapture(window);
        sCaptured = window;
        tLeaveTracked = NULL;   // asked again at the next move (the capture may have dropped the request)
    }

    // A mouse button is down now (the physical ones, swapped or not).
    bool anyButtonDown()
    {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
               (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
    }

    // A drag that starts here goes on here (and so in the UI's coordinates)
    // when the mouse leaves the window, until the last button is up.
    // Returns false for a move that found the capture held with no button
    // down (the button went up unseen): the capture is let go and the move
    // is to go to the UI as the left button going up, ending the drag.
    bool followCapture(HWND window, UINT message, WPARAM w_param)
    {
        if (isButtonDown(message))
        {
            capture(window);
        }
        else if (isButtonUp(message) && !(w_param & ANY_BUTTON) && GetCapture() == window)
        {
            ReleaseCapture();   // WM_CAPTURECHANGED clears sCaptured
        }
        else if (message == WM_MOUSEMOVE && !(w_param & ANY_BUTTON) && GetCapture() == window)
        {
            ReleaseCapture();
            return false;
        }
        return true;
    }

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
    {
        switch (message)
        {
        case WM_MOUSELEAVE:
            if (tLeaveTracked == window)
            {
                tLeaveTracked = NULL;
            }
            // The mouse left this window for the desktop or another window:
            // the UI hears where it is now (outside the window, so the views
            // it was over get onMouseLeave; the main window's handling keeps
            // such a point at its edge, as when the mouse leaves it). Not
            // while a drag that started here holds the mouse.
            if (sCaptured.load() != window)
            {
                return toMainWindow(window, WM_MOUSEMOVE, 0, toMainClient(cursorNow()));
            }
            return 0;

        case WM_MOUSEMOVE:
            trackLeave(window);
            [[fallthrough]];
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            if (!followCapture(window, message, w_param))
            {
                return toMainWindow(window, WM_LBUTTONUP, w_param, toMainClient(GetMessagePos()));
            }
            return toMainWindow(window, message, w_param, toMainClient(GetMessagePos()));

        // Screen points (the wheel), or no point at all.
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_IME_SETCONTEXT:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_REQUEST:
        case WM_INPUT:
            return toMainWindow(window, message, w_param, l_param);

        case WM_SETCURSOR:
            if (LOWORD(l_param) == HTCLIENT)
            {
                return toMainWindow(window, message, w_param, l_param);   // the UI's cursor
            }
            break;

        case WM_SETFOCUS:
            sFocused = window;
            if (!isViewerWindow((HWND)w_param))
            {
                toMainWindow(window, message, w_param, l_param);    // the viewer has the focus again
            }
            return 0;

        case WM_KILLFOCUS:
        {
            HWND self = window;
            sFocused.compare_exchange_strong(self, NULL);
            if (!isViewerWindow((HWND)w_param))
            {
                toMainWindow(window, message, w_param, l_param);    // the focus left the viewer
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            tLeaveTracked = NULL;   // asked again at the next move
            HWND self = window;
            sCaptured.compare_exchange_strong(self, NULL);
            return 0;
        }

        case WM_SYSCOMMAND:
            switch (w_param & 0xFFF0)
            {
            case SC_KEYMENU:
                return 0;   // as the main window: Alt does not open a system menu
            case SC_CLOSE:
                PostMessage(sMainWindow, WM_SYSCOMMAND, SC_CLOSE, 0);  // Alt+F4 here closes the viewer, as in the main window
                return 0;
            default:
                break;
            }
            break;

        case WM_CLOSE:
            return 0;       // the viewer closes it (the floater's close button puts the floater back)

        case WM_ERASEBKGND:
            if (styleTest().erase)
            {
                RECT client = {};
                GetClientRect(window, &client);
                FillRect((HDC)w_param, &client, (HBRUSH)GetStockObject(GRAY_BRUSH));
            }
            return 1;       // Vulkan draws all of it

        case WM_PAINT:
            if (styleTest().erase)
            {
                PAINTSTRUCT paint;
                BeginPaint(window, &paint);    // sends WM_ERASEBKGND
                EndPaint(window, &paint);
                return 0;
            }
            ValidateRect(window, NULL);
            return 0;

        case WM_DPICHANGED:
            return 0;       // keeps its size: the UI is drawn at the main window's scale (research/87 4 (g))

        case WM_SIZE:
            if (sCorners.load() == Corners::Region)
            {
                cutCorners(window);     // the region was cut to the old size
            }
            break;

        case WM_DESTROY:
        {
            {
                std::lock_guard<std::mutex> lock(sWindowsMutex);
                sWindows.erase(std::remove(sWindows.begin(), sWindows.end(), window), sWindows.end());
            }
            HWND self = window;
            sFocused.compare_exchange_strong(self, NULL);
            self = window;
            sCaptured.compare_exchange_strong(self, NULL);
            if (tLeaveTracked == window)
            {
                tLeaveTracked = NULL;
            }
            break;
        }

        default:
            break;
        }
        return DefWindowProc(window, message, w_param, l_param);
    }

    // Window thread.
    bool registerClass()
    {
        static const bool registered = []()
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.style = CS_DBLCLKS;      // double clicks, as the main window gets them
            wc.lpfnWndProc = &windowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = WINDOW_CLASS;
            return RegisterClassExW(&wc) != 0;
        }();
        return registered;
    }
}

namespace plus::detach
{
    struct OsWindow::Shared
    {
        std::atomic<HWND> handle{ NULL };
        std::atomic<bool> closed{ false };
        std::atomic<bool> failed{ false };  // the window could not be made; it never will be
    };

    OsWindow::OsWindow(const ScreenRect& rect, const std::wstring& title, bool take_mouse)
        : mShared(std::make_shared<Shared>()),
          mRect(rect)
    {
        if (!sMainWindow)
        {
            sMainWindow = (HWND)gViewerWindow->getPlatformWindow();
        }
        std::shared_ptr<Shared> shared = mShared;
        const bool posted = plus_post_to_window_thread(sMainWindow, [shared, rect, title, take_mouse]()
        {
            if (shared->closed)
            {
                return;
            }
            if (!sMainWindowProc.load())
            {
                // The class's procedure, LLWindowWin32::mainWindowProc. Not the
                // window's (GWLP_WNDPROC): opengl32 subclasses the main window
                // for its GL context, and its forwarder, called for another
                // window, calls that window's own procedure back (an endless
                // recursion, research/96).
                sMainWindowProc = (WNDPROC)GetClassLongPtr(sMainWindow, GCLP_WNDPROC);
            }
            if (!registerClass())
            {
                LL_WARNS("Detach") << "Cannot register the window class for detached floaters" << LL_ENDL;
                shared->failed = true;
                return;
            }
            // Owned by the main window: in front of it, minimized with it, no taskbar button.
            const StyleTest& test = styleTest();
            const DWORD ex_style = test.no_redirect ? WS_EX_NOREDIRECTIONBITMAP : 0;
            const DWORD style = (test.caption ? WS_OVERLAPPEDWINDOW : WS_POPUP) | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
            HWND window = CreateWindowExW(ex_style, WINDOW_CLASS, title.c_str(), style,
                                          rect.left, rect.top, rect.width(), rect.height(), test.unowned ? NULL : sMainWindow,
                                          NULL, GetModuleHandle(NULL), NULL);
            if (!window)
            {
                LL_WARNS("Detach") << "Cannot make a window for a detached floater (error " << GetLastError() << ")" << LL_ENDL;
                shared->failed = true;
                return;
            }
            // mainWindowProc finds its LLWindowWin32 here.
            SetWindowLongPtr(window, GWLP_USERDATA, GetWindowLongPtr(sMainWindow, GWLP_USERDATA));
            {
                std::lock_guard<std::mutex> lock(sWindowsMutex);
                sWindows.push_back(window);
            }
            roundCorners(window);
            shared->handle = window;
            if (shared->closed)
            {
                // ~OsWindow() came meanwhile and could not post the destruction (or it is queued
                // behind this: it then finds no window).
                if (HWND gone = shared->handle.exchange(NULL))
                {
                    DestroyWindow(gone);
                }
                return;
            }
            ShowWindow(window, SW_SHOWNOACTIVATE);
            if (take_mouse)
            {
                // The drag goes on here, wherever the mouse goes. (The main
                // window's own capture, LLWindowWin32::captureMouse(), is set
                // from the main thread for a window of this thread, and holds
                // nothing: off the main window the moves went elsewhere.)
                if (anyButtonDown())
                {
                    capture(window);
                }
                else
                {
                    // Let go meanwhile, maybe where the viewer did not hear it: the drag ends.
                    toMainWindow(window, WM_LBUTTONUP, 0, toMainClient(cursorNow()));
                }
            }
        });
        if (!posted)
        {
            shared->failed = true;
        }
    }

    OsWindow::~OsWindow()
    {
        mShared->closed = true;
        std::shared_ptr<Shared> shared = mShared;
        // After the work that makes it (the window thread does both in order).
        const bool posted = plus_post_to_window_thread(sMainWindow, [shared]()
        {
            HWND window = shared->handle.exchange(NULL);
            if (window)
            {
                DestroyWindow(window);
            }
        });
        if (!posted)
        {
            // Only its own thread may destroy it: hidden, it goes with the main window (its owner).
            if (HWND window = shared->handle.exchange(NULL))
            {
                ShowWindowAsync(window, SW_HIDE);
            }
        }
    }

    bool OsWindow::failed() const
    {
        return mShared->failed.load();
    }

    void* OsWindow::handle() const
    {
        return mShared->handle.load();
    }

    void OsWindow::setRect(const ScreenRect& rect)
    {
        HWND window = mShared->handle.load();
        if (!window)
        {
            return;     // made at the rect it was asked for
        }
        if (rect != mRect)
        {
            mRect = rect;
            // Posted to the window thread: the main thread never waits for it.
            SetWindowPos(window, NULL, rect.left, rect.top, rect.width(), rect.height(),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }

    namespace
    {
        // The monitor showing the most of window, and the display adapter
        // (GPU) it is plugged into.
        std::string monitorAndAdapter(HWND window)
        {
            MONITORINFOEXW info = {};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info))
            {
                return "(unknown)";
            }
            std::wstring adapter = L"(unknown adapter)";
            DISPLAY_DEVICEW device = {};
            device.cb = sizeof(device);
            for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &device, 0); ++i)
            {
                if (wcscmp(device.DeviceName, info.szDevice) == 0)
                {
                    adapter = device.DeviceString;
                    break;
                }
                device.cb = sizeof(device);
            }
            return ll_convert<std::string>(std::wstring(info.szDevice)) + " on " + ll_convert<std::string>(adapter);
        }
    }

    std::string OsWindow::describe() const
    {
        HWND window = mShared->handle.load();
        std::ostringstream text;
        if (!window)
        {
            text << "no OS window yet";
            return text.str();
        }
        RECT rect = {};
        GetWindowRect(window, &rect);
        const POINT middle = { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };

        DWORD cloaked = 0;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        DWORD affinity = 0;
        GetWindowDisplayAffinity(window, &affinity);
        // Windows 10 1607 and later (looked up, not linked).
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        using GetWindowDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
        using GetAwarenessFromDpiAwarenessContextFn = DPI_AWARENESS(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto dpi_for_window = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        auto window_awareness = (GetWindowDpiAwarenessContextFn)GetProcAddress(user32, "GetWindowDpiAwarenessContext");
        auto awareness_of = (GetAwarenessFromDpiAwarenessContextFn)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");

        // The window the mouse would hit there (another window covering it?),
        // and the colour the desktop shows there (DWM's composed picture).
        HWND on_top = WindowFromPoint(middle);
        COLORREF colour = CLR_INVALID;
        if (HDC screen = GetDC(NULL))
        {
            colour = GetPixel(screen, middle.x, middle.y);
            ReleaseDC(NULL, screen);
        }

        text << "OS window: visible " << (IsWindowVisible(window) ? 1 : 0) << ", minimized " << (IsIconic(window) ? 1 : 0)
             << ", style 0x" << std::hex << GetWindowLong(window, GWL_STYLE) << ", ex style 0x" << GetWindowLong(window, GWL_EXSTYLE)
             << std::dec << ", cloaked " << cloaked << ", display affinity " << affinity << ", DPI "
             << (dpi_for_window ? (S32)dpi_for_window(window) : -1) << ", DPI awareness "
             << (window_awareness && awareness_of ? (S32)awareness_of(window_awareness(window)) : -1) << ", rect "
             << rect.left << "," << rect.top << " " << (rect.right - rect.left) << "x" << (rect.bottom - rect.top)
             << ", monitor " << monitorAndAdapter(window) << " (main window: " << monitorAndAdapter(sMainWindow) << ")"
             << "; at its middle: window " << on_top << (on_top == window ? " (itself)" : " (another)") << ", screen RGB ";
        if (colour == CLR_INVALID)
        {
            text << "(unreadable)";
        }
        else
        {
            text << (U32)GetRValue(colour) << "," << (U32)GetGValue(colour) << "," << (U32)GetBValue(colour);
        }
        return text.str();
    }

    void OsWindow::setCornerRadius(S32 pixels)
    {
        pixels = llclamp(pixels, 0, MAX_CORNER_RADIUS);
        // The radius first, then the mode: a window made meanwhile on the
        // window thread (the mode, then the radius) is cut to it either way.
        if (sCornerRadius.exchange(pixels) == pixels || sCorners.load() != Corners::Region || !sMainWindow)
        {
            return;     // DWM's corners have their own radius
        }
        // Every window cut again where they live (the window thread owns them).
        plus_post_to_window_thread(sMainWindow, []()
        {
            std::vector<HWND> windows;
            {
                std::lock_guard<std::mutex> lock(sWindowsMutex);
                windows = sWindows;
            }
            for (HWND window : windows)
            {
                cutCorners(window);
            }
        });
    }

    void OsWindow::installHooks()
    {
        gPlusIsDetachedWindow = &isDetachedWindow;
        gPlusMapImePoints = &mapImePoints;
    }

    void OsWindow::beginDrawing(void* window, S32 height)
    {
        tDrawing.window = (HWND)window;
        tDrawing.height = height;
    }

    void OsWindow::endDrawing()
    {
        tDrawing = Drawing();
    }

    U32 OsWindow::forwardLoopsBroken()
    {
        return sForwardLoopsBroken.load();
    }

    bool OsWindow::hasMouseCapture()
    {
        return sCaptured.load() != NULL;
    }

    namespace
    {
        ScreenRect toScreenRect(const RECT& rect)
        {
            return { rect.left, rect.top, rect.right, rect.bottom };
        }

        bool monitorInfo(HMONITOR handle, Monitor& out)
        {
            MONITORINFOEXW info = {};
            info.cbSize = sizeof(info);
            if (!handle || !GetMonitorInfoW(handle, &info))
            {
                return false;
            }
            out.device = ll_convert<std::string>(std::wstring(info.szDevice));
            out.bounds = toScreenRect(info.rcMonitor);
            out.work = toScreenRect(info.rcWork);
            return true;
        }
    }

    ScreenRect OsWindow::otherMonitorWorkArea()
    {
        struct Search
        {
            HMONITOR main = NULL;
            RECT other = {};
            bool found = false;
        } search;
        HWND main_window = (HWND)gViewerWindow->getPlatformWindow();
        search.main = MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST);
        EnumDisplayMonitors(NULL, NULL, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
        {
            Search* search = (Search*)data;
            MONITORINFO info = { sizeof(info) };
            if (monitor != search->main && GetMonitorInfo(monitor, &info))
            {
                search->other = info.rcWork;
                search->found = true;
                return FALSE;   // the first one will do
            }
            return TRUE;
        }, (LPARAM)&search);

        if (search.found)
        {
            return toScreenRect(search.other);
        }
        return mainMonitor().work;
    }

    Monitor OsWindow::mainMonitor()
    {
        Monitor monitor;
        HWND main_window = (HWND)gViewerWindow->getPlatformWindow();
        monitorInfo(MonitorFromWindow(main_window, MONITOR_DEFAULTTOPRIMARY), monitor);
        return monitor;
    }

    Monitor OsWindow::monitorOf(const ScreenRect& rect)
    {
        const RECT win_rect = { rect.left, rect.top, rect.right, rect.bottom };
        Monitor monitor;
        if (!monitorInfo(MonitorFromRect(&win_rect, MONITOR_DEFAULTTONEAREST), monitor))
        {
            monitor = mainMonitor();
        }
        return monitor;
    }

    bool OsWindow::findMonitor(const std::string& device, Monitor& out)
    {
        struct Search
        {
            const std::string* device;
            Monitor* out;
            bool found = false;
        } search = { &device, &out };
        EnumDisplayMonitors(NULL, NULL, [](HMONITOR handle, HDC, LPRECT, LPARAM data) -> BOOL
        {
            Search* search = (Search*)data;
            Monitor monitor;
            if (monitorInfo(handle, monitor) && monitor.device == *search->device)
            {
                *search->out = monitor;
                search->found = true;
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&search);
        return search.found;
    }

    bool OsWindow::anyMouseButtonDown()
    {
        return anyButtonDown();
    }

    bool OsWindow::cursorOffMainWindow(S32 margin)
    {
        HWND main_window = gViewerWindow ? (HWND)gViewerWindow->getPlatformWindow() : NULL;
        POINT cursor = {};
        RECT frame = {};
        if (!main_window || !GetCursorPos(&cursor) || !GetWindowRect(main_window, &frame))
        {
            return false;
        }
        InflateRect(&frame, margin, margin);
        return !PtInRect(&frame, cursor);
    }
}

#else // !LL_WINDOWS: no detached windows

namespace plus::detach
{
    struct OsWindow::Shared
    {
    };

    OsWindow::OsWindow(const ScreenRect& rect, const std::wstring&, bool) : mRect(rect) {}
    OsWindow::~OsWindow() {}
    void* OsWindow::handle() const { return nullptr; }
    void OsWindow::setRect(const ScreenRect& rect) { mRect = rect; }
    std::string OsWindow::describe() const { return "no OS window"; }
    void OsWindow::setCornerRadius(S32) {}
    void OsWindow::installHooks() {}
    bool OsWindow::hasMouseCapture() { return false; }
    void OsWindow::beginDrawing(void*, S32) {}
    void OsWindow::endDrawing() {}
    ScreenRect OsWindow::otherMonitorWorkArea() { return {}; }
    Monitor OsWindow::mainMonitor() { return {}; }
    Monitor OsWindow::monitorOf(const ScreenRect&) { return {}; }
    bool OsWindow::findMonitor(const std::string&, Monitor&) { return false; }
    bool OsWindow::cursorOffMainWindow(S32) { return false; }
    bool OsWindow::anyMouseButtonDown() { return true; }
    U32 OsWindow::forwardLoopsBroken() { return 0; }
}

#endif

namespace plus::detach
{
    ScreenRect ScreenRect::fittedInto(const ScreenRect& area) const
    {
        const S32 w = llmin(width(), area.width());
        const S32 h = llmin(height(), area.height());
        ScreenRect fitted;
        fitted.left = llclamp(left, area.left, area.right - w);
        fitted.top = llclamp(top, area.top, area.bottom - h);
        fitted.right = fitted.left + w;
        fitted.bottom = fitted.top + h;
        return fitted;
    }

    ScreenRect ScreenRect::centredOn(const ScreenRect& area) const
    {
        const S32 w = llmin(width(), area.width());
        const S32 h = llmin(height(), area.height());
        ScreenRect centred;
        centred.left = area.left + (area.width() - w) / 2;
        centred.top = area.top + (area.height() - h) / 2;
        centred.right = centred.left + w;
        centred.bottom = centred.top + h;
        return centred;
    }
}
