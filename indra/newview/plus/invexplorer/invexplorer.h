/**
 * @file invexplorer.h
 * @brief Inventory window like a file explorer: a detail pane on the right that
 *        shows the selected item and appears when the window is wide enough.
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

class LLFolderViewItem;
class LLPanelMainInventory;

namespace plus
{
namespace invexplorer
{
    // The selection of an inventory view changed (LLPanelMainInventory::onSelectionChange).
    void onSelection(LLPanelMainInventory* owner, const std::deque<LLFolderViewItem*>& items);

    // Once per frame (LLPanelMainInventory::draw): shows the detail pane while the window
    // is wide enough (PlusInvExplorerMinWidth) and PlusInvExplorerDetail is on.
    void tick(LLPanelMainInventory* owner);
}
}

#endif // PLUS_INVEXPLORER_H
