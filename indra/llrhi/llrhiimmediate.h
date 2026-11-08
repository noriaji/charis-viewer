/**
 * @file llrhiimmediate.h
 * @brief The immediate mode's fast path (research/137 N9): an LLRender flush's
 *        vertices straight into the frame's ring, drawn once.
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

#ifndef LL_LLRHIIMMEDIATE_H
#define LL_LLRHIIMMEDIATE_H

#include <atomic>
#include "stdtypes.h"
#include "llrhi.h"

// Research/137 N9 (research/95 U1 (b)). In Vulkan mode LLRender::flush hands
// the vertices gGL.begin/vertex/end gathered to the backend as values: the
// backend copies them into the frame's upload ring and draws them from there,
// as one direct draw. Gone from the old way (bufferfromCache, genBuffer,
// drawBuffer): the vertex hash, the VB cache, a new LLVertexBuffer on every
// cache miss, its GL buffer name, and on the backend's side the buffer's CPU
// copy, its Vulkan buffer (VMA) and the staging copy into it.
//
// Everything else is what sDirectDraw takes from the current state, as for the
// old way's draw: the program, the render state (blend, depth, culling,
// scissor, viewport), the target, the texture units (LLTexUnit::bind set them
// through the hooks and flushed first) and the matrices (LLRender::syncMatrices
// runs before). So the same draw goes out in the same order with the same
// values and the same pipeline; only where its vertices come from changes.
// The state and the matrices are not values of the draw on purpose: the
// caller already set them through the hooks, and a copy here would leave the
// viewer's caches (LLRender, LLTexUnit, LLGLSLShader::mMatHash) behind
// (research/137 2.3 promise 4).
namespace LLRHI
{
    struct ImmediateDraw
    {
        // `bytes` from `data`, copied to `offset` of the draw's vertex block.
        struct Piece
        {
            const void* data = nullptr;
            U32 bytes = 0;
            U32 offset = 0;
        };
        static constexpr U32 MAX_PIECES = 4;

        U32 mode = 0x0004;          // GL primitive mode (GL_TRIANGLES)
        U32 vertex_count = 0;       // vertices 0..vertex_count-1 of the block
        U32 bytes = 0;              // the vertex block's size
        Piece pieces[MAX_PIECES];
        U32 piece_count = 0;
        // As DirectDraw's, their offsets into the vertex block.
        DirectDraw::Attribute attributes[DirectDraw::MAX_ATTRIBUTES];
        U32 attribute_count = 0;
    };

    // LLRHI_IMMEDIATE_RING=1 turns the fast path on (off by default).
    // research/137 N7 adds the cue immediatering.toggle, which flips it; it is
    // read at every flush.
    extern bool sImmediateRing;

    // Set by the Vulkan backend. Records the draw with the current state;
    // false: nothing was recorded (no frame being recorded, a probe list
    // capture, a draw sDirectDraw would refuse, no room in the ring) and the
    // caller draws it the old way. Values only: the pieces may be reused once
    // it returns. Main thread.
    extern bool (*sImmediateDraw)(const ImmediateDraw& draw);

    // research/155 T0b: LLRender's VB cache misses (a vertex buffer made for
    // immediate mode vertices), always counted; the VKPerf line's counters
    // read it and start it over (LLVKGL::counterReport).
    extern std::atomic<U32> sVBCacheMisses;

    // Whether LLRender::flush takes the fast path: the switch, the backend's
    // hook, and the direct draws the old way's draw would go as
    // (LLVertexBuffer::drawNative's conditions).
    inline bool immediateRing()
    {
        return sImmediateRing && sImmediateDraw && sGLDrawsDisabled && sDirect && sDirectDraw;
    }
}

#endif // LL_LLRHIIMMEDIATE_H
