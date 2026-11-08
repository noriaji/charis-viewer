/**
 * @file llvkgtao.h
 * @brief GTAO and contact shadows recorded directly in Vulkan.
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

#ifndef LL_LLVKGTAO_H
#define LL_LLVKGTAO_H

// The five passes of llrhigtao.h (research/92) as native fullscreen passes
// (llvkfullscreen) on the GL framebuffers the viewer binds.
namespace LLVKGtao
{
    // Installs the LLRHI::sGtao* and sContactShadows entry points (unless
    // LLRHI_GTAO=0). The pipelines are made at the first pass.
    void install();
}

#endif // LL_LLVKGTAO_H
