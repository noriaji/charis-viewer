/**
 * @file llvkmotion.h
 * @brief The G-buffer's motion vectors (research/373): the camera's pass and
 *        the view, recorded directly in Vulkan.
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

#ifndef LL_LLVKMOTION_H
#define LL_LLVKMOTION_H

// The passes of llrhimotion.h as native fullscreen passes (llvkfullscreen) on
// the GL framebuffer the viewer binds.
namespace LLVKMotion
{
    // Installs LLRHI::sMotionClear, sMotionCamera and sMotionViewDraw (not with
    // LLRHI_TEMPORAL=0, nor on a device with fewer than five colour attachments
    // (the G-buffer with its motion vectors) or fragment outputs, or without
    // independentBlend).
    // The pipelines are made at the first pass (or ahead of it by --bake-shaders).
    void install();
}

#endif // LL_LLVKMOTION_H
