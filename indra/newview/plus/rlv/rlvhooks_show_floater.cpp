/**
 * @file rlvhooks_show_floater.cpp
 * @brief rlv-show hooks: E15-E23 inspect, reporter, world map floater, give inventory, hud look-at/text, inspectors, location bar (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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

#include "llavatarnamecache.h"
#include "llimview.h"
#include "lltrans.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvui.h"

namespace plus::rlv
{

// ============================================================================
// Names (E15, E21)
//

bool canShowCreatorName(const LLUUID& idCreator, const LLUUID& idOwner)
{
    if (idCreator.isNull())             // nobody to hide (RlvUtil::isNearbyAgent() asserts on null)
        return true;
    // Only anonymize the creator if they're also the owner or if they're a nearby avie
    return (RlvActions::canShowName(RlvActions::SNC_DEFAULT, idCreator)) || ( (idOwner != idCreator) && (!RlvUtil::isNearbyAgent(idCreator)) );
}

// ============================================================================
// Places (E17; the hidden texts of E16, E17, E22 are hiddenRegionText() / hiddenGenericText() of show_misc)
//

bool canGoHomeFromWorldMap()
{
    return !( (RlvActions::hasBehaviour(RLV_BHVR_TPLM)) && (RlvActions::hasBehaviour(RLV_BHVR_TPLOC)) );
}

// ============================================================================
// Giving inventory (E18)
//

bool hidesGiveRecipient(const LLUUID& idAgent)
{
    // An in-world drop on a nearby avatar (as opposed to a drop on an IM session or on a profile)
    return (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, idAgent)) && (RlvUtil::isNearbyAgent(idAgent)) && (!RlvUIEnabler::hasOpenProfile(idAgent));
}

void logHiddenGiveOffer(const LLUUID& idAgent, const std::string& strItemName, bool fFolder)
{
    // Log to the nearby chat history (not to the recipient's IM session or IM log) to avoid revealing the name of the recipient
    LLSD sdArgs;
    sdArgs["ITEM_NAME"] = strItemName;

    // FS' "inventory_item_offered_rlv" ("Inventory item offered to [NAME]", strings.xml) when the name cache has the
    // recipient; without it (or without that string) the text says what was given but not to whom
    std::string strMsgName = (fFolder) ? "inventory_folder_offered-im" : "inventory_item_offered-im";
    LLAvatarName avName;
    std::string strUnused;
    if ( (LLAvatarNameCache::get(idAgent, &avName)) && (LLTrans::findString(strUnused, "inventory_item_offered_rlv")) )
    {
        sdArgs["NAME"] = RlvStrings::getAnonym(avName);
        strMsgName = "inventory_item_offered_rlv";
    }
    gIMMgr->addSystemMessage(LLUUID::null, strMsgName, sdArgs);
}

// ============================================================================
// Floating text (E20)
//

const std::string& filterHoverText(const std::string& strText)
{
    // NOTE: setString() is called for debug and map beacons as well
    // (FS also hides the whole text for @showhovertext* and clears every nearby name for @shownearby: stage 3, research/248 1)
    const bool fCanShowLocation = RlvActions::canShowLocation();
    const bool fCanShowNames = RlvActions::canShowName(RlvActions::SNC_DEFAULT);
    if ( (fCanShowLocation) && (fCanShowNames) )
        return strText;

    static std::string s_strFiltered;   // reused: keeps its capacity
    s_strFiltered = strText;
    if (!fCanShowLocation)
        RlvUtil::filterLocation(s_strFiltered);
    if (!fCanShowNames)
        RlvUtil::filterNames(s_strFiltered, true, false);
    return s_strFiltered;
}

} // namespace plus::rlv
