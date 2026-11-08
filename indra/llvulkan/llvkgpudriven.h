/**
 * @file llvkgpudriven.h
 * @brief GPU-driven drawing: vertex and index buffers pooled into large buffers (research/46).
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

#ifndef LL_LLVKGPUDRIVEN_H
#define LL_LLVKGPUDRIVEN_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <string>

// Stage G1 of research/46. Draws from many GL buffers cannot share one
// indirect draw call: each binds its own vertex and index buffer. The pool
// copies the positions and indices of a GL vertex/index buffer pair into one
// of a few large pool buffers, so draws from any number of pairs go out as
// one vkCmdDrawIndexedIndirect per pool buffer and index size.
//
// The copies follow GL: a write to a GL buffer or its deletion drops every
// copy made from it (LLVKTracker's buffer callback), and the pair is copied
// again the next time it is drawn. A copy is taken from the tracker's CPU copy
// of the buffers in the frame's upload commands, which run before the frame's
// draws; a pool range is reused only frames after its copy was dropped, when
// no frame in flight can still read it.
//
// Render thread only, under LLVKTracker::lock().
namespace LLVKGPUDriven
{
    // Pool buffers hold positions as vec3 (12 bytes; GL's LLVector4a without w),
    // then the indices as GL had them (16 or 32 bits).
    constexpr U32 POSITION_STRIDE = 12;
    constexpr U32 MAX_CHUNKS = 16;

    // The device can draw from the pool (multi-draw indirect with first instance).
    bool supported();

    // Where a vertex/index buffer pair is in the pool.
    struct Placement
    {
        U32 chunk = 0;              // pool buffer (chunkBuffer)
        S32 vertex_offset = 0;      // VkDrawIndexedIndirectCommand::vertexOffset for the pair's indices
        U32 first_index = 0;        // the pair's index 0, counted in its index size from the pool buffer's start
        U32 index_count = 0;        // indices copied
        bool index32 = false;       // GL_UNSIGNED_INT indices (else GL_UNSIGNED_SHORT)
    };

    // The pool placement of the pair: positions vertex_count LLVector4a from
    // position_offset in vertex_buffer, index_count indices of index_type from
    // the start of index_buffer (GL names). Copies the pair in when it is not
    // in the pool. False: draw it from the GL buffers instead (not supported,
    // the pool or this frame's copy budget is full, the data does not fit the
    // description, or GL keeps rewriting the buffers).
    bool place(U32 vertex_buffer, U32 index_buffer, U32 index_type, U32 position_offset, U32 vertex_count, U32 index_count,
               Placement& out);

    // place(), and the copy is kept for `pin` (it is not dropped for being idle)
    // until unpin(). When GL writes or deletes either buffer the copy goes
    // anyway, and the pin-dropped callback hears of each pin it had (the
    // callback must not call back into the pool).
    bool placePinned(U32 vertex_buffer, U32 index_buffer, U32 index_type, U32 position_offset, U32 vertex_count, U32 index_count,
                     U32 pin, Placement& out);
    void unpin(U32 vertex_buffer, U32 index_buffer, U32 pin);
    void setPinDroppedCallback(void (*callback)(U32 pin));

    // The pool buffer to bind as both vertex buffer (offset 0, stride
    // POSITION_STRIDE) and index buffer (offset 0) for a placement's chunk.
    VkBuffer chunkBuffer(U32 chunk);

    // Pool counters since the last call, for the periodic perf log (empty when
    // the pool was never used).
    std::string takeStats(U32 frames);
    // Device memory the pool holds and how much of it is in use, for the
    // periodic memory log (research/54). Empty when the pool was never used.
    std::string memoryReport();
}

#endif // LL_LLVKGPUDRIVEN_H
