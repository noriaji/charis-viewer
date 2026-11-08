/**
 * @file detachview.h
 * @brief The floater view of the detached floaters (their place in the UI,
 *        outside the main window), and drawing UI into a detached window.
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

#ifndef PLUS_DETACH_DETACHVIEW_H
#define PLUS_DETACH_DETACHVIEW_H

#include "llfloater.h"

#include <functional>

// The UI's coordinates go on past the main window over the whole desktop
// (research/87 4 (a)): this view, a child of the root view, covers the
// desktop in them, and the detached floaters are its children, each where its
// window is. So the main window's input handling reaches them as it reaches
// any floater (hover, clicks, drags, the wheel, mouse capture), and a
// floater's drag handle and resize handles move it over the desktop.
//
// gFloaterView only clicks through to floaters inside the main window, and
// keeps its floaters there; this is a floater view of its own for that
// (official code finds it through gPlusDetachedFloaterView, the <plus> hooks
// in llfloater.cpp). It never draws in the main window: each detached window
// draws its floaters itself (plus::detach::WindowSpace).
class LLPlusDetachedFloaterView : public LLFloaterView
{
public:
    struct Params : public LLInitParam::Block<Params, LLFloaterView::Params>
    {
    };

    // Makes the view, behind the root view's other children (see
    // plus::detach::installHitTest()).
    static LLPlusDetachedFloaterView* create();
    ~LLPlusDetachedFloaterView() override;

    void draw() override;

    // Covers rect (root view coordinates) from now on; the floaters keep their
    // place on the screen.
    void setScreenRect(const LLRect& rect);

protected:
    explicit LLPlusDetachedFloaterView(const Params& p);
    friend class LLUICtrlFactory;
};

namespace plus::detach
{
    // Mouse input on a detached window reaches the popups shown there (menus,
    // combo lists, tooltips: they belong to main_view, whose views only
    // contain points in the main window) and then the detached floaters, and
    // never the main window's own UI under it (when the windows overlap).
    // Installs LLView's hit test (LLView::TemporaryDrilldownFunc, never
    // removed); on_window: whether a UI screen point is on a detached window.
    // Once, when view is made.
    void installHitTest(LLPlusDetachedFloaterView* view, std::function<bool(S32 x, S32 y)> on_window);

    // Draws UI into a detached window's render target (bound, its 2D
    // projection set up by gl_state_for_2d()) while it lasts: the UI's origin
    // moves to the window's bottom-left corner, so every view on the window
    // draws there with its own place, children and clipping (the root view's
    // rect, LLView::sDirtyRect and the mouse position follow; see the .cpp).
    // Main thread, in render_ui().
    class WindowSpace
    {
    public:
        // window_ui: the window in UI screen coordinates; scale: pixels per
        // UI unit; os_window, height: the window (HWND) and its height in
        // pixels (for the IME's place, OsWindow::beginDrawing()).
        WindowSpace(const LLRect& window_ui, const LLVector2& scale, void* os_window, S32 height);
        ~WindowSpace();

        WindowSpace(const WindowSpace&) = delete;
        WindowSpace& operator=(const WindowSpace&) = delete;

        // A view, where it is on the screen, if it is visible and on the window.
        void draw(LLView* view);
        // What main_view shows over everything on the window: popups (combo
        // lists), menus, tooltips, and the "top" control.
        void drawPopups();

    private:
        LLView* mRoot;
        LLRect mRootRect;
        LLRect mDirtyRect;
        bool mWasDrawing;
        LLRect mWindowLocal;    // the window, from its own corner
    };
}

#endif // PLUS_DETACH_DETACHVIEW_H
