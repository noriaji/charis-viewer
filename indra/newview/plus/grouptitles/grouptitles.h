/**
 * @file grouptitles.h
 * @brief Group titles: the active group title, and the one given to each region ("Auto Region")
 *        (plus/grouptitles, from Firestorm's FSGroupTitleRegionMgr and FSFloaterGroupTitles).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 The Phoenix Firestorm Project, Inc.
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// A group title is a group and one of its roles (the role's title shows over the avatar); the null group
// is "no group". Auto Region gives a title to a region by name: entering that region (login, teleport,
// crossing) makes it the active one, and with "none in unassigned regions" a region without one makes no
// group active. Kept per account in region_group_titles.xml, Firestorm's file and format, so a copied file
// works in both viewers. Main thread only; nothing runs per frame (the agent's region-changed signal).

#ifndef PLUS_GROUPTITLES_H
#define PLUS_GROUPTITLES_H

#include "lluuid.h"

#include <boost/signals2.hpp>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace plus::grouptitles
{
    // Makes group_id's role_id title the active one: the title is sent, and the group activated unless it is
    // the active one already. A null group_id is "no group" (no title sent). Refused (false, nothing sent)
    // while RLVa holds the active group (plus::rlv::canChangeActiveGroup()).
    bool activateTitle(const LLUUID& group_id, const LLUUID& role_id);

    // ---- Auto Region ---------------------------------------------------------------------------
    // The changes below do nothing before the account's file was read (login), so they never write over it.
    // Gives the title to the region (a name as typed or as the grid has it; the one the title had there
    // before is replaced). Saved at once. An empty name does nothing.
    void setAssignment(const LLUUID& group_id, const LLUUID& role_id, const std::string& region_name);
    // The same for the region the agent is in.
    void setAssignmentForCurrentRegion(const LLUUID& group_id, const LLUUID& role_id);
    // Asks for a region's name (a dialog) and gives the title to it once the grid knows the region
    // (the map's name lookup, waited for 10 seconds at most); else says it was not found.
    void askRegionName(const LLUUID& group_id, const LLUUID& role_id);
    // Takes the title away from every region / takes the title of one region away.
    void clearAssignment(const LLUUID& group_id, const LLUUID& role_id);
    void clearAssignmentByRegion(const std::string& region_name);

    // The regions given to the title, as the grid spells them (sorted by name).
    std::vector<std::string> regionsForTitle(const LLUUID& group_id, const LLUUID& role_id);
    // The same, joined with ", " (the window's column).
    std::string regionListForTitle(const LLUUID& group_id, const LLUUID& role_id);
    // Every title (group, role) given to some region, each once: the window shows those of groups the agent
    // is not in (left, or not received yet), so that they can be cleared.
    std::vector<std::pair<LLUUID, LLUUID>> assignedTitles();

    bool noneOnUnassigned();
    void setNoneOnUnassigned(bool none);

    // Called after any change of the assignments (not of noneOnUnassigned).
    boost::signals2::connection onAssignmentsChanged(const std::function<void()>& callback);

    // From the feature's init (grouptitlesfloater.cpp): loads the account's file once its folder is
    // known during login, and follows the agent's region from then on.
    void startAutoRegion();
}

#endif // PLUS_GROUPTITLES_H
