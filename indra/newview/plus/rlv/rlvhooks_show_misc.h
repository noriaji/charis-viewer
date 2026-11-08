/**
 * @file rlvhooks_show_misc.h
 * @brief rlv-show hooks: E38-E45 slurl, status bar, teleport history, tool pie, viewer window, voavatar (name tags), world map view (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_misc.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".
// These files also use canShowMenuName() / canShowMenuLocation() (wear, E49) and anonymizedName() (comm).

// The texts that stand for what @showloc hides (RlvStrings "hidden_generic", "hidden_parcel", "hidden_region")
const std::string& hiddenGenericText();
const std::string& hiddenParcelText();
const std::string& hiddenRegionText();

// LLSLURL::getSLURLString() (llslurl.cpp, E38): @showloc - true when the region is one of the agent's own or its
// neighbours; the SLURL then names the hidden region instead of the region and the position.
bool hidesSLURLRegion(const std::string& strRegion);

// LLToolPie::walkToClickedLocation() / teleportToClickedLocation() (lltoolpie.cpp, E42): @tplocal - true = refused, the
// user was told ("blocked_autopilot").
bool refusesLocalMove(const LLVector3d& posGlobal);

// LLVOAvatar::idleUpdateNameTag() (llvoavatar.cpp, E44), every frame for each avatar whose tag may show: @shownametags
// (fShowTag: the tag may be shown) and @shownames (fShowName: its name and group title may be shown). pAvatar: the
// LLVOAvatar.
void nameTagRestrictions(const LLViewerObject* pAvatar, bool& fShowTag, bool& fShowName);
// LLVOAvatar::idleUpdateNameTagText() (E44): what a @shownames tag shows instead of the name (strName: the legacy name, or
// the full name with display names off, as FS)
std::string nameTagAnonym(const std::string& strName);

