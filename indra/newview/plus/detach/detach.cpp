/**
 * @file detach.cpp
 * @brief Detached floaters: a floater shown in a top-level window of its own,
 *        on any monitor, outside the main window (research/87, steps S0, S1).
 *        Registers with the plus foundation.
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

// Research/87 (in the sl-viewer repository), steps S0 and S1: a floater of
// the main window dragged by its title bar off the main window goes into a
// window of its own there, on any monitor (watchDragOut(); PlusDetachAutoOnDrag,
// the one detach item, 2026-10-23; in Preferences > Charis since 2026-11-01). Floaters that can go
// apart but not so (autoDetachable(): their title bar cannot be dragged), and
// all that can go apart while PlusDetachAutoOnDrag is off, get the detach
// button on their title bar instead. It goes back into the main window with
// the button on its title bar (left of the others), or by dragging its title
// bar onto the main window (Ctrl held: it stays apart, over the main window).
// Up to PlusDetachedMaxWindows (3) at once. Vulkan only. (Plus.Detach.Toggle,
// .Frontmost and .IsDetached stay registered, for no menu item now.)
//
// How it fits together (research/87 4, the ImGui multi-viewport model):
//   - UI coordinates go on past the main window over the whole desktop. The
//     floater moves into LLPlusDetachedFloaterView (detachview.h), which covers
//     the desktop in them, to where its window is.
//   - The window (OsWindow, detachwin32.h) hands its input to the main
//     window's message handling with the mouse in the main window's client
//     coordinates, so the UI's own handling reaches the floater. The <plus>
//     hooks in LLViewerWindow let such points through (plus::detach::
//     containsPoint() and friends), LLView's hit test sends them to the popups
//     shown there and the detached floaters only (installHitTest()); those in
//     llwindowwin32.cpp keep the focus in the viewer while it moves between
//     the windows, let the wheel through and place the IME's windows.
//   - Popups opened on the window (menus, combo lists, tooltips: the <plus>
//     hooks in llmenugl.cpp, llcombobox.cpp, llui.cpp) are kept inside it and
//     drawn into it; small floaters opened there (emoji and mention pickers)
//     and floaters that depend on its floater move onto it (the <plus> hook in
//     LLFloaterView::adjustToFitScreen(), fitFloater()).
//   - Each frame (render_ui(), with the main UI shown or hidden, and
//     plus/uiframe's UI-only frames), the window's UI is drawn into a render
//     target of the window's size and the renderer presents it on the window
//     with the frame
//     (LLRHI::sPresentWindow: a swapchain of its own, without vsync).
//   - The window follows the floater when the UI moves or resizes it (its drag
//     handle, resize handles); the floater keeps its place on the desktop
//     when the main window moves.
//   - Its place (rect, monitor) and whether it is apart are kept across
//     sessions (detachplaces.h): it opens apart again, after the next login
//     too, on its monitor, or in the middle of the main window's monitor when
//     that monitor is gone. While apart, LLFloater keeps neither storing nor
//     applying its rect control (the <plus> hooks in llfloater.cpp).
//
// A floater goes apart where it is on the screen (dragged off: under the
// mouse, the drag going on), or at its kept place while that is on a monitor
// (the detach button, Plus.Detach.Toggle, the floaters open apart at login).
//
// Not yet (S2 and later): several floaters in one window, popups as windows
// of their own (a menu taller than a small window is cut), the UI scale of
// the window's own monitor (it uses the main window's). LLRHI_DETACH=0 turns the menu items
// off; LLRHI_DETACH_LOG=1 logs what each window draws and presents every 5 s.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "detach.h"
#include "detachplaces.h"
#include "detachview.h"
#include "detachwin32.h"

#include "plus/photoscreen/psui/psstyle.h"

#include "llapp.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "lldraghandle.h"
#include "llevents.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llkeyboard.h"
#include "llmodaldialog.h"
#include "llmultifloater.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigl.h"
#include "llrootview.h"
#include "llstartup.h"
#include "llthread.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

// llviewerdisplay.cpp: the frame being drawn is a snapshot's.
extern bool gSnapshot;
// llfloater.cpp and llui.cpp, the <plus> hooks set here.
extern bool (*gPlusFitFloater)(LLFloaterView* view, LLFloater* floater);
extern LLRect (*gPlusPopupArea)(S32 screen_x, S32 screen_y);
extern LLRect (*gPlusResizeArea)(const LLView* view);

namespace
{
    using plus::detach::OsWindow;
    using plus::detach::ScreenRect;
    using plus::detach::WindowSpace;
    namespace places = plus::detach::places;

    constexpr char ENV_ENABLED[] = "LLRHI_DETACH";          // 0: the menu items do nothing
    constexpr char ENV_LOG[] = "LLRHI_DETACH_LOG";          // 1: what the windows draw, every LOG_PERIOD;
                                                            // 2: the same, and the windows show a test colour;
                                                            // 3: 2, and what reaches the screen (research/105);
                                                            // 4: 3, and the renderer's view of each step;
                                                            // 5: 1, and the CPU time each window costs
    constexpr F32 LOG_PERIOD = 5.f;                         // seconds
    constexpr char WINDOW_TITLE_PREFIX[] = "Charis Viewer - ";
    constexpr char STRINGS_FILE[] = "panel_plus_detach.xml";
    constexpr char SETTING_MAX_WINDOWS[] = "PlusDetachedMaxWindows";
    constexpr S32 MAX_WINDOWS_LIMIT = 8;
    // The return button's images (the floaters' own dock button).
    constexpr char RETURN_IMAGE[] = "Icon_Dock_Foreground";
    constexpr char RETURN_IMAGE_PRESSED[] = "Icon_Dock_Press";
    // The detach button's (its pair: an arrow up off the bar, research/105).
    constexpr char DETACH_IMAGE[] = "Plus_Detach_Foreground";
    constexpr char DETACH_IMAGE_PRESSED[] = "Plus_Detach_Press";
    constexpr char DETACH_BUTTON_NAME[] = "plus_detach_out_btn";
    // Floaters kept in the main window: no detach button, and a drag off it
    // does not take them apart either (whyNotDetachable()). Add here, by their
    // LLFloaterReg name, the floaters that turn out to work badly apart.
    // (Modal dialogs are kept in without being named.)
    const char* const NOT_DETACHABLE[] = {
        "region_restarting",    // a countdown alert in the middle of the main window
        "web_modal",            // a web page asked for as a dialog over the main window
        "toybox",               // its buttons are dragged onto the main window's toolbars
    };
    // Floaters whose content moves by itself all the time: apart, drawn at
    // PlusDetachedMaxFps even while nobody uses them (as if typed in), not at
    // PlusDetachedIdleFps (DetachedWindow::due()). By LLFloaterReg name.
    const char* const ALWAYS_DRAWN[] = {
        "mini_map", "plus_radar",                                                   // maps, avatars about
        "camera",                                                                   // the camera controls
        "stats", "scene_load_stats", "performance", "lagmeter", "block_timers",    // statistics, debugging
        "web_content", "search", "destinations", "marketplace",                     // web pages, media
        "upload_model",                                                             // the model's 3D preview
    };
    // How often updateDetachButtons() goes through the main window's floaters (fix-pipeleak).
    constexpr F32 DETACH_BUTTON_LOOK_SECONDS = 0.25f;
    // The title bar keeps at least this much (UI units, from its left) for
    // its title and for dragging, or the floater gets no detach button.
    constexpr S32 MIN_TITLE_ROOM = 40;
    // Over the window while letting go would put it back in the main window.
    const LLColor4 DROP_HINT_FILL(0.25f, 0.55f, 1.f, 0.2f);
    const LLColor4 DROP_HINT_EDGE(0.25f, 0.55f, 1.f, 0.9f);
    constexpr S32 DROP_HINT_EDGE_WIDTH = 3;     // UI units
    // On (the default): a main window's floater dragged by its title bar off
    // the main window goes apart there, the drag going on (watchDragOut()).
    // Off: the detach button and the menu only. (Dragging a window back onto
    // the main window and letting go puts it back either way: droppedOnMain().)
    constexpr char SETTING_AUTO_ON_DRAG[] = "PlusDetachAutoOnDrag";
    // How far off the main window's frame the mouse goes before the floater
    // goes apart (UI units, as pixels at the UI's scale).
    constexpr S32 DRAG_OUT_MARGIN = 8;
    // watchDragOut(): idles running with every mouse button up before a drag
    // whose button went up unheard is ended (fix-round84).
    constexpr U32 LOST_DRAG_IDLES = 2;

    bool enabled()
    {
        static const bool env_off = []()
        {
            const char* env = getenv(ENV_ENABLED);
            return env && strcmp(env, "0") == 0;
        }();
        return !env_off && LLRHI::sPresentWindow;  // the Vulkan renderer shows the windows
    }

    // LLRHI_DETACH_LOG: 0 (unset) to 5 (ENV_LOG says what each shows).
    int logLevel()
    {
        static const int level = []()
        {
            const char* env = getenv(ENV_LOG);
            return env ? llclamp(atoi(env), 0, 5) : 0;
        }();
        return level;
    }

    bool logging()
    {
        return logLevel() >= 1;
    }

    // LLRHI_DETACH_LOG=2: the windows show TEST_COLOUR instead of their UI.
    // A window that stays black then does not get its frames at all (see the
    // llvkwindows log lines); one that shows the colour gets them, and the UI
    // drawn into its target is what is missing.
    bool testColour()
    {
        const int level = logLevel();
        return level >= 2 && level <= 4;   // 5 measures the real UI
    }

    // LLRHI_DETACH_LOG=3: each window's line also says what the screen shows
    // in its middle and what Windows makes of the window (research/105; the
    // renderer's line gives the same pixel in its images).
    bool screenProbe()
    {
        const int level = logLevel();
        return level >= 3 && level <= 4;
    }

    // LLRHI_DETACH_LOG=4: also the window's target as the renderer resolves
    // it while the window's UI is drawn into it, and the GL state then.
    bool describeTarget()
    {
        return logLevel() == 4;
    }

    // LLRHI_DETACH_LOG=5: each window's line also gives the CPU time of its
    // UI drawn and handed to the renderer (research/105).
    bool timing()
    {
        return logLevel() >= 5;
    }

    // How often a window's UI is drawn and shown (research/105). Each drawing
    // costs the frame about what the floater costs in the main window, and
    // each present a driver call, so a window is drawn only as often as what
    // it shows can change:
    //   - the mouse on it, a drag from it, a drag about to put it back: every frame;
    //   - the mouse just gone: once (hover marks go);
    //   - the keyboard in its floaters (typing, the caret): PlusDetachedMaxFps (0: every frame);
    //   - nobody using it: PlusDetachedIdleFps, for what changes by itself
    //     (new chat, inventory fetched; 0: as PlusDetachedMaxFps).
    constexpr char SETTING_MAX_FPS[] = "PlusDetachedMaxFps";
    constexpr char SETTING_IDLE_FPS[] = "PlusDetachedIdleFps";
    const LLColor4 TEST_COLOUR(1.f, 0.f, 1.f, 1.f);

    size_t maxWindows()
    {
        return (size_t)llclamp(gSavedSettings.getS32(SETTING_MAX_WINDOWS), 1, MAX_WINDOWS_LIMIT);
    }

    constinit plus::psui::StringsPanel sStrings(STRINGS_FILE);

    // A text of panel_plus_detach.xml (localized), with its [ARGS].
    std::string text(const std::string& name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
    {
        return sStrings.get(name, args);
    }

    void tellUser(const std::string& message)
    {
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
    }

    // The name a floater's place is kept under: only floaters of one instance.
    std::string placeName(LLFloater* floater)
    {
        const LLSD& key = floater->getKey();
        const bool no_key = key.isUndefined() || (key.isString() && key.asString().empty());
        return no_key ? floater->getInstanceName() : std::string();
    }

    // A floater whose XML says save_visibility="false" (LL keeps its
    // visibility out on purpose: plus_subtitle starts the helper and the voice
    // transcription when opened, plus_local_mesh forces selection and takes
    // the inspect tool) is not opened again at login, apart or not: kept as
    // closed at the session's end. Floaters that do not say it (inventory,
    // conversations, ...) open again as before. Read once per file.
    bool keepsVisibilityOut(LLFloater* floater)
    {
        static std::map<std::string, bool> known;    // main thread only
        const std::string file = floater ? floater->getXMLFilename() : std::string();
        if (file.empty())
        {
            return false;
        }
        const auto it = known.find(file);
        if (it != known.end())
        {
            return it->second;
        }
        bool        save = true;
        LLXMLNodePtr root;
        const bool  out = LLUICtrlFactory::getLayeredXMLNode(file, root) && root.notNull() &&
                         root->getAttributeBOOL("save_visibility", save) && !save;
        known.emplace(file, out);
        return out;
    }

    // The floater's title bar (LLFloater::getDragHandle() is protected): its
    // child drag handle, whichever its name (LLFloater::addDragHandle():
    // "Drag Handle" on top, "drag" on the left; research/105: looking for
    // "drag" only found none on the usual floaters, and the title bar's
    // buttons of ours went nowhere).
    LLDragHandle* dragHandleOf(LLFloater* floater)
    {
        if (!floater)
        {
            return nullptr;
        }
        for (LLView* child : *floater->getChildList())
        {
            if (LLDragHandle* handle = dynamic_cast<LLDragHandle*>(child))
            {
                return handle;
            }
        }
        return nullptr;
    }

    // The title bar's own buttons (close, minimize, dock, help, ...: LLFloater::
    // sButtonNames) shown, all in one rect (the floater's coordinates; empty
    // when none is). Found by their names, not by the drag handle's buttons
    // rect, which may still hold an earlier button of ours.
    LLRect ownTitleButtons(LLFloater* floater)
    {
        constexpr char OWN_PREFIX[] = "llfloater_";
        constexpr char OWN_SUFFIX[] = "_btn";
        LLRect own;
        for (LLView* child : *floater->getChildList())
        {
            const std::string& name = child->getName();
            if (!child->getVisible() || name.rfind(OWN_PREFIX, 0) != 0 || name.size() < sizeof(OWN_SUFFIX) - 1 ||
                name.compare(name.size() - (sizeof(OWN_SUFFIX) - 1), std::string::npos, OWN_SUFFIX) != 0)
            {
                continue;
            }
            if (own.isEmpty())
            {
                own = child->getRect();
            }
            else
            {
                own.unionWith(child->getRect());
            }
        }
        return own;
    }

    // The title keeps clear of buttons (the floater's coordinates; empty: none).
    void keepTitleClear(LLFloater* floater, LLDragHandle* handle, const LLRect& buttons)
    {
        LLRect in_handle;
        if (buttons.notEmpty())
        {
            floater->localRectToOtherView(buttons, &in_handle, handle);
        }
        if (handle->getButtonsRect() != in_handle)
        {
            handle->setButtonsRect(in_handle);
            handle->reshape(handle->getRect().getWidth(), handle->getRect().getHeight());
        }
    }

    // Hides a title bar button of ours, and gives its room back to the title.
    void hideTitleButton(LLFloater* floater, LLView* button)
    {
        if (!floater || !button)
        {
            return;
        }
        if (button->getVisible())
        {
            button->setVisible(false);
        }
        if (LLDragHandle* handle = dragHandleOf(floater))
        {
            keepTitleClear(floater, handle, ownTitleButtons(floater));
        }
    }

    // Puts button (a child of floater) left of the title bar's own buttons
    // (right on the title bar when it has none), and keeps the title clear of
    // it. needs_room: where that leaves the title less than MIN_TITLE_ROOM,
    // the button is hidden instead (false); the return button always shows.
    bool placeTitleButton(LLFloater* floater, LLView* button, bool needs_room)
    {
        static LLUICachedControl<S32> button_size("UIFloaterCloseBoxSize", 0);
        static LLUICachedControl<S32> from_top("UICloseBoxFromTop", 0);
        LLDragHandle* handle = dragHandleOf(floater);
        if (!button || !handle)
        {
            return false;
        }
        const LLRect own = ownTitleButtons(floater);   // ours are not named as these
        const S32 right = own.notEmpty() ? own.mLeft : floater->getRect().getWidth() - LLPANEL_BORDER_WIDTH;
        LLRect rect;
        rect.setLeftTopAndSize(right - button_size - 1, floater->getRect().getHeight() - from_top, button_size, button_size);
        if (needs_room && rect.mLeft < MIN_TITLE_ROOM)
        {
            if (button->getVisible())
            {
                button->setVisible(false);
            }
            keepTitleClear(floater, handle, own);
            return false;
        }
        if (button->getRect() != rect)
        {
            button->setRect(rect);
        }
        button->setVisible(true);

        LLRect all = rect;      // the floater's coordinates: what the title keeps clear of
        if (own.notEmpty())
        {
            all.unionWith(own);
        }
        keepTitleClear(floater, handle, all);
        return true;
    }

    // A title bar button of ours, as the title bar's own look.
    LLButton* makeTitleButton(const std::string& name, const char* image, const char* pressed, const std::string& tool_tip,
                              std::function<void()> clicked)
    {
        static LLUICachedControl<S32> button_size("UIFloaterCloseBoxSize", 0);
        LLButton::Params p;
        p.name(name);
        p.rect(LLRect(0, button_size, button_size, 0));
        p.image_unselected(LLUI::getUIImage(image));
        p.image_selected(LLUI::getUIImage(pressed));
        p.image_hover_selected(LLUI::getUIImage(pressed));
        p.hover_glow_amount(0.33f);     // as the title bar's buttons
        p.tab_stop(false);
        p.follows.flags(FOLLOWS_TOP | FOLLOWS_RIGHT);
        p.tool_tip(tool_tip);
        p.click_callback.function([clicked](LLUICtrl*, const LLSD&) { clicked(); });
        return LLUICtrlFactory::create<LLButton>(p);
    }

    void deleteChild(LLView* view)
    {
        if (LLView* parent = view->getParent())
        {
            parent->removeChild(view);
        }
        delete view;
    }

    // A floater shown as a tab of another (nearby chat in the conversations)
    // goes apart with its host.
    LLFloater* outermost(LLFloater* floater)
    {
        LLFloater* host = floater ? floater->getHost() : nullptr;
        return host ? host : floater;
    }

    template <size_t N>
    bool listed(const char* const (&names)[N], const std::string& name)
    {
        return !name.empty() && std::any_of(std::begin(names), std::end(names), [&name](const char* listed_name) { return name == listed_name; });
    }

    // Why floater stays in the main window whatever it does (nullptr: it need not).
    const char* keptInMain(LLFloater* floater)
    {
        const LLModalDialog* dialog = dynamic_cast<const LLModalDialog*>(floater);
        if (dialog && dialog->isModal())
        {
            return "a modal dialog";
        }
        if (listed(NOT_DETACHABLE, floater->getInstanceName()))
        {
            return "kept in the main window (NOT_DETACHABLE)";
        }
        return nullptr;
    }

    // Why floater cannot go apart (nullptr: it can). detach() refuses it for
    // this (a drag off the main window, the detach button, Plus.Detach.Toggle,
    // the floaters opened apart again), and updateDetachButtons() gives it no
    // detach button.
    const char* whyNotDetachable(LLFloater* floater)
    {
        if (floater->getHost())
        {
            return "a tab of another floater";      // its host goes apart (outermost())
        }
        if (floater->getParent() != gFloaterView)
        {
            return "not in the main window";        // the snapshot floater's own view, a window already
        }
        if (floater->isDependent())
        {
            return "depends on another floater";    // it goes where that one is (fitFloater())
        }
        return keptInMain(floater);
    }

    // Whether a drag of floater's title bar off the main window takes it
    // apart (watchDragOut()): it can go apart, is not minimized, and its title
    // bar can be dragged (LLFloater::setCanDrag(false): the call dialogs, the
    // maturity and new feature dialogs). The one test for both the drag and
    // the detach button (updateDetachButtons(): the button goes on those that
    // can go apart but not so).
    bool autoDetachable(LLFloater* floater)
    {
        if (!floater || floater->isMinimized() || whyNotDetachable(floater))
        {
            return false;
        }
        const LLDragHandle* handle = dragHandleOf(floater);
        return handle && handle->getVisible() && handle->getEnabled();
    }

    // Why an open floater of the main window never gets the detach button
    // while it is open, whatever it does (nullptr: it may; whyNotDetachable(),
    // autoDetachable() and the title bar's room decide each time).
    const char* noDetachButtonEver(LLFloater* floater)
    {
        static LLUICachedControl<S32> button_size("UIFloaterCloseBoxSize", 0);
        if (const char* kept = keptInMain(floater))
        {
            return kept;
        }
        // A title bar on top, tall enough for a button (not the left one, not none).
        if (floater->isDragOnLeft() || floater->getHeaderHeight() < button_size || !dynamic_cast<LLDragHandleTop*>(dragHandleOf(floater)))
        {
            return "no title bar";
        }
        return nullptr;
    }

    // r moved (not resized) to lie inside area as far as it fits.
    LLRect movedInto(LLRect r, const LLRect& area)
    {
        S32 dx = 0;
        S32 dy = 0;
        if (r.mRight > area.mRight)
        {
            dx = area.mRight - r.mRight;
        }
        if (r.mLeft + dx < area.mLeft)
        {
            dx = area.mLeft - r.mLeft;
        }
        if (r.mBottom < area.mBottom)
        {
            dy = area.mBottom - r.mBottom;
        }
        if (r.mTop + dy > area.mTop)
        {
            dy = area.mTop - r.mTop;
        }
        r.translate(dx, dy);
        return r;
    }

    // A screen rect (UI units) in view's local coordinates.
    LLRect screenToLocal(const LLView* view, const LLRect& screen)
    {
        LLRect local;
        view->screenPointToLocal(screen.mLeft, screen.mBottom, &local.mLeft, &local.mBottom);
        view->screenPointToLocal(screen.mRight, screen.mTop, &local.mRight, &local.mTop);
        return local;
    }

    // UI coordinates (the root view's: bottom-left origin, UI units) and
    // desktop pixels (top-left origin), through the main window's client area.
    struct Mapping
    {
        LLVector2 scale;            // pixels per UI unit
        S32 client_left = 0;        // the main window's client area on the desktop
        S32 client_top = 0;
        S32 client_height = 0;

        static bool current(Mapping& out)
        {
#if LL_WINDOWS
            HWND main_window = gViewerWindow ? (HWND)gViewerWindow->getPlatformWindow() : NULL;
            POINT origin = { 0, 0 };
            if (!main_window || gViewerWindow->getWindow()->getMinimized() || !ClientToScreen(main_window, &origin))
            {
                return false;   // minimized: no place on the desktop (the windows are hidden with it)
            }
            out.client_left = origin.x;
            out.client_top = origin.y;
            out.client_height = gViewerWindow->getWindowHeightRaw();
            out.scale = gViewerWindow->getDisplayScale();
            return out.client_height > 0 && out.scale.mV[VX] > 0.f && out.scale.mV[VY] > 0.f;
#else
            return false;
#endif
        }

        ScreenRect toScreen(const LLRect& ui) const
        {
            ScreenRect screen;
            screen.left = client_left + ll_round(ui.mLeft * scale.mV[VX]);
            screen.right = client_left + ll_round(ui.mRight * scale.mV[VX]);
            screen.top = client_top + client_height - ll_round(ui.mTop * scale.mV[VY]);
            screen.bottom = client_top + client_height - ll_round(ui.mBottom * scale.mV[VY]);
            return screen;
        }

        LLRect toUI(const ScreenRect& screen) const
        {
            LLRect ui;
            ui.mLeft = ll_round((screen.left - client_left) / scale.mV[VX]);
            ui.mRight = ll_round((screen.right - client_left) / scale.mV[VX]);
            ui.mTop = ll_round((client_height - (screen.top - client_top)) / scale.mV[VY]);
            ui.mBottom = ll_round((client_height - (screen.bottom - client_top)) / scale.mV[VY]);
            return ui;
        }

        // The whole desktop (every monitor), in UI coordinates.
        LLRect desktopUI() const
        {
#if LL_WINDOWS
            ScreenRect desktop;
            desktop.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            desktop.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            return toUI(desktop);
#else
            return LLRect();
#endif
        }
    };

    // LLRHI_DETACH_LOG: why frames did not reach a window, counted.
    struct DrawCounts
    {
        U32 throttled = 0;          // not due yet (PlusDetachedMaxFps) and not in use
        F64 draw_ms = 0.0;          // timing(): CPU time of the frames drawn
        U32 presented = 0;
        U32 not_presented = 0;      // the renderer refused (llvkwindows logs why)
        U32 hidden = 0;             // the floater is not visible
        U32 no_window = 0;          // the OS window is still being made
        U32 no_size = 0;
        U32 no_target = 0;          // the render target could not be made
    };

    // A floater in a window of its own.
    class DetachedWindow
    {
    public:
        // dragged: taken apart by a drag off the main window, still going on
        // (its window takes the mouse, and letting go puts it back only once
        // the drag has been off the main window).
        DetachedWindow(LLFloater* floater, const LLRect& main_rect, bool could_minimize, const ScreenRect& rect,
                       const std::string& place_name, bool dragged)
            : mFloater(floater->getHandle()),
              mMainRect(main_rect),
              mCouldMinimize(could_minimize),
              mPlaceName(place_name),
              mScreenRect(rect),
              mSavedRect(rect),
              mUIRect(floater->calcScreenRect()),
              mOpensAgain(!keepsVisibilityOut(floater)),
              mAlwaysDrawn(listed(ALWAYS_DRAWN, floater->getInstanceName())),
              mDropNeedsLeaving(dragged),
              mWindow(rect, ll_convert<std::wstring>(WINDOW_TITLE_PREFIX + floater->getTitle()), dragged)
        {
            addReturnButton(floater);
        }

        ~DetachedWindow()
        {
            if (LLView* button = mReturnButton.get())
            {
                deleteChild(button);
            }
            // The renderer lets the window go before mWindow destroys it.
            void* handle = mWindow.handle();
            if (handle && LLRHI::sForgetWindow)
            {
                LLRHI::sForgetWindow(handle);
            }
        }

        DetachedWindow(const DetachedWindow&) = delete;
        DetachedWindow& operator=(const DetachedWindow&) = delete;

        LLFloater* floater() const { return mFloater.get(); }
        const LLRect& mainRect() const { return mMainRect; }
        bool couldMinimize() const { return mCouldMinimize; }
        const std::string& placeName() const { return mPlaceName; }
        // Opened again at login when the session ends with it open apart (keepsVisibilityOut()).
        bool opensAgain() const { return mOpensAgain; }

        // The window in UI coordinates (its floater's screen rect).
        LLRect uiRect() const
        {
            LLFloater* floater = mFloater.get();
            return floater ? floater->calcScreenRect() : LLRect();
        }

        // A UI point on the window.
        bool contains(S32 x, S32 y) const
        {
            LLFloater* floater = mFloater.get();
            return floater && floater->getVisible() && mWindow.handle() && floater->calcScreenRect().pointInRect(x, y);
        }

        // Its place now, as kept across sessions (never open for one that does not open again).
        places::Place place(bool detached, bool open) const
        {
            places::Place place;
            place.detached = detached;
            place.open = open && mOpensAgain;
            place.rect = mScreenRect;
            place.monitor = OsWindow::monitorOf(mScreenRect).device;
            return place;
        }

        // Moved or resized since its place was last kept, and let go.
        bool placeChanged() const
        {
            LLFloater* floater = mFloater.get();
            return mScreenRect != mSavedRect && floater && !gFocusMgr.childHasMouseCapture(floater);
        }
        void placeKept() { mSavedRect = mScreenRect; }

        // Its window could not be made: the floater must not stay in the detached view, unseen.
        bool windowFailed() const { return mWindow.failed(); }

        // The return button was clicked (once).
        bool takeReturnClick()
        {
            const bool clicked = mReturnClicked;
            mReturnClicked = false;
            return clicked;
        }

        // Idle: the window and the floater stay together. When the UI moved or
        // resized the floater (its drag handle, its resize handles), the window
        // follows; otherwise the floater keeps its place on the desktop (the
        // main window may have moved, which moves the UI coordinates).
        void follow(const Mapping& mapping)
        {
            LLFloater* floater = mFloater.get();
            LLRect ui = floater->calcScreenRect();
            if (ui != mUIRect)
            {
                mScreenRect = mapping.toScreen(ui);
            }
            else
            {
                const LLRect want = mapping.toUI(mScreenRect);
                if (want.mLeft != ui.mLeft || want.mTop != ui.mTop)
                {
                    const LLRect& rect = floater->getRect();
                    floater->setOrigin(rect.mLeft + want.mLeft - ui.mLeft, rect.mBottom + want.mTop - ui.mTop);
                    ui = floater->calcScreenRect();
                }
                // In pixels it follows the UI scale.
                const ScreenRect size = mapping.toScreen(ui);
                mScreenRect.right = mScreenRect.left + size.width();
                mScreenRect.bottom = mScreenRect.top + size.height();
            }
            mUIRect = ui;
            mWindow.setRect(mScreenRect);
            placeReturnButton();
        }

        // Idle: the floater dragged by its title bar onto the main window and
        // let go there, Ctrl not held (true, once): it goes back.
        bool droppedOnMain()
        {
            LLFloater* floater = mFloater.get();
            LLDragHandle* handle = dragHandleOf(floater);
            const bool dragging = handle && gFocusMgr.getMouseCapture() == handle;
            if (dragging)
            {
                const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
                const bool over_main = gViewerWindow->getRootView()->getLocalRect().pointInRect(mouse.mX, mouse.mY);
                const bool keep_apart = (gKeyboard->currentMask(true) & MASK_CONTROL) != 0;
                if (!over_main)
                {
                    mDropNeedsLeaving = false;
                }
                mDropArmed = over_main && !keep_apart && !mDropNeedsLeaving;
            }
            const bool dropped = mDragging && !dragging && mDropArmed;
            mDragging = dragging;
            if (!dragging)
            {
                mDropArmed = false;
                mDropNeedsLeaving = false;
            }
            return dropped;
        }

        // render_ui(), main framebuffer bound: the window's UI (its floater,
        // the floaters on it in front, then the popups shown on it) into the
        // target, handed to the renderer to show on the window with this frame.
        void draw(const Mapping& mapping, const std::vector<LLFloater*>& on_top)
        {
            LLFloater* floater = mFloater.get();
            void* handle = mWindow.handle();
            const S32 width = mScreenRect.width();
            const S32 height = mScreenRect.height();
            if (!floater || !floater->getVisible())
            {
                ++mCounts.hidden;
                return;
            }
            if (!handle)
            {
                ++mCounts.no_window;
                return;
            }
            if (width <= 0 || height <= 0)
            {
                ++mCounts.no_size;
                return;
            }
            const bool resized = !mTarget.isComplete() || mTarget.getWidth() != (U32)width || mTarget.getHeight() != (U32)height;
            const F64 now = LLTimer::getTotalSeconds();
            if (!resized && !due(floater, on_top, now))
            {
                ++mCounts.throttled;
                return;     // the window keeps showing its last picture
            }
            if (resized && !mTargetRetry.due())
            {
                // It could not be made a moment ago: not again every frame.
                // Nothing is presented, so the window keeps its last picture.
                ++mCounts.no_target;
                return;
            }
            mLastDrawn = now;
            if (resized)
            {
                mTarget.release();
                if (!mTarget.allocate((U32)width, (U32)height, GL_RGBA8))
                {
                    mTarget.release();  // a half-made target: the next try starts from nothing
                    mTargetRetry.failed();
                    ++mCounts.no_target;
                    LL_WARNS_ONCE("Detach") << "No " << width << "x" << height << " target for a detached window" << LL_ENDL;
                    return;
                }
                mTargetRetry.succeeded();
            }

            mTarget.bindTarget();
            {
                // UI state (as LLViewerWindow::draw() has it), whatever the main window's UI left.
                LLGLSUIDefault gls_ui;
                LLGLDisable scissor(GL_SCISSOR_TEST);
                gGL.setColorMask(true, true);
                const LLColor4 background = testColour() ? TEST_COLOUR : LLColor4::black;
                LLRHI::clearColor(background.mV[VRED], background.mV[VGREEN], background.mV[VBLUE], 1.f);
                mTarget.clear(GL_COLOR_BUFFER_BIT);
                if (describeTarget() && LLRHI::sDescribeDrawTarget)
                {
                    mDrawTarget = "texture " + std::to_string(mTarget.getTexture()) + ", " + LLRHI::sDescribeDrawTarget();
                }
                gGL.setColorMask(true, false);

                gl_state_for_2d(width, height);
                gUIProgram.bind();
                gGL.color4f(1.f, 1.f, 1.f, 1.f);
                if (!testColour())
                {
                    // Where the floater is now (the UI may have moved it since
                    // follow()): the window shows it whole while it catches up.
                    const LLRect ui = floater->calcScreenRect();
                    WindowSpace space(ui, mapping.scale, handle, height);
                    space.draw(floater);
                    for (LLFloater* other : on_top)
                    {
                        space.draw(other);
                    }
                    space.drawPopups();
                    if (mDropArmed)
                    {
                        drawDropHint(ui);
                    }
                }
                gGL.flush();
                gUIProgram.unbind();
            }
            mTarget.flush();

            if (LLRHI::sPresentWindow(handle, mTarget.getTexture(), (U32)width, (U32)height))
            {
                ++mCounts.presented;
            }
            else
            {
                ++mCounts.not_presented;
            }
            if (timing())
            {
                mCounts.draw_ms += (LLTimer::getTotalSeconds() - now) * 1000.0;
            }
        }

        // Whether the window is drawn this frame (SETTING_IDLE_FPS's rules).
        bool due(LLFloater* floater, const std::vector<LLFloater*>& on_top, F64 now)
        {
            static LLCachedControl<F32> max_fps(gSavedSettings, SETTING_MAX_FPS, 30.f);
            static LLCachedControl<F32> idle_fps(gSavedSettings, SETTING_IDLE_FPS, 4.f);
            const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
            const bool pointed = mDragging || mDropArmed || OsWindow::hasMouseCapture() || contains(mouse.mX, mouse.mY);
            const bool left = mWasPointed && !pointed;
            mWasPointed = pointed;
            if (pointed || left)
            {
                return true;
            }
            // What moves by itself all the time (the mini-map, ...: ALWAYS_DRAWN): drawn as if typed in.
            bool typed = mAlwaysDrawn || gFocusMgr.childHasKeyboardFocus(floater);
            for (LLFloater* other : on_top)
            {
                typed = typed || gFocusMgr.childHasKeyboardFocus(other);
            }
            const F32 fps = typed || idle_fps <= 0.f ? max_fps : idle_fps;
            return fps <= 0.f || now - mLastDrawn >= 1.0 / fps;
        }

        // LLRHI_DETACH_LOG: one line, then the counts start again.
        void log(std::ostringstream& line)
        {
            const DrawCounts& c = mCounts;
            LLFloater* floater = mFloater.get();
            line << (floater ? floater->getInstanceName() : std::string("(closed)")) << ": window " << mWindow.handle()
                 << " at " << mScreenRect.left << "," << mScreenRect.top << " " << mScreenRect.width() << "x"
                 << mScreenRect.height() << " px, UI " << mUIRect << "; presented " << c.presented << ", not presented "
                 << c.not_presented << ", hidden " << c.hidden << ", no window " << c.no_window << ", no size " << c.no_size
                 << ", no target " << c.no_target << ", throttled " << c.throttled;
            if (timing())
            {
                line << "; CPU ms per frame drawn (UI and hand-over) " << c.draw_ms / llmax(c.presented + c.not_presented, 1u);
            }
            if (describeTarget())
            {
                line << "; while its UI is drawn: " << mDrawTarget;
            }
            if (screenProbe())
            {
                line << "; " << mWindow.describe();
            }
            mCounts = DrawCounts();
        }

    private:
        // A button left of the title bar's own: back to the main window.
        void addReturnButton(LLFloater* floater)
        {
            // Handled at the next idle: the button goes with the window.
            LLButton* button = makeTitleButton("plus_detach_return_btn", RETURN_IMAGE, RETURN_IMAGE_PRESSED,
                                               text("return_tooltip"), [this]() { mReturnClicked = true; });
            floater->addChild(button);
            mReturnButton = button->getHandle();
            placeReturnButton();
        }

        // Keeps the return button left of the title bar's buttons, and the
        // title clear of it (LLFloater::updateTitleButtons() lays out its own
        // buttons again when the floater changes: the button follows then).
        void placeReturnButton()
        {
            placeTitleButton(mFloater.get(), mReturnButton.get(), false);
        }

        // Window space, UI units: the whole window tinted and edged.
        static void drawDropHint(const LLRect& ui)
        {
            const LLRect local(0, ui.getHeight(), ui.getWidth(), 0);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gl_rect_2d(local, DROP_HINT_FILL, true);
            for (S32 i = 0; i < DROP_HINT_EDGE_WIDTH; ++i)
            {
                gl_rect_2d(LLRect(local.mLeft + i, local.mTop - i, local.mRight - i, local.mBottom + i), DROP_HINT_EDGE, false);
            }
        }

        LLHandle<LLFloater> mFloater;
        LLRect mMainRect;           // its rect in the main window, to go back to
        bool mCouldMinimize;        // its own setting, to give back
        std::string mPlaceName;     // kept under this name across sessions (empty: not kept)
        ScreenRect mScreenRect;     // where the window is on the desktop
        ScreenRect mSavedRect;      // mScreenRect when its place was last kept
        LLRect mUIRect;             // the floater's screen rect at the last follow()
        bool mOpensAgain;           // !keepsVisibilityOut() of its floater
        bool mAlwaysDrawn;          // its floater is one of ALWAYS_DRAWN
        // Taken apart by a drag off the main window: the mouse point was last
        // kept at the main window's edge, so letting go puts it back only
        // once the drag has been off the main window (droppedOnMain()).
        bool mDropNeedsLeaving;
        OsWindow mWindow;
        LLRenderTarget mTarget;     // the window's UI, as many pixels as the window
        plus::RetryWait mTargetRetry;   // after mTarget could not be made (VRAM full)
        std::string mDrawTarget;    // describeTarget(): mTarget as the renderer had it at the last draw
        LLHandle<LLView> mReturnButton;
        bool mReturnClicked = false;
        bool mDragging = false;     // its drag handle has the mouse
        bool mDropArmed = false;    // and letting go now puts it back
        F64 mLastDrawn = 0.0;       // LLTimer::getTotalSeconds() of the last drawing (due())
        bool mWasPointed = false;   // due(): the mouse was on it (or in a drag from it) last frame
        DrawCounts mCounts;
    };

    class Manager
    {
    public:
        static Manager& manager();

        void init()
        {
            doOnIdleRepeating([]() { manager().update(); return false; });     // for the session
            // LLAppViewer::forceQuit() (the logout reply, the logout timing
            // out, a disconnection, the main window closed at once) sets the
            // app quitting, and no later frame runs idle() (doFrame()): the
            // windows go now, while the renderer still has its swapchains and
            // images. A forceQuit() from inside idle() (idleNetwork()) still
            // reaches update() once in that frame (gIdleCallbacks run after
            // it), which sees isExiting() and only ends the session (nothing
            // left by then).
            // Should "quitting" come while the windows are being gone through
            // (update(), drawWindows(), endSession(); no such path is known),
            // they go when that is done, still before any idle (WalkGuard).
            LLEventPumps::instance().obtain("LLApp").listen("PlusDetach", [](const LLSD& event)
            {
                if (event["status"].asString() == "quitting" && on_main_thread())
                {
                    manager().endSessionWhenFree();
                }
                return false;
            });
        }

        // The session ends: every floater back where it belongs before the
        // floaters' places are saved, kept as open apart for the next session.
        void endSession()
        {
            const WalkGuard walking(*this);
            while (!mWindows.empty())
            {
                returnToMain(mWindows.begin(), Return::SessionEnd);
            }
            // Those closed while quitting (update()) were open apart when it began.
            for (const std::string& name : mClosedQuitting)
            {
                places::Place place = places::get(name);
                if (place.detached && !place.open)
                {
                    place.open = true;
                    places::set(name, place);
                }
            }
            mClosedQuitting.clear();
        }

        // endSession() now, or, inside a walk of the windows, as it ends.
        void endSessionWhenFree()
        {
            if (mWalkDepth)
            {
                mEndSessionPending = true;
                return;
            }
            endSession();
        }

        bool hasWindows() const { return !mWindows.empty(); }

        bool isDetached(const std::string& name) const
        {
            LLFloater* floater = outermost(LLFloaterReg::findInstance(name));
            return floater && find(floater) != mWindows.end();
        }

        // The menu items: shows the floater apart (opening it first), or puts it back.
        void toggle(const std::string& name)
        {
            if (LLFloater* floater = outermost(LLFloaterReg::findInstance(name)))
            {
                auto it = find(floater);
                if (it != mWindows.end())
                {
                    returnToMain(it, Return::ByUser);
                    return;
                }
            }
            if (!canDetachMore())
            {
                return;
            }
            if (LLFloater* floater = outermost(LLFloaterReg::showInstance(name)))
            {
                detach(floater, true);
            }
        }

        void detachFrontmost()
        {
            if (!canDetachMore())
            {
                return;
            }
            LLFloater* floater = gFloaterView ? gFloaterView->getFrontmost() : nullptr;
            if (!floater)
            {
                tellUser(text("no_frontmost"));
                return;
            }
            detach(floater, true);
        }

        // Idle (main thread, before the frame is drawn).
        void update()
        {
            const WalkGuard walking(*this);
            if (LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent())
            {
                endSession();
                return;
            }
            if (LLStartUp::getStartupState() < STATE_STARTED)
            {
                return;
            }
            // Quitting (LLAppViewer::requestQuit()): the floaters are being
            // closed, on the windows too (the <plus> hooks in LLFloaterView::
            // closeAllChildren() and allChildrenClosed(): "save changes?"). The
            // windows stay while a floater on them is open (the user may cancel
            // the quit); one closed now opens apart after the next login if
            // the session does end (endSession()); none goes apart.
            const bool quitting = LLAppViewer::instance()->quitRequested();
            if (!quitting)
            {
                mClosedQuitting.clear();    // the quit was cancelled (LLAppViewer::abortQuit()): closed as any
                restoreAtLogin();
                updateDetachButtons();
                watchDragOut();
            }
            else
            {
                mDetachClicks.clear();
            }
            if (!mWindows.empty() || !mAdopted.empty() || !mToPlace.empty())
            {
                updateWindows(quitting);
                placePending();
                sweepAdopted();
            }
            if (!quitting)
            {
                detachReopened();
            }
            logIfDue();
        }

        void drawWindows()
        {
            if (gSnapshot)
            {
                ++mSkips.snapshot;
                return;     // not the frame on the screen
            }
            if (LLRenderTarget::sCurFBO != 0)
            {
                ++mSkips.target_bound;
                mSkips.last_target = LLRenderTarget::sCurFBO;
                return;     // not the frame on the screen
            }
            Mapping mapping;
            if (!LLRHI::sPresentWindow || !Mapping::current(mapping))
            {
                ++mSkips.no_mapping;
                return;
            }
            LL_PROFILE_ZONE_NAMED_CATEGORY_UI("Detached windows");
            const WalkGuard walking(*this);
            for (const std::unique_ptr<DetachedWindow>& window : mWindows)
            {
                window->draw(mapping, floatersOn(*window));
            }
            gViewerWindow->setup2DRender();     // the main window's 2D state again
        }

        bool contains(S32 x, S32 y) const
        {
            return windowAt(x, y) != nullptr;
        }

        // keepsMousePoint(): on a window, or a drag of a floater on one.
        bool keepsPoint(S32 x, S32 y) const
        {
            return contains(x, y) || capturedApart();
        }

        // gPlusResizeArea: the desktop (UI units) for a view in a floater on a
        // window, which its resize handles and bars may go about, as a main
        // window's go about the main window; else an empty rect.
        LLRect resizeArea(const LLView* view)
        {
            if (!mView || mWindows.empty())
            {
                return LLRect();
            }
            for (const LLView* up = view; up; up = up->getParent())
            {
                if (up->getParent() == mView)
                {
                    return mView->calcScreenRect();
                }
            }
            return LLRect();
        }

        // gPlusPopupArea: the window at a UI point, or an empty rect.
        LLRect popupArea(S32 x, S32 y) const
        {
            const DetachedWindow* window = windowAt(x, y);
            return window ? window->uiRect() : LLRect();
        }

        // gPlusFitFloater: LLFloaterView::adjustToFitScreen(), view being
        // gFloaterView or ours. A window's own floater stays where it is; a
        // floater that belongs on a window (one that depends on a floater on
        // it, or a small popup floater opened over it) moves onto it, inside
        // it; one that was on a window and is no more goes back to gFloaterView.
        // Moved at the next idle (placePending()), not here: the floater views
        // call this while going through their children (LLFloaterView::refresh()
        // every frame), which a floater changing views would upset.
        bool fitFloater(LLFloaterView* view, LLFloater* floater)
        {
            if (!mView || !floater || (mWindows.empty() && mAdopted.empty()))
            {
                return false;   // nothing apart: the official fitting, at no cost
            }
            if (find(floater) != mWindows.end())
            {
                return true;
            }
            if (belongsOn(floater) || floater->getParent() == mView)
            {
                const LLHandle<LLFloater> handle = floater->getHandle();
                if (std::find(mToPlace.begin(), mToPlace.end(), handle) == mToPlace.end())
                {
                    mToPlace.push_back(handle);
                }
                return true;
            }
            if (floater->getVisible() && floater->getParent() == gFloaterView)
            {
                mShownInMain.insert(floater->getHandle());   // a main window's floater from now on
            }
            return false;
        }

    private:
        enum class Return
        {
            ByUser,         // the button, a drag onto the main window, the menu item: no longer apart
            Closed,         // the floater was closed: apart again when it opens
            SessionEnd,     // logout or quit: apart again after the next login
        };

        // A floater on a window that is not the window's own (adopt()).
        struct Adopted
        {
            LLHandle<LLFloater> floater;
            LLHandle<LLFloater> owner;      // the window's own floater
        };

        using Windows = std::vector<std::unique_ptr<DetachedWindow>>;

        // Why frames drew no window at all (LLRHI_DETACH_LOG).
        struct Skips
        {
            U32 snapshot = 0;
            U32 target_bound = 0;   // a render target, not the screen, was bound
            U32 last_target = 0;
            U32 no_mapping = 0;     // minimized, or no Vulkan
        };

        Windows::const_iterator find(const LLFloater* floater) const
        {
            return std::find_if(mWindows.begin(), mWindows.end(),
                                [floater](const std::unique_ptr<DetachedWindow>& window) { return window->floater() == floater; });
        }

        Windows::iterator find(const LLFloater* floater)
        {
            return std::find_if(mWindows.begin(), mWindows.end(),
                                [floater](const std::unique_ptr<DetachedWindow>& window) { return window->floater() == floater; });
        }

        const DetachedWindow* windowAt(S32 x, S32 y) const
        {
            for (const std::unique_ptr<DetachedWindow>& window : mWindows)
            {
                if (window->contains(x, y))
                {
                    return window.get();
                }
            }
            return nullptr;
        }

        DetachedWindow* windowAt(S32 x, S32 y)
        {
            return const_cast<DetachedWindow*>(std::as_const(*this).windowAt(x, y));
        }

        // The window a floater is on: its own, or the one it was adopted by.
        DetachedWindow* windowOf(const LLFloater* floater)
        {
            // fix-round90: never find(nullptr): it can hit a window whose floater has gone but which is not
            // cleaned up yet (updateWindows() returns early while the main window is minimized).
            if (!floater)
            {
                return nullptr;
            }
            auto it = find(floater);
            if (it != mWindows.end())
            {
                return it->get();
            }
            for (const Adopted& adopted : mAdopted)
            {
                if (adopted.floater.get() == floater)
                {
                    if (!adopted.owner.get())
                    {
                        return nullptr; // its owner has gone (see above)
                    }
                    auto owner = find(adopted.owner.get());
                    return owner != mWindows.end() ? owner->get() : nullptr;
                }
            }
            return nullptr;
        }

        // The window a floater that is not a window's own belongs on, if any.
        DetachedWindow* belongsOn(LLFloater* floater)
        {
            if (!floater->getVisible())
            {
                return nullptr;     // placed when it opens
            }
            if (LLFloater* dependee = floater->getDependee())
            {
                if (DetachedWindow* window = windowOf(dependee))
                {
                    return window;
                }
            }
            if (floater->getParent() == gFloaterView && gFocusMgr.childHasMouseCapture(floater))
            {
                return nullptr;     // the user is moving a main window's floater about
            }
            // Popup floaters (emoji, mention and gesture pickers): opened where text is typed.
            if (!floater->getIsChrome() && floater->getHeaderHeight() > 0)
            {
                return nullptr;
            }
            const LLRect rect = floater->calcScreenRect();
            if (floater->getParent() == mView)
            {
                // On a window already: it stays while any of it is on that window.
                DetachedWindow* owner = windowOf(floater);
                return owner && owner->uiRect().overlaps(rect) ? owner : nullptr;
            }
            // Only one just opened from the window (its text has the keyboard).
            // A chrome floater of the main window (the mini-map) that a window
            // was moved over stays in the main window: on the window it showed
            // only where the window was, and nowhere else (research/105).
            if (mShownInMain.count(floater->getHandle()))
            {
                return nullptr;
            }
            DetachedWindow* window = windowAt(rect.mLeft, rect.mTop - 1);
            return window && hasKeyboardFocus(*window) ? window : nullptr;
        }

        // Whether the keyboard focus is in window's floater or a floater on it.
        bool hasKeyboardFocus(const DetachedWindow& window)
        {
            LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
            LLFloater* floater = focus ? gFloaterView->getParentFloater(focus) : nullptr;
            return floater && windowOf(floater) == &window;
        }

        bool canDetachMore()
        {
            LLAppViewer* app = LLAppViewer::instance();
            if (LLApp::isExiting() || app->quitRequested() || app->logoutRequestSent())
            {
                return false;   // the session is ending: the windows are going (update())
            }
            if (!enabled())
            {
                LL_WARNS("Detach") << "Detached windows need the Vulkan renderer (and LLRHI_DETACH not 0)" << LL_ENDL;
                tellUser(text("needs_vulkan"));
                return false;
            }
            if (mWindows.size() >= maxWindows())
            {
                LLStringUtil::format_map_t args;
                args["[MAX]"] = std::to_string(maxWindows());
                tellUser(text("too_many", args));
                return false;
            }
            return true;
        }

        // Idle: the detach button on the title bar of the main window's open
        // floaters that can go apart but not by a drag off the main window
        // (autoDetachable()), or all that can go apart while SETTING_AUTO_ON_DRAG
        // is off (none while the renderer cannot show windows apart), and the
        // clicks on them.
        void updateDetachButtons()
        {
            mClicked.clear();
            mClicked.swap(mDetachClicks);   // the two keep their room: nothing allocated each frame
            for (const LLHandle<LLFloater>& handle : mClicked)
            {
                LLFloater* floater = handle.get();
                if (floater && find(floater) == mWindows.end() && canDetachMore())
                {
                    detach(floater, true);
                }
            }
            if (!enabled() || !gFloaterView)
            {
                return;
            }
            static LLCachedControl<bool> auto_on_drag(gSavedSettings, SETTING_AUTO_ON_DRAG, true);
            const bool all = !auto_on_drag;     // no drag off the main window: the button on all
            bool look = !mClicked.empty() || all != mButtonsOnAll || mDetachButtonLook.getElapsedTimeF32() >= DETACH_BUTTON_LOOK_SECONDS;
            if (all != mButtonsOnAll)
            {
                mButtonsOnAll = all;
                mNoDetachButton.clear();    // decided under the other setting
            }
            // fix-pipeleak: looking for the floaters and their buttons every
            // frame cost more than the rest of the idle. Between full looks
            // (every DETACH_BUTTON_LOOK_SECONDS, after a click, the setting
            // changed, and as soon as a button placed should go: its floater
            // closed, minimized, moved to a window or gone), the buttons
            // placed only follow their floaters' size. A floater just opened
            // gets its button at the next look; one whose own title buttons
            // were laid out again (LLFloater::updateTitleButtons()) has its
            // button placed again then.
            for (auto it = mDetachButtons.begin(); it != mDetachButtons.end() && !look; ++it)
            {
                LLFloater* floater = it->first.get();
                LLView* button = it->second.button.get();
                if (!floater || !button || button->getParent() != floater || !floater->getVisible() || floater->isMinimized() ||
                    floater->getParent() != gFloaterView || floater->isDependent())
                {
                    look = true;
                    break;
                }
                const LLRect& rect = floater->getRect();
                if (rect.getWidth() != it->second.width || rect.getHeight() != it->second.height)
                {
                    it->second.width = rect.getWidth();
                    it->second.height = rect.getHeight();
                    look = !placeTitleButton(floater, button, true);    // too narrow now: it goes at the look
                }
            }
            if (look)
            {
                lookForDetachButtons(all);
            }
        }

        // Idle: a main window's floater that autoDetachable() lets go apart,
        // dragged by its title bar off the main window, goes apart where it
        // is, and the drag goes on on its window (SETTING_AUTO_ON_DRAG).
        // A press on a floater clips the cursor to the main window's client
        // area (LLViewerWindow::handleAnyMouseClick(), the tool's
        // clipMouseWhenDown()): for such a drag the clipping is let go, so the
        // mouse can leave. The floater stays inside the main window (the mouse
        // point is kept at its edge, LLViewerWindow::saveLastMouse(); the top
        // also below the bars there, LLFloaterView::adjustToFitScreen()) until
        // the cursor is off the main window's frame by DRAG_OUT_MARGIN, any
        // side alike (OsWindow::cursorOffMainWindow(), desktop pixels). Then
        // it goes apart with the spot it was grabbed at on the mouse point
        // kept at the edge (the drag handle's last point): the drag handle,
        // keeping the UI's mouse capture, moves it from there with the mouse
        // (keepsMousePoint(): capturedApart()), the spot staying under the
        // cursor. Its window takes the OS's mouse capture (OsWindow), so the
        // moves come wherever the mouse goes: the main window's own capture
        // holds nothing (detach-vertical, 2026-10-23). Once per drag.
        // Nothing but a test while no mouse capture is held.
        //
        // fix-round84: with the clipping let go, the button can go up where the
        // viewer does not hear it (on the main window's own title bar or frame,
        // a little off it, anywhere off it while the drag is not taken apart),
        // and the drag handle would keep the mouse. So while the clipping is let
        // go (mDragOut.unclipped), every way out that does not take the floater
        // apart clips the cursor again (restoreClipping(): Ctrl held, no window
        // free, detach() refused, the floater gone or moved), and a drag that
        // ever had it let go (mDragOut.watched) whose buttons are all up is
        // ended as if the left button went up (endLostDrag()). Taken apart, its window has the mouse capture and
        // ends the drag itself (OsWindow).
        void watchDragOut()
        {
            LLMouseHandler* captor = gFocusMgr.getMouseCapture();
            if (!captor)
            {
                mDragOut.handle = nullptr;
                mDragOut.unclipped = false;     // the button went up: LLViewerWindow ended the clipping
                mDragOut.watched = false;
                return;
            }
            if (captor == mDragOut.handle && mDragOut.watched)
            {
                // Up at two idles running: not a button up heard but not handled yet (posted from the window thread).
                mDragOut.idles_up = OsWindow::anyMouseButtonDown() ? 0 : mDragOut.idles_up + 1;
                if (mDragOut.idles_up >= LOST_DRAG_IDLES)
                {
                    endLostDrag();
                    return;
                }
            }
            if (captor == mDragOut.handle && !mDragOut.armed)
            {
                return;     // this drag was decided: nothing to do until it ends
            }
            static LLCachedControl<bool> auto_on_drag(gSavedSettings, SETTING_AUTO_ON_DRAG, true);
            if (captor != mDragOut.handle)
            {
                // A new drag: one of a main window's floater by its title bar, that could go apart?
                mDragOut.handle = captor;
                mDragOut.armed = false;
                mDragOut.unclipped = false;
                mDragOut.watched = false;
                mDragOut.idles_up = 0;
                LLDragHandle* handle = dynamic_cast<LLDragHandle*>(captor);
                LLFloater* floater = handle ? dynamic_cast<LLFloater*>(handle->getParent()) : nullptr;
                const bool ctrl = (gKeyboard->currentMask(true) & MASK_CONTROL) != 0;
                if (!auto_on_drag || ctrl || !floater || !enabled() || dragHandleOf(floater) != handle || !autoDetachable(floater))
                {
                    return;
                }
                mDragOut.floater = floater->getHandle();
                mDragOut.armed = true;
                // Where on the floater it was grabbed (its drag handle, on top or on the left: kept on it).
                const LLRect rect = floater->calcScreenRect();
                const LLRect bar = handle->calcScreenRect();
                const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
                mDragOut.grab_x = llclamp(mouse.mX, bar.mLeft, llmax(bar.mLeft, bar.mRight)) - rect.mLeft;
                mDragOut.grab_down = rect.mTop - llclamp(mouse.mY, llmin(bar.mBottom, bar.mTop), bar.mTop);
            }
            LLFloater* floater = mDragOut.floater.get();
            if (!auto_on_drag || !floater || floater->getParent() != gFloaterView)
            {
                mDragOut.armed = false;
                restoreClipping();
                return;
            }
            if ((gKeyboard->currentMask(true) & MASK_CONTROL) != 0)
            {
                restoreClipping();  // held: it stays in the main window (as the drop onto the main window)
                return;
            }
            // Again each time: a press clips the cursor anew (nothing when it is not clipped).
            gViewerWindow->getWindow()->setMouseClipping(false);
            mDragOut.unclipped = true;
            mDragOut.watched = true;
            const S32 margin = ll_round(DRAG_OUT_MARGIN * gViewerWindow->getDisplayScale().mV[VX]);
            if (!OsWindow::cursorOffMainWindow(margin))
            {
                return;
            }
            mDragOut.armed = false;
            bool detached = false;
            if (canDetachMore())    // tells the user when there are windows enough
            {
                const LLRect rect = floater->calcScreenRect();
                const LLCoordGL mouse = gViewerWindow->getCurrentMouse();     // still kept at the edge
                LLRect at;
                at.setLeftTopAndSize(mouse.mX - mDragOut.grab_x, mouse.mY + mDragOut.grab_down, rect.getWidth(), rect.getHeight());
                detached = detach(floater, false, &at);
            }
            if (detached)
            {
                mDragOut.unclipped = false;     // its window has the mouse now and ends the drag
                mDragOut.watched = false;
            }
            else
            {
                restoreClipping();  // the drag goes on in the main window, as without this
            }
        }

        // watchDragOut(): the drag stays in the main window after all: the
        // cursor is clipped to it again, as LLViewerWindow had it (the tool's
        // clipMouseWhenDown(), as at the press), so the button going up comes
        // to the main window (nothing when not let go). fix-round100: only while
        // a button is still down - one that went up unheard since leaves the
        // cursor free (nothing would end that clipping before the next click).
        void restoreClipping()
        {
            if (mDragOut.unclipped)
            {
                mDragOut.unclipped = false;
                LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();
                if (tool && tool->clipMouseWhenDown() && OsWindow::anyMouseButtonDown())
                {
                    gViewerWindow->getWindow()->setMouseClipping(true);
                }
            }
        }

        // watchDragOut(): every button up, unheard (on the main window's title
        // bar or frame, or off it): the UI hears the left button go up where
        // the mouse is, so the drag handle lets go and LLViewerWindow ends the
        // clipping and its capture, as for a button up it hears.
        void endLostDrag()
        {
            mDragOut.unclipped = false;     // the button up just below ends the clipping, if any
            mDragOut.watched = false;
            mDragOut.armed = false;
            const LLCoordGL ui = gViewerWindow->getCurrentMouse();
            const LLVector2& scale = gViewerWindow->getDisplayScale();
            const LLCoordGL raw(ll_round(ui.mX * scale.mV[VX]), ll_round(ui.mY * scale.mV[VY]));
            LL_INFOS("Detach") << "A drag off the main window ended with its button up unheard: ended" << LL_ENDL;
            gViewerWindow->handleMouseUp(gViewerWindow->getWindow(), raw, gKeyboard->currentMask(true));
        }

        // The mouse capture is held by a view on a window (the drag handle of
        // a floater taken apart while dragged: watchDragOut()).
        bool capturedApart() const
        {
            if (!mView)
            {
                return false;
            }
            for (const LLView* view = dynamic_cast<const LLView*>(gFocusMgr.getMouseCapture()); view; view = view->getParent())
            {
                if (view == mView)
                {
                    return true;
                }
            }
            return false;
        }

        // updateDetachButtons(), a full look: through the main window's
        // floaters (no copy of the list: buttons go on the floaters, not on
        // gFloaterView), each open one given its button or none. all: every
        // one that can go apart (SETTING_AUTO_ON_DRAG off).
        void lookForDetachButtons(bool all)
        {
            mDetachButtonLook.reset();
            ++mDetachButtonLooks;
            for (LLView* view : *gFloaterView->getChildList())
            {
                LLFloater* floater = dynamic_cast<LLFloater*>(view);
                if (!floater || !floater->getVisible())
                {
                    continue;   // closed: its button (if any) goes with it, forgotten below
                }
                const LLHandle<LLFloater> handle = floater->getHandle();
                if (mNoDetachButton.count(handle))
                {
                    continue;   // decided while it is open
                }
                auto shown = mDetachButtons.find(handle);
                LLView* button = shown != mDetachButtons.end() ? shown->second.button.get() : nullptr;
                if (button && button->getParent() != floater)
                {
                    button = nullptr;   // not its (taken off): looked for again below
                }
                if (const char* never = noDetachButtonEver(floater))
                {
                    mNoDetachButton.insert(handle);
                    noteDetachButton(floater, never);
                    hideTitleButton(floater, floater->findChild<LLButton>(DETACH_BUTTON_NAME, false));  // the setting turned off
                    if (shown != mDetachButtons.end())
                    {
                        mDetachButtons.erase(shown);
                    }
                    continue;
                }
                const char* skipped = floater->isMinimized() ? "minimized" : whyNotDetachable(floater);
                if (!skipped && !all && autoDetachable(floater))
                {
                    skipped = "goes apart by a drag off the main window";
                }
                if (!skipped)
                {
                    if (!button)
                    {
                        button = floater->findChild<LLButton>(DETACH_BUTTON_NAME, false);   // hidden at an earlier look
                    }
                    if (!button)
                    {
                        button = makeTitleButton(DETACH_BUTTON_NAME, DETACH_IMAGE, DETACH_IMAGE_PRESSED, text("detach_tooltip"),
                                                 [this, handle]() { mDetachClicks.push_back(handle); });     // at the next idle
                        floater->addChild(button);
                    }
                    if (!placeTitleButton(floater, button, true))
                    {
                        skipped = "title bar too narrow";
                    }
                }
                if (skipped)
                {
                    noteDetachButton(floater, skipped);
                    hideTitleButton(floater, button);
                    if (shown != mDetachButtons.end())
                    {
                        mDetachButtons.erase(shown);
                    }
                    continue;
                }
                noteDetachButton(floater, "shown");
                DetachButton& entry = shown != mDetachButtons.end() ? shown->second : mDetachButtons[handle];
                entry.button = button->getHandle();
                entry.width = floater->getRect().getWidth();
                entry.height = floater->getRect().getHeight();
                entry.look = mDetachButtonLooks;
            }
            // Those not seen open in the main window: closed, gone (the
            // button with it), or apart (detach() took the button).
            for (auto it = mDetachButtons.begin(); it != mDetachButtons.end();)
            {
                if (it->second.look == mDetachButtonLooks)
                {
                    ++it;
                    continue;
                }
                if (LLFloater* floater = it->first.get())
                {
                    LLView* button = it->second.button.get();
                    hideTitleButton(floater, button && button->getParent() == floater ? button : nullptr);
                    noteDetachButton(floater, !floater->getVisible() ? "closed" : "not in the main window");
                }
                it = mDetachButtons.erase(it);
            }
            for (auto it = mNoDetachButton.begin(); it != mNoDetachButton.end();)
            {
                LLFloater* floater = it->get();
                it = floater && floater->getVisible() && floater->getParent() == gFloaterView ? std::next(it) : mNoDetachButton.erase(it);
            }
        }

        // The log, once each time a floater's detach button changes state
        // (research/105: the button did not show).
        void noteDetachButton(LLFloater* floater, const char* state)
        {
            const std::string& name = floater->getInstanceName().empty() ? floater->getName() : floater->getInstanceName();
            auto it = mDetachButtonStates.find(name);
            if (it == mDetachButtonStates.end())
            {
                it = mDetachButtonStates.emplace(name, std::string()).first;
            }
            std::string& last = it->second;
            if (last != state)
            {
                last = state;
                LL_INFOS("Detach") << "Detach button: " << name << ": " << state << LL_ENDL;
            }
        }

        // Before floater goes apart: its detach button goes (the return
        // button takes the place).
        void removeDetachButton(LLFloater* floater)
        {
            if (LLButton* button = floater->findChild<LLButton>(DETACH_BUTTON_NAME, false))
            {
                deleteChild(button);
            }
            mDetachButtons.erase(floater->getHandle());
        }

        // Shows floater (open, in gFloaterView) apart: at its kept place while
        // that is on a monitor of the desktop as it is now (keptOnScreen()),
        // else where it is in the main window (the same pixels on the screen,
        // inside that monitor's work area). dragged_ui: being dragged off the
        // main window (watchDragOut()): at that screen rect (UI units), as it
        // is, kept place or not, so that the drag goes on from there.
        bool detach(LLFloater* floater, bool tell_user, const LLRect* dragged_ui = nullptr)
        {
            const bool dragged = dragged_ui != nullptr;
            if (!enabled() || mWindows.size() >= maxWindows())
            {
                return false;
            }
            if (find(floater) != mWindows.end())
            {
                return true;
            }
            if (const char* why = whyNotDetachable(floater))
            {
                LL_WARNS("Detach") << floater->getName() << " stays in the main window (" << why << "); not detached" << LL_ENDL;
                if (tell_user)
                {
                    LLStringUtil::format_map_t args;
                    args["[TITLE]"] = plus::safeText(floater->getTitle()); // an IM window's: someone's name
                    tellUser(text("not_detachable", args));
                }
                return false;
            }
            Mapping mapping;
            if (!Mapping::current(mapping))
            {
                return false;
            }
            if (!mView)
            {
                mView = LLPlusDetachedFloaterView::create();
                plus::detach::installHitTest(mView, [](S32 x, S32 y) { return manager().contains(x, y); });
            }
            mView->setScreenRect(mapping.desktopUI());
            if (floater->isMinimized())
            {
                floater->setMinimized(false);
            }
            if (floater->isDocked())
            {
                floater->setDocked(false, false);
            }

            // The window, in desktop pixels. The window has no frame and the
            // floater fills it, so the window's rect is the floater's.
            const std::string place_name = placeName(floater);
            const places::Place kept = places::get(place_name);
            const LLRect main_rect = floater->getRect();
            const S32 width = ll_round(main_rect.getWidth() * mapping.scale.mV[VX]);
            const S32 height = ll_round(main_rect.getHeight() * mapping.scale.mV[VY]);
            const ScreenRect here = mapping.toScreen(dragged ? *dragged_ui : floater->calcScreenRect());
            ScreenRect rect;
            const bool at_kept = !dragged && places::keptOnScreen(kept, rect);
            if (!at_kept)
            {
                rect = dragged ? here : here.fittedInto(OsWindow::monitorOf(here).work);
            }
            if (!floater->isResizable())
            {
                rect.right = rect.left + width;
                rect.bottom = rect.top + height;
            }

            removeDetachButton(floater);
            // The floater goes where the window is, in the detached view (at
            // least its smallest size).
            gFloaterView->removeChild(floater);
            mView->addChild(floater);
            LLRect local = mapping.toUI(rect);
            local.translate(-mView->getRect().mLeft, -mView->getRect().mBottom);
            S32 min_width = 0;
            S32 min_height = 0;
            floater->getResizeLimits(&min_width, &min_height);
            local.setLeftTopAndSize(local.mLeft, local.mTop, llmax(local.getWidth(), min_width), llmax(local.getHeight(), min_height));
            floater->setShape(local);
            // Minimized, it would go to the main window's bottom corner.
            const bool could_minimize = floater->isMinimizeable();
            floater->setCanMinimize(false);
            mView->bringToFront(floater, false);

            rect = mapping.toScreen(floater->calcScreenRect());     // as the UI rounded it
            mWindows.push_back(std::make_unique<DetachedWindow>(floater, main_rect, could_minimize, rect, place_name, dragged));
            DetachedWindow& window = *mWindows.back();
            places::set(place_name, window.place(true, true));
            window.placeKept();
            LL_INFOS("Detach") << "Detached " << floater->getName() << ": window at " << rect.left << "," << rect.top << " "
                               << rect.width() << "x" << rect.height() << " (desktop pixels)"
                               << (at_kept ? ", its kept place"
                                   : dragged ? ", dragged out of the main window"
                                   : kept.rect.empty() ? ", where it was" : ", where it was (its kept place is off the desktop)")
                               << LL_ENDL;
            return true;
        }

        // Puts the floater back in the main window, where it was or (drop)
        // where it was let go; the window goes.
        Windows::iterator returnToMain(Windows::iterator it, Return why, const LLRect* drop = nullptr)
        {
            DetachedWindow& window = **it;
            LLFloater* floater = window.floater();
            places::set(window.placeName(), window.place(why != Return::ByUser, why == Return::SessionEnd));
            for (size_t i = 0; i < mAdopted.size();)
            {
                LLFloater* adopted = mAdopted[i].floater.get();
                if (adopted && mAdopted[i].owner.get() == floater)
                {
                    unadopt(adopted);   // erases mAdopted[i]
                }
                else
                {
                    ++i;
                }
            }
            const LLRect main_rect = window.mainRect();
            const bool could_minimize = window.couldMinimize();
            if (floater)
            {
                endDragOf(floater);
            }
            it = mWindows.erase(it);    // the renderer forgets the window, which is destroyed
            if (floater && mView && floater->getParent() == mView)
            {
                const LLRect screen = floater->calcScreenRect();
                mView->removeChild(floater);
                gFloaterView->addChild(floater);
                // As if the user put it there: its place is stored again and it fits the window.
                floater->setShape(drop ? screenToLocal(gFloaterView, screen) : main_rect, true);
                if (drop)
                {
                    gFloaterView->adjustToFitScreen(floater, false);
                    gFloaterView->bringToFront(floater);
                }
                LL_INFOS("Detach") << "Back in the main window: " << floater->getName()
                                   << (why == Return::Closed ? " (closed)" : why == Return::SessionEnd ? " (session end)" : "")
                                   << LL_ENDL;
            }
            if (floater)
            {
                // Wherever it is now (closed with the window, moved elsewhere): detach() took it.
                floater->setCanMinimize(could_minimize);
            }
            return it;
        }

        // returnToMain(): a drag of floater still going on ends here (fix-round86).
        // Its window may never have been made (taken apart by a drag off the
        // main window: watchDragOut() left the drag to that window, which was
        // to take the mouse), or the session ends mid-drag: the drag handle
        // would keep the mouse and the floater follow the cursor in the main
        // window until the next click there. The ways back while nothing drags
        // (the return button, a drop onto the main window, closing) find no
        // capture and do nothing. The cursor is not clipped then (a drag-out
        // let the clipping go; one on the window never had it).
        void endDragOf(LLFloater* floater)
        {
            if (!gFocusMgr.childHasMouseCapture(floater))
            {
                return;
            }
            LL_INFOS("Detach") << "A drag of " << floater->getName() << " ended: its window goes" << LL_ENDL;
            gFocusMgr.setMouseCapture(nullptr);
            if (gViewerWindow && gViewerWindow->getWindow())
            {
                gViewerWindow->getWindow()->setMouseClipping(false);
            }
            mDragOut.unclipped = false;
            mDragOut.watched = false;
            mDragOut.armed = false;
        }

        // quitting: a floater closed now is noted for endSession().
        void updateWindows(bool quitting)
        {
            Mapping mapping;
            if (!Mapping::current(mapping))
            {
                return;
            }
            mView->setScreenRect(mapping.desktopUI());
            for (auto it = mWindows.begin(); it != mWindows.end();)
            {
                DetachedWindow& window = **it;
                LLFloater* floater = window.floater();
                if (!floater || !floater->getVisible() || floater->getParent() != mView)
                {
                    if (quitting && !window.placeName().empty() && window.opensAgain())
                    {
                        mClosedQuitting.push_back(window.placeName());
                    }
                    it = returnToMain(it, Return::Closed);      // closed, or taken elsewhere
                    continue;
                }
                if (window.windowFailed())
                {
                    LL_WARNS("Detach") << "No window for " << window.placeName() << "; back on the main window" << LL_ENDL;
                    it = returnToMain(it, Return::ByUser);
                    continue;
                }
                if (window.takeReturnClick())
                {
                    it = returnToMain(it, Return::ByUser);
                    continue;
                }
                if (window.droppedOnMain())
                {
                    const LLRect drop = floater->calcScreenRect();
                    it = returnToMain(it, Return::ByUser, &drop);
                    continue;
                }
                window.follow(mapping);
                if (window.placeChanged())
                {
                    places::set(window.placeName(), window.place(true, true));
                    window.placeKept();
                }
                ++it;
            }
        }

        // fitFloater()'s floaters: onto the window they belong on, or back to gFloaterView.
        void placePending()
        {
            std::vector<LLHandle<LLFloater>> pending;
            pending.swap(mToPlace);
            for (const LLHandle<LLFloater>& handle : pending)
            {
                LLFloater* floater = handle.get();
                if (!floater || find(floater) != mWindows.end())
                {
                    continue;
                }
                if (DetachedWindow* window = belongsOn(floater))
                {
                    adopt(floater, *window);
                }
                else if (floater->getParent() == mView)
                {
                    unadopt(floater);
                }
                else
                {
                    gFloaterView->adjustToFitScreen(floater, false);   // as it was asked for
                }
            }
        }

        // Moves floater onto window (into the detached view), inside it.
        void adopt(LLFloater* floater, DetachedWindow& window)
        {
            const LLRect screen = floater->calcScreenRect();
            if (floater->getParent() != mView)
            {
                if (LLView* parent = floater->getParent())
                {
                    parent->removeChild(floater);
                }
                mView->addChild(floater);
            }
            auto it = std::find_if(mAdopted.begin(), mAdopted.end(),
                                   [floater](const Adopted& adopted) { return adopted.floater.get() == floater; });
            if (it == mAdopted.end())
            {
                mAdopted.push_back({ floater->getHandle(), window.floater()->getHandle() });
            }
            else
            {
                it->owner = window.floater()->getHandle();
            }
            const LLRect local = screenToLocal(mView, movedInto(screen, window.uiRect()));
            floater->setOrigin(local.mLeft, local.mBottom);
            mView->sendChildToFront(floater);
        }

        // Back to gFloaterView, where it is on the screen, then fitted to the main window.
        void unadopt(LLFloater* floater)
        {
            mAdopted.erase(std::remove_if(mAdopted.begin(), mAdopted.end(),
                                          [floater](const Adopted& adopted) { return adopted.floater.get() == floater; }),
                           mAdopted.end());
            if (floater->getParent() != mView)
            {
                return;
            }
            const LLRect screen = floater->calcScreenRect();
            mView->removeChild(floater);
            gFloaterView->addChild(floater);
            const LLRect local = screenToLocal(gFloaterView, screen);
            floater->setOrigin(local.mLeft, local.mBottom);
            gFloaterView->adjustToFitScreen(floater, false);
        }

        // Adopted floaters closed, gone, whose window went or that are no
        // longer on it: back to gFloaterView. Main window floaters closed: a
        // popup again when they next open.
        void sweepAdopted()
        {
            for (size_t i = 0; i < mAdopted.size();)
            {
                LLFloater* floater = mAdopted[i].floater.get();
                auto owner = find(mAdopted[i].owner.get());
                if (!floater)
                {
                    mAdopted.erase(mAdopted.begin() + i);
                }
                else if (!floater->getVisible() || owner == mWindows.end() ||
                         !(*owner)->uiRect().overlaps(floater->calcScreenRect()))
                {
                    unadopt(floater);
                }
                else
                {
                    ++i;
                }
            }
            for (auto it = mShownInMain.begin(); it != mShownInMain.end();)
            {
                LLFloater* floater = it->get();
                if (!floater || !floater->getVisible() || floater->getParent() != gFloaterView)
                {
                    it = mShownInMain.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // The floaters adopted by window, back to front.
        std::vector<LLFloater*> floatersOn(const DetachedWindow& window) const
        {
            std::vector<LLFloater*> floaters;
            if (mAdopted.empty() || !mView)
            {
                return floaters;
            }
            const LLView::child_list_t& children = *mView->getChildList();
            for (auto it = children.rbegin(); it != children.rend(); ++it)
            {
                for (const Adopted& adopted : mAdopted)
                {
                    if (adopted.floater.get() == *it && adopted.owner.get() == window.floater())
                    {
                        floaters.push_back(adopted.floater.get());
                    }
                }
            }
            return floaters;
        }

        // Once after login: the floaters that were open apart when the last
        // session ended open again (detachReopened() takes them apart).
        void restoreAtLogin()
        {
            if (mRestored)
            {
                return;
            }
            mRestored = true;
            if (!enabled())
            {
                return;
            }
            const std::vector<std::string> names = places::detachedNames();
            for (const std::string& name : names)
            {
                places::Place place = places::get(name);
                if (place.open)
                {
                    // Kept open by an earlier build: built (postBuild() only, not opened) to read its XML.
                    if (keepsVisibilityOut(LLFloaterReg::getInstance(name)))
                    {
                        LL_INFOS("Detach") << "Not opening " << name << " again: its visibility is not kept (save_visibility)" << LL_ENDL;
                        place.open = false;
                        places::set(name, place);
                        continue;
                    }
                    LL_INFOS("Detach") << "Opening " << name << " apart, as at the end of the last session" << LL_ENDL;
                    LLFloaterReg::showInstance(name);
                }
            }
        }

        // A floater kept as opening apart, open in the main window (just
        // opened): taken apart, to its kept place.
        void detachReopened()
        {
            if (!enabled() || places::detachedNames().empty())
            {
                return;
            }
            std::vector<LLFloater*> opened;     // detach() writes the places: not while going through them
            for (const std::string& name : places::detachedNames())
            {
                LLFloater* floater = LLFloaterReg::findInstance(name);
                // Not one detach() refuses (opened depending on another, ...): no warning each frame.
                if (floater && floater->getVisible() && !floater->isMinimized() && !whyNotDetachable(floater))
                {
                    opened.push_back(floater);
                }
            }
            for (LLFloater* floater : opened)
            {
                detach(floater, false);     // up to maxWindows()
            }
        }

        void logIfDue()
        {
            if (!logging() || mLogTimer.getElapsedTimeF32() < LOG_PERIOD)
            {
                return;
            }
            mLogTimer.reset();
            std::ostringstream line;
            line << "Detached windows: " << mWindows.size() << " (+" << mAdopted.size() << " floaters on them); frames skipped: snapshot "
                 << mSkips.snapshot << ", render target bound " << mSkips.target_bound << " (last " << mSkips.last_target
                 << "), minimized or no Vulkan " << mSkips.no_mapping << "; message loops broken " << OsWindow::forwardLoopsBroken();
            LL_INFOS("Detach") << line.str() << LL_ENDL;
            mSkips = Skips();
            for (const std::unique_ptr<DetachedWindow>& window : mWindows)
            {
                std::ostringstream window_line;
                window->log(window_line);
                LL_INFOS("Detach") << window_line.str() << LL_ENDL;
            }
        }

        LLPlusDetachedFloaterView* mView = nullptr;    // owned by the root view once made
        Windows mWindows;
        std::vector<Adopted> mAdopted;
        std::vector<LLHandle<LLFloater>> mToPlace;     // fitFloater()
        std::vector<LLHandle<LLFloater>> mDetachClicks;    // the detach buttons clicked (updateDetachButtons())
        std::vector<LLHandle<LLFloater>> mClicked;         // updateDetachButtons(): mDetachClicks taken, its room kept
        std::map<std::string, std::string, std::less<>> mDetachButtonStates;  // noteDetachButton()
        // updateDetachButtons(): the buttons shown, by their floaters.
        struct DetachButton
        {
            LLHandle<LLView> button;
            S32 width = 0;          // the floater's size when the button was placed
            S32 height = 0;
            U32 look = 0;           // mDetachButtonLooks when last seen
        };
        std::map<LLHandle<LLFloater>, DetachButton> mDetachButtons;
        // Open floaters that get no button while they stay open (noDetachButtonEver()).
        std::set<LLHandle<LLFloater>> mNoDetachButton;
        bool mButtonsOnAll = false;         // !SETTING_AUTO_ON_DRAG at the last look
        U32 mDetachButtonLooks = 0;         // full looks so far
        LLFrameTimer mDetachButtonLook;     // since the last full look
        // Floaters open in the main window when last fitted (never adopted
        // as popups: belongsOn()); dropped when closed or moved.
        std::set<LLHandle<LLFloater>> mShownInMain;
        // Place names of the floaters closed on windows while quitting (update(), endSession()).
        std::vector<std::string> mClosedQuitting;
        bool mRestored = false;
        // watchDragOut(): the drag going on (its mouse captor, compared only,
        // never used), its floater, and whether it may still take it apart.
        struct DragOut
        {
            const LLMouseHandler* handle = nullptr;
            LLHandle<LLFloater> floater;
            bool armed = false;
            bool unclipped = false; // the cursor's clipping to the main window let go for it (restoreClipping())
            // The clipping was let go for it at some point and it is not apart:
            // a button up may have gone unheard (endLostDrag()), even after the
            // clipping came back (restoreClipping()), until the drag ends.
            bool watched = false;
            U32 idles_up = 0;       // idles running with every mouse button up while watched (endLostDrag())
            S32 grab_x = 0;         // where it was grabbed, from the floater's left (UI units)
            S32 grab_down = 0;      // and down from its top
        };
        DragOut mDragOut;
        Skips mSkips;
        LLFrameTimer mLogTimer;

        // Held while mWindows is gone through: endSessionWhenFree() waits for
        // the outermost one to end, and that one then ends the session.
        class WalkGuard
        {
        public:
            explicit WalkGuard(Manager& manager) : mManager(manager) { ++mManager.mWalkDepth; }
            ~WalkGuard()
            {
                if (--mManager.mWalkDepth == 0 && mManager.mEndSessionPending)
                {
                    mManager.mEndSessionPending = false;
                    mManager.endSession();
                }
            }
            WalkGuard(const WalkGuard&) = delete;
            WalkGuard& operator=(const WalkGuard&) = delete;

        private:
            Manager& mManager;
        };
        U32 mWalkDepth = 0;
        bool mEndSessionPending = false;
    };

    // Never deleted: its render targets must not be released after the GL
    // context at exit (a static destructor would).
    Manager* sManager = nullptr;

    Manager& Manager::manager()
    {
        if (!sManager)
        {
            sManager = new Manager();
        }
        return *sManager;
    }

    Manager& manager()
    {
        return Manager::manager();
    }

    bool anyWindows()
    {
        return sManager && sManager->hasWindows();
    }

    void init()
    {
        // Check box: skins/default/xui/<lang>/panel_preferences_charis.xml ("c_detach_on_drag").
        // Hooks: llviewerdisplay.cpp (render_ui), llviewerwindow.cpp (mouse),
        // llwindowwin32.cpp (focus, wheel, IME), llfloater.cpp (the floater
        // view, rect controls, fitting floaters), llui.cpp, llmenugl.cpp and
        // llcombobox.cpp (popups, the mouse while a window is drawn).
        // The window thread is not running yet: its hooks are set before it reads them.
        OsWindow::installHooks();
        gPlusFitFloater = [](LLFloaterView* view, LLFloater* floater) { return sManager && sManager->fitFloater(view, floater); };
        gPlusPopupArea = [](S32 x, S32 y) { return sManager ? sManager->popupArea(x, y) : LLRect(); };
        gPlusResizeArea = [](const LLView* view) { return sManager ? sManager->resizeArea(view) : LLRect(); };
        // The detach button's images (the return button's are the official dock icon's).
        plus::declareUIImage(DETACH_IMAGE, "plus/Icon_Undock_Foreground.png");
        plus::declareUIImage(DETACH_IMAGE_PRESSED, "plus/Icon_Undock_Press.png");
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Detach.Toggle",
                                                                 [](LLUICtrl*, const LLSD& param) { manager().toggle(param.asString()); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Detach.Frontmost",
                                                                 [](LLUICtrl*, const LLSD&) { manager().detachFrontmost(); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Detach.IsDetached",
                                                                 [](LLUICtrl*, const LLSD& param) { return manager().isDetached(param.asString()); });
        manager().init();
    }

    plus::FeatureRegistration sRegistration({ "detach", &init, {} });
}

namespace plus::detach
{
    void drawWindows()
    {
        if (anyWindows())
        {
            sManager->drawWindows();
        }
    }

    bool containsPoint(S32 x, S32 y)
    {
        return anyWindows() && sManager->contains(x, y);
    }

    bool containsWindowPoint(const LLCoordWindow& point)
    {
        if (!anyWindows() || !gViewerWindow)
        {
            return false;
        }
        // As LLWindowWin32 makes GL points of window points, then the scale
        // LLViewerWindow divides them by.
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        const S32 gl_x = point.mX;
        const S32 gl_y = gViewerWindow->getWindowHeightRaw() - point.mY - 1;
        return sManager->contains(ll_round(gl_x / scale.mV[VX]), ll_round(gl_y / scale.mV[VY]));
    }

    bool keepsMousePoint(S32 x, S32 y)
    {
        return anyWindows() && (OsWindow::hasMouseCapture() || sManager->keepsPoint(x, y));
    }
}
