/**
 * @file invexplorer.h
 * @brief Inventory window like a file explorer: a detail pane on the right that
 *        shows the selected item and appears when the window is wide enough
 *        (placed by invlayout.cpp, between the tab row and the received items).
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

#ifndef PLUS_INVEXPLORER_H
#define PLUS_INVEXPLORER_H

#include <deque>

#include "llframetimer.h"
#include "lluuid.h"

class LLFloater;
class LLFolderViewItem;
class LLInventoryPanel;
class LLPanel;
class LLPanelMainInventory;

namespace plus
{
namespace invexplorer
{
    // Looking for a part of the window (the detail pane, the center column) that may be missing (a floater
    // without it): one look now, then another only after RETRY_FRAMES frames, as the search walks the floater.
    struct LookupGate
    {
        static constexpr U32 RETRY_FRAMES = 120;

        bool due()
        {
            const U32 frame = LLFrameTimer::getFrameCount();
            if (mTried && frame - mLast < RETRY_FRAMES)
            {
                return false;
            }
            mTried = true;
            mLast  = frame;
            return true;
        }

    private:
        U32  mLast  = 0;
        bool mTried = false;
    };

    // The detail pane shows only from this height (panel_plus_inv_detail.xml: the title from the top, the buttons
    // at the bottom); a lower one shows the title and the buttons without the text (research/318 7.3).
    constexpr S32 PANE_MIN_HEIGHT_PX = 60;

    // The detail pane of this window, made the first time it shows (plus::invlayout): a child of the inventory
    // view, hidden, watching the window's minimize and visibility. Null when it cannot be made.
    LLPanel* ensurePane(LLPanelMainInventory* owner, LLFloater* floater);

    // Right before the pane shows (plus::invlayout, every time): it is given the selection of the moment, read
    // without choosing anything (a hidden pane is not told of the changes, showInDetail). Its text is built when
    // it shows.
    void syncPane(LLPanelMainInventory* owner);

    // The keyboard focus is in this window's detail pane (its text, its buttons).
    bool paneHasFocus(const LLPanelMainInventory* owner);

    // The inventory id of a folder view's row (null: none, or not an inventory row).
    LLUUID idOf(const LLFolderViewItem* view);

    // The selection of an inventory view changed (LLPanelMainInventory::onSelectionChange): the detail
    // pane shows it, and in three columns the center opens the folder chosen on the left. In three columns, while
    // the last choice is the center's (invcolumns::centerSelection), the detail pane shows the center's selection
    // instead (the left giving the item the columns came with way to its folder, another tab's notification).
    void onSelection(LLPanelMainInventory* owner, LLInventoryPanel* panel, const std::deque<LLFolderViewItem*>& items);

    // The detail pane shows the first of these (the center column's selection, invcolumns.cpp). Nothing while
    // the pane is hidden.
    void showInDetail(LLPanelMainInventory* owner, const std::deque<LLFolderViewItem*>& items);

    // Once per frame (LLPanelMainInventory::draw): plus::invlayout::tick shows the detail pane while
    // the window is wide enough (PlusInvExplorerMinWidth) and PlusInvExplorerDetail is on, and enters
    // or leaves the three columns.
    void tick(LLPanelMainInventory* owner);

    // The inventory view is being destroyed (~LLPanelMainInventory): drop what is kept for it.
    void forget(const LLPanelMainInventory* owner);
}
}

#endif // PLUS_INVEXPLORER_H
