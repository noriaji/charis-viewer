/**
 * @file photoscreen.cpp
 * @brief Photo screen: the frame (research/218 in the sl-viewer repository).
 *
 * What is here:
 *  - Entering and leaving. Going in hides the UI that is shown (the menu bar, the toolbars, the
 *    navigation bar, the mini location bar, the menu shortcut bar (plus/shortcutbar), the status
 *    bar, the floaters except the chat, and the floaters in windows of their own: plus/detach)
 *    and keeps, one by one, what it hid. Going out
 *    shows exactly those again, and puts the HUD attachments back as they were. It does not use
 *    LLFloaterView::hideAllFloaters() (its list is shared with View > Hide UI) nor the Hide UI
 *    switch; View > Hide UI is undone on the way in and, if it is pressed in here, undone again.
 *    The hiding is one object (UiScope) so that every way out runs the same restore: leave(),
 *    a teleport that ends in a log out, a disconnect, quitting.
 *  - The world's rectangle (worldRect(), from the one <plus> hook in
 *    LLViewerWindow::updateWorldViewRect()): in the detailed mode the biggest rectangle of the
 *    picture's shape in the middle of what the panels leave free; in the simple mode the window.
 *    The frame between the rectangle and the window edge is covered by plain views that take the
 *    mouse (the clicks cannot go through to the world there).
 *  - The buttons: the HUD attachments on / off, the 360 snapshot window, the switch between the
 *    two modes, the shutter and the exit, at the bottom right of the world's rectangle. These are
 *    the smallest places for them; the later branches (photoscreen-detail, photoscreen-simple) put
 *    their panels on overlay().
 *  - The exit button (panel_plus_photoscreen_exit.xml): a cross at the top right corner of the world's rectangle in both
 *    modes; it does what F12 does (leave()), a frame later like the key. It is off while a one-press photo is asked for
 *    (the picture's size is the world's rectangle at the moment it is taken).
 *  - The conversations (photoconv.cpp): listened to from the way in to the way out (conv::start(), conv::stop()).
 *
 * Speed: nothing is done while it is off but one pointer test in worldRect(). While on, a few
 * bool reads a frame; the rectangle and the views are laid out when the window or the mode
 * changes, not every frame; no allocation per frame.
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

#include "photoscreen.h"
#include "photoconv.h"
#include "photodetail.h"
#include "photodetail_layout.h"
#include "panel_simple.h"
#include "psui/psstyle.h"

#include "plus/plus.h"
#include "plus/focuslock/focuslock.h"
#include "plus/photomode/photoshot.h"
#include "plus/shortcutbar/shortcutbar.h"

#include <cmath>
#include <vector>

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfloater.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llnavigationbar.h"
#include "llpanel.h"
#include "llmoveview.h"
#include "llpaneltopinfobar.h"
#include "llrender2dutils.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "lltoolbarview.h"
#include "lluictrlfactory.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "pipeline.h"
#include "plus/rlv/rlvhooks.h"

extern LLFloaterView* gPlusDetachedFloaterView;

namespace plus::photoscreen
{
    namespace
    {
        constexpr char SETTING_SIMPLE[]  = "PlusPhotoScreenSimple";
        constexpr char SETTING_ASPECT[]  = "PlusPhotoShotAspect";
        constexpr char SETTING_HIDE_UI[] = "HideUIControls";

        // The frame of the detailed mode (RAIL_W .. FRAME_SCALE_MAX) is in the table of sizes,
        // photodetail_layout.h.
        using namespace plus::photoscreen::sizes;
        constexpr S32 BUTTON_MARGIN = 8;

        // the ground between the panels and around the world; the world's rounded corners are cut in the same colour
        // (research/221 0., psui/psstyle.h)
        const LLColor4 COVER_COLOR = plus::psui::style::GROUND;

        // The floaters that stay: the chat (the conversations window holds the nearby chat).
        bool staysShown(const LLFloater* floater)
        {
            const std::string& name = floater->getName();
            return name == "nearby_chat" || name == "im_container";
        }

        // What View > Hide UI does back (LLViewToggleUI::confirm()), without its question.
        void undoHideUiControls()
        {
            if (gSavedSettings.getBOOL(SETTING_HIDE_UI))
            {
                gViewerWindow->setUIVisibility(true);
                LLPanelStandStopFlying::getInstance()->setVisible(true);
                gSavedSettings.setBOOL(SETTING_HIDE_UI, false);
            }
        }

        // The sizes of the frame for a window w x h (false: the simple mode, or too small for a frame).
        bool frameMargins(S32 w, S32 h, Margins& m)
        {
            static LLCachedControl<bool> simple(gSavedSettings, SETTING_SIMPLE, false);
            if (simple)
            {
                return false;
            }
            const F32 s = llclamp(h / 1080.f, FRAME_SCALE_MIN, FRAME_SCALE_MAX);
            m.wide        = w >= NARROW_WIDTH;
            m.rail        = ll_round(RAIL_W * s);
            m.left_panel_full = ll_round(LEFT_PANEL_W * s);
            m.left_panel  = m.wide ? m.left_panel_full : 0;
            m.right_panel = m.wide ? ll_round(RIGHT_PANEL_W * s) : 0;
            m.bottom      = ll_round(BOTTOM_H * s);
            m.top         = ll_round(TOP_H * s);
            return w - m.rail - m.left_panel - m.right_panel >= MIN_WORLD && h - m.bottom - m.top >= MIN_WORLD;
        }

        // The world's rectangle in UI coordinates (origin bottom left).
        LLRect computeRect(S32 w, S32 h)
        {
            static LLCachedControl<S32> aspect(gSavedSettings, SETTING_ASPECT, 0);

            const LLRect window(0, h, w, 0);
            Margins m;
            if (!frameMargins(w, h, m))
            {
                return window;  // the simple mode, or the window is too small for a frame
            }
            const S32 left    = m.rail + m.left_panel;
            const S32 right   = m.right_panel;
            const S32 bottom  = m.bottom;
            const S32 avail_w = w - left - right;
            const S32 avail_h = h - bottom - m.top;
            const double ratio = plus::photomode::shotAspectRatio(aspect);  // 0: the free shape
            S32 world_w = avail_w;
            S32 world_h = avail_h;
            if (ratio > 0.0)
            {
                if ((double)avail_w / avail_h > ratio)
                {
                    world_w = (S32)std::lround(avail_h * ratio);
                }
                else
                {
                    world_h = (S32)std::lround(avail_w / ratio);
                }
                world_w = llclamp(world_w, 1, avail_w);
                world_h = llclamp(world_h, 1, avail_h);
            }
            const S32 x = left + (avail_w - world_w) / 2;
            const S32 y = bottom + (avail_h - world_h) / 2;
            return LLRect(x, y + world_h, x + world_w, y);
        }

        // A plain opaque view over one side of the frame. It takes the mouse (LLView's default for an
        // opaque view): a click or the wheel on it does not reach the world.
        class CoverView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit CoverView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override { gl_rect_2d(getLocalRect(), COVER_COLOR, true); }
        };

        // The view over the whole window that the covers, the buttons and (later) the panels are children of.
        class OverlayView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit OverlayView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;
        };

        // The UI that was shown when the photo screen went in, and what it hid. Its destructor shows
        // those again: every way out of the mode is the deletion of this object. Views are held by
        // handle: one that is gone by then (log out) is skipped.
        class UiScope
        {
        public:
            UiScope() : mHud(LLPipeline::sShowHUDAttachments)
            {
                undoHideUiControls();
                // The shortcut bar decides its own showing every idle (its setting, the UI shown), so it is
                // held off by its flag, not hidden here; let go, it shows again only if it would anyway.
                plus::shortcutbar::setSuppressed(true);
                hideSingles();
                hideFloaters(gFloaterView);
                hideFloaters(gPlusDetachedFloaterView);
            }

            ~UiScope() { restore(); }

            UiScope(const UiScope&)            = delete;
            UiScope& operator=(const UiScope&) = delete;

            // The bars and the menu: every frame, as the official code may show them again (the switch of
            // View > Hide UI, a setting that shows the navigation bar). What is not shown is left alone.
            void hideSingles()
            {
                hide(gMenuBarView);
                hide(LLNavigationBar::getInstance());
                hide(LLPanelTopInfoBar::getInstance());
                if (gStatusBar)
                {
                    hide(gStatusBar->getParent());  // the container of the status bar, as setUIVisibility()
                }
                if (gToolBarView)
                {
                    gToolBarView->setToolBarsVisible(false);    // as setUIVisibility(): the flag of the bars, the same every frame
                    mToolbars = true;
                }
                // The chiclet bar (conversations and the notification well's unread count) is a sibling of the
                // toolbars, so setToolBarsVisible() above does not reach it; hide it here, restored with the rest.
                if (gViewerWindow)
                {
                    hide(gViewerWindow->getChicletContainer());
                }
            }

            void restore()
            {
                // in the order the hiding was done
                for (LLHandle<LLView>& handle : mViews)
                {
                    if (LLView* view = handle.get())
                    {
                        view->setVisible(true);
                    }
                }
                mViews.clear();
                for (LLHandle<LLFloater>& handle : mFloaters)
                {
                    if (LLFloater* floater = handle.get())
                    {
                        floater->setVisible(true);
                    }
                }
                mFloaters.clear();
                if (mToolbars && gToolBarView)
                {
                    gToolBarView->setToolBarsVisible(true);
                }
                mToolbars = false;
                plus::shortcutbar::setSuppressed(false);
                // RLVa: a locked HUD that came while in the mode is not left hidden
                LLPipeline::sShowHUDAttachments = mHud || (plus::rlv::enabled() && plus::rlv::hasLockedHUD());
            }

        private:
            void hide(LLView* view)
            {
                if (view && view->getVisible())
                {
                    mViews.push_back(view->getHandle());
                    view->setVisible(false);
                }
            }

            // Once, at the entry: a floater the user opens in here (the 360 snapshot, a notification)
            // is theirs to see.
            void hideFloaters(LLFloaterView* view)
            {
                if (!view)
                {
                    return;
                }
                const LLView::child_list_t children = *view->getChildList();    // setVisible() may call back
                for (LLView* child : children)
                {
                    LLFloater* floater = dynamic_cast<LLFloater*>(child);
                    if (floater && floater->getVisible() && !staysShown(floater))
                    {
                        mFloaters.push_back(floater->getHandle());
                        floater->setVisible(false);
                    }
                }
            }

            std::vector<LLHandle<LLView>>    mViews;
            std::vector<LLHandle<LLFloater>> mFloaters;
            bool mToolbars = false;
            bool mHud;
        };

        // Not deleted at the end of the process on purpose (a static destructor after the UI is gone
        // would walk handles); leave() and the idle callback delete it.
        UiScope*          sScope = nullptr;
        LLHandle<LLView>  sOverlay;
        LLHandle<LLPanel> sButtons;
        LLHandle<LLPanel> sExit;            // the exit button's panel (top right of the world)
        LLHandle<LLView>  sExitButton;      // the button inside it
        LLHandle<LLView>  sBarExitButton;   // the old bar's Exit (sButtons: shown in the simple view and in a window too small for a frame)
        bool              sExitEnabled = true;
        bool              sTickRegistered = false;

        // what the views were last laid out for
        LLRect sLaidWorld;
        S32    sLaidW = -1, sLaidH = -1;
        bool   sLaidSimple = false;

        LLView* makeCover(LLView* parent, const char* name)
        {
            CoverView::Params params;
            params.name         = name;
            params.rect         = LLRect(0, 1, 1, 0);
            params.mouse_opaque = true;
            CoverView* cover = LLUICtrlFactory::create<CoverView>(params);
            if (cover)
            {
                parent->addChild(cover);
            }
            return cover;
        }

        void refreshModeButton()
        {
            LLPanel* buttons = sButtons.get();
            if (!buttons)
            {
                return;
            }
            LLButton* mode = buttons->findChild<LLButton>("btn_mode");
            if (mode)
            {
                mode->setLabel(buttons->getString(isSimple() ? "mode_to_detail" : "mode_to_simple"));
            }
        }

        void onShoot()
        {
            plus::photomode::takePhoto();
        }

        // The exit button's box: WORLD_INSET inside the world's top right corner.
        LLRect exitRect(const LLRect& world)
        {
            const S32 right = world.mRight - WORLD_INSET;
            const S32 top   = world.mTop - WORLD_INSET;
            return LLRect(right - EXIT_SIZE, top, right, top - EXIT_SIZE);
        }

        // Like F12 (keyPhotoScreenToggle): the windows and bars come back on the next idle, not inside the button's own
        // callback. A second press before then finds the mode off already.
        void onExit()
        {
            doOnIdleOneTime([]() {
                if (sScope)
                {
                    leave();
                }
            });
        }

        void setExitButtonsEnabled(bool enable)
        {
            for (const LLHandle<LLView>* handle : { &sExitButton, &sBarExitButton })
            {
                if (LLView* button = handle->get())
                {
                    button->setEnabled(enable);
                }
            }
        }

        // The button is off while a one-press photo is asked for: the picture takes the world's rectangle as it is when
        // the photo is taken (photoshot.cpp shotDims()), and a mode left by then would make a different picture.
        void refreshExitEnabled()
        {
            const plus::photomode::ShotState state = plus::photomode::shotState();
            const bool                       enable = state != plus::photomode::ShotState::Pending && state != plus::photomode::ShotState::Busy;
            if (enable != sExitEnabled)
            {
                sExitEnabled = enable;
                setExitButtonsEnabled(enable);
            }
        }

        // The key(s) of photo_screen_toggle in the key settings as they are now ("F12"), the first when there are several;
        // empty when none is bound. Read when the mode is entered, not every frame.
        std::string toggleKey()
        {
            const std::string keys = gViewerInput.getKeyBindingAsString(std::string(), "photo_screen_toggle");
            return keys.substr(0, keys.find(" | "));
        }

        void refreshExitTip()
        {
            LLPanel*   exit = sExit.get();
            LLView*    button = sExitButton.get();
            if (!exit || !button)
            {
                return;
            }
            const std::string key = toggleKey();
            LLStringUtil::format_map_t args;
            args["[KEY]"] = key;
            button->setToolTip(exit->getString(key.empty() ? "tip_nokey" : "tip_key", args));
        }

        // The overlay: made at the first entry, kept (hidden) after it, so no view is deleted from
        // inside one of its own button's callbacks.
        LLView* ensureOverlay()
        {
            if (LLView* existing = sOverlay.get())
            {
                return existing;
            }
            // at the back: under the floaters that come up in here
            OverlayView* overlay = psui::attachWorldOverlay<OverlayView>(sOverlay, "plus_photoscreen");
            if (!overlay)
            {
                return nullptr;
            }
            makeCover(overlay, "cover_top");
            makeCover(overlay, "cover_bottom");
            makeCover(overlay, "cover_left");
            makeCover(overlay, "cover_right");

            LLPanel* buttons = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(
                "panel_plus_photoscreen_buttons.xml", overlay, LLPanel::child_registry_t::instance());
            if (buttons)
            {
                sButtons = buttons->getHandle();
                buttons->getChild<LLButton>("btn_hud")->setCommitCallback([](LLUICtrl*, const LLSD&) {
                    // RLVa: a locked HUD cannot be hidden (research/248 W10, 3.4)
                    if (LLPipeline::sShowHUDAttachments && plus::rlv::enabled() && plus::rlv::hasLockedHUD())
                        return;
                    LLPipeline::sShowHUDAttachments = !LLPipeline::sShowHUDAttachments;
                });
                buttons->getChild<LLButton>("btn_360")->setCommitCallback([](LLUICtrl*, const LLSD&) {
                    LLFloaterReg::showInstance("360capture");
                });
                buttons->getChild<LLButton>("btn_mode")->setCommitCallback([](LLUICtrl*, const LLSD&) {
                    setSimple(!isSimple());
                });
                buttons->getChild<LLButton>("btn_shoot")->setCommitCallback([](LLUICtrl*, const LLSD&) { onShoot(); });
                // the same exit as the cross at the top right (and off while a photo is asked for), not leave() directly
                LLButton* bar_exit = buttons->getChild<LLButton>("btn_exit");
                bar_exit->setCommitCallback([](LLUICtrl*, const LLSD&) { onExit(); });
                sBarExitButton = bar_exit->getHandle();
                bar_exit->setEnabled(sExitEnabled);
            }
            LLPanel* exit = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(
                "panel_plus_photoscreen_exit.xml", overlay, LLPanel::child_registry_t::instance());
            if (exit)
            {
                sExit = exit->getHandle();
                if (LLUICtrl* button = exit->findChild<LLUICtrl>("b_exit"))
                {
                    sExitButton = button->getHandle();
                    button->setCommitCallback([](LLUICtrl*, const LLSD&) { onExit(); });
                    button->setEnabled(sExitEnabled);
                }
            }
            return overlay;
        }

        // The sides' covers and the buttons follow the world's rectangle.
        void layout(S32 w, S32 h)
        {
            LLView* overlay = sOverlay.get();
            if (!overlay)
            {
                return;
            }
            const LLRect world = computeRect(w, h);
            const struct { const char* name; LLRect rect; } sides[] = {
                { "cover_top",    LLRect(0, h, w, world.mTop) },
                { "cover_bottom", LLRect(0, world.mBottom, w, 0) },
                { "cover_left",   LLRect(0, world.mTop, world.mLeft, world.mBottom) },
                { "cover_right",  LLRect(world.mRight, world.mTop, w, world.mBottom) },
            };
            for (const auto& side : sides)
            {
                if (LLView* cover = overlay->findChildView(side.name, false))
                {
                    const bool any = side.rect.getWidth() > 0 && side.rect.getHeight() > 0;
                    cover->setVisible(any);
                    if (any)
                    {
                        cover->setShape(side.rect);
                    }
                }
            }
            Margins framed;
            const bool in_frame = frameMargins(w, h, framed);
            if (LLPanel* buttons = sButtons.get())
            {
                buttons->setVisible(!in_frame);     // the detailed mode has its own buttons (photodetail.cpp)
                const LLRect size = buttons->getRect();
                buttons->setOrigin(world.mRight - size.getWidth() - BUTTON_MARGIN, world.mBottom + BUTTON_MARGIN);
            }
            detail::place(overlay, world, w, h, !in_frame);
            if (LLPanel* exit = sExit.get())
            {
                // after the panels are placed, and in front of them (and of the world's rounded corners)
                exit->setShape(exitRect(world));
                overlay->sendChildToFront(exit);
            }
            sLaidWorld  = world;
            sLaidW      = w;
            sLaidH      = h;
            sLaidSimple = isSimple();
            refreshModeButton();
            simple::onLayout(overlay, w, h, sLaidSimple);   // the simple view (panel_simple.cpp)
        }

        bool mustLeave()
        {
            return LLApp::isExiting() || gDisconnected || LLStartUp::getStartupState() < STATE_STARTED || !gViewerWindow;
        }

        // Every frame while on (an idle callback; true removes it).
        bool tick()
        {
            if (!sScope)
            {
                sTickRegistered = false;
                return true;
            }
            if (mustLeave())
            {
                leave();
                sTickRegistered = false;
                return true;
            }
            static LLCachedControl<bool> hide_ui(gSavedSettings, SETTING_HIDE_UI, false);
            if (hide_ui)
            {
                undoHideUiControls();   // View > Hide UI is not for in here
            }
            sScope->hideSingles();
            refreshExitEnabled();

            // In mouselook the world is the whole window (the official code does not ask for our rectangle):
            // the frame and the buttons would be on it.
            LLView* overlay = sOverlay.get();
            if (overlay)
            {
                const bool show = !gAgentCamera.cameraMouselook();
                if (overlay->getVisible() != show)
                {
                    overlay->setVisible(show);
                }
                if (show)
                {
                    const S32 w = gViewerWindow->getWindowWidthScaled();
                    const S32 h = gViewerWindow->getWindowHeightScaled();
                    if (w != sLaidW || h != sLaidH || isSimple() != sLaidSimple || computeRect(w, h) != sLaidWorld)
                    {
                        layout(w, h);
                    }
                }
            }
            return false;
        }

        void init()
        {
            plus::declareUIImage("Plus_PhotoScreen_Icon", "plus/photoscreen_18.png");  // the toolbar button (commands_plus.xml)
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PhotoScreen.Toggle",
                                                                     [](LLUICtrl*, const LLSD&) { toggle(); });
            LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.PhotoScreen.IsOn",
                                                                     [](LLUICtrl*, const LLSD&) { return isActive(); });
        }

        plus::FeatureRegistration sRegistration({ "photoscreen", &init });
    }

    bool isActive()
    {
        return sScope != nullptr;
    }

    bool isSimple()
    {
        static LLCachedControl<bool> simple(gSavedSettings, SETTING_SIMPLE, false);
        return simple;
    }

    void setSimple(bool simple)
    {
        gSavedSettings.setBOOL(SETTING_SIMPLE, simple);
        refreshModeButton();    // the world's rectangle follows in the next tick / updateWorldViewRect()
    }

    bool canEnter()
    {
        return !mustLeave();
    }

    bool enter()
    {
        if (sScope)
        {
            return true;
        }
        if (!canEnter())
        {
            return false;
        }
        if (!ensureOverlay())
        {
            return false;
        }
        sScope = new UiScope();     // hides; deleting it shows again
        refreshExitTip();           // the key may have been changed in the key settings
        conv::start();              // the conversations tab's list and badge (photoconv.cpp)
        LLView* overlay = sOverlay.get();
        overlay->setVisible(!gAgentCamera.cameraMouselook());
        layout(gViewerWindow->getWindowWidthScaled(), gViewerWindow->getWindowHeightScaled());
        if (!sTickRegistered)
        {
            sTickRegistered = true;
            doOnIdleRepeating(&tick);
        }
        return true;
    }

    void leave()
    {
        UiScope* scope = sScope;
        sScope         = nullptr;   // first: worldRect() and isActive() are off while the views come back
        if (LLView* overlay = sOverlay.get())
        {
            overlay->setVisible(false);
        }
        delete scope;
        conv::stop();               // after the toolbars are back: it may flash their conversations button
        plus::focuslock::setPicking(false); // the world's clicks are the world's again (the point stays)
    }

    void toggle()
    {
        if (sScope)
        {
            leave();
        }
        else
        {
            enter();
        }
    }

    bool worldRect(S32 ui_w, S32 ui_h, LLRect& rect)
    {
        if (!sScope)
        {
            return false;
        }
        rect = computeRect(ui_w, ui_h);
        return true;
    }

    LLView* overlay()
    {
        return sScope ? sOverlay.get() : nullptr;
    }

    bool margins(S32 ui_w, S32 ui_h, Margins& out)
    {
        return frameMargins(ui_w, ui_h, out);
    }

    S32 chatConsoleFloor()
    {
        return sScope ? simple::chatLineTop() : 0;
    }

    S32 chatConsoleCeiling()
    {
        return sScope ? simple::chatConsoleTop() : 0;
    }

    S32 worldTopTaken(S32 left, S32 right)
    {
        LLRect shown;
        if (!shownWorldRect(shown))
        {
            return 0;
        }
        S32 taken = detail::topTaken(left, right);
        const LLRect button = exitRect(shown);      // in both modes
        if (left < button.mRight && right > button.mLeft)
        {
            taken = llmax(taken, shown.mTop - button.mBottom);
        }
        return taken;
    }

    bool shownWorldRect(LLRect& rect)
    {
        const LLView* view = sScope ? sOverlay.get() : nullptr;
        if (!view || !view->getVisible() || sLaidW < 0)
        {
            return false;
        }
        rect = sLaidWorld;
        return true;
    }

    void sendChatLine(LLLineEditor* editor)
    {
        if (!editor)
        {
            return;
        }
        std::string text = editor->getText();
        LLStringUtil::trim(text);
        editor->setText(LLStringExplicit(""));
        if (!text.empty())
        {
            LLFloaterIMNearbyChat::sendChatFromViewer(text, CHAT_TYPE_NORMAL, gSavedSettings.getBOOL("PlayChatAnim"));
        }
        gAgent.stopTyping();
        // The line keeps the keyboard for the next one, as the official chat bar does; Esc (LLPanel::handleKeyHere())
        // or a click on the world gives it back to the camera and the photo keys. CloseChatOnReturn: let go after each.
        static LLCachedControl<bool> close_on_return(gSavedSettings, "CloseChatOnReturn", false);
        if (close_on_return)
        {
            editor->setFocus(false);
        }
    }
}
