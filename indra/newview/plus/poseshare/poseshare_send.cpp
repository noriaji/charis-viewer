/**
 * @file poseshare_send.cpp
 * @brief Showing the pose of your own avatar to the people near (research/374 section 4).
 *
 * Two kinds of run use the same states: the share of the own avatar (aud: everyone or friends, shouted by the bridge to
 * the people near) and, for each person whose pose we set and who allowed it to show on their own screen (stage 2 of
 * the consent, research/374b), a push to that person only (no aud; at most 4 people at once; the pose read is the
 * one the poser window holds on their avatar). A push run ends with the share of the person being ignored for 25 s
 * (pushedRecently(): what their viewer shares is the pose we pushed). We do not know whether they took our pose or were
 * posing themselves (their own pose comes first), so for up to 33 s (25 s and the 8 s keepalive) their own shared pose
 * is not shown on our screen either; a mark in their share ("a push is on me") would tell, a change of the protocol (v2).
 *
 * The sender has three states:
 *   Idle     nothing is shared.
 *   Sharing  the setting is on, the bridge runs and you are posing your own avatar (the poser window, or a pose pushed
 *            onto you, research/374b). Four times a second the pose is packed (poseshareproto) and compared with the last
 *            message sent: a different one goes at once if the last was 0.5 s ago or more (at most 2 a second), the same one
 *            goes every 8 seconds so that people who come near, and messages that were lost, are made good.
 *            A changed pose is also sent once more 1 s after its last change (the receiving bridge drops a second message
 *            within 0.3 s, so the newest pose could be lost); every exit of Sharing cancels that.
 *   Ending   the share is over (the setting off, the pose stopped, the poser closed, the bridge not running, a teleport,
 *            a region change, a logout): the end message goes 3 times, about 0.5 s apart, the first not sooner than 0.35 s
 *            after the last share (the clock goes by 0.25 s; the bridge's lane keeps one waiting end, so an end that comes
 *            while another is still posted replaces it: 2 may arrive). A receiver that misses all of them lets the pose go
 *            by itself after 20 s. An Ending always runs to the end before a new Sharing, so that the run of
 *            sequence numbers of the one does not mix with the other; every Sharing starts from a new random number.
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

#include "poseshare.h"
#include "poseshareinternal.h"

#include "plus/lslbridge/lslbridge.h"
#include "plus/lslbridge/lslbridgeproto.h"
#include "plus/poseconsent/posereflect.h"
#include "plus/poser/fsjointpose.h"
#include "plus/poser/fsposeranimator.h"
#include "plus/poser/fsposingmotion.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llrand.h"
#include "llvoavatarself.h"

#include <algorithm>
#include <map>
#include <string>

namespace plus::poseshare
{
    // The packing and the bridge keep to the same limit: a longer pose would be packed and then refused by the bridge
    static_assert(proto::PAYLOAD_MAX == lslbridge::proto::SHARE_PAYLOAD_MAX, "poseshareproto.h and lslbridgeproto.h disagree on the size of a share");

    namespace detail
    {
        namespace
        {
            constexpr F64 MIN_SEND_INTERVAL_SECONDS = 0.5; // a changed pose: 2 a second at most
            constexpr F64 KEEPALIVE_SECONDS         = 8.0; // an unchanged pose: once in this
            constexpr F64 RESEND_AFTER_SECONDS      = 1.0; // the pose is sent once more this long after its last change (the receiving bridge drops a second message within 0.3 s)
            constexpr int END_SENDS                 = 3;
            constexpr F64 END_INTERVAL_SECONDS      = 0.5; // the clock goes by 0.25 s: every second look
            constexpr F64 END_AFTER_SHARE_SECONDS   = 0.35; // the first end is not sooner than this after the last share: the bridge of a
                                                            // receiver passes one message of a person in 0.3 s
            constexpr int SEND_PER_SECOND           = 4;   // a bug must not be able to flood the chat (the bridge keeps to it too)
            constexpr S32 SEQ_START_MAX             = 500000000; // a run starts below this and grows by 1 a message
            constexpr int SEQ_RESEED_AT             = 900000000; // (999999999 is the most the bridge takes)

            enum class SenderState { Idle, Sharing, Ending };

            constexpr std::size_t MAX_PUSH_TARGETS = 4; // people pushed to at once: the bridge sends 4 messages a second in all

            // One run of messages: the share of the own avatar (target null) or the pushes of a pose set on another
            // person (research/374b: target is that person, who shows it on their own screen).
            struct Sender
            {
                LLUUID           target;                // null: the share of the own avatar
                SenderState      state{ SenderState::Idle };
                int              seq{ 0 };              // the number of the next message
                F64              lastSentS{ 0.0 };
                std::string      lastPayload;           // the pose of the last message sent, packed
                int              lastAud{ -1 };
                JointCache       cache;                 // of the own avatar's motion
                int              endsLeft{ 0 };
                F64              nextEndS{ 0.0 };
                proto::RateLimit rate{ SEND_PER_SECOND };
                bool             resendPending{ false }; // a changed pose was sent and has not been sent again (the trailing resend)
                F64              lastChangeSentS{ 0.0 };  // when that changed pose was sent
                bool             warnedBusy{ false };   // the log line of a message the bridge refused: once a run
            };
            Sender sSender;

            bool bridgeAndSessionAllow()
            {
                if (lslbridge::state() != lslbridge::State::Running)
                    return false;
                return !LLAppViewer::instance()->logoutRequestSent() && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE;
            }

            // Whether the sender runs now by the rules. The share: the setting, the bridge, nothing that takes the avatar
            // or the region away. A push: the same, and stage 2 of the consent still holds for the person.
            bool wantsToSend(const Sender& sender)
            {
                if (!bridgeAndSessionAllow())
                    return false;
                return sender.target.isNull() ? sendMode() != SendMode::Off : poseconsent::reflect::canPush(sender.target);
            }

            // The avatar the sender reads: our own (the share) or the person pushed to. null: not there.
            LLVOAvatar* sourceAvatar(const Sender& sender)
            {
                if (sender.target.isNull())
                    return (gAgentAvatarp.isNull() || gAgentAvatarp->isDead()) ? nullptr : gAgentAvatarp.get();
                return FSPoserAnimator::findLiveOther(sender.target);
            }

            // The target pose of the posed joints, packed. false: not posing (no motion, stopped, or the owner is not
            // one that may be sent: our own pose is sent when the window or a push holds it, a person's when the window
            // does (a share of theirs is theirs, not ours to send)), or no joint is posed.
            bool packPose(Sender& sender, std::string& payload)
            {
                LLVOAvatar* avatar = sourceAvatar(sender);
                if (!avatar)
                    return false;
                PoseOwner       owner  = PoseOwner::None;
                FSPosingMotion* motion = FSPoserAnimator::motionOf(avatar, &owner);
                if (!motion || motion->isStopped())
                    return false;
                if (owner != PoseOwner::Window && !(sender.target.isNull() && owner == PoseOwner::Push))
                    return false;
                if (sender.target.isNull() && owner == PoseOwner::Push && pushedNotForAround())
                    return false; // a pose pushed onto us that we did not allow to be shown to the people near
                if (!sender.cache.matches(motion))
                    buildCache(motion, sender.cache); // (by the motion object, not its id: see JointCache)

                proto::Pose pose;
                for (std::size_t i = 0; i < proto::JOINT_COUNT; ++i)
                {
                    FSJointPose* joint = sender.cache.table[i];
                    if (!joint || !motion->currentlyPosingJoint(joint))
                        continue;
                    proto::JointValue& value = pose.joints[i];
                    value.present            = true;
                    const LLQuaternion rotation = joint->getTargetRotation();
                    value.rotation[0] = rotation.mQ[VX];
                    value.rotation[1] = rotation.mQ[VY];
                    value.rotation[2] = rotation.mQ[VZ];
                    value.rotation[3] = rotation.mQ[VW];
                    const LLVector3 position = joint->getPublicPosition();
                    const LLVector3 scale    = joint->getPublicScale();
                    value.hasPosition        = true; // the packing leaves out what is zero
                    value.hasScale           = true;
                    for (int k = 0; k < 3; ++k)
                    {
                        value.position[k] = position.mV[k];
                        value.scale[k]    = scale.mV[k];
                    }
                }
                return proto::encodePose(pose, payload);
            }

            void beginSharing(Sender& sender)
            {
                sender.state       = SenderState::Sharing;
                sender.seq         = ll_rand(SEQ_START_MAX);
                sender.lastSentS   = 0.0;
                sender.lastPayload.clear();
                sender.lastAud     = -1;
                sender.cache.clear();
                sender.warnedBusy  = false;
                sender.resendPending = false;
            }

            void beginEnding(Sender& sender, F64 nowS)
            {
                sender.state    = SenderState::Ending;
                sender.endsLeft = END_SENDS;
                sender.nextEndS = std::max(nowS, sender.lastSentS + END_AFTER_SHARE_SECONDS);
                sender.cache.clear();
                sender.lastPayload.clear();
                sender.resendPending = false; // every exit of Sharing cancels the trailing resend
            }

            bool sendMessage(const Sender& sender, int aud, int seq, const std::string& payload)
            {
                return sender.target.isNull() ? lslbridge::sendShare(aud, seq, payload) : lslbridge::sendPush(sender.target, seq, payload);
            }

            bool sendEnd(const Sender& sender, int seq)
            {
                return sender.target.isNull() ? lslbridge::sendShareEnd(seq) : lslbridge::sendPushEnd(sender.target, seq);
            }

            void sharingTick(Sender& sender, F64 nowS)
            {
                std::string payload;
                if (!wantsToSend(sender) || !packPose(sender, payload))
                {
                    beginEnding(sender, nowS);
                    return;
                }
                if (sender.seq >= SEQ_RESEED_AT)
                    sender.seq = ll_rand(SEQ_START_MAX); // a run that long: the receivers see a new one

                const int  aud      = (sender.target.isNull() && sendMode() == SendMode::Friends) ? 1 : 0; // a push has no audience: it is for one person
                const bool changed  = payload != sender.lastPayload || aud != sender.lastAud;
                const F64  since_s  = nowS - sender.lastSentS;
                const bool resend   = !changed && sender.resendPending && nowS - sender.lastChangeSentS >= RESEND_AFTER_SECONDS;
                if (changed ? since_s < MIN_SEND_INTERVAL_SECONDS : (!resend && since_s < KEEPALIVE_SECONDS))
                    return;
                if (!sender.rate.take(nowS))
                    return; // dropped quietly: the next look sends the same

                if (!sendMessage(sender, aud, sender.seq, payload))
                {
                    // The bridge is busy (4 a second, 8 waiting) or went: the same pose is tried at the next look
                    if (!sender.warnedBusy)
                    {
                        sender.warnedBusy = true;
                        LL_INFOS("PoseShare") << "the bridge did not take a " << (sender.target.isNull() ? "share" : "push") << " (busy or not running)" << LL_ENDL;
                    }
                    return;
                }
                sender.lastPayload = std::move(payload);
                sender.lastAud     = aud;
                sender.lastSentS   = nowS;
                sender.resendPending = changed; // a change is followed by one more send; the resend and the keepalive end it
                if (changed)
                    sender.lastChangeSentS = nowS;
                ++sender.seq;
            }

            void endingTick(Sender& sender, F64 nowS)
            {
                if (nowS < sender.nextEndS)
                    return;
                if (!sendEnd(sender, sender.seq))
                {
                    // Nothing can be said now (the bridge is not running): the receivers let the pose go after their
                    // time-out
                    sender.state = SenderState::Idle;
                    return;
                }
                if (--sender.endsLeft <= 0)
                {
                    sender.state = SenderState::Idle;
                    return;
                }
                sender.nextEndS = nowS + END_INTERVAL_SECONDS;
            }
        }

        // The pushes (research/374b): one run for each person whose pose we set and who allowed it to show on their own
        // screen (stage 2 of the consent). Looked at with the share, 4 times a second.
        namespace
        {
            std::map<LLUUID, Sender> sPushers;
            std::map<LLUUID, F64>    sPushEnds;      // person -> until when their share is not shown (PUSH_STALE_SECONDS after the run)
            constexpr std::size_t    PUSH_ENDS_MAX = 32;
            std::vector<LLUUID>      sPushTargets;

            void markPushEnded(const LLUUID& peer, F64 nowS)
            {
                if (sPushEnds.size() >= PUSH_ENDS_MAX && !sPushEnds.contains(peer))
                {
                    auto first = sPushEnds.begin(); // the mark that ends first goes
                    for (auto m = sPushEnds.begin(); m != sPushEnds.end(); ++m)
                    {
                        if (m->second < first->second)
                            first = m;
                    }
                    sPushEnds.erase(first);
                }
                sPushEnds[peer] = nowS + PUSH_STALE_SECONDS;
            }

            void tickSender(Sender& sender, F64 nowS)
            {
                if (sender.state == SenderState::Sharing)
                    sharingTick(sender, nowS);
                if (sender.state == SenderState::Ending) // also in the look that began it (when the first end is due)
                    endingTick(sender, nowS);
            }

            void pushTick(F64 nowS)
            {
                for (auto it = sPushEnds.begin(); it != sPushEnds.end();)
                    it = (nowS > it->second) ? sPushEnds.erase(it) : std::next(it);
                poseconsent::reflect::pushTargets(sPushTargets); // empty at once while no permission is held
                if (sPushTargets.empty() && sPushers.empty())
                    return;
                if (bridgeAndSessionAllow())
                {
                    for (const LLUUID& target : sPushTargets)
                    {
                        if (sPushers.size() >= MAX_PUSH_TARGETS)
                            break; // the others wait: nobody is put out
                        if (sPushers.contains(target))
                            continue;
                        Sender fresh;
                        fresh.target = target;
                        std::string probe; // a run starts only once there is a pose to send
                        if (!packPose(fresh, probe))
                            continue;
                        beginSharing(fresh);
                        sPushers.emplace(target, std::move(fresh));
                    }
                }
                for (auto it = sPushers.begin(); it != sPushers.end();)
                {
                    tickSender(it->second, nowS);
                    if (it->second.state == SenderState::Idle)
                    {
                        markPushEnded(it->first, nowS);
                        it = sPushers.erase(it);
                    }
                    else
                        ++it;
                }
            }
        }

        bool pushedRecently(const LLUUID& peer, F64 nowS)
        {
            if (sPushers.contains(peer))
                return true;
            auto it = sPushEnds.find(peer);
            return it != sPushEnds.end() && nowS <= it->second;
        }

        // A region change or a teleport: every run ends (the ends are sent where the bridge can)
        void sendBeginEnding(F64 nowS)
        {
            if (sSender.state == SenderState::Sharing)
                beginEnding(sSender, nowS);
            for (auto& entry : sPushers)
            {
                if (entry.second.state == SenderState::Sharing)
                    beginEnding(entry.second, nowS);
            }
        }

        // 4 a second. With the setting off and nothing to end it is a cached setting and an empty map.
        void sendTick(F64 nowS)
        {
            pushTick(nowS);
            if (sSender.state == SenderState::Idle)
            {
                if (!wantsToSend(sSender))
                    return;
                std::string probe; // a share starts only once there is a pose to send
                if (!packPose(sSender, probe))
                    return;
                beginSharing(sSender);
            }
            tickSender(sSender, nowS);
        }
    }
}
