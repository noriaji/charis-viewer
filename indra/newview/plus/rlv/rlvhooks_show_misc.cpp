/**
 * @file rlvhooks_show_misc.cpp
 * @brief rlv-show hooks: E38-E45 slurl, status bar, teleport history, tool pie, viewer window, voavatar (name tags), world map view (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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

#include "llvoavatar.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvdefines.h"
#include "rlvhandler.h"

namespace plus::rlv
{

// Everything here is only called with RLVa on (the official files check enabled() first) and from the main thread.

const std::string& hiddenGenericText()
{
    return RlvStrings::getString(RlvStringKeys::Hidden::Generic);
}

const std::string& hiddenParcelText()
{
    return RlvStrings::getString(RlvStringKeys::Hidden::Parcel);
}

const std::string& hiddenRegionText()
{
    return RlvStrings::getString(RlvStringKeys::Hidden::Region);
}

// ============================================================================
// E38: llslurl.cpp
//

bool hidesSLURLRegion(const std::string& strRegion)
{
    // FS: only a region the agent can see (its own and the neighbours) is hidden
    return gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC) && RlvUtil::isNearbyRegion(strRegion);
}

// ============================================================================
// E42: lltoolpie.cpp
//

bool refusesLocalMove(const LLVector3d& posGlobal)
{
    if (RlvActions::canTeleportToLocal(posGlobal))
    {
        return false;
    }
    RlvUtil::notifyBlocked(RlvStringKeys::Blocked::AutoPilot);
    return true;
}

// ============================================================================
// E44: llvoavatar.cpp (every frame for each avatar with a tag: two bools when nothing is restricted)
//

void nameTagRestrictions(const LLViewerObject* pAvatar, bool& fShowTag, bool& fShowName)
{
    // FS: fRlvShowAvTag = canShowNameTag(this), fRlvShowAvName = fRlvShowAvTag && canShowName(SNC_DEFAULT, id)
    const LLVOAvatar* pAvatarVO = static_cast<const LLVOAvatar*>(pAvatar);
    fShowTag = RlvActions::canShowNameTag(pAvatarVO);
    fShowName = fShowTag && (pAvatarVO) && RlvActions::canShowName(RlvActions::SNC_DEFAULT, pAvatarVO->getID());
}

std::string nameTagAnonym(const std::string& strName)
{
    return RlvStrings::getAnonym(strName);
}

} // namespace plus::rlv
