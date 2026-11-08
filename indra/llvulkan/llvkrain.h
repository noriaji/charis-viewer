/**
 * @file llvkrain.h
 * @brief Rain (streaks, splashes) recorded directly in Vulkan, and the
 *        overhead map's lookup shared with the wet look (research/120
 *        sections 7 and 8).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#ifndef LL_LLVKRAIN_H
#define LL_LLVKRAIN_H

// The pass of llrhirain.h as a native fullscreen pass (llvkfullscreen) laid
// over the scene's HDR target the viewer binds.
namespace LLVKRain
{
    // GLSL: overheadOpen(p, info, depth_to_ndc), how open to the sky a point
    // is (1 open, 0 under a roof) from the overhead map (LLRHI::RainOverhead).
    // The shader that includes it declares `uniform sampler2D overhead_map`
    // before it. p: the point in the map's clip space; info: x 1 with a map,
    // y the cover bias, w the edge fade (clip units); depth_to_ndc: the map's
    // stored depth to clip z (LLVKFullscreen::depthToNDC).
    extern const char* const OVERHEAD_GLSL;

    // Installs LLRHI::sRainStreaks (unless LLRHI_RAIN=0). The pipeline is
    // made at the first pass.
    void install();
}

#endif // LL_LLVKRAIN_H
