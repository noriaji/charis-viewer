/**
 * @file llvkrecorder.h
 * @brief Records the frame's Vulkan commands on a worker thread.
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

#ifndef LL_LLVKRECORDER_H
#define LL_LLVKRECORDER_H

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// The NVIDIA driver spends about 30 ms of a heavy frame recording vkCmd* calls
// (research/37); GL's driver does the same work on a thread of its own. Between
// LLVKMirror::beginFrame and endFrame the frame's two command buffers are
// recorded by a worker: the calls (through llvkdispatch.h) only append packets,
// with their pointed-to arguments copied, and the worker replays them in order.
// Everything else (other command buffers, device calls) still runs directly.
namespace LLVKRecorder
{
    // A packet starts with its replay function and its size in bytes.
    struct Header
    {
        void (*exec)(void* packet);
        uint32_t size;
    };

    // A private packet stream another thread fills for a known place in the
    // frame (research/66 step 1, "splice"). Opaque.
    struct Stream;

    // This thread's stream being filled (Fill), and the command buffers it
    // records for: while one is set, those command buffers count as deferred
    // on this thread even across a finish()/start() on the main thread.
    inline thread_local Stream* tStream = nullptr;
    inline thread_local const VkCommandBuffer* tStreamCmds = nullptr;

    // Where this thread counts its vkCmd* calls (LLVKDispatch::counts()):
    // null for the shared counters, a stream's own while this thread fills it
    // (added to the shared ones once the stream has been replayed).
    constexpr size_t MAX_COUNTERS = 64;
    inline thread_local unsigned int* tCounts = nullptr;

    // The command buffers whose commands go to the worker (null when not
    // deferring). Written by the main thread in start() and finish(); read by
    // any thread recording a stream (relaxed: a stream filler does not rely on
    // them, see deferred()).
    inline std::atomic<VkCommandBuffer> sDeferred[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    inline bool deferred(VkCommandBuffer cmd)
    {
        if (!cmd)
        {
            return false;
        }
        if (cmd == sDeferred[0].load(std::memory_order_relaxed) || cmd == sDeferred[1].load(std::memory_order_relaxed))
        {
            return true;
        }
        const VkCommandBuffer* filling = tStreamCmds;
        return filling && (cmd == filling[0] || cmd == filling[1]);
    }

    // LLRHI_VK_RECORD_THREAD=0 turns it off; so do draw checkpoints (their
    // markers are recorded directly, which would reorder them).
    bool enabled();
    // From now on, commands for these go to the worker.
    void start(VkCommandBuffer a, VkCommandBuffer b);
    // Returns once the worker recorded everything; later commands run directly.
    // frame_end: the frame is over (LLVKMirror::endFrame), so a stream acquired
    // and never spliced is reported and dropped (a mid-frame finish, for
    // submitAndWait, leaves them be: they may still be spliced later).
    void finish(bool frame_end = false);
    void shutdown();
    // fix-threads4: called first by finish() (on its caller's thread, while
    // that thread fills no stream): a filler's owner records there what no
    // thread has started and waits for what runs, so no filler goes on past
    // the frame (LLVKDrawList's segments). Set once at install.
    extern void (*sJoinFillers)();
    // research/208 S2: called by finish() before sJoinFillers (same rules):
    // the render thread's jobs (LLVKRender) are all recorded when it returns.
    // frame_end as finish()'s. Set once when the render thread starts.
    extern void (*sJoinRender)(bool frame_end);
    // research/208 S2: called first by shutdown(): the render thread joins
    // its jobs and ends before the worker does. Set with sJoinRender.
    extern void (*sEndRender)();

    // Room for one packet of `bytes` (a multiple of 8, header included). The
    // caller fills it before the next reserve(). On a thread filling a stream
    // (Fill) it goes into that stream; otherwise into the frame's main stream,
    // whose callers are serialized (the GL translation's lock).
    // A packet is at most MAX_PACKET_BYTES (one chunk); callers with
    // unbounded arrays split them over several packets (llvkdispatch.h).
    constexpr size_t MAX_PACKET_BYTES = 64 * 1024;
    void* reserve(size_t bytes);

    constexpr size_t align8(size_t bytes) { return (bytes + 7) & ~(size_t)7; }

    // --- Splicing (research/66 step 1) -------------------------------------
    // A worker thread records part of the frame into a private stream while
    // the main thread goes on; the main thread puts one splice packet where
    // those commands belong, and the recording worker replays the stream
    // there, waiting for it to be complete if it has to. The command buffers
    // get exactly what they would have got had the main thread recorded the
    // commands itself at the splice point.
    //
    // Rules
    // - acquire, splice: main thread (with the same serialization as
    //   reserve). Fill: the one thread that fills the stream; at most one
    //   stream per thread at a time; the main thread may fill one too.
    // - Each acquired stream is spliced exactly once (before or after it is
    //   filled). A stream lives within its frame: the recording
    //   worker has replayed it before the frame's finish() returns, after
    //   which its memory goes back to a pool (packets hold copies of their
    //   arguments, so nothing in a stream outlives the frame; what packets
    //   point at - the frame's ring, rows, sets - follows the frame slot's
    //   fence as before).
    // - A filler records only with the frame's command buffers (those of
    //   start()), only through the llvkdispatch.h calls (and without pNext
    //   chains, which those calls record directly), and never calls
    //   finish() (directly or through LLVKMirror::submitAndWait / endFrame)
    //   while filling: the worker would wait for a stream that cannot complete.
    // - What a stream binds stays bound after the splice point and what was
    //   bound before it is what the stream starts with: the main thread's
    //   caches of bound state are stale past a splice. Splice inside a native
    //   pass (after LLVKTracker::beginPass, which drops them), as the self-test
    //   does. A stream's calls are counted by the stream and reach
    //   LLVKDispatch::sCounts only at the perf report, so nothing may use those
    //   counts to notice commands recorded in a stream.
    // - Rows and ring memory a filler writes (LLVKBindless::matrixRow(s),
    //   paletteRows, LLVKMirror::allocate / UploadBlock) are written before its
    //   Fill ends; descriptor sets come from the main thread (allocateSet is
    //   not thread-safe).
    // - The worker waits for a spliced stream as long as it takes until a
    //   finish() has joined the fillers (sJoinRender, sJoinFillers): before
    //   that a filler may only be late. A stream still not complete
    //   LLRHI_SPLICE_TIMEOUT_MS (default 5000) after that join is lost work:
    //   the frame is not submitted and the viewer ends at the next present
    //   (LLVKMirror::workLost), rather than hanging the frame or drawing
    //   without its commands. Its memory is reclaimed when the filler ends it.
    //
    // Null when nothing is deferred (no frame, LLRHI_VK_RECORD_THREAD=0,
    // checkpoints) or splicing is off (LLRHI_RECORD_SPLICE=0,
    // recordsplice.toggle): the caller records inline as before.
    Stream* acquire();
    // The stream's commands go here in the frame's order.
    void splice(Stream* stream);

    // Routes this thread's packets into the stream until destroyed, then
    // marks it complete. A null stream does nothing (record as usual).
    class Fill
    {
    public:
        explicit Fill(Stream* stream);
        ~Fill();
        Fill(const Fill&) = delete;
        Fill& operator=(const Fill&) = delete;

        // research/208 S2: the filler could not finish the stream (an
        // exception part way): what it put in is dropped and this thread
        // stops filling it, without marking it complete, so another thread
        // may fill it from the start (a new Fill). False: the frame gave the
        // stream up meanwhile (the splice timed out); it is freed and must
        // not be filled again. Nothing happens at the end of the scope.
        bool abandon();

    private:
        // This thread's stream again (none, or the one it filled before this Fill).
        void restoreOuter();

        Stream* mStream;
        Stream* mOuter = nullptr;   // a fill inside another (security audit B 12)
    };

    // Statistics for the perf report: main stream packets and bytes since the
    // last call, and how long finish() waited.
    void takeStats(uint64_t& packets, uint64_t& bytes, double& wait_ms);
    // The rest of the recording worker's part of the VKPerf line: its busy
    // time (replaying, not waiting) and the splices, per frame; resets.
    std::string takeWorkerStats(uint32_t frames);
    // Adds the vkCmd* counts of the streams replayed since the last call to
    // counts (LLVKDispatch::sCounts, count entries); resets.
    void takeStreamCounts(unsigned int* counts, size_t count);
}

#endif // LL_LLVKRECORDER_H
