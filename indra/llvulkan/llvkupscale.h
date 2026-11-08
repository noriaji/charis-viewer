/**
 * @file llvkupscale.h
 * @brief Upscaling of the main view (research/165): AMD FidelityFX Super
 *        Resolution 1 (EASU and RCAS) recorded directly in Vulkan.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLVKUPSCALE_H
#define LL_LLVKUPSCALE_H

// The passes of llrhiupscale.h as native fullscreen passes (llvkfullscreen)
// on the GL framebuffer the viewer binds. The FSR 1 parts are a GLSL port of
// AMD's ffx_fsr1.h (MIT, see ORIGIN.md).
namespace LLVKUpscale
{
    // Installs LLRHI::sUpscaleDraw (unless LLRHI_UPSCALE=0). The pipelines
    // are made at the first pass (or ahead of it by --bake-shaders).
    void install();
}

#endif // LL_LLVKUPSCALE_H
