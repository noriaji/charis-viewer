/**
 * @file posehistorytools.h
 * @brief The viewer side of the poser MCP's pose history: saves the own
 *        avatar's pose before every pose-changing tool, notices edits made in
 *        the poser window, keeps the history in user_settings, and the tools
 *        undo / redo / history / mark / compare. The history itself (no viewer
 *        code) is posehistory.h.
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

#ifndef PLUS_POSERMCP_POSEHISTORYTOOLS_H
#define PLUS_POSERMCP_POSEHISTORYTOOLS_H

#include "posehistory.h"
#include "posertools.h"

#include <boost/json.hpp>

#include <string>
#include <vector>

namespace plus::posermcp
{
    // ---- implemented in posertools.cpp (PlusPoserTools is the poser window's friend) ----

    // The own avatar's pose as the poser holds it (every poser joint the
    // skeleton has). False when not logged in or the own avatar is not being
    // posed. Main thread.
    bool captureSelfPose(history::Pose& out);

    // Writes a pose back to the own avatar: rotations, position and scale
    // offsets, and which joints the poser drives. Joints the skeleton does not
    // have are skipped. Returns the number of joints that had to change.
    // Throws std::runtime_error when the own avatar is not being posed. Main thread.
    std::size_t applySelfPose(const history::Pose& pose);

    // ---- posehistorytools.cpp ----

    // Main thread, a few times a second while the MCP server runs: notices
    // edits made in the poser window (one history entry per edit, once the
    // pose has been still for a moment).
    void historyIdle();

    // A drag made by hand in the 3D view (the poser's IK tool): begin saves the own avatar's pose
    // (false: not posing yourself, nothing to record), hold is called on every move so that the drag
    // is not taken for window edits, end records the whole drag as one entry (source, e.g. "ik").
    bool beginUserEdit();
    void holdUserEdit();
    void endUserEdit(const std::string& source, const std::string& summary);

    // Wraps the main-thread step of a tool that changes the pose: saves the own
    // avatar's pose before the step and records it if the step changed the
    // pose (the result then carries "history_id"). Other avatars (animesh) pass
    // through unrecorded.
    MainThreadStep withHistory(const std::string& tool, const boost::json::object& args, MainThreadStep step);

    // undo, redo, history, mark, compare.
    void appendHistoryTools(std::vector<plus::mcp::Tool>& tools, const MainThreadRunner& run);

    // A paragraph for serverInstructions().
    std::string historyInstructions();
}

#endif // PLUS_POSERMCP_POSEHISTORYTOOLS_H
