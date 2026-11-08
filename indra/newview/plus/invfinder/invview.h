/**
 * @file invview.h
 * @brief Shows ids in the inventory window's "AI Search" tab.
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

// The tab is an ordinary inventory panel (panel_main_inventory.xml, the
// <plus> block "AI Search"); its filter gets an id set (LLInventoryFilter::
// setPlusIdSet), so it looks and works like a search result: the matches in
// their folders, with the usual menus, drag and drop, and the search field of
// the inventory window narrowing them further. Nothing in the inventory changes.
//
// Main thread only.

#ifndef PLUS_INVFINDER_INVVIEW_H
#define PLUS_INVFINDER_INVVIEW_H

#include <cstddef>
#include <string>
#include <vector>

namespace plus::invfinder
{
    struct ShowResult
    {
        std::size_t shown = 0; // ids handed to the tab (some may not be in the viewer's inventory)
        std::string error;     // non-empty = the tab could not be used
    };

    // Opens the inventory window on the AI Search tab, showing exactly these ids
    // (item and folder ids as UUID text) and the folders above them.
    ShowResult showInInventory(const std::vector<std::string>& ids);

    // Empties the tab (it shows nothing) and leaves it where it is.
    ShowResult clearInventoryTab();
}

#endif // PLUS_INVFINDER_INVVIEW_H
