/**
 * @file invtools.h
 * @brief The MCP tools of the inventory finder: status and search over the
 *        whole inventory.
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

#ifndef PLUS_INVFINDER_INVTOOLS_H
#define PLUS_INVFINDER_INVTOOLS_H

#include "posermcp/mcphttpserver.h"

#include <functional>
#include <string>
#include <vector>

namespace plus::invfinder
{
    // Runs a step on the viewer's main thread and blocks the calling
    // (connection) thread until it finished or timeout_seconds passed.
    using MainThreadRunner =
        std::function<plus::mcp::ToolResult(std::function<plus::mcp::ToolResult()> step, double timeout_seconds)>;

    std::vector<plus::mcp::Tool> makeInvTools(const MainThreadRunner& run);

    // Text returned by "initialize" (MCP "instructions").
    std::string serverInstructions();
}

#endif // PLUS_INVFINDER_INVTOOLS_H
