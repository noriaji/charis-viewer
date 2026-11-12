/**
 * @file fpscorner.cpp
 * @brief FPS display ("87 FPS"): in the status bar right of the volume button (the default), or in a
 *        corner of the screen on a dim box; on or off from Preferences > Charis.
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

// Charis's own. Firestorm shows the FPS in its status bar right of the volume button (FSStatusBarShowFPS,
// a text box in its panel_status_bar.xml and LLStatusBar moving its right-hand group to make room); the
// status bar place here looks the same, but no code of it is used and no official file is touched.
//
// The number is the one of the performance floater (LLFloaterPerformance::setFPSText()): the median of
// the FPS stat over the last 50 loops, as the official floater reads it, except while UI-only frames
// (plus/uiframe) run among those loops: they count there as 0 FPS and the median falls to 0, so then
// it is the world's frames over the last second (plus::uiframe::worldFps()).
// It is read again every UPDATE_SECONDS, not every frame, so it does not flicker;
// the text, its width and its glyphs (LLFontVertexBuffer) are made only when the number changed, a frame
// draws the cached glyphs (and, in a corner, one quad).
//
// In the status bar (Position::StatusBar, the default): a view of its own added to the official status
// bar, right of the volume (speaker) button. To make room, updateStatusBar() moves the bar's right-hand
// group (menu search box, L$ balance, clock, presets icons, media and volume buttons) left by the
// number's width; the official code keeps them there (they follow the bar's right edge, and the balance
// and search box are laid out from the balance's right edge). The network graphs at the far right stay.
// When the moved group would reach the menus (a narrow window), the number is not shown and the group
// stays where the official layout has it: the number never pushes anything over the menus.
//
// In a corner: a view of its own at the back of the window's root view, beside the official HUD view
// (gHUDView: the tracking arrows, the media controls, also sent to the back of the root view) and the
// photo tools' guide. That is above the 3D picture and under every floater, toolbar, menu, toast and
// tooltip, and it takes no mouse: a window moved over the corner covers the number, the number never
// covers a window. Within that, the box stays in the area floaters snap to (between the toolbars,
// under the menu bar, the status bar and the navigation bar) and, in a top corner, under the chiclet
// bar (the notification and conversation icons at the top right) and the mini location bar.
//
// Not shown: before the agent is in the world (the login screen), while a snapshot renders (with the
// snapshot window's "show interface" too: plus::presets::snapshotInProgress(), as the photo guide; in the
// status bar the room stays, empty, like the L$ balance's without RenderBalanceInSnapshot), and with the
// UI hidden (Ctrl+Alt+F1: the root view is not drawn at all). Both views are in the main window only, so
// detached windows (plus/detach) never show them. In mouselook the status bar hides its own items and
// background but the number stays (with a shadow, since it is over the world then).

#include "llviewerprecompiledheaders.h"

#include "fpscornermath.h"

#include "plus.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/presets/presets.h"
#include "plus/uiframe/uiframe.h"

#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llfontvertexbuffer.h"
#include "llframetimer.h"
#include "llmenugl.h"
#include "llrender2dutils.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "lltracerecording.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerstats.h"
#include "llviewerwindow.h"

#include <vector>

namespace plus::fpscorner
{
    namespace
    {
        constexpr char SETTING_ON[]       = "PlusFpsCornerOn";
        constexpr char SETTING_POSITION[] = "PlusFpsCornerPosition";

        constexpr F32 UPDATE_SECONDS = 0.5f;
        constexpr S32 NUM_PERIODS    = 50;        // as the performance floater
        // The room is at least this wide: it does not jump with each number. Four digits, as shownFps()
        // goes up to 9999 (a light scene, a UI-only frame): the status bar's room is this wide only.
        constexpr char WIDEST[]      = "0000 FPS";

        // In a corner.
        constexpr S32 MARGIN = 6;  // from the edges of the area
        constexpr S32 PAD_X  = 6;  // text to the box's sides
        constexpr S32 PAD_Y  = 2;
        const LLColor4 BOX_COLOR(0.f, 0.f, 0.f, 0.45f);
        const LLColor4 TEXT_COLOR(1.f, 1.f, 1.f, 0.95f);

        // Views of the official UI the box keeps clear of, found by name once (LLView::findChildView).
        constexpr char SNAP_REGION[]  = "floater_snap_region";   // panel_toolbar_view.xml
        constexpr char CHICLET_BAR[]  = "chiclet_container";     // panel_toolbar_view.xml
        constexpr char TOP_INFO_BAR[] = "topinfo_bar_container"; // main_view.xml
        constexpr char SHORTCUT_BAR[] = "shortcut_bar_container"; // main_view.xml: plus/shortcutbar, over the world

        // In the status bar.
        constexpr S32 STATUS_GAP      = 4;              // volume button to the number (Firestorm's left_pad)
        constexpr S32 STATUS_MENU_PAD = 8;              // kept free between the menus and the moved group
        constexpr char STATUS_TEXT_COLOR[] = "TimeTextColor"; // colors.xml: the status bar's clock

        // The status bar's children (panel_status_bar.xml) that are moved left together to make room.
        constexpr char STATUS_SEARCH[]  = "menu_search_panel";
        constexpr char STATUS_BALANCE[] = "balance_bg";
        constexpr char STATUS_VOLUME[]  = "volume_btn";
        constexpr const char* STATUS_MOVED[] = { STATUS_SEARCH, STATUS_BALANCE, "TimeText", "presets_icon_camera",
                                                 "presets_icon_graphic", "media_toggle_btn", STATUS_VOLUME };

        bool displayOn()
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, true);
            return on;
        }

        Position currentPosition()
        {
            static LLCachedControl<S32> position(gSavedSettings, SETTING_POSITION, (S32)DEFAULT_POSITION);
            return positionFrom(position);
        }

        bool inWorld()
        {
            return LLStartUp::getStartupState() >= STATE_STARTED;
        }

        // The number and its text, for one of the views.
        class FpsText
        {
        public:
            // Read again at once when shown again.
            void forget() { mHaveText = false; }

            // The number again every UPDATE_SECONDS; the text, its width and its glyphs again only when the
            // number (or the font) changed.
            void update(const LLFontGL* font)
            {
                if (mHaveText && font == mFont && mUpdateTimer.getElapsedTimeF32() < UPDATE_SECONDS)
                {
                    return;
                }
                mUpdateTimer.reset();
                const F64 stat_fps = LLTrace::get_frame_recording().getPeriodMedianPerSec(LLStatViewer::FPS, NUM_PERIODS);
                const S32 fps = shownFps(plus::uiframe::worldFps(stat_fps));
                if (mHaveText && fps == mShownFps && font == mFont)
                {
                    return;
                }
                mHaveText = true;
                mShownFps = fps;
                mFont     = font;
                mText     = utf8str_to_wstring(llformat("%d FPS", fps));
                mBuffer.reset();
                mWidth = font->getWidth(mText.c_str());
            }

            S32 width() const { return mWidth; }

            // Whole pixels: a half-pixel origin would blur the glyphs. After update().
            void render(S32 x, S32 y, const LLColor4& color, LLFontGL::HAlign halign, LLFontGL::ShadowType shadow)
            {
                mBuffer.render(mFont, mText, 0, (F32)x, (F32)y, color, halign, LLFontGL::VCENTER, LLFontGL::NORMAL, shadow);
            }

        private:
            LLFrameTimer       mUpdateTimer;
            LLWString          mText;
            LLFontVertexBuffer mBuffer;
            const LLFontGL*    mFont     = nullptr;
            S32                mShownFps = -1;
            S32                mWidth    = 0;
            bool               mHaveText = false;
        };

        //---------------------------------------------------------------------------------------------
        // In a corner

        class FpsCornerView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit FpsCornerView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void findOfficialViews(LLView* root)
            {
                if (LLView* view = root->findChildView(SNAP_REGION, true))
                {
                    mSnapRegion = view->getHandle();
                }
                if (LLView* view = root->findChildView(CHICLET_BAR, true))
                {
                    mChicletBar = view->getHandle();
                }
                if (LLView* view = root->findChildView(TOP_INFO_BAR, true))
                {
                    mTopInfoBar = view->getHandle();
                }
                if (LLView* view = root->findChildView(SHORTCUT_BAR, true))
                {
                    mShortcutBar = view->getHandle();
                }
            }

            void draw() override
            {
                const Position position = currentPosition();
                if (!displayOn() || !isCorner(position) || plus::presets::snapshotInProgress() || !inWorld()
                    || gAgentCamera.cameraMouselook()) // first person: the UI is hidden, as the official one does
                {
                    mText.forget();
                    return;
                }
                const LLFontGL* font = LLFontGL::getFontSansSerif();
                if (!font)
                {
                    return;
                }
                mText.update(font);
                if (font != mWidestFont)
                {
                    mWidestFont  = font;
                    mWidestWidth = font->getWidth(std::string(WIDEST));
                }

                const S32 width = llmax(mText.width(), mWidestWidth) + 2 * PAD_X;
                Box box;
                if (!place(area(position), width, font->getLineHeight() + 2 * PAD_Y, position, MARGIN, box))
                {
                    return;
                }
                gl_rect_2d(box.left, box.top, box.right, box.bottom, BOX_COLOR);
                mText.render((box.left + box.right) / 2, (box.top + box.bottom) / 2, TEXT_COLOR, LLFontGL::HCENTER,
                             LLFontGL::DROP_SHADOW_SOFT);
            }

        private:
            // Where the box may go, in this view's coordinates: the world view, within the floaters'
            // snap region (between the toolbars) and, at the top, under the chiclet, top info and shortcut bars.
            Box area(Position position) const
            {
                const LLRect local = isTop(position)
                    ? psui::worldChipArea(*this, mSnapRegion.get(), { mChicletBar.get(), mTopInfoBar.get(), mShortcutBar.get() })
                    : psui::worldChipArea(*this, mSnapRegion.get());
                Box box;
                box.left   = local.mLeft;
                box.top    = local.mTop;
                box.right  = local.mRight;
                box.bottom = local.mBottom;
                return box;
            }

            LLHandle<LLView> mSnapRegion;
            LLHandle<LLView> mChicletBar;
            LLHandle<LLView> mTopInfoBar;
            LLHandle<LLView> mShortcutBar;
            FpsText          mText;
            const LLFontGL*  mWidestFont  = nullptr; // mWidestWidth is for this font (as the status bar's width)
            S32              mWidestWidth = 0;       // the room of the widest number (WIDEST)
        };

        LLHandle<LLView> sCornerView;

        void attachCornerView()
        {
            psui::attachWorldOverlay<FpsCornerView>(sCornerView, "plus_fps_corner", true, psui::OverlayOrder::Back,
                                                    [](FpsCornerView& view, LLView* root) { view.findOfficialViews(root); });
        }

        //---------------------------------------------------------------------------------------------
        // In the status bar

        // The number, in the room updateStatusBar() made right of the volume button. Shown and placed by
        // updateStatusBar(); this only draws. A click opens the statistics floater.
        class FpsStatusView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit FpsStatusView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                if (plus::presets::snapshotInProgress())
                {
                    mText.forget();
                    return;
                }
                const LLFontGL* font = LLFontGL::getFontSansSerifSmall(); // as the clock beside it
                if (!font)
                {
                    return;
                }
                mText.update(font);

                static const LLUIColor color = LLUIColorTable::instance().getColor(STATUS_TEXT_COLOR, LLColor4::white);
                // In mouselook the bar's background is not drawn and the number is over the world.
                const LLFontGL::ShadowType shadow =
                    (gStatusBar && gStatusBar->isBackgroundVisible()) ? LLFontGL::NO_SHADOW : LLFontGL::DROP_SHADOW_SOFT;
                mText.render(0, getRect().getHeight() / 2, color.get(), LLFontGL::LEFT, shadow);
            }

            // A click opens the statistics floater (the graphics speed floater is removed).
            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                LLFloaterReg::showInstanceOrBringToFront("stats");
                return true;
            }

        private:
            FpsText mText;
        };

        // The status bar side, kept up by updateStatusBar() every frame. The status bar is made at login
        // (LLViewerWindow::initWorldUI()) and deleted with the world UI, so all of this is of one status
        // bar: a new one starts again from the official layout.
        struct StatusBarState
        {
            LLHandle<LLView>              bar;
            LLHandle<LLView>              view;    // FpsStatusView, once needed
            std::vector<LLHandle<LLView>> moved;   // STATUS_MOVED found in the bar
            LLHandle<LLView>              search;
            LLHandle<LLView>              balance;
            LLHandle<LLView>              volume;
            S32                           shift = 0;       // how far left of the official layout the group is now
            const LLFontGL*               font  = nullptr; // width is for this font
            S32                           width = 0;       // the number's room (WIDEST)
        };
        StatusBarState sStatus;

        void findStatusBarViews(LLView* bar)
        {
            for (const char* name : STATUS_MOVED)
            {
                if (LLView* view = bar->findChildView(name, false))
                {
                    sStatus.moved.push_back(view->getHandle());
                }
            }
            if (LLView* view = bar->findChildView(STATUS_SEARCH, false))
            {
                sStatus.search = view->getHandle();
            }
            if (LLView* view = bar->findChildView(STATUS_BALANCE, false))
            {
                sStatus.balance = view->getHandle();
            }
            if (LLView* view = bar->findChildView(STATUS_VOLUME, false))
            {
                sStatus.volume = view->getHandle();
            }
            if (!sStatus.balance.get() || !sStatus.volume.get())
            {
                LL_WARNS("Plus") << "fpscorner: the status bar has no " << STATUS_BALANCE << " or " << STATUS_VOLUME
                                 << "; the FPS is not shown there" << LL_ENDL;
            }
        }

        // The menus' right edge in the status bar's coordinates (0 without menus: mouselook hides them).
        S32 menuRight(const LLView* bar)
        {
            if (!gMenuBarView || !gMenuBarView->isInVisibleChain())
            {
                return 0;
            }
            const LLRect screen = gMenuBarView->calcScreenRect();
            S32 x = 0;
            S32 y = 0;
            bar->screenPointToLocal(screen.mRight, screen.mBottom, &x, &y);
            return x;
        }

        LLView* attachStatusView(LLView* bar)
        {
            FpsStatusView::Params params;
            params.name          = "plus_fps_status";
            params.follows.flags = FOLLOWS_RIGHT | FOLLOWS_TOP; // as the volume button
            params.mouse_opaque  = true;                        // its tooltip and click
            params.tool_tip      = LLTrans::getString("PlusFpsStatusBarToolTip");
            params.visible       = false;
            FpsStatusView* view = LLUICtrlFactory::create<FpsStatusView>(params);
            if (!view)
            {
                return nullptr;
            }
            bar->addChild(view);
            sStatus.view = view->getHandle();
            return view;
        }

        // Makes room in the status bar and shows the number there when it is to be shown there and the
        // room fits; gives the room back otherwise. Cheap: a few rectangles a frame, the views move only
        // when the room changes.
        void updateStatusBar(void*)
        {
            LLView* bar = gStatusBar;
            if (sStatus.bar.get() != bar)
            {
                sStatus = StatusBarState(); // the old bar's views are gone with it
                if (!bar)
                {
                    return;
                }
                sStatus.bar = bar->getHandle();
                findStatusBarViews(bar);
            }
            if (!bar)
            {
                return;
            }
            const LLView* balance = sStatus.balance.get();
            const LLView* volume  = sStatus.volume.get();
            if (!balance || !volume)
            {
                return; // not the layout this was written for: left alone
            }

            if (const LLFontGL* font = LLFontGL::getFontSansSerifSmall(); font && font != sStatus.font)
            {
                sStatus.font  = font;
                sStatus.width = font->getWidth(std::string(WIDEST));
            }

            S32 wanted = 0;
            if (displayOn() && currentPosition() == Position::StatusBar && inWorld() && sStatus.width > 0)
            {
                const S32 shift = STATUS_GAP + sStatus.width;
                S32 left = balance->getRect().mLeft;
                if (const LLView* search = sStatus.search.get(); search && search->getVisible())
                {
                    left = llmin(left, search->getRect().mLeft);
                }
                if (statusBarHasRoom(left + sStatus.shift, menuRight(bar), shift, STATUS_MENU_PAD))
                {
                    wanted = shift;
                }
            }

            if (wanted != sStatus.shift)
            {
                for (const LLHandle<LLView>& handle : sStatus.moved)
                {
                    if (LLView* view = handle.get())
                    {
                        view->translate(sStatus.shift - wanted, 0);
                    }
                }
                sStatus.shift = wanted;
            }

            LLView* view = sStatus.view.get();
            if (!view && wanted > 0)
            {
                view = attachStatusView(bar);
            }
            if (view)
            {
                if (wanted > 0)
                {
                    // the room: right of the volume button, as high as it; ends where the button ended before
                    const LLRect& button = volume->getRect();
                    const LLRect rect(button.mRight + STATUS_GAP, button.mTop, button.mRight + STATUS_GAP + sStatus.width, button.mBottom);
                    if (rect != view->getRect())
                    {
                        view->setShape(rect);
                    }
                }
                view->setVisible(wanted > 0);
            }
        }

        //---------------------------------------------------------------------------------------------

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusFpsCorner*).
            // Preferences > Charis: skins/default/xui/<lang>/panel_preferences_charis.xml. The on/off check
            // is the control PlusFpsCornerOn; picking a position (cb_fps_position) sets the position and
            // turns the display on.
            LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.FpsCorner.IsPosition",
                [](LLUICtrl*, const LLSD& param)
                {
                    return (S32)positionFrom(gSavedSettings.getS32(SETTING_POSITION)) == param.asInteger();
                });
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.FpsCorner.SetPosition",
                [](LLUICtrl*, const LLSD& param)
                {
                    gSavedSettings.setS32(SETTING_POSITION, (S32)positionFrom(param.asInteger()));
                    gSavedSettings.setBOOL(SETTING_ON, true);
                });
        }

        void onUIReady()
        {
            attachCornerView();
            gIdleCallbacks.addFunction(&updateStatusBar); // the status bar comes later, at login
        }

        plus::FeatureRegistration sRegistration({ "fpscorner", &init, &onUIReady });
    }
}
