/**
 * @file pstabs.h
 * @brief plus_ps_tabs: the official tab container with its tabs drawn in the Charis style (research/266
 *        prefs-frame): on the left a rail of one-line rows, on top underlined tabs (as plus_ps_tab).
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

// What stays the official LLTabContainer's: the pages (each <panel> child is a tab, its label the tab's
// name), selectTab() and the commit on a change, getCurrentPanelIndex() (LastPrefTab is an index),
// setTabVisibility() (the preferences' search hides the tabs without a match and opens the first one
// with a match, also inside a page: a plus_ps_tabs in a page is found by it as any tab container), and
// the C++ that asks for an LLTabContainer by name (getChild<LLTabContainer>("pref core")).
// What is this class's: the tabs are drawn and clicked here (the official tab buttons are hidden:
// hide_tabs), the pages are placed beside the rail (tab_position left, tab_width wide; the rows are
// TABS_ROW_HEIGHT high and shrink down to TABS_ROW_MIN in a short window, then the rail scrolls with the
// wheel) or under the strip (tab_position top, tab_height high; too many tabs scroll sideways). A page
// that is hidden keeps no keyboard focus (it goes to the tabs: releaseFocusIn()). A page's tool_tip is its
// tab's tip (taken off the page, so that it is not shown over the page itself).
//
// The search: with search_editor (the name of an LLSearchEditor in the same floater, the preferences'
// search_prefs_edit), the tab containers inside the pages get all their tabs back when the search is emptied.
// The official search does not do that for a tab container inside a page (PanelData::hightlightAndHide()
// returns before going into the pages when the text is empty), so the sub-tabs it hid stayed hidden until a
// restart. A plus_ps_tabs inside then shows the tab it showed before the search again; an official one
// shows its first.
//
// Keys: a click on a tab gives the tabs the keyboard focus; then Up / Down (left) or Left / Right (top)
// pick the next tab, Right (left) or Down (top) goes into the page, and Alt+Left / Alt+Right work from inside
// the page too (all the official tab container's keys).

#ifndef PLUS_PSTABS_H
#define PLUS_PSTABS_H

#include "lltabcontainer.h"

#include <functional>
#include <string>
#include <vector>

class LLFontGL;

namespace plus::psui
{
    class PsTabs : public LLTabContainer
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLTabContainer::Params>
        {
            Optional<S32> row_height;   // tab_position left: a row's height while there is room (TABS_ROW_HEIGHT)
            Optional<std::string> search_editor;    // the floater's search box whose emptying shows the inner tabs again
            Params();
        };

        bool postBuild() override;
        bool addChild(LLView* view, S32 tab_group = 0) override;
        void draw() override;
        void setFocus(bool b) override;
        bool handleMouseDown(S32 x, S32 y, MASK mask) override;
        bool handleMouseUp(S32 x, S32 y, MASK mask) override;
        bool handleHover(S32 x, S32 y, MASK mask) override;
        bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
        bool handleToolTip(S32 x, S32 y, MASK mask) override;
        bool handleKeyHere(KEY key, MASK mask) override;
        void onMouseLeave(S32 x, S32 y, MASK mask) override;
        void onMouseCaptureLost() override;

        // A tab picked by the user (a click, the keys): the index of its page. Not called when code selects a
        // tab (selectTab(), the search). For remembering the tab picked.
        void setPickedCallback(std::function<void(S32 index)> picked) { mOnPicked = std::move(picked); }
        // The page shown changed, by anyone (the user, selectTab(), the search): the index of the page now shown.
        void setShownCallback(std::function<void(S32 index)> shown) { mOnShown = std::move(shown); }
        // The index of the page view is on (view itself or one of its ancestors), or -1.
        S32 pageOf(const LLView* view) const;
        LLHandle<PsTabs> getTabsHandle() const { return getDerivedHandle<PsTabs>(); }
        // Every tab shown again (the ones the search hid) and the tab shown before the search chosen again.
        void showAllTabs();

    protected:
        friend class LLUICtrlFactory;
        explicit PsTabs(const Params& p);

    private:
        struct Tab
        {
            LLHandle<LLPanel> panel;
            LLWString   label;
            std::string tip;
            S32         textWidth = 0;
            bool        visible = true;
            LLRectf     rect;               // where it is drawn (local; empty while hidden)
        };

        // The tabs as the container has them now (made again only when their number changed).
        void readTabs();
        // The official tab buttons and scroll arrows are not drawn or clicked here.
        void hideOfficialButtons();
        // A page's place: beside the rail or under the strip.
        LLRect pageRect() const;
        // The rectangle the tabs are in (the rail, or the strip), and whether (x, y) is in it.
        LLRectf tabArea() const;
        bool inTabArea(S32 x, S32 y) const { return tabArea().pointInRect(F32(x), F32(y)); }
        // Each tab's rectangle from the size and the tabs shown (cheap: no allocation).
        void layOut();
        void scrollTo(S32 index);
        S32  tabAt(S32 x, S32 y) const;
        // The page shown changed (by anyone): the focus out of the hidden pages, the tab into view, the callback.
        void syncShown();
        // The search box was emptied: the tab containers inside the pages show all their tabs again.
        void watchSearch();
        // The official selectNextTab() / selectPrevTab() give the focus to a hidden tab button: it comes back here.
        void keepFocusOffHiddenButtons();
        // A tab picked by the user.
        void pick(S32 index);
        void drawTab(const Tab& tab, S32 index, F32 alpha) const;
        void drawRailRow(const Tab& tab, const LLRectf& r, bool chosen, bool pressed, bool hovered, F32 alpha) const;

        bool mLeft;
        S32  mRailWidth;
        S32  mRowHeight;
        S32  mStripHeight;
        const LLFontGL* mFont;
        std::vector<Tab> mTabs;
        S32  mScroll = 0;           // pixels the tabs are moved up (left) or to the left (top)
        S32  mMaxScroll = 0;
        S32  mStep = 0;             // a wheel click's pixels
        S32  mShown = -1;           // the page syncShown() saw last
        S32  mSettled = -1;         // the page shown last while no tab was hidden (before a search)
        std::string mSearchName;
        LLHandle<LLView> mSearch;   // the search box (found once)
        bool mSearchLooked = false;
        bool mSearching = false;    // the search box had text at the last draw
        S32  mHoverTab = -1;
        S32  mPressedTab = -1;
        std::function<void(S32)> mOnPicked;
        std::function<void(S32)> mOnShown;
    };
}

#endif // PLUS_PSTABS_H
