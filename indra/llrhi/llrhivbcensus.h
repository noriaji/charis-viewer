/**
 * @file llrhivbcensus.h
 * @brief Counts for the vertex buffer double-hold work (research/293 branch 0, LLRHI_VB_CENSUS=1).
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

#ifndef LL_LLRHIVBCENSUS_H
#define LL_LLRHIVBCENSUS_H

#include "stdtypes.h"

#include <atomic>

// Counting only: nothing here changes what is drawn, written or kept. Off by
// default; LLRHI_VB_CENSUS=1 turns it on for the run (decided once, by init()
// at the backend's install). While off, every call site is `if (sOn)` on one
// bool and nothing else runs. Counts are cumulative over a window that
// report() closes (about once a minute, with the memory report) into the
// "VBCensus" log lines: numbers, sizes and callers' function names only.
//
// Threads: the counters are relaxed atomics; the per-buffer records and the
// peak snapshot are under one mutex of their own (never held while calling
// out). Any thread may call.
namespace LLRHIVBCensus
{
    // True from init() on if LLRHI_VB_CENSUS is set (and the backend's numbers
    // are installed). Read without a lock; set once before the first buffer
    // the backend numbers.
    extern bool sOn;

    // A snapshot (high resolution shot, 360) is in progress: writes, allocations
    // and moves are counted apart. Set by display() (llviewerdisplay.cpp).
    // Written by the main thread (display()) every frame; the others only read it.
    extern std::atomic<bool> sSnapshot;

    // The backend gives the census the two ways into the GL buffer tracker
    // (the names it was told, by number: set a mark, clear it if it is still
    // that number's). Given at install, before init().
    void setOwnerHooks(void (*set_owner)(U32 gl_name, U32 number), void (*clear_owner)(U32 gl_name, U32 number));

    // Reads `LLRHI_VB_CENSUS`. Call once, after the safe start's environment is in.
    void init();

    // LLVertexBuffer::sTypeSize (bytes per vertex of each AttributeType bit),
    // for the desk calculations of family waste. Given once.
    void setTypeSizes(const U32* sizes, U32 count);

    // ---- Counters of the window ----
    enum Counter
    {
        // (e) allocations (LLVertexBuffer::allocateBuffer)
        C_ALLOCS,
        C_ALLOC_NO_INDEX,           // allocations without indices (UI, stars ...)
        C_ALLOC_FIRST,              // the buffer's first allocation
        C_ALLOC_VERTICES_ONLY,      // later: only the vertex side was made again, with indices kept (F7)
        C_ALLOC_INDICES_ONLY,       // later: only the index side was made again, with vertices kept
        C_ALLOC_BOTH,
        C_ALLOC_VERTICES_SHRUNK,    // vertex side only (indices kept), fewer vertices than before
        C_ALLOC_KEPT_INDEX_OVER,    // ... and a kept index names a vertex past the new count (research/293 2.2 item 14)
        C_ALLOC_KEPT_INDEX_UNKNOWN, // ... and the index copy cannot be trusted (the indices were written by setIndexData: not in mMappedIndexData)
        C_SWITCH32_PARTIAL,         // setIndexData(U32*, offset, count) switched the index type (F62)
        C_SWITCH32_FULL,            // setIndexData(U32*) switched the index type
        // (d) writes (LLVertexBuffer::flush_vbo)
        C_WRITES,
        C_WRITE_BYTES,
        C_WRITE_SETX_BYTES,         // of them, by setXData (the source is not the mapped data)
        C_WRITE_INDEX_BYTES,
        // (d) the attribute pool's writes (numbered_write)
        C_POOL_MOVED,               // written after a draw read the place: moved
        C_POOL_NO_RECORD,           // the write found no upload recording open
        C_POOL_STAGING_FAILED,      // ... or no room in the upload ring (the place was dropped)
        C_POOL_BUDGET,              // ... or the frame's copy budget was spent (the place was dropped)
        // (d) the same, during a snapshot
        C_SNAP_ALLOCS,
        C_SNAP_WRITES,
        C_SNAP_WRITE_BYTES,
        C_SNAP_MOVED,
        // (e) the pool's ranges
        C_RANGES_TAKEN,
        C_RANGES_FAILED,
        // (c) draws
        C_DRAWS,                    // drawNative calls of numbered buffers
        C_RUNS_OPENED,
        C_RUN_DRAWS,
        C_RUN_MASK_BREAKS,          // draws that joined a run with another type mask than the run's last draw
        C_SCENE_BUCKETS,            // buckets handed to a scene draw job
        // (a) refusals counted by draws (VB counts come from the records)
        C_REFUSED_DRAWS,
        C_FAN_INDEXED_DRAWS,        // fans and line loops with indices: drawn (the backend converts them from the CPU copy of the indices)
        COUNTER_COUNT
    };

    // (a) the reads by GL name of a buffer that has a number (the owner mark).
    enum Read
    {
        READ_GPU_BUFFER,            // gpu_buffer(Buffer&)
        READ_GPU_NAMED_CACHED,      // gpu_buffer_named answered from its per-frame cache (LLVKTracker::drawBuffer reads through it)
        READ_CONTENTS,              // LLVKTracker::bufferContents
        READ_VERSIONS,              // sObjectVersions
        READ_CHANGED,               // buffer_changed
        READ_FIND,                  // sBuffers.find for the CPU copy of the data, read (source_indices)
        READ_FIND_SIZE,             // sBuffers.find for the data's size or existence only (index_range_fits, the depth-only draw's held check): needs a CPU copy to exist
        READ_COUNT
    };

    // (f) why drawNative / the backend did not take a draw.
    enum Refusal
    {
        REFUSE_NO_PROGRAM,
        REFUSE_UNBUILT,
        REFUSE_SHAPE,
        REFUSE_UNIT,
        REFUSE_NOT_DIRECT,          // !LLRHI::sDirect
        REFUSE_NO_SHADER,
        REFUSE_NO_BUFFER,           // no GL name, or indexed without an index buffer
        REFUSE_COUNT
    };
    static_assert(REFUSE_COUNT <= 8, "Rec::refused holds one bit per reason");

    inline void count(Counter c, U64 n = 1);   // defined below

    // ---- Per-buffer records (by the buffer's number) ----
    struct Shape
    {
        U32 type_mask = 0;
        U32 vertices = 0;
        U32 indices = 0;
        U32 index_type = 0;         // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
        U32 vertex_bytes = 0;       // LLVertexBuffer::getSize
        U32 index_bytes = 0;        // LLVertexBuffer::getIndicesSize
        U32 vertex_name = 0;        // GL names
        U32 index_name = 0;
    };

    // The buffer was allocated (or its index type changed): its shape now.
    void layout(U32 number, const Shape& now);
    // The buffer is gone.
    void release(U32 number);
    // The buffer's indices were written by setIndexData: its mMappedIndexData is not a copy of them.
    void indexSetByData(U32 number);
    bool indexCopyUnknown(U32 number);
    // A draw of the buffer was asked for.
    void drawn(U32 number);
    // The draw was not taken.
    void refused(U32 number, Refusal why);
    // A draw joined a run (the run was opened by it when `opened`).
    void runDraw(U32 number, bool opened);

    // (a) A read by GL name of a buffer carrying the owner mark `number`.
    // Counts by kind, and samples the caller (1 in 64) by its function names.
    void nameRead(Read kind, U32 number);

    // The end of a window: logs the lines (rate limited to once in 55 s; a
    // call in between returns at once) and clears the window.
    void report();
}

// ---- Inline: the counters ----
namespace LLRHIVBCensus
{
    namespace detail
    {
        extern std::atomic<U64> sCounters[COUNTER_COUNT];
    }
    inline void count(Counter c, U64 n)
    {
        detail::sCounters[c].fetch_add(n, std::memory_order_relaxed);
    }
}

#endif // LL_LLRHIVBCENSUS_H
