/**
 *
 * Copyright (c) 2009-2020, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 */

#include "llviewerprecompiledheaders.h"
#include "llagent.h"
#include "llimview.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "llworld.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhelper.h"
#include "rlvhandler.h"
#include "rlvlocks.h"

// ============================================================================
// Communication/Avatar interaction
//

bool RlvActions::s_BlockNamesContexts[SNC_COUNT] = { 0 };

// Position of an avatar the viewer knows about: the avatar object if it's in view, otherwise its coarse
// (minimap) location (FS adds LLWorld::getAvatar() for this; the official LLWorld doesn't have it)
static bool rlvGetAvatarPosition(const LLUUID& idAvatar, LLVector3d& posAvatar)
{
    // (FS skips dummy and orphaned avatars; the official LLVOAvatar's preview avatars are isUIAvatar())
    const LLViewerObject* pObj = gObjectList.findObject(idAvatar);
    if ( (pObj) && (pObj->isAvatar()) && (!pObj->isDead()) && (!pObj->isOrphaned()) )
    {
        const LLVOAvatar* pAvatar = static_cast<const LLVOAvatar*>(pObj);
        if ( (!pAvatar->isUIAvatar()) && (!pAvatar->isControlAvatar()) )
        {
            posAvatar = pAvatar->getPositionGlobal();
            return true;
        }
    }

    uuid_vec_t idAgents; std::vector<LLVector3d> posAgents;
    LLWorld::getInstance()->getAvatars(&idAgents, &posAgents);
    for (size_t idxAgent = 0, cntAgent = llmin(idAgents.size(), posAgents.size()); idxAgent < cntAgent; idxAgent++)
    {
        if (idAgents[idxAgent] == idAvatar)
        {
            posAvatar = posAgents[idxAgent];
            return true;
        }
    }
    return false;
}

// Little helper function to check the IM exclusion range for @recvim, @sendim and @startim (returns: min_dist <= (pos user - pos target) <= max_dist)
static bool rlvCheckAvatarIMDistance(const LLUUID& idAvatar, ERlvBehaviourModifier eModDistMin, ERlvBehaviourModifier eModDistMax)
{
    //                      | no limits | minimum set | min+max set |       !fHasMin | !fHasMax = INF | fHasMax
    // --------------------------------------------------------------       ------------------------------------
    // dist <= min  <= max  | block     | block       | block       |           F    |  F             |    F
    // min  <= dist <= max  | block     | allow       | allow       |           F    |  T             |    T
    // min  <= max  <= dist | block     | allow       | block       |           F    |  ^ (see above) |    F
    // off-region           | block     | allow       | block       |           F    |  ^ (see above) |    F

    const RlvBehaviourModifier *pBhvrModDistMin = RlvBehaviourDictionary::instance().getModifier(eModDistMin), *pBhvrModDistMax = RlvBehaviourDictionary::instance().getModifier(eModDistMax);
    if ( (pBhvrModDistMin) && (pBhvrModDistMax) && (pBhvrModDistMin->hasValue()) )
    {
        LLVector3d posAgent; bool fHasMax = pBhvrModDistMax->hasValue();
        float nMinDist = pBhvrModDistMin->getValue<float>(), nMaxDist = (fHasMax) ? pBhvrModDistMax->getValue<float>() : std::numeric_limits<float>::max();
        float nDist = (rlvGetAvatarPosition(idAvatar, posAgent)) ? (float)llabs(dist_vec_squared(gAgent.getPositionGlobal(), posAgent)) : std::numeric_limits<float>::max();
        return (nMinDist < nMaxDist) && (nMinDist <= nDist) && (nDist <= nMaxDist);
    }
    return false;
}

bool RlvActions::canReceiveIM(const LLUUID& idSender)
{
    // User can receive an IM from "sender" (could be an agent or a group) if:
    //   - not generally restricted from receiving IMs (or the sender is an exception or inside the exclusion range)
    //   - not specifically restricted from receiving an IM from the sender
    return
        (!isRlvEnabled()) ||
        ( ( (!gRlvHandler.hasBehaviour(RLV_BHVR_RECVIM)) || (gRlvHandler.isException(RLV_BHVR_RECVIM, idSender)) || (rlvCheckAvatarIMDistance(idSender, RLV_MODIFIER_RECVIMDISTMIN, RLV_MODIFIER_RECVIMDISTMAX)) ) &&
          ( (!gRlvHandler.hasBehaviour(RLV_BHVR_RECVIMFROM)) || (!gRlvHandler.isException(RLV_BHVR_RECVIMFROM, idSender)) ) );
}

bool RlvActions::canSendChannel(int nChannel)
{
    return
        ( (!gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHANNEL)) || (gRlvHandler.isException(RLV_BHVR_SENDCHANNEL, nChannel)) ) &&
        ( (!gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHANNELEXCEPT)) || (!gRlvHandler.isException(RLV_BHVR_SENDCHANNELEXCEPT, nChannel)) );
}

bool RlvActions::canSendIM(const LLUUID& idRecipient)
{
    // User can send an IM to "recipient" (could be an agent or a group) if:
    //   - not generally restricted from sending IMs (or the recipient is an exception or inside the exclusion range)
    //   - not specifically restricted from sending an IM to the recipient
    return
        (!isRlvEnabled()) ||
        ( ( (!gRlvHandler.hasBehaviour(RLV_BHVR_SENDIM)) || (gRlvHandler.isException(RLV_BHVR_SENDIM, idRecipient)) || (rlvCheckAvatarIMDistance(idRecipient, RLV_MODIFIER_SENDIMDISTMIN, RLV_MODIFIER_SENDIMDISTMAX)) ) &&
          ( (!gRlvHandler.hasBehaviour(RLV_BHVR_SENDIMTO)) || (!gRlvHandler.isException(RLV_BHVR_SENDIMTO, idRecipient)) ) );
}

// Handles: @redirchat
bool RlvActions::canSendTypingStart()
{
    // The CHAT_TYPE_START indicator can be sent if:
    //   - nearby chat isn't being redirected
    //   - the user specifically indicated that they want to show typing under @redirchat
    return !RlvHandler::instance().hasBehaviour(RLV_BHVR_REDIRCHAT) || RlvSettings::getShowRedirectChatTyping();
}

bool RlvActions::canStartIM(const LLUUID& idRecipient, bool fIgnoreOpen)
{
    // User can start an IM session with "recipient" (could be an agent or a group) if:
    //   - not generally restricted from starting IM sessions (or the recipient is an exception or inside the exclusion range)
    //   - not specifically restricted from starting an IM session with the recipient
    //   - the session already exists
    return
        (!isRlvEnabled()) ||
        ( ( (!gRlvHandler.hasBehaviour(RLV_BHVR_STARTIM)) || (gRlvHandler.isException(RLV_BHVR_STARTIM, idRecipient)) || (rlvCheckAvatarIMDistance(idRecipient, RLV_MODIFIER_STARTIMDISTMIN, RLV_MODIFIER_STARTIMDISTMAX)) ) &&
          ( (!gRlvHandler.hasBehaviour(RLV_BHVR_STARTIMTO)) || (!gRlvHandler.isException(RLV_BHVR_STARTIMTO, idRecipient)) ) ) ||
        ( (!fIgnoreOpen) && ((hasOpenP2PSession(idRecipient)) || (hasOpenGroupSession(idRecipient))) );
}

bool RlvActions::canShowName(EShowNamesContext eContext, const LLUUID& idAgent)
{
    // Handle most common case upfront
    if ( (eContext >= SNC_COUNT) || (!s_BlockNamesContexts[eContext]) )
        return true;

    if (idAgent.notNull())
    {
        switch (eContext)
        {
            // Show/hide avatar name
            case SNC_DEFAULT:
            case SNC_TELEPORTOFFER:
            case SNC_TELEPORTREQUEST:
                return gRlvHandler.isException(RLV_BHVR_SHOWNAMES, idAgent) || (gAgentID == idAgent);
            default:
                return false;
        }
    }
    return false;
}

bool RlvActions::canShowNameTag(const LLVOAvatar* pAvatar)
{
    // An avatar's name tag can be shown if:
    //   - not restricted from seeing avatars' name tags
    //   - OR the avatar is a @shownametags exception
    //   - OR the avatar is within the distance that nametags can be shown
    if ( (!pAvatar) || (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMETAGS)) || (gRlvHandler.isException(RLV_BHVR_SHOWNAMETAGS, pAvatar->getID())) || (gAgentID == pAvatar->getID()) )
        return true;

    const RlvBehaviourModifier* pDistModifier = RlvBehaviourDictionary::instance().getModifier(RLV_MODIFIER_SHOWNAMETAGSDIST);
    const F32 nShowNameTagsDist = (pDistModifier) ? pDistModifier->getValue<F32>() : 0.f;
    return (nShowNameTagsDist != 0.f) && (dist_vec_squared(pAvatar->getPositionGlobal(), gAgent.getPositionGlobal()) < nShowNameTagsDist * nShowNameTagsDist);
}

// Handles: @chatwhisper, @chatnormal and @chatshout
EChatType RlvActions::checkChatVolume(EChatType chatType)
{
    // In vs Bhvr | whisper |  normal |  shout  | n+w     |   n+s   |  s+w   |  s+n+w  |
    // ---------------------------------------------------------------------------------
    // whisper    | normal  | -       | -       | normal  | -       | normal | normal  |
    // normal     | -       | whisper | -       | whisper | whisper | -      | whisper |
    // shout      | -       | whisper | normal  | whisper | whisper | normal | whisper |

    RlvHandler& rlvHandler = RlvHandler::instance();
    if ( ((CHAT_TYPE_SHOUT == chatType) || (CHAT_TYPE_NORMAL == chatType)) && (rlvHandler.hasBehaviour(RLV_BHVR_CHATNORMAL)) )
        chatType = CHAT_TYPE_WHISPER;
    else if ( (CHAT_TYPE_SHOUT == chatType) && (rlvHandler.hasBehaviour(RLV_BHVR_CHATSHOUT)) )
        chatType = CHAT_TYPE_NORMAL;
    else if ( (CHAT_TYPE_WHISPER == chatType) && (rlvHandler.hasBehaviour(RLV_BHVR_CHATWHISPER)) )
        chatType = CHAT_TYPE_NORMAL;
    return chatType;
}

// ============================================================================
// Inventory
//

bool RlvActions::canPasteInventory(const LLInventoryCategory* pSourceCat, const LLInventoryCategory* pDestCat)
{
    // The user can paste the specified object into the destination if:
    //   - the source and destination are subject to the same lock type (or none at all) => NOTE: this happens to be the same logic we use for moving
    return (!isRlvEnabled()) ||
        ( (pSourceCat) && (pDestCat) && ((!RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ANY)) || (RlvFolderLocks::instance().canMoveFolder(pSourceCat->getUUID(), pDestCat->getUUID()))) );
}

bool RlvActions::canPasteInventory(const LLInventoryItem* pSourceItem, const LLInventoryCategory* pDestCat)
{
    // The user can paste the specified object into the destination if:
    //   - the source and destination are subject to the same lock type (or none at all) => NOTE: this happens to be the same logic we use for moving
    return (!isRlvEnabled()) ||
        ( (pSourceItem) && (pDestCat) && ((!RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ANY)) || (RlvFolderLocks::instance().canMoveItem(pSourceItem->getUUID(), pDestCat->getUUID()))) );
}

// ============================================================================
// Movement
//

bool RlvActions::canAcceptTpOffer(const LLUUID& idSender)
{
    return ((!gRlvHandler.hasBehaviour(RLV_BHVR_TPLURE)) || (gRlvHandler.isException(RLV_BHVR_TPLURE, idSender))) && (canStand());
}

bool RlvActions::autoAcceptTeleportOffer(const LLUUID& idSender)
{
    return ((idSender.notNull()) && (gRlvHandler.isException(RLV_BHVR_ACCEPTTP, idSender))) || (gRlvHandler.hasBehaviour(RLV_BHVR_ACCEPTTP));
}

bool RlvActions::canAcceptTpRequest(const LLUUID& idSender)
{
    return (!gRlvHandler.hasBehaviour(RLV_BHVR_TPREQUEST)) || (gRlvHandler.isException(RLV_BHVR_TPREQUEST, idSender));
}

bool RlvActions::autoAcceptTeleportRequest(const LLUUID& idRequester)
{
    return ((idRequester.notNull()) && (gRlvHandler.isException(RLV_BHVR_ACCEPTTPREQUEST, idRequester))) || (gRlvHandler.hasBehaviour(RLV_BHVR_ACCEPTTPREQUEST));
}

// ============================================================================
// Teleporting
//

bool RlvActions::canTeleportToLocal(const LLVector3d& posGlobal)
{
    // User can initiate a local teleport if:
    //   - can stand up (or isn't sitting)
    //   - not restricted from "sit teleporting" (or the destination is within the allowed xyz-radius)
    //   - not restricted from teleporting locally (or the destination is within the allowed xy-radius)
    // NOTE: if we're teleporting due to an active command we should disregard any restrictions from the same object
    const LLUUID& idRlvObjExcept = gRlvHandler.getCurrentObject();
    bool fCanTeleport = RlvActions::canStand(idRlvObjExcept);
    // RLV-SIT: if (fCanTeleport && gRlvHandler.hasBehaviourExcept(RLV_BHVR_SITTP, idRlvObjExcept)) the destination must be
    //          within RLV_MODIFIER_SITTPDIST (xyz distance) of the avatar
    if ( (fCanTeleport) && (gRlvHandler.hasBehaviourExcept(RLV_BHVR_TPLOCAL, idRlvObjExcept)) )
    {
        const F32 nDistSq = (LLVector2((F32)posGlobal.mdV[VX], (F32)posGlobal.mdV[VY]) - LLVector2((F32)gAgent.getPositionGlobal().mdV[VX], (F32)gAgent.getPositionGlobal().mdV[VY])).lengthSquared();
        const F32 nTpLocalDist = llmin(RlvBehaviourDictionary::instance().getModifier(RLV_MODIFIER_TPLOCALDIST)->getValue<float>(), RLV_MODIFIER_TPLOCAL_DEFAULT);
        fCanTeleport = nDistSq < nTpLocalDist * nTpLocalDist;
    }
    return fCanTeleport;
}

bool RlvActions::canTeleportToLocation()
{
    // NOTE: if we're teleporting due to an active command we should disregard any restrictions from the same object
    const LLUUID& idRlvObjExcept = gRlvHandler.getCurrentObject();
    return (!gRlvHandler.hasBehaviourExcept(RLV_BHVR_TPLOC, idRlvObjExcept)) && (RlvActions::canStand(idRlvObjExcept));
}

bool RlvActions::isLocalTp(const LLVector3d& posGlobal)
{
    const F32 nDistSq = (LLVector2((F32)posGlobal.mdV[VX], (F32)posGlobal.mdV[VY]) - LLVector2((F32)gAgent.getPositionGlobal().mdV[VX], (F32)gAgent.getPositionGlobal().mdV[VY])).lengthSquared();
    return nDistSq < RLV_MODIFIER_TPLOCAL_DEFAULT * RLV_MODIFIER_TPLOCAL_DEFAULT;
}

// ============================================================================
// World interaction
//

bool RlvActions::canGroundSit()
{
    // RLV-SIT: (!hasBehaviour(RLV_BHVR_SIT)) && (canStand()) - the sit behaviours aren't registered until rlv-sit
    return true;
}

bool RlvActions::canGroundSit(const LLUUID& idRlvObjExcept)
{
    // RLV-SIT: (!gRlvHandler.hasBehaviourExcept(RLV_BHVR_SIT, idRlvObjExcept)) && (canStand(idRlvObjExcept))
    return true;
}

bool RlvActions::canSit(const LLViewerObject* pObj, const LLVector3& posOffset /*=LLVector3::zero*/)
{
    // RLV-SIT: not @sit restricted; not @unsit/@standtp restricted while sitting; @sit:<uuid>=force passes;
    //          otherwise within the @sittp (RLV_MODIFIER_SITTPDIST) radius. (The @fartouch part is stage 3.)
    //          Until then only the unrestricted part of FS's check remains: a volume object can be sat on.
    return (pObj) && (LL_PCODE_VOLUME == pObj->getPCode());
}

// Checked: 2014-02-24 (RLVa-1.4.10)
bool RlvActions::canShowLocation()
{
    return !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC);
}

bool RlvActions::canStand()
{
    // RLV-SIT: (!gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT)) || ((isAgentAvatarValid()) && (!gAgentAvatarp->isSitting()))
    return true;
}

bool RlvActions::canStand(const LLUUID& idRlvObjExcept)
{
    // RLV-SIT: (!gRlvHandler.hasBehaviourExcept(RLV_BHVR_UNSIT, idRlvObjExcept)) || ((isAgentAvatarValid()) && (!gAgentAvatarp->isSitting()))
    return true;
}

// ============================================================================
// Helper functions
//

template<>
const float& RlvActions::getModifierValue<float>(ERlvBehaviourModifier eBhvrMod)
{
    return RlvBehaviourDictionary::instance().getModifier(eBhvrMod)->getValue<float>();
}

// Checked: 2013-05-10 (RLVa-1.4.9)
bool RlvActions::hasBehaviour(ERlvBehaviour eBhvr)
{
    return gRlvHandler.hasBehaviour(eBhvr);
}

// Checked: 2013-05-09 (RLVa-1.4.9)
bool RlvActions::hasOpenP2PSession(const LLUUID& idAgent)
{
    const LLUUID idSession = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, idAgent);
    return (idSession.notNull()) && (LLIMMgr::instance().hasSession(idSession));
}

// Checked: 2013-05-09 (RLVa-1.4.9)
bool RlvActions::hasOpenGroupSession(const LLUUID& idGroup)
{
    return (idGroup.notNull()) && (LLIMMgr::instance().hasSession(idGroup));
}

// Checked: 2013-11-08 (RLVa-1.4.9)
bool RlvActions::isRlvEnabled()
{
    return RlvHandler::isEnabled();
}

#ifdef CATZNIP_STRINGVIEW
void RlvActions::notifyBlocked(const std::string_view& strNotifcation, const LLSD& sdArgs)
#else
void RlvActions::notifyBlocked(const std::string& strNotifcation, const LLSD& sdArgs)
#endif // CATZNIP_STRINGVIEW
{
    RlvUtil::notifyBlocked(strNotifcation, sdArgs);
}

// ============================================================================
