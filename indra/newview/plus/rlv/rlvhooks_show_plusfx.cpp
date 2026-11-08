/**
 * @file rlvhooks_show_plusfx.cpp
 * @brief rlv-show hooks: plus features (radar, areasearch, avatarattach, subtitle, poser, otherland, slwx, tplag, posermcp, phototoolsfloater) (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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

#include "llavatarname.h"
#include "llavatarnamecache.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvdefines.h"
#include "rlvhandler.h"

#include "plus/areasearch/areasearch.h"
#include "plus/radar/radar.h"

namespace plus::rlv
{

// Everything here is only called with RLVa on (the plus features check enabled() first) and from the main thread.
// FS has these in the windows themselves (FSRadarEntry::getRadarName(), RLVa_hideNameIfRestricted() of fsareasearch.cpp);
// the plus features call them instead.

std::string avatarNameForPlus(const LLUUID& idAgent, const std::string& strName)
{
    if (RlvActions::canShowName(RlvActions::SNC_DEFAULT, idAgent))
    {
        return strName;
    }
    LLAvatarName avName;
    return (LLAvatarNameCache::get(idAgent, &avName)) ? RlvStrings::getAnonym(avName) : RlvStrings::getAnonym(strName);
}

std::string nearbyAvatarNameForPlus(const LLUUID& idAgent, const std::string& strName)
{
    return (RlvUtil::isNearbyAgent(idAgent)) ? avatarNameForPlus(idAgent, strName) : strName;
}

void filterTextForPlus(std::string& strText)
{
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES))
    {
        RlvUtil::filterNames(strText);
    }
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        RlvUtil::filterLocation(strText);
    }
}

std::string regionNameForPlus(const std::string& strRegion)
{
    return (RlvActions::canShowLocation()) ? strRegion : RlvStrings::getString(RlvStringKeys::Hidden::Region);
}

void refreshPlusNames()
{
    // FS: FSRadar::updateNames() (RlvBehaviourToggleHandler<RLV_BHVR_SHOWNAMES>) and FSAreaSearch::updateRlvRestrictions().
    // The other plus windows that show names read them again on their own (each refresh, or when they are opened).
    plus::radar::refreshNames();
    plus::areasearch::refreshNames();
}

} // namespace plus::rlv
