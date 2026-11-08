/**
 * @file posenudge.h
 * @brief The poser MCP "nudge" tool's maths: small changes said in words
 *        ("bend the left knee 10 degrees", "left wrist to the palm side",
 *        "the left toes 3 cm out"). No viewer state.
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

// The words and how each one turns its joint come from tools/poser-mcp/nudge_motions.json
// (one source: posesolvertables.h is written from it; posenudge.py is the Python twin;
// nudge_check.py checks every word with ik_check.py's forward kinematics and dumps cases
// for ik/posesolver_test.cpp). The left side is defined, the right side is its mirror
// image (rotation axes (x, y, z) -> (-x, y, -z), directions (x, y, z) -> (x, -y, z)).
//
// Kinds: own (a turn about an axis of the joint's own frame), hinge (an elbow / knee bend),
// twist (about the joint's own bone), toward (the bone turns toward a direction of a
// reference joint's frame, the chest for arms and the pelvis for legs, and stops there).
// A turn stops at the joint's anatomical range (the same ranges as solve_pose) unless the
// caller switches limits off.
//
// Moving a point by centimetres is a solve_pose with one anchor goal (the point at its new
// place) and only the limb's shoulder + elbow / hip + knee free (moveSpec).
//
// Stage 4 (pins, not built yet): a nudge will keep the pinned contacts by solving again with
// the pins as hard goals and the nudged joint locked (the Spec has lock and anchor goals for
// that); the tool's "keep" argument is the switch reserved for it.

#ifndef PLUS_POSERMCP_POSENUDGE_H
#define PLUS_POSERMCP_POSENUDGE_H

#include "posesolver.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace plus::posermcp::nudge
{
    using solver::Mat3;
    using solver::Vec3;

    constexpr double DEFAULT_DEG = 10.0; // when the call gives no amount
    constexpr double MAX_DEG     = 45.0; // more than this in one call: aim or solve_pose
    constexpr double DEFAULT_CM  = 3.0;
    constexpr double MAX_CM      = 20.0;

    struct NudgeError : public std::invalid_argument
    {
        using std::invalid_argument::invalid_argument;
    };

    struct Motion
    {
        std::string family, name, kind, ref, check, what;
        Vec3        axis, toward; // left side / middle
        double      sign    = 1.0;
        int         segment = 0;  // fingers: only this segment (0 = any)
        bool        sided   = false;
    };

    // "mShoulder" for mShoulderLeft, "mHandIndex" for mHandIndex2Left, "mHead" for mHead; "" for others.
    std::string familyOf(const std::string& joint);
    // The finger segment (1-3) of a finger joint, else 0.
    int segmentOf(const std::string& joint);
    // The words a joint takes, for messages ("bend, straighten (also flex, extend)").
    std::string wordsOf(const std::string& joint);
    // The motion of a word (a name or an alias). Throws NudgeError naming the words the joint takes.
    const Motion& findMotion(const std::string& joint, const std::string& word);

    // The joints a request names: the joint itself; a whole finger (mHandIndexLeft: segments
    // 1-3, or only the one the word is for); fingers_left / fingers_right (every finger but the
    // thumb, the segments the word is for). Throws NudgeError.
    std::vector<std::string> expandJoints(const std::string& joint, const std::string& word);

    struct Turn
    {
        Mat3                  local;           // the new local rotation
        double                askedDeg = 0.0;
        double                movedDeg = 0.0;  // the rotation between the old and the new local rotation
        bool                  reached  = false; // toward: stopped where the bone points that way
        bool                  limited  = false; // stopped at the anatomical range
        std::string           limitMotion;      // the range that stopped it
        double                limitValue = 0.0; // its value at the asked amount
        std::optional<double> limit;
        std::string           note;
    };

    // Turns one joint of the state by a word (deg > 0). With limits, the turn stops at the
    // joint's range (body joints; flexibility as solve_pose's option).
    Turn turnJoint(const solver::Skeleton& skeleton, const solver::State& state, int joint, const Motion& motion, double deg,
                   bool limits, const std::string& flexibility);

    // A direction word for moving a point (body frame): forward, back, up, down, left, right
    // (the avatar's), out / in (away from / toward the body's middle, by the point's side).
    bool directionOf(const std::string& word, const std::string& side, Vec3& out);

    // The solve_pose spec that puts `at` on target (body frame) moving only root and middle.
    solver::Spec moveSpec(const solver::PointSpec& at, const Vec3& target, const std::string& root, const std::string& middle,
                          const std::string& flexibility);
}

#endif // PLUS_POSERMCP_POSENUDGE_H
