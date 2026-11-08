/**
 * @file overhead.h
 * @brief Rain: the overhead depth map around the camera (research/120
 *        section 8). Used by plus/rain only (the wet look asks rain.h).
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

#ifndef PLUS_RAIN_OVERHEAD_H
#define PLUS_RAIN_OVERHEAD_H

#include "llrhirain.h"
#include "v2math.h"

#include <glm/mat4x4.hpp>

class LLCamera;

// Main thread.
namespace plus::rain::overhead
{
    // Draws the map when it is due: none yet, the camera moved far enough
    // from where it was drawn, or it is a few seconds old. camera: the main
    // view's (agent coordinates).
    void update(LLCamera& camera);

    // Frees the map (nothing reads it).
    void release();

    // The map for a pass whose points to_agent takes to agent coordinates.
    // rain_slope: how far the rain moves sideways per metre it falls (world
    // x, y), for where slanted rain comes from. depth_texture 0: no map.
    LLRHI::RainOverhead lookup(const glm::mat4& to_agent, const LLVector2& rain_slope);
}

#endif // PLUS_RAIN_OVERHEAD_H
