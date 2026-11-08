/**
 * @file posertools.h
 * @brief The MCP tools that drive the Poser (plus/poser) from the local MCP
 *        endpoint: list avatars and joints, pose, save/load, camera, snapshot.
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

#ifndef PLUS_POSERMCP_POSERTOOLS_H
#define PLUS_POSERMCP_POSERTOOLS_H

#include "mcphttpserver.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace plus::posermcp
{
    // State of a job handed to the main thread, passed to each call of its step.
    struct JobProgress
    {
        unsigned calls        = 0;   // how many times the step ran before (0 on the first call)
        double   firstCallAge = 0.0; // seconds since the first call of the step
    };

    // A unit of work for the viewer's main thread. Returning std::nullopt asks
    // to be called again on the next frame (e.g. a snapshot waiting for the pose
    // to settle). Exceptions become tool errors.
    using MainThreadStep = std::function<std::optional<plus::mcp::ToolResult>(const JobProgress&)>;

    // Runs a step on the main thread and blocks the calling (connection)
    // thread until it finished or timeout_seconds passed.
    using MainThreadRunner = std::function<plus::mcp::ToolResult(MainThreadStep step, double timeout_seconds)>;

    // The tool table. Every handler parses its arguments and then does all
    // viewer work through run(), on the main thread.
    std::vector<plus::mcp::Tool> makePoserTools(const MainThreadRunner& run);

    // Text returned by "initialize" (MCP "instructions").
    std::string serverInstructions();
}

#endif // PLUS_POSERMCP_POSERTOOLS_H
