/**
 * @file rlvhooks_comm.cpp
 * @brief rlv-comm's part of the calls the official viewer files make into RLVa (plus/rlv), research/248
 *        (chat, IM, teleport, script permissions, offers). The code of every hook is FS' [RLVa:KB] block of the
 *        same function; only the place it is called from may differ (see ORIGIN.md).
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
#include "llcallingcard.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterreg.h"
#include "llimview.h"
#include "llinstantmessage.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llslurl.h"
#include "llspeakers.h"
#include "llstartup.h"
#include "llui.h"
#include "lluistring.h"
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"

#include "llscriptruntimeperms.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvdefines.h"
#include "rlvhandler.h"
#include "rlvhelper.h"
#include "rlvinventory.h"
#include "rlvui.h"

#include "plus.h"                               // plus::safeText()

namespace plus::rlv
{

// Everything here is only called with RLVa on (the official files check enabled() first) and from the main thread.

namespace
{
    // The anonymous name of an agent: from the name cache when it is there, else from strFallback (which keeps the same
    // agent on the same anonym)
    std::string rlvAnonymFor(const LLUUID& idAgent, const std::string& strFallback)
    {
        LLAvatarName avName;
        return (LLAvatarNameCache::get(idAgent, &avName)) ? RlvStrings::getAnonym(avName) : RlvStrings::getAnonym(strFallback);
    }

    // The refusal of an IM / call / conference start: the sound, the notice. Always true.
    bool rlvRefuseStart(const std::string_view& strBlockedKey, const LLSD& sdArgs = LLSD())
    {
        make_ui_sound("UISndInvalidOp");
        RlvUtil::notifyBlocked(strBlockedKey, sdArgs);
        return true;
    }
}

// ============================================================================
// B: sending chat
//

bool filterSentChat(std::string& utf8_out_text, EChatType& type, int channel)
{
    // Only process chat messages (ie not CHAT_TYPE_START, CHAT_TYPE_STOP, etc)
    if ( (CHAT_TYPE_WHISPER != type) && (CHAT_TYPE_NORMAL != type) && (CHAT_TYPE_SHOUT != type) )
        return false;

    if (0 == channel)
    {
        // Clamp the volume of the chat if needed
        type = RlvActions::checkChatVolume(type);

        // Redirect chat if needed
        if ( ( (gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT) || (gRlvHandler.hasBehaviour(RLV_BHVR_REDIREMOTE)) ) &&
             (gRlvHandler.redirectChatOrEmote(utf8_out_text)) ) )
        {
            return true;
        }

        // Filter public chat if sendchat restricted
        if (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT))
            gRlvHandler.filterChat(utf8_out_text, true);
    }
    else
    {
        // Don't allow chat on a non-public channel if sendchannel restricted (unless the channel is an exception)
        if (!RlvActions::canSendChannel(channel))
            return true;

        // Don't allow chat on debug channel if @sendchat, @redirchat or @rediremote restricted (shows as public chat on viewers)
        if (CHAT_CHANNEL_DEBUG == channel)
        {
            const bool fIsEmote = RlvUtil::isEmote(utf8_out_text);
            if ( (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT)) ||
                 ((!fIsEmote) && (gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT))) ||
                 ((fIsEmote) && (gRlvHandler.hasBehaviour(RLV_BHVR_REDIREMOTE))) )
            {
                return true;
            }
        }
    }
    return false;
}

EChatType checkChatVolume(EChatType type)
{
    return RlvActions::checkChatVolume(type);
}

bool allowsChatAnimation(const std::string& utf8_text)
{
    return !RlvActions::hasBehaviour( (!RlvUtil::isEmote(utf8_text)) ? RLV_BHVR_REDIRCHAT : RLV_BHVR_REDIREMOTE );
}

bool canSendTypingStart()
{
    return RlvActions::canSendTypingStart();
}

// *TODO-RLVa (FS): it's too late into the release cycle to block all script interactions so just take care of the nearby chat loophole for now
bool blocksScriptDialogReply(int channel)
{
    return (0 == channel) && (RlvActions::hasBehaviour(RLV_BHVR_SENDCHAT));
}

bool blocksScriptDialog(int channel)
{
    if (!blocksScriptDialogReply(channel))
        return false;
    RlvUtil::notifyBlocked("blocked_scriptdialog");
    return true;
}

// ============================================================================
// C: IM
//

bool handleIMQuery(const LLUUID& idSender, const std::string& strSenderName, const std::string& strMessage, bool fOnline)
{
    // The sender is checked in there (online, not muted, friends only, a word of the list; FS processNewMessage())
    return gRlvHandler.processIMQuery(idSender, strSenderName, strMessage, fOnline);
}

bool canReceiveIM(const LLUUID& idSender)
{
    return RlvActions::canReceiveIM(idSender);
}

std::string receivedIMBlockedText()
{
    return RlvStrings::getString(RlvStringKeys::Blocked::RecvIm);
}

bool filterReceivedIM(const LLUUID& idSender, bool fOffline, bool fLinden, bool fMuted, const LLUUID& idSession, std::string& strMessage)
{
    // Don't block offline IMs, or IMs from Lindens
    if ( (!fOffline) && (!RlvActions::canReceiveIM(idSender)) && (!fLinden) )
    {
        if (!fMuted)
            RlvUtil::sendBusyMessage(idSender, RlvStrings::getString(RlvStringKeys::Blocked::RecvImRemote), idSession);
        strMessage = RlvStrings::getString(RlvStringKeys::Blocked::RecvIm);
        return true;
    }
    return false;
}

void filterOutgoingIMText(const LLUUID& idRecipient, std::string& strText)
{
    if (!RlvActions::canSendIM(idRecipient))
        strText = RlvStrings::getString(RlvStringKeys::Blocked::SendIm);
}

void filterSentIM(int nDialog, const LLUUID& idSession, const LLUUID& idOther, std::string& strText)
{
    if ( (!RlvActions::hasBehaviour(RLV_BHVR_SENDIM)) && (!RlvActions::hasBehaviour(RLV_BHVR_SENDIMTO)) )
        return;

    // A one-on-one IM (IM_NOTHING_SPECIAL) goes to idOther whatever the session is
    if (IM_NOTHING_SPECIAL == nDialog)
    {
        if (!RlvActions::canSendIM(idOther))
            strText = RlvStrings::getString(RlvStringKeys::Blocked::SendIm);
        return;
    }

    // FS does this in FSFloaterIM::sendMsg(); Charis has one place every IM goes through
    const LLIMModel::LLIMSession* pIMSession = LLIMModel::instance().findIMSession(idSession);

    bool fRlvFilter = !pIMSession;
    if (pIMSession)
    {
        switch (pIMSession->mSessionType)
        {
            case LLIMModel::LLIMSession::P2P_SESSION:   // One-on-one IM
                fRlvFilter = !RlvActions::canSendIM(idOther);
                break;
            case LLIMModel::LLIMSession::GROUP_SESSION: // Group chat
                fRlvFilter = !RlvActions::canSendIM(idSession);
                break;
            case LLIMModel::LLIMSession::ADHOC_SESSION: // Conference chat: allow if all participants can be sent an IM
            {
                if (!pIMSession->mSpeakers)
                {
                    fRlvFilter = true;
                    break;
                }

                LLSpeakerMgr::speaker_list_t speakers;
                pIMSession->mSpeakers->getSpeakerList(&speakers, true);
                for (const auto& pSpeaker : speakers)
                {
                    if ( (gAgent.getID() != pSpeaker->mID) && (!RlvActions::canSendIM(pSpeaker->mID)) )
                    {
                        fRlvFilter = true;
                        break;
                    }
                }
                break;
            }
            default:
                fRlvFilter = true;
                break;
        }
    }

    if (fRlvFilter)
        strText = RlvStrings::getString(RlvStringKeys::Blocked::SendIm);
}

bool filterInvitationText(const LLUUID& idSession, const LLUUID& idFrom, std::string& strMessage)
{
    if ( (RlvActions::hasBehaviour(RLV_BHVR_RECVIM)) || (RlvActions::hasBehaviour(RLV_BHVR_RECVIMFROM)) )
    {
        if (gAgent.isInGroup(idSession))                    // Group chat: don't accept the invite if not an exception
        {
            if (!RlvActions::canReceiveIM(idSession))
                return false;
        }
        else if (!RlvActions::canReceiveIM(idFrom))         // Conference chat: don't block; censor if not an exception
        {
            strMessage = RlvStrings::getString(RlvStringKeys::Blocked::RecvIm);
        }
    }
    return true;
}

void filterHistoryText(const LLUUID& idFrom, const LLUUID& idSession, LLSD& sdText)
{
    if ( (idFrom.isNull()) || (gAgent.getID() == idFrom) )
        return;
    const LLUUID& idJudged = (gAgent.isInGroup(idSession)) ? idSession : idFrom;
    if (!RlvActions::canReceiveIM(idJudged))
        sdText = RlvStrings::getString(RlvStringKeys::Blocked::RecvIm);
}

bool refuseStartIM(const LLUUID& idRecipient)
{
    return (!RlvActions::canStartIM(idRecipient))
        && rlvRefuseStart(RlvStringKeys::Blocked::StartIm, LLSD().with("RECIPIENT", LLSLURL("agent", idRecipient, "completename").getSLURLString()));
}

bool refuseStartConference(const std::vector<LLUUID>& idRecipients)
{
    for (const LLUUID& idAgent : idRecipients)
    {
        if (!RlvActions::canStartIM(idAgent))
            return rlvRefuseStart(RlvStringKeys::Blocked::StartConference);
    }
    return false;
}

bool refuseStartGroupIM(const LLUUID& idGroup)
{
    return (!RlvActions::canStartIM(idGroup))
        && rlvRefuseStart(RlvStringKeys::Blocked::StartIm, LLSD().with("RECIPIENT", LLSLURL("group", idGroup, "about").getSLURLString()));
}

bool canStartIM(const LLUUID& idRecipient)
{
    return RlvActions::canStartIM(idRecipient);
}

void filterTeleportRequestText(const LLUUID& idRecipient, std::string& strText)
{
    // Filter the request message if the recipients is IM-blocked
    if ( (!RlvActions::canStartIM(idRecipient)) || (!RlvActions::canSendIM(idRecipient)) )
        strText = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
}

bool canSendIM(const LLUUID& idRecipient)
{
    return RlvActions::canSendIM(idRecipient);
}

bool canChangeDoNotDisturbResponse()
{
    return !RlvActions::hasBehaviour(RLV_BHVR_SENDIM);
}

// ============================================================================
// D: teleport and script permissions
//

bool refuseTeleportToLandmark(bool fHome)
{
    // FS (FIRE-21576): prevent teleporting home while still logging in
    if (LLStartUp::getStartupState() < STATE_STARTED)
        return true;

    // NOTE: we'll allow teleporting home unless both @tplm=n *and* @tploc=n restricted
    if ( ( (!fHome) ? gRlvHandler.hasBehaviour(RLV_BHVR_TPLM)
                    : gRlvHandler.hasBehaviour(RLV_BHVR_TPLM) && gRlvHandler.hasBehaviour(RLV_BHVR_TPLOC) ) ||
         ((gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT)) && (isAgentAvatarValid()) && (gAgentAvatarp->isSitting())) )
    {
        RlvUtil::notifyBlocked(RlvStringKeys::Blocked::Teleport);
        return true;
    }
    return false;
}

bool refuseTeleportToLocation(const LLVector3d& posGlobal)
{
    // A teleport RLVa itself makes (@tpto=force) ignores the restrictions
    if (RlvUtil::isForceTp())
        return false;

    if (LLStartUp::getStartupState() < STATE_STARTED)
        return true;

    if ( (RlvActions::isLocalTp(posGlobal)) ? !RlvActions::canTeleportToLocal(posGlobal) : !RlvActions::canTeleportToLocation() )
    {
        RlvUtil::notifyBlocked(RlvStringKeys::Blocked::Teleport);
        return true;
    }
    return false;
}

void noCancelForStartedTpto()
{
    // Charis: FS sets the flag in the check above, before the teleport begins; a @tpto whose teleport did not begin (no
    // region, a request that went nowhere) left every later teleport uncancellable until one ended (research/275 L086)
    if ( (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) &&
         (gRlvHandler.getCurrentCommand()) && (RLV_BHVR_TPTO == gRlvHandler.getCurrentCommand()->getBehaviourType()) )
        gRlvHandler.setCanCancelTp(false);
}

bool isRlvMadeTeleport()
{
    // (@tpto=force to a region name asks the map first and teleports from a callback: only the object is on the stack then)
    return (RlvUtil::isForceTp()) || (gRlvHandler.getCurrentObject().notNull()) ||
           ( (gRlvHandler.getCurrentCommand()) && (RLV_BHVR_TPTO == gRlvHandler.getCurrentCommand()->getBehaviourType()) );
}

bool refuseTeleportRequestAccept(const LLUUID& idFrom)
{
    if (RlvActions::canAcceptTpRequest(idFrom))
        return false;
    RlvUtil::notifyBlocked(RlvStringKeys::Blocked::Teleport);
    return true;
}

bool refuseLureAccept(const LLUUID& idFrom)
{
    if (RlvActions::canAcceptTpOffer(idFrom))
        return false;
    RlvUtil::notifyBlocked(RlvStringKeys::Blocked::Teleport);
    return true;
}

bool canCancelTeleport()
{
    return gRlvHandler.getCanCancelTp();
}

bool autoAcceptTeleport(int nDialog, const LLUUID& idFrom)
{
    // If we auto-accept the offer/request then this will override DnD status (but we'll still let the other party know later)
    return ( ((IM_LURE_USER == nDialog) && (RlvActions::autoAcceptTeleportOffer(idFrom))) ||
             ((IM_TELEPORT_REQUEST == nDialog) && (RlvActions::autoAcceptTeleportRequest(idFrom))) );
}

bool blocksTeleportOffer(int nDialog, const LLUUID& idFrom)
{
    if ( ((IM_LURE_USER == nDialog) && (!RlvActions::canAcceptTpOffer(idFrom))) ||
         ((IM_TELEPORT_REQUEST == nDialog) && (!RlvActions::canAcceptTpRequest(idFrom))) )
    {
        RlvUtil::sendBusyMessage(idFrom, RlvStrings::getString(RlvStringKeys::Blocked::TpLureRequestRemote));
        return true;
    }
    return false;
}

void censorTeleportOfferText(int nDialog, const LLUUID& idFrom, std::string& strMessage)
{
    // Censor message if: 1) restricted from receiving IMs from the sender, or 2) teleport offer/request and @showloc=n restricted
    if ( (!RlvActions::canReceiveIM(idFrom)) ||
         ((gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC)) && (IM_LURE_USER == nDialog || IM_TELEPORT_REQUEST == nDialog)) )
    {
        strMessage = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
    }
}

void noCancelForTeleport()
{
    gRlvHandler.setCanCancelTp(false);
}

std::string anonymizedName(const LLUUID& idAgent)
{
    // Charis: the text itself (FS builds an "rlvanonym" link that the URL registry turns into the same text)
    return rlvAnonymFor(idAgent, idAgent.asString());
}

bool restrictsTeleportOffers()
{
    return gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC);
}

bool mayOfferTeleportRestricted(const LLUUID& idAgent)
{
    // Only allow offering teleports if everyone is a @tplure exception or able to map this avie under @showloc=n
    const LLRelationship* pBuddyInfo = LLAvatarTracker::instance().getBuddyInfo(idAgent);
    return (gRlvHandler.isException(RLV_BHVR_TPLURE, idAgent, ERlvExceptionCheck::Permissive)) ||
           ((pBuddyInfo) && (pBuddyInfo->isOnline()) && (pBuddyInfo->isRightGrantedTo(LLRelationship::GRANT_MAP_LOCATION)));
}

bool prepareTeleportOffer(const std::vector<LLUUID>& idRecipients, LLSD& sdEditArgs, LLSD& sdPayload)
{
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        sdEditArgs["REGION"] = RlvStrings::getString(RlvStringKeys::Hidden::Generic);

    bool fShouldHideNames = false;
    for (const LLUUID& idAgent : idRecipients)
    {
        // Only allow offering teleports if everyone is a @tplure exception or able to map this avie under @showloc=n
        if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        {
            const LLRelationship* pBuddyInfo = LLAvatarTracker::instance().getBuddyInfo(idAgent);
            if ( (!gRlvHandler.isException(RLV_BHVR_TPLURE, idAgent, ERlvExceptionCheck::Permissive)) &&
                 ((!pBuddyInfo) || (!pBuddyInfo->isOnline()) || (!pBuddyInfo->isRightGrantedTo(LLRelationship::GRANT_MAP_LOCATION))) )
            {
                RlvUtil::notifyBlocked(RlvStringKeys::Blocked::TeleportOffer);
                return false;
            }
        }
        fShouldHideNames |= !RlvActions::canShowName(RlvActions::SNC_TELEPORTOFFER, idAgent);
    }
    sdPayload["rlv_shownames"] = fShouldHideNames;
    return true;
}

void filterLureText(const LLSD& sdRecipients, std::string& strText)
{
    // Filter the lure message if any of the recipients are IM-blocked
    if (!sdRecipients.isArray())
        return;
    for (LLSD::array_const_iterator itId = sdRecipients.beginArray(), endId = sdRecipients.endArray(); itId != endId; ++itId)
    {
        const LLUUID idAgent = itId->asUUID();
        if ( (!RlvActions::canStartIM(idAgent)) || (!RlvActions::canSendIM(idAgent)) )
        {
            strText = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
            return;
        }
    }
}

bool canShowWorldMap()
{
    // FIRE-35747: llMapDestination() respects @showworldmap=n
    return !RlvActions::hasBehaviour(RLV_BHVR_SHOWWORLDMAP);
}

void filterScriptQuestions(const LLUUID& idTask, S32& nQuestions, LLSD& sdPayload)
{
    // Attach / teleport permissions are withheld (sets sdPayload["questions"], which is what is granted when the dialog is answered)
    RlvUtil::filterScriptQuestions(nQuestions, sdPayload);

    if ( (nQuestions) && (gRlvHandler.hasBehaviour(RLV_BHVR_ACCEPTPERMISSION)) )
    {
        // (the notice of what was granted is shown below, within the budget)
        const LLViewerObject* pObj = gObjectList.findObject(idTask);
        if (pObj)
        {
            if ( (pObj->permYouOwner()) && (!pObj->isAttachment()) )
            {
                nQuestions &= ~(SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_TAKE_CONTROLS].permbit |
                                SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_ATTACH].permbit);
            }
            else
            {
                nQuestions &= ~(SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_TAKE_CONTROLS].permbit);
            }
            sdPayload["rlv_notify"] = !pObj->permYouOwner();
        }
    }

    // What is shown about an answer RLVa gave by itself (a refused attach / teleport permission, a granted one) is limited: any object
    // can ask, and a request counts only when something is shown
    if ( (sdPayload.has("rlv_blocked")) || (sdPayload["rlv_notify"].asBoolean()) )
    {
        // per object: one that asks over and over can't use up what other objects may show
        static std::map<LLUUID, std::pair<F64, S32>> s_Shown;   // object -> (start of its minute, notices shown in it)
        const F64 tmNow = LLTimer::getTotalSeconds();
        if (s_Shown.size() >= 256)
        {
            for (auto itShown = s_Shown.begin(); itShown != s_Shown.end(); )
                itShown = (tmNow - itShown->second.first >= 60.0) ? s_Shown.erase(itShown) : std::next(itShown);
        }
        std::pair<F64, S32>& shown = s_Shown[idTask];
        if (tmNow - shown.first >= 60.0)
        {
            shown.first = tmNow;
            shown.second = 0;
        }
        if (shown.second >= 10)
        {
            sdPayload.erase("rlv_blocked");
            sdPayload.erase("rlv_notify");
        }
        else
        {
            shown.second++;
        }
    }
}

bool hideNoticeLocation(LLUIString& notice)
{
    if (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        return false;
    notice.setArg("[REGIONNAME]", RlvStrings::getString(RlvStringKeys::Hidden::Region));
    notice.setArg("[REGIONPOS]", RlvStrings::getString(RlvStringKeys::Hidden::Generic));
    return true;
}

void tellGrantedPermission(const std::string& strNotice)
{
    LLFloaterIMNearbyChat* pNearbyChat = LLFloaterReg::getTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (pNearbyChat)
    {
        LLChat chat_msg(plus::safeText(strNotice));   // object and owner names are other residents' text
        chat_msg.mFromName = SYSTEM_FROM;
        chat_msg.mFromID = LLUUID::null;
        chat_msg.mSourceType = CHAT_SOURCE_SYSTEM;
        pNearbyChat->addMessage(chat_msg);
    }
}

void notifyBlockedPermission(const std::string& strKey, const std::string& strObjectName)
{
    RlvUtil::notifyBlocked(strKey, LLSD().with("OBJECT", strObjectName));
}

// ============================================================================
// E: names and places (E46-E48)
//

bool canAddToRecentPeople(const LLUUID& idAgent)
{
    // Keep in sync with the condition in processNewMessage() IM_INVENTORY_OFFERED (FS: RELEASE-RLVa [RLVa-2.0.1])
    return (RlvActions::canShowName(RlvActions::SNC_DEFAULT, idAgent)) || (!RlvUtil::isNearbyAgent(idAgent)) ||
           (RlvUIEnabler::hasOpenIM(idAgent)) || (RlvUIEnabler::hasOpenProfile(idAgent));
}

std::string offerOwnerName(const LLUUID& idOwner, const LLAvatarName& avName)
{
    return (canAddToRecentPeople(idOwner)) ? avName.getUserName() : RlvStrings::getAnonym(avName);
}

void filterSystemText(std::string& strText)
{
    if (!RlvActions::canShowLocation())
        RlvUtil::filterLocation(strText);
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT))
        RlvUtil::filterNames(strText);
}

std::string meanCollisionName(const LLAvatarName& avName)
{
    return (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES)) ? RlvStrings::getAnonym(avName) : avName.getUserName();
}

void anonymizeObjectOfferOwner(const LLOfferInfo& offer, LLSD& sdArgs, LLSD& sdPayload)
{
    // Only filter if the object owner is a nearby agent (a null id is nobody: RlvUtil::isNearbyAgent() asserts on it)
    if ( (offer.mFromID.notNull()) && (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, offer.mFromID)) && (RlvUtil::isNearbyAgent(offer.mFromID)) )
    {
        sdPayload["rlv_shownames"] = true;
        sdArgs["NAME_SLURL"] = anonymizedName(offer.mFromID);
    }
}

void anonymizeAgentOfferSender(const LLOfferInfo& offer, LLSD& sdArgs, LLSD& sdPayload)
{
    // Only filter if the offer is from a nearby agent and if there's no open IM session (doesn't necessarily have to be focused)
    if (!canAddToRecentPeople(offer.mFromID))
    {
        sdPayload["rlv_shownames"] = true;
        sdArgs["NAME"] = rlvAnonymFor(offer.mFromID, offer.mFromName);
        sdArgs["NAME_SLURL"] = sdArgs["NAME"];
    }
}

void anonymizeNearbyName(const LLUUID& idAgent, const std::string& strOriginalName, LLSD& sdArgs)
{
    // Only anonymize the name if the agent is nearby, there isn't an open IM session to them and their profile isn't open
    if (!canAddToRecentPeople(idAgent))
    {
        const std::string strAnonym = rlvAnonymFor(idAgent, strOriginalName);
        sdArgs["NAME"] = strAnonym;
        sdArgs["ORIGINAL_NAME"] = strAnonym;
    }
}

void filterObjectIM(bool fFromGroup, const LLUUID& idOwner, std::string& strName, std::string& strLocation, LLSD& sdQuery, LLChat& chat)
{
    // NOTE: the chat message itself will be filtered in LLNearbyChatHandler::processChat()
    if ( (!RlvActions::canShowName(RlvActions::SNC_DEFAULT)) && (!fFromGroup) && (idOwner.notNull()) && (RlvUtil::isNearbyAgent(idOwner)) )
    {
        sdQuery["rlv_shownames"] = true;

        RlvUtil::filterNames(strName);
        chat.mFromName = strName;
    }
    if (!RlvActions::canShowLocation())
    {
        const std::string::size_type idxPos = strLocation.find('/');
        if ( (std::string::npos != idxPos) && (RlvUtil::isNearbyRegion(strLocation.substr(0, idxPos))) )
            strLocation = RlvStrings::getString(RlvStringKeys::Hidden::Region);
    }
}

void anonymizeTeleportRequestName(const LLUUID& idAgent, const LLAvatarName& avName, LLSD& sdNotification)
{
    if (!RlvActions::canShowName(RlvActions::SNC_TELEPORTREQUEST, idAgent))
        sdNotification["NAME"] = RlvStrings::getAnonym(avName);
}

bool anonymizeUnknownTeleportRequestName(const LLUUID& idAgent, LLSD& sdNotification)
{
    // Charis: FS waits for the name and shows it then, whatever the name context was (research/275 L105). Without the
    // name, the generic hidden text: an anonym made from the id would differ from the one the name gives everywhere else,
    // and the same resident would show under two anonyms
    if (RlvActions::canShowName(RlvActions::SNC_TELEPORTREQUEST, idAgent))
        return false;
    sdNotification["NAME"] = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
    return true;
}

// ============================================================================
// W23, W24: "#RLV/~" folders
//

bool isGiveToRLVOffer(const LLOfferInfo& offer)
{
    return RlvInventory::instance().isGiveToRLVOffer(offer);
}

void onAgentFolderOffer(LLOfferInfo& offer)
{
    if ( (!RlvSettings::getForbidGiveToRLV()) && (LLAssetType::AT_CATEGORY == offer.mType) && (offer.mDesc.find(RLV_PUTINV_PREFIX) == 0) )
    {
        RlvGiveToRLVAgentOffer* pOfferObserver = new RlvGiveToRLVAgentOffer(offer.mObjectID);
        pOfferObserver->startFetch();
        if (pOfferObserver->isFinished())
            pOfferObserver->done();
        else
            gInventory.addObserver(pOfferObserver);
    }
}

void onTaskFolderOfferAccepted(LLOfferInfo& offer)
{
    // Only treat the offer as 'Give to #RLV' if:
    //   - the user has enabled the feature
    //   - the inventory offer came from a script (and specifies a folder)
    //   - the name starts with the prefix - mDesc format: '[OBJECTNAME]'  ( http://slurl.com/... )
    if ( (IM_TASK_INVENTORY_OFFERED != offer.mIM) || (LLAssetType::AT_CATEGORY != offer.mType) || (offer.mDesc.find(RLV_PUTINV_PREFIX) != 1) )
        return;

    if (!RlvSettings::getForbidGiveToRLV())
    {
        const LLUUID& idRlvRoot = RlvInventory::instance().getSharedRootID();
        if (idRlvRoot.notNull())
            offer.mFolderID = idRlvRoot;

        // "accepted_in_rlv" is sent from RlvGiveToRLVTaskOffer *after* we have the folder
        RlvGiveToRLVTaskOffer* pOfferObserver = new RlvGiveToRLVTaskOffer(offer.mTransactionID, RlvGiveToRLVTaskOffer::getOfferedFolderName(offer.mDesc));
        gInventory.addObserver(pOfferObserver);
    }
    else
    {
        const std::string::size_type idxToken = offer.mDesc.find("'  ( http://");
        if (std::string::npos != idxToken)
            RlvBehaviourNotifyHandler::sendNotification("accepted_in_inv inv_offer " + offer.mDesc.substr(1, idxToken - 1));
    }
}

void onTaskFolderOfferDeclined(const LLOfferInfo& offer)
{
    if ( (IM_TASK_INVENTORY_OFFERED == offer.mIM) && (LLAssetType::AT_CATEGORY == offer.mType) && (offer.mDesc.find(RLV_PUTINV_PREFIX) == 1) )
    {
        const std::string::size_type idxToken = offer.mDesc.find("'  ( http://");
        if (std::string::npos != idxToken)
            RlvBehaviourNotifyHandler::sendNotification("declined inv_offer " + offer.mDesc.substr(1, idxToken - 1));
    }
}

} // namespace plus::rlv
