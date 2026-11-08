/**
 * @file testscenetools.h
 * @brief Test scene: the MCP tools testscene_build, testscene_clear, testscene_stats (served by plus/posermcp).
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

#ifndef PLUS_TESTSCENE_TOOLS_H
#define PLUS_TESTSCENE_TOOLS_H

#include "posermcp/posertools.h"

#include <vector>

namespace plus::testscene
{
    // The three tools. Every handler checks its arguments on the connection thread and does the rest in run(), on
    // the main thread. They refuse while PlusTestScene is off.
    void appendTools(std::vector<plus::mcp::Tool>& tools, const plus::posermcp::MainThreadRunner& run);
}

#endif // PLUS_TESTSCENE_TOOLS_H
