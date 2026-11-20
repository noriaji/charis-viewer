/**
 * @file poseconsentcommon.cpp
 * @brief The world the two consent ledgers look at, and the confirmation of a request in (poseconsentcommon.h). The
 *        code was poseconsent.cpp's own; posereflect.cpp (research/374b) uses it too.
 *
 * Logs: agents by id only, never a name.
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

#include "poseconsentcommon.h"

#include "poseblocklist.h"
#include "poseconsentui.h"

#include "plus.h"
#include "plus/lslbridge/lslbridge.h"
#include "plus/rlv/rlvhooks.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallingcard.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llframetimer.h"
#include "llmutelist.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"

#include <algorithm>
#include <cstring>

namespace plus::poseconsent
{
    namespace
    {
        constexpr char NOTIFY_MESSAGE[] = "SystemMessageTip";                    // official: [MESSAGE]
        constexpr char STRING_ALLOW_NOT_SENT[] = "PlusPoseConsentAllowNotSent";             // [NAME]: no bridge
        constexpr char STRING_ALLOW_BUSY[] = "PlusPoseConsentAllowBusy";                    // [NAME]: there, busy
        constexpr char STRING_ALLOW_EXPIRED[] = "PlusPoseConsentAllowExpired";              // [NAME]
        constexpr char STRING_BLOCK_LIST_FULL[] = "PlusPoseConsentBlockListFull";           // [NAME], [COUNT]
        // After "busy", the confirmation comes back this much later, and only once the bridge runs (while it is getting
        // ready, Starting, it is not shown again: no "busy" over and over; review 11).
        constexpr F64 BUSY_DIALOG_WAIT_SECONDS = 2.0;
        // The buttons of the confirmation (their index).
        constexpr S32 OPTION_ALLOW = 0;
        constexpr S32 OPTION_BLOCK = 2;

        const char* answerName(core::Answer answer)
        {
            switch (answer)
            {
                case core::Answer::Allowed: return "allowed";
                case core::Answer::Refused: return "refused";
                case core::Answer::Expired: return "allowed, but no longer valid (refused)";
                case core::Answer::Busy:    return "allowed, accept not sent (bridge busy; asked again)";
                case core::Answer::NotSent: return "allowed, accept not sent (no bridge)";
                case core::Answer::Gone:    return "gone";
            }
            return "?";
        }
    }

    core::Id toId(const LLUUID& uuid)
    {
        static_assert(UUID_BYTES == 16, "an LLUUID is 16 bytes");
        core::Id id;
        std::memcpy(&id.hi, uuid.mData, 8);
        std::memcpy(&id.lo, uuid.mData + 8, 8);
        return id;
    }

    LLUUID toUUID(const core::Id& id)
    {
        LLUUID uuid;
        std::memcpy(uuid.mData, &id.hi, 8);
        std::memcpy(uuid.mData + 8, &id.lo, 8);
        return uuid;
    }

    // False for a value that is none of the five (it is dropped, never taken as another action).
    bool toCore(lslbridge::PoseAction action, core::Action& out)
    {
        switch (action)
        {
            case lslbridge::PoseAction::Request:  out = core::Action::Request;  return true;
            case lslbridge::PoseAction::Accept:   out = core::Action::Accept;   return true;
            case lslbridge::PoseAction::Refuse:   out = core::Action::Refuse;   return true;
            case lslbridge::PoseAction::Cancel:   out = core::Action::Cancel;   return true;
            case lslbridge::PoseAction::Withdraw: out = core::Action::Withdraw; return true;
        }
        return false;
    }

    lslbridge::PoseAction toBridge(core::Action action)
    {
        switch (action)
        {
            case core::Action::Request:  return lslbridge::PoseAction::Request;
            case core::Action::Accept:   return lslbridge::PoseAction::Accept;
            case core::Action::Refuse:   return lslbridge::PoseAction::Refuse;
            case core::Action::Cancel:   return lslbridge::PoseAction::Cancel;
            case core::Action::Withdraw: return lslbridge::PoseAction::Withdraw;
        }
        return lslbridge::PoseAction::Cancel;
    }

    const char* actionName(core::Action action)
    {
        switch (action)
        {
            case core::Action::Request:  return "request";
            case core::Action::Accept:   return "accept";
            case core::Action::Refuse:   return "refuse";
            case core::Action::Cancel:   return "cancel";
            case core::Action::Withdraw: return "withdraw";
        }
        return "?";
    }

    double now()
    {
        return LLFrameTimer::getElapsedSeconds();
    }

    bool leaving()
    {
        return LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent();
    }

    // The bridge is there: running, or getting ready (Starting: a new URL and its hello, wearing it again, making it
    // again; each step has its deadline (60 s to be worn, 120 s for the script; 3 repairs), but an outfit being put
    // on is waited for, which the official viewer ends after 600 s at most: research/353). Not off, taken off,
    // failed, silent.
    bool bridgeThereNow()
    {
        const lslbridge::State state = lslbridge::state();
        return state == lslbridge::State::Running || state == lslbridge::State::Starting;
    }

    // Where `peer` is from us, when they are in our region. The avatar's own position when the viewer has the
    // avatar, else the region's coarse location (whole metres; the height is not known from 1020 m up: then
    // across the ground only).
    bool locate(const LLUUID& peer, F64& distance)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || peer.isNull() || peer == gAgent.getID())
        {
            return false;
        }
        const LLVector3d agent_pos = gAgent.getPositionGlobal();
        LLViewerObject* object = gObjectList.findObject(peer);
        if (object && object->isAvatar() && !object->isDead() && !static_cast<LLVOAvatar*>(object)->isOrphaned())
        {
            if (object->getRegion() != region)
            {
                return false;
            }
            distance = dist_vec(object->getPositionGlobal(), agent_pos);
            return true;
        }
        const LLVector3d& origin = region->getOriginGlobal();
        const size_t count = std::min(region->mMapAvatarIDs.size(), region->mMapAvatars.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (region->mMapAvatarIDs[i] != peer)
            {
                continue;
            }
            const U32 packed = region->mMapAvatars[i];
            const U32 height = packed & 0xFFU;      // in 4 m steps; 255: 1020 m or more
            LLVector3d pos = origin + LLVector3d((packed >> 16) & 0xFFU, (packed >> 8) & 0xFFU, height * 4.0);
            if (height == 0xFFU)
            {
                pos.mdV[VZ] = agent_pos.mdV[VZ];
            }
            distance = dist_vec(pos, agent_pos);
            return true;
        }
        return false;
    }

    bool isPresent(const LLUUID& peer, F64& distance)
    {
        return locate(peer, distance) && !LLMuteList::getInstance()->isMuted(peer);
    }

    bool avatarName(const LLUUID& agent_id, std::string& name, void (*on_arrival)())
    {
        LLAvatarName av_name;
        if (!LLAvatarNameCache::get(agent_id, &av_name))
        {
            if (on_arrival)
            {
                LLAvatarNameCache::get(agent_id, [on_arrival](const LLUUID&, const LLAvatarName&) { on_arrival(); });
            }
            return false;
        }
        name = av_name.getCompleteName(true, true);     // "Display (user.name)" even with display names off
        if (plus::rlv::enabled())
        {
            name = plus::rlv::avatarNameForPlus(agent_id, name);
        }
        return true;
    }

    std::string nameForNotice(const LLUUID& peer)
    {
        std::string name;
        return plus::safeText(avatarName(peer, name, nullptr) ? name : peer.asString());
    }

    void showMessage(const char* string_id, const LLStringUtil::format_map_t& args)
    {
        LLSD message;
        message["MESSAGE"] = LLTrans::getString(string_id, args);
        LLNotificationsUtil::add(NOTIFY_MESSAGE, message);
    }

    // ---- the world for a ledger ---------------------------------------------------------------------

    core::Id WorldEnv::self() const { return toId(gAgent.getID()); }
    bool WorldEnv::bridgeRunning() const { return lslbridge::state() == lslbridge::State::Running; }

    bool WorldEnv::inRegion(const core::Id& peer) const
    {
        F64 distance = 0.0;
        return locate(toUUID(peer), distance);     // muted or not
    }

    bool WorldEnv::present(const core::Id& peer) const
    {
        F64 distance = 0.0;
        return isPresent(toUUID(peer), distance);
    }

    bool WorldEnv::nearby(const core::Id& peer) const
    {
        F64 distance = 0.0;
        return isPresent(toUUID(peer), distance) && distance <= RANGE_METERS;
    }

    bool WorldEnv::isFriend(const core::Id& peer) const { return LLAvatarTracker::instance().isBuddy(toUUID(peer)); }

    core::Id WorldEnv::newNonce()
    {
        LLUUID nonce;
        nonce.generate();
        return toId(nonce);
    }

    // ---- the confirmation ----------------------------------------------------------------------------

    void Confirmation::takeDown()
    {
        if (mShownNotification.notNull())
        {
            if (LLNotificationPtr shown = LLNotifications::instance().find(mShownNotification))
            {
                LLNotifications::instance().cancel(shown);
            }
        }
        mShownNotification.setNull();
        mShownNonce.setNull();
    }

    void Confirmation::onAnswer(const LLSD& notification, const LLSD& response)
    {
        const LLSD& payload = notification["payload"];
        const LLUUID nonce = payload["nonce"].asUUID();
        const LLUUID peer = payload["peer"].asUUID();
        if (nonce == mShownNonce)
        {
            mShownNonce.setNull();
            mShownNotification.setNull();
        }
        if (nonce.isNull() || peer.isNull() || leaving())
        {
            return;
        }
        const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
        if (option == OPTION_BLOCK && blocklist::add(peer) == blocklist::Added::Full)
        {
            // As Aurora: a full list takes no one more (no one is taken off it to make room).
            LLStringUtil::format_map_t args;
            args["[NAME]"] = nameForNotice(peer);
            args["[COUNT]"] = llformat("%d", (S32)blocklist::MAX_ENTRIES);
            showMessage(STRING_BLOCK_LIST_FULL, args);
        }
        const bool allow = option == OPTION_ALLOW || (mConfig.allowAltOption >= 0 && option == mConfig.allowAltOption);
        const core::Answer answered = mConfig.ledger->answer(toId(nonce), toId(peer), allow, now());
        if (mConfig.onAnswered)
        {
            mConfig.onAnswered(peer, option, answered);
        }
        LL_INFOS("PoseConsent") << mConfig.stage << " request of " << peer << ": " << answerName(answered)
                                << (option == OPTION_BLOCK ? " (blocked)" : "") << LL_ENDL;
        const char* message = nullptr;
        switch (answered)
        {
            case core::Answer::Expired: message = STRING_ALLOW_EXPIRED;  break;
            case core::Answer::NotSent: message = STRING_ALLOW_NOT_SENT; break;
            case core::Answer::Busy:
                message = STRING_ALLOW_BUSY;
                mWaitUntil = now() + BUSY_DIALOG_WAIT_SECONDS;   // the same request is asked again then
                break;
            case core::Answer::Allowed:
            case core::Answer::Refused:
            case core::Answer::Gone:
                break;
        }
        if (message)
        {
            LLStringUtil::format_map_t args;
            args["[NAME]"] = nameForNotice(peer);
            showMessage(message, args);
        }
        mConfig.afterChange();      // the next confirmation, and the reminder window when allowed
    }

    void Confirmation::sync()
    {
        if (leaving())
        {
            takeDown();
            return;
        }
        const std::vector<core::Ledger::Incoming>& incoming = mConfig.ledger->incoming();
        if (mShownNonce.notNull())
        {
            const bool still_asked = std::any_of(incoming.begin(), incoming.end(),
                                                 [this](const core::Ledger::Incoming& in) { return toUUID(in.nonce) == mShownNonce; });
            if (still_asked)
            {
                return;
            }
            takeDown();
        }
        if (incoming.empty() || now() < mWaitUntil)
        {
            return;     // (while waiting after "busy", idle comes back every second)
        }
        if (incoming.front().busy && lslbridge::state() != lslbridge::State::Running)
        {
            return;     // allowed once while the bridge could not send: asked again once it runs (or told if it ends)
        }
        const LLUUID peer = toUUID(incoming.front().peer);
        const LLUUID nonce = toUUID(incoming.front().nonce);
        // The name is asked for once per asker (idle comes here every second while it is on its way).
        void (*on_arrival)() = (peer == mNameAskedFor) ? nullptr : mConfig.onNameArrival;
        mNameAskedFor = peer;
        std::string name;
        F64 distance = 0.0;
        if (!avatarName(peer, name, on_arrival) || !isPresent(peer, distance))
        {
            return;     // shown when the name comes (or dropped by the ledger at its next look)
        }
        LLSD args;
        args["NAME"] = plus::safeText(name);
        args["DISTANCE"] = llformat("%.0f", distance);
        LLSD payload;
        payload["nonce"] = nonce;
        payload["peer"] = peer;
        LLNotificationPtr shown = LLNotificationsUtil::add(mConfig.notification, args, payload,
            [this](const LLSD& notification, const LLSD& response) { onAnswer(notification, response); });
        if (shown)
        {
            mShownNonce = nonce;
            mShownNotification = shown->id();
        }
    }
}
