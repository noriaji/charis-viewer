/**
 * @file rlvhooks_show_panel2.cpp
 * @brief rlv-show hooks: E30-E37 place profile, places, profile, classifieds/picks, teleport history panel, top info bar, item/task info (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llinventory.h"
#include "llpanelprofileclassifieds.h"
#include "llpanelprofilepicks.h"
#include "llpermissions.h"
#include "llselectmgr.h"
#include "llslurl.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
#include "rlvui.h"

namespace plus::rlv
{

// ---- E31, E34: teleport buttons and menu items (a landmark: canTeleportToLandmarkMenu(), panel1)

bool canTeleportToLocation()
{
    return RlvActions::canTeleportToLocation();
}

// ---- E31: the agent's place in the Places panel (FS: RlvUIEnabler::filterPanelShowLoc())

bool canShowAgentPlaceInfo()
{
    return RlvUIEnabler::canViewParcelProperties();
}

// ---- E33: picks and classifieds of the own profile

void updateProfilePlaceButtons()
{
    // FS connects every panel to the behaviour signal; here the open profiles are looked up when @showloc switches.
    // updateButtons() does nothing for another resident's profile.
    for (LLFloater* pFloater : LLFloaterReg::getFloaterList("profile"))
    {
        if (!pFloater)
            continue;
        if (LLPanelProfilePicks* pPicks = pFloater->findChild<LLPanelProfilePicks>("panel_profile_picks"))
            pPicks->updateButtons();
        if (LLPanelProfileClassifieds* pClassifieds = pFloater->findChild<LLPanelProfileClassifieds>("panel_profile_classifieds"))
            pClassifieds->updateButtons();
    }
}

// ---- E36: item properties

bool canShowItemCreator(const LLInventoryItem* pItem)
{
    if (!pItem)
        return true;

    const LLUUID& idCreator = pItem->getCreatorUUID();
    if (RlvActions::canShowName(RlvActions::SNC_DEFAULT, idCreator))
        return true;

    // If the object creator matches the object owner we need to anonymize the creator field as well
    const LLPermissions& perm = pItem->getPermissions();
    return canShowCreatorName(idCreator, ( (perm.isOwned()) && (!perm.isGroupOwned()) ) ? perm.getOwner() : LLUUID::null);
}

std::string anonymizedAgentSlurl(const LLUUID& idAgent)
{
    return LLSLURL("agent", idAgent, "rlvanonym").getSLURLString();
}

// ---- E37: object properties

void filterSelectionNames(bool fCreatorsIdentical, const LLUUID& idCreator, bool fOwnersIdentical, const LLUUID& idOwner,
                          const LLUUID& idLastOwner, std::string& strCreator, std::string& strOwner)
{
    if (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES))
        return;

    // Only anonymize the creator if all of the selection was created by the same avie who's also the owner or they're a nearby avie
    if ( (fCreatorsIdentical) && (idCreator != gAgentID) && ((idCreator == idOwner) || (RlvUtil::isNearbyAgent(idCreator))) )
        strCreator = anonymizedAgentSlurl(idCreator);

    // Only anonymize the owner name if all of the selection is owned by the same avie and isn't group owned
    const bool fGroupOwned = LLSelectMgr::getInstance()->selectIsGroupOwned();
    if ( (fOwnersIdentical) && (!fGroupOwned) && (idOwner != gAgentID) )
    {
        strOwner = anonymizedAgentSlurl(idOwner);
    }
    else if ( (idOwner.isNull()) && (!fGroupOwned) && (idLastOwner.notNull()) && (idLastOwner != gAgentID) )
    {
        // Charis: the last owner that a public object shows ("..., last <name>") is anonymized as well (FS shows it)
        LLStringUtil::replaceString(strOwner, LLSLURL("agent", idLastOwner, "inspect").getSLURLString(), anonymizedAgentSlurl(idLastOwner));
    }
}

}
