/**
 * @file llrhireplaycapture.cpp
 * @brief research/271 P1b stage 2, first entry: LLRHI::sDirectDraw.
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

#include "llrhireplaycapture.h"

#include "fsyspath.h"
#include "llrhi.h"
#include "llrhireplayformat.h"
#include "llstring.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace
{
    constexpr U32 ENTRY_DIRECT_DRAW = 1;   // research/271: the only entry id used so far

    std::mutex sMutex;                                    // guards sWriter, sFramesLeft and sStarted only
    std::unique_ptr<LLRHIReplay::Writer> sWriter;          // null: not capturing (or done)
    std::atomic<bool> sActive{ false };                    // checked on every sDirectDraw call
    std::atomic<U64> sSequence{ 0 };
    U32 sFramesLeft = 0;
    bool sStarted = false;                                 // a capture was started (it may be over): under sMutex
    // Why a drawing thread stopped the capture (STOP_NONE: it did not). The file is closed and
    // the reason logged at the next frame's end, not on the drawing thread (review replay-p0
    // low: closing logs, which may run out of memory and throw into the draw's caller).
    enum Stopping : U32 { STOP_NONE, STOP_COPY_OUT_OF_MEMORY, STOP_REFUSED };
    std::atomic<U32> sStopping{ STOP_NONE };

    // On a drawing thread: no more copies from now on, the first reason kept for the frame's end.
    void stop_from_draw(Stopping why)
    {
        sActive.store(false, std::memory_order_relaxed);
        U32 none = STOP_NONE;
        sStopping.compare_exchange_strong(none, why, std::memory_order_relaxed);
    }

    U64 thread_hash()
    {
        return (U64)std::hash<std::thread::id>()(std::this_thread::get_id());
    }

    void append_u32(std::vector<U8>& out, U32 value)
    {
        const U8* p = reinterpret_cast<const U8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    void append_floats(std::vector<U8>& out, const F32* values, U32 count)
    {
        const U8* p = reinterpret_cast<const U8*>(values);
        out.insert(out.end(), p, p + sizeof(F32) * count);
    }

    // research/271 3.2-3: the DirectDraw struct's fields copied by value (the scalars grouped
    // first, then the two array fields, then the five matrices: not llrhi.h's declaration order,
    // chosen instead so every array's own count comes immediately before it). A pointer field's
    // array is copied by its contents, not the pointer; a matrix's "present" flag is a U32
    // (0/1) so every field in a record is the same width, which makes a hex dump easy to read
    // while this is still being proved out; a denser form can come once more entries need the
    // same treatment (the generator's per-type table, research/271 3.1).
    //
    // Not carried over (research/271 3.3, left for when this is replayed, not just recorded):
    //  - vertex_buffer, index_buffer and textures[] are GL names (or research/197 1a draw
    //    handles): meaningless outside the capturing process without an identity table.
    //  - draw_template is a pointer the backend writes an id through after this call returns
    //    (llrhi.h's LLRHI::DirectDraw::draw_template, research/271 3.3): only whether it was
    //    null is recorded, not a value (there is nothing valid to read from it yet: the backend
    //    has not written to it at the time sDirectDraw is called, llvkgl_runs.cpp).
    void serialize_direct_draw(const LLRHI::DirectDraw& draw, std::vector<U8>& out)
    {
        append_u32(out, draw.vertex_buffer);
        append_u32(out, draw.index_buffer);
        append_u32(out, draw.index_type);
        append_u32(out, draw.first_index);
        append_u32(out, draw.index_count);
        append_u32(out, draw.first_vertex);
        append_u32(out, draw.vertex_count);
        append_u32(out, draw.mode);
        append_u32(out, draw.matrix_id);
        append_u32(out, draw.instance_count);
        append_u32(out, draw.table_base);
        append_u32(out, draw.buffer_vertices);
        append_u32(out, draw.buffer_indices);
        append_u32(out, draw.vb);
        append_u32(out, draw.draw_template != nullptr ? 1u : 0u);

        // Clamped the way every other reader of DirectDraw is (llvkattribpool.cpp,
        // llvkgl_scene.cpp, llvkimmediate.cpp): attribute_count (or texture_count, which has no
        // named limit but is never more than a handful of texture units in practice) coming out
        // of range would otherwise read past whatever fixed-size array the caller built the
        // draw from.
        const U32 attribute_count = llmin(draw.attribute_count, LLRHI::DirectDraw::MAX_ATTRIBUTES);
        append_u32(out, attribute_count);
        for (U32 i = 0; i < attribute_count && draw.attributes; ++i)
        {
            const LLRHI::DirectDraw::Attribute& a = draw.attributes[i];
            append_u32(out, a.location);
            append_u32(out, a.size);
            append_u32(out, a.type);
            append_u32(out, a.normalized ? 1u : 0u);
            append_u32(out, a.integer ? 1u : 0u);
            append_u32(out, a.stride);
            append_u32(out, a.offset);
        }

        constexpr U32 MAX_TEXTURES = 32;   // no named limit in llrhi.h; a generous bound
        const U32 texture_count = llmin(draw.texture_count, MAX_TEXTURES);
        append_u32(out, texture_count);
        for (U32 i = 0; i < texture_count && draw.textures; ++i)
        {
            append_u32(out, draw.textures[i]);
        }

        const struct { const F32* values; U32 count; } matrices[] = {
            { draw.modelview, 16 }, { draw.modelview_projection, 16 }, { draw.normal, 9 },
            { draw.inverse_modelview, 16 }, { draw.texture_matrix0, 16 },
        };
        for (const auto& m : matrices)
        {
            append_u32(out, m.values != nullptr ? 1u : 0u);
            if (m.values)
            {
                append_floats(out, m.values, m.count);
            }
        }
    }

    std::atomic<bool (*)(const LLRHI::DirectDraw&)> sOriginalDirectDraw{ nullptr };

    // Caller holds sMutex. Leaves sWriter null and sActive false either way, so a concurrent
    // captured_direct_draw already past its `active` read finds nothing to append to.
    void close_locked(const char* why)
    {
        if (sWriter)
        {
            const U64 records = sWriter->recordCount();
            if (sWriter->close())
            {
                LL_INFOS("ReplayCapture") << "research/271: capture " << why << ", " << records << " record(s)" << LL_ENDL;
            }
            else
            {
                LL_WARNS("ReplayCapture") << "research/271: capture " << why << ", but the file could not be written completely ("
                                         << records << " record(s) taken; the file ends early)" << LL_ENDL;
            }
            sWriter.reset();
        }
        sActive.store(false, std::memory_order_relaxed);
    }

    // Caller holds sMutex: the writer refused a record (or a frame boundary), so the capture
    // ends here, with the reason in the log.
    void stop_locked()
    {
        if (!sWriter)
        {
            return;
        }
        close_locked(sWriter->limitReached() ? "stopped (LLRHI_REPLAY_CAPTURE_MAX_MB reached)" :
                     sWriter->outOfMemory()  ? "stopped (out of memory)" :
                                               "stopped (the file could not be written)");
    }

    // Caller holds sMutex, off the drawing threads: the capture a drawing thread stopped
    // (sStopping), closed with its reason. False: none was.
    bool close_stopped_locked()
    {
        const U32 why = sStopping.exchange(STOP_NONE, std::memory_order_relaxed);
        if (why == STOP_COPY_OUT_OF_MEMORY)
        {
            close_locked("stopped (out of memory)");
        }
        else if (why == STOP_REFUSED)
        {
            stop_locked();
        }
        return why != STOP_NONE;
    }

    bool captured_direct_draw(const LLRHI::DirectDraw& draw)
    {
        // research/271 3.6 M4: the argument copy happens now, while the caller still owns the
        // memory draw's pointers reach into; the sequence number is taken only after calling
        // through (below), so two threads racing here cannot deadlock against a lock the real
        // draw function might itself take (as LLRHI::TrackerHold's callers sometimes do).
        // The copy goes into a buffer each thread keeps (no allocation per call once it has
        // grown to a draw's size), and running out of memory for it ends the capture, never
        // the draw: nothing here may throw into the render thread's caller.
        bool active = sActive.load(std::memory_order_relaxed);
        thread_local std::vector<U8> payload;
        if (active)
        {
            try
            {
                payload.clear();
                serialize_direct_draw(draw, payload);
            }
            catch (const std::bad_alloc&)
            {
                active = false;
                stop_from_draw(STOP_COPY_OUT_OF_MEMORY);
            }
        }
        const U64 thread = active ? thread_hash() : 0;     // only for a record (the wrapper stays after a capture)
        const bool result = sOriginalDirectDraw.load(std::memory_order_acquire)(draw);
        if (active)
        {
            const U64 sequence = sSequence.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(sMutex);
            if (sWriter && !sWriter->record(ENTRY_DIRECT_DRAW, sequence, thread, payload.data(), (U32)payload.size()))
            {
                stop_from_draw(STOP_REFUSED);
            }
        }
        return result;
    }

    // research/271 3.1: installed once (wrap_direct_draw() below is called from install() only),
    // and never undone; idempotent the way llrhidrawlist.cpp's Guard<>::wrap is, so calling it
    // more than once (there is no reason to, but a future caller doing so is harmless) does not
    // wrap its own wrapper.
    void wrap_direct_draw()
    {
        bool (*const current)(const LLRHI::DirectDraw&) = LLRHI::sDirectDraw;
        if (current && current != &captured_direct_draw)
        {
            sOriginalDirectDraw.store(current, std::memory_order_seq_cst);
            LLRHI::sDirectDraw = &captured_direct_draw;
        }
    }
}

namespace LLRHIReplayCapture
{
    void install()
    {
        // UTF-8 (GetEnvironmentVariableW underneath): a path with non-ASCII characters in it
        // reaches the file as given, which std::getenv's ANSI copy would not.
        const std::string path = LLStringUtil::getenv("LLRHI_REPLAY_CAPTURE");
        if (path.empty())
        {
            return;     // off: nothing wrapped, no cost (research/271 3.1)
        }
        // The only entry recorded so far: without it (LLRHI_DIRECT=0, so the backend set no
        // direct draw hook) a capture would hold frame boundaries and nothing else.
        if (!LLRHI::sDirectDraw)
        {
            LL_WARNS("ReplayCapture") << "research/271: LLRHI_REPLAY_CAPTURE is set, but direct draws are off "
                                         "(LLRHI_DIRECT=0): nothing to capture, not started" << LL_ENDL;
            return;
        }
        // One capture a run: a later call does nothing, while the capture runs or after it ended
        // (review replay-p0 low: it would replace the writer without closing it, and open, and so
        // empty, the file again). At the start, once: the lock is held while the file opens.
        std::lock_guard<std::mutex> lock(sMutex);
        if (sStarted)
        {
            return;
        }
        // The numbers as research/149 (P) reads switches (std::getenv): whole numbers only, a
        // value that is not one logged and the default kept; larger than the most, the most.
        U64 frames = 3;
        LLRHI::envNumber("LLRHI_REPLAY_CAPTURE_FRAMES", 1, ~(U64)0, frames);
        frames = llmin(frames, (U64)1000000);
        // research/271: at 4000-5000 draws a frame of about 500 bytes each, a frame is a couple
        // of MB; the limit keeps a long capture from filling the disk.
        U64 max_mb = 1024;
        LLRHI::envNumber("LLRHI_REPLAY_CAPTURE_MAX_MB", 1, ~(U64)0, max_mb);
        max_mb = llmin(max_mb, (U64)(1024 * 1024));
        // Best effort: a missing folder is worth making now, since a capture can run for
        // minutes before a write error is otherwise discovered.
        std::error_code unused;
        std::filesystem::create_directories(fsyspath(path).parent_path(), unused);
        auto writer = std::make_unique<LLRHIReplay::Writer>(path, max_mb << 20);
        if (!writer->ok())
        {
            LL_WARNS("ReplayCapture") << "research/271: the capture file cannot be opened, not started: " << path << LL_ENDL;
            return;
        }
        sWriter = std::move(writer);
        sStarted = true;
        sFramesLeft = (U32)frames;
        sSequence.store(0, std::memory_order_relaxed);
        wrap_direct_draw();
        sActive.store(true, std::memory_order_relaxed);
        LL_INFOS("ReplayCapture") << "research/271: capturing up to " << frames << " frame(s) (at most " << max_mb
                                  << " MB) of LLRHI::sDirectDraw to " << path << LL_ENDL;
    }

    void onFramePresented()
    {
        if (sStopping.load(std::memory_order_relaxed) != STOP_NONE)
        {
            std::lock_guard<std::mutex> lock(sMutex);
            close_stopped_locked();
            return;
        }
        if (!sActive.load(std::memory_order_relaxed))
        {
            return;
        }
        const U64 sequence = sSequence.fetch_add(1, std::memory_order_relaxed);
        const U64 thread = thread_hash();
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sWriter)
        {
            return;
        }
        if (!sWriter->frameBoundary(sequence, thread))
        {
            stop_locked();
        }
        else if (sFramesLeft && --sFramesLeft == 0)
        {
            close_locked("done (frame limit reached)");
        }
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!close_stopped_locked() && sWriter)
        {
            close_locked("flushed at shutdown (the frame limit was not reached)");
        }
    }
}
