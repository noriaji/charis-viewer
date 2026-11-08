/**
 * @file llvkimmediate.cpp
 * @brief The Vulkan backend's side of the immediate mode's fast path (research/137 N9).
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

#include "llvkimmediate.h"

#include "llrhiimmediate.h"
#include "llvkresource.h"
// Before the #if: llvkgl_internal.h brings llglheaders.h, which defines
// LL_GL_FUNC_POINTER (as in the llvkgl*.cpp files).
#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"

#if LL_GL_FUNC_POINTER

// LLRHI::sImmediateDraw: the draw's vertex block is copied into the frame's
// upload ring (LLVKMirror::allocate, as the uniform copies and the constant
// attributes are) and drawn from there by direct_draw, the way sDirectDraw
// draws, so everything but the vertex buffer goes as the old way's draw went:
// the program, the state, the target, the units, the matrices, the pass
// cache, the pipeline (the same vertex formats and strides), OIT, the fan and
// line loop conversions, the counters.
//
// direct_draw finds a draw's vertex buffer by its GL name (direct_inputs:
// gpu_buffer_named). The ring block has none, so the draw names the stream
// name below, and right before the draw (under the lock) that name's entry in
// the frame's resolved buffers (sResolvedBuffers) is made to say the ring.
// With LLRHI_NATIVE_RESOURCES (research/137 N14, LLVKResource::installed) the
// draw brings the ring's Vulkan buffer instead (tDirectVertexBuffer, which
// direct_inputs takes) and names no buffer; the stream name and the entry go
// once that way is the only one.
namespace
{
    using namespace llvkgl;

    // No GL buffer has this name: NameAllocator (llvkgl_context.cpp) hands
    // buffer names out from 1 up, smallest free first, and the buffer table
    // (sBuffers) has no entry for it. An entry of sResolvedBuffers that a
    // buffer of the same slot takes in between only costs that buffer a
    // lookup; the stream's entry is made again before each of its draws.
    constexpr GLuint STREAM_NAME = 0xfffffff0u;

    // Why a draw went back to the caller (who draws it the old way).
    enum Refusal
    {
        REFUSED_NO_FRAME,       // no frame being recorded
        REFUSED_SPECIAL,        // a probe list capture, an occlusion query, the depth-only pass: they read the vertex buffer by name
        REFUSED_SHAPE,          // the draw's own values do not hold together
        REFUSED_DIRECT,         // direct_draw would refuse it (no program, unbuilt, a descriptor unit)
        REFUSED_RING,           // no room in the ring this frame
        REFUSAL_COUNT
    };

    struct Counts
    {
        U32 draws = 0;
        U64 vertices = 0;
        U64 bytes = 0;
        U32 refused[REFUSAL_COUNT] = {};
        U64 ticks = 0;              // recording, __rdtsc
    };
    Counts sCounts;

    bool valid(const LLRHI::ImmediateDraw& d)
    {
        if (!d.vertex_count || !d.bytes || d.piece_count > LLRHI::ImmediateDraw::MAX_PIECES || !d.attribute_count ||
            d.attribute_count > LLRHI::DirectDraw::MAX_ATTRIBUTES)
        {
            return false;
        }
        for (U32 i = 0; i < d.piece_count; ++i)
        {
            const LLRHI::ImmediateDraw::Piece& p = d.pieces[i];
            if (!p.data || (U64)p.offset + p.bytes > d.bytes)
            {
                return false;
            }
        }
        for (U32 i = 0; i < d.attribute_count; ++i)
        {
            if (d.attributes[i].offset >= d.bytes)
            {
                return false;
            }
        }
        return true;
    }

    bool record(const LLRHI::ImmediateDraw& in)
    {
        const U64 start = __rdtsc();
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            ++sCounts.refused[REFUSED_NO_FRAME];
            return false;
        }
        if (sCapture.on || sActiveQuery || depth_only_pass())
        {
            ++sCounts.refused[REFUSED_SPECIAL];
            return false;
        }
        if (!valid(in))
        {
            ++sCounts.refused[REFUSED_SHAPE];
            return false;
        }
        LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        memcpy(attributes, in.attributes, sizeof(attributes[0]) * in.attribute_count);
        LLRHI::DirectDraw d;
        d.vertex_buffer = STREAM_NAME;
        d.mode = in.mode;
        d.first_vertex = 0;
        d.vertex_count = in.vertex_count;
        d.attributes = attributes;
        d.attribute_count = in.attribute_count;
        // Asked before the ring is touched: what direct_draw would refuse goes
        // back whole (the old way's direct draw is refused the same, and
        // counted there).
        if (direct_refusal(current_program(), d) != DIRECT_REFUSAL_COUNT)
        {
            ++sCounts.refused[REFUSED_DIRECT];
            return false;
        }
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void* dst = nullptr;
        // 16: the block's planar parts start at multiples of 16 (LLVertexBuffer::calcOffsets).
        if (!mirror.allocate(in.bytes, 16, ring, offset, dst) || offset + in.bytes > (VkDeviceSize)U32_MAX)
        {
            ++sCounts.refused[REFUSED_RING];
            return false;
        }
        U8* block = (U8*)dst;
        for (U32 i = 0; i < in.piece_count; ++i)
        {
            const LLRHI::ImmediateDraw::Piece& p = in.pieces[i];
            memcpy(block + p.offset, p.data, p.bytes);
        }
        for (U32 i = 0; i < in.attribute_count; ++i)
        {
            attributes[i].offset += (U32)offset;
        }
        bool drawn = false;
        if (LLVKResource::installed())
        {
            // research/137 N14: the draw brings the ring's Vulkan buffer itself
            // (direct_inputs takes it); no GL name, no entry of the name table's
            // frame cache is borrowed.
            d.vertex_buffer = 0;
            struct RingScope
            {
                explicit RingScope(VkBuffer buffer) { tDirectVertexBuffer = buffer; }
                ~RingScope() { tDirectVertexBuffer = VK_NULL_HANDLE; }
            } ring_scope(ring);
            drawn = direct_draw(d);         // takes sLock again (a recursive mutex)
        }
        else
        {
            ResolvedBuffer& entry = sResolvedBuffers[STREAM_NAME & (RESOLVED_BUFFERS - 1)];
            entry.name = STREAM_NAME;
            entry.frame = sFrame;
            entry.gpu = ring;
            drawn = direct_draw(d);         // takes sLock again (a recursive mutex)
        }
        llassert(drawn);                    // what it refuses was asked above, under the same lock
        ++sCounts.draws;
        sCounts.vertices += in.vertex_count;
        sCounts.bytes += in.bytes;
        sCounts.ticks += __rdtsc() - start;
        return drawn;
    }
}

void LLVKImmediate::install()
{
    if (!LLRHI::sDirect)
    {
        LL_INFOS("VKImmediate") << "immediate mode ring off: direct draws are off (LLRHI_DIRECT=0)" << LL_ENDL;
        return;
    }
    LLRHI::sImmediateDraw = &record;
    LL_INFOS("VKImmediate") << "immediate mode ring available: LLRHI_IMMEDIATE_RING=" << (LLRHI::sImmediateRing ? 1 : 0) << LL_ENDL;
}

std::string LLVKImmediate::report(U32 frames)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    frames = llmax(frames, 1u);
    const F64 per = 1.0 / frames;
    const Counts c = sCounts;
    sCounts = Counts();
    U32 refused = 0;
    for (U32 count : c.refused)
    {
        refused += count;
    }
    if (!LLRHI::sImmediateRing && !c.draws && !refused)
    {
        return std::string();   // off, and nothing came (flush asks only while it is on)
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "draws " << c.draws * per << ", vertices " << c.vertices * per << ", ring " << c.bytes * per / 1024.0
        << " KB; handed back: no frame " << c.refused[REFUSED_NO_FRAME] * per << ", capture/query/depth-only "
        << c.refused[REFUSED_SPECIAL] * per << ", shape " << c.refused[REFUSED_SHAPE] * per << ", direct refusal "
        << c.refused[REFUSED_DIRECT] * per << ", ring full " << c.refused[REFUSED_RING] * per << "; record "
        << c.ticks / (perf_ticks_per_ns() * 1e6) * per << " ms";
    return out.str();
}

#else // !LL_GL_FUNC_POINTER

void LLVKImmediate::install()
{
}

std::string LLVKImmediate::report(U32)
{
    return std::string();
}

#endif // LL_GL_FUNC_POINTER
