/**
 * @file llvkwetlook.h
 * @brief Wet look (wet ground and puddles) recorded directly in Vulkan.
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

#ifndef LL_LLVKWETLOOK_H
#define LL_LLVKWETLOOK_H

// The passes of llrhiwetlook.h (research/120 section 6) as native fullscreen
// passes (llvkfullscreen) writing three or four colour attachments at once,
// on the GL framebuffers the viewer binds; they may test the avatar mark in
// the stencil.
namespace LLVKWetLook
{
    // Installs LLRHI::sWetLookCompute and sWetLookWriteBack (unless
    // LLRHI_WET_LOOK=0). The pipelines are made at the first pass.
    void install();
}

#endif // LL_LLVKWETLOOK_H
