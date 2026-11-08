/**
 * @file posesolverlive.h
 * @brief The poser's IK drag: one joint pulled to a target every mouse move, the chain follows.
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

// research/226-poser-ik-drag.md section 2.2. A thin wrapper over posesolver's Problem: the
// dragged joint is a weak Anchor whose target is replaced every call, pinned joints are strong
// Anchors, the variables are only the dragged joint's chain (so the rest cannot move), and each
// solve starts from the previous one (one start, 1-2 outer rounds, 10-20 iterations).
// The maths only, no viewer state; built and tested on its own
// (tools/poser-mcp/ik/posesolverlive_test.cpp). Body frame as in posesolver.h.

#ifndef PLUS_POSERMCP_POSESOLVERLIVE_H
#define PLUS_POSERMCP_POSESOLVERLIVE_H

#include "posesolver.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plus::posermcp
{
    class LiveSolve
    {
    public:
        // A joint held at a world (body frame) position while another is dragged.
        struct Pin
        {
            std::string  joint;
            solver::Vec3 world;
        };

        // Modifier keys.
        struct Mods
        {
            bool trunk = false; // Ctrl: the chest, torso and pelvis follow (a limb that cannot reach turns the body)
            bool carry = false; // Shift: dragging an elbow / knee takes the wrist / ankle along, else it stays put
        };

        // Throws solver::SolveError when the joint or a pin is not in the skeleton or the
        // skeleton is bad. From `options` it uses ranges, stay, comfort, spineSmooth, maxOuter
        // (cut to 1..2) and maxIter (cut to 10..20); the rest is for solve_pose. `state` is the pose the drag starts from (also what the joints
        // pull back toward, so an elbow keeps its direction).
        LiveSolve(const solver::Skeleton& skeleton, const solver::State& state, const std::string& dragJoint,
                  const std::vector<Pin>& pins, const solver::Options& options = solver::Options(),
                  Mods mods = Mods());
        ~LiveSolve();
        LiveSolve(const LiveSolve&)            = delete;
        LiveSolve& operator=(const LiveSolve&) = delete;

        // Solves for the dragged joint at this body frame point (cut to what the chain can reach
        // unless the body follows) and returns the joints whose local rotation changed since the
        // previous call as (index into the skeleton, new local rotation). The reference stays
        // valid until the next call.
        const std::vector<std::pair<int, solver::Mat3>>& moveTarget(const solver::Vec3& bodyFrameTarget);

        // The poser's new mPelvis position offset (get_pose "position"); moves only when the pelvis is dragged.
        solver::Vec3 pelvisPosition() const;
        // The last target was out of reach or the joint stopped short of it (a range was hit).
        bool atLimit() const;
        // The dragged joint after the last call, body frame.
        solver::Vec3 dragPosition() const;
        // The state after the last call.
        const solver::State& state() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif // PLUS_POSERMCP_POSESOLVERLIVE_H
