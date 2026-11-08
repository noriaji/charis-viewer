/**
 * @file llvkavatar.h
 * @brief Stage 4b (research/199): the resident avatar palette rows and the
 * compute skinning check, the backend's side of llrhiavatar.h.
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

#ifndef LL_LLVKAVATAR_H
#define LL_LLVKAVATAR_H

namespace LLVKAvatar
{
    // Registers the compute kernel's bake step, and installs the backend
    // (LLRHIAvatar::setBackend) when LLRHI_AVATAR_PALETTE or LLRHI_COMPUTE_SKIN
    // is on. Main thread, once,
    // with the other native hooks (llvkgl::install_native_hooks).
    void install();
}

#endif // LL_LLVKAVATAR_H
