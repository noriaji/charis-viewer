/**
 * @file llvkresource.h
 * @brief The Vulkan backend's side of resources by handle (research/137 N14).
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

#ifndef LL_LLVKRESOURCE_H
#define LL_LLVKRESOURCE_H

#include "stdtypes.h"

#include <string>

namespace LLVKResource
{
    // Installs LLRHI::sBufferHandle and LLRHI::sBufferWriteRanges
    // (llrhiresource.h) with LLRHI_NATIVE_RESOURCES=1, only while the
    // native buffer data hooks are in. Logs one line when it was asked for.
    // Called from LLRHIVKMirror::install.
    void install();

    // Whether install put the hooks in (the immediate ring then draws from
    // its Vulkan buffer by itself, llvkimmediate.cpp).
    bool installed();

    // The resource counts per frame over `frames` since the last call, in
    // words: buffer writes (calls, ranges, the calls by handle, KB, and with
    // LLRHI_VK_PERF their ms), texture data (images, new images, sub-images,
    // mip chains, ms); resets them. Under the backend's lock (LLVKGL::perfReport).
    std::string report(U32 frames);
}

#endif // LL_LLVKRESOURCE_H
