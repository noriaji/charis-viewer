/**
 * @file llvkocclusion.h
 * @brief The Vulkan backend's side of the occlusion box batch (research/137 N8).
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

#ifndef LL_LLVKOCCLUSION_H
#define LL_LLVKOCCLUSION_H

#include "stdtypes.h"

#include <string>

// Implemented in llvkgl_occlusion.cpp (with the queries and their marks).
namespace LLVKOcclusion
{
    // Installs LLRHI::sOcclusionBoxesReady, sOcclusionBoxes and
    // sOcclusionResult. Called from LLRHIVKMirror::install after
    // LLVKGL::install.
    void install();

    // The batch's counts per frame over `frames` since the last call, the
    // backend's and the viewer side's (LLRHI::Occlusion::takeStats), in words;
    // resets them. Empty while the switch is off and nothing was counted.
    std::string report(U32 frames);
}

#endif // LL_LLVKOCCLUSION_H
