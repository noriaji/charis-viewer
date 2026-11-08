/**
 * @file debugtools.h
 * @brief MCP tools of the viewer's local endpoint for the render switches of
 *        development starts: cue_list, cue_toggle (the cue files of
 *        LLRHI_CUE_DIR) and env_switches (the LLRHI_* switches read at
 *        startup and where their auto modes went). For tests by an assistant.
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

#ifndef PLUS_POSERMCP_DEBUGTOOLS_H
#define PLUS_POSERMCP_DEBUGTOOLS_H

#include "posertools.h"

#include <string>
#include <vector>

namespace plus::posermcp
{
    // cue_list, cue_toggle, env_switches. Every handler reads the viewer's
    // switches through run(), on the main thread; the file work (writing a
    // cue file and waiting for the viewer to take it, reading the log) is done
    // on the connection thread, so the viewer's frames do not wait for it.
    void appendDebugTools(std::vector<plus::mcp::Tool>& tools, const MainThreadRunner& run);

    // A paragraph for the server's instructions.
    std::string debugInstructions();
}

#endif // PLUS_POSERMCP_DEBUGTOOLS_H
