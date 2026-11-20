/**
 * @file poseconsent.cpp
 * @brief Ties the consent ledger (poseconsentcore.h) to the viewer: the worn bridge (plus/lslbridge), who is near,
 *        friends, mutes, the setting PlusPoseAskPolicy, the block list, the confirmation and the reminder window.
 *        research/336 (the design, 4.1 the review, and its appendix on the states of poseconsent, the one whose
 *        Japanese heading ends in "(implementation)").
 *
 * The confirmation (plus::poseconsent::Confirmation, poseconsentcommon.h): one LLNotification at a time, for the oldest
 * request in (the ledger keeps them in order of arrival). It is synced after everything that can change the requests
 * in (afterChange()): a notification whose request is gone (timed out, cancelled, the asker left, the policy changed,
 * a region change) is taken down, and the next one is shown. The answer is checked again by the ledger (the request still there, in time, the asker near). A
 * request allowed while the bridge could not send it (busy) is not shown again until the bridge runs (2 s after the
 * press when it ran then, else at the next idle once it runs); while it waits at the front, the ones after it wait
 * too.
 *
 * The clock: LLFrameTimer::getElapsedSeconds() (F64, seconds since the viewer started). The ledger is looked at
 * once a second by an idle callback that returns at once while nothing is going on.
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

#include "poseconsent.h"

#include "poseblocklist.h"
#include "poseconsentcommon.h"
#include "poseconsentcore.h"
#include "poseconsentui.h"

#include "plus.h"
#include "plus/lslbridge/lslbridge.h"
#include "plus/rlv/rlvhooks.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llcallingcard.h"
#include "llframetimer.h"
#include "llmutelist.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lltrans.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"

#include <algorithm>
#include <cstring>

namespace plus::poseconsent
{
    namespace
    {
        constexpr F64  IDLE_SECONDS = 1.0;
        constexpr char SETTING_POLICY[] = "PlusPoseAskPolicy";
        constexpr char SETTING_SETTABLE[] = "PlusPoseConsentSettable";      // transient: true once logged in

        // notifications.xml
        constexpr char NOTIFY_ASK[] = "PlusPoseConsentAsk";                      // the confirmation
        constexpr char NOTIFY_WITHDRAW_NOT_SENT[] = "PlusPoseConsentWithdrawNotSent";
        constexpr char NOTIFY_BLOCKS_CLEARED[] = "PlusPoseConsentBlocksCleared";
        constexpr char NOTIFY_CLEAR_BLOCKS_CONFIRM[] = "PlusPoseConsentClearBlocksConfirm";   // [COUNT]; 0 OK, 1 Cancel
        constexpr char NOTIFY_NO_BLOCKS[] = "PlusPoseConsentNoBlocks";
        constexpr char NOTIFY_BLOCKS_AFTER_LOGIN[] = "PlusPoseConsentBlocksAfterLogin";
        // strings_plus.xml, shown through NOTIFY_MESSAGE
        constexpr char STRING_ALLOW_NEVER_SENT[] = "PlusPoseConsentAllowNeverSent";         // [NAME]: busy, then ended
        constexpr char STRING_ALLOW_UNSURE[] = "PlusPoseConsentAllowUnsure";                // [NAME]: its post failed
        constexpr char STRING_WITHDRAW_NOT_SENT_MANY[] = "PlusPoseConsentWithdrawNotSentMany"; // [COUNT], [REASON]
        // [REASON] of a withdraw not sent: the bridge is there (Running or Starting) but could not send it now, or there
        // is no bridge.
        constexpr char STRING_REASON_BUSY[] = "PlusPoseConsentReasonBusy";
        constexpr char STRING_REASON_NO_BRIDGE[] = "PlusPoseConsentReasonNoBridge";
        // ---- conversions ---------------------------------------------------------------------------------

        // The strings_plus.xml ids of the notices for the poser window ([NAME]: the peer's name).
        const char* noticeString(core::Notice notice)
        {
            switch (notice)
            {
                case core::Notice::Accepted:        return "PlusPoseConsentAccepted";
                case core::Notice::Refused:         return "PlusPoseConsentRefused";
                case core::Notice::NoAnswer:        return "PlusPoseConsentNoAnswer";
                case core::Notice::AskCancelledBridgeLost: return "PlusPoseConsentAskCancelledBridge";
                case core::Notice::Withdrawn:       return "PlusPoseConsentWithdrawn";
                case core::Notice::WithdrawnWhileAsking: return "PlusPoseConsentWithdrawnWhileAsking";
                case core::Notice::Ended:           return "PlusPoseConsentEnded";
                case core::Notice::OutOfRange:      return "PlusPoseConsentOutOfRange";
                case core::Notice::AllowNeverSent:  return "PlusPoseConsentAllowNeverSent";
                case core::Notice::AllowUnsure:     return "PlusPoseConsentAllowUnsure";
            }
            return "PlusPoseConsentEnded";
        }

        // ---- the world ----------------------------------------------------------------------------------
        // (who is near, the clock, the bridge: poseconsentcommon.h)

        Policy currentPolicy()
        {
            // Per account (the consent is each account's): gSavedPerAccountSettings. Before a login it holds the
            // declared default (Friends), and requests come only after one anyway.
            static LLCachedControl<S32> setting(gSavedPerAccountSettings, SETTING_POLICY, static_cast<S32>(Policy::Friends));
            const S32 value = setting;
            if (value < static_cast<S32>(Policy::Everyone) || value > static_cast<S32>(Policy::Nobody))
            {
                return Policy::Friends;     // a value no version wrote: the default
            }
            return static_cast<Policy>(value);
        }

        ConsentLostHandler sConsentLost;
        NoticeHandler sNotice;

        class ViewerEnv : public WorldEnv
        {
        public:
            bool isBlocked(const core::Id& peer) const override { return blocklist::contains(toUUID(peer)); }
            core::Policy policy() const override { return static_cast<core::Policy>(currentPolicy()); }

            bool send(core::Action action, const core::Id& nonce, const core::Id& peer) override
            {
                const LLUUID peer_id = toUUID(peer);
                const bool sent = lslbridge::sendPose(toBridge(action), toUUID(nonce), peer_id);
                LL_INFOS("PoseConsent") << actionName(action) << " to " << peer_id << (sent ? "" : ": not sent (no bridge or busy)") << LL_ENDL;
                return sent;
            }

            void consentLost(const core::Id& peer) override
            {
                const LLUUID peer_id = toUUID(peer);
                LL_INFOS("PoseConsent") << "consent to pose " << peer_id << " no longer usable" << LL_ENDL;
                if (sConsentLost)
                {
                    sConsentLost(peer_id);
                }
            }

            void notice(core::Notice notice, const core::Id& peer) override;
        };

        ViewerEnv sEnv;
        core::Ledger sLedger(sEnv);

        // ---- the confirmation ----------------------------------------------------------------------------

        F64 sNextIdle = 0.0;

        // grantedList(): made again when the ledger's list changed.
        std::vector<Granted> sGranted;
        unsigned sGrantedGeneration = 0;
        // outgoingList(): the same for the people we asked or may pose.
        std::vector<Outgoing> sOutgoing;
        std::vector<core::Asked> sAskedScratch;     // the ledger's list, filled again on a change (kept for its room)
        unsigned sAskedGeneration = ~0u;            // never the ledger's at first: the first call fills the list

        void syncDialog();
        void afterChange();
        Confirmation sConfirmation({ &sLedger, "pose", NOTIFY_ASK, &afterChange, &syncDialog });
        void syncDialog() { sConfirmation.sync(); }     // the asker's name arrived

        // Makes sGranted again when the ledger's list changed. open_for_new: show the reminder window when someone
        // was added (from afterChange() only, never from a reader such as the poser's draw).
        void refreshGranted(bool open_for_new)
        {
            if (sGrantedGeneration == sLedger.grantedGeneration())
            {
                return;
            }
            sGrantedGeneration = sLedger.grantedGeneration();
            bool someone_new = false;
            std::vector<Granted> granted;
            granted.reserve(sLedger.grantedCount());
            for (std::size_t i = 0; i < sLedger.grantedCount(); ++i)
            {
                const LLUUID peer = toUUID(sLedger.grantedPeer(i));
                someone_new |= std::none_of(sGranted.begin(), sGranted.end(), [&](const Granted& g) { return g.peer == peer; });
                granted.push_back({ peer, sLedger.grantedWithdrawPending(i) });
            }
            sGranted.swap(granted);
            if (open_for_new && someone_new && !leaving())
            {
                showGrantedFloater(false);
            }
        }

        // After anything that may have changed the ledger: the confirmation and the reminder follow it.
        void afterChange()
        {
            sConfirmation.sync();
            refreshGranted(true);
        }

        void ViewerEnv::notice(core::Notice notice, const core::Id& peer)
        {
            const LLUUID peer_id = toUUID(peer);
            const char* id = noticeString(notice);
            LL_INFOS("PoseConsent") << id << ": " << peer_id << LL_ENDL;
            if (notice == core::Notice::AllowNeverSent || notice == core::Notice::AllowUnsure)
            {
                // Our own side's (the confirmation's), not the poser's.
                LLStringUtil::format_map_t args;
                args["[NAME]"] = nameForNotice(peer_id);
                showMessage(notice == core::Notice::AllowNeverSent ? STRING_ALLOW_NEVER_SENT : STRING_ALLOW_UNSURE, args);
                return;
            }
            if (sNotice)
            {
                sNotice(id, peer_id);
            }
        }

        // ---- the hooks -----------------------------------------------------------------------------------

        void onPoseMessage(const LLUUID& nonce, lslbridge::PoseAction action, const LLUUID& peer)
        {
            if (leaving())
            {
                return;
            }
            core::Action core_action;
            if (!toCore(action, core_action))
            {
                LL_WARNS("PoseConsent") << "a pose message of an unknown kind from " << peer << " dropped" << LL_ENDL;
                return;
            }
            LL_DEBUGS("PoseConsent") << actionName(core_action) << " from " << peer << LL_ENDL;
            sLedger.receive(toId(nonce), core_action, toId(peer), now());
            afterChange();
        }

        // A send the bridge took may not have got through (its HTTP post failed): the ledger puts a withdraw back to be
        // sent again, and keeps an allow listed for the person to withdraw, telling them it is unsure (integration
        // reviews L2 and 2 M1).
        void onSendResult(lslbridge::PoseAction action, const LLUUID& nonce, const LLUUID& peer, bool ok)
        {
            core::Action core_action;
            if (ok || leaving() || !toCore(action, core_action))
            {
                return;
            }
            LL_INFOS("PoseConsent") << actionName(core_action) << " to " << peer << ": may not have been delivered (the bridge's post failed)" << LL_ENDL;
            sLedger.sendFailed(core_action, toId(nonce), toId(peer), now());
            afterChange();
        }

        void onBridgeState(lslbridge::State state)
        {
            if (leaving())
            {
                // Logging out or quitting: bridgeLost()'s sends and its "request taken back" notices are left out;
                // idle() empties the ledger at the logout without sending anything. The Accepted still get one
                // ConsentLostHandler and one Ended notice there (the poser stops their poses; it looks its window up
                // with findTypedInstance, so a notice opens no window).
                return;
            }
            switch (state)
            {
                case lslbridge::State::Running:
                case lslbridge::State::Starting:
                    return;
                case lslbridge::State::Off:
                case lslbridge::State::Detached:
                case lslbridge::State::Failed:
                case lslbridge::State::Silent:
                    break;
            }
            if (sLedger.active())
            {
                LL_INFOS("PoseConsent") << "the bridge is gone: requests out and in, and permissions to pose others, end"
                                        << " (the people we allowed stay listed, to withdraw later)" << LL_ENDL;
                sLedger.bridgeLost(now());
                afterChange();
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
                LL_INFOS("PoseConsent") << "session ends (" << reasonName(reason) << "): every request and permission dropped" << LL_ENDL;
            }
            sLedger.clearAll(reason, now());
            afterChange();
        }

        // Preferences > Privacy: the pose settings (per account) can be changed only once logged in (integration review
        // 2, L4): set once when the login is done (a logout ends the session, and the viewer with it).
        void markSettableOnceLoggedIn()
        {
            static bool sMarked = false;
            if (sMarked || LLStartUp::getStartupState() < STATE_STARTED)
            {
                return;
            }
            sMarked = true;
            gSavedSettings.setBOOL(SETTING_SETTABLE, true);
            // Nothing else is needed for Preferences opened at the login screen (integration review 4, L1): its Privacy
            // page (LLPanelPreferencePrivacy) remembers no values before STATE_STARTED but AutoDisengageMic
            // (llfloaterpreference.cpp, LLPanelPreferencePrivacy::saveSettings), so its Cancel puts no default back.
        }

        void onFrame()
        {
            markSettableOnceLoggedIn();     // a bool once logged in
            idle();     // at once back while nothing is held; at most once a second otherwise
        }

        // Preferences > Privacy, "Unblock all": asked first with the number (it cannot be undone). Before a login the
        // account's list is not read yet (count() is 0 then, which says nothing): "available after you log in".
        // Logged in with no one blocked: said so, not "0 people may ask again".
        void clearBlocks()
        {
            if (gAgent.getID().isNull() || LLStartUp::getStartupState() < STATE_STARTED)
            {
                LLNotificationsUtil::add(NOTIFY_BLOCKS_AFTER_LOGIN);
                return;
            }
            const std::size_t blocked = blocklist::count();
            if (blocked == 0)
            {
                LLNotificationsUtil::add(NOTIFY_NO_BLOCKS);
                return;
            }
            LLSD args;
            args["COUNT"] = LLSD::Integer(blocked);
            LLNotificationsUtil::add(NOTIFY_CLEAR_BLOCKS_CONFIRM, args, LLSD(),
                [](const LLSD& notification, const LLSD& response)
                {
                    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0 || leaving())
                    {
                        return;
                    }
                    LLSD done;
                    done["COUNT"] = LLSD::Integer(blocklist::count());     // as it is now (it may have grown meanwhile)
                    blocklist::clear();
                    LLNotificationsUtil::add(NOTIFY_BLOCKS_CLEARED, done);
                });
        }

        void init()
        {
            registerFloater();
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PoseConsent.ShowGranted",
                [](LLUICtrl*, const LLSD&) { showGrantedFloater(true); });
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PoseConsent.ClearBlocks",
                [](LLUICtrl*, const LLSD&) { clearBlocks(); });
            gIdleCallbacks.addFunction([](void*) { onFrame(); }, nullptr);
            lslbridge::setPoseHandler(&onPoseMessage);
            lslbridge::setStateHandler(&onBridgeState);
            lslbridge::setSendResultHandler(&onSendResult);
        }

        void onUIReady()
        {
            gAgent.addRegionChangedCallback([]() { clearSession(core::ClearReason::RegionChanged); });
            LLViewerMessage::getInstance()->setTeleportStartedCallback([]() { clearSession(core::ClearReason::Teleport); });
        }

        plus::FeatureRegistration sRegistration({ "poseconsent", &init, &onUIReady });
    }

    // ---- shared inside plus/poseconsent (avatarName() and the rest: poseconsentcommon.cpp) -------------------------

    unsigned grantedGeneration()
    {
        return sLedger.grantedGeneration();
    }

    // ---- the contract (poseconsent.h) ----------------------------------------------------------------------

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
        return !leaving() && sLedger.canAsk(toId(peer), now());
    }

    bool ask(const LLUUID& peer)
    {
        if (leaving())
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

    bool canPose(const LLUUID& peer)
    {
        return sLedger.canPose(toId(peer));
    }

    Policy policy()
    {
        return currentPolicy();
    }

    const std::vector<Granted>& grantedList()
    {
        refreshGranted(false);
        return sGranted;
    }

    void withdraw(const LLUUID& peer)
    {
        withdrawPeers({ peer });
    }

    bool mayPoseUs(const LLUUID& peer)
    {
        const core::Id id = toId(peer);
        for (std::size_t i = 0; i < sLedger.grantedCount(); ++i)
        {
            if (sLedger.grantedPeer(i) == id)
            {
                return !sLedger.grantedWithdrawPending(i);
            }
        }
        return false;
    }

    void withdrawPeers(const std::vector<LLUUID>& peers)
    {
        // Who could not be told (no bridge, or a bridge there but not able to send right now: getting ready again, or 16
        // sends in flight): told once for all of them, and only the first time for each. idle() sends them again until
        // they go (or the peer does).
        std::vector<LLUUID> not_sent;
        for (const LLUUID& peer : peers)
        {
            if (sLedger.withdraw(toId(peer), now()) == core::Withdrawal::NotSent)
            {
                not_sent.push_back(peer);
            }
        }
        if (!not_sent.empty())
        {
            const bool busy = bridgeThereNow();     // Running or Starting: "not just now", else "no bridge"
            const std::string reason = LLTrans::getString(busy ? STRING_REASON_BUSY : STRING_REASON_NO_BRIDGE);
            LL_INFOS("PoseConsent") << not_sent.size() << " withdraw(s) not sent (" << (busy ? "busy" : "no bridge")
                                    << "), to be sent again" << LL_ENDL;
            if (not_sent.size() == 1)
            {
                LLSD args;
                args["NAME"] = nameForNotice(not_sent.front());
                args["REASON"] = reason;
                LLNotificationsUtil::add(NOTIFY_WITHDRAW_NOT_SENT, args);
            }
            else
            {
                LLStringUtil::format_map_t args;
                args["[COUNT]"] = llformat("%d", (S32)not_sent.size());
                args["[REASON]"] = reason;
                showMessage(STRING_WITHDRAW_NOT_SENT_MANY, args);
            }
        }
        afterChange();
    }

    const std::vector<Outgoing>& outgoingList()
    {
        if (sAskedGeneration != sLedger.askedGeneration())
        {
            sAskedGeneration = sLedger.askedGeneration();
            sLedger.asked(sAskedScratch);
            sOutgoing.clear();
            for (const core::Asked& asked : sAskedScratch)
            {
                sOutgoing.push_back({ toUUID(asked.peer), asked.status == core::Status::Accepted ? Status::Accepted : Status::Pending });
            }
        }
        return sOutgoing;
    }

    void setConsentLostHandler(ConsentLostHandler handler)
    {
        sConsentLost = std::move(handler);
    }

    void setNoticeHandler(NoticeHandler handler)
    {
        sNotice = std::move(handler);
    }

    void idle()
    {
        if (!sLedger.active() && !sConfirmation.isShown())
        {
            return;
        }
        const F64 t = now();
        if (t < sNextIdle)
        {
            return;     // called more often than once a second (every frame by onFrame()): the rest wait
        }
        sNextIdle = t + IDLE_SECONDS;
        if (leaving())
        {
            clearSession(core::ClearReason::Logout);
            blocklist::forget();
            return;
        }
        sLedger.idle(now());
        afterChange();
    }
}
