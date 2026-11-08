/**
 * @file rlvhooks_show_panel1.h
 * @brief rlv-show hooks: E24-E29 netmap, land, landmarks, people, people menus, permissions (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_panel1.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".
// For one agent's name these files also use canShowMenuName() and anonymizedName(), for the location canShowMenuLocation()
// and for the world map canShowWorldMap() (rlvhooks.h).

// ---- E24 llnetmap.cpp
// handleToolTip(): the line that names the hidden agent under the cursor anonymously ("<anonym>\n"; FS' [AGENT]).
// Empty while the agent's name is not in the name cache (as in FS).
std::string netMapAgentLine(const LLUUID& idAgent);
// handleToolTip(): @showloc - the region / parcel name becomes the "hidden" text.
void filterMapRegionName(std::string& strRegionName);
void filterMapParcelName(std::string& strParcelName);

// ---- E26 llpanellandmarks.cpp
// isActionEnabled() "teleport": @tplm
bool canTeleportToLandmarkMenu();

// ---- E27 llpanelpeople.cpp, E28 llpanelpeoplemenus.cpp
// @shownames: every one of the agents may be shown by name (true for an empty list).
bool canShowNames(const std::vector<LLUUID>& idAgents);
// NearbyPeopleContextMenu::buildContextMenu(): when a selected agent's name is hidden, fills items with what is left
// (offering / requesting a teleport, zooming in, blocking) and returns true. False: the menu is built as usual.
bool anonymousNearbyMenuItems(const std::vector<LLUUID>& idAgents, bool fMultiSelection, std::vector<std::string>& items);
// PeopleContextMenu::requestTeleport() / offerTeleport(): while it lives, the names that the teleport request / offer
// shows follow @shownames for the agents of the nearby list (FS: RlvActions::setShowName(SNC_TELEPORTREQUEST /
// SNC_TELEPORTOFFER)). Puts the name context back on every path. fCheck false (RLVa off, or another list): does nothing.
class TeleportNamesScope
{
public:
    enum EKind { TELEPORT_OFFER, TELEPORT_REQUEST };
    TeleportNamesScope(bool fCheck, EKind eKind, const std::vector<LLUUID>& idAgents);
    ~TeleportNamesScope();
    TeleportNamesScope(const TeleportNamesScope&) = delete;
    TeleportNamesScope& operator=(const TeleportNamesScope&) = delete;
private:
    int mContext; // RlvActions::EShowNamesContext that was set, -1 for none
};

// ---- E29 llpanelpermissions.cpp
// refresh(): @shownames - the creator shown for the selection is hidden (one who is nearby, or who owns the selection)
bool hidesSelectionCreator(const LLUUID& idCreator);
// refresh(): @shownames - the owner shown for an agent-owned selection is hidden. fLastOwner: it is the last owner of a
// public object (FS hides that one whenever @shownames is on, unless it is the user).
bool hidesSelectionOwner(const LLUUID& idOwner, bool fLastOwner);
