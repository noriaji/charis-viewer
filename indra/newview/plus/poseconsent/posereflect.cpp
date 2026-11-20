/**
 * @file posereflect.cpp
 * @brief Stage 2 of the consent (posereflect.h, research/374b): a second instance of the consent ledger on the reflect
 *        channel of the pose bridge, with the confirmation T sees.
 *
 * What ties it to stage 1 (poseconsent.cpp), all in one place (followStageOne()): P may ask only of someone stage 1
 * allowed, T takes a request only from someone stage 1 allowed (Env::isBlocked), and a stage 2 session ends when its
 * stage 1 does, or when the two are no longer friends (stage 2 is for friends only). Looked at once a second (the ledger's own look), with the check of every push in poseshare as the
 * second guard.
 *
 * The bridge's state is looked at in that look too (the bridge has one state handler, poseconsent's): a bridge that
 * went (off, taken off, failed, silent) ends the requests and permissions to pose others, as in stage 1.
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

#include "posereflect.h"

#include "poseblocklist.h"
#include "poseconsentcommon.h"
#include "poseconsentcore.h"
#include "poseconsentui.h"

#include "plus.h"
#include <set>
#include "plus/lslbridge/lslbridge.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llstartup.h"
#include "llviewermessage.h"

namespace plus::poseconsent::reflect
{
    namespace
    {
        constexpr F64  IDLE_SECONDS = 1.0;
        constexpr char NOTIFY_ASK[] = "PlusPoseReflectAsk";     // the confirmation (notifications.xml)
        constexpr char LOG[] = "PoseConsent";

        // The notices of the ledger, as tips (strings_plus.xml; [NAME]). The poser window shows its own line for the
        // state (the label of its button).
        const char* noticeString(core::Notice notice)
        {
            switch (notice)
            {
                case core::Notice::Accepted:        return "PlusPoseReflectAccepted";
                case core::Notice::Refused:         return "PlusPoseReflectRefused";
                case core::Notice::NoAnswer:        return "PlusPoseReflectNoAnswer";
                case core::Notice::AllowNeverSent:  return "PlusPoseReflectAllowNeverSent";
                case core::Notice::AllowUnsure:     return "PlusPoseReflectAllowUnsure";
                case core::Notice::AskCancelledBridgeLost:
                case core::Notice::Withdrawn:
                case core::Notice::WithdrawnWhileAsking:
                case core::Notice::Ended:
                    return "PlusPoseReflectEnded";
            }
            return "PlusPoseReflectEnded";
        }

        class ReflectEnv : public WorldEnv
        {
        public:
            // The block list of stage 1 is shared (a person blocked is blocked from both). And a person stage 1 did not
            // allow cannot ask: the request is dropped as if the block list had it (the ledger answers nothing then).
            bool isBlocked(const core::Id& peer) const override
            {
                const LLUUID id = toUUID(peer);
                return blocklist::contains(id) || !mayPoseUs(id);
            }
            // Friends only (what the poses of a person do to our avatar and to who sees it is for friends; and the permission
            // ends when they stop being friends: followStageOne()). Each request is a confirmation: no automatic yes.
            core::Policy policy() const override { return core::Policy::Friends; }

            bool send(core::Action action, const core::Id& nonce, const core::Id& peer) override
            {
                const LLUUID peer_id = toUUID(peer);
                const bool sent = lslbridge::sendReflect(toBridge(action), toUUID(nonce), peer_id);
                LL_INFOS(LOG) << "reflect " << actionName(action) << " to " << peer_id << (sent ? "" : ": not sent (no bridge or busy)") << LL_ENDL;
                return sent;
            }

            // The pushes check canPush() every time; nothing is held that needs a stop here
            void consentLost(const core::Id& peer) override
            {
                LL_INFOS(LOG) << "reflect: " << toUUID(peer) << " no longer usable (ended, or out of range): pushes stop" << LL_ENDL;
            }

            void notice(core::Notice notice, const core::Id& peer) override
            {
                const LLUUID peer_id = toUUID(peer);
                if (notice == core::Notice::OutOfRange)
                {
                    // The core says it each time the person crosses the 50 m line: a tip each time would pile up (the
                    // button shows the state)
                    LL_DEBUGS(LOG) << "reflect: " << peer_id << " out of range" << LL_ENDL;
                    return;
                }
                const char* id = noticeString(notice);
                LL_INFOS(LOG) << id << ": " << peer_id << LL_ENDL;
                LLStringUtil::format_map_t args;
                args["[NAME]"] = nameForNotice(peer_id);
                showMessage(id, args);
            }
        };

        ReflectEnv sEnv;
        core::Ledger sLedger(sEnv);

        constexpr int OPTION_ALLOW_AROUND = 3;  // the button "allow, and to the people near me too"
        std::set<LLUUID> sAround;               // people allowed with that button (only meaningful while still allowed)
        std::set<LLUUID> sPushTold;             // people whose first push was told to us (once for each permission)

        bool isGrantedNow(const LLUUID& peer)
        {
            const core::Id id = toId(peer);
            for (std::size_t i = 0; i < sLedger.grantedCount(); ++i)
            {
                if (sLedger.grantedPeer(i) == id)
                    return !sLedger.grantedWithdrawPending(i);
            }
            return false;
        }

        // The answer of a confirmation: the choice is kept for the permission it gave (a refusal or an expired one: none)
        void onAnswered(const LLUUID& peer, int option, core::Answer answer)
        {
            if (answer == core::Answer::Allowed || answer == core::Answer::Busy)
            {
                if (option == OPTION_ALLOW_AROUND)
                    sAround.insert(peer);
                else
                    sAround.erase(peer);
            }
        }

        void afterChange();
        void syncDialog();
        Confirmation sConfirmation({ &sLedger, "reflect", NOTIFY_ASK, &afterChange, &syncDialog, OPTION_ALLOW_AROUND, &onAnswered });
        void syncDialog() { sConfirmation.sync(); }

        // After anything that may have changed the ledger: the confirmation follows it, and the reminder window opens
        // (without the focus) when someone was allowed, so that the permission is not forgotten
        std::size_t sGrantedSeen = 0;
        void afterChange()
        {
            sConfirmation.sync();
            const std::size_t granted = sLedger.grantedCount();
            if (granted > sGrantedSeen && !leaving())
            {
                showGrantedFloater(false);
            }
            sGrantedSeen = granted;
        }

        F64  sNextIdle = 0.0;
        bool sBridgeWasThere = true;
        std::vector<core::Asked> sAskedScratch;     // the ledger's list, for pushTargets() (kept for its room)

        // A session of stage 2 ends when its stage 1 does: P's Accepted when stage 1 is not Accepted any more, T's
        // permission when stage 1 is withdrawn. (Stage 1 ends by its own exits: a withdraw, the peer leaving, a bridge
        // gone, a teleport.) The one place that knows; onFrame() runs it before the ledger's look.
        void followStageOne()
        {
            sLedger.endWhereParentGone(
                [](const core::Id& peer) { return sEnv.isFriend(peer) && poseconsent::status(toUUID(peer)) == poseconsent::Status::Accepted; },
                [](const core::Id& peer) { return sEnv.isFriend(peer) && mayPoseUs(toUUID(peer)); }, now());
            // The choices of those who are no longer allowed go (a later permission asks again)
            for (auto it = sAround.begin(); it != sAround.end();)
            {
                it = isGrantedNow(*it) ? std::next(it) : sAround.erase(it);
            }
            for (auto it = sPushTold.begin(); it != sPushTold.end();)
            {
                it = isGrantedNow(*it) ? std::next(it) : sPushTold.erase(it);
            }
        }

        // The bridge went (the ledger's requests and permissions to pose others end; the ones we allowed stay, to
        // withdraw once it is back): once, when it goes, not every look.
        void followBridge()
        {
            const lslbridge::State state = lslbridge::state();
            const bool there = state == lslbridge::State::Running || state == lslbridge::State::Starting;
            if (there)
            {
                sBridgeWasThere = true;
                return;
            }
            if (sBridgeWasThere)
            {
                sBridgeWasThere = false;
                LL_INFOS(LOG) << "reflect: the bridge is gone: requests and permissions to be shown on others' screens end" << LL_ENDL;
                sLedger.bridgeLost(now());
            }
        }

        const char* reasonName(core::ClearReason reason)
        {
            switch (reason)
            {
                case core::ClearReason::RegionChanged: return "region change";
                case core::ClearReason::Teleport:      return "teleport";
                case core::ClearReason::Logout:        return "logout";
            }
            return "?";
        }

        void clearSession(core::ClearReason reason)
        {
            if (sLedger.active())
            {
                LL_INFOS(LOG) << "reflect: session ends (" << reasonName(reason) << "): every request and permission dropped" << LL_ENDL;
            }
            sLedger.clearAll(reason, now());
            sAround.clear();
            sPushTold.clear();
            afterChange();
        }

        void onReflectMessage(const LLUUID& nonce, lslbridge::PoseAction action, const LLUUID& peer)
        {
            if (leaving())
            {
                return;
            }
            core::Action core_action;
            if (!toCore(action, core_action))
            {
                LL_WARNS(LOG) << "a reflect message of an unknown kind from " << peer << " dropped" << LL_ENDL;
                return;
            }
            LL_DEBUGS(LOG) << "reflect " << actionName(core_action) << " from " << peer << LL_ENDL;
            sLedger.receive(toId(nonce), core_action, toId(peer), now());
            afterChange();
        }

        // A send the bridge took may not have got through (its HTTP post failed): as stage 1's
        void onSendResult(lslbridge::PoseAction action, const LLUUID& nonce, const LLUUID& peer, bool ok)
        {
            core::Action core_action;
            if (ok || leaving() || !toCore(action, core_action))
            {
                return;
            }
            LL_INFOS(LOG) << "reflect " << actionName(core_action) << " to " << peer << ": may not have been delivered (the bridge's post failed)" << LL_ENDL;
            sLedger.sendFailed(core_action, toId(nonce), toId(peer), now());
            afterChange();
        }

        // Every frame: at once back while nothing is held, and the work once a second.
        void onFrame()
        {
            if (!sLedger.active() && !sConfirmation.isShown())
            {
                sBridgeWasThere = true;     // a new session can start only with a bridge there: the next loss is seen
                return;
            }
            const F64 t = now();
            if (t < sNextIdle)
            {
                return;
            }
            sNextIdle = t + IDLE_SECONDS;
            if (leaving())
            {
                clearSession(core::ClearReason::Logout);
                return;
            }
            followBridge();
            followStageOne();
            sLedger.idle(t);
            afterChange();
        }

        void init()
        {
            gIdleCallbacks.addFunction([](void*) { onFrame(); }, nullptr);
            lslbridge::setReflectHandler(&onReflectMessage);
            lslbridge::setReflectResultHandler(&onSendResult);
        }

        void onUIReady()
        {
            gAgent.addRegionChangedCallback([]() { clearSession(core::ClearReason::RegionChanged); });
            LLViewerMessage::getInstance()->setTeleportStartedCallback([]() { clearSession(core::ClearReason::Teleport); });
        }

        plus::FeatureRegistration sRegistration({ "posereflect", &init, &onUIReady });
    }

    // ---- the contract (posereflect.h) ---------------------------------------------------------------------

    Status status(const LLUUID& peer)
    {
        switch (sLedger.status(toId(peer), now()))
        {
            case core::Status::Ask:      return Status::Ask;
            case core::Status::Pending:  return Status::Pending;
            case core::Status::Accepted: return Status::Accepted;
            case core::Status::Refused:  return Status::Refused;
            case core::Status::NoAnswer: return Status::NoAnswer;
        }
        return Status::Ask;
    }

    bool canAsk(const LLUUID& peer)
    {
        return !leaving() && poseconsent::canPose(peer) && sEnv.isFriend(toId(peer)) && sLedger.canAsk(toId(peer), now());
    }

    bool ask(const LLUUID& peer)
    {
        if (!canAsk(peer))
        {
            return false;
        }
        const bool asked = sLedger.ask(toId(peer), now());
        afterChange();
        return asked;
    }

    void cancel(const LLUUID& peer)
    {
        sLedger.cancel(toId(peer), now());
        afterChange();
    }

    bool canPush(const LLUUID& peer)
    {
        return sLedger.canPose(toId(peer)) && poseconsent::canPose(peer) && sEnv.isFriend(toId(peer));
    }

    void pushTargets(std::vector<LLUUID>& out)
    {
        out.clear();
        if (!sLedger.active())
        {
            return;
        }
        sLedger.asked(sAskedScratch);
        for (const core::Asked& asked : sAskedScratch)
        {
            const LLUUID peer = toUUID(asked.peer);
            if (asked.status == core::Status::Accepted && canPush(peer))
            {
                out.push_back(peer);
            }
        }
    }

    bool mayReflect(const LLUUID& peer)
    {
        return mayPoseUs(peer) && sEnv.isFriend(toId(peer)) && isGrantedNow(peer);
    }

    bool mayShowAround(const LLUUID& peer)
    {
        return mayReflect(peer) && sAround.contains(peer);
    }

    void withdraw(const LLUUID& peer)
    {
        sLedger.withdraw(toId(peer), now());
        afterChange();
    }

    unsigned grantedGeneration()
    {
        return sLedger.grantedGeneration();
    }

    void tellPushStarted(const LLUUID& peer)
    {
        if (!sPushTold.insert(peer).second)
        {
            return;     // told once for each permission, not at each run (a pause and a restart would pile tips up)
        }
        LLStringUtil::format_map_t args;
        args["[NAME]"] = nameForNotice(peer);
        showMessage("PlusPoseReflectPushing", args);
    }
}
