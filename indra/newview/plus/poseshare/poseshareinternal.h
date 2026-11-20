/**
 * @file poseshareinternal.h
 * @brief What the files of plus/poseshare share between them (not for other features).
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

#ifndef PLUS_POSESHARE_POSESHAREINTERNAL_H
#define PLUS_POSESHARE_POSESHAREINTERNAL_H

#include "plus/poseshare/poseshare.h"
#include "plus/poseshare/poseshareproto.h"

#include "llframetimer.h"
#include "lluuid.h"

#include <array>
#include <vector>

class FSJointPose;
class LLVOAvatar;
class FSPoserAnimator;
class FSPosingMotion;

namespace plus::poseshare::detail
{
    constexpr F64 TICK_SECONDS = 0.25; // the pace of the sender's look and of the ledger's look (4 a second)

    constexpr F64 TTL_SECONDS  = 20.0; // no message for this long: a shown pose goes (a share, a push)
    constexpr F64 ENTER_METERS = 50.0; // a person is taken up within this (the same as the consent's)
    constexpr F64 KEEP_METERS  = 55.0; // and let go beyond this: a person at the border does not come and go

    inline F64 nowSeconds() { return LLFrameTimer::getElapsedSeconds(); }

    // The joint poses of one posing motion by the numbers of proto::JOINT_NAMES, found once: looking a name up walks
    // all the joints of the motion, which 8 people twice a second would do 130 times each. The pointers are those of
    // the motion object `serial` names (FSPosingMotion::instanceSerial()) and live as long as it does (its joint poses
    // are made once, in onInitialize()): the cache is dropped whenever the motion may be another object or gone (a
    // stop, a change of owner). The motion id is not used to know a motion again: flushAllMotions() makes a new object
    // under the same id (research/374 review 1).
    struct JointCache
    {
        U64                                       serial{ 0 };    // 0: empty
        std::array<FSJointPose*, proto::JOINT_COUNT> table{};     // null: the skeleton has no such joint
        std::vector<FSJointPose*>                 others;         // every other joint pose of the motion (collision volumes,
                                                                  // bones the table has not): never shared, never posed here
        bool matches(const FSPosingMotion* motion) const;
        void clear();
    };
    void buildCache(FSPosingMotion* motion, JointCache& cache);

    // Sets the joints of `avatar` to `pose` through the motion shareAnimator() holds (a share of another person, or a
    // push onto the own avatar): the joints in it posed at its values, the others given back to the avatar's own
    // animations. false: no avatar, or the motion is not this instance's (the window holds it).
    bool applyPoseTo(LLVOAvatar* avatar, JointCache& cache, const proto::Pose& pose);
    // In our region and within limitMeters of us (false for a null avatar).
    bool nearEnough(LLVOAvatar* avatar, F64 limitMeters);

    // The one instance of the poser's animator that holds the shared poses of other people (Share) and the poses
    // pushed onto the own avatar (Push).
    FSPoserAnimator& shareAnimator();

    // The receiving side (poseshare_receive.cpp)
    void receiveInit();
    void receiveTick(F64 nowS);
    void receiveClearAll(); // every held pose goes (a teleport, a region change, the setting off, leaving)

    // A pose pushed onto our own avatar is held, and we did not choose to show it to the people near us: our share
    // (poseshare_send.cpp) leaves it out (research/374b)
    bool pushedNotForAround();

    // The poses of other people set on our own avatar, with our consent (poseshare_pushed.cpp, research/374b)
    void pushedInit();
    void pushedTick(F64 nowS);
    void pushedClearAll(); // the pose goes (a teleport, a region change, leaving)

    // True while we push a pose to `peer`, and for PUSH_STALE_SECONDS after the run ended: the share of `peer` is then
    // the pose we pushed, coming back (their viewer shares what it was given), and is not shown (research/374b).
    constexpr F64 PUSH_STALE_SECONDS = 25.0; // a little over TTL_SECONDS: a share that outlives the end of the push goes by its time-out
    bool pushedRecently(const LLUUID& peer, F64 nowS);

    // The sending side (poseshare_send.cpp)
    void sendTick(F64 nowS);
    void sendBeginEnding(F64 nowS); // a region change or a teleport: the share ends (the ends are sent while the bridge can)
}

#endif // PLUS_POSESHARE_POSESHAREINTERNAL_H
