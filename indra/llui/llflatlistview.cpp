/**
 * @file llflatlistview.cpp
 * @brief LLFlatListView base class and extension to support messages for several cases of an empty list.
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llpanel.h"
#include "lltextbox.h"

#include "llflatlistview.h"

#include "llfocusmgr.h"
#include "llrender2dutils.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llwindow.h"

#include <algorithm>

static const LLDefaultChildRegistry::Register<LLFlatListView> flat_list_view("flat_list_view");

const LLSD SELECTED_EVENT   = LLSD().with("selected", true);
const LLSD UNSELECTED_EVENT = LLSD().with("selected", false);

//forward declaration
bool llsds_are_equal(const LLSD& llsd_1, const LLSD& llsd_2);

LLFlatListView::Params::Params()
:   item_pad("item_pad"),
    allow_select("allow_select"),
    multi_select("multi_select"),
    keep_one_selected("keep_one_selected"),
    keep_selection_visible_on_reshape("keep_selection_visible_on_reshape",false),
    allow_reorder("allow_reorder", false),
    drag_indicator_color("drag_indicator_color"),
    no_items_text("no_items_text")
{};

void LLFlatListView::reshape(S32 width, S32 height, bool called_from_parent /* = true */)
{
    S32 delta = height - getRect().getHeight();
    LLRect visible_rc;
    LLRect selected_rc = getLastSelectedItemRect();
    bool keep_selection_visible = false;
    if (delta != 0 && mKeepSelectionVisibleOnReshape && selected_rc.isValid())
    {
        visible_rc = getVisibleContentRect();
        keep_selection_visible = visible_rc.overlaps(selected_rc);
    }

    LLScrollContainer::reshape(width, height, called_from_parent);
    setItemsNoScrollWidth(width);
    rearrangeItems();

    if(keep_selection_visible)
    {
        ensureSelectedVisible();
    }
}

const LLRect& LLFlatListView::getItemsRect() const
{
    return mItemsPanel->getRect();
}

bool LLFlatListView::addItem(LLPanel * item, const LLSD& value /*= LLUUID::null*/, EAddPosition pos /*= ADD_BOTTOM*/,bool rearrange /*= true*/)
{
    if (!item) return false;
    if (value.isUndefined()) return false; // item stays an orphan?!!!

    //force uniqueness of items, easiest check but unreliable
    if (item->getParent() == mItemsPanel) return false;

    item_pair_t* new_pair = new item_pair_t(item, value);
    switch (pos)
    {
    case ADD_TOP:
        mItemPairs.push_front(new_pair);
        //in LLView::draw() children are iterated in backorder
        mItemsPanel->addChildInBack(item);
        break;
    case ADD_BOTTOM:
        mItemPairs.push_back(new_pair);
        mItemsPanel->addChild(item);
        break;
    default:
        break;
    }

    //_4 is for MASK
    item->setMouseDownCallback(boost::bind(&LLFlatListView::onItemMouseClick, this, new_pair, _4, CLICK_LEFT));
    item->setRightMouseDownCallback(boost::bind(&LLFlatListView::onItemRightMouseClick, this, new_pair, _4));

    // Children don't accept the focus
    item->setTabStop(false);

    if (rearrange)
    {
        rearrangeItems();
        notifyParentItemsRectChanged();
    }
    return true;
}

bool LLFlatListView::addItemPairs(pairs_list_t panel_list, bool rearrange /*= true*/)
{
    if (!mItemComparator)
    {
        LL_WARNS_ONCE() << "No comparator specified for inserting FlatListView items." << LL_ENDL;
        return false;
    }
    if (panel_list.size() == 0)
    {
        return false;
    }

    // presort list so that it will be easier to sort elements into mItemPairs
    panel_list.sort(ComparatorAdaptor(*mItemComparator));

    pairs_const_iterator_t new_pair_it = panel_list.begin();
    item_pair_t* new_pair = *new_pair_it;
    pairs_iterator_t pair_it = mItemPairs.begin();
    item_pair_t* item_pair = *pair_it;

    // sort panel_list into mItemPars
    while (new_pair_it != panel_list.end() && pair_it != mItemPairs.end())
    {
        if (!new_pair->first || new_pair->first->getParent() == mItemsPanel)
        {
            // iterator already used or we are reusing existing panel
            new_pair_it++;
            new_pair = *new_pair_it;
        }
        else if (mItemComparator->compare(new_pair->first, item_pair->first))
        {
            LLPanel* panel = new_pair->first;

            mItemPairs.insert(pair_it, new_pair);
            mItemsPanel->addChild(panel);

            //_4 is for MASK
            panel->setMouseDownCallback(boost::bind(&LLFlatListView::onItemMouseClick, this, new_pair, _4, CLICK_LEFT));
            panel->setRightMouseDownCallback(boost::bind(&LLFlatListView::onItemRightMouseClick, this, new_pair, _4));
            // Children don't accept the focus
            panel->setTabStop(false);
        }
        else
        {
            pair_it++;
            item_pair = *pair_it;
        }
    }

    // Add what is left of panel_list into the end of mItemPairs.
    for (; new_pair_it != panel_list.end(); ++new_pair_it)
    {
        item_pair_t* item_pair = *new_pair_it;
        LLPanel *panel = item_pair->first;
        if (panel && panel->getParent() != mItemsPanel)
        {
            mItemPairs.push_back(item_pair);
            mItemsPanel->addChild(panel);

            //_4 is for MASK
            panel->setMouseDownCallback(boost::bind(&LLFlatListView::onItemMouseClick, this, item_pair, _4, CLICK_LEFT));
            panel->setRightMouseDownCallback(boost::bind(&LLFlatListView::onItemRightMouseClick, this, item_pair, _4));
            // Children don't accept the focus
            panel->setTabStop(false);
        }
    }

    if (rearrange)
    {
        rearrangeItems();
        notifyParentItemsRectChanged();
    }
    return true;
}


bool LLFlatListView::insertItemAfter(LLPanel* after_item, LLPanel* item_to_add, const LLSD& value /*= LLUUID::null*/)
{
    if (!after_item) return false;
    if (!item_to_add) return false;
    if (value.isUndefined()) return false;

    if (mItemPairs.empty()) return false;

    //force uniqueness of items, easiest check but unreliable
    if (item_to_add->getParent() == mItemsPanel) return false;

    item_pair_t* after_pair = getItemPair(after_item);
    if (!after_pair) return false;

    item_pair_t* new_pair = new item_pair_t(item_to_add, value);
    if (after_pair == mItemPairs.back())
    {
        mItemPairs.push_back(new_pair);
        mItemsPanel->addChild(item_to_add);
    }
    else
    {
        pairs_iterator_t it = mItemPairs.begin();
        for (; it != mItemPairs.end(); ++it)
        {
            if (*it == after_pair)
            {
                // insert new elements before the element at position of passed iterator.
                mItemPairs.insert(++it, new_pair);
                mItemsPanel->addChild(item_to_add);
                break;
            }
        }
    }

    //_4 is for MASK
    item_to_add->setMouseDownCallback(boost::bind(&LLFlatListView::onItemMouseClick, this, new_pair, _4, CLICK_LEFT));
    item_to_add->setRightMouseDownCallback(boost::bind(&LLFlatListView::onItemRightMouseClick, this, new_pair, _4));

    rearrangeItems();
    notifyParentItemsRectChanged();
    return true;
}


bool LLFlatListView::removeItem(LLPanel* item, bool rearrange)
{
    if (!item) return false;
    if (item->getParent() != mItemsPanel) return false;

    item_pair_t* item_pair = getItemPair(item);
    if (!item_pair) return false;

    return removeItemPair(item_pair, rearrange);
}

bool LLFlatListView::removeItemByValue(const LLSD& value, bool rearrange)
{
    if (value.isUndefined()) return false;

    item_pair_t* item_pair = getItemPair(value);
    if (!item_pair) return false;

    return removeItemPair(item_pair, rearrange);
}

bool LLFlatListView::removeItemByUUID(const LLUUID& uuid, bool rearrange)
{
    return removeItemByValue(LLSD(uuid), rearrange);
}

LLPanel* LLFlatListView::getItemByValue(const LLSD& value) const
{
    if (value.isUndefined()) return NULL;

    item_pair_t* pair = getItemPair(value);
    if (pair) return pair->first;
    return NULL;
}

bool LLFlatListView::selectItem(LLPanel* item, bool select /*= true*/)
{
    if (!item) return false;
    if (item->getParent() != mItemsPanel) return false;

    item_pair_t* item_pair = getItemPair(item);
    if (!item_pair) return false;

    return selectItemPair(item_pair, select);
}

bool LLFlatListView::selectItemByValue(const LLSD& value, bool select /*= true*/)
{
    if (value.isUndefined()) return false;

    item_pair_t* item_pair = getItemPair(value);
    if (!item_pair) return false;

    return selectItemPair(item_pair, select);
}

bool LLFlatListView::selectItemByUUID(const LLUUID& uuid, bool select /* = true*/)
{
    return selectItemByValue(LLSD(uuid), select);
}


LLSD LLFlatListView::getSelectedValue() const
{
    if (mSelectedItemPairs.empty()) return LLSD();

    item_pair_t* first_selected_pair = mSelectedItemPairs.front();
    return first_selected_pair->second;
}

void LLFlatListView::getSelectedValues(std::vector<LLSD>& selected_values) const
{
    if (mSelectedItemPairs.empty()) return;

    for (pairs_const_iterator_t it = mSelectedItemPairs.begin(); it != mSelectedItemPairs.end(); ++it)
    {
        selected_values.push_back((*it)->second);
    }
}

LLUUID LLFlatListView::getSelectedUUID() const
{
    const LLSD& value = getSelectedValue();
    if (value.isDefined() && value.isUUID())
    {
        return value.asUUID();
    }
    else
    {
        return LLUUID::null;
    }
}

void LLFlatListView::getSelectedUUIDs(uuid_vec_t& selected_uuids) const
{
    if (mSelectedItemPairs.empty()) return;

    for (pairs_const_iterator_t it = mSelectedItemPairs.begin(); it != mSelectedItemPairs.end(); ++it)
    {
        selected_uuids.push_back((*it)->second.asUUID());
    }
}

LLPanel* LLFlatListView::getSelectedItem() const
{
    if (mSelectedItemPairs.empty()) return NULL;

    return mSelectedItemPairs.front()->first;
}

void LLFlatListView::getSelectedItems(std::vector<LLPanel*>& selected_items) const
{
    if (mSelectedItemPairs.empty()) return;

    for (pairs_const_iterator_t it = mSelectedItemPairs.begin(); it != mSelectedItemPairs.end(); ++it)
    {
        selected_items.push_back((*it)->first);
    }
}

void LLFlatListView::resetSelection(bool no_commit_on_deselection /*= false*/)
{
    if (mSelectedItemPairs.empty()) return;

    for (pairs_iterator_t it= mSelectedItemPairs.begin(); it != mSelectedItemPairs.end(); ++it)
    {
        item_pair_t* pair_to_deselect = *it;
        LLPanel* item = pair_to_deselect->first;
        item->setValue(UNSELECTED_EVENT);
    }

    mSelectedItemPairs.clear();

    if (mCommitOnSelectionChange && !no_commit_on_deselection)
    {
        onCommit();
    }

    // Stretch selected item rect to ensure it won't be clipped
    mSelectedItemsBorder->setRect(getLastSelectedItemRect().stretch(-1));
}

void LLFlatListView::setNoItemsCommentText(const std::string& comment_text)
{
    mNoItemsCommentTextbox->setValue(comment_text);
}

U32 LLFlatListView::size(const bool only_visible_items) const
{
    if (only_visible_items)
    {
        U32 size = 0;
        for (pairs_const_iterator_t
                 iter = mItemPairs.begin(),
                 iter_end = mItemPairs.end();
             iter != iter_end; ++iter)
        {
            if ((*iter)->first->getVisible())
                ++size;
        }
        return size;
    }
    else
    {
        return static_cast<U32>(mItemPairs.size());
    }
}

void LLFlatListView::clear()
{
    if (mReorderDragPair)
    {
        cancelReorderDrag();
    }

    // This will clear mSelectedItemPairs, calling all appropriate callbacks.
    resetSelection();

    // do not use LLView::deleteAllChildren to avoid removing nonvisible items. drag-n-drop for ex.
    for (pairs_iterator_t it = mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        mItemsPanel->removeChild((*it)->first);
        (*it)->first->die();
        delete *it;
    }
    mItemPairs.clear();

    // also set items panel height to zero. Reshape it to allow reshaping of non-item children
    LLRect rc = mItemsPanel->getRect();
    rc.mBottom = rc.mTop;
    mItemsPanel->reshape(rc.getWidth(), rc.getHeight());
    mItemsPanel->setRect(rc);

    setNoItemsCommentVisible(true);
    notifyParentItemsRectChanged();
}

void LLFlatListView::sort()
{
    if (!mItemComparator)
    {
        LL_WARNS() << "No comparator specified for sorting FlatListView items." << LL_ENDL;
        return;
    }

    mItemPairs.sort(ComparatorAdaptor(*mItemComparator));
    rearrangeItems();
}

bool LLFlatListView::updateValue(const LLSD& old_value, const LLSD& new_value)
{
    if (old_value.isUndefined() || new_value.isUndefined()) return false;
    if (llsds_are_equal(old_value, new_value)) return false;

    item_pair_t* item_pair = getItemPair(old_value);
    if (!item_pair) return false;

    item_pair->second = new_value;
    return true;
}

//////////////////////////////////////////////////////////////////////////
// PROTECTED STUFF
//////////////////////////////////////////////////////////////////////////

LLFlatListView::LLFlatListView(const LLFlatListView::Params& p)
:   LLScrollContainer(p)
  , mItemComparator(NULL)
  , mItemsPanel(NULL)
  , mItemPad(p.item_pad)
  , mAllowSelection(p.allow_select)
  , mMultipleSelection(p.multi_select)
  , mKeepOneItemSelected(p.keep_one_selected)
  , mCommitOnSelectionChange(false)
  , mPrevNotifyParentRect(LLRect())
  , mNoItemsCommentTextbox(NULL)
  , mIsConsecutiveSelection(false)
  , mKeepSelectionVisibleOnReshape(p.keep_selection_visible_on_reshape)
  , mFocusOnItemClicked(true)
  , mAllowReorder(p.allow_reorder)
  , mIsReordering(false)
  , mReorderDragPair(nullptr)
  , mDeferredSelectPair(nullptr)
  , mReorderMouseDownX(0)
  , mReorderMouseDownY(0)
  , mReorderOutOfRange(false) // <plus> reorder-live </plus>
  , mReorderSwapLockPair(nullptr) // <plus> reorder-live </plus>
  , mDragIndicatorColor(p.drag_indicator_color.isProvided()
        ? p.drag_indicator_color()
        : LLUIColorTable::instance().getColor("EmphasisColor"))
{
    mBorderThickness = getBorderWidth();

    LLRect scroll_rect = getRect();
    LLRect items_rect;

    setItemsNoScrollWidth(scroll_rect.getWidth());
    items_rect.setLeftTopAndSize(mBorderThickness, scroll_rect.getHeight() - mBorderThickness, mItemsNoScrollWidth, 0);

    LLPanel::Params pp;
    pp.rect(items_rect);
    mItemsPanel = LLUICtrlFactory::create<LLPanel> (pp);
    addChild(mItemsPanel);

    //we don't need to stretch in vertical direction on reshaping by a parent
    //no bottom following!
    mItemsPanel->setFollows(FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_TOP);

    LLViewBorder::Params params;
    params.name("scroll border");
    params.rect(getLastSelectedItemRect());
    params.visible(false);
    params.bevel_style(LLViewBorder::BEVEL_IN);
    mSelectedItemsBorder = LLUICtrlFactory::create<LLViewBorder> (params);
    mItemsPanel->addChild( mSelectedItemsBorder );

    {
        // create textbox for "No Items" comment text
        LLTextBox::Params text_p = p.no_items_text;
        if (!text_p.rect.isProvided())
        {
            LLRect comment_rect = getRect();
            comment_rect.setOriginAndSize(0, 0, comment_rect.getWidth(), comment_rect.getHeight());
            comment_rect.stretch(-getBorderWidth());
            text_p.rect(comment_rect);
        }
        text_p.border_visible(false);

        if (!text_p.follows.isProvided())
        {
            text_p.follows.flags(FOLLOWS_ALL);
        }
        mNoItemsCommentTextbox = LLUICtrlFactory::create<LLTextBox>(text_p, this);
    }
};

LLFlatListView::~LLFlatListView()
{
    for (pairs_iterator_t it = mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        mItemsPanel->removeChild((*it)->first);
        (*it)->first->die();
        delete *it;
    }
    mItemPairs.clear();
}

// virtual
void LLFlatListView::draw()
{
    // Keep scrolling while a reorder drag rests over the top/bottom edge, so the
    // user does not have to drop, scroll, and grab again.
    if (mIsReordering)
    {
        S32 local_x, local_y;
        LLUI::getInstance()->getMousePositionLocal(this, &local_x, &local_y);
        if (autoScroll(local_x, local_y))
        {
            updateReorderPreview(local_x, local_y); // <plus> reorder-live </plus>
        }
    }

    // Highlight border if a child of this container has keyboard focus
    if( mSelectedItemsBorder->getVisible() )
    {
        mSelectedItemsBorder->setKeyboardFocusHighlight( hasFocus() );
    }
    LLScrollContainer::draw();

    if (mIsReordering) // <plus> reorder-live </plus>
    {
        drawReorderIndicator();
    }
}

// virtual
bool LLFlatListView::postBuild()
{
    setTabStop(true);
    return LLScrollContainer::postBuild();
}

void LLFlatListView::rearrangeItems()
{
    LL_PROFILE_ZONE_SCOPED;
    static LLUICachedControl<S32> scrollbar_size ("UIScrollbarSize", 0);

    setNoItemsCommentVisible(0==size());

    if (mItemPairs.empty()) return;

    //calculating required height - assuming items can be of different height
    //list should accommodate all its items
    S32 height = 0;

    S32 invisible_children_count = 0;
    pairs_iterator_t it = mItemPairs.begin();
    for (; it != mItemPairs.end(); ++it)
    {
        LLPanel* item = (*it)->first;

        // skip invisible child
        if (!item->getVisible())
        {
            ++invisible_children_count;
            continue;
        }

        height += item->getRect().getHeight();
    }

    // add paddings between items, excluding invisible ones
    height += mItemPad * (static_cast<S32>(mItemPairs.size()) - invisible_children_count - 1);

    LLRect rc = mItemsPanel->getRect();
    S32 width = mItemsNoScrollWidth;

    // update width to avoid horizontal scrollbar
    if (height > getRect().getHeight() - 2 * mBorderThickness)
        width -= scrollbar_size;

    //changes the bottom, end of the list goes down in the scroll container
    rc.setLeftTopAndSize(rc.mLeft, rc.mTop, width, height);
    mItemsPanel->setRect(rc);

    //reshaping items
    S32 item_new_top = height;
    pairs_iterator_t it2, first_it = mItemPairs.begin();
    for (it2 = first_it; it2 != mItemPairs.end(); ++it2)
    {
        LLPanel* item = (*it2)->first;

        // skip invisible child
        if (!item->getVisible())
            continue;

        LLRect rc = item->getRect();
        rc.setLeftTopAndSize(rc.mLeft, item_new_top, width, rc.getHeight());
        item->reshape(rc.getWidth(), rc.getHeight());
        item->setRect(rc);

        // move top for next item in list
        item_new_top -= (rc.getHeight() + mItemPad);
    }

    // Stretch selected item rect to ensure it won't be clipped
    mSelectedItemsBorder->setRect(getLastSelectedItemRect().stretch(-1));
}

void LLFlatListView::onItemMouseClick(item_pair_t* item_pair, MASK mask, EMouseClickType clicktype)
{
    if (!item_pair) return;

    if (!item_pair->first)
    {
        LL_WARNS() << "Attempt to selet an item pair containing null panel item" << LL_ENDL;
        return;
    }

    if (mFocusOnItemClicked)
    {
        setFocus(true);
    }

    if (clicktype == CLICK_LEFT && !(mask & (MASK_SHIFT | MASK_CONTROL)))
    {
        armReorderDrag(item_pair);
    }

    bool select_item = !isSelected(item_pair);

    //*TODO find a better place for that enforcing stuff
    if (mKeepOneItemSelected && numSelected() == 1 && !select_item) return;

    if ( (mask & MASK_SHIFT) && !(mask & MASK_CONTROL)
         && mMultipleSelection && !mSelectedItemPairs.empty() )
    {
        item_pair_t* last_selected_pair = mSelectedItemPairs.back();

        // If item_pair is already selected - do nothing
        if (last_selected_pair == item_pair)
            return;

        bool grab_items = false;
        bool reverse = false;
        pairs_list_t pairs_to_select;

        // Pick out items from list between last selected and current clicked item_pair.
        for (pairs_iterator_t
                 iter = mItemPairs.begin(),
                 iter_end = mItemPairs.end();
             iter != iter_end; ++iter)
        {
            item_pair_t* cur = *iter;
            if (cur == last_selected_pair || cur == item_pair)
            {
                // We've got reverse selection if last grabed item isn't a new selection.
                reverse = grab_items && (cur != item_pair);
                grab_items = !grab_items;
                // Skip last selected and current clicked item pairs.
                continue;
            }
            if (!cur->first->getVisible())
            {
                // Skip invisible item pairs.
                continue;
            }
            if (grab_items)
            {
                pairs_to_select.push_back(cur);
            }
        }

        if (reverse)
        {
            pairs_to_select.reverse();
        }

        pairs_to_select.push_back(item_pair);

        for (pairs_iterator_t
                 iter = pairs_to_select.begin(),
                 iter_end = pairs_to_select.end();
             iter != iter_end; ++iter)
        {
            item_pair_t* pair_to_select = *iter;
            if (isSelected(pair_to_select))
            {
                // Item was already selected but there is a need to keep order from last selected pair to new selection.
                // Do it here to prevent extra mCommitOnSelectionChange in selectItemPair().
                mSelectedItemPairs.remove(pair_to_select);
                mSelectedItemPairs.push_back(pair_to_select);
            }
            else
            {
                selectItemPair(pair_to_select, true);
            }
        }

        if (!select_item)
        {
            // Update last selected item border.
            mSelectedItemsBorder->setRect(getLastSelectedItemRect().stretch(-1));
        }
        return;
    }

    // When a reorderable list has a multi-selection, a plain click on one of the
    // selected rows must not collapse the selection yet: the user may be about to
    // drag the whole selection. Defer collapsing to mouse-up if no drag happens.
    if (mAllowReorder && mMultipleSelection && !(mask & (MASK_CONTROL | MASK_SHIFT))
        && isSelected(item_pair) && numSelected() > 1)
    {
        mDeferredSelectPair = item_pair;
        return;
    }

    //no need to do additional commit on selection reset
    if (!(mask & MASK_CONTROL) || !mMultipleSelection) resetSelection(true);

    //only CTRL usage allows to deselect an item, usual clicking on an item cannot deselect it
    if (mask & MASK_CONTROL)
    selectItemPair(item_pair, select_item);
    else
        selectItemPair(item_pair, true);
}

void LLFlatListView::onItemRightMouseClick(item_pair_t* item_pair, MASK mask)
{
    if (!item_pair)
        return;

    // Forbid deselecting of items on right mouse button click if mMultipleSelection flag is set on,
    // because some of derived classes may have context menu and selected items must be kept.
    if ( !(mask & MASK_CONTROL) && mMultipleSelection && isSelected(item_pair) )
        return;

    // else got same behavior as at onItemMouseClick, but a right click must never start a drag
    onItemMouseClick(item_pair, mask, CLICK_RIGHT);
}

static constexpr S32 REORDER_DRAG_THRESHOLD = 5;

void LLFlatListView::armReorderDrag(item_pair_t* item_pair)
{
    if (!mAllowReorder)
    {
        return;
    }

    if (!item_pair || !item_pair->first)
    {
        return;
    }

    if (size() < 2)
    {
        return; // nothing to reorder against
    }

    if (gFocusMgr.getMouseCapture() && !hasMouseCapture())
    {
        return;
    }

    mReorderDragPair = item_pair;
    mReorderDragGroup.clear();
    mDeferredSelectPair = nullptr;
    mIsReordering = false;
    mReorderOutOfRange = false; // <plus> reorder-live </plus>
    mReorderSwapLockPair = nullptr; // <plus> reorder-live </plus>
    mReorderOrigOrder.clear(); // <plus> reorder-live </plus>

    LLUI::getInstance()->getMousePositionLocal(this, &mReorderMouseDownX, &mReorderMouseDownY);
    gFocusMgr.setMouseCapture(this);
}

void LLFlatListView::updateReorderDrag(S32 x, S32 y)
{
    if (!mReorderDragPair)
        return;

    if (!mIsReordering)
    {
        if (abs(y - mReorderMouseDownY) < REORDER_DRAG_THRESHOLD
            && abs(x - mReorderMouseDownX) < REORDER_DRAG_THRESHOLD)
        {
            return; // still within the click slop, not a drag yet
        }
        mIsReordering = true;
        mReorderOutOfRange = false; // <plus> reorder-live </plus>
        mReorderSwapLockPair = nullptr; // <plus> reorder-live </plus>
        mReorderOrigOrder = mItemPairs; // <plus> reorder-live </plus>
        buildReorderGroup();
    }

    updateReorderPreview(x, y); // <plus> reorder-live </plus>
}

// <plus> reorder-live: live swap preview. Replaces the official insertion-line
// drag (#5918); if that code changes upstream, merge by hand.
void LLFlatListView::getReorderRestoredOrder(pairs_list_t& restored) const
{
    // The start order, limited to rows that still exist. A row added since (a
    // refresh mid-drag) goes right after the row that precedes it now, so it keeps
    // the place addItem gave it.
    restored.clear();
    for (item_pair_t* pair : mReorderOrigOrder)
    {
        if (std::find(mItemPairs.begin(), mItemPairs.end(), pair) != mItemPairs.end())
            restored.push_back(pair);
    }

    item_pair_t* prev = nullptr;
    for (item_pair_t* pair : mItemPairs)
    {
        if (std::find(restored.begin(), restored.end(), pair) == restored.end())
        {
            pairs_iterator_t at = prev ? std::find(restored.begin(), restored.end(), prev) : restored.begin();
            if (prev)
                ++at;
            restored.insert(at, pair);
        }
        prev = pair;
    }
}

void LLFlatListView::restoreReorderOrder()
{
    pairs_list_t restored;
    getReorderRestoredOrder(restored);
    if (restored == mItemPairs)
        return;

    mItemPairs = restored;
    rearrangeItems();
}

bool LLFlatListView::isReorderChanged() const
{
    // Compare the start order with the current order over the rows present in both;
    // rows added or removed since do not count as a move.
    auto present = [](const pairs_list_t& list, item_pair_t* pair)
    {
        return std::find(list.begin(), list.end(), pair) != list.end();
    };

    pairs_const_iterator_t a = mReorderOrigOrder.begin();
    pairs_const_iterator_t b = mItemPairs.begin();
    while (true)
    {
        while (a != mReorderOrigOrder.end() && !present(mItemPairs, *a)) ++a;
        while (b != mItemPairs.end() && !present(mReorderOrigOrder, *b)) ++b;
        if (a == mReorderOrigOrder.end() || b == mItemPairs.end())
            return a != mReorderOrigOrder.end() || b != mItemPairs.end();
        if (*a != *b)
            return true;
        ++a;
        ++b;
    }
}

LLFlatListView::item_pair_t* LLFlatListView::getReorderPairNearestY(S32 x, S32 y) const
{
    S32 panel_x, panel_y;
    localPointToOtherView(x, y, &panel_x, &panel_y, mItemsPanel);

    item_pair_t* nearest = nullptr;
    S32 best = S32_MAX;
    for (item_pair_t* pair : mItemPairs)
    {
        if (!pair->first->getVisible())
            continue;

        const LLRect& rc = pair->first->getRect();
        S32 dist = (panel_y > rc.mTop) ? panel_y - rc.mTop : (panel_y < rc.mBottom) ? rc.mBottom - panel_y : 0;
        if (dist < best)
        {
            best = dist;
            nearest = pair;
        }
    }
    return nearest;
}

void LLFlatListView::getReorderGrabbedExtentAfterSwap(item_pair_t* target, bool target_after, S32& top, S32& bottom) const
{
    // Where the grabbed row would sit (items-panel coordinates, y up) if the dragged
    // rows took the target's slot, without moving anything: stack the visible rows
    // from the top, as rearrangeItems does.
    S32 y = mItemsPanel->getRect().getHeight();
    top = y;
    bottom = y;

    auto place_group = [&]()
    {
        for (item_pair_t* pair : mReorderDragGroup)
        {
            if (!pair->first->getVisible())
                continue;

            if (pair == mReorderDragPair)
            {
                top = y;
                bottom = y - pair->first->getRect().getHeight();
            }
            y -= pair->first->getRect().getHeight() + mItemPad;
        }
    };

    for (item_pair_t* pair : mItemPairs)
    {
        if (!pair->first->getVisible() || isInReorderGroup(pair))
            continue;

        if (pair == target && !target_after)
            place_group();
        y -= pair->first->getRect().getHeight() + mItemPad;
        if (pair == target && target_after)
            place_group();
    }
}

void LLFlatListView::updateReorderPreview(S32 x, S32 y)
{
    if (!mReorderDragPair->first->getVisible())
    {
        return; // the grabbed row was hidden mid-drag: there is no slot to move it to
    }

    item_pair_t* target = getReorderPairAt(x, y);
    if (!target && (!mReorderValidateCallback || getLocalRect().pointInRect(x, y)))
    {
        // Past either end of the rows (inside the list, or anywhere for a list
        // without a validator) the drag counts as the nearest row; the validator
        // still decides whether that row is allowed.
        target = getReorderPairNearestY(x, y);
    }
    bool in_group = target && isInReorderGroup(target);
    bool allowed = in_group
        || (target && (!mReorderValidateCallback || mReorderValidateCallback(mReorderDragPair->second, target->second)));

    if (!allowed)
    {
        // Outside the range the rows may swap in: show the start order again so the
        // drop visibly goes back to where the drag started. Only on entering.
        if (!mReorderOutOfRange)
        {
            mReorderOutOfRange = true;
            mReorderSwapLockPair = nullptr;
            restoreReorderOrder();
        }
        return;
    }

    mReorderOutOfRange = false;
    if (in_group)
    {
        // The dragged rows are under the cursor again: the next row it enters swaps.
        mReorderSwapLockPair = nullptr;
        return;
    }

    // The target is after the grabbed row when we meet that row before it. (The grabbed
    // row, not any dragged row: the lock test below judges by the grabbed row too, and
    // the two must agree when the selection is not contiguous.)
    bool target_after = false;
    for (item_pair_t* pair : mItemPairs)
    {
        if (pair == target)
            break;
        if (pair == mReorderDragPair)
        {
            target_after = true;
            break;
        }
    }

    // Right after a swap the cursor may still be over the row it swapped with (rows
    // of different heights). Swapping with that row again would flicker, so do it only
    // once the cursor is where the grabbed row would end up (the same range
    // getReorderPairAt hits for a row), or beyond it.
    if (target == mReorderSwapLockPair)
    {
        S32 panel_x, panel_y;
        localPointToOtherView(x, y, &panel_x, &panel_y, mItemsPanel);
        S32 grab_top, grab_bottom;
        getReorderGrabbedExtentAfterSwap(target, target_after, grab_top, grab_bottom);
        bool reaches_cursor = target_after ? (panel_y < grab_top + mItemPad) : (panel_y >= grab_bottom);
        if (!reaches_cursor)
        {
            return;
        }
    }

    // The dragged rows take the target's slot: past it when coming from above,
    // before it when coming from below (also when the cursor jumped over rows).
    for (item_pair_t* pair : mReorderDragGroup)
    {
        mItemPairs.remove(pair);
    }
    pairs_iterator_t it = std::find(mItemPairs.begin(), mItemPairs.end(), target);
    if (target_after)
    {
        ++it;
    }
    for (item_pair_t* pair : mReorderDragGroup)
    {
        mItemPairs.insert(it, pair); // preserves group order
    }
    mReorderSwapLockPair = target;

    rearrangeItems();
}
// </plus>

void LLFlatListView::buildReorderGroup()
{
    mReorderDragGroup.clear();

    bool drag_selection = mMultipleSelection && isSelected(mReorderDragPair) && numSelected() > 1;

    for (item_pair_t* pair : mItemPairs)
    {
        if (!pair->first->getVisible())
            continue;

        if (pair == mReorderDragPair)
        {
            mReorderDragGroup.push_back(pair);
        }
        else if (drag_selection && isSelected(pair)
                 && (!mReorderValidateCallback || mReorderValidateCallback(mReorderDragPair->second, pair->second)))
        {
            // only carry along selected rows that belong to the grabbed row's group
            mReorderDragGroup.push_back(pair);
        }
    }
}

bool LLFlatListView::isInReorderGroup(item_pair_t* item_pair) const
{
    return std::find(mReorderDragGroup.begin(), mReorderDragGroup.end(), item_pair) != mReorderDragGroup.end();
}

void LLFlatListView::finishReorderDrag()
{
    if (mReorderDragPair && mIsReordering) // <plus> reorder-live </plus>
    {
        // <plus> reorder-live
        // Committed (or reverted): the cancel below must not restore the start order.
        // Out of range the start order is already on screen, so nothing is reported.
        bool changed = !mReorderOutOfRange && isReorderChanged();
        mReorderOrigOrder.clear();

        if (changed && mReorderCallback)
        {
            LLSD moved_value = mReorderDragPair->second;

            S32 visible_index = 0;
            for (item_pair_t* pair : mItemPairs)
            {
                if (!pair->first->getVisible())
                    continue;
                if (pair == mReorderDragPair)
                    break;
                ++visible_index;
            }

            mReorderCallback(moved_value, visible_index);
        }
        // </plus>
    }

    cancelReorderDrag();
}

void LLFlatListView::cancelReorderDrag(bool send_ended) // <plus> reorder-live </plus>
{
    // <plus> reorder-live: a cancelled drag (list hidden, row removed, Esc) must not
    // leave the preview order. The finished drag keeps it: the owner was just told.
    if (mIsReordering)
    {
        restoreReorderOrder();
    }
    // </plus>

    if (hasMouseCapture())
    {
        // <plus> reorder-live: setMouseCapture(nullptr) calls onMouseCaptureLost, which would
        // send the ended call right here. With send_ended false, release quietly instead.
        if (send_ended)
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
        else
        {
            gFocusMgr.removeMouseCaptureWithoutCallback(this);
        }
        // </plus>
    }

    clearReorderDragState(send_ended); // <plus> reorder-live </plus>
}

void LLFlatListView::clearReorderDragState(bool send_ended) // <plus> reorder-live </plus>
{
    if (mIsReordering)
    {
        getWindow()->setCursor(UI_CURSOR_ARROW);
    }

    bool was_armed = (mReorderDragPair != nullptr);

    mReorderDragPair = nullptr;
    mReorderDragGroup.clear();
    mDeferredSelectPair = nullptr;
    mIsReordering = false;
    mReorderOutOfRange = false; // <plus> reorder-live </plus>
    mReorderSwapLockPair = nullptr; // <plus> reorder-live </plus>
    mReorderOrigOrder.clear(); // <plus> reorder-live </plus>

    notifyReorderEnded(was_armed && send_ended); // <plus> reorder-live </plus>
} // <plus> reorder-live </plus>

// <plus> reorder-live: one place for the drag-ended call, so a caller can run it last
void LLFlatListView::notifyReorderEnded(bool armed)
{
    if (armed && mReorderEndedCallback)
    {
        mReorderEndedCallback();
    }
}
// </plus>

void LLFlatListView::onMouseCaptureLost()
{
    // <plus> reorder-live: focus loss or a modal window ends the drag without a drop;
    // put the rows back (a finished drag has already cleared the snapshot).
    if (mIsReordering)
    {
        restoreReorderOrder();
    }
    // </plus>
    clearReorderDragState();
}

void LLFlatListView::onVisibilityChange(bool new_visibility)
{
    if (!new_visibility && mReorderDragPair)
    {
        cancelReorderDrag();
    }

    LLScrollContainer::onVisibilityChange(new_visibility);
}

LLFlatListView::item_pair_t* LLFlatListView::getReorderPairAt(S32 x, S32 y) const
{
    S32 panel_x, panel_y;
    localPointToOtherView(x, y, &panel_x, &panel_y, mItemsPanel);

    for (item_pair_t* pair : mItemPairs)
    {
        if (!pair->first->getVisible())
            continue;

        // Claim the padding above each row so gap presses still resolve to a row.
        LLRect rc = pair->first->getRect();
        rc.mTop += mItemPad;
        if (rc.pointInRect(panel_x, panel_y))
        {
            return pair;
        }
    }
    return nullptr;
}

// <plus> reorder-live: only the grabbed rows are marked (rows swap live, no insertion line)
void LLFlatListView::drawReorderIndicator()
{
    // Rows swap live while dragging, so only the grabbed rows need marking.
    const LLRect& panel_rc = mItemsPanel->getRect();
    const LLColor4& color = mDragIndicatorColor.get();
    const F32 alpha = mReorderOutOfRange ? 0.08f : 0.15f;

    for (item_pair_t* pair : mReorderDragGroup)
    {
        const LLRect& dr = pair->first->getRect();
        gl_rect_2d(panel_rc.mLeft + dr.mLeft, panel_rc.mBottom + dr.mTop,
                   panel_rc.mLeft + dr.mRight, panel_rc.mBottom + dr.mBottom,
                   color % alpha, true);
    }
}
// </plus>

bool LLFlatListView::handleHover(S32 x, S32 y, MASK mask)
{
    if (mAllowReorder && mReorderDragPair && hasMouseCapture())
    {
        updateReorderDrag(x, y);
        if (mIsReordering)
        {
            getWindow()->setCursor(mReorderOutOfRange ? UI_CURSOR_NO : UI_CURSOR_ARROWDRAG); // <plus> reorder-live </plus>
            return true;
        }
    }
    return LLScrollContainer::handleHover(x, y, mask);
}

bool LLFlatListView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    bool handled = LLScrollContainer::handleMouseDown(x, y, mask);

    // A press in the padding between rows misses every item, so the per-item
    // mouse-down never arms a reorder. Map it to the nearest row and arm there;
    // armReorderDrag's capture guard ignores presses a child control already took.
    if (mAllowReorder && !mReorderDragPair && !(mask & (MASK_CONTROL | MASK_SHIFT)))
    {
        if (item_pair_t* pair = getReorderPairAt(x, y))
        {
            onItemMouseClick(pair, mask, CLICK_LEFT);
        }
    }

    return handled || (mReorderDragPair != nullptr);
}

bool LLFlatListView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mReorderDragPair && hasMouseCapture())
    {
        bool was_reordering = mIsReordering;
        item_pair_t* deferred = mDeferredSelectPair;
        mDeferredSelectPair = nullptr;
        // <plus> reorder-live-uaf: finishReorderDrag can run a pending refresh that frees
        // every row (onReorderEnded), and a new row may reuse the freed address. Keep the
        // row's value and look it up again afterwards instead of comparing the old pointer.
        const bool has_deferred = deferred != nullptr;
        const LLSD deferred_value = has_deferred ? deferred->second : LLSD();
        // </plus>

        finishReorderDrag();

        if (was_reordering)
        {
            return true; // a drag happened: keep the (multi-)selection on the moved rows
        }

        // No drag: apply the selection collapse that was deferred on mouse-down.
        item_pair_t* deferred_now = has_deferred ? getItemPair(deferred_value) : nullptr; // <plus> reorder-live-uaf </plus>
        if (deferred_now) // <plus> reorder-live-uaf </plus>
        { // <plus> reorder-live-uaf </plus>
            resetSelection(true);
            selectItemPair(deferred_now, true); // <plus> reorder-live-uaf </plus>
            return true;
        }
    }
    return LLScrollContainer::handleMouseUp(x, y, mask);
}

bool LLFlatListView::handleKeyHere(KEY key, MASK mask)
{
    bool reset_selection = (mask != MASK_SHIFT);
    bool handled = false;
    switch (key)
    {
        case KEY_RETURN:
        {
            if (mSelectedItemPairs.size() && mask == MASK_NONE)
            {
                mOnReturnSignal(this, getValue());
                handled = true;
            }
            break;
        }
        case KEY_UP:
        {
            if ( !selectNextItemPair(true, reset_selection) && reset_selection)
            {
                // If case we are in accordion tab notify parent to go to the previous accordion
                if(notifyParent(LLSD().with("action","select_prev")) > 0 )//message was processed
                    resetSelection();
            }
            break;
        }
        case KEY_DOWN:
        {
            if ( !selectNextItemPair(false, reset_selection) && reset_selection)
            {
                // If case we are in accordion tab notify parent to go to the next accordion
                if( notifyParent(LLSD().with("action","select_next")) > 0 ) //message was processed
                    resetSelection();
            }
            break;
        }
        case KEY_ESCAPE:
        {
            // <plus> reorder-live: Esc cancels a drag in progress
            if (mReorderDragPair)
            {
                cancelReorderDrag();
                handled = true;
            }
            // </plus>
            else if (mask == MASK_NONE) // <plus> reorder-live: now an else of the drag check </plus>
            {
                setFocus(false); // pass focus to the game area (EXT-8357)
            }
            break;
        }
        default:
            break;
    }

    if ( ( key == KEY_UP || key == KEY_DOWN ) && mSelectedItemPairs.size() )
    {
        ensureSelectedVisible();
        /*
        LLRect visible_rc = getVisibleContentRect();
        LLRect selected_rc = getLastSelectedItemRect();

        if ( !visible_rc.contains (selected_rc) )
        {
            // But scroll in Items panel coordinates
            scrollToShowRect(selected_rc);
        }

        // In case we are in accordion tab notify parent to show selected rectangle
        LLRect screen_rc;
        localRectToScreen(selected_rc, &screen_rc);
        notifyParent(LLSD().with("scrollToShowRect",screen_rc.getValue()));*/

        handled = true;
    }

    return handled ? handled : LLScrollContainer::handleKeyHere(key, mask);
}

LLFlatListView::item_pair_t* LLFlatListView::getItemPair(LLPanel* item) const
{
    llassert(item);

    for (pairs_const_iterator_t it= mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        item_pair_t* item_pair = *it;
        if (item_pair->first == item) return item_pair;
    }
    return NULL;
}

//compares two LLSD's
bool llsds_are_equal(const LLSD& llsd_1, const LLSD& llsd_2)
{
    llassert(llsd_1.isDefined());
    llassert(llsd_2.isDefined());

    if (llsd_1.type() != llsd_2.type()) return false;

    if (!llsd_1.isMap())
    {
        if (llsd_1.isUUID()) return llsd_1.asUUID() == llsd_2.asUUID();

        //assumptions that string representaion is enough for other types
        return llsd_1.asString() == llsd_2.asString();
    }

    if (llsd_1.size() != llsd_2.size()) return false;

    LLSD::map_const_iterator llsd_1_it = llsd_1.beginMap();
    LLSD::map_const_iterator llsd_2_it = llsd_2.beginMap();
    for (S32 i = 0; i < llsd_1.size(); ++i)
    {
        if ((*llsd_1_it).first != (*llsd_2_it).first) return false;
        if (!llsds_are_equal((*llsd_1_it).second, (*llsd_2_it).second)) return false;
        ++llsd_1_it;
        ++llsd_2_it;
    }
    return true;
}

LLFlatListView::item_pair_t* LLFlatListView::getItemPair(const LLSD& value) const
{
    llassert(value.isDefined());

    for (pairs_const_iterator_t it= mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        item_pair_t* item_pair = *it;
        if (llsds_are_equal(item_pair->second, value)) return item_pair;
    }
    return NULL;
}

bool LLFlatListView::selectItemPair(item_pair_t* item_pair, bool select)
{
    llassert(item_pair);

    if (!mAllowSelection && select) return false;

    if (isSelected(item_pair) == select) return true; //already in specified selection state
    if (select)
    {
        mSelectedItemPairs.push_back(item_pair);
    }
    else
    {
        mSelectedItemPairs.remove(item_pair);
    }

    //a way of notifying panel of selection state changes
    LLPanel* item = item_pair->first;
    item->setValue(select ? SELECTED_EVENT : UNSELECTED_EVENT);

    if (mCommitOnSelectionChange)
    {
        onCommit();
    }

    // Stretch selected item rect to ensure it won't be clipped
    mSelectedItemsBorder->setRect(getLastSelectedItemRect().stretch(-1));
    // By default mark it as not consecutive selection
    mIsConsecutiveSelection = false;

    return true;
}

void LLFlatListView::scrollToShowFirstSelectedItem()
{
    if (!mSelectedItemPairs.size()) return;

    LLRect selected_rc = mSelectedItemPairs.front()->first->getRect();

    if (selected_rc.isValid())
    {
        scrollToShowRect(selected_rc);
    }
}

LLRect LLFlatListView::getLastSelectedItemRect()
{
    if (!mSelectedItemPairs.size())
    {
        return LLRect::null;
    }

    return mSelectedItemPairs.back()->first->getRect();
}

void LLFlatListView::selectFirstItem    ()
{
    // No items - no actions!
    if (0 == size()) return;

    // Select first visible item
    for (pairs_iterator_t
             iter = mItemPairs.begin(),
             iter_end = mItemPairs.end();
         iter != iter_end; ++iter)
    {
        // skip invisible items
        if ( (*iter)->first->getVisible() )
        {
            selectItemPair(*iter, true);
            ensureSelectedVisible();
            break;
        }
    }
}

void LLFlatListView::selectLastItem     ()
{
    // No items - no actions!
    if (0 == size()) return;

    // Select last visible item
    for (pairs_list_t::reverse_iterator
             r_iter = mItemPairs.rbegin(),
             r_iter_end = mItemPairs.rend();
         r_iter != r_iter_end; ++r_iter)
    {
        // skip invisible items
        if ( (*r_iter)->first->getVisible() )
        {
            selectItemPair(*r_iter, true);
            ensureSelectedVisible();
            break;
        }
    }
}

void LLFlatListView::ensureSelectedVisible()
{
    LLRect selected_rc = getLastSelectedItemRect();

    if ( selected_rc.isValid() )
    {
        scrollToShowRect(selected_rc);
    }
}


// virtual
bool LLFlatListView::selectNextItemPair(bool is_up_direction, bool reset_selection)
{
    // No items - no actions!
    if ( 0 == size() )
        return false;

    if (!mIsConsecutiveSelection)
    {
        // Leave only one item selected if list has not consecutive selection
        if (mSelectedItemPairs.size() && !reset_selection)
        {
            item_pair_t* cur_sel_pair = mSelectedItemPairs.back();
            resetSelection();
            selectItemPair (cur_sel_pair, true);
        }
    }

    if ( mSelectedItemPairs.size() )
    {
        item_pair_t* to_sel_pair = NULL;
        item_pair_t* cur_sel_pair = NULL;

        // Take the last selected pair
        cur_sel_pair = mSelectedItemPairs.back();
        // Bases on given direction choose next item to select
        if ( is_up_direction )
        {
            // Find current selected item position in mItemPairs list
            pairs_list_t::reverse_iterator sel_it = std::find(mItemPairs.rbegin(), mItemPairs.rend(), cur_sel_pair);

            for (;++sel_it != mItemPairs.rend();)
            {
                // skip invisible items
                if ( (*sel_it)->first->getVisible() )
                {
                    to_sel_pair = *sel_it;
                    break;
                }
            }
        }
        else
        {
            // Find current selected item position in mItemPairs list
            pairs_list_t::iterator sel_it = std::find(mItemPairs.begin(), mItemPairs.end(), cur_sel_pair);

            for (;++sel_it != mItemPairs.end();)
            {
                // skip invisible items
                if ( (*sel_it)->first->getVisible() )
                {
                    to_sel_pair = *sel_it;
                    break;
                }
            }
        }

        if ( to_sel_pair )
        {
            bool select = true;
            if ( reset_selection )
            {
                // Reset current selection if we were asked about it
                resetSelection();
            }
            else
            {
                // If item already selected and no reset request than we should deselect last selected item.
                select = (mSelectedItemPairs.end() == std::find(mSelectedItemPairs.begin(), mSelectedItemPairs.end(), to_sel_pair));
            }
            // Select/Deselect next item
            selectItemPair(select ? to_sel_pair : cur_sel_pair, select);
            // Mark it as consecutive selection
            mIsConsecutiveSelection = true;
            return true;
        }
    }
    else
    {
        // If there weren't selected items then choose the first one bases on given direction
        // Force selection to first item
        if (is_up_direction)
            selectLastItem();
        else
            selectFirstItem();
        // Mark it as consecutive selection
        mIsConsecutiveSelection = true;
        return true;
    }

    return false;
}

bool LLFlatListView::canSelectAll() const
{
    return 0 != size() && mAllowSelection && mMultipleSelection;
}

void LLFlatListView::selectAll()
{
    if (!mAllowSelection || !mMultipleSelection)
        return;

    mSelectedItemPairs.clear();

    for (pairs_const_iterator_t it= mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        item_pair_t* item_pair = *it;
        mSelectedItemPairs.push_back(item_pair);
        //a way of notifying panel of selection state changes
        LLPanel* item = item_pair->first;
        item->setValue(SELECTED_EVENT);
    }

    if (mCommitOnSelectionChange)
    {
        onCommit();
    }

    // Stretch selected item rect to ensure it won't be clipped
    mSelectedItemsBorder->setRect(getLastSelectedItemRect().stretch(-1));
}

bool LLFlatListView::isSelected(item_pair_t* item_pair) const
{
    llassert(item_pair);

    pairs_const_iterator_t it_end = mSelectedItemPairs.end();
    return std::find(mSelectedItemPairs.begin(), it_end, item_pair) != it_end;
}

bool LLFlatListView::removeItemPair(item_pair_t* item_pair, bool rearrange)
{
    llassert(item_pair);

    bool removing_reorder_pair = item_pair == mReorderDragPair
        || std::find(mReorderDragGroup.begin(), mReorderDragGroup.end(), item_pair) != mReorderDragGroup.end();

    // <plus> reorder-live: the drag-ended callback may refresh the owner and free every
    // row, so it runs at the very end, after this row is gone (not in the middle).
    if (removing_reorder_pair)
    {
        cancelReorderDrag(false);
    }
    // </plus>
    mReorderOrigOrder.remove(item_pair); // <plus> reorder-live: no dangling pointer in the snapshot </plus>
    // <plus> reorder-live
    if (item_pair == mReorderSwapLockPair)
    {
        mReorderSwapLockPair = nullptr;
    }
    // </plus>
    if (item_pair == mDeferredSelectPair)
    {
        mDeferredSelectPair = nullptr;
    }

    bool deleted = false;
    bool selection_changed = false;
    for (pairs_iterator_t it = mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        item_pair_t* _item_pair = *it;
        if (_item_pair == item_pair)
        {
            mItemPairs.erase(it);
            deleted = true;
            break;
        }
    }

    // <plus> reorder-live
    if (!deleted)
    {
        notifyReorderEnded(removing_reorder_pair);
        return false;
    }
    // </plus>

    for (pairs_iterator_t it = mSelectedItemPairs.begin(); it != mSelectedItemPairs.end(); ++it)
    {
        item_pair_t* selected_item_pair = *it;
        if (selected_item_pair == item_pair)
        {
            it = mSelectedItemPairs.erase(it);
            selection_changed = true;
            break;
        }
    }

    mItemsPanel->removeChild(item_pair->first);
    item_pair->first->die();
    delete item_pair;

    if (rearrange)
    {
    rearrangeItems();
    notifyParentItemsRectChanged();
    }

    if (selection_changed && mCommitOnSelectionChange)
    {
        onCommit();
    }

    notifyReorderEnded(removing_reorder_pair); // <plus> reorder-live </plus>

    return true;
}

void LLFlatListView::notifyParentItemsRectChanged()
{
    LL_PROFILE_ZONE_SCOPED;
    S32 comment_height = 0;

    // take into account comment text height if exists
    if (mNoItemsCommentTextbox && mNoItemsCommentTextbox->getVisible())
    {
        // top text padding inside the textbox is included into the height
        comment_height = mNoItemsCommentTextbox->getTextPixelHeight();

        // take into account a distance from parent's top border to textbox's top
        comment_height += getRect().getHeight() - mNoItemsCommentTextbox->getRect().mTop;
    }

    LLRect req_rect =  getItemsRect();

    // get maximum of items total height and comment text height
    req_rect.setOriginAndSize(req_rect.mLeft, req_rect.mBottom, req_rect.getWidth(), llmax(req_rect.getHeight(), comment_height));

    // take into account border size.
    req_rect.stretch(getBorderWidth());

    if (req_rect == mPrevNotifyParentRect)
        return;

    mPrevNotifyParentRect = req_rect;

    LLSD params;
    params["action"] = "size_changes";
    params["width"] = req_rect.getWidth();
    params["height"] = req_rect.getHeight();

    if (getParent()) // dummy widgets don't have a parent
        getParent()->notifyParent(params);
}

void LLFlatListView::setNoItemsCommentVisible(bool visible) const
{
    if (mNoItemsCommentTextbox)
    {
        mSelectedItemsBorder->setVisible(!visible);
        mNoItemsCommentTextbox->setVisible(visible);
    }
}

void LLFlatListView::getItems(std::vector<LLPanel*>& items) const
{
    if (mItemPairs.empty()) return;

    items.clear();
    for (pairs_const_iterator_t it = mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        items.push_back((*it)->first);
    }
}

void LLFlatListView::getValues(std::vector<LLSD>& values) const
{
    if (mItemPairs.empty()) return;

    values.clear();
    for (pairs_const_iterator_t it = mItemPairs.begin(); it != mItemPairs.end(); ++it)
    {
        values.push_back((*it)->second);
    }
}

// virtual
void LLFlatListView::onFocusReceived()
{
    if (size())
    {
        mSelectedItemsBorder->setVisible(true);
    }
    gEditMenuHandler = this;
}
// virtual
void LLFlatListView::onFocusLost()
{
    mSelectedItemsBorder->setVisible(false);
    // Route menu back to the default
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = NULL;
    }
}

//virtual
S32 LLFlatListView::notify(const LLSD& info)
{
    if (info.has("action"))
    {
        std::string str_action = info["action"];
        if (str_action == "select_first")
        {
            setFocus(true);
            selectFirstItem();
            return 1;
        }
        else if (str_action == "select_last")
        {
            setFocus(true);
            selectLastItem();
            return 1;
        }
    }
    else if (info.has("rearrange"))
    {
        rearrangeItems();
        notifyParentItemsRectChanged();
        return 1;
    }

    return 0;
}

void LLFlatListView::detachItems(std::vector<LLPanel*>& detached_items)
{
    LLSD action;
    action.with("detach", LLSD());
    // <plus> reorder-live: rows are about to be freed
    if (mReorderDragPair)
    {
        cancelReorderDrag();
    }
    // </plus>
    // Clear detached_items list
    detached_items.clear();
    // Go through items and detach valid items, remove them from items panel
    // and add to detached_items.
    for (auto item_pair : mItemPairs)
    {
        LLPanel* pItem = item_pair->first;
        if (1 == pItem->notify(action))
        {
            selectItemPair(item_pair, false);
            mItemsPanel->removeChild(pItem);
            detached_items.emplace_back(pItem);
        }
    }
    if (!detached_items.empty())
    {
        // Some items were detached, clean ourself from unusable memory
        if (detached_items.size() == mItemPairs.size())
        {
            // This way will be faster if all items were disconnected
            for (auto item_pair : mItemPairs)
            {
                item_pair->first = nullptr;
                delete item_pair;
            }
            mItemPairs.clear();
            // Also set items panel height to zero.
            // Reshape it to allow reshaping of non-item children.
            LLRect rc = mItemsPanel->getRect();
            rc.mBottom = rc.mTop;
            mItemsPanel->reshape(rc.getWidth(), rc.getHeight());
            mItemsPanel->setRect(rc);
            setNoItemsCommentVisible(true);
        }
        else
        {
            for (auto detached_item : detached_items)
            {
                auto found_pos = std::find_if(mItemPairs.begin(), mItemPairs.end(), [detached_item](auto item_pair) { return item_pair->first == detached_item; });
                if (found_pos != mItemPairs.end())
                {
                    auto item_pair = *found_pos; // <plus> reorder-live: read before erase </plus>
                    mItemPairs.erase(found_pos);
                    item_pair->first = nullptr;
                    delete item_pair;
                }
            }
            rearrangeItems();
        }
        notifyParentItemsRectChanged();
    }
}


/************************************************************************/
/*             LLFlatListViewEx implementation                          */
/************************************************************************/
LLFlatListViewEx::Params::Params()
: no_items_msg("no_items_msg")
, no_filtered_items_msg("no_filtered_items_msg")
{
}

LLFlatListViewEx::LLFlatListViewEx(const Params& p)
:   LLFlatListView(p)
, mNoFilteredItemsMsg(p.no_filtered_items_msg)
, mNoItemsMsg(p.no_items_msg)
, mForceShowingUnmatchedItems(false)
, mHasMatchedItems(false)
{
}

void LLFlatListViewEx::updateNoItemsMessage(const std::string& filter_string)
{
    bool items_filtered = !filter_string.empty();
    if (items_filtered)
    {
        // items were filtered
        LLStringUtil::format_map_t args;
        args["[SEARCH_TERM]"] = LLURI::escape(filter_string);
        std::string text = mNoFilteredItemsMsg;
        LLStringUtil::format(text, args);
        setNoItemsCommentText(text);
    }
    else
    {
        // list does not contain any items at all
        setNoItemsCommentText(mNoItemsMsg);
    }
}

bool LLFlatListViewEx::getForceShowingUnmatchedItems() const
{
    return mForceShowingUnmatchedItems;
}

void LLFlatListViewEx::setForceShowingUnmatchedItems(bool show, bool notify_parent)
{
    if (mForceShowingUnmatchedItems != show)
{
    mForceShowingUnmatchedItems = show;
        if (!mFilterSubString.empty())
        {
            updateNoItemsMessage(mFilterSubString);
            filterItems(false, true);
        }
    }
}

void LLFlatListViewEx::setFilterSubString(const std::string& filter_str, bool notify_parent)
{
    if (0 != LLStringUtil::compareInsensitive(filter_str, mFilterSubString))
    {
        mFilterSubString = filter_str;
        updateNoItemsMessage(mFilterSubString);
        filterItems(false, notify_parent);
    }
}

bool LLFlatListViewEx::updateItemVisibility(LLPanel* item, const LLSD &action)
{
    if (!item)
        return false;

    bool visible = true;

    // 0 signifies that filter is matched,
    // i.e. we don't hide items that don't support 'match_filter' action, separators etc.
    if (0 == item->notify(action))
    {
        mHasMatchedItems = true;
    }
    else
    {
        // TODO: implement (re)storing of current selection.
        if (!mForceShowingUnmatchedItems)
        {
            selectItem(item, false);
            visible = false;
        }
    }

    if (item->getVisible() != visible)
    {
        item->setVisible(visible);
        return true;
    }

    return false;
}

bool LLFlatListViewEx::filterItems(bool re_sort, bool notify_parent)
{
    std::string cur_filter = mFilterSubString;
    LLStringUtil::toUpper(cur_filter);

    LLSD action;
    action.with("match_filter", cur_filter);

    mHasMatchedItems = false;
    bool visibility_changed{ false };
    for (auto item_pair : getItemPairs())
    {
        LLPanel* pItem = item_pair->first;
        visibility_changed |= updateItemVisibility(pItem, action);
    }

    if (re_sort)
    {
        sort();
    }

    if (visibility_changed && notify_parent)
    {
        rearrangeItems();
        notifyParentItemsRectChanged();
        return true;
    }
    return false;
}

bool LLFlatListViewEx::hasMatchedItems()
{
    return mHasMatchedItems;
}

//EOF
