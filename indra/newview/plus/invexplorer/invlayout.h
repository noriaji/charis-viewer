/**
 * @file invlayout.h
 * @brief Inventory window like a file explorer: where the columns of a wide window are (the
 *        tab's contents, the center column, the splits) and when they come.
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

#ifndef PLUS_INVLAYOUT_H
#define PLUS_INVLAYOUT_H

class LLPanelMainInventory;

namespace plus
{
namespace invlayout
{
    // Once per frame (through plus::invexplorer::tick, before the window draws its children): shows or
    // hides the detail pane and enters or leaves the three columns (plus::invcolumns) by the window's
    // width, and places the row between the tab row and the received items: the tree, the center, the
    // detail and the splits between them (research/318 7). Also shows the detail's button at the end of
    // the search row while the window is wide enough for the detail (research/318 8).
    void tick(LLPanelMainInventory* owner);

    // The inventory view is being destroyed: drop what is kept for it.
    void forget(const LLPanelMainInventory* owner);
}
}

#endif // PLUS_INVLAYOUT_H
