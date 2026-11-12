/**
 * @file invlayout.cpp
 * @brief Inventory window like a file explorer: where the columns of a wide window are and when they come.
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

// research/318 7. The row of the inventory's contents, between the header (the item count, the search bar, the
// tab row) and the footer (the received items), which keep the window's full width:
//   one column                 the contents as the official views have them;
//   tree | detail              from PlusInvExplorerMinWidth: the detail pane (invexplorer.cpp) on the right,
//                              turned on and off by PlusInvExplorerDetail, whose button this file adds at the
//                              right end of the search row while the window is wide enough for the detail;
//   tree | center | detail     from PlusInvExplorerColumnsWidth: the three columns (invcolumns.cpp has what they
//                              mean), the detail pane too while it shows.
// The contents `P` are the tab shown (in the official single-folder view, "combination_view_inventory", which
// never gets three columns). P keeps the rectangle its container gives it but its width, which this file narrows
// to the tree (the tab container sets a tab's rectangle once, LLTabContainer::addTabPanel, and only moves it by
// its follows afterwards). Only this file narrows P and gives it back (setNarrowed). The center column and the
// split before it are children of the tab container's parent ("default_inventory_panel"), the detail pane and the
// split before it children of the inventory view (the owner, so that they also show in the single-folder view);
// all are made the first time they show and die with the window. The window itself is the official one again
// (floater_my_inventory.xml).
//
// State (research/149 (R), research/318 7.5 and 8.5), all on the UI thread, one Row per window:
//   detailRoom   from a width >= the detail's showing line, kept down to the staying line, whatever
//                PlusInvExplorerDetail says.
//   detailShown  while there is detailRoom, PlusInvExplorerDetail is on, the pane could be made and P is at least
//                PANE_MIN_HEIGHT_PX high. A hidden pane is not built (invexplorer.cpp); it reads the selection again
//                right before it shows.
//   the detail's button (toggle) in the official search row ("top_stack"), tied to PlusInvExplorerDetail: shown
//                while turning the setting on would show the detail (detailShown without the setting), else
//                hidden, the search box getting its width back. Made once, at the window's first tick.
//   three columns (plus::invcolumns::isActive) from a width >= the entering line (it includes the detail pane at
//                its minimum while it shows), while PlusInvExplorerColumns is on, out of the single-folder view, on
//                a tab other than Favorites, the center could be made; left below the staying line.
//                While a split is held, neither changes by the width (the other ways out still go).
//   narrowed     P narrowed (its follows and right margin remembered) while the detail or the columns show;
//                given back when neither does, or when P changes (another tab, the single-folder view).
//   leftGrip / detailGrip  the tree's and the detail's wished widths (PlusInvExplorerLeftWidth /
//                PlusInvExplorerDetailWidth when the window's row is made), read from the dragged edge while a
//                split is held, saved when it is let go or the window hides. A narrow window shows less than the
//                wish (the center first, then the detail, then the tree give way) and keeps the wish.
//   detailCapPx  the detail's shown width, kept as its upper bound when the left split is let go with the detail
//                narrower than its wish (the shape stays as dragged); raised by what a wider row gains, cleared when it
//                reaches the wish, the stage changes, the detail split is held, the detail or the columns go, or the
//                window hides.

#include "llviewerprecompiledheaders.h"

#include "invlayout.h"

#include <unordered_map>

#include "invcolumns.h"
#include "invexplorer.h"

#include "llfloater.h"
#include "llfocusmgr.h"
#include "llfolderview.h"
#include "llinventorypanel.h"
#include "lllayoutstack.h"
#include "llpanelmaininventory.h"
#include "llresizebar.h"
#include "lltabcontainer.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

namespace
{
    constexpr char SETTING_COLUMNS_ON[]    = "PlusInvExplorerColumns";
    constexpr char SETTING_COLUMNS_WIDTH[] = "PlusInvExplorerColumnsWidth";
    constexpr char SETTING_LEFT[]          = "PlusInvExplorerLeftWidth";
    constexpr char SETTING_DETAIL_ON[]     = "PlusInvExplorerDetail";
    constexpr char SETTING_DETAIL_FROM[]   = "PlusInvExplorerMinWidth";
    constexpr char SETTING_DETAIL_WIDTH[]  = "PlusInvExplorerDetailWidth";
    constexpr char WINDOW_NAME[]   = "floater_my_inventory";          // floater_my_inventory.xml
    constexpr char TABS_NAME[]     = "inventory filter tabs";         // panel_main_inventory.xml
    constexpr char COMBINATION_NAME[] = "combination_view_inventory"; // panel_main_inventory.xml (single-folder view)
    constexpr char CENTER_FILE[]   = "panel_plus_inv_center.xml";
    constexpr char LIST_NAME[]     = "plus_inv_center_list";
    constexpr char LEFT_SPLIT_NAME[]   = "plus_inv_split";
    constexpr char DETAIL_SPLIT_NAME[] = "plus_inv_detail_split";
    constexpr char SPLIT_LINE_NAME[]   = "plus_inv_split_line";
    constexpr char SPLIT_LINE_COLOR[]  = "PsDividerRight";            // a little lighter than the window's ground
    constexpr char SEARCH_ROW_NAME[]   = "top_stack";                 // panel_main_inventory.xml, a child of the owner
    constexpr char TOGGLE_FILE[]       = "panel_plus_inv_detail_toggle.xml";
    // The Favorites tab lists favorites gathered from anywhere at its top, not a folder tree: the items at its
    // top have no folder for the center column to open, so that tab gets the detail but not three columns.
    constexpr char FAVORITES_TAB[] = "Favorites";

    // The columns' widths (research/318 6.10 and 7.10, decided 2026-11-11 and 2026-11-12). A wider window widens
    // the center only (the tree in tree | detail); a narrower one takes from the center, then the detail, then the
    // tree.
    constexpr S32 LEFT_MIN_PX       = 160;   // the tree of folders only (three columns)
    constexpr S32 LEFT_DEFAULT_PX   = 260;
    constexpr S32 TREE_ITEMS_MIN_PX = 240;   // the tree with its items (tree | detail)
    constexpr S32 CENTER_MIN_PX     = 240;
    constexpr S32 DETAIL_MIN_PX     = 200;
    constexpr S32 DETAIL_DEFAULT_PX = 260;
    constexpr S32 SPLIT_PX          = 6;     // a split bar between two columns
    constexpr S32 SPLIT_LINE_PX     = 1;     // the line drawn in its middle

    // Each part goes only this far below the width it came at: no flicker while resizing at the edge.
    constexpr S32 LEAVE_MARGIN_PX  = 40;
    // The lowest width PlusInvExplorerColumnsWidth may ask for: what three columns need with the detail pane at its
    // minimum (the lines never count a wider wish: a saved width only shows narrower when it does not fit) and the
    // margins around the contents in today's XML (default_inventory_panel 3 + 3, the tab container's 3 + 2), plus
    // LEAVE_MARGIN_PX, so that leaving below it still has the room. The lines measure the real rectangles.
    constexpr S32 XML_MARGINS_PX   = 11;
    constexpr S32 COLUMNS_DEFAULT_PX = 860;
    constexpr S32 COLUMNS_MIN_PX   = XML_MARGINS_PX + LEFT_MIN_PX + SPLIT_PX + CENTER_MIN_PX + SPLIT_PX + DETAIL_MIN_PX
                                     + LEAVE_MARGIN_PX;
    constexpr S32 DETAIL_FROM_DEFAULT_PX = 640;
    constexpr S32 WIDTH_MAX_PX     = 8192;
    static_assert(COLUMNS_MIN_PX <= COLUMNS_DEFAULT_PX, "the default must be a width the setting may ask for");

    // A wished width kept in a setting: read when a window's row is made, written when its split is let go.
    struct WidthSetting
    {
        const char* name;
        S32 minPx;
        S32 defaultPx;
    };
    constexpr WidthSetting LEFT_WIDTH   { SETTING_LEFT, LEFT_MIN_PX, LEFT_DEFAULT_PX };
    constexpr WidthSetting DETAIL_WIDTH { SETTING_DETAIL_WIDTH, DETAIL_MIN_PX, DETAIL_DEFAULT_PX };

    LLCachedControl<S32>& cachedWidth(const WidthSetting& setting)
    {
        static LLCachedControl<S32> left_width(gSavedSettings, LEFT_WIDTH.name, LEFT_WIDTH.defaultPx);
        static LLCachedControl<S32> detail_width(gSavedSettings, DETAIL_WIDTH.name, DETAIL_WIDTH.defaultPx);
        return &setting == &LEFT_WIDTH ? left_width : detail_width;
    }

    S32 readWidth(const WidthSetting& setting)
    {
        return llclamp(static_cast<S32>(cachedWidth(setting)), setting.minPx, WIDTH_MAX_PX);
    }

    // A split's grip: the column's wished width (the window may show less) and whether the split was held at the
    // last place() (let go: the wish is saved to the setting).
    struct Grip
    {
        const WidthSetting* setting;
        S32  wishPx = setting->defaultPx;
        bool held   = false;
    };

    // Whether a split is held now (place(), every frame while it shows; the ways out, which let it go). When it was
    // held and is not any more, the wish is kept for the next windows and the next session: only then (never while
    // dragging, never every frame), and only when it changed.
    void noteHeld(Grip& grip, bool held_now)
    {
        if (grip.held && !held_now)
        {
            const WidthSetting& setting = *grip.setting;
            const S32 width_px = llclamp(grip.wishPx, setting.minPx, WIDTH_MAX_PX);
            if (width_px != cachedWidth(setting))
            {
                gSavedSettings.setS32(setting.name, width_px);
            }
        }
        grip.held = held_now;
    }

    struct Row
    {
        bool ownWindow = false;            // the window is floater_my_inventory (another floater gets nothing)
        LLHandle<LLTabContainer> tabs;     // the inventory's tab container (TABS_NAME)
        LLHandle<LLPanel> combination;     // the single-folder view's contents (COMBINATION_NAME)
        plus::invexplorer::LookupGate lookup;
        LLHandle<LLPanel> checkedTab;      // the tab tabContents() looked at last
        bool tabUsable  = false;           // an inventory view in the tab container
        bool tabColumns = false;           // and not Favorites

        LLHandle<LLPanel> box;             // the center column (CENTER_FILE), a child of the tab container's parent
        LLHandle<LLResizeBar> leftSplit;   // the split between the tree and the center (a sibling of the box)
        bool centerFailed = false;         // the center could not be made: this window gets no three columns

        LLHandle<LLPanel> pane;            // the detail pane (plus::invexplorer::ensurePane), a child of the owner
        LLHandle<LLResizeBar> detailSplit; // the split before it (a sibling of the pane)
        bool paneFailed  = false;          // the pane could not be made: this window gets no detail
        bool detailRoom  = false;          // the window is wide enough for the detail, whatever the setting says
        bool detailShown = false;

        LLHandle<LLPanel> toggle;          // the detail's button (TOGGLE_FILE), at the end of the search row's stack
        bool toggleTried = false;          // made once (or could not be): never looked for again

        LLHandle<LLPanel> narrowed;        // P narrowed (null: none)
        U32  narrowedFollows  = 0;         // its follows before (given back with its width)
        S32  narrowedMarginPx = 0;         // its right margin in its container before

        Grip leftGrip   { &LEFT_WIDTH };   // the tree's wished width and its split's hold
        Grip detailGrip { &DETAIL_WIDTH }; // the detail's
        S32  detailCapPx = 0;              // the detail shown no wider than this since the left split was let go (0: none)
        S32  capRoomPx   = 0;              // the row's width at the last place() (a wider row raises the cap by the gain)

        boost::signals2::scoped_connection hiddenConnection;   // the window hides: a held split's width is saved
    };

    std::unordered_map<const LLPanelMainInventory*, Row> sRows;

    Row* find(const LLPanelMainInventory* owner)
    {
        auto it = sRows.find(owner);
        return it == sRows.end() ? nullptr : &it->second;
    }

    void clearDetailCap(Row& r)
    {
        r.detailCapPx = 0;
    }

    // The window hides (closed: reuse_instance keeps it): a split held then is let go by the window
    // (LLFloater::setVisible, right after this signal), so its width is saved here, also when the window is not
    // opened again before the viewer ends.
    void onWindowHidden(const LLHandle<LLPanel>& owner_handle)
    {
        const LLPanelMainInventory* owner = dynamic_cast<const LLPanelMainInventory*>(owner_handle.get());
        if (Row* r = owner ? find(owner) : nullptr)
        {
            noteHeld(r->leftGrip, false);
            noteHeld(r->detailGrip, false);
            clearDetailCap(*r);
        }
    }

    // The window's row: made at its first tick, with the widths last set.
    Row& entryFor(LLPanelMainInventory* owner, LLFloater* floater)
    {
        auto [it, made] = sRows.try_emplace(owner);
        Row& r = it->second;
        if (made)
        {
            r.ownWindow    = floater->getName() == WINDOW_NAME;
            r.leftGrip.wishPx   = readWidth(LEFT_WIDTH);
            r.detailGrip.wishPx = readWidth(DETAIL_WIDTH);
            const LLHandle<LLPanel> owner_handle = owner->getHandle();
            r.hiddenConnection = floater->setVisibleCallback([owner_handle](LLUICtrl*, const LLSD& visible)
            {
                if (!visible.asBoolean())
                {
                    onWindowHidden(owner_handle);
                }
            });
        }
        return r;
    }

    // The window's parts, found once and remembered by handle (looked for again every LookupGate::RETRY_FRAMES
    // while missing): the inventory's tab container and the single-folder view's contents. Null while one is missing.
    LLTabContainer* partsFor(Row& r, LLPanelMainInventory* owner)
    {
        LLTabContainer* tabs = r.tabs.get();
        if (tabs && r.combination.get())
        {
            return tabs;
        }
        if (!r.lookup.due())
        {
            return nullptr;
        }
        tabs = owner->findChild<LLTabContainer>(TABS_NAME, true);
        LLPanel* combination = owner->findChild<LLPanel>(COMBINATION_NAME, false);
        r.tabs        = tabs ? tabs->getDerivedHandle<LLTabContainer>() : LLHandle<LLTabContainer>();
        r.combination = combination ? combination->getHandle() : LLHandle<LLPanel>();
        return (tabs && combination) ? tabs : nullptr;
    }

    // The tab shown, when its contents can be the row: the tab container's current panel, when it is an inventory
    // view, the same as the window's active panel (getActivePanel() casts without checking) and a child of the tab
    // container (its rectangle is the one narrowed). Null otherwise. `columns`: and it may have three columns (not
    // Favorites). The custom tabs' button tabs ("+", the hidden tabs' arrow) are never current (invcustomtabs
    // validates the selection).
    LLInventoryPanel* tabContents(Row& r, LLPanelMainInventory* owner, LLTabContainer* tabs, bool& columns)
    {
        columns = false;
        LLPanel* current = tabs->getCurrentPanel();
        if (!current)
        {
            return nullptr;
        }
        if (r.checkedTab.get() != current)
        {
            r.checkedTab = current->getHandle();
            r.tabUsable  = dynamic_cast<LLInventoryPanel*>(current) && current->getParent() == tabs;
            r.tabColumns = r.tabUsable && current->getName() != FAVORITES_TAB;
        }
        LLInventoryPanel* tab = r.tabUsable ? static_cast<LLInventoryPanel*>(current) : nullptr;   // (checked above)
        if (!tab || tab != owner->getActivePanel())
        {
            return nullptr;
        }
        columns = r.tabColumns;
        return tab;
    }

    // Where a view's own coordinates start in the owner's (its ancestors' origins added up to the owner). False when
    // the view is not inside the owner.
    bool originIn(const LLView* view, const LLView* owner, S32& x, S32& y)
    {
        x = 0;
        y = 0;
        for (const LLView* v = view; v != owner; v = v->getParent())
        {
            if (!v)
            {
                return false;
            }
            x += v->getRect().mLeft;
            y += v->getRect().mBottom;
        }
        return true;
    }

    // P's right margin in its container: remembered while it is narrowed, else measured.
    S32 rightMarginPx(const Row& r, const LLPanel* contents)
    {
        if (r.narrowed.get() == contents)
        {
            return r.narrowedMarginPx;
        }
        const LLView* container = contents->getParent();
        return container ? container->getRect().getWidth() - contents->getRect().mRight : 0;
    }

    // The row's area, in the owner's coordinates. Its left, top and bottom are P's, as its container placed it. Its
    // right is the owner's right less the margin of the tab view (the tab container's margin in the owner and the
    // tab's in the tab container, measured before narrowing), in both views, so that the detail pane's right edge
    // stays where it is when the single-folder view comes or goes. `outer`: the window around the owner.
    struct Area
    {
        S32 left   = 0;
        S32 right  = 0;
        S32 top    = 0;
        S32 bottom = 0;
        S32 outer  = 0;
        S32 ownerPx = 0;
        S32 width() const  { return right - left; }
        S32 height() const { return top - bottom; }
        // The window's width at which the area is this wide (what is around it does not change with the row).
        S32 windowFor(S32 row_px) const { return outer + ownerPx - width() + row_px; }
    };

    bool measure(const Row& r, const LLPanelMainInventory* owner, const LLFloater* floater, LLTabContainer* tabs,
                 const LLPanel* contents, Area& area)
    {
        S32 x = 0;
        S32 y = 0;
        S32 tabs_x = 0;
        S32 tabs_y = 0;
        if (!originIn(contents->getParent(), owner, x, y) || !originIn(tabs->getParent(), owner, tabs_x, tabs_y))
        {
            return false;
        }
        const LLPanel* tab = tabs->getCurrentPanel();
        const S32 tab_margin_px = (tab && tab->getParent() == tabs) ? rightMarginPx(r, tab) : 0;
        const S32 owner_px      = owner->getRect().getWidth();
        const S32 inset_px      = owner_px - (tabs_x + tabs->getRect().mRight) + tab_margin_px;
        const LLRect& rect = contents->getRect();
        area.left   = x + rect.mLeft;
        area.right  = owner_px - inset_px;
        area.top    = y + rect.mTop;
        area.bottom = y + rect.mBottom;
        area.outer  = floater->getRect().getWidth() - owner_px;
        area.ownerPx = owner_px;
        return true;
    }

    // A part showing from `from_px` (raised to what it needs plus LEAVE_MARGIN_PX) and staying down to
    // LEAVE_MARGIN_PX below that (not below what it needs): the window is wide enough for it now.
    bool wideEnough(S32 window_px, S32 from_px, S32 need_px, bool shown)
    {
        const S32 line_px = shown ? llmax(from_px - LEAVE_MARGIN_PX, need_px) : llmax(from_px, need_px + LEAVE_MARGIN_PX);
        return window_px >= line_px;
    }

    // A split bar with a line: a panel 1 px wide in the middle of the bar, drawn by the bar (it takes no mouse: the
    // bar handles the mouse itself), as the official layout stack's drag handle (LLLayoutStack::createResizeBar). It
    // moves the left edge of `resizing` (its right edge stays), keeping it at least `min_px` wide.
    LLResizeBar* makeSplit(LLView* resizing, S32 min_px, const char* name)
    {
        LLResizeBar::Params bar_params;
        bar_params.name(name);
        bar_params.rect(LLRect(0, resizing->getRect().getHeight(), SPLIT_PX, 0));
        bar_params.resizing_view(resizing);
        bar_params.side(LLResizeBar::LEFT);
        bar_params.min_size(min_px);
        bar_params.snapping_enabled(false);
        bar_params.allow_double_click_snapping(false);
        bar_params.visible(false);
        LLResizeBar* bar = LLUICtrlFactory::create<LLResizeBar>(bar_params);
        if (!bar)
        {
            return nullptr;
        }
        LLPanel::Params line_params;
        line_params.name(SPLIT_LINE_NAME);
        const S32 line_left = (SPLIT_PX - SPLIT_LINE_PX) / 2;
        line_params.rect(LLRect(line_left, bar->getRect().getHeight(), line_left + SPLIT_LINE_PX, 0));
        line_params.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP | FOLLOWS_BOTTOM;
        line_params.mouse_opaque(false);
        line_params.tab_stop(false);
        line_params.background_visible(true);
        line_params.bg_alpha_color(LLUIColorTable::instance().getColor(SPLIT_LINE_COLOR));
        if (LLPanel* line = LLUICtrlFactory::create<LLPanel>(line_params))
        {
            bar->addChild(line);
        }
        return bar;
    }

    bool isHeld(const LLHandle<LLResizeBar>& split)
    {
        LLResizeBar* bar = split.get();
        return bar && bar->hasMouseCapture();
    }

    // A split held while its column goes (a setting turned off, the single-folder view...) is let go: a hidden bar
    // holding the mouse would keep moving a hidden column until the button comes up.
    void letGo(const LLHandle<LLResizeBar>& split)
    {
        if (isHeld(split))
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
    }

    // The center column of this window: made the first time the window shows three columns (a window never in three
    // columns never makes it), as a child of the tab container's parent, with the split bar next to it. Its list is
    // handed to invcolumns, which connects its callbacks once. Both stay hidden until the columns come and die with
    // the window. When it cannot be made, what was made is taken away again (nothing is connected yet, and this runs
    // before the window draws its children), and the window gets no three columns, without trying again.
    bool ensureCenter(Row& r, LLPanelMainInventory* owner, LLTabContainer* tabs)
    {
        if (r.box.get() && r.leftSplit.get())
        {
            return true;
        }
        if (r.centerFailed)
        {
            return false;
        }
        LLView* parent = tabs->getParent();
        LLPanel* box = parent
            ? LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(CENTER_FILE, parent, LLPanel::child_registry_t::instance())
            : nullptr;
        LLInventorySingleFolderPanel* list = box ? box->findChild<LLInventorySingleFolderPanel>(LIST_NAME, true) : nullptr;
        LLResizeBar* split = list ? makeSplit(box, CENTER_MIN_PX, LEFT_SPLIT_NAME) : nullptr;
        if (!split)
        {
            if (box)
            {
                parent->removeChild(box);
                delete box;
            }
            r.centerFailed = true;
            LL_WARNS_ONCE("InvColumns") << "The inventory's center column could not be made (" << CENTER_FILE
                                        << "): the window gets no three columns" << LL_ENDL;
            return false;
        }
        parent->addChild(split);   // after the box: in front of it and of the tab container
        r.box       = box->getHandle();
        r.leftSplit = split->getDerivedHandle<LLResizeBar>();
        plus::invcolumns::connectCenter(owner, list);
        return true;
    }

    // The detail pane of this window: made the first time it shows (invexplorer.cpp makes it, a child of the owner;
    // showDetail gives it the selection), with the split bar before it. When the pane cannot be made, nothing is
    // left; when its split cannot, the pane stays hidden (a hidden pane holds no picture). Either way the window
    // gets no detail, without trying again.
    bool ensureDetail(Row& r, LLPanelMainInventory* owner, LLFloater* floater)
    {
        if (r.pane.get() && r.detailSplit.get())
        {
            return true;
        }
        if (r.paneFailed)
        {
            return false;
        }
        LLPanel* pane = plus::invexplorer::ensurePane(owner, floater);
        LLResizeBar* split = pane ? makeSplit(pane, DETAIL_MIN_PX, DETAIL_SPLIT_NAME) : nullptr;
        if (!split)
        {
            r.paneFailed = true;
            LL_WARNS_ONCE("InvColumns") << "The inventory's detail pane could not be made: the window gets none" << LL_ENDL;
            return false;
        }
        owner->addChild(split);   // after the pane: in front of it and of the contents
        r.pane        = pane->getHandle();
        r.detailSplit = split->getDerivedHandle<LLResizeBar>();
        return true;
    }

    void showCenter(const Row& r, bool shown)
    {
        if (LLPanel* box = r.box.get())
        {
            box->setVisible(shown);
        }
        if (LLResizeBar* split = r.leftSplit.get())
        {
            split->setVisible(shown);
        }
    }

    // The detail pane hides with the keyboard focus in it (its text, its buttons): the focus goes to the view the
    // gear menu acts on (invcolumns::commandPanel: the center when it was chosen last, else the tab shown, in the
    // single-folder view its list; with the focus in the pane it only reads), not wherever the official
    // LLViewerWindow::updateKeyboardFocus would put it. Not when that view is hidden (the single-folder view showing
    // its gallery only) or is a single-folder list with nothing selected (its onFocusReceived would select its first
    // row, which nobody chose): the focus stays in the window then, on the inventory view itself (only the pane's
    // branch loses it, the window and the inventory view keep it and receive nothing), and the gear menu still acts
    // on the last choice (chosenCenter).
    void focusFromPane(LLPanelMainInventory* owner)
    {
        if (!plus::invexplorer::paneHasFocus(owner))
        {
            return;
        }
        LLInventoryPanel* view = plus::invcolumns::commandPanel(owner);
        LLFolderView* root = view ? view->getRootFolder() : nullptr;
        const bool selects_first = dynamic_cast<LLInventorySingleFolderPanel*>(view)
                                   && (!root || root->getSelectedItems().empty());
        if (view && view->isInVisibleChain() && !selects_first)
        {
            view->setFocus(true);
        }
        else
        {
            gFocusMgr.setKeyboardFocus(owner);
        }
    }

    // Called from tick() only (the window draws: the pane's visibility notice comes). A hidden pane is not told of
    // the selection (invexplorer::showInDetail), so it is given the selection of the moment right before it shows,
    // and builds its text once then.
    void showDetail(Row& r, LLPanelMainInventory* owner, bool shown)
    {
        LLPanel* pane = r.pane.get();
        if (!shown)
        {
            letGo(r.detailSplit);
            noteHeld(r.detailGrip, false);
            clearDetailCap(r);
            focusFromPane(owner);
        }
        else if (pane)
        {
            plus::invexplorer::syncPane(owner);
        }
        if (pane)
        {
            pane->setVisible(shown);   // (shown: builds its text; hidden: lets go of its picture, onVisibilityChange)
        }
        if (LLResizeBar* split = r.detailSplit.get())
        {
            split->setVisible(shown);
        }
        r.detailShown = shown;
    }

    // The detail's button (research/318 8.3): a layout panel with a toggle button tied to PlusInvExplorerDetail
    // (control_name: pressing it writes the setting, the setting sets its pressed look), added once at the end of
    // the official search row's stack, hidden. The stack lays it out on its next draw (after this tick, in the same
    // frame) and gives its width back to the search box while it is hidden. When the stack or the file is missing
    // (the official XML changed), the window gets no button, without looking again (the detail still follows the
    // setting). The button takes no keyboard focus (tab_stop="false").
    void ensureToggle(Row& r, LLPanelMainInventory* owner)
    {
        if (r.toggleTried)
        {
            return;
        }
        r.toggleTried = true;
        LLLayoutStack* stack = owner->findChild<LLLayoutStack>(SEARCH_ROW_NAME, false);
        LLLayoutPanel* panel = stack
            ? LLUICtrlFactory::createFromFile<LLLayoutPanel>(TOGGLE_FILE, stack, LLLayoutStack::child_registry_t::instance())
            : nullptr;
        if (!panel)
        {
            LL_WARNS_ONCE("InvColumns") << "The inventory's search row (" << SEARCH_ROW_NAME << ") has no room for the detail's button ("
                                        << TOGGLE_FILE << "): the window gets none" << LL_ENDL;
            return;
        }
        r.toggle = panel->getHandle();
    }

    // Written only when it changes (the stack lays itself out again then).
    void showToggle(const Row& r, bool shown)
    {
        LLPanel* panel = r.toggle.get();
        if (panel && panel->getVisible() != shown)
        {
            panel->setVisible(shown);
        }
    }

    // Narrows P (null: none), giving the one narrowed before its follows and its full width back first. Its follows
    // become left, top and bottom: its container keeps moving it and its height, place() alone sets its width (a
    // window narrowed at once cannot make it negative on the way).
    void setNarrowed(Row& r, LLPanel* contents)
    {
        LLPanel* old = r.narrowed.get();
        if (contents == old)
        {
            return;
        }
        if (old)
        {
            old->setFollows(r.narrowedFollows);
            if (const LLView* container = old->getParent())
            {
                LLRect rect = old->getRect();
                rect.mRight = llmax(rect.mLeft + 1, container->getRect().getWidth() - r.narrowedMarginPx);
                old->setShape(rect);
            }
        }
        r.narrowed = LLHandle<LLPanel>();
        if (contents)
        {
            r.narrowedMarginPx = rightMarginPx(r, contents);   // (measured: not narrowed yet)
            r.narrowedFollows  = contents->getFollows();
            r.narrowed         = contents->getHandle();
            contents->setFollows(FOLLOWS_LEFT | FOLLOWS_TOP | FOLLOWS_BOTTOM);
        }
    }

    // What the row shows besides the tree (nothing: the contents get their width back, setNarrowed).
    enum class Stage
    {
        TreeDetail,      // tree | detail
        Columns,         // tree | center
        ColumnsDetail    // tree | center | detail
    };

    bool hasCenter(Stage stage) { return stage != Stage::TreeDetail; }
    bool hasDetail(Stage stage) { return stage != Stage::Columns; }

    // How the row's width goes to the columns (from the left: the tree, the center, the detail; a split before each
    // but the tree). A row too narrow for the wishes takes from the center (to its minimum), then from the detail,
    // then from the tree. The wishes are not changed.
    struct Widths
    {
        S32 tree   = 0;
        S32 center = 0;
        S32 detail = 0;
    };

    Widths share(S32 room_px, Stage stage, S32 left_wish_px, S32 detail_wish_px)
    {
        Widths w;
        if (stage == Stage::ColumnsDetail)
        {
            const S32 avail_px = room_px - 2 * SPLIT_PX - CENTER_MIN_PX;   // the tree's and the detail's
            w.tree   = left_wish_px;
            w.detail = detail_wish_px;
            if (w.tree + w.detail > avail_px)
            {
                w.detail = llmax(DETAIL_MIN_PX, avail_px - w.tree);
                if (w.tree + w.detail > avail_px)
                {
                    w.tree = llmax(LEFT_MIN_PX, avail_px - w.detail);
                }
            }
            w.center = room_px - 2 * SPLIT_PX - w.tree - w.detail;
        }
        else if (stage == Stage::Columns)
        {
            w.tree   = llmin(left_wish_px, room_px - SPLIT_PX - CENTER_MIN_PX);
            w.center = room_px - SPLIT_PX - w.tree;
        }
        else
        {
            w.detail = llmin(detail_wish_px, room_px - SPLIT_PX - TREE_ITEMS_MIN_PX);
            w.tree   = room_px - SPLIT_PX - w.detail;
        }
        // (a row narrower than its minimums comes only while a split is held, the lines being stopped: no column
        // gets a width below 1)
        w.tree   = llmax(1, w.tree);
        w.center = hasCenter(stage) ? llmax(1, w.center) : 0;
        w.detail = hasDetail(stage) ? llmax(1, w.detail) : 0;
        return w;
    }

    // While the detail or the columns show, every frame after the columns followed the tab: P narrowed to the tree,
    // then the left split and the center (in the coordinates of the tab container's parent, where they are), then
    // the detail split and the pane (in the owner's), below the tab row and above the received items. Only what
    // differs is written (setShape compares). A split's drag moves its column's left edge only: while it is held,
    // its wish is read from there (this runs before the children are drawn, so the dragged shape is never drawn out
    // of place); when it is let go, the wish is saved.
    void place(Row& r, LLPanelMainInventory* owner, LLPanel* contents, const Area& area, Stage stage)
    {
        LLPanel* box            = hasCenter(stage) ? r.box.get() : nullptr;
        LLResizeBar* left_split = hasCenter(stage) ? r.leftSplit.get() : nullptr;
        LLPanel* pane           = hasDetail(stage) ? r.pane.get() : nullptr;
        LLResizeBar* det_split  = hasDetail(stage) ? r.detailSplit.get() : nullptr;
        S32 box_x = 0;
        S32 box_y = 0;
        const bool center_ok = !hasCenter(stage) || (box && left_split && originIn(box->getParent(), owner, box_x, box_y));
        const bool detail_ok = !hasDetail(stage) || (pane && det_split);
        if (!center_ok || !detail_ok)
        {
            setNarrowed(r, nullptr);   // (not expected: they are made before they show and go with the window)
            return;
        }
        setNarrowed(r, contents);

        const bool left_held   = left_split && left_split->hasMouseCapture();
        const bool left_let_go = r.leftGrip.held && !left_held;
        if (left_held)
        {
            r.leftGrip.wishPx = llmax(box_x + box->getRect().mLeft - SPLIT_PX - area.left, LEFT_MIN_PX);
        }
        noteHeld(r.leftGrip, left_held);
        const bool detail_held = pane && det_split->hasMouseCapture();
        if (detail_held)
        {
            r.detailGrip.wishPx = llmax(area.right - pane->getRect().mLeft, DETAIL_MIN_PX);
        }
        noteHeld(r.detailGrip, detail_held);

        // While the left split is dragged, the detail keeps the width it shows (narrower than its wish in a narrow
        // window): the room the drag frees or takes goes to the center next to the split, not to the detail on the
        // far side. When it is let go, that width stays the detail's cap (the shape stays as dragged). A wider row
        // raises the cap by what it gained (the usual rule: the room goes to the detail first, the center keeps its
        // width) and the cap goes once it reaches the wish; a narrower row keeps it (it only bounds the detail from
        // above, the usual rule still narrows it). It also goes when the stage changes, the detail split is held,
        // the detail or the columns go, or the window hides (clearDetailCap).
        const S32 shown_px = pane ? llmax(pane->getRect().getWidth(), DETAIL_MIN_PX) : 0;
        if (r.detailCapPx > 0 && (stage != Stage::ColumnsDetail || detail_held))
        {
            clearDetailCap(r);
        }
        else if (r.detailCapPx > 0 && area.width() > r.capRoomPx)
        {
            // A wider row raises the cap by what it gained: that room goes to the detail first (the usual rule) and
            // the center keeps its width; once the cap reaches the wish, it goes.
            r.detailCapPx += area.width() - r.capRoomPx;
            if (r.detailCapPx >= r.detailGrip.wishPx)
            {
                clearDetailCap(r);
            }
        }
        if (left_let_go && stage == Stage::ColumnsDetail && shown_px < r.detailGrip.wishPx)
        {
            r.detailCapPx = shown_px;
        }
        r.capRoomPx = area.width();
        S32 detail_px = r.detailGrip.wishPx;
        if (left_held && pane)
        {
            detail_px = shown_px;
        }
        else if (r.detailCapPx > 0)
        {
            detail_px = llmin(detail_px, r.detailCapPx);
        }
        const Widths w = share(area.width(), stage, r.leftGrip.wishPx, detail_px);
        LLRect rect = contents->getRect();
        rect.mRight = rect.mLeft + w.tree;
        contents->setShape(rect);
        S32 x = area.left + w.tree;
        if (box)
        {
            left_split->setShape(LLRect(x - box_x, area.top - box_y, x + SPLIT_PX - box_x, area.bottom - box_y));
            x += SPLIT_PX;
            box->setShape(LLRect(x - box_x, area.top - box_y, x + w.center - box_x, area.bottom - box_y));
            x += w.center;
            left_split->setResizeLimits(CENTER_MIN_PX, w.center + w.tree - LEFT_MIN_PX);
        }
        if (pane)
        {
            det_split->setShape(LLRect(x, area.top, x + SPLIT_PX, area.bottom));
            x += SPLIT_PX;
            pane->setShape(LLRect(x, area.top, area.right, area.bottom));
            const S32 given_px = box ? w.center - CENTER_MIN_PX : w.tree - TREE_ITEMS_MIN_PX;   // what a drag may take
            det_split->setResizeLimits(DETAIL_MIN_PX, w.detail + given_px);
        }
    }

    // Out of three columns: what the columns mean first (invcolumns), then the left split let go (its width kept)
    // and the center hidden. P stays narrowed while the detail shows (place, or the caller, gives it back).
    void leaveColumns(Row& r, LLPanelMainInventory* owner)
    {
        plus::invcolumns::leave(owner);
        letGo(r.leftSplit);
        noteHeld(r.leftGrip, false);
        clearDetailCap(r);
        showCenter(r, false);
    }

    // Back to one column: the columns, the detail and its button go, P gets its width back.
    void leaveRow(Row& r, LLPanelMainInventory* owner)
    {
        if (plus::invcolumns::isActive(owner))
        {
            leaveColumns(r, owner);
        }
        if (r.detailShown)
        {
            showDetail(r, owner, false);
        }
        showToggle(r, false);
        setNarrowed(r, nullptr);
    }
}

namespace plus
{
namespace invlayout
{
    void tick(LLPanelMainInventory* owner)
    {
        static LLCachedControl<bool> columns_on(gSavedSettings, SETTING_COLUMNS_ON, true);
        static LLCachedControl<S32>  columns_from(gSavedSettings, SETTING_COLUMNS_WIDTH, COLUMNS_DEFAULT_PX);
        static LLCachedControl<bool> detail_on(gSavedSettings, SETTING_DETAIL_ON, true);
        static LLCachedControl<S32>  detail_from(gSavedSettings, SETTING_DETAIL_FROM, DETAIL_FROM_DEFAULT_PX);
        LLFloater* floater = owner->getParentByType<LLFloater>();
        if (!floater)
        {
            if (Row* r = find(owner))
            {
                leaveRow(*r, owner);
            }
            return;
        }
        Row& r = entryFor(owner, floater);
        if (!r.ownWindow)
        {
            return;
        }
        // The detail's button is there to turn the detail on: with both settings off, only it needs the row measured.
        ensureToggle(r, owner);
        if (!columns_on && !detail_on && !r.toggle.get())
        {
            leaveRow(r, owner);
            return;
        }
        LLTabContainer* tabs = partsFor(r, owner);
        LLInventoryPanel* tab = nullptr;
        bool tab_columns = false;
        LLPanel* contents = nullptr;
        if (tabs)
        {
            if (owner->isSingleFolderMode())
            {
                contents = r.combination.get();
            }
            else
            {
                tab      = tabContents(r, owner, tabs, tab_columns);
                contents = tab;
            }
        }
        Area area;
        if (!contents || !measure(r, owner, floater, tabs, contents, area))
        {
            leaveRow(r, owner);
            return;
        }
        const S32 window_px = floater->getRect().getWidth();
        // While a split is held, the width neither shows nor hides a part (the dragged layout stays what it was).
        const bool held   = isHeld(r.leftSplit) || isHeld(r.detailSplit);
        const bool active = plus::invcolumns::isActive(owner);

        // The detail first: it does not depend on the columns (they count it at its minimum while it shows). The
        // room for it is the width alone (the setting and its button are not part of it), the detail and the button
        // follow from the room.
        if (!held)
        {
            const S32 from_px = llmin(static_cast<S32>(detail_from), WIDTH_MAX_PX);
            const S32 need_px = area.windowFor(TREE_ITEMS_MIN_PX + SPLIT_PX + DETAIL_MIN_PX);
            r.detailRoom = wideEnough(window_px, from_px, need_px, r.detailRoom);
        }
        const bool detail_room = r.detailRoom && area.height() >= plus::invexplorer::PANE_MIN_HEIGHT_PX;
        bool detail = detail_room && detail_on && !r.paneFailed;
        if (detail && held)
        {
            detail = r.detailShown;   // (the setting turned on while a split is held: after it is let go)
        }
        if (detail && !ensureDetail(r, owner, floater))
        {
            detail = false;
        }
        // The button shows exactly when turning the setting on would show the detail (a window that never made its
        // pane cannot know paneFailed yet: it shows the button, and hides it if the pane cannot be made then).
        showToggle(r, detail_room && !r.paneFailed);

        bool columns = columns_on && tab_columns && !r.centerFailed;
        if (columns && held)
        {
            columns = active;
        }
        else if (columns)
        {
            const S32 from_px = llclamp(static_cast<S32>(columns_from), COLUMNS_MIN_PX, WIDTH_MAX_PX);
            const S32 need_px = area.windowFor(LEFT_MIN_PX + SPLIT_PX + CENTER_MIN_PX
                                               + (detail ? SPLIT_PX + DETAIL_MIN_PX : 0));
            columns = wideEnough(window_px, from_px, need_px, active);
        }

        if (active && !columns)
        {
            leaveColumns(r, owner);
        }
        else if (!active && columns)
        {
            // The center is made when the window enters for the first time.
            columns = ensureCenter(r, owner, tabs) && plus::invcolumns::enter(owner, tab);
            if (columns)
            {
                showCenter(r, true);
            }
        }
        else if (active && !plus::invcolumns::follow(owner, tab))
        {
            leaveColumns(r, owner);   // (not expected: the center is the window's own child and goes with it)
            columns = false;
        }
        if (detail != r.detailShown)
        {
            showDetail(r, owner, detail);
        }
        if (!columns && !detail)
        {
            setNarrowed(r, nullptr);
            return;
        }
        const Stage stage = !columns ? Stage::TreeDetail : (detail ? Stage::ColumnsDetail : Stage::Columns);
        place(r, owner, contents, area, stage);   // before anything is drawn
    }

    void forget(const LLPanelMainInventory* owner)
    {
        sRows.erase(owner);   // the center, the pane, the splits and the narrowed contents go with the window
    }
}
}
