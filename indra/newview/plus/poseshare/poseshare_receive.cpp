/**
 * @file poseshare_receive.cpp
 * @brief The poses other people share, shown on this screen (research/374 sections 5 and 6'): the gate a message has to
 *        pass, the ledger of the people shown, and the one function that ends a person's pose.
 *
 * The ledger has an entry for each person whose shared pose is held (at most FSPoserAnimator::MAX_SHARED_OTHERS), in
 * one of two states:
 *   Active   the avatar is posed (FSPoserAnimator owner Share) and each message sets its joints. When the motion is made
 *            again (flushAllMotions() after a skeleton reset) the pose held (Entry::payload) is put on it at the next look.
 *   Yielded  the poser window poses that person (owner Window: it keeps the avatar). The last message is kept; once
 *            the window lets go and 2 seconds have passed (the pose it gave may still come back to that person as a
 *            share of theirs, see research/374b), it is shown again if it has not timed out.
 * Every end of an entry goes through eraseEntry(): the entry goes, and the pose with it when it is a share's.
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
#include "plus/poser/fsjointpose.h"
#include "plus/poser/fsposeranimator.h"
#include "plus/poser/fsposingmotion.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallingcard.h"
#include "llmutelist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"

#include <map>
#include <string>

namespace plus::poseshare
{
    namespace detail
    {
        namespace
        {
            constexpr F64 REACTIVATE_DELAY_SECONDS  = 2.0;  // after the window let go
            constexpr F64 RECENT_END_SECONDS        = 3.0;  // a message older than the end that came is a late one
            constexpr int PER_PEER_PER_SECOND       = 4;
            constexpr int ALL_PER_SECOND            = 16;
            constexpr std::size_t RECENT_ENDS_MAX   = 32;
            constexpr std::size_t NEWCOMER_RATES_MAX = 32;  // people not shown yet whose messages are counted (4 a second each)

            enum class EntryState { Active, Yielded };

            struct Entry
            {
                EntryState      state{ EntryState::Active };
                F64             lastReceivedS{ 0.0 }; // of the last message taken
                int             lastSeq{ 0 };
                int             aud{ 0 };             // of the last message taken
                std::string     payload;              // the last message taken (base64): shown again after Yielded, and put on a motion that was made again
                F64             freeSinceS{ 0.0 };    // Yielded: when the window was seen to have let go (0: it holds it)
                proto::RateLimit rate{ PER_PEER_PER_SECOND };
                JointCache      cache;                // Active only
            };

            struct EndMark
            {
                int seq{ 0 };
                F64 untilS{ 0.0 };
            };

            std::map<LLUUID, Entry>   sLedger;
            std::map<LLUUID, EndMark> sRecentEnds;
            std::map<LLUUID, proto::RateLimit> sNewcomerRates; // for people with no entry: a short-lived count, kept small
            proto::RateLimit          sAllRate(ALL_PER_SECOND); // only messages that are going to be shown count

            // The 4 a second of a person who has no entry yet (a person with one has the entry's own count). The table is
            // small and its counts are kept while the ledger is empty (the usual time): receiveTick() lets go of the ones whose
            // window of a second is over, and when the table is still full the one with the oldest window goes.
            bool newcomerMayPass(const LLUUID& peer, F64 nowS)
            {
                auto it = sNewcomerRates.find(peer);
                if (it == sNewcomerRates.end())
                {
                    if (sNewcomerRates.size() >= NEWCOMER_RATES_MAX)
                    {
                        auto oldest = sNewcomerRates.begin();
                        for (auto n = sNewcomerRates.begin(); n != sNewcomerRates.end(); ++n)
                        {
                            if (n->second.windowStart() < oldest->second.windowStart())
                                oldest = n;
                        }
                        sNewcomerRates.erase(oldest);
                    }
                    it = sNewcomerRates.emplace(peer, proto::RateLimit(PER_PEER_PER_SECOND)).first;
                }
                return it->second.take(nowS);
            }

            // The rules about the person, from the setting: not ourselves, not muted or blocked, a friend when the
            // message asks for it (aud 1) or the setting does.
            bool personAllowed(const LLUUID& peer, int aud, ReceiveMode mode)
            {
                if (mode == ReceiveMode::Nobody || peer.isNull() || peer == gAgent.getID())
                    return false;
                if (LLMuteList::getInstance()->isMuted(peer) || poseconsent::blocklist::contains(peer))
                    return false;
                if ((aud == 1 || mode == ReceiveMode::Friends) && !LLAvatarTracker::instance().isBuddy(peer))
                    return false;
                return true;
            }

            // The one end of an entry: it is taken out of the ledger first (nothing below reads it), then the pose goes
            // when a share holds it. A Yielded entry's avatar is the window's: its pose is not ours to stop.
            std::map<LLUUID, Entry>::iterator eraseEntry(std::map<LLUUID, Entry>::iterator it)
            {
                const LLUUID id = it->first;
                auto         next = sLedger.erase(it);
                if (FSPoserAnimator::isHeldByShare(id))
                    FSPoserAnimator::forceStopPosingAvatar(id);
                return next;
            }

            void eraseAllEntries()
            {
                for (auto it = sLedger.begin(); it != sLedger.end();)
                    it = eraseEntry(it);
            }

            // Sets the joints of the person's avatar to `pose` (applyPoseTo()). false: no avatar, or the motion is not ours.
            bool applyPose(const LLUUID& id, Entry& entry, const proto::Pose& pose)
            {
                return applyPoseTo(FSPoserAnimator::findLiveOther(id), entry.cache, pose);
            }

            // A person whose pose the window holds: the message is kept, shown when the window lets go
            void yieldEntry(Entry& entry)
            {
                entry.cache.clear();
                entry.state      = EntryState::Yielded;
                entry.freeSinceS = 0.0;
            }

            // Yielded -> Active when the window has let go for REACTIVATE_DELAY_SECONDS. false: the entry is to end.
            bool tryReactivate(const LLUUID& id, Entry& entry, F64 nowS)
            {
                if (pushedRecently(id, nowS))
                {
                    entry.freeSinceS = 0.0; // we pushed this pose and have just stopped: it is stale, wait (the time-out ends it)
                    return true;
                }
                if (FSPoserAnimator::ownerOf(id) != PoseOwner::None)
                {
                    entry.freeSinceS = 0.0; // held again (by the window)
                    return true;
                }
                if (entry.freeSinceS == 0.0)
                {
                    entry.freeSinceS = nowS;
                    return true;
                }
                if (nowS - entry.freeSinceS < REACTIVATE_DELAY_SECONDS)
                    return true;

                proto::Pose pose;
                if (!proto::decodePose(entry.payload, pose))
                    return false; // cannot be: it was read once
                LLVOAvatar* avatar = FSPoserAnimator::findLiveOther(id);
                if (!avatar)
                    return false; // gone
                if (!shareAnimator().startShared(avatar, PoseOwner::Share))
                    return true;  // not ready (the window took it again): looked at again at the next tick
                entry.cache.clear();
                entry.state = EntryState::Active;
                return applyPose(id, entry, pose);
            }

            void onShareEnd(const LLUUID& peer, int seq)
            {
                auto it = sLedger.find(peer);
                if (it == sLedger.end() || !proto::seqIsAccepted(it->second.lastSeq, seq, true))
                    return; // nobody shown, or an end that came late
                if (sRecentEnds.size() >= RECENT_ENDS_MAX)
                {
                    // the mark that ends first goes (not all of them: a late message of a person who just ended must still be met)
                    auto first = sRecentEnds.begin();
                    for (auto m = sRecentEnds.begin(); m != sRecentEnds.end(); ++m)
                    {
                        if (m->second.untilS < first->second.untilS)
                            first = m;
                    }
                    sRecentEnds.erase(first);
                }
                sRecentEnds[peer] = EndMark{ seq, nowSeconds() + RECENT_END_SECONDS };
                eraseEntry(it);
            }

            // The gate of a message with a pose, from the cheap checks to the dear ones (research/374 section 5): the setting,
            // the bridge and a teleport; the person; the room in the ledger; the distance; the count of this person (4 a
            // second); a message from before the end that came; the reading and the sequence number; and last the 16 a
            // second in all, which only messages that are going to be shown use. The message is the
            // bridge's: its sender is the owner of the attachment that shouted it, which the simulator says, and the line
            // was checked by lslbridge (a base64 text of at most 900 characters).
            void onSharePose(const LLUUID& peer, int aud, int seq, const std::string& payload)
            {
                const ReceiveMode mode = receiveMode();
                if (mode == ReceiveMode::Nobody || lslbridge::state() != lslbridge::State::Running)
                    return;
                if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
                    return; // the avatars of this region go with the teleport: nothing is made again meanwhile

                auto it = sLedger.find(peer);
                if (!personAllowed(peer, aud, mode))
                {
                    // A person shown who no longer may be (for one: the sender went from everyone to friends only and we
                    // are not friends): the pose goes now, not after the time-out
                    if (it != sLedger.end() && proto::seqIsAccepted(it->second.lastSeq, seq, false))
                        eraseEntry(it); // (a line that came late, from before the change, is only dropped)
                    return;
                }
                if (it == sLedger.end() && sLedger.size() >= FSPoserAnimator::MAX_SHARED_OTHERS)
                    return; // 8 people (the ninth is turned away, nobody is put out)

                const F64   nowS   = nowSeconds();
                if (pushedRecently(peer, nowS))
                    return; // their share is our own push coming back: not shown (a held pose goes by its time-out)
                LLVOAvatar* avatar = FSPoserAnimator::findLiveOther(peer);
                if (!nearEnough(avatar, it != sLedger.end() ? KEEP_METERS : ENTER_METERS))
                    return;
                if (it != sLedger.end() ? !it->second.rate.take(nowS) : !newcomerMayPass(peer, nowS))
                    return; // 4 a second for a person

                auto ended = sRecentEnds.find(peer);
                if (ended != sRecentEnds.end())
                {
                    if (nowS > ended->second.untilS || proto::seqIsAccepted(ended->second.seq, seq, false))
                        sRecentEnds.erase(ended);
                    else
                        return; // a message from before the end
                }

                proto::Pose pose; // all of it is read and checked before anything is touched
                if (!proto::decodePose(payload, pose))
                {
                    LL_DEBUGS("PoseShare") << "dropped a shared pose of " << peer << ": not a pose" << LL_ENDL;
                    return;
                }
                if (it != sLedger.end() && !proto::seqIsAccepted(it->second.lastSeq, seq, false))
                    return; // a message that came late
                if (!sAllRate.take(nowS))
                    return; // 16 a second in all

                if (it == sLedger.end())
                {
                    // Taken up. When another owner has the avatar and it is not the window (a leftover of a share
                    // that has no entry), it is cleared first
                    if (FSPoserAnimator::isHeldByShare(peer))
                        FSPoserAnimator::forceStopPosingAvatar(peer);
                    const bool windowHolds = FSPoserAnimator::ownerOf(peer) == PoseOwner::Window;
                    if (!windowHolds && !shareAnimator().startShared(avatar, PoseOwner::Share))
                        return; // not ready (the avatar is not usable): the next message tries again
                    sNewcomerRates.erase(peer);
                    it = sLedger.emplace(peer, Entry{}).first;
                    Entry& fresh = it->second;
                    fresh.rate.take(nowS);
                    if (windowHolds)
                        fresh.state = EntryState::Yielded;
                    LL_INFOS("PoseShare") << "showing the shared pose of " << peer << (windowHolds ? " (waiting: the poser holds the avatar)" : "") << LL_ENDL;
                }

                Entry& entry        = it->second;
                entry.lastReceivedS = nowS;
                entry.lastSeq       = seq;
                entry.aud           = aud;
                entry.payload       = payload;
                if (entry.state == EntryState::Active && !applyPose(peer, entry, pose))
                {
                    // The motion is not there: the window took the avatar (Yielded), or it is gone (the entry ends)
                    if (FSPoserAnimator::ownerOf(peer) == PoseOwner::Window)
                        yieldEntry(entry);
                    else
                        eraseEntry(it);
                }
            }

            void onShare(const LLUUID& peer, int aud, int seq, const std::string& payload)
            {
                if (!LLApp::isRunning())
                    return;
                if (payload.empty())
                    onShareEnd(peer, seq);
                else
                    onSharePose(peer, aud, seq, payload);
            }
        }

        void receiveInit()
        {
            lslbridge::setShareHandler(&onShare);
        }

        void receiveClearAll()
        {
            sLedger.clear(); // nothing below reads it
            sRecentEnds.clear();
            sNewcomerRates.clear();
            FSPoserAnimator::forceStopAllShared();
        }

        // The ledger's look, 4 a second: every entry is checked against everything that can end or move it.
        void receiveTick(F64 nowS)
        {
            for (auto it = sRecentEnds.begin(); it != sRecentEnds.end();)
                it = (nowS > it->second.untilS) ? sRecentEnds.erase(it) : std::next(it);
            for (auto it = sNewcomerRates.begin(); it != sNewcomerRates.end();)
                it = (nowS - it->second.windowStart() >= 1.0) ? sNewcomerRates.erase(it) : std::next(it); // a spent window
            if (sLedger.empty())
                return;

            const ReceiveMode mode = receiveMode();
            if (LLAppViewer::instance()->logoutRequestSent())
            {
                receiveClearAll();
                return;
            }
            if (mode == ReceiveMode::Nobody || lslbridge::state() == lslbridge::State::Off)
            {
                // the setting, or the bridge itself turned off (PlusPoseBridgeEnable): the poses go now
                eraseAllEntries(); // (the pushes onto our own avatar, research/374b, look at the setting themselves: poseshare_pushed.cpp)
                return;
            }

            for (auto it = sLedger.begin(); it != sLedger.end();)
            {
                const LLUUID id     = it->first;
                Entry&       entry  = it->second;
                LLVOAvatar*  avatar = FSPoserAnimator::findLiveOther(id);
                bool         end    = nowS - entry.lastReceivedS > TTL_SECONDS;
                end = end || !personAllowed(id, entry.aud, mode) || !nearEnough(avatar, KEEP_METERS);
                if (!end && entry.state == EntryState::Active)
                {
                    if (FSPoserAnimator::ownerOf(id) != PoseOwner::Share)
                        yieldEntry(entry); // the window took the avatar: its pose is not stopped here
                    else if (!FSPoserAnimator::isOtherPoseAlive(id))
                        end = true; // the avatar is gone, or a new object of the person has not the motion
                    else if (FSPosingMotion* motion = shareAnimator().sharedMotion(avatar); motion && !entry.cache.matches(motion))
                    {
                        // The motion was made again (flushAllMotions() after a skeleton reset): it stands in the pose it was
                        // made in with every joint posed. The pose held is put on it now, not at the next message
                        proto::Pose pose;
                        end = !proto::decodePose(entry.payload, pose) || !applyPose(id, entry, pose);
                    }
                }
                if (!end && entry.state == EntryState::Yielded)
                    end = !tryReactivate(id, entry, nowS);
                it = end ? eraseEntry(it) : std::next(it);
            }
        }
    }

    bool isReceiving(const LLUUID& avatar_id)
    {
        return !detail::sLedger.empty() && detail::sLedger.find(avatar_id) != detail::sLedger.end();
    }

    int receivingCount()
    {
        return static_cast<int>(detail::sLedger.size());
    }
}
