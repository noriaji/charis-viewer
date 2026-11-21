/**
 * @file llrhiresource.h
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

#ifndef LL_LLRHIRESOURCE_H
#define LL_LLRHIRESOURCE_H

#include "stdtypes.h"

// The vertex buffers' numbers (research/197 stage 1b). Research/137 N14's resources
// by the backend's own handle (LLRHI_NATIVE_RESOURCES, off by default) went in
// research/391: stage 1b's numbers and LLRHI_TEX_HANDLES replaced it.
namespace LLRHI
{
    // --- Vertex buffer numbers (research/197 stage 1b) -----------------------
    // An LLVertexBuffer's number for its whole life (LLVertexBuffer::mRHIRef).
    // The backend keeps "number -> where the buffer's vertices and indices are
    // in the attribute pool now" (llvkattribpool.cpp): the place moves (the
    // buffer is allocated again with other sizes, a write lands while a draw
    // of this frame still reads the old place, the pool needs the room), the
    // number stays. Draws name their pair by the number (DirectDraw::vb) and
    // the backend takes the place from the number's entry: no table by GL
    // name, and a write goes straight into the place (only the ranges
    // written) instead of dropping the pair to copy it whole again. The GL
    // names stay beside it for what still draws by name (research/202 §4).
    // 0: none.
    struct BufferRef
    {
        U32 id = 0;
    };
    // What the buffer is now (LLVertexBuffer's GL names, sizes and index
    // type), as DirectDraw describes it. A write or a draw that finds the
    // number's place made for another layout drops the place (made again
    // at the next draw).
    struct BufferLayout
    {
        U32 vertex_buffer = 0;      // GL names
        U32 index_buffer = 0;
        U32 vertices = 0;           // LLVertexBuffer::getNumVerts / getNumIndices
        U32 indices = 0;
        U32 index_type = 0;         // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
    };
    struct Buffers
    {
        // A new number (main thread; the backend locks). {0} without a
        // backend, with the switch off, or when every number is taken: the
        // buffer then goes by its GL names.
        static BufferRef create();
        // The buffer was allocated again (or its index type changed): the
        // number's place goes, if it was made for another layout.
        static void layout(BufferRef ref, const BufferLayout& now);
        // Before the same bytes go to the GL name (sBufferSubData): `size` bytes of the vertex buffer (or the
        // index buffer) at `offset`, from src. Only while sVBNative.
        static void write(BufferRef ref, const BufferLayout& now, bool indices, U32 offset, U32 size, const void* src);
        // The buffer is gone. Its place goes back to the pool after the
        // frames that may still read it; the number is given out again later.
        static void release(BufferRef ref);

        // Set by the Vulkan backend (all null: no numbers).
        struct Backend
        {
            BufferRef (*create)() = nullptr;
            void (*layout)(BufferRef ref, const BufferLayout& now) = nullptr;
            void (*write)(BufferRef ref, const BufferLayout& now, bool indices, U32 offset, U32 size, const void* src) = nullptr;
            void (*release)(BufferRef ref) = nullptr;
        };
        static void install(const Backend& backend);
    };
    // LLRHI_VB_HANDLES: on (unset, the default since research/218 vk-s7f);
    // 0 off (draws name their buffers by GL name, as before); 1 on; verify;
    // auto (verify first, then on once 600 frames of the world compared with
    // no mismatch, off for the run at the first: the default before). Verify: draws still go by name; a sample of the numbers
    // (1 in 16) keeps a place too, with a CPU copy of what went into it, and
    // that copy is compared with GL's (the backend's copy of the GL buffer)
    // now and then: a write that missed the number shows up there.
    // vbhandles.toggle / vbhandlesverify.toggle flip them (a cue ends auto).
    extern bool sVBHandles;
    extern bool sVBHandlesVerify;
    extern bool sVBHandlesAuto;
    // Set by the backend once a frame: it keeps places by number now (on or
    // verify). While false the viewer sends no numbers and no writes (the
    // backend drops every place by number when it turns false, so nothing
    // written meanwhile is missed).
    extern bool sVBNative;
    // ---- end of the vertex buffer numbers ----
}

#endif // LL_LLRHIRESOURCE_H
