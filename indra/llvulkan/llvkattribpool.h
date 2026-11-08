/**
 * @file llvkattribpool.h
 * @brief GPU-driven drawing, stage G4b: every vertex attribute and the indices of
 * GL vertex/index buffer pairs pooled into large buffers (research/46).
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

#ifndef LL_LLVKATTRIBPOOL_H
#define LL_LLVKATTRIBPOOL_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <string>

namespace LLRHI
{
    struct DirectDraw;
}

// The G-buffer draws of a pass differ only in their vertex and index buffers
// and their push constants (research/46 G4a). One indirect draw can replace a
// run of them only if they all read one set of vertex buffers and one index
// buffer. G1's pool holds positions only (shadows); this one holds every
// attribute a draw reads.
//
// A pool chunk is a vertex range shared by streams, one buffer per stream key
// and stride (LLVertexBuffer arrays are planar: each attribute is an array of
// its own; the key is the attribute's location, or with LLRHI::sVBSourceStreams
// the array it reads, DirectDraw::Attribute::streamKey), plus a 32-bit index
// buffer. A pair gets the same vertex range in every stream, so one vertexOffset
// serves all of its attributes and the stream buffers are bound at the same
// offset for every pair of the chunk (the attribute's streamShift: 0, but 12
// for TEXTURE_INDEX, which reads the positions' w). Bytes
// are copied as GL has them (the same stride), so the pipeline's vertex input
// is the one the draw would use. 16-bit indices become 32-bit (same values;
// pipelines never enable primitive restart).
//
// The copies follow GL exactly as G1's do: a write to either GL buffer, or its
// deletion, drops the pair (LLVKTracker's buffer callbacks); it is copied again
// the next time it is drawn. Copies come from the tracker's CPU copy, through
// the frame's upload commands, which run before the frame's draws. A dropped
// range is reused only when no frame in flight can still read it.
//
// Render thread only, under LLVKTracker::lock().
namespace LLVKAttribPool
{
    // The device has multi-draw indirect with a first instance and a count
    // buffer (vkCmdDrawIndexedIndirectCount).
    bool supported();
    // vkCmdDrawIndexedIndirectCount (null when not supported).
    PFN_vkCmdDrawIndexedIndirectCount drawIndexedIndirectCount();

    struct Placement
    {
        U32 chunk = 0;
        S32 vertex_offset = 0;  // VkDrawIndexedIndirectCommand::vertexOffset for the pair's indices
        U32 first_index = 0;    // the pair's index 0 in the chunk's index buffer (32-bit indices)
    };

    // The pool copy of the draw's vertex buffer (each attribute it lists) and
    // index buffer, copying what is missing. False: draw it from the GL buffers
    // (not supported, unknown buffer sizes, not an indexed draw, the data does
    // not fit the description, the pool or this frame's copy budget is full,
    // or GL keeps rewriting the buffers).
    bool place(const LLRHI::DirectDraw& draw, Placement& out);

    // A placed pair as place() handed it out, for callers that keep it (the
    // draw templates of llvkgl, research/46 G4c). Valid while the pair stays
    // placed as it was: any drop or new copy of it makes redeem() fail.
    struct Ticket
    {
        U32 vertex_buffer = 0;      // GL name the pair is keyed by
        U64 serial = 0;             // the placement's serial number
        U32 epoch = U32_MAX;        // the pool's epoch when entry was looked up
        void* entry = nullptr;      // the pool's record, valid while the epoch stays
        Placement placement;
        // research/197 stage 1b: the vertex buffer number the placement was
        // found by (DirectDraw::vb), 0: by its GL names. A numbered ticket is
        // redeemed from the number's entry (no lookup by name).
        U32 vb = 0;
    };
    // place() that also hands out the pair's ticket.
    bool place(const LLRHI::DirectDraw& draw, Placement& out, Ticket& ticket);
    // The ticket's placement, if the pair is still placed as when the ticket
    // was handed out (also marks the pair drawn this frame, as place() does).
    // Costs a lookup only when some entry was erased since the ticket's last
    // use (with LLRHI_POOL_LAZY_FORGET=0: also when some pair was dropped).
    bool redeem(Ticket& ticket, Placement& out);

    // The buffers to bind for a chunk: an attribute's stream (its streamKey()
    // and stride) at its streamShift() as the binding offset; the indices at
    // offset 0, 32-bit. A caller that asks for a fixed location (llvkscene's
    // shadow buckets, the ray tracing positions) asks for one whose attribute
    // has no source (key = location).
    VkBuffer streamBuffer(U32 chunk, U32 key, U32 stride);
    VkBuffer indexBuffer(U32 chunk);
    // Research/50 step 4 (device generated commands, which bind buffers by
    // address): the chunks made so far, and the device address and size of a
    // chunk's stream or index buffer. False: none (or no generated commands).
    U32 chunkCount();
    bool streamAddress(U32 chunk, U32 key, U32 stride, VkDeviceAddress& address, VkDeviceSize& size);
    bool indexAddress(U32 chunk, VkDeviceAddress& address, VkDeviceSize& size);

    // Research/46 G5: a list that draws a placed pair later without placing it
    // again pins it, so it is not dropped for being idle. Returns the pin (the
    // pair's placement number; 0 when the vertex buffer has no placed pair).
    // GL writes and deletions still drop a pinned pair: then the drop callback
    // gets its vertex buffer, and the pin is gone with it (unpin ignores it).
    U32 pin(U32 vertex_buffer);
    void unpin(U32 vertex_buffer, U32 pin);
    // Called (under LLVKTracker::lock()) with the vertex buffer of every placed
    // pair that is dropped, before its ranges can be reused.
    void setDropCallback(void (*callback)(U32 vertex_buffer));

    // Pool counters since the last call, for the perf report (empty when the
    // pool was never used).
    std::string takeStats(U32 frames);
    // research/218 vk-s7f (research/207 7.1): the old paths since the last
    // call, totals (places and redeems by the GL name); starts them over. Under LLVKTracker::lock().
    std::string takeOldPathStats();

    // Research/197 stage 1b: vertex buffer numbers (LLRHI::Buffers,
    // llrhiresource.h). A number's place is a pair of this pool, kept by the
    // number instead of by the GL names: made at the number's first draw
    // (copied from the tracker's copy, as a pair by name is), then written
    // range by range as LLVertexBuffer writes the buffer (in place, or in a
    // new place when a draw of this frame still reads the old one), and
    // dropped when the buffer is allocated again with another layout, goes,
    // or the pool cannot take a write now (made again at its next draw).
    // place() takes a draw with a number (DirectDraw::vb) that way while the
    // switch (LLRHI_VB_HANDLES) is on; verify keeps places for a sample of
    // the numbers beside the pairs by name and compares a CPU copy of what
    // went in with the tracker's copy. Installs LLRHI::Buffers' backend.
    void installNumbers();
    // ", vertex buffer numbers [...]" for the perf report (empty while never on).
    std::string numbersReport(U32 frames);

    // Research/198 5.5: the retained scene draws from numbers' places
    // without a draw by number going through place() (llvkscene.cpp).
    // A number's place as it is now. copied: a bit per attribute location
    // (below 32) a draw can read from the place, through the attribute's
    // streamKey() and streamShift(). A bit L does not mean a stream (L, stride)
    // exists: with LLRHI::sVBSourceStreams TEXTURE_INDEX's bit is served by the
    // positions' stream. Ask the stream of a fixed location only for a location
    // whose attributes have no source. False: no place (none made
    // yet, gone, or not copyable).
    struct NumberPlace
    {
        U32 chunk = 0;
        U32 vertex_base = 0;
        U32 index_base = 0;
        U32 vertices = 0;
        U32 indices = 0;
        U32 copied = 0;
        // research/230 R2: what a copy of the place's data (an acceleration
        // structure) is good for: the placement and the writes into it.
        U64 serial = 0;
        U32 writes = 0;
    };
    bool numberPlace(U32 number, NumberPlace& out);
    // Draws take places by number this frame (numberPlace hands places out):
    // when it turns on, places made by verify's sample are handed out too,
    // without a word to the listener (the scene asks again).
    bool numbersTaken();
    // Told (under LLVKTracker::lock(), from inside the pool) whenever the
    // place of a number with interest is made, moved, given another
    // attribute's array, or goes. The listener only reads (numberPlace) and
    // notes; it never calls back into the pool's other entries.
    void setNumberListener(void (*listener)(U32 number));
    // Interest in a number, counted (on adds one, off takes one away).
    void numberInterest(U32 number, bool on);
    // research/230 R2: moves whenever data is written into a place by number.
    U64 numberWriteSerial();
    // While the scene draws: the places of numbers with interest are not
    // dropped for being idle (the scene's draws do not go through place()),
    // and after sceneDrew in a frame a write to such a place moves it first,
    // as after a draw by number (the frame's earlier draws read the old one).
    void sceneDrawing(bool on);
    void sceneDrew();
    // Device memory the pool holds and how much of it is in use, for the
    // periodic memory log (research/54). Empty when the pool was never used.
    std::string memoryReport();
}

#endif // LL_LLVKATTRIBPOOL_H
