/**
 * @file aimsolver.h
 * @brief Bone aiming for the poser MCP "aim" tool: the maths only, no viewer state.
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

// Only needs llmath (LLVector3, LLQuaternion), like reachsolver.h, so it can be
// built and tested on its own (tools/poser-mcp/ik in the sl-viewer repository)
// against cases written from the real skeleton file by tools/poser-mcp/ik_check.py.
//
// Rotations follow the viewer: row vectors (v * q), world = local * parent.

#ifndef PLUS_POSERMCP_AIMSOLVER_H
#define PLUS_POSERMCP_AIMSOLVER_H

#include "llmath.h" // must come first (llquaternion.h insists)
#include "llquaternion.h"
#include "v3math.h"

#include <string>
#include <vector>

namespace plus::posermcp
{
    // A joint of the part of the skeleton the solver works on: every aimed joint
    // and all of its ancestors (or at least up to an ancestor whose world rotation
    // does not change).
    struct AimJoint
    {
        int          parent = -1;   // index into the joint list; -1 = the parent is not in the list
        LLQuaternion local;         // current local rotation (as drawn)
        LLQuaternion parentWorld;   // only when parent == -1: world rotation of the parent
    };

    enum class AimTwist
    {
        Auto, // hinge joints (elbows, knees): bend about the hinge only (the direction projected onto
              // what the hinge can reach); others: Keep
        Keep, // smallest rotation from the bone's current orientation (keeps its twist)
        Rest  // smallest rotation from the rest orientation (local rotation 0) under the new parent
    };

    // An upper arm / thigh whose forearm / shin direction is closer to its own than this is
    // "almost straight": the twist that would let the elbow / knee bend onto it is not
    // defined (any twist bends by the same few degrees), so limb_roll is not used and the
    // twist is taken from the rest pose (as twist Rest).
    constexpr float AIM_STRAIGHT_LIMB_DEG = 15.f;
    // An elbow / knee may bend this far the wrong way (the rest legs are not quite straight).
    constexpr float AIM_HINGE_BACK_DEG = 5.f;
    // Warnings (mesh folding): a joint turned further than this from its rest rotation, or
    // twisted further than this about its own bone. Hinge bends and limb rolls (upper arm /
    // thigh twisted so that the elbow / knee bends onto the forearm / shin) are the pose itself
    // and are not warned about.
    constexpr float AIM_WARN_ANGLE_DEG = 120.f;
    constexpr float AIM_WARN_TWIST_DEG = 90.f;

    // How a bone's rotation was found.
    enum class AimMethod
    {
        Swing, // smallest turn (no roll)
        Side,  // roll: the reference side axis turned toward rollToward
        Limb,  // roll of an upper arm / thigh: twisted so that the elbow / knee, bending about its
               // hinge only, can put the forearm / shin onto rollToward
        Hinge  // elbow / knee bent about its hinge only (local rotation = the bend)
    };

    struct AimBone
    {
        int       joint = -1;      // index into the joint list
        LLVector3 bone;            // where the bone points, in the joint's own frame (need not be unit)
        LLVector3 direction;       // wanted bone direction, world (need not be unit)
        AimTwist  twist = AimTwist::Auto;

        bool      roll = false;    // turn the bone about its own axis (see side / limb)
        LLVector3 rollToward;      // world
        LLVector3 side;            // the joint's reference side axis, in its own frame

        // Upper arm / thigh: the child limb, which makes the roll a Limb roll.
        bool      limb = false;
        LLVector3 childBone;       // forearm / shin, in the elbow's / knee's own frame
        LLVector3 childHinge;      // the elbow's / knee's hinge axis, in its own frame
        float     childNatural = 1.f; // sign of the bend angle that bends the natural way

        // Elbow / knee: its hinge (used with AimTwist::Auto).
        bool      hinge = false;
        LLVector3 hingeAxis;       // own frame
        float     natural = 1.f;
    };

    struct AimResult
    {
        LLQuaternion local;           // new local rotation of the joint
        LLQuaternion world;           // new world rotation of the joint
        LLVector3    achieved;        // unit bone direction with the new rotation, world
        float        error     = 0.f; // radians between achieved and wanted
        AimMethod    method    = AimMethod::Swing;
        float        rollError = 0.f; // Side: radians between the side axis and rollToward (both across the bone);
                                      // Limb: radians the forearm / shin can not follow rollToward by
        float        bend      = 0.f; // Limb / Hinge: the elbow / knee bend about its hinge (radians, signed)
        float        hingeMiss = 0.f; // Hinge: radians the bone misses the asked direction (the hinge can not
                                      // reach it; 0 when it can, to 0.25 degrees, which a tiny swing takes up)
        bool         hingeClamped = false; // Hinge: the bend was limited to AIM_HINGE_BACK_DEG the wrong way
        bool         rollIgnored  = false; // an elbow's / knee's roll_toward was ignored (it only bends)
        bool         straight     = false; // upper arm / thigh: almost straight limb, limb roll skipped, twist Rest
        float        angle     = 0.f; // rotation angle of the new local rotation (radians, 0..pi)
        float        twist     = 0.f; // its twist about the joint's own bone (radians, 0..pi)
        bool         warnAngle = false; // angle > AIM_WARN_ANGLE_DEG (not for hinge bends or limb rolls)
        bool         warnTwist = false; // twist > AIM_WARN_TWIST_DEG (not for limb rolls)
        const char*  note      = nullptr; // why a roll was not used
    };

    // The twist of a rotation (v * q) about the unit vector axis: the angle (0..pi) between
    // where q takes a vector across axis and where the smallest turn of axis onto axis * q
    // takes it (= the twist part of q, applied before its swing).
    float twistAbout(const LLQuaternion& q, const LLVector3& axis);

    // Solves every bone, parents before children (whatever the order of `bones`):
    // each bone uses the new rotations of the bones above it. results[i] belongs to
    // bones[i]. Returns false and sets why / bad_bone (index into bones, or -1) for
    // input that cannot be solved: a zero direction or bone, a joint listed twice,
    // a bad index, a loop in the parent links.
    bool solveAim(const std::vector<AimJoint>& joints, const std::vector<AimBone>& bones, std::vector<AimResult>& results,
                  std::string& why, int& bad_bone);

    // The smallest rotation (v * q) that turns unit vector a onto unit vector b. For
    // opposite vectors the axis is the one across a closest to the coordinate axis
    // a is least along (ties: X before Y before Z).
    LLQuaternion swingRotation(const LLVector3& a, const LLVector3& b);
}

#endif // PLUS_POSERMCP_AIMSOLVER_H
