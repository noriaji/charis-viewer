/**
 * @file llvkdrawlist.h
 * @brief The Vulkan backend's side of the draw lists (research/137 N1):
 *        LLRHI::DrawList::sDrawList and sDrawListReady.
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

#ifndef LL_LLVKDRAWLIST_H
#define LL_LLVKDRAWLIST_H

#include "stdtypes.h"

#include <string>

namespace LLVKDrawList
{
    // Installs LLRHI::DrawList::sDrawList and sDrawListReady while direct
    // draws are on. Called from LLRHIVKMirror::install after LLVKGL::install.
    void install();

    // The backend's counts per frame over `frames` since the last call, in
    // words (lists, draws, the reference path's, draws dropped and why,
    // lists by length and by name, record ms); resets them. Empty while
    // LLRHI_DRAW_LISTS is 0 and nothing was recorded. For the VKPerf line.
    std::string report(U32 frames);
}

#endif // LL_LLVKDRAWLIST_H
