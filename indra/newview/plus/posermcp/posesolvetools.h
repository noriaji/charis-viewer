/**
 * @file posesolvetools.h
 * @brief The poser MCP tools solve_pose (many goals at once, posesolver.h) and
 *        nudge (small changes said in words, posenudge.h).
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

// How a call runs (research/poser-solver-design.md section 3, "computation and where"):
// the main thread waits for the pose to settle and copies the skeleton as drawn
// (captureSolveSkeleton: plain data, no viewer pointers) -> a worker thread solves it
// (the viewer keeps drawing) -> the main thread checks that the poser's targets did
// not change in between (else it reads the skeleton again and solves once more) and
// applies the rotations the way aim does -> waits for the skeleton to be drawn again,
// measures, and when the drawn joints miss the prediction by more than 1 cm reads the
// skeleton again and solves the same goals from there (solve_pose; at most `correct`
// times). The whole call is one main-thread job wrapped by withHistory: one history
// entry, the pose before it, "history_id" in the result.

#ifndef PLUS_POSERMCP_POSESOLVETOOLS_H
#define PLUS_POSERMCP_POSESOLVETOOLS_H

#include "posesolver.h"
#include "posertools.h"

#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace plus::posermcp
{
    // The skeleton of the avatar being posed, as drawn, for the solver.
    struct SolveCapture
    {
        solver::Skeleton skeleton;
        std::uint64_t    fingerprint = 0;    // the poser's targets when it was read (poseFingerprint)
        bool             settled     = true; // the joints had eased into their targets
    };

    // ---- implemented in posertools.cpp (PlusPoserTools is the poser window's friend) ----

    // Main thread. The avatar of args["avatar"] (default self), which this viewer must be
    // posing: the `wanted` joints it has (others are skipped) and their ancestors, as drawn,
    // in its body frame, with the poser's mPelvis position offset. Throws std::runtime_error.
    void captureSolveSkeleton(const boost::json::object& args, const std::vector<std::string>& wanted, SolveCapture& out);

    // Main thread. A hash of the poser's targets (rotations, position and scale offsets, which
    // joints are posed) of the avatar: equal values = the pose was not changed in between.
    std::uint64_t poseFingerprint(const boost::json::object& args);

    // Main thread. Whether every posed joint has eased into its target (as snapshot waits for).
    bool posesSettledNow();

    // Main thread. Sets whole local rotations (as aim and set_joints absolute: the
    // animation's base rotation of those joints is zeroed) and the mPelvis position offset.
    void applySolvedPose(const boost::json::object& args, const std::vector<std::pair<std::string, LLQuaternion>>& rotations,
                         const std::optional<LLVector3>& pelvisPosition);

    // Main thread. Where joints are drawn, in the avatar's body frame (missing ones skipped).
    std::vector<std::pair<std::string, LLVector3>> drawnPositions(const boost::json::object& args, const std::vector<std::string>& names);

    // ---- posesolvetools.cpp ----

    // solve_pose and nudge.
    void appendSolveTools(std::vector<plus::mcp::Tool>& tools, const MainThreadRunner& run);

    // A paragraph for serverInstructions().
    std::string solveInstructions();
}

#endif // PLUS_POSERMCP_POSESOLVETOOLS_H
