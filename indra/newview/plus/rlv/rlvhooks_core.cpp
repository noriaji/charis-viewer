/**
 * @file rlvhooks_core.cpp
 * @brief rlv-core's part of the calls the official viewer files make into RLVa (plus/rlv), research/248.
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

#include "plus/plus.h"

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llchat.h"
#include "llslurl.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llviewerregion.h"
#include "llworld.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvdefines.h"
#include "rlvhandler.h"

namespace plus::rlv
{

bool enabled()
{
    return RlvHandler::isEnabled();
}

bool canChangeActiveGroup()
{
    return !RlvHandler::isEnabled() || !gRlvHandler.hasBehaviour(RLV_BHVR_SETGROUP);
}

void setCommandBatchDone(command_batch_done_fn fn)
{
    // Kept by RlvHandler: the lines that wait for their object (RlvHandler::deferCommandLine()) run it too
    RlvHandler::setCommandBatchDone(fn);
}

// An llOwnerSay line is an RLV command when it starts with '@' and holds more than the '@' and one
// character (FS: "mesg.length() > 3" counted the '@' and a three-character command at least).
static bool isCommandLine(const std::string& mesg)
{
    return (mesg.length() > 3) && (RLV_CMD_PREFIX == mesg[0]);
}

// The speech particles of an object (the same as process_chat_from_simulator() makes).
static void makeChatParticles(LLViewerObject* chatter, const LLUUID& owner_id)
{
    if ( (!chatter) || (!gSavedSettings.getBOOL("EffectScriptChatParticles")) )
        return;

    LLPointer<LLViewerPartSourceChat> psc = new LLViewerPartSourceChat(chatter->getPositionAgent());
    psc->setSourceObject(chatter);
    psc->setColor(LLColor4(1.f, 1.f, 1.f, 1.f));
    // The particles are owned by the object's owner, just in case they should be muted by the mute list
    psc->setOwnerUUID(owner_id);
    LLViewerPartSim::getInstance()->addPartSource(psc);
}

// An llOwnerSay line is a command of ours only when the object is the agent's own (the simulator says so
// in the owner id; FS trusts the chat type alone).
static bool isOwnObjectSay(const LLChat& chat, const LLUUID& owner_id)
{
    return (CHAT_TYPE_OWNER == chat.mChatType) && (CHAT_SOURCE_OBJECT == chat.mSourceType) && (owner_id == gAgentID);
}

bool holdsChatParticles(int chat_type)
{
    return (RlvHandler::isEnabled()) && (CHAT_TYPE_OWNER == chat_type);
}

bool filterChat(LLChat& chat, std::string& mesg, const LLUUID& from_id, const LLUUID& owner_id, LLViewerObject* chatter)
{
    // The speech particles of the llOwnerSay lines that are not commands (see holdsChatParticles())
    if ( (CHAT_SOURCE_OBJECT == chat.mSourceType) && (CHAT_TYPE_OWNER == chat.mChatType) &&
         ( (!isOwnObjectSay(chat, owner_id)) || (!isCommandLine(mesg)) ) )
    {
        makeChatParticles(chatter, owner_id);
    }

    if ( (CHAT_TYPE_START == chat.mChatType) || (CHAT_TYPE_STOP == chat.mChatType) )
        return false;

    // NOTE: chatter can be NULL (may not have rezzed yet, or could be another avie's HUD attachment)
    bool is_attachment = (chatter) ? chatter->isAttachment() : false;
    bool is_owned_by_me = (chatter) ? chatter->permYouOwner() : false;

    // Filtering "rules":
    //   avatar  => filter all avie text (unless it's this avie or they're an exemption)
    //   objects => filter everything except attachments this avie owns (never filter llOwnerSay or llRegionSayTo chat)
    if ( ( (CHAT_SOURCE_AGENT == chat.mSourceType) && (from_id != gAgentID) ) ||
         ( (CHAT_SOURCE_OBJECT == chat.mSourceType) && ((!is_owned_by_me) || (!is_attachment)) &&
           (CHAT_TYPE_OWNER != chat.mChatType) && (CHAT_TYPE_DIRECT != chat.mChatType) ) )
    {
        bool fIsEmote = RlvUtil::isEmote(mesg);
        if ( (!fIsEmote) &&
             ( ((gRlvHandler.hasBehaviour(RLV_BHVR_RECVCHAT)) && (!gRlvHandler.isException(RLV_BHVR_RECVCHAT, from_id))) ||
               ((gRlvHandler.hasBehaviour(RLV_BHVR_RECVCHATFROM)) && (gRlvHandler.isException(RLV_BHVR_RECVCHATFROM, from_id))) ) )
        {
            if ( (gRlvHandler.filterChat(mesg, false)) && (!RlvSettings::getShowEllipsis()) )
                return true;
        }
        else if ( (fIsEmote) &&
                  ( ((gRlvHandler.hasBehaviour(RLV_BHVR_RECVEMOTE)) && (!gRlvHandler.isException(RLV_BHVR_RECVEMOTE, from_id))) ||
                    ((gRlvHandler.hasBehaviour(RLV_BHVR_RECVEMOTEFROM)) && (gRlvHandler.isException(RLV_BHVR_RECVEMOTEFROM, from_id))) ) )
        {
            if (!RlvSettings::getShowEllipsis())
                return true;
            mesg = "/me ...";
        }
    }

    // Filtering "rules":
    //   avatar => filter only their name (unless it's this avie)
    //   other  => filter everything
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT))
    {
        if (CHAT_SOURCE_AGENT != chat.mSourceType)
        {
            RlvUtil::filterNames(chat.mFromName);
        }
        else if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, chat.mFromID))
        {
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(chat.mFromID, &av_name))
                chat.mFromName = RlvStrings::getAnonym(av_name);
            else
                chat.mFromName = RlvStrings::getAnonym(chat.mFromName);
            chat.mRlvNamesFiltered = true;
        }
    }

    // Create an "objectim" URL for objects if we're either @shownames or @showloc restricted
    // (we need to do this now because we won't be have enough information to do it later on)
    if ( (CHAT_SOURCE_OBJECT == chat.mSourceType) &&
         ( (!RlvActions::canShowName(RlvActions::SNC_DEFAULT)) || (!RlvActions::canShowLocation()) ) )
    {
        LLSD sdQuery;
        sdQuery["name"] = chat.mFromName;
        sdQuery["owner"] = owner_id;

        if ( (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, owner_id)) && (!is_owned_by_me) )
            sdQuery["rlv_shownames"] = true;

        const LLViewerRegion* pRegion = LLWorld::getInstance()->getRegionFromPosAgent(chat.mPosAgent);
        if (pRegion)
        {
            // @showloc: a nearby region is told as the hidden region, without the position (FS writes the real place here)
            if ( (!RlvActions::canShowLocation()) && (RlvUtil::isNearbyRegion(pRegion->getName())) )
                sdQuery["slurl"] = RlvStrings::getString(RlvStringKeys::Hidden::Region);
            else
                sdQuery["slurl"] = LLSLURL(pRegion->getName(), chat.mPosAgent).getLocationString();
        }

        chat.mURL = LLSLURL("objectim", from_id, LLURI::mapToQueryString(sdQuery)).getSLURLString();
    }
    return false;
}

bool handleOwnerSay(LLChat& chat, std::string& mesg, const LLUUID& from_id, const LLUUID& owner_id, LLViewerObject* chatter)
{
    // Commands come from the agent's own objects only
    if ( (!isCommandLine(mesg)) || (!isOwnObjectSay(chat, owner_id)) )
        return false;

    // Temporary attachments (Experiences) only when allowed and not refused. Whether the object is one can only be told once
    // it is known as ours, so until then the line waits in RlvHandler (=y and @clear in it run at once)
    const RlvHandler::ERlvCmdOrigin eOrigin = gRlvHandler.checkCommandOrigin(from_id, chat.mFromName, chatter);
    if (RlvHandler::ERlvCmdOrigin::Allowed != eOrigin)
    {
        std::string strCommands = mesg.substr(1);
        LLStringUtil::toLower(strCommands);
        switch (eOrigin)
        {
            case RlvHandler::ERlvCmdOrigin::Deferred:
                gRlvHandler.deferCommandLine(from_id, chat.mFromName, strCommands);
                return true;
            case RlvHandler::ERlvCmdOrigin::NotCommand:
                // Temporary attachments are off: the line shows as chat (FS), but =y / =rem and @clear in it still run
                gRlvHandler.runTakeBackCommands(from_id, strCommands);
                return false;
            case RlvHandler::ERlvCmdOrigin::Blocked:
            default:
                // A refused Experience's object: only taking restrictions back runs
                gRlvHandler.runTakeBackCommands(from_id, strCommands);
                return true;
        }
    }

    mesg.erase(0, 1);
    LLStringUtil::toLower(mesg);

    std::string strExecuted, strFailed, strRetained, *pstr;

    // Comma separated, empty tokens dropped
    size_t idxStart = 0;
    while (idxStart <= mesg.length())
    {
        size_t idxEnd = mesg.find(',', idxStart);
        if (std::string::npos == idxEnd)
            idxEnd = mesg.length();
        std::string strCmd = mesg.substr(idxStart, idxEnd - idxStart);
        idxStart = idxEnd + 1;
        if (strCmd.empty())
            continue;

        ERlvCmdRet eRet = gRlvHandler.processCommand(from_id, strCmd, true);
        if ( (RlvSettings::getDebug()) &&
             ( (!RlvSettings::getDebugHideUnsetDup()) ||
               ((RLV_RET_SUCCESS_UNSET != eRet) && (RLV_RET_SUCCESS_DUPLICATE != eRet)) ) )
        {
            if ( RLV_RET_SUCCESS == (eRet & RLV_RET_SUCCESS) )
                pstr = &strExecuted;
            else if ( RLV_RET_FAILED == (eRet & RLV_RET_FAILED) )
                pstr = &strFailed;
            else if (RLV_RET_RETAINED == eRet)
                pstr = &strRetained;
            else
            {
                RLV_ASSERT(false);
                pstr = &strFailed;
            }

            const char* pstrSuffix = RlvStrings::getStringFromReturnCode(eRet);
            if (pstrSuffix)
                strCmd.append(" (").append(pstrSuffix).append(")");

            if (!pstr->empty())
                pstr->push_back(',');
            pstr->append(strCmd);
        }
    }

    RlvHandler::onCommandBatchDone();

    if ( (!RlvSettings::getDebug()) || ((strExecuted.empty()) && (strFailed.empty()) && (strRetained.empty())) )
        return true;

    // Silly people want comprehensive debug messages, blah :p
    if ( (!strExecuted.empty()) && (strFailed.empty()) && (strRetained.empty()) )
    {
        chat.mText = " executes: @";
        mesg = strExecuted;
    }
    else if ( (strExecuted.empty()) && (!strFailed.empty()) && (strRetained.empty()) )
    {
        chat.mText = " failed: @";
        mesg = strFailed;
    }
    else if ( (strExecuted.empty()) && (strFailed.empty()) && (!strRetained.empty()) )
    {
        chat.mText = " retained: @";
        mesg = strRetained;
    }
    else
    {
        chat.mText = ": @";
        if (!strExecuted.empty())
            mesg += "\n    - executed: @" + strExecuted;
        if (!strFailed.empty())
            mesg += "\n    - failed: @" + strFailed;
        if (!strRetained.empty())
            mesg += "\n    - retained: @" + strRetained;
    }
    return false;
}

void onStartupState(int from, int to)
{
    if (STATE_LOGIN_CLEANUP == to)
    {
        // The value of RestrainedLove at the moment the login is pressed decides (FS does the same): turning it
        // on and cancelling the preferences leaves it off.
        if ( (gSavedSettings.get<bool>(RlvSettingNames::Main)) && (!RlvHandler::isEnabled()) )
            RlvHandler::setEnabled(true);
        // The folder locks guard the inventory from now on (plus/invguard, rlvplus.cpp)
        if (RlvHandler::isEnabled())
            enableInvGuard();
        // rlv-show: what @shownames / @showloc / @showminimap ... change in the UI (rlvui_show.cpp)
        if (RlvHandler::isEnabled())
            registerShowToggles();
        // Show the RLVa menu now that RLVa is on (the menu bar is built before the login)
        rlvMenuToggleVisible();
    }
    else if ( (STATE_MISC == to) && (RlvHandler::isEnabled()) )
    {
        // Regularly process a select subset of retained commands during logon
        gIdleCallbacks.addFunction(RlvHandler::onIdleStartup, new LLTimer());
    }
}

void cleanup()
{
    disableInvGuard();
    if (!RlvHandler::isEnabled())
    {
        RlvSettings::cleanupClass();
        return;
    }
    RlvHandler::cleanupClass();
    RlvSettings::cleanupClass();
    RlvNearbyNames::cleanupClass();
}

bool mustShowLoginScreen()
{
    return gSavedSettings.get<bool>(RlvSettingNames::Main);
}

void forceLastLocationLogin()
{
    if ( (RlvHandler::isEnabled()) && (RlvSettings::getLoginLastLocation()) )
    {
        // Force login at the last location
        LLStartUp::setStartSLURL(LLSLURL(LLSLURL::SIM_LOCATION_LAST));
    }
}

// RestrainedLove's change signal is connected from the start (the login screen can switch it).
static plus::FeatureRegistration sRegistration({ "rlv", [] { RlvSettings::initMainListener(); } });

} // namespace plus::rlv
