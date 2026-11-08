/**
 * @file rlvhooks_show_panel1.cpp
 * @brief rlv-show hooks: E24-E29 netmap, land, landmarks, people, people menus, permissions (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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

#include "llviewerprecompiledheaders.h"

#include "rlvhooks.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llselectmgr.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

namespace plus::rlv
{

// ============================================================================
// E24 llnetmap.cpp
//

std::string netMapAgentLine(const LLUUID& idAgent)
{
    LLAvatarName avName;
    return (idAgent.notNull() && LLAvatarNameCache::get(idAgent, &avName)) ? RlvStrings::getAnonym(avName) + "\n" : std::string();
}

void filterMapRegionName(std::string& strRegionName)
{
    if (!RlvActions::canShowLocation())
        strRegionName = hiddenRegionText();
}

void filterMapParcelName(std::string& strParcelName)
{
    if (!RlvActions::canShowLocation())
        strParcelName = hiddenParcelText();
}

// ============================================================================
// E26 llpanellandmarks.cpp
//

bool canTeleportToLandmarkMenu()
{
    return !RlvActions::hasBehaviour(RLV_BHVR_TPLM);
}

// ============================================================================
// E27 llpanelpeople.cpp, E28 llpanelpeoplemenus.cpp
//

bool canShowNames(const std::vector<LLUUID>& idAgents)
{
    // Most common case first: @shownames is not on
    if (RlvActions::canShowName(RlvActions::SNC_DEFAULT))
        return true;

    for (const LLUUID& idAgent : idAgents)
    {
        if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, idAgent))
            return false;
    }
    return true;
}

bool anonymousNearbyMenuItems(const std::vector<LLUUID>& idAgents, bool fMultiSelection, std::vector<std::string>& items)
{
    if (canShowNames(idAgents))
        return false;

    items.push_back(std::string("offer_teleport"));
    if (!fMultiSelection)
    {
        items.push_back(std::string("request_teleport"));
        items.push_back(std::string("separator_invite_to_group"));
        items.push_back(std::string("zoom_in"));
        items.push_back(std::string("block_unblock"));
    }
    return true;
}

TeleportNamesScope::TeleportNamesScope(bool fCheck, EKind eKind, const std::vector<LLUUID>& idAgents)
    : mContext(-1)
{
    if ( (!fCheck) || (idAgents.empty()) )
        return;

    // A request names the one agent it goes to, an offer every agent it goes to
    const RlvActions::EShowNamesContext eContext = (TELEPORT_REQUEST == eKind) ? RlvActions::SNC_TELEPORTREQUEST : RlvActions::SNC_TELEPORTOFFER;
    const bool fCanShowName = (TELEPORT_REQUEST == eKind) ? RlvActions::canShowName(RlvActions::SNC_DEFAULT, idAgents.front()) : canShowNames(idAgents);
    RlvActions::setShowName(eContext, fCanShowName);
    mContext = eContext;
}

TeleportNamesScope::~TeleportNamesScope()
{
    // The teleport contexts show names unless one of these scopes says otherwise (FS sets them back to true)
    if (mContext >= 0)
        RlvActions::setShowName(static_cast<RlvActions::EShowNamesContext>(mContext), true);
}

// ============================================================================
// E29 llpanelpermissions.cpp
//

bool hidesSelectionCreator(const LLUUID& idCreator)
{
    if ( (RlvActions::canShowName(RlvActions::SNC_DEFAULT)) || (idCreator.isNull()) || (RlvActions::canShowName(RlvActions::SNC_DEFAULT, idCreator)) )
        return false;

    // Only anonymize the creator if they're a nearby avie or also the owner (FS: "mCreatorID == mOwnerID"; Charis' refresh() reads
    // the owner after the creator, so it is read here)
    LLUUID idOwner;
    std::string strOwnerLink;
    LLSelectMgr::getInstance()->selectGetOwner(idOwner, strOwnerLink);
    return !canShowCreatorName(idCreator, idOwner);
}

bool hidesSelectionOwner(const LLUUID& idOwner, bool fLastOwner)
{
    if ( (RlvActions::canShowName(RlvActions::SNC_DEFAULT)) || (idOwner.isNull()) )
        return false;

    // FS anonymizes the last owner whenever @shownames is on, unless it is the user
    if (fLastOwner)
        return idOwner != gAgent.getID();
    return !RlvActions::canShowName(RlvActions::SNC_DEFAULT, idOwner);
}

} // namespace plus::rlv
