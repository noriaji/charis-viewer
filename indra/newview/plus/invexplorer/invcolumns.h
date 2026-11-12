/**
 * @file invcolumns.h
 * @brief Inventory window like a file explorer: three columns (the tab's folders on the
 *        left, the objects in the chosen folder in the middle, the detail pane on the right)
 *        while the window is wide enough.
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

#ifndef PLUS_INVCOLUMNS_H
#define PLUS_INVCOLUMNS_H

#include <deque>

class LLFolderViewItem;
class LLInventoryPanel;
class LLInventorySingleFolderPanel;
class LLPanelMainInventory;

namespace plus
{
namespace invcolumns
{
    // The center column's list of this window, made by plus::invlayout the first time the window shows three
    // columns: its selection and root changes are connected here, once (the list's select signal adds every slot).
    void connectCenter(LLPanelMainInventory* owner, LLInventorySingleFolderPanel* center);

    // Three columns now (between enter() and leave()).
    bool isActive(const LLPanelMainInventory* owner);

    // plus::invlayout decides when the columns come and go, and places them; these are what they mean.
    // enter(): the tab becomes the left column (folders only), the center opens its selected folder. False when
    // the window has no center list (nothing changed).
    bool enter(LLPanelMainInventory* owner, LLInventoryPanel* tab);
    // Every frame in three columns: another tab shown becomes the left column, else the tab's folder view, filter
    // and sort order are followed. False when the center list is gone (the caller leaves).
    bool follow(LLPanelMainInventory* owner, LLInventoryPanel* tab);
    // Back to one column: the left shows its items again and selects what was chosen in the center.
    void leave(LLPanelMainInventory* owner);

    // The selection of one of the window's tabs changed: the center column opens the chosen folder.
    void onSelection(LLPanelMainInventory* owner, LLInventoryPanel* panel, const std::deque<LLFolderViewItem*>& items);

    // In three columns, the center list when it was chosen last (it has the keyboard focus, or the last choice was
    // made there and none in the received items since) and it has a selection, else null: the gear menu's commands
    // (llpanelmaininventory.cpp) and the selection LLAvatarActions reads (llavataractions.cpp) act on it. A choice
    // the focus decides is kept as the last one.
    LLInventoryPanel* chosenCenter(LLPanelMainInventory* owner);

    // Only reading (no choice is noted, the received items are left alone): in three columns, the center's
    // selection when the last choice was made there and it has one, else null. The detail pane made later shows it.
    const std::deque<LLFolderViewItem*>* centerSelection(const LLPanelMainInventory* owner);

    // The view the gear menu's commands act on: chosenCenter(), else the tab shown (getActivePanel()).
    LLInventoryPanel* commandPanel(LLPanelMainInventory* owner);

    // The inventory view is being destroyed: drop what is kept for it.
    void forget(const LLPanelMainInventory* owner);
}
}

#endif // PLUS_INVCOLUMNS_H
