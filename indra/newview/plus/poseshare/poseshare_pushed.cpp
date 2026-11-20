/**
 * @file poseshare_pushed.cpp
 * @brief The pose another person sets on our avatar, shown on our own screen (research/374b): the person (P) poses
 *        us in their poser and, with our consent of stage 2 (plus::poseconsent::reflect), their viewer pushes the pose
 *        to ours through the bridge (poseshare_send.cpp). We put it on our own avatar, and the share of the own pose
 *        (poseshare_send.cpp, our setting PlusPoseShareMode) shows it to the Charis Viewers near us when we share. If we
 *        do not share, it is on our screen only.
 *
 * One person at a time (the first to come); a state of a single place:
 *   Absent  nothing is held.
 *   Active  a pose of `peer` is on our avatar (the owner of the motion is Push, poseshare_send.cpp reads it as the
 *           window's own pose).
 * In: a message that passes the gate (below). Out, all through eraseEntry(): the end of the push, 20 s without a
 * message, the person gone or beyond 55 m, stage 2 or stage 1 no longer allowed, the person muted or blocked, our own
 * window taking the avatar (the window's pose comes first: it is not given back after), the motion gone, a teleport or a
 * region change, a logout, the bridge off, our setting "receive shared poses" turned to nobody. There is no Yielded
 * state: a message that comes while our window poses us is dropped, and the next one after the window let go is taken.
 * "Our window poses us" is: a pose of ours running, or stopped while the poser window is open (a pause to compare
 * is still us posing: it keeps what it has); only a stopped pose with the window closed is given up to a push.
 *
 * The gate, from the cheap checks to the dear ones: the bridge and the session; our setting for shared poses is not
 * "nobody"; the person is a friend (a cheap early return: mayReflect looks again), not ourselves, not muted or
 * blocked, and stage 2 and stage 1 allow them (poseconsent::reflect::mayReflect: each message, not only the first); our
 * avatar not posed by our window; no one else is held; the person near; 4 a second; a message from before the end
 * that came; the whole message read and checked (poseshareproto), then the sequence number.
 *
 * Main thread only. The message is the bridge's: its sender is the owner of the attachment that said it to us, which
 * the simulator says, and the line was checked by lslbridge (base64 text of at most 900 characters).
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
#include "plus/poseconsent/poseblocklist.h"
#include "plus/poseconsent/posereflect.h"
#include "plus/poser/fsposeranimator.h"
#include "plus/poser/fsposingmotion.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallingcard.h"
#include "llfloaterreg.h"
#include "llmutelist.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

#include <string>

namespace plus::poseshare
{
    namespace detail
    {
        namespace
        {
            constexpr F64 RECENT_END_SECONDS = 3.0; // a message older than the end that came is a late one
            constexpr int PER_SECOND         = 4;

            struct Entry
            {
                LLUUID           peer;                // null: Absent
                F64              lastReceivedS{ 0.0 };
                int              lastSeq{ 0 };
                std::string      payload;             // the last message taken: put on a motion that was made again
                proto::RateLimit rate{ PER_SECOND };
                JointCache       cache;
            };
            Entry sEntry;

            struct EndMark
            {
                LLUUID peer;
                int    seq{ 0 };
                F64    untilS{ 0.0 };
            };
            unsigned         sGeneration = 0; // bumped when a push starts or ends (isPushedBy(), the reminder window)
            EndMark          sRecentEnd;
            proto::RateLimit sNewRate(PER_SECOND); // messages of people not held (only people stage 2 allows get here)

            bool held() { return sEntry.peer.notNull(); }

            // The one end: the entry is cleared first (nothing below reads it), then the pose goes when a push holds our
            // avatar (not when the window does: that pose is the window's)
            void eraseEntry()
            {
                if (held())
                    ++sGeneration;
                sEntry.peer.setNull();
                sEntry.payload.clear();
                sEntry.cache.clear();
                sEntry.lastSeq = 0;
                sEntry.rate    = proto::RateLimit(PER_SECOND); // the next person starts with a whole second
                if (FSPoserAnimator::isHeldByShare(gAgent.getID()))
                    FSPoserAnimator::forceStopPosingAvatar(gAgent.getID());
            }

            bool sessionAllows()
            {
                return lslbridge::state() == lslbridge::State::Running && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE &&
                       !LLAppViewer::instance()->logoutRequestSent();
            }

            // The person: allowed by both stages, not ourselves, not muted, not blocked; and our setting for shared poses
            // is not "nobody" (it is also the way to stop what is pushed onto us, research/374b section 3)
            bool personAllowed(const LLUUID& peer)
            {
                if (peer.isNull() || peer == gAgent.getID() || receiveMode() == ReceiveMode::Nobody)
                    return false;
                if (!LLAvatarTracker::instance().isBuddy(peer))
                    return false; // stage 2 is for friends (and ends when they stop being)
                if (LLMuteList::getInstance()->isMuted(peer) || poseconsent::blocklist::contains(peer))
                    return false;
                return poseconsent::reflect::mayReflect(peer);
            }

            LLVOAvatar* ownAvatar()
            {
                return (gAgentAvatarp.isNull() || gAgentAvatarp->isDead()) ? nullptr : gAgentAvatarp.get();
            }

            // Our own window poses us: it comes first, what is pushed is dropped. A pose that runs, or one that is stopped
            // while the poser window is open (the person pauses to compare: it keeps its joints and its undo, which a
            // push would take away). A stopped pose with the window closed is not in use.
            bool windowPosesUs(LLVOAvatar* avatar)
            {
                PoseOwner       owner  = PoseOwner::None;
                FSPosingMotion* motion = FSPoserAnimator::motionOf(avatar, &owner);
                if (!motion || owner != PoseOwner::Window)
                    return false;
                return !motion->isStopped() || LLFloaterReg::instanceVisible("fs_poser");
            }

            void onPushEnd(const LLUUID& peer, int seq)
            {
                if (!held() || sEntry.peer != peer || !proto::seqIsAccepted(sEntry.lastSeq, seq, true))
                    return; // nobody held from them, or an end that came late
                sRecentEnd = EndMark{ peer, seq, nowSeconds() + RECENT_END_SECONDS };
                eraseEntry();
            }

            void onPushPose(const LLUUID& peer, int seq, const std::string& payload)
            {
                if (!sessionAllows())
                    return;
                if (!personAllowed(peer))
                {
                    if (held() && sEntry.peer == peer)
                        eraseEntry(); // a person held who no longer may be: the pose goes now, not after the time-out
                    return;
                }
                LLVOAvatar* own = ownAvatar();
                if (!own || windowPosesUs(own))
                    return;
                if (held() && sEntry.peer != peer)
                    return; // one person at a time: the first keeps it

                const F64   nowS   = nowSeconds();
                const bool  isHeld = held();
                if (!nearEnough(FSPoserAnimator::findLiveOther(peer), isHeld ? KEEP_METERS : ENTER_METERS))
                    return;
                if (isHeld ? !sEntry.rate.take(nowS) : !sNewRate.take(nowS))
                    return; // 4 a second

                if (sRecentEnd.peer == peer)
                {
                    if (nowS > sRecentEnd.untilS || proto::seqIsAccepted(sRecentEnd.seq, seq, false))
                        sRecentEnd.peer.setNull();
                    else
                        return; // a message from before the end
                }

                proto::Pose pose; // all of it is read and checked before anything is touched
                if (!proto::decodePose(payload, pose))
                {
                    LL_DEBUGS("PoseShare") << "dropped a pushed pose of " << peer << ": not a pose" << LL_ENDL;
                    return;
                }
                if (isHeld && !proto::seqIsAccepted(sEntry.lastSeq, seq, false))
                    return; // a message that came late

                if (!isHeld)
                {
                    // Taken up. A leftover push or share on our avatar (no entry for it) is cleared first
                    if (FSPoserAnimator::isHeldByShare(gAgent.getID()))
                        FSPoserAnimator::forceStopPosingAvatar(gAgent.getID());
                    if (!shareAnimator().startShared(own, PoseOwner::Push))
                        return; // not ready (the window holds the avatar, or it is not usable): the next message tries again
                    sEntry.peer = peer;
                    sEntry.rate.take(nowS);
                    sEntry.cache.clear();
                    ++sGeneration;
                    LL_INFOS("PoseShare") << "showing the pose " << peer << " sets on us" << LL_ENDL;
                    poseconsent::reflect::tellPushStarted(peer);
                }
                sEntry.lastReceivedS = nowS;
                sEntry.lastSeq       = seq;
                sEntry.payload       = payload;
                if (!applyPoseTo(own, sEntry.cache, pose))
                    eraseEntry(); // the motion is not there (gone, or the window took the avatar)
            }

            void onPush(const LLUUID& peer, int seq, const std::string& payload)
            {
                if (!LLApp::isRunning())
                    return;
                if (payload.empty())
                    onPushEnd(peer, seq);
                else
                    onPushPose(peer, seq, payload);
            }
        }

        void pushedInit()
        {
            lslbridge::setPushHandler(&onPush);
        }

        void pushedClearAll()
        {
            eraseEntry();
            sRecentEnd.peer.setNull();
            sNewRate = proto::RateLimit(PER_SECOND);
        }

        // 4 a second: the held pose is checked against everything that can end it
        void pushedTick(F64 nowS)
        {
            if (!held())
                return;
            LLVOAvatar* own = ownAvatar();
            bool        end = !own || !sessionAllows() || nowS - sEntry.lastReceivedS > TTL_SECONDS || !personAllowed(sEntry.peer) ||
                       !nearEnough(FSPoserAnimator::findLiveOther(sEntry.peer), KEEP_METERS);
            if (!end && FSPoserAnimator::ownerOf(gAgent.getID()) != PoseOwner::Push)
                end = true; // our window took the avatar (its pose comes first and is not stopped here), or the motion is gone
            if (!end)
            {
                FSPosingMotion* motion = shareAnimator().sharedMotion(own);
                if (!motion)
                    end = true;
                else if (!sEntry.cache.matches(motion))
                {
                    // The motion was made again (flushAllMotions() after a skeleton reset): it stands in the pose it was
                    // made in with every joint posed. The pose held is put on it now, not at the next message
                    proto::Pose pose;
                    end = !proto::decodePose(sEntry.payload, pose) || !applyPoseTo(own, sEntry.cache, pose);
                }
            }
            if (end)
                eraseEntry();
        }
    }

    bool isPushedBy(const LLUUID& peer)
    {
        return detail::held() && detail::sEntry.peer == peer;
    }

    namespace detail
    {
        bool pushedNotForAround()
        {
            return held() && !poseconsent::reflect::mayShowAround(sEntry.peer);
        }
    }

    unsigned pushedGeneration()
    {
        return detail::sGeneration;
    }
}
