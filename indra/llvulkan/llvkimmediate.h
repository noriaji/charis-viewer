/**
 * @file llvkimmediate.h
 * @brief The Vulkan backend's side of the immediate mode's fast path (research/137 N9).
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

#ifndef LL_LLVKIMMEDIATE_H
#define LL_LLVKIMMEDIATE_H

#include "stdtypes.h"

#include <string>

namespace LLVKImmediate
{
    // Installs LLRHI::sImmediateDraw when direct draws are on. Called from
    // LLRHIVKMirror::install.
    void install();

    // The backend's counts per frame over `frames` since the last call, in
    // words (draws, vertices, ring KB, the draws handed back and why, record
    // ms); resets them. Empty while LLRHI_IMMEDIATE_RING is off and nothing
    // came. On the VKPerf line as "immediate ring: ..." (LLVKGL::perfReport,
    // while the hook is installed; it holds sLock, which this takes again).
    std::string report(U32 frames);
}

#endif // LL_LLVKIMMEDIATE_H
