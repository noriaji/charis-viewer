/**
 * @file detachview.cpp
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

#include "llviewerprecompiledheaders.h"

#include "detachview.h"

#include "detach.h"

#include "detachwin32.h"

#include "llfocusmgr.h"
#include "llmenugl.h"
#include "llpopupview.h"
#include "llrender.h"
#include "llrootview.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewerwindow.h"

// llfloater.cpp, the <plus> hooks: this view (adjustToFitScreen leaves its
// floaters to plus/detach, gFloaterView->getParentFloater() looks in it too).
extern LLFloaterView* gPlusDetachedFloaterView;
// llui.cpp, the <plus> hook: added to the mouse's screen position.
extern S32 gPlusMouseOffsetX;
extern S32 gPlusMouseOffsetY;

LLPlusDetachedFloaterView* LLPlusDetachedFloaterView::create()
{
    Params p;
    p.name("plus_detached_floater_view");
    p.follows.flags(FOLLOWS_NONE);      // the main window's size does not move it
    p.mouse_opaque(false);              // over the main window it lets everything through
    p.tab_stop(false);
    p.visible(true);
    LLPlusDetachedFloaterView* view = LLUICtrlFactory::create<LLPlusDetachedFloaterView>(p);
    // Behind main_view: on a detached window, the popups of main_view's menu
    // and popup holders come first (installHitTest()).
    gViewerWindow->getRootView()->addChildInBack(view);
    gPlusDetachedFloaterView = view;
    return view;
}

LLPlusDetachedFloaterView::LLPlusDetachedFloaterView(const Params& p)
    : LLFloaterView(p)
{
}

LLPlusDetachedFloaterView::~LLPlusDetachedFloaterView()
{
    // The root view's children go at shutdown in their order: gFloaterView's
    // hooks (closeAllChildren(), getParentFloater(), ...) no longer look here.
    if (gPlusDetachedFloaterView == this)
    {
        gPlusDetachedFloaterView = NULL;
    }
}

void LLPlusDetachedFloaterView::draw()
{
    // Nothing in the main window (LLFloaterView::draw() would also keep the
    // floaters inside it): each detached window draws its own floaters.
}

void LLPlusDetachedFloaterView::setScreenRect(const LLRect& rect)
{
    const LLRect old_rect = getRect();
    if (rect == old_rect)
    {
        return;
    }
    const S32 dx = old_rect.mLeft - rect.mLeft;
    const S32 dy = old_rect.mBottom - rect.mBottom;
    setRect(rect);
    for (LLView* child : *getChildList())
    {
        child->setOrigin(child->getRect().mLeft + dx, child->getRect().mBottom + dy);
    }
}

namespace
{
    // installHitTest(): the views it decides for (compared, not used), and
    // the popup holder WindowSpace draws from.
    struct HitTest
    {
        const LLView* root = nullptr;
        const LLView* main_view = nullptr;
        const LLView* detached_view = nullptr;
        LLPopupView* popup_holder = nullptr;
        std::function<bool(S32, S32)> on_window;
    };
    HitTest sHitTest;

    // LLView's hit test (LLView::sDrilldown): pointInView(), except for the
    // root view's and main_view's own children at a point on a detached
    // window. main_view's views cover the main window only, so main_view and
    // its popup layers (tooltips, menus, popups) are let in there; main_view's
    // other children (the main window's floaters, toolbars, ...) and the root
    // view's other children never get such a point.
    bool hitTest(const LLView* view, S32 x, S32 y)
    {
        const bool inside = view->pointInView(x, y);
        const LLView* parent = view->getParent();
        if (!parent || (parent != sHitTest.root && parent != sHitTest.main_view))
        {
            return inside;
        }
        S32 screen_x = 0;
        S32 screen_y = 0;
        view->localPointToScreen(x, y, &screen_x, &screen_y);
        if (!sHitTest.on_window(screen_x, screen_y))
        {
            return inside;
        }
        if (parent == sHitTest.root)
        {
            return view == sHitTest.main_view || (view == sHitTest.detached_view && inside);
        }
        return view == LLMenuGL::sMenuContainer || view == gToolTipView || view == sHitTest.popup_holder;
    }
}

namespace plus::detach
{
    void installHitTest(LLPlusDetachedFloaterView* view, std::function<bool(S32 x, S32 y)> on_window)
    {
        LLView* root = gViewerWindow->getRootView();
        LLView* main_view = root->findChild<LLView>("main_view", false);
        sHitTest.root = root;
        sHitTest.main_view = main_view;
        sHitTest.detached_view = view;
        sHitTest.popup_holder = main_view ? main_view->findChild<LLPopupView>("popup_holder", false) : nullptr;
        sHitTest.on_window = std::move(on_window);
        if (!main_view)
        {
            LL_WARNS("Detach") << "No main_view: menus and popups on detached windows will not get the mouse" << LL_ENDL;
        }
        // For the rest of the session (a scoped object made to last: the
        // function stays in place and falls back to pointInView()).
        static LLView::TemporaryDrilldownFunc* installed = new LLView::TemporaryDrilldownFunc(&hitTest);
        (void)installed;
    }

    bool hoverContains(const LLView* view, S32 x, S32 y)
    {
        const bool inside = view->calcScreenBoundingRect().pointInRect(x, y);
        const LLView* parent = view->getParent();
        const bool top = view == sHitTest.root || view == sHitTest.main_view || parent == sHitTest.root ||
                         parent == sHitTest.main_view;
        // Only the top of the tree is decided here; a point on no detached
        // window (or none installed yet) is the official test.
        if (!top || !sHitTest.root || !sHitTest.on_window || !sHitTest.on_window(x, y))
        {
            return inside;
        }
        if (view == sHitTest.root || view == sHitTest.main_view)
        {
            return true;    // they cover the main window only, and lead to the views on the window
        }
        if (parent == sHitTest.root)
        {
            return view == sHitTest.detached_view && inside;
        }
        return view == LLMenuGL::sMenuContainer || view == gToolTipView || view == sHitTest.popup_holder;
    }

    // The UI's origin moves to the window's corner for the drawing, so that
    // every view on the window, wherever it sits in the tree, draws there with
    // its own place:
    //   - the root view's origin moves (setRect() moves no child and reshapes
    //     nothing): screen rects are then measured from the window's corner,
    //     and the root view's local rect is the window's, which with
    //     LLView::sDirtyRect is what LLView::drawChildren() keeps;
    //   - the mouse's screen position (LLUI, the <plus> hook in llui.cpp)
    //     moves with it, for the views that look at the mouse while drawn
    //     (pressed buttons, scrollbar glow);
    //   - the IME's place, which text fields set while drawn, is taken from
    //     this window (OsWindow::beginDrawing()).
    // All of it is put back when the drawing ends.
    WindowSpace::WindowSpace(const LLRect& window_ui, const LLVector2& scale, void* os_window, S32 height)
        : mRoot(gViewerWindow->getRootView()),
          mRootRect(mRoot->getRect()),
          mDirtyRect(LLView::sDirtyRect),
          mWasDrawing(LLView::sIsDrawing),
          mWindowLocal(0, window_ui.getHeight(), window_ui.getWidth(), 0)
    {
        mRoot->setRect(LLRect(-window_ui.mLeft, window_ui.getHeight() - window_ui.mBottom,
                              window_ui.getWidth() - window_ui.mLeft, -window_ui.mBottom));
        LLView::sDirtyRect = mWindowLocal;
        LLView::sIsDrawing = true;
        gPlusMouseOffsetX = -window_ui.mLeft;
        gPlusMouseOffsetY = -window_ui.mBottom;
        OsWindow::beginDrawing(os_window, height);

        // As LLViewerWindow::draw() sets up the UI: UI units to pixels.
        gGL.pushMatrix();
        LLUI::pushMatrix();
        LLUI::loadIdentity();
        gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);
    }

    WindowSpace::~WindowSpace()
    {
        LLUI::popMatrix();
        gGL.popMatrix();

        OsWindow::endDrawing();
        gPlusMouseOffsetX = 0;
        gPlusMouseOffsetY = 0;
        mRoot->setRect(mRootRect);
        LLView::sDirtyRect = mDirtyRect;
        LLView::sIsDrawing = mWasDrawing;
    }

    void WindowSpace::draw(LLView* view)
    {
        if (!view || !view->getVisible() || !view->calcScreenBoundingRect().overlaps(mWindowLocal))
        {
            return;
        }
        S32 x = 0;
        S32 y = 0;
        view->localPointToScreen(0, 0, &x, &y);
        LLUI::pushMatrix();
        LLUI::translate((F32)x, (F32)y);
        view->draw();
        LLUI::popMatrix();
    }

    void WindowSpace::drawPopups()
    {
        // In main_view's order, back to front (LLPopupView::draw(): the most
        // recent popup on top), then the top control (LLViewerWindow::draw()).
        if (LLPopupView* popups = sHitTest.popup_holder)
        {
            const LLPopupView::popup_list_t list = popups->getCurrentPopups();
            for (auto it = list.rbegin(); it != list.rend(); ++it)
            {
                draw(it->get());
            }
        }
        // The holders cover the main window; their children are kept or not
        // by LLView::drawChildren(), as they are in the main window.
        auto draw_holder = [](LLView* holder)
        {
            if (holder && holder->getVisible())
            {
                S32 x = 0;
                S32 y = 0;
                holder->localPointToScreen(0, 0, &x, &y);
                LLUI::pushMatrix();
                LLUI::translate((F32)x, (F32)y);
                holder->draw();
                LLUI::popMatrix();
            }
        };
        draw_holder(LLMenuGL::sMenuContainer);
        draw_holder(gToolTipView);
        draw(gFocusMgr.getTopCtrl());
    }
}
