/**
 * @file rlvhooks_show_list.cpp
 * @brief rlv-show hooks: E6-E14 agentui, appviewer, avatar lists, chat history/toast, avatar picker, avatar textures (research/248). The code of every hook is FS' [RLVa:KB] block of the same function.
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
#include "llchat.h"
#include "llconversationmodel.h"
#include "llfloaterimcontainer.h"
#include "llsd.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvdefines.h"
#include "rlvhandler.h"

// LLChatMentionHelper's gate (llchatmentionhelper.cpp, S-H1): false keeps the mention picker closed. Null unless set here.
extern bool (*gPlusChatMentionGate)();

namespace plus::rlv
{

// Everything here is only called with RLVa on (the official files check enabled() first) and from the main thread.

namespace
{
    // showNamesGeneration(): counts the changes of @shownames and of its exceptions
    unsigned int s_nShowNamesGeneration = 0;
    bool s_fShowNamesConnected = false;     // (asked every frame by a visible list: a bool, not connection::connected())

    void onShowNamesSignal(ERlvBehaviour eBhvr)
    {
        if (RLV_BHVR_SHOWNAMES == eBhvr)
            ++s_nShowNamesGeneration;
    }
}

// ============================================================================
// E6 llagentui.cpp, E7 llappviewer.cpp: @showloc
//

bool hideAgentLocation(std::string& strParcel, std::string& strRegion)
{
    if (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        return false;

    strParcel = RlvStrings::getString(RlvStringKeys::Hidden::Parcel);
    strRegion = RlvStrings::getString(RlvStringKeys::Hidden::Region);
    return true;
}

void filterViewerInfo(LLSD& sdInfo)
{
    sdInfo["RLV_VERSION"] = RlvStrings::getVersionAbout();

    // FS keeps only the hidden region and the server version (and its release notes)
    if ( (sdInfo.has("REGION")) && (!RlvActions::canShowLocation()) )
    {
        sdInfo.erase("POSITION");
        sdInfo.erase("POSITION_LOCAL");
        sdInfo.erase("HOSTNAME");
        sdInfo.erase("SLURL");
        sdInfo["REGION"] = RlvStrings::getString(RlvStringKeys::Hidden::Region);
    }
}

// ============================================================================
// E8 llavatarlist, E9 llavatarlistitem, E13 llfloateravatarpicker, E14 llfloateravatartextures: @shownames
//

bool hasShowNames()
{
    return gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES);
}

std::string anonymName(const LLAvatarName& avName)
{
    return RlvStrings::getAnonym(avName);
}

unsigned int showNamesGeneration()
{
    // Connected the first time a list that hides names asks (LLAvatarList::setRlvCheckShowNames(), before its items are
    // made). The behaviour signal is fired for every @shownames command (FS' per-list callback), the exception signal for
    // @shownames:<uuid>. The slots hold no state, so they may stay connected for the handler's whole life.
    if (!s_fShowNamesConnected)
    {
        s_fShowNamesConnected = true;
        gRlvHandler.setBehaviourCallback([](ERlvBehaviour eBhvr, ERlvParamType) { onShowNamesSignal(eBhvr); });
        gRlvHandler.setExceptionCallback([](ERlvBehaviour eBhvr, const LLUUID&, ERlvParamType) { onShowNamesSignal(eBhvr); });
    }
    return s_nShowNamesGeneration;
}

// ============================================================================
// E10 llchathistory.cpp, E12 llfloaterimnearbychathandler.cpp
//

bool hasObjectIMURL(const LLChat& chat)
{
    return (CHAT_SOURCE_OBJECT == chat.mSourceType) && (std::string::npos != chat.mURL.find("objectim"));
}

void filterNearbyChatText(LLChat& chat, const LLSD& sdArgs)
{
    // NOTE-RLVa: we can only filter the *message* here since most everything else will already be part of "args" as well
    if ( (!RlvActions::canShowLocation()) && (!chat.mRlvLocFiltered) && (CHAT_SOURCE_AGENT != chat.mSourceType) )
    {
        RlvUtil::filterLocation(chat.mText);
        chat.mRlvLocFiltered = true;
    }
    if ( (!RlvActions::canShowName(RlvActions::SNC_DEFAULT)) && (!chat.mRlvNamesFiltered) && (CHAT_SOURCE_AGENT != chat.mSourceType) &&
         ( (!sdArgs.has("ONLINE_STATUS")) || (!sdArgs["ONLINE_STATUS"].asBoolean()) ) )
    {
        RlvUtil::filterNames(chat.mText);
        chat.mRlvNamesFiltered = true;
    }
    RlvUtil::filterMentions(chat.mText);
}

// ============================================================================
// S-H1 llconversationmodel.cpp, llfloaterimcontainer.cpp, llfloaterchatmentionpicker.cpp, llui/llchatmentionhelper.cpp
//

namespace
{
    // The nearby chat session's model in the Conversations floater (its session id is null), or null
    LLConversationItemSession* nearbySessionModel()
    {
        LLFloaterIMContainer* pContainer = LLFloaterIMContainer::findInstance();
        return (pContainer) ? dynamic_cast<LLConversationItemSession*>(pContainer->getSessionModel(LLUUID::null)) : nullptr;
    }

    // gPlusChatMentionGate while the mention helper works for the nearby chat session
    bool nearbyMentionGate()
    {
        return !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES);
    }
}

namespace
{
    // NearbyParticipantsScope: the participant command / menu item being handled is for the nearby chat session's list
    bool s_fNearbyParticipantsScope = false;
}

bool hidesNearbyParticipantNames(const LLUUID& idSession, const std::vector<LLUUID>& idAgents)
{
    return (idSession.isNull()) && (!canShowNames(idAgents));
}

NearbyParticipantsScope::NearbyParticipantsScope(bool fNearbySession)
    : mPrevious(s_fNearbyParticipantsScope)
{
    s_fNearbyParticipantsScope = fNearbySession;
}

NearbyParticipantsScope::~NearbyParticipantsScope()
{
    s_fNearbyParticipantsScope = mPrevious;
}

bool hidesNearbyParticipantNames(const std::vector<LLUUID>& idAgents)
{
    return (s_fNearbyParticipantsScope) && (!canShowNames(idAgents));
}

void refreshConversationNames()
{
    if (!enabled())
        return;

    LLConversationItemSession* pNearbySession = nearbySessionModel();
    if (!pNearbySession)
        return;
    // updateName() takes the name from the cache (it does not fetch) and anonymizes it when it is hidden (llconversationmodel.cpp).
    // It only flags the session for sorting and posts "update_participant", so the children are not changed while we walk them.
    for (auto itChild = pNearbySession->getChildrenBegin(); itChild != pNearbySession->getChildrenEnd(); ++itChild)
    {
        if (LLConversationItemParticipant* pParticipant = dynamic_cast<LLConversationItemParticipant*>(itChild->get()))
            pParticipant->updateName();
    }
}

void setMentionSession(const LLUUID& idSession)
{
    gPlusChatMentionGate = (idSession.isNull()) ? &nearbyMentionGate : nullptr;
}

} // namespace plus::rlv
