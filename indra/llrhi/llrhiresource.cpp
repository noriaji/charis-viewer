/**
 * @file llrhiresource.cpp
 * @brief The vertex buffers' numbers (research/197 stage 1b): the switches and the hooks.
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

#include "linden_common.h"

#include "llrhiresource.h"

#include "llrhi.h"

// research/197 stage 1b: LLRHI_VB_HANDLES on (unset, the default since
// research/218 vk-s7f; was auto) / 0 / 1 / verify / auto (verify first, as before).
namespace
{
    LLRHI::Buffers::Backend sBuffersBackend;
}
bool LLRHI::sVBHandles = LLRHI::envUnset("LLRHI_VB_HANDLES") || LLRHI::envIs("LLRHI_VB_HANDLES", "1") || LLRHI::envIs("LLRHI_VB_HANDLES", "on") ||
                         LLRHI::envIs("LLRHI_VB_HANDLES", "verify") || LLRHI::envIs("LLRHI_VB_HANDLES", "auto");
bool LLRHI::sVBHandlesVerify = LLRHI::envIs("LLRHI_VB_HANDLES", "verify") || LLRHI::envIs("LLRHI_VB_HANDLES", "auto");
bool LLRHI::sVBHandlesAuto = LLRHI::envIs("LLRHI_VB_HANDLES", "auto");
bool LLRHI::sVBNative = false;

LLRHI::BufferRef LLRHI::Buffers::create()
{
    return sBuffersBackend.create && sVBHandles ? sBuffersBackend.create() : BufferRef();
}

void LLRHI::Buffers::layout(BufferRef ref, const BufferLayout& now)
{
    if (ref.id && sBuffersBackend.layout)
    {
        sBuffersBackend.layout(ref, now);
    }
}

void LLRHI::Buffers::write(BufferRef ref, const BufferLayout& now, bool indices, U32 offset, U32 size, const void* src)
{
    if (ref.id && size && src && sBuffersBackend.write)
    {
        sBuffersBackend.write(ref, now, indices, offset, size, src);
    }
}

void LLRHI::Buffers::release(BufferRef ref)
{
    if (ref.id && sBuffersBackend.release)
    {
        sBuffersBackend.release(ref);
    }
}

void LLRHI::Buffers::install(const Backend& backend)
{
    sBuffersBackend = backend;
}
