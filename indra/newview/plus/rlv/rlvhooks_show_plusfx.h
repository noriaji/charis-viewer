/**
 * @file rlvhooks_show_plusfx.h
 * @brief rlv-show hooks: plus features (radar, areasearch, avatarattach, subtitle, poser, otherland, slwx, tplag, posermcp, phototoolsfloater) (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_plusfx.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".
//
// The names and places the plus features show (research/248 3.4). Whether one avatar's name may be shown is
// canShowMenuName(), whether the location may be is canShowMenuLocation() (both in the wear section of rlvhooks.h).

// @shownames: the name to show for an avatar in a plus feature (a list, a window's title, the clipboard, speech):
// strName itself, or its anonym when the avatar's name may not be shown (the agent and the @shownames exceptions are
// shown). The anonym is made from the name cache's legacy name when the cache has it (the same anonym chat and the
// name tags give), else from strName.
std::string avatarNameForPlus(const LLUUID& idAgent, const std::string& strName);
// The same for a nearby avatar only (as RlvUtil::filterNames() hides the names in chat): the voice speakers of the
// subtitles, who may be in a group call.
std::string nearbyAvatarNameForPlus(const LLUUID& idAgent, const std::string& strName);
// @shownames / @showloc: the nearby avatars' names and the names of the regions and the parcel around taken out of a
// text that may hold them (the subtitles and their translation).
void filterTextForPlus(std::string& strText);
// @showloc: strRegion itself, or the "hidden region" text.
std::string regionNameForPlus(const std::string& strRegion);

// The plus lists that keep names (the radar, the area search) make them again. For rlvui_show.cpp: when @shownames is
// switched on or off and when its exceptions change. Main thread.
void refreshPlusNames();
