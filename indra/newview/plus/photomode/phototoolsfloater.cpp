/**
 * @file phototoolsfloater.cpp
 * @brief The photo tools window (floater_plus_photo_tools.xml): the frame, the tabs and their pages, the rows of
 *        settings, the search, the list of changes and the compare (research/265 pt-ui-frame).
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

// A standard LLFloater and nothing of its own beyond the widgets: no drawing, no window handling. That is what
// lets plus/detach take it to a window of its own like the conversations (it is a floater of gFloaterView: not
// hosted, not dependent), and every control acts on the viewer's settings at once, from wherever the window is.
//
// The window is laid out as the Preferences' Charis tab (research/263, 265): psui widgets on a plus_ps_box, a row
// of sub-tabs over one page each (psui::TabPages). The rows of settings are made from the items of catalog.xml
// with psui::Stack (the labels beside the controls, and before each row the star that shows it on Home, "*"
// changed and "!" rebuilds), the first time their page is shown; the tick reads the rows of the page shown only.
// What a page holds besides its rows (the sky list, the camera positions ...) is a block of its XUI, run by
// phototools_env.cpp and phototools_blocks.cpp. The class: phototoolsfloater_impl.h.

#include "llviewerprecompiledheaders.h"

#include "phototoolsfloater.h"
#include "phototoolsfloater_impl.h"
#include "photoguide.h"
#include "plus/flycamsens/flycamsens.h"
#include "plus/freeze/freeze.h"
#include "plus/look/look.h"
#include "plus/photoscreen/photoscreen.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/photoscreen/psui/pswidgets.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llcombobox.h"
#include "llfiltereditor.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llinventorypanel.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llscrollcontainer.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"

namespace plus::photomode::tools
{
    using psui::Stack;
    using psui::setComboIfChanged;

    namespace
    {
        constexpr F32 COMPARE_CLICK_SECONDS = 0.35f; // a press shorter than this is a click: it keeps the compare
        constexpr S32 TAB_ROW_PAD = 14;  // the row of tabs' pad (the XUI's) on the left, and as much kept on the right
        constexpr S32 TAB_PAD_MIN = 4;   // the tabs' text pads when they do not fit with their own (English, narrow)

        // The columns of rows, in the order of the class's ColumnId: their panels and pages.
        struct ColumnPlace
        {
            const char* panel;
            int         page;
        };
        constexpr ColumnPlace COLUMN_PLACES[] = {
            { "rows_easy",    TAB_HOME },
            { "rows_pins",    TAB_HOME },
            { "rows_quality", TAB_QUALITY },
            { "rows_light",   TAB_SKY },
            { "rows_color",   TAB_COLOR },
            { "rows_camera",  TAB_CAMERA },
            { "rows_subject", TAB_SUBJECT },
            { "rows_shoot",   TAB_SHOOT },
            { "rows_results", PAGE_RESULTS },
        };
        // Home's "Adjust": a few items of other tabs.
        constexpr const char* EASY_ITEMS[] = { "param:brightness", "RenderDepthOfField", "param:blur", "param:tint", "PlusLook" };

        // Hidden, and keeping no keyboard focus (a psui widget does not take it on a click, so a key would still
        // reach the hidden one).
        void hideView(LLView* view)
        {
            if (!view)
            {
                return;
            }
            if (gFocusMgr.childHasKeyboardFocus(view))
            {
                gFocusMgr.setKeyboardFocus(nullptr);
            }
            view->setVisible(false);
        }

        // The words of a psui widget (a button, a tab, a check box, a slider's label); nothing for another.
        void setLabelOf(LLUICtrl* ctrl, const std::string& label)
        {
            if (psui::PsCtrl* ps = dynamic_cast<psui::PsCtrl*>(ctrl))
            {
                ps->setLabelText(label);
            }
        }

        // The Compare button: a psui button that also says when it is pressed and let go (the compare is held
        // while it is down; PsCtrl has the mouse while it is pressed). Space or Return keeps or ends the compare,
        // as a short click does.
        class CompareButton : public psui::PsButton
        {
        public:
            struct Params : public LLInitParam::Block<Params, psui::PsButton::Params>
            {
                Params() {}
            };

            void setCallbacks(std::function<void()> down, std::function<void()> up, std::function<void()> key)
            {
                mDown = std::move(down);
                mUp   = std::move(up);
                mKey  = std::move(key);
            }

            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                const bool handled = psui::PsButton::handleMouseDown(x, y, mask);
                if (hasMouseCapture() && mDown)
                {
                    mDown();
                }
                return handled;
            }

            // A key held down repeats (the OS, about 30 a second): only its first press keeps or ends the compare.
            // Each one would write the settings again, and one that rebuilds the pipeline stops the viewer.
            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (mask == MASK_NONE && (key == ' ' || key == KEY_RETURN) && gKeyboard && gKeyboard->getKeyRepeated(key))
                {
                    return true;
                }
                return psui::PsButton::handleKeyHere(key, mask);
            }

            bool handleMouseUp(S32 x, S32 y, MASK mask) override
            {
                const bool held    = hasMouseCapture();
                const bool handled = psui::PsButton::handleMouseUp(x, y, mask);
                if (held && mUp)
                {
                    mUp();
                }
                return handled;
            }

        protected:
            friend class LLUICtrlFactory;
            explicit CompareButton(const Params& p) : psui::PsButton(p) {}

            // A click: the mouse's is handled by the press and the release; the keyboard's (x, y < 0) by mKey.
            void onClick(S32 x, S32 y) override
            {
                if (x < 0 && y < 0 && mKey)
                {
                    mKey();
                }
            }

        private:
            std::function<void()> mDown, mUp, mKey;
        };
        LLDefaultChildRegistry::Register<CompareButton> sCompareButtonRegistration("plus_photo_compare_button");
    }

    void setTextIfChanged(LLTextBox* box, const std::string& text)
    {
        if (box->getText() != text)
        {
            box->setText(text);
        }
    }

    PhotoToolsFloater::PhotoToolsFloater(const LLSD& key) : LLFloater(key) {}

    // The rows' commits point into mColumns, which go before the widgets (LLView deletes the children after the
    // members): what is being typed is taken now, while everything is there, then the columns' widgets go first.
    PhotoToolsFloater::~PhotoToolsFloater()
    {
        gFocusMgr.releaseFocusIfNeeded(this);
        mRebuilding = true;
        for (Column& column : mColumns)
        {
            if (LLPanel* panel = column.panel.get())
            {
                panel->deleteAllChildren();
            }
        }
    }

    bool PhotoToolsFloater::postBuild()
    {
        captureBaseline();
        buildFrame();
        buildEnv();
        buildBlocks();
        mPages.show(TAB_HOME); // its rows are made at the first draw, when the window has its size
        return true;
    }

    void PhotoToolsFloater::onOpen(const LLSD& key)
    {
        LLFloater::onOpen(key);
        ++mPinsGeneration; // the stars are read again
        openBlocks();
        openEnv();
        refreshRows();
        refreshStatus();
    }

    // The world does not stay stopped behind a closed window (nor a closed window of its own): it
    // resumes, with the held movements of the objects applied as the freeze does on its own.
    void PhotoToolsFloater::onClose(bool app_quitting)
    {
        endCompare(); // a compare that was held while the window went
        if (plus::freeze::isFrozen())
        {
            plus::freeze::setFrozen(false);
        }
        if (!plus::photoscreen::isActive())
        {
            plus::focuslock::setPicking(false); // picked from here: the world's clicks come back with the window gone
        }
        LLFloater::onClose(app_quitting);
    }

    void PhotoToolsFloater::draw()
    {
        if (!mReady)
        {
            mReady = true; // the window has its size (a saved one is put on after postBuild())
            mLayoutDirty = true;
        }
        drawBlocks();
        if (mDofBusy)
        {
            // Over after a run of ordinary frames, or after a while whatever happened. The buffers are made
            // again inside the click (handleReleaseGLBufferChanged); a stall, if any, comes in the frames right
            // after it and starts the run again. Waiting for a stall that never comes (the shaders are made at
            // the build) kept the row off for 20 s: a second click did nothing (pt-home-check).
            constexpr int DOF_CALM_FRAMES = 8;
            constexpr F32 DOF_MIN_SECONDS = 0.5f;
            constexpr F32 DOF_MAX_SECONDS = 20.f;
            const F32     elapsed         = mDofTimer.getElapsedTimeF32();
            mDofCalmFrames = gFrameIntervalSeconds < 0.2f ? mDofCalmFrames + 1 : 0;
            if (elapsed > DOF_MAX_SECONDS || (elapsed > DOF_MIN_SECONDS && mDofCalmFrames >= DOF_CALM_FRAMES))
            {
                mDofBusy = false;
                refreshRows();
            }
        }
        drawEnv();
        if (mRefreshTimer.checkExpirationAndReset(REFRESH_SECONDS))
        {
            tickEnv();
            tickBlocks();
            refreshRows();
            refreshStatus();
        }
        else if (mRefreshSoon)
        {
            refreshRows();
        }
        // the button was let go where this window did not see it (the window lost the mouse): end the compare held by it
        // or ended without it (a quit, cancelled after: compareEnd() on "quitting")
        if ((mCompare == Compare::Held && !gFocusMgr.childHasMouseCapture(mCompareButton.get())) ||
            (mCompare != Compare::Off && !comparing()))
        {
            endCompare();
        }
        // Minimized, the window is only its title (UIMinimizedWidth): not a width to fit the tabs or the rows to.
        if (!isMinimized())
        {
            if (const LLView* row = mTabRow.get(); row && row->getRect().getWidth() != mTabsFitFor)
            {
                fitTabs(); // the window was resized
            }
            preparePage(); // a column of another width, a block that changed: before the page is drawn
        }
        LLFloater::draw();
    }

    // Keys that work while this window has the focus, also as a window of its own: 1 to 8 go to the
    // named cameras of the list. Not while a name is typed. While the flycam is on, the digits are its
    // speed (plus/flycamsens), as with the focus in the world: going to a camera would end the flycam.
    bool PhotoToolsFloater::handleKeyHere(KEY key, MASK mask)
    {
        if (mask == MASK_NONE && key >= '0' && key <= '9' && LLViewerJoystick::getInstance()->getOverrideCamera() &&
            plus::flycamsens::keySens(KEYSTATE_DOWN, (S32)(key - '0')))
        {
            return true;
        }
        // Nor while a list that searches by the typed letters has the focus (the sky's and the
        // water's folders, the looks): the digit would both search there and move the camera.
        const bool typing_in_list = (mLookList && gFocusMgr.childHasKeyboardFocus(mLookList)) ||
                                    (mSky.list && gFocusMgr.childHasKeyboardFocus(mSky.list)) ||
                                    (mWater.list && gFocusMgr.childHasKeyboardFocus(mWater.list));
        if (mask == MASK_NONE && key >= '1' && key <= '0' + MAX_NAMED_VIEWS &&
            !LLViewerJoystick::getInstance()->getOverrideCamera() &&
            !dynamic_cast<LLLineEditor*>(gFocusMgr.getKeyboardFocus()) && !typing_in_list)
        {
            if (goToNamedView((size_t)(key - '1'), gSavedSettings.getBOOL(SETTING_SMOOTH)))
            {
                return true;
            }
        }
        return LLFloater::handleKeyHere(key, mask);
    }

    // --- the frame: the bands, the tabs and their pages -----------------------------------

    void PhotoToolsFloater::buildFrame()
    {
        // --- the top and the bottom ---
        LLUICtrl* search = getChild<LLUICtrl>("flt_search");
        mSearch = search->getHandle();
        search->setCommitCallback([this](LLUICtrl*, const LLSD& param) { onSearch(param.asString()); });
        if (CompareButton* compare = findChild<CompareButton>("btn_compare"))
        {
            mCompareButton = compare->getHandle();
            compare->setCallbacks([this]() { onCompareDown(); }, [this]() { onCompareUp(); }, [this]() { onCompareKey(); });
        }
        else
        {
            LL_WARNS("Plus") << "Photo tools: no btn_compare (plus_photo_compare_button)" << LL_ENDL;
        }
        LLUICtrl* changes = getChild<LLUICtrl>("btn_changes");
        mChangesButton = changes->getHandle();
        changes->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanges(); });
        getChild<LLUICtrl>("btn_reset_all")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetAll(); });
        mStatus = getChild<LLUICtrl>("status")->getHandle();

        // --- the tabs and their pages ---
        LLView* body = getChild<LLView>("body");
        mTabRow = getChild<LLView>("tabs")->getHandle();
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            const std::string tab = std::string("tab_") + TABS[i];
            mPages.add(body, tab, std::string("page_") + TABS[i]); // a missing pair is warned about
            if (LLUICtrl* ctrl = body->findChild<LLUICtrl>(tab))
            {
                mTabs[i] = ctrl->getHandle();
            }
            mTabTipBase[i] = getString(std::string("tab_tip_") + TABS[i]);
            mTabCounts[i]  = -1; // named and tipped at the refreshStatus() below
        }
        mPages.setShownCallback([this](S32 index, bool) { onPageShown(index); });
        LLView* results = getChild<LLView>("page_results");
        mResultsPage    = results->getHandle();
        results->setVisible(false);

        // --- the pages' parts: from the top, in the order of their tops in the XUI ---
        for (int p = 0; p < PAGE_COUNT; ++p)
        {
            const std::string name    = p < TAB_COUNT ? TABS[p] : "results";
            LLScrollContainer* scroll  = findChild<LLScrollContainer>("scroll_" + name);
            LLPanel*           content = findChild<LLPanel>("content_" + name);
            if (!scroll || !content)
            {
                LL_WARNS("Plus") << "Photo tools: no scroll_" << name << " or content_" << name << LL_ENDL;
                continue;
            }
            Page& page   = mPageInfo[p];
            page.scroll  = scroll->getDerivedHandle<LLScrollContainer>();
            page.content = content->getHandle();
            std::vector<LLView*> parts(content->getChildList()->begin(), content->getChildList()->end());
            std::stable_sort(parts.begin(), parts.end(),
                             [](const LLView* a, const LLView* b) { return a->getRect().mTop > b->getRect().mTop; });
            const S32 width = content->getRect().getWidth();
            for (LLView* view : parts)
            {
                page.parts.push_back({ view->getHandle(), view->getRect().mLeft, width - view->getRect().mRight });
            }
        }

        // --- the columns of rows ---
        static_assert(std::size(COLUMN_PLACES) == COLUMN_COUNT, "COLUMN_PLACES and ColumnId differ");
        for (int c = 0; c < COLUMN_COUNT; ++c)
        {
            Column& column = mColumns[c];
            column.page    = COLUMN_PLACES[c].page;
            if (LLPanel* panel = findChild<LLPanel>(COLUMN_PLACES[c].panel))
            {
                column.panel = panel->getHandle();
            }
            else
            {
                LL_WARNS("Plus") << "Photo tools: no " << COLUMN_PLACES[c].panel << LL_ENDL;
            }
        }
        refreshStatus(); // the tabs' names
    }

    // A tab was clicked, or showTab(): its page made and read at once (only once the window has its size).
    void PhotoToolsFloater::onPageShown(int page)
    {
        mLayoutDirty = true;
        if (!mReady)
        {
            return;
        }
        tickEnv();      // its blocks first: one may come or go (the note for old skies)
        tickBlocks();
        preparePage();
        refreshRows();
    }

    void PhotoToolsFloater::showTab(int tab)
    {
        hideResults();
        mPages.show(tab);
    }

    int PhotoToolsFloater::shownTab() const
    {
        return llclamp(mPages.shown(), 0, TAB_COUNT - 1);
    }

    int PhotoToolsFloater::pageShown() const
    {
        return mMode != Mode::Tabs ? PAGE_RESULTS : shownTab();
    }

    void PhotoToolsFloater::requestLayout()
    {
        mLayoutDirty = true;
    }

    void PhotoToolsFloater::layoutShownPage()
    {
        mLayoutDirty = true;
        preparePage();
    }

    LLScrollContainer* PhotoToolsFloater::pageScroll(int page) const
    {
        return page >= 0 && page < PAGE_COUNT ? mPageInfo[page].scroll.get() : nullptr;
    }

    // The width of a page's content: the view less the scroll bar, whether it is shown or not (the rows do not
    // move when it comes).
    S32 PhotoToolsFloater::contentWidth(int page) const
    {
        static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);
        const LLScrollContainer* scroll = pageScroll(page);
        return scroll ? llmax(1, scroll->getRect().getWidth() - 2 * scroll->getBorderWidth() - (S32)scrollbar_size) : 1;
    }

    void PhotoToolsFloater::preparePage()
    {
        const int page  = pageShown();
        const S32 width = contentWidth(page);
        // Not while the mouse is held (the window's edge dragged): a column of another width is made when it stops.
        const bool held = gFocusMgr.getMouseCapture() != nullptr;
        for (Column& column : mColumns)
        {
            if (column.page == page && column.panel.get() && (column.dirty || (column.builtWidth != width && !held)))
            {
                buildColumn(column, width);
            }
        }
        const Page&              info   = mPageInfo[page];
        const LLScrollContainer* scroll = info.scroll.get();
        if (scroll && (mLayoutDirty || scroll->getRect().getWidth() != info.laidWidth ||
                       scroll->getRect().getHeight() != info.laidHeight))
        {
            layoutPage(page);
            mLayoutDirty = false;
        }
    }

    // The visible parts of a page from the top, each under the one before, as wide as the content less its insets;
    // the content as tall as they are, or as the view.
    void PhotoToolsFloater::layoutPage(int page)
    {
        Page&              info    = mPageInfo[page];
        LLScrollContainer* scroll  = info.scroll.get();
        LLPanel*           content = info.content.get();
        if (!scroll || !content)
        {
            return;
        }
        const S32 width  = contentWidth(page);
        const S32 view_h = scroll->getRect().getHeight() - 2 * scroll->getBorderWidth();
        S32       needed = 0;
        for (const Part& part : info.parts)
        {
            const LLView* view = part.view.get();
            if (view && view->getVisible())
            {
                needed += view->getRect().getHeight();
            }
        }
        const S32 height = llmax(needed, view_h, 1);
        if (content->getRect().getWidth() != width || content->getRect().getHeight() != height)
        {
            content->reshape(width, height);
        }
        S32 top = height;
        for (const Part& part : info.parts)
        {
            LLView* view = part.view.get();
            if (!view || !view->getVisible())
            {
                continue;
            }
            const S32 h = view->getRect().getHeight();
            const S32 w = llmax(1, width - part.left - part.right);
            if (view->getRect().getWidth() != w)
            {
                view->reshape(w, h);
            }
            view->setOrigin(part.left, top - h);
            top -= h;
        }
        info.laidWidth  = scroll->getRect().getWidth();
        info.laidHeight = scroll->getRect().getHeight();
    }

    // The tabs as wide as their text and its pads; when that is more than the row has (English in a narrow
    // window), the pads are made smaller, down to TAB_PAD_MIN.
    void PhotoToolsFloater::fitTabs()
    {
        psui::PsRow*    row  = dynamic_cast<psui::PsRow*>(mTabRow.get());
        const LLFontGL* font = psui::font(psui::style::FONT_BODY);
        if (!row || !font)
        {
            return;
        }
        mTabsFitFor = row->getRect().getWidth();
        std::array<S32, TAB_COUNT> text {};
        S32 sum = 0;
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            if (const psui::PsCtrl* tab = dynamic_cast<const psui::PsCtrl*>(mTabs[i].get()))
            {
                text[i] = font->getWidth(tab->getLabelText());
                sum += text[i];
            }
        }
        const S32 room = mTabsFitFor - 2 * TAB_ROW_PAD;
        S32       pad  = psui::style::TAB_PAD;
        if (sum + 2 * pad * TAB_COUNT > room)
        {
            pad = llmax(TAB_PAD_MIN, (room - sum) / (2 * TAB_COUNT));
        }
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            if (LLUICtrl* tab = mTabs[i].get())
            {
                const S32 w = text[i] + 2 * pad;
                if (tab->getRect().getWidth() != w)
                {
                    tab->reshape(w, tab->getRect().getHeight());
                }
            }
        }
        row->arrange();
    }

    // Runs work on this window at the next idle: a widget cannot be removed from inside its own click.
    void PhotoToolsFloater::later(std::function<void(PhotoToolsFloater*)> work)
    {
        LLHandle<LLView> handle = getHandle();
        doOnIdleOneTime([handle, work]() {
            if (auto* floater = dynamic_cast<PhotoToolsFloater*>(handle.get()))
            {
                work(floater);
            }
        });
    }

    // --- the columns of rows ------------------------------------------------------------------

    // Made again (the old widgets go first), with the frame's way of making rows: the labels beside the controls,
    // the marks before them, the items written through writeItem() (and the look list's row through addLookRow()).
    void PhotoToolsFloater::buildColumn(Column& column, S32 width)
    {
        LLPanel* panel = column.panel.get();
        if (!panel)
        {
            return;
        }
        Stack stack = emptyColumn(column, *panel, width);
        column.pinsShown = mPinsGeneration; // the stars are made as pinsNow() has them

        stack.setLabelsBeside(true);
        const std::string pin_tip     = getString("pin_tip");
        const std::string changed_tip = getString("changed_tip");
        const std::string rebuild_tip = getString("rebuild_tip");
        const std::string maybe_tip   = getString("rebuild_tip_maybe");
        stack.setDecorator(
            [this, pin_tip, changed_tip, rebuild_tip, maybe_tip](const Item* item, S32, std::vector<LLView*>& out) {
                if (!item || !mBuilding)
                {
                    return; // a row that is not an item's has no marks
                }
                Mark mark;
                mark.item = item;
                if (psui::PsIconButton* star = psui::stack::makeStar(pin_tip, pinsNow().count(item->id) > 0))
                {
                    const std::string id = item->id;
                    star->setCommitCallback([this, id](LLUICtrl* ctrl, const LLSD&) { onStar(id, ctrl->getValue().asBoolean()); });
                    mark.star = star->getHandle();
                    out.push_back(star);
                }
                if (psui::PsLabel* changed = psui::stack::makeMark("*", changed_tip))
                {
                    changed->setVisible(false);
                    mark.changed = changed->getHandle();
                    out.push_back(changed);
                }
                if (item->rebuild > 0)
                {
                    out.push_back(psui::stack::makeMark("!", item->rebuild == 1 ? rebuild_tip : maybe_tip));
                }
                mBuilding->marks.push_back(mark);
            },
            psui::stack::DECOR_STAR_W + 2 * psui::stack::DECOR_MARK_W);
        Stack::ItemHooks hooks;
        hooks.write     = [this](const Item& item, float value) { writeItem(item, value); };
        hooks.writeText = [this](const Item& item, const std::string& text) { return writeItemText(item, text); };
        hooks.look      = [this](Stack& s, const Item& item) { addLookRow(s, item); };
        hooks.textHint  = getString("text_hint");
        stack.setItemHooks(std::move(hooks));

        mBuilding = &column;
        fillColumn(ColumnId(&column - mColumns.data()), stack);
        mBuilding = nullptr;
        stack.finish();
        column.builtWidth = width;
        column.dirty      = false;
        mLayoutDirty      = true;
        refreshColumn(column);
    }

    // Emptied (the search closed): made again the next time its page is shown.
    void PhotoToolsFloater::clearColumn(Column& column)
    {
        if (LLPanel* panel = column.panel.get())
        {
            emptyColumn(column, *panel, llmax(1, panel->getRect().getWidth())).finish();
        }
        column.builtWidth = -1;
        column.dirty      = true;
    }

    // The column's widgets deleted and what the frame kept of them forgotten; the Stack to fill it again.
    Stack PhotoToolsFloater::emptyColumn(Column& column, LLPanel& panel, S32 width)
    {
        mRebuilding = true; // a commit of a widget on its way out does not read the rows
        Stack stack(&panel, width, column.rows);
        mRebuilding = false;
        column.marks.clear();
        column.looks.clear();
        column.specials.clear();
        column.blurNotes.clear();
        column.blurNoteShown = -1;
        return stack;
    }

    void PhotoToolsFloater::fillColumn(ColumnId id, Stack& stack)
    {
        switch (id)
        {
            case COL_EASY:
                stack.subHeader(getString("easy_title"));
                for (const char* item_id : EASY_ITEMS)
                {
                    if (const Item* item = findItem(item_id))
                    {
                        addItemRow(stack, *item);
                    }
                }
                break;
            case COL_PINS:
            {
                stack.subHeader(getString("pins_title"));
                const std::vector<const Item*> pinned = pinnedItems();
                if (pinned.empty())
                {
                    stack.note(getString("pins_empty"));
                }
                for (const Item* item : pinned)
                {
                    addItemRow(stack, *item);
                }
                break;
            }
            case COL_RESULTS:
                fillResults(stack);
                break;
            default:
                fillTab(stack, TABS[mColumns[id].page]);
                break;
        }
        stack.endSection();
    }

    // The main rows of a tab under its blocks, the headings of catalog.xml over them, then "Details (N)".
    void PhotoToolsFloater::fillTab(Stack& stack, const std::string& tab)
    {
        std::vector<const Item*> main_items, detail_items;
        for (const Item& item : items())
        {
            // The look list is a block of the Light & Color tab (sec_look); its row is for the Home tab, the
            // search and the list of changes.
            if (item.tab == tab && item.composite != "look")
            {
                (item.detail ? detail_items : main_items).push_back(&item);
            }
        }
        stack.gap(psui::stack::ROW_GAP); // under the blocks (and a line over a first heading)
        for (const Item* item : main_items)
        {
            for (const Heading& heading : headings())
            {
                if (heading.tab == tab && heading.before == item->id)
                {
                    stack.subHeader(heading.label);
                }
            }
            addItemRow(stack, *item);
        }
        if (!detail_items.empty())
        {
            // Opened and closed without the rows being made again; the page under it laid out again.
            LLStringUtil::format_map_t args;
            args["[N]"] = std::to_string(detail_items.size());
            stack.foldHeader(getString("details_row", args), mDetailOpen[tab], [this, tab](bool open) {
                mDetailOpen[tab] = open;
                requestLayout();
            });
            for (const Item* item : detail_items)
            {
                addItemRow(stack, *item);
            }
            stack.endFold();
        }
    }

    // The search's results, or the changes: the blocks found (a button that goes there), then the rows under a
    // line that says where they are (the tab, and "Details"), in the order the places first come.
    void PhotoToolsFloater::fillResults(Stack& stack)
    {
        for (const Place* place : mFoundPlaces)
        {
            const std::string id  = place->id;
            const std::string tab = place->tab;
            stack.button(place->label + "  - " + tabName(tab), std::string(), [this, id, tab]() {
                later([id, tab](PhotoToolsFloater* floater) { floater->goToPlace(id, tab); }); // the button goes with the results
            });
        }
        std::vector<std::pair<std::string, std::vector<const Item*>>> by_place;
        for (const Item* item : mFound)
        {
            const std::string where = tabName(item->tab) + (item->detail ? " / " + getString("details_name") : std::string());
            auto at = std::find_if(by_place.begin(), by_place.end(), [&where](const auto& group) { return group.first == where; });
            if (at == by_place.end())
            {
                by_place.emplace_back(where, std::vector<const Item*>());
                at = by_place.end() - 1;
            }
            at->second.push_back(item);
        }
        for (const auto& [where, group] : by_place)
        {
            stack.subHeader(where);
            for (const Item* item : group)
            {
                addItemRow(stack, *item);
            }
        }
        if (mFound.empty() && mFoundPlaces.empty())
        {
            stack.note(getString("search_none"));
        }
    }

    // An item's row, and what the frame keeps of it besides the Stack's row: the depth of field's and the
    // sun's label, the blur's note.
    void PhotoToolsFloater::addItemRow(Stack& stack, const Item& item)
    {
        const size_t first = mBuilding ? mBuilding->rows.size() : 0;
        if (!stack.item(item))
        {
            return;
        }
        if (mBuilding && mBuilding->rows.size() > first)
        {
            const size_t row = mBuilding->rows.size() - 1;
            if (item.id == "RenderDepthOfField")
            {
                mBuilding->specials.push_back({ row, Special::Kind::Dof });
            }
            else if (item.param == Param::Blur)
            {
                mBuilding->specials.push_back({ row, Special::Kind::Blur });
            }
            else if (item.param == Param::Sun)
            {
                mBuilding->specials.push_back({ row, Special::Kind::Sun });
            }
        }
        if (item.param == Param::Blur)
        {
            addBlurNote(stack);
        }
    }

    // The line under the background blur that says why it is off (empty while it is on).
    void PhotoToolsFloater::addBlurNote(Stack& stack)
    {
        psui::PsLabel::Params p(LLUICtrlFactory::getDefaultParams<psui::PsLabel>());
        p.name          = "blur_note";
        p.rect          = LLRect(0, psui::stack::LABEL_H, stack.innerWidth(), 0);
        p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
        p.kind          = "hint";
        p.mouse_opaque  = false;
        psui::PsLabel* note = LLUICtrlFactory::create<psui::PsLabel>(p);
        if (!note || !mBuilding)
        {
            delete note;
            return;
        }
        stack.view(note, psui::stack::LABEL_H);
        mBuilding->blurNotes.push_back(note->getHandle());
    }

    // A look's id is a string: the row puts it on itself, not through setItemValue().
    void PhotoToolsFloater::addLookRow(Stack& stack, const Item& item)
    {
        psui::PsCombo* combo = stack.stepCombo(item.label, getString("look_tip"), getString("look_prev_tip"),
                                               getString("look_next_tip"),
                                               [this](int step) { onLookChosen(stepLook(allLooks(), step)); });
        if (!combo || !mBuilding)
        {
            return;
        }
        combo->setCommitCallback([this](LLUICtrl* changed, const LLSD&) { onLookChosen(changed->getValue().asString()); });
        LookRow row;
        row.combo = combo->getHandle();
        fillLookCombo(combo, row.filled);
        setComboIfChanged(combo, LLSD(currentLook()));
        mBuilding->looks.push_back(row);
    }

    // Every row of the page shown from the settings: its value, the marks, the star.
    void PhotoToolsFloater::refreshRows()
    {
        if (mRebuilding || !mReady)
        {
            mRefreshSoon = true; // not while a column's widgets are being deleted: at the next draw
            return;
        }
        mRefreshSoon = false;
        const int page = pageShown();
        for (Column& column : mColumns)
        {
            if (column.page == page && column.builtWidth >= 0)
            {
                refreshColumn(column);
            }
        }
    }

    void PhotoToolsFloater::refreshColumn(Column& column)
    {
        Stack::refresh(column.rows);
        for (Special& special : column.specials)
        {
            if (special.row >= column.rows.size())
            {
                continue;
            }
            Stack::Row& row = column.rows[special.row];
            if (!row.ctl || !row.item)
            {
                continue;
            }
            if (special.kind != Special::Kind::Sun && mDofBusy)
            {
                row.ctl->setEnabled(false); // not while the pipeline is rebuilt (a click queued in a stall would switch it back)
                row.enabled = -1;           // ... and as the tick has it again after
            }
            if (special.kind == Special::Kind::Blur)
            {
                continue;
            }
            const bool sun   = special.kind == Special::Kind::Sun;
            const int  state = sun ? (itemValue(*row.item) <= UNSET ? 1 : 0) : (mDofBusy ? 1 : 0);
            if (state != special.shown)
            {
                special.shown = state;
                setLabelOf(row.ctl, state ? row.item->label + " " + getString(sun ? "sun_unset" : "dof_switching")
                                          : row.item->label);
            }
        }
        const int blur = blurAvailable() ? 1 : 0;
        if (!column.blurNotes.empty() && blur != column.blurNoteShown)
        {
            column.blurNoteShown = blur;
            const LLSD text      = blur ? std::string() : getString("blur_off");
            for (const LLHandle<LLUICtrl>& note : column.blurNotes)
            {
                if (LLUICtrl* label = note.get())
                {
                    label->setValue(text);
                }
            }
        }
        const std::set<std::string>* pins = column.pinsShown != mPinsGeneration ? &pinsNow() : nullptr;
        column.pinsShown = mPinsGeneration;
        for (Mark& mark : column.marks)
        {
            if (LLUICtrl* changed = mark.changed.get())
            {
                const int now = isChanged(*mark.item) ? 1 : 0;
                if (now != mark.changedShown)
                {
                    mark.changedShown = now;
                    changed->setVisible(now != 0);
                }
            }
            LLUICtrl* star = pins ? mark.star.get() : nullptr;
            if (star)
            {
                star->setValue(LLSD(pins->count(mark.item->id) > 0));
            }
        }
        for (LookRow& look : column.looks)
        {
            refreshLookRow(look);
        }
    }

    const std::set<std::string>& PhotoToolsFloater::pinsNow()
    {
        if (mPinsRead != mPinsGeneration)
        {
            mPins     = pinnedIds();
            mPinsRead = mPinsGeneration;
        }
        return mPins;
    }

    // A star pressed: the other rows of the item show it at their next tick, Home's list of them is made again.
    void PhotoToolsFloater::onStar(const std::string& id, bool starred)
    {
        setPinned(id, starred);
        ++mPinsGeneration;
        mColumns[COL_PINS].dirty = true;
        // now if Home is shown; not from inside the click of a star that may be in that list
        later([](PhotoToolsFloater* floater) {
            floater->preparePage();
            floater->refreshRows();
        });
    }

    void PhotoToolsFloater::writeItem(const Item& item, float value)
    {
        endCompare(); // the settings are the user's again before this one is written
        setItemValue(item, value);
        if (item.id == "RenderDepthOfField")
        {
            startDofSwitch();
        }
        refreshRows();
        refreshStatus();
        refreshLookSection(); // "In use: ..." of the look list (the color tone puts a look on)
    }

    // What is not a date that the item takes is not written: the box shows the setting again (the Stack's).
    bool PhotoToolsFloater::writeItemText(const Item& item, const std::string& text)
    {
        endCompare();
        const bool taken = setItemText(item, text);
        refreshRows();
        refreshStatus();
        return taken;
    }

    // The switch rebuilds the pipeline's buffers (the viewer may stand still a moment): the row stays off until
    // the frames are ordinary again (draw()), so that a click queued during a stall does not switch it back.
    void PhotoToolsFloater::startDofSwitch()
    {
        mDofBusy       = true;
        mDofCalmFrames = 0;
        mDofTimer.reset();
    }

    // "N changed", and the tabs with changes marked (their counts in their tips: the counts on the tabs would not
    // fit the row in English).
    void PhotoToolsFloater::refreshStatus()
    {
        const ChangeCounts counts = changeCounts();
        // what the line says: the compare (-1), or the count; its words made only when that changed
        const int status = mCompare != Compare::Off ? -1 : counts.total;
        LLUICtrl* line   = mStatus.get();
        if (line && status != mStatusShown)
        {
            mStatusShown = status;
            line->setValue(LLSD(status < 0 ? getString("status_compare")
                                           : status > 0 ? changedText(status) : getString("status_none")));
        }
        bool marked = false; // a tab's "*" came or went
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            const int count = i == TAB_HOME ? counts.total : counts.inTab(TABS[i]);
            LLUICtrl* tab   = mTabs[i].get();
            if (!tab || count == mTabCounts[i])
            {
                continue;
            }
            if (mTabCounts[i] < 0 || (count > 0) != (mTabCounts[i] > 0))
            {
                setLabelOf(tab, tabName(TABS[i]) + (count > 0 ? " *" : ""));
                marked = true;
            }
            mTabCounts[i] = count;
            tab->setToolTip(count > 0 ? mTabTipBase[i] + "\n" + changedText(count) : mTabTipBase[i]);
        }
        if (marked)
        {
            fitTabs();
        }
    }

    // "N changed"
    std::string PhotoToolsFloater::changedText(int count) const
    {
        LLStringUtil::format_map_t args;
        args["[N]"] = std::to_string(count);
        return getString("status_changes", args);
    }

    void PhotoToolsFloater::onResetAll()
    {
        endCompare();
        bool pause = false;
        for (const Item* changed : changedItems())
        {
            pause = pause || changed->rebuild > 0;
            if (changed->id == "RenderDepthOfField")
            {
                startDofSwitch();
            }
        }
        if (pause)
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("reset_pause")));
        }
        resetAll();
        refreshNow();
    }

    // After a change of many settings at once (a preset, Undo All): everything the page shown has, at once and
    // not at the next tick (the look in use, the presets' buttons, the home's switches, the sky in use).
    void PhotoToolsFloater::refreshNow()
    {
        tickEnv();
        tickBlocks();
        refreshRows();
        refreshStatus();
    }

    // --- search, the list of changes ----------------------------------------------------------

    void PhotoToolsFloater::onSearch(const std::string& text)
    {
        std::string term = text;
        LLStringUtil::trim(term);
        LLStringUtil::toLower(term);
        if (term.empty())
        {
            hideResults();
            return;
        }
        // the blocks that are more than a setting (the sky list, the water list) first
        mFoundPlaces.clear();
        for (const Place& place : places())
        {
            if (place.words.find(term) != std::string::npos)
            {
                mFoundPlaces.push_back(&place);
            }
        }
        mFound.clear();
        for (const Item& item : items())
        {
            std::string label = item.label;
            LLStringUtil::toLower(label);
            std::string id = item.id;
            LLStringUtil::toLower(id);
            if (label.find(term) != std::string::npos || id.find(term) != std::string::npos ||
                item.words.find(term) != std::string::npos)
            {
                mFound.push_back(&item);
                if (mFound.size() >= MAX_RESULTS)
                {
                    break;
                }
            }
        }
        showResults(Mode::Search);
    }

    // The results' page over the tabs and their pages (made again: what it shows is new).
    void PhotoToolsFloater::showResults(Mode mode)
    {
        mMode = mode;
        hideView(mTabRow.get());
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            hideView(mPages.page(i));
        }
        if (LLView* results = mResultsPage.get())
        {
            results->setVisible(true);
        }
        setLabelOf(mChangesButton.get(), getString(mode == Mode::Changes ? "changes_hide" : "changes_show"));
        mColumns[COL_RESULTS].dirty = true;
        mLayoutDirty = true;
        if (mReady)
        {
            preparePage();
            refreshRows();
        }
    }

    // Back to the tab that was shown. Not from inside a click of the results (goToPlace() comes by later()).
    void PhotoToolsFloater::hideResults()
    {
        if (mMode == Mode::Tabs)
        {
            return;
        }
        mMode = Mode::Tabs;
        hideView(mResultsPage.get());
        clearColumn(mColumns[COL_RESULTS]);
        mFound.clear();
        mFoundPlaces.clear();
        if (LLView* row = mTabRow.get())
        {
            row->setVisible(true);
        }
        setLabelOf(mChangesButton.get(), getString("changes_show"));
        mPages.show(mPages.shown()); // its page again (onPageShown())
    }

    void PhotoToolsFloater::onChanges()
    {
        if (mMode == Mode::Changes)
        {
            hideResults();
            return;
        }
        if (LLFilterEditor* search = dynamic_cast<LLFilterEditor*>(mSearch.get()))
        {
            search->clear();
        }
        mFound = changedItems();
        mFoundPlaces.clear();
        showResults(Mode::Changes);
    }

    // --- compare ------------------------------------------------------------------------------

    void PhotoToolsFloater::onCompareDown()
    {
        if (mCompare == Compare::Latched)
        {
            endCompare(); // the second click
            mCompareSwallowUp = true;
            return;
        }
        mCompareSwallowUp = false;
        compareBegin();
        mCompare = Compare::Held;
        mCompareTimer.reset();
        showCompareState();
        refreshRows();
    }

    // Space or Return on the button: as a short click (kept), or the second one (back).
    void PhotoToolsFloater::onCompareKey()
    {
        if (mCompare != Compare::Off)
        {
            endCompare();
            return;
        }
        compareBegin();
        mCompare = Compare::Latched;
        showCompareState();
        refreshRows();
    }

    void PhotoToolsFloater::onCompareUp()
    {
        if (mCompareSwallowUp)
        {
            mCompareSwallowUp = false;
            return;
        }
        if (mCompare != Compare::Held)
        {
            return;
        }
        if (mCompareTimer.getElapsedTimeF32() < COMPARE_CLICK_SECONDS)
        {
            mCompare = Compare::Latched;
            showCompareState();
            return;
        }
        endCompare();
    }

    void PhotoToolsFloater::endCompare()
    {
        if (mCompare == Compare::Off && !comparing())
        {
            return;
        }
        compareEnd();
        mCompare = Compare::Off;
        showCompareState();
        refreshRows();
        refreshStatus();
    }

    // The button's label, the line at the bottom and the badge on the screen.
    void PhotoToolsFloater::showCompareState()
    {
        const bool on = mCompare != Compare::Off;
        setLabelOf(mCompareButton.get(), getString(mCompare == Compare::Latched ? "compare_label_latched" : "compare_label"));
        setGuideBadge(on ? getString(mCompare == Compare::Latched ? "compare_badge_latched" : "compare_badge") : std::string());
        if (on)
        {
            refreshStatus();
        }
    }

    // --- the look rows ------------------------------------------------------------------------
    //
    // The look in use is PlusLook (plus/look), the setting the Charis menu's "Look" list sets too:
    // nothing is kept here, every list shows the setting. The Light & Color tab has the whole list (all,
    // favorites, recent, a search: phototools_env.cpp); Home and the search have a row with a menu and arrows.

    std::string PhotoToolsFloater::lookName(const LookChoice& choice) const
    {
        if (choice.id.empty())
        {
            return getString("look_none");
        }
        if (!choice.exists || choice.label.empty())
        {
            std::string name = choice.id;
            if (name.rfind("user:", 0) == 0)
            {
                name.erase(0, 5);
            }
            LLStringUtil::format_map_t args;
            args["[NAME]"] = name;
            return getString("look_missing", args);
        }
        return choice.label;
    }

    // The menu of a look row has every look of the folders, and the one in use when it is not among them.
    std::string PhotoToolsFloater::lookComboKey() const
    {
        const std::string current = currentLook();
        const bool        missing = !current.empty() && plus::look::labelOf(current).empty();
        return std::to_string(plus::look::generation()) + (missing ? "|" + current : std::string());
    }

    void PhotoToolsFloater::fillLookCombo(LLComboBox* combo, std::string& filled)
    {
        const std::string current = currentLook();
        bool              found   = current.empty();
        bool              shipped_started = false, user_started = false;
        combo->removeall();
        for (const LookChoice& choice : allLooks())
        {
            if (choice.id.empty())
            {
                combo->add(lookName(choice), LLSD(std::string()));
                continue;
            }
            if (!choice.user && !shipped_started)
            {
                combo->addSeparator();
                shipped_started = true;
            }
            if (choice.user && !user_started)
            {
                combo->addSeparator();
                user_started = true;
            }
            combo->add(choice.user ? lookName(choice) + " " + getString("look_user_mark") : lookName(choice), LLSD(choice.id));
            found = found || choice.id == current;
        }
        if (!found)
        {
            LookChoice gone;
            gone.id     = current;
            gone.exists = false;
            combo->addSeparator();
            combo->add(lookName(gone), LLSD(current));
        }
        filled = lookComboKey();
    }

    void PhotoToolsFloater::refreshLookRow(LookRow& row)
    {
        LLComboBox* combo = dynamic_cast<LLComboBox*>(row.combo.get());
        if (!combo)
        {
            return;
        }
        if (row.filled != lookComboKey())
        {
            fillLookCombo(combo, row.filled); // the folders were read again, or the look in use is not in them
        }
        setComboIfChanged(combo, LLSD(currentLook()));
    }

    // From a list, a menu or an arrow. The lists are looked at again at the next idle: not from inside
    // the click of the list that may be filled again.
    void PhotoToolsFloater::onLookChosen(const std::string& id)
    {
        endCompare();
        if (id != currentLook())
        {
            applyLook(id);
        }
        later([](PhotoToolsFloater* floater) {
            floater->rebuildLookList(false);
            floater->refreshLookSection();
            floater->refreshRows();
            floater->refreshStatus();
        });
    }
}

namespace plus::photomode
{
    void registerFloater()
    {
        LLFloaterReg::add(tools::FLOATER_NAME, "floater_plus_photo_tools.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<tools::PhotoToolsFloater>);
    }

    bool photoToolsShown()
    {
        const LLFloater* floater = LLFloaterReg::findInstance(PHOTO_TOOLS_FLOATER);
        return floater && floater->getVisible() && !floater->isMinimized();
    }

    bool inPhotoMode()
    {
        return plus::photoscreen::isActive() || photoToolsShown();
    }
}
