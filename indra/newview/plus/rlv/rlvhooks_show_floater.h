/**
 * @file rlvhooks_show_floater.h
 * @brief rlv-show hooks: E15-E23 inspect, reporter, world map floater, give inventory, hud look-at/text, inspectors, location bar (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_floater.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".


// Also used by these files (declared elsewhere): canShowMenuName() / canShowMenuLocation() (wear, E49) for "@shownames
// does not hide this agent" / "@showloc is not set", anonymizedName() (comm) for the anonym of an agent,
// hiddenRegionText() / hiddenGenericText() (show_misc) for the texts that stand for a hidden place.

// llfloaterinspect.cpp (E15), llinspectobject.cpp (E21): the creator of an object may be named. Under @shownames a
// creator is hidden only when he is also the owner or is nearby (an unrelated creator says nothing about who is here).
bool canShowCreatorName(const LLUUID& idCreator, const LLUUID& idOwner);

// llfloaterworldmap.cpp draw() (E17): false while both @tplm and @tploc are set (the "Go Home" button is disabled)
bool canGoHomeFromWorldMap();

// llgiveinventory.cpp (E18): the recipient of a give is not named (@shownames, nearby, the profile is not open).
// Then the recipient is not added to the recent people, and the give is told in nearby chat with the anonym
// (logHiddenGiveOffer) instead of in the recipient's IM session / IM log.
bool hidesGiveRecipient(const LLUUID& idAgent);
void logHiddenGiveOffer(const LLUUID& idAgent, const std::string& strItemName, bool fFolder);

// llhudtext.cpp LLHUDText::setString() (E20): the floating text with the names (@shownames) and the places (@showloc)
// hidden. Returns strText itself when nothing is restricted, else a buffer that is reused (valid until the next call;
// main thread only). The original text stays in LLViewerObject::mHudText (F5).
const std::string& filterHoverText(const std::string& strText);
