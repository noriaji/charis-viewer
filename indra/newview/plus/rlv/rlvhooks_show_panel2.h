/**
 * @file rlvhooks_show_panel2.h
 * @brief rlv-show hooks: E30-E37 place profile, places, profile, classifieds/picks, teleport history panel, top info bar, item/task info (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_panel2.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".
// Used here as well (declared elsewhere): canShowMenuLocation() (@showloc), canShowMenuName() (@shownames, wear),
// canShowWorldMap() (@showworldmap), anonymizedName() (comm), canTeleportToLandmarkMenu() (@tplm, panel1).

// E31 LLPanelPlaces::updateVerbs(), E34 LLTeleportHistoryPanel::isActionEnabled(): the "Teleport" button / menu item
bool canTeleportToLocation();   // @tploc (FS RlvActions::canTeleportToLocation())

// E31 LLPanelPlaces::onOpen() / changedParcelSelection(): the agent's place may be shown and kept up to date (@showloc; the
// parcel's owner or a group member with the return power still may: FS' filter of the "agent" panel). False: back to the list.
bool canShowAgentPlaceInfo();

// E33 @showloc was switched on or off: the "New" buttons of the picks and classifieds of every open profile are updated
// (FS LLPanelProfilePicks / LLPanelProfileClassifieds::updateRlvRestrictions()). Called by RlvUIEnabler (rlvui_show.cpp).
void updateProfilePlaceButtons();

// E36 LLSidepanelItemInfo: the creator is anonymized when @shownames hides them and they own the item or are nearby
bool canShowItemCreator(const LLInventoryItem* pItem);
// The "rlvanonym" link of an agent (its label is the anonym, research/248 E5)
std::string anonymizedAgentSlurl(const LLUUID& idAgent);

// E37 LLSidepanelTaskInfo::refresh(): @shownames anonymizes the creator (when they made the whole selection and own it or are
// nearby), the owner and the last owner (not group owned, not the agent) of the selection. Changes the SLURLs in the strings.
void filterSelectionNames(bool fCreatorsIdentical, const LLUUID& idCreator, bool fOwnersIdentical, const LLUUID& idOwner,
                          const LLUUID& idLastOwner, std::string& strCreator, std::string& strOwner);

