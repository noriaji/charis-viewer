/**
 * @file pstabs.cpp
 * @brief plus_ps_tabs (pstabs.h): the official tab container with its tabs drawn in the Charis style.
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

#include "pstabs.h"
#include "psstyle.h"
#include "pswidgets.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "llsearcheditor.h"
#include "lltooltip.h"
#include "lluictrlfactory.h"
#include "llwindow.h"

namespace plus::psui
{
    namespace
    {
        LLDefaultChildRegistry::Register<PsTabs> sTabsRegistration("plus_ps_tabs");

        LLRect wholePixels(const LLRectf& r)
        {
            return LLRect(S32(r.mLeft), S32(r.mTop), S32(r.mRight), S32(r.mBottom));
        }

        // Every tab of an official tab container shown again (setTabVisibility() also shows the container).
        void showTabsOf(LLTabContainer& tabs)
        {
            for (S32 i = 0; i < tabs.getTabCount(); ++i)
            {
                LLPanel* page = tabs.getPanelByIndex(i);
                if (page && !tabs.getTabVisibility(page))
                {
                    tabs.setTabVisibility(page, true);
                }
            }
        }

        // The tab containers in view (at any depth) show all their tabs again.
        void showTabsIn(LLView* view)
        {
            for (LLView* child : *view->getChildList())
            {
                if (PsTabs* ps = dynamic_cast<PsTabs*>(child))
                {
                    ps->showAllTabs();
                }
                else if (LLTabContainer* tabs = dynamic_cast<LLTabContainer*>(child))
                {
                    showTabsOf(*tabs);
                }
                showTabsIn(child);
            }
        }
    }

    PsTabs::Params::Params()
    :   row_height("row_height", style::TABS_ROW_HEIGHT),
        search_editor("search_editor")
    {
        hide_tabs = true;       // the tabs are drawn and clicked here
    }

    PsTabs::PsTabs(const Params& p)
    :   LLTabContainer(p),
        mLeft(p.tab_position() == LLTabContainer::LEFT),
        mRailWidth(p.tab_width.isProvided() ? p.tab_width() : style::TABS_RAIL_WIDTH),
        mRowHeight(llmax(1, p.row_height())),
        mStripHeight(p.tab_height.isProvided() ? p.tab_height() : style::TABS_TOP_HEIGHT),
        mFont(font(style::FONT_BODY)),
        mSearchName(p.search_editor())
    {
    }

    bool PsTabs::postBuild()
    {
        const bool built = LLTabContainer::postBuild();
        hideOfficialButtons();
        readTabs();
        // Every change of the page shown commits (selectTab()); the first draw also looks, for one that did not.
        setCommitCallback([this](LLUICtrl*, const LLSD&) { syncShown(); });
        syncShown();
        return built;
    }

    bool PsTabs::addChild(LLView* view, S32 tab_group)
    {
        const bool added = LLTabContainer::addChild(view, tab_group);
        LLPanel* page = dynamic_cast<LLPanel*>(view);
        if (page && page->getParent() == this)
        {
            // The official container placed it as if there were no tabs (hide_tabs): beside the rail or under the strip.
            const LLRect r = pageRect();
            page->translate(r.mLeft - page->getRect().mLeft, r.mBottom - page->getRect().mBottom);
            page->reshape(r.getWidth(), r.getHeight(), true);
        }
        hideOfficialButtons();
        readTabs();
        return added;
    }

    void PsTabs::hideOfficialButtons()
    {
        for (LLView* child : *getChildList())
        {
            if (dynamic_cast<LLButton*>(child) && child->getVisible())
            {
                child->setVisible(false);
            }
        }
    }

    void PsTabs::readTabs()
    {
        const S32 count = getTabCount();
        if ((S32)mTabs.size() == count)
        {
            return;
        }
        std::vector<Tab> tabs((size_t)count);
        for (S32 i = 0; i < count; ++i)
        {
            LLPanel* page = getPanelByIndex(i);
            if (!page)
            {
                continue;
            }
            Tab& tab = tabs[i];
            tab.panel = page->getHandle();
            std::string label = page->getLabel();
            LLStringUtil::trim(label);
            tab.label = utf8str_to_wstring(label);
            tab.textWidth = mFont ? mFont->getWidth(tab.label.c_str()) : 0;
            // The page's tool tip is its tab's: taken off the page when it comes, kept here after.
            for (const Tab& old : mTabs)
            {
                if (old.panel.get() == page)
                {
                    tab.tip = old.tip;
                    break;
                }
            }
            if (tab.tip.empty() && !page->getToolTip().empty())
            {
                tab.tip = page->getToolTip();
                page->setToolTip(LLStringUtil::null);
            }
        }
        mTabs.swap(tabs);
        layOut();
    }

    LLRect PsTabs::pageRect() const
    {
        const S32 w = getRect().getWidth();
        const S32 h = getRect().getHeight();
        return mLeft ? LLRect(mRailWidth, h, w, 0) : LLRect(0, h - mStripHeight, w, 0);
    }

    LLRectf PsTabs::tabArea() const
    {
        const F32 w = F32(getRect().getWidth());
        const F32 h = F32(getRect().getHeight());
        return mLeft ? LLRectf(0.f, h, F32(mRailWidth), 0.f) : LLRectf(0.f, h, w, h - F32(mStripHeight));
    }

    void PsTabs::layOut()
    {
        // The official container keeps each tab's visibility (the search's); read for each tab, at most a
        // few dozen.
        S32 shown = 0;
        S32 strip = style::TABS_TOP_PAD;    // top: the tabs' width with the pads at both ends
        for (Tab& tab : mTabs)
        {
            const LLPanel* page = tab.panel.get();
            tab.visible = page && getTabVisibility(page);
            if (tab.visible)
            {
                strip += (shown > 0 ? style::TABS_TOP_GAP : 0) + tab.textWidth + 2 * style::TAB_PAD;
                ++shown;
            }
        }
        const S32 w = getRect().getWidth();
        const S32 h = getRect().getHeight();
        if (mLeft)
        {
            const S32 row = shown > 0 ? llclamp(h / shown, style::TABS_ROW_MIN, mRowHeight) : mRowHeight;
            mStep = row;
            mMaxScroll = llmax(0, shown * row - h);
            mScroll = llclamp(mScroll, 0, mMaxScroll);
            const F32 right = F32(mRailWidth - style::TABS_ROW_GAP);
            F32 top = F32(h + mScroll);
            for (Tab& tab : mTabs)
            {
                tab.rect = tab.visible ? LLRectf(0.f, top, right, top - F32(row)) : LLRectf();
                top -= tab.visible ? F32(row) : 0.f;
            }
        }
        else
        {
            strip += style::TABS_TOP_PAD;
            mStep = style::TABS_TOP_GAP * 4;
            mMaxScroll = llmax(0, strip - w);
            mScroll = llclamp(mScroll, 0, mMaxScroll);
            const F32 top = F32(h);
            const F32 bottom = F32(h - mStripHeight);
            F32 x = F32(style::TABS_TOP_PAD - mScroll);
            for (Tab& tab : mTabs)
            {
                if (!tab.visible)
                {
                    tab.rect = LLRectf();
                    continue;
                }
                const F32 width = F32(tab.textWidth + 2 * style::TAB_PAD);
                tab.rect = LLRectf(x, top, x + width, bottom);
                x += width + F32(style::TABS_TOP_GAP);
            }
        }
    }

    void PsTabs::scrollTo(S32 index)
    {
        if (index < 0 || index >= (S32)mTabs.size() || !mTabs[index].visible || mMaxScroll == 0)
        {
            return;
        }
        const LLRectf& r = mTabs[index].rect;
        const LLRectf area = tabArea();
        S32 scroll = mScroll;
        if (mLeft)
        {
            if (r.mTop > area.mTop)             scroll -= S32(r.mTop - area.mTop);
            else if (r.mBottom < area.mBottom)  scroll += S32(area.mBottom - r.mBottom);
        }
        else
        {
            if (r.mLeft < area.mLeft)           scroll -= S32(area.mLeft - r.mLeft);
            else if (r.mRight > area.mRight)    scroll += S32(r.mRight - area.mRight);
        }
        if (scroll != mScroll)
        {
            mScroll = scroll;
            layOut();
        }
    }

    S32 PsTabs::tabAt(S32 x, S32 y) const
    {
        if (!inTabArea(x, y))
        {
            return -1;
        }
        for (S32 i = 0; i < (S32)mTabs.size(); ++i)
        {
            if (mTabs[i].visible && mTabs[i].rect.pointInRect(F32(x), F32(y)))
            {
                return i;
            }
        }
        return -1;
    }

    S32 PsTabs::pageOf(const LLView* view) const
    {
        for (S32 i = 0; view && i < getTabCount(); ++i)
        {
            const LLPanel* page = getPanelByIndex(i);
            if (page && (view == page || view->hasAncestor(page)))
            {
                return i;
            }
        }
        return -1;
    }

    void PsTabs::syncShown()
    {
        const S32 now = getCurrentPanelIndex();
        if (now == mShown)
        {
            return;
        }
        mShown = now;
        bool all_shown = true;
        for (S32 i = 0; i < getTabCount(); ++i)
        {
            const LLPanel* page = getPanelByIndex(i);
            all_shown = all_shown && page && getTabVisibility(page);
            if (i != now)
            {
                releaseFocusIn(page, this);     // to the tabs
            }
        }
        if (all_shown)
        {
            mSettled = now;
        }
        layOut();
        scrollTo(now);
        if (mOnShown)
        {
            mOnShown(now);
        }
    }

    void PsTabs::showAllTabs()
    {
        const S32 before = mSettled;    // the tabs turned back on one by one select each of them
        showTabsOf(*this);
        if (!getVisible())
        {
            setVisible(true);
        }
        if (before >= 0 && before < getTabCount() && before != getCurrentPanelIndex())
        {
            selectTab(before);
        }
        syncShown();
    }

    void PsTabs::watchSearch()
    {
        if (mSearchName.empty())
        {
            return;
        }
        if (!mSearchLooked)
        {
            mSearchLooked = true;
            LLFloater* floater = getParentByType<LLFloater>();
            LLSearchEditor* found = floater ? floater->findChild<LLSearchEditor>(mSearchName) : nullptr;
            if (found)
            {
                mSearch = found->getHandle();
            }
            else
            {
                LL_WARNS("Plus") << "plus_ps_tabs " << getName() << ": no search box " << mSearchName << LL_ENDL;
            }
        }
        const LLSearchEditor* search = static_cast<const LLSearchEditor*>(mSearch.get());
        if (!search)
        {
            return;
        }
        const bool searching = !search->getText().empty();
        if (mSearching && !searching)
        {
            showTabsIn(this);   // the containers in the pages (this one's own tabs are the official search's)
        }
        mSearching = searching;
    }

    void PsTabs::keepFocusOffHiddenButtons()
    {
        const LLView* focus = dynamic_cast<const LLView*>(gFocusMgr.getKeyboardFocus());
        if (focus && focus->getParent() == this && dynamic_cast<const LLButton*>(focus))
        {
            setFocus(true);
        }
    }

    void PsTabs::pick(S32 index)
    {
        if (index != getCurrentPanelIndex())
        {
            selectTab(index);
            syncShown();        // also when selectTab() did not commit
        }
        setFocus(true);         // the keys pick the next tab from here
        if (index == getCurrentPanelIndex() && mOnPicked)
        {
            mOnPicked(index);
        }
    }

    void PsTabs::setFocus(bool b)
    {
        // The tabs themselves take the focus (LLPanel's would pass it on to the first control of the page).
        if (b)
        {
            LLUICtrl::setFocus(true);
        }
        else
        {
            LLTabContainer::setFocus(false);
        }
    }

    void PsTabs::draw()
    {
        watchSearch();
        keepFocusOffHiddenButtons();
        readTabs();
        layOut();
        syncShown();
        {
            // the page shown (not LLTabContainer::draw(): its tab buttons are hidden here), cut to the container as
            // the official one cuts it: a child with no place written (stacked under the one before it by
            // applyXUILayout) is not drawn below the window
            LLLocalClipRect clip(getLocalRect());
            LLPanel::draw();
        }

        const F32 a = drawAlphaOf(*this);
        const LLRectf area = tabArea();
        if (!mLeft)
        {
            fillRect(LLRectf(0.f, area.mBottom + 1.f, area.mRight, area.mBottom), Fill::solid(faded(style::DIVIDER_LEFT, a)));
        }
        // Tabs scrolled out of the area are cut off; the focus ring (2 px out) and the glow under the strip are not.
        constexpr S32 RING = 3;
        LLRect clip_rect = wholePixels(area);
        clip_rect.mLeft -= RING;
        clip_rect.mTop += RING;
        clip_rect.mRight += RING;
        clip_rect.mBottom -= llmax(RING, S32(style::TAB_GLOW));
        LLLocalClipRect clip(clip_rect);
        for (S32 i = 0; i < (S32)mTabs.size(); ++i)
        {
            if (mTabs[i].visible)
            {
                drawTab(mTabs[i], i, a);
            }
        }
    }

    void PsTabs::drawTab(const Tab& tab, S32 index, F32 alpha) const
    {
        const LLRectf& r = tab.rect;
        const bool chosen = index == getCurrentPanelIndex();
        const bool hovered = index == mHoverTab && getEnabled();
        if (!mLeft)
        {
            drawUnderlineTab(mFont, tab.label, r, chosen, hovered, alpha);
        }
        else
        {
            drawRailRow(tab, r, chosen, hovered && index == mPressedTab && gFocusMgr.getMouseCapture() == this,
                        hovered, alpha);
        }
        if (chosen)
        {
            drawFocusRingOf(*this, r, mLeft ? style::LIST_ROW_RADIUS : 4.f);   // while the tabs have the focus
        }
    }

    // A rail's row: as a row of a list (19.) in the rail's colours (1.).
    void PsTabs::drawRailRow(const Tab& tab, const LLRectf& r, bool chosen, bool pressed, bool hovered, F32 alpha) const
    {
        const F32 radius = style::LIST_ROW_RADIUS;
        if (chosen)
        {
            drawRound(r, radius, Fill::horizontal(style::RAIL_SEL_LEFT, style::RAIL_SEL_RIGHT).faded(alpha));
            fillRect(LLRectf(r.mLeft, r.mTop - 6.f, r.mLeft + style::RAIL_BAR_WIDTH, r.mBottom + 6.f),
                     Fill::solid(faded(style::RAIL_SEL_BAR, alpha)));
        }
        else if (hovered)
        {
            drawRound(r, radius, Fill::solid(faded(pressed ? style::RAIL_PRESS : style::RAIL_HOVER, alpha)));
        }
        const F32 x = r.mLeft + F32(style::TABS_TEXT_PAD);
        drawText(mFont, tab.label, x, r.getCenterY(), faded(chosen ? style::RAIL_SEL_TEXT : style::RAIL_TEXT, alpha),
                 LLFontGL::LEFT, LLFontGL::VCENTER, llmax(0, S32(r.mRight - x) - 6), true);
    }

    bool PsTabs::handleMouseDown(S32 x, S32 y, MASK mask)
    {
        const S32 index = tabAt(x, y);
        if (index >= 0)
        {
            mPressedTab = index;
            gFocusMgr.setMouseCapture(this);
            return true;
        }
        if (inTabArea(x, y))
        {
            return true;    // between the tabs: nothing is under them
        }
        return LLTabContainer::handleMouseDown(x, y, mask);
    }

    bool PsTabs::handleMouseUp(S32 x, S32 y, MASK mask)
    {
        if (hasMouseCapture())
        {
            const S32 pressed = mPressedTab;
            gFocusMgr.setMouseCapture(nullptr);     // onMouseCaptureLost() forgets mPressedTab
            if (pressed >= 0 && tabAt(x, y) == pressed)
            {
                pick(pressed);
            }
            return true;
        }
        return LLTabContainer::handleMouseUp(x, y, mask);
    }

    bool PsTabs::handleHover(S32 x, S32 y, MASK mask)
    {
        mHoverTab = tabAt(x, y);
        if (hasMouseCapture() || inTabArea(x, y))
        {
            getWindow()->setCursor(UI_CURSOR_ARROW);
            return true;
        }
        return LLTabContainer::handleHover(x, y, mask);
    }

    bool PsTabs::handleScrollWheel(S32 x, S32 y, S32 clicks)
    {
        if (mMaxScroll > 0 && inTabArea(x, y))
        {
            mScroll = llclamp(mScroll + clicks * mStep, 0, mMaxScroll);
            layOut();
            return true;
        }
        return LLTabContainer::handleScrollWheel(x, y, clicks);
    }

    bool PsTabs::handleToolTip(S32 x, S32 y, MASK mask)
    {
        const S32 index = tabAt(x, y);
        if (index < 0)
        {
            return LLTabContainer::handleToolTip(x, y, mask);
        }
        const Tab& tab = mTabs[index];
        std::string tip = tab.tip;
        const S32 room = mLeft ? S32(tab.rect.getWidth()) - style::TABS_TEXT_PAD - 6 : S32(tab.rect.getWidth());
        if (tip.empty() && tab.textWidth > room)
        {
            tip = wstring_to_utf8str(tab.label);    // a name cut short
        }
        if (tip.empty())
        {
            return false;
        }
        LLRect screen;
        localRectToScreen(wholePixels(tab.rect), &screen);
        LLToolTipMgr::instance().show(LLToolTip::Params().message(tip).sticky_rect(screen));
        return true;
    }

    bool PsTabs::handleKeyHere(KEY key, MASK mask)
    {
        // The official keys (Up / Down or Left / Right with the focus here, Alt+Left / Alt+Right from the page).
        const S32 before = getCurrentPanelIndex();
        const bool handled = LLTabContainer::handleKeyHere(key, mask);
        syncShown();
        keepFocusOffHiddenButtons();
        const S32 now = getCurrentPanelIndex();
        if (now != before && mOnPicked)
        {
            mOnPicked(now);
        }
        return handled;
    }

    void PsTabs::onMouseLeave(S32 x, S32 y, MASK mask)
    {
        mHoverTab = -1;
        LLTabContainer::onMouseLeave(x, y, mask);
    }

    void PsTabs::onMouseCaptureLost()
    {
        mPressedTab = -1;
        LLTabContainer::onMouseCaptureLost();
    }
}
