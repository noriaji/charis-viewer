/**
 * @file contactsolver.h
 * @brief The maths of the poser MCP "contact" tool: make two body points touch.
 *        No viewer state.
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

// Only needs llmath and reachsolver, so it is built and tested on its own
// (tools/poser-mcp/ik/contact_test.cpp in the sl-viewer repository) against the
// Python twin tools/poser-mcp/contactsolver.py.
//
// A contact point is carried either by nothing (fixed: chin, hip ...) or by a limb:
// the wrist / ankle or something below it (a finger, the foot), at a fixed offset
// from the wrist / ankle that turns with the forearm / shin. Such a point can go
// anywhere on a shell around the shoulder / hip: the two-bone reach range, moved by
// the offset. The preferred side reaches toward the other one, up to a comfortable
// share of its reach; when that is not enough the other side comes the rest of the
// way (they meet in between). Each moving limb is then solved so that its contact
// point - not its wrist / ankle - lands on the goal. The viewer applies that, waits
// for the skeleton to show it, measures again and repeats until the points touch.

#ifndef PLUS_POSERMCP_CONTACTSOLVER_H
#define PLUS_POSERMCP_CONTACTSOLVER_H

#include "reachsolver.h"

namespace plus::posermcp
{
    // A point a limb can put its contact point at: minReach <= |p - centre| <= maxReach.
    struct ContactShell
    {
        LLVector3 centre;
        F32       minReach = 0.f;
        F32       maxReach = 0.f;
    };

    // Nearest point to p on or inside the shell (radially; p on the centre goes +X).
    LLVector3 projectToShell(const LLVector3& p, const ContactShell& shell);

    // The two-bone reach range of a limb as drawn (in.target is ignored).
    bool reachRange(const TwoBoneInput& in, F32& minReach, F32& maxReach, const char*& why);

    // Where the elbow / knee points now: its offset from the root -> end line, or
    // fallback when the limb is (nearly) straight (offset below straight x upper bone).
    LLVector3 autoPole(const TwoBoneInput& in, const LLVector3& fallback, F32 straight = 0.12f);

    // Where the joints and the contact point (end + offset, world, as drawn) go with
    // a solved limb. Exact forward kinematics: the wrist / ankle and everything
    // below it keep their local rotations, so they turn with the forearm / shin.
    struct LimbPrediction
    {
        LLVector3    middle;
        LLVector3    end;
        LLVector3    point;
        LLQuaternion middleWorld;
    };
    LimbPrediction predictLimb(const TwoBoneInput& in, const TwoBoneResult& solved, const LLVector3& offset);

    // Solves the limb (in.pole set, in.target ignored) so that its contact point
    // lands on goal. The wrist / ankle target is corrected by the predicted miss a
    // few times (the offset turns with the forearm / shin). Keeps the best try.
    struct LimbGoal
    {
        TwoBoneResult  solved;
        LLVector3      target;  // the wrist / ankle target that gives it
        LimbPrediction predicted;
        F32            miss = 0.f; // |goal - predicted contact point| (0 when reachable)
    };
    bool solveLimbToGoal(const TwoBoneInput& in, const LLVector3& offset, const LLVector3& goal, LimbGoal& out,
                         const char*& why, int iterations = 8, F32 tolerance = 1e-5f);

    enum class ContactMode
    {
        None, // nothing moves
        A,    // only a moves
        B,    // only b moves
        Both  // both move and meet in between
    };

    // Goal points for both contact points. A / B: that side goes to the point of its
    // shell nearest the other point. Both: a reaches toward b up to comfort x its
    // reach, then b comes the rest of the way (alternating projections onto both
    // shells: a common point when they meet, else the nearest pair). The shells of
    // a side that does not move are not read.
    struct Meeting
    {
        LLVector3 goalA;
        LLVector3 goalB;
        F32       gap = 0.f; // |goalA - goalB|: 0 when the points can touch
    };
    Meeting chooseMeeting(ContactMode mode, const LLVector3& pointA, const LLVector3& pointB, const ContactShell& shellA,
                          const ContactShell& shellB, F32 comfort);

    struct ContactSide
    {
        bool         movable = false; // on a limb below the elbow / knee
        bool         arm     = false; // that limb is an arm (preferred over a leg)
        bool         support = false; // a standing leg: never moved by auto
        bool         busy    = false; // the limb holds another contact: moved only when nothing else can
        LLVector3    point;
        ContactShell shell;
    };

    // Mover "auto": the free (not standing, not busy) sides; with two, the arm (else
    // a) alone when the other point is within its comfortable reach, otherwise both.
    // note says why nothing moves (None).
    ContactMode resolveAuto(const ContactSide& a, const ContactSide& b, F32 comfort, F32 tolerance, const char*& note);

    // Bends the trunk (mTorso and mChest by half the angle each, about one world
    // axis) so that an arm's reach shell (centre, radius reach) comes to hold goal.
    // The axis turns the centre toward the goal about the chest. angle is the
    // smallest that is enough, else (shortBy > 0) the one up to maxAngle that comes
    // nearest.
    struct TrunkBend
    {
        F32       angle = 0.f; // radians
        LLVector3 axis;        // world
        LLVector3 centre;      // where the shell centre goes
        F32       shortBy = 0.f;
    };
    TrunkBend trunkBend(const LLVector3& torso, const LLVector3& chest, const LLVector3& centre, const LLVector3& goal,
                        F32 reach, F32 maxAngle);

    // New local rotations of mTorso and mChest for a TrunkBend (world = local * parent).
    void trunkLocals(const LLQuaternion& torsoLocal, const LLQuaternion& torsoParentWorld, const LLQuaternion& chestLocal,
                     const LLQuaternion& torsoWorld, const LLVector3& axis, F32 angle, LLQuaternion& newTorso,
                     LLQuaternion& newChest);
}

#endif // PLUS_POSERMCP_CONTACTSOLVER_H
