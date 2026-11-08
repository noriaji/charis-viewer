/**
 * @file llvkvolumetric.h
 * @brief Volumetric light (sun shafts) recorded directly in Vulkan.
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

#ifndef LL_LLVKVOLUMETRIC_H
#define LL_LLVKVOLUMETRIC_H

// The two passes of llrhivolumetric.h (research/84) as native passes on the
// GL framebuffers the viewer binds (LLVKTracker::beginPass): one fullscreen
// triangle each (llvkfullscreen), with their own shaders and uniforms.
namespace LLVKVolumetric
{
    // Installs LLRHI::sVolumetricMarch and sVolumetricComposite (unless
    // LLRHI_VOLUMETRIC=0). The pipelines are made at the first pass.
    void install();
}

#endif // LL_LLVKVOLUMETRIC_H
