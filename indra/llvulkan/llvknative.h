/**
 * @file llvknative.h
 * @brief Draws recorded directly in Vulkan from viewer draw packets.
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

#ifndef LL_LLVKNATIVE_H
#define LL_LLVKNATIVE_H

#include "llrhi.h"

#include <string>

// The native path, as opposed to translating GL calls one by one (llvkgl):
// the viewer describes what to draw (LLRHI packets), and this records it with
// fixed pipelines, a bindless texture table and push constants. GL still owns
// the buffers and textures; their Vulkan copies come from LLVKTracker.
namespace LLVKNative
{
    // Installs the LLRHI hooks.
    void install();

    // LLRHI::sDrawShadowPackets.
    bool drawShadowPackets(const LLRHI::ShadowPacket* packets, size_t count, const F32* view_proj, F32 target_width,
                           const LLRHI::ShadowListPass* list);

    // Counters since the last call, for the periodic perf log.
    std::string takeStats(U32 frames);
    // Research/56 (the RHIAvatar line): skinned shadow casters' palettes copied
    // into the pass's ring, reused from the caster before, and read from the
    // frame's palette table instead; since the last call.
    void takePaletteStats(U32& copies, U32& reused, U32& rows);
}

#endif // LL_LLVKNATIVE_H
