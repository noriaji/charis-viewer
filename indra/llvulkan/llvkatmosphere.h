/**
 * @file llvkatmosphere.h
 * @brief Ground fog and fireflies recorded directly in Vulkan (research/148).
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

#ifndef LL_LLVKATMOSPHERE_H
#define LL_LLVKATMOSPHERE_H

// The passes of llrhiatmosphere.h as native fullscreen passes (llvkfullscreen)
// laid over the scene's HDR target the viewer binds.
namespace LLVKAtmosphere
{
    // Installs the LLRHI hooks (unless LLRHI_ATMOSPHERE=0). The pipelines are
    // made at the first pass.
    void install();
}

#endif // LL_LLVKATMOSPHERE_H
