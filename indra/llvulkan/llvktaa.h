/**
 * @file llvktaa.h
 * @brief Temporal anti-aliasing (research/185) recorded directly in Vulkan.
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

#ifndef LL_LLVKTAA_H
#define LL_LLVKTAA_H

// The pass of llrhitaa.h as a native fullscreen pass (llvkfullscreen) on the
// GL framebuffer the viewer binds.
namespace LLVKTaa
{
    // Installs LLRHI::sTaaDraw (unless LLRHI_TAA=0). The pipeline is made at
    // the first pass (or ahead of it by --bake-shaders).
    void install();
}

#endif // LL_LLVKTAA_H
