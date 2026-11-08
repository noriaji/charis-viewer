/**
 * @file llvkrecorder.cpp
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

#include "linden_common.h"

#include "llvkrecorder.h"

#include "llvkmirror.h"

#include "llrhi.h"
#include "llthread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    // Small chunks keep the worker close behind the main thread.
    constexpr size_t CHUNK_SIZE = LLVKRecorder::MAX_PACKET_BYTES;

    struct Chunk
    {
        std::unique_ptr<unsigned char[]> data{ new unsigned char[CHUNK_SIZE] };
        size_t used = 0;
    };

    std::mutex sMutex;
    std::condition_variable sWork;      // the worker: a chunk is queued (or quit)
    std::condition_variable sIdle;      // finish(): the queue drained
    std::condition_variable sFilled;    // the worker: a spliced stream is complete
    std::deque<Chunk*> sQueue;          // filled, in order
    std::vector<Chunk*> sFree;
    bool sBusy = false;                 // the worker is replaying a chunk
    bool sQuit = false;
    std::thread sWorker;
    Chunk* sCurrent = nullptr;          // being filled by the main thread

    uint64_t sPackets = 0;              // main stream (main thread)
    uint64_t sBytes = 0;
    double sWaitMs = 0.0;

    Chunk* fresh_chunk()
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sFree.empty())
        {
            Chunk* chunk = sFree.back();
            sFree.pop_back();
            return chunk;
        }
        return new Chunk();
    }
}

// ----------------------------------------------------------------------------
// Streams (splicing)

struct LLVKRecorder::Stream
{
    enum State
    {
        FREE,           // in the pool
        FILLING,        // acquired; not complete yet
        COMPLETE,       // its filler ended it: the worker may replay it
        ORPHANED,       // given up (never spliced, or timed out): whoever is last frees it
    };
    // Changed under sMutex; COMPLETE is also read without it (acquire).
    std::atomic<int> state{ FREE };
    // Filler side (one thread at a time; the worker reads them once COMPLETE).
    std::vector<Chunk*> chunks;
    Chunk* current = nullptr;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    unsigned int counts[LLVKRecorder::MAX_COUNTERS] = {};
    // The frame's command buffers when it was acquired (deferred() on the filler).
    VkCommandBuffer cmds[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    // Main thread side.
    bool spliced = false;
    // sJoinEpoch when it was acquired (under sMutex). The worker's splice
    // timeout runs only once a later finish() has joined the fillers.
    uint64_t acquired_epoch = 0;
};

namespace
{
    using Stream = LLVKRecorder::Stream;

    std::vector<Stream*> sFreeStreams;      // under sMutex
    std::vector<Stream*> sOutstanding;      // main thread: acquired, not spliced yet

    // Worker statistics, under sMutex.
    double sBusyMs = 0.0;               // replaying (splice waits excluded)
    uint64_t sSplices = 0;              // streams replayed
    uint64_t sStreamPackets = 0;
    uint64_t sStreamBytes = 0;
    uint64_t sSpliceWaits = 0;          // splices the worker had to wait for
    double sSpliceWaitMs = 0.0;
    uint64_t sSpliceTimeouts = 0;
    uint64_t sSpliceMismatches = 0;     // replayed packets differ from what the filler put in
    uint64_t sStreamsDropped = 0;       // acquired and never spliced
    unsigned int sStreamCounts[LLVKRecorder::MAX_COUNTERS] = {};
    // Review retro-vulkan 1: how many finish() calls have joined the frame's
    // fillers (or found they could not, see finish()). Under sMutex; written
    // by the thread that calls finish().
    uint64_t sJoinEpoch = 0;

    // The worker's own tallies for the chunk being replayed (worker thread only),
    // added to the ones above under sMutex after the chunk.
    struct WorkerTally
    {
        double wait_ms = 0.0;
        uint64_t splices = 0, packets = 0, bytes = 0, waits = 0, timeouts = 0, mismatches = 0;
    };
    WorkerTally sTally;

    double splice_timeout_ms()
    {
        static const double ms = []()
        {
            const char* env = std::getenv("LLRHI_SPLICE_TIMEOUT_MS");
            const double value = env && *env ? atof(env) : 0.0;
            return value > 0.0 ? value : 5000.0;
        }();
        return ms;
    }

    // Under sMutex: the stream and its chunks back to the pools.
    void recycle_locked(Stream* stream)
    {
        for (Chunk* chunk : stream->chunks)
        {
            chunk->used = 0;
            sFree.push_back(chunk);
        }
        stream->chunks.clear();
        if (stream->current)
        {
            stream->current->used = 0;
            sFree.push_back(stream->current);
            stream->current = nullptr;
        }
        stream->packets = stream->bytes = 0;
        memset(stream->counts, 0, sizeof(stream->counts));
        stream->cmds[0] = stream->cmds[1] = VK_NULL_HANDLE;
        stream->spliced = false;
        stream->state.store(Stream::FREE, std::memory_order_relaxed);
        sFreeStreams.push_back(stream);
    }

    // Main thread, under sMutex: a stream the frame will not replay. Freed now
    // if its filler is done with it, else when the filler ends it.
    void orphan_locked(Stream* stream)
    {
        if (stream->state.load(std::memory_order_relaxed) == Stream::COMPLETE)
        {
            recycle_locked(stream);
        }
        else
        {
            stream->state.store(Stream::ORPHANED, std::memory_order_relaxed);
        }
    }

    // fix-threads4: a packet whose replay threw (bad_alloc in a deferred
    // call, ...): logged (the first few) and skipped. The rest replays, and
    // the worker goes on (an exception leaving its thread ends the process).
    void packet_failed(const char* what)
    {
        static std::atomic<uint32_t> sFailures{ 0 };
        if (sFailures.fetch_add(1, std::memory_order_relaxed) < 5)
        {
            try
            {
                LL_WARNS("VKRecorder") << "a recorded command failed to replay (" << what << "): skipped" << LL_ENDL;
            }
            catch (...)
            {
            }
        }
    }

    // The chunk's packets in order; returns how many. One try for the chunk
    // (nothing per packet when none throws): after a failure it goes on with
    // the next packet.
    uint64_t replay(Chunk* chunk)
    {
        uint64_t packets = 0;
        size_t offset = 0;
        while (offset < chunk->used)
        {
            try
            {
                for (; offset < chunk->used; ++packets)
                {
                    LLVKRecorder::Header* header = (LLVKRecorder::Header*)(chunk->data.get() + offset);
                    header->exec(header);
                    offset += header->size;
                }
            }
            catch (const std::exception& e)
            {
                packet_failed(e.what());
                offset += ((LLVKRecorder::Header*)(chunk->data.get() + offset))->size;
                ++packets;
            }
            catch (...)
            {
                packet_failed("an unknown exception");
                offset += ((LLVKRecorder::Header*)(chunk->data.get() + offset))->size;
                ++packets;
            }
        }
        return packets;
    }

    // The splice packet (worker thread): waits for the stream, replays it
    // where the packet stands, and frees it.
    struct SplicePacket
    {
        LLVKRecorder::Header header;
        Stream* stream;

        static void run(void* self)
        {
            Stream* stream = ((SplicePacket*)self)->stream;
            if (stream->state.load(std::memory_order_acquire) != Stream::COMPLETE)
            {
                const auto start = std::chrono::steady_clock::now();
                bool complete = false;
                {
                    std::unique_lock<std::mutex> lock(sMutex);
                    const auto filled = [stream] { return stream->state.load(std::memory_order_relaxed) == Stream::COMPLETE; };
                    // Review retro-vulkan 1: no timeout before a finish()
                    // has joined the fillers. Until then a filler may only
                    // be late (a cull thread behind a main thread that is
                    // held up: a cold pipeline cache, a resume from sleep,
                    // a debugger), and the main thread always comes to
                    // finish(), which records what no thread has started
                    // and waits for what runs; the worker blocks nobody
                    // before that (only finish() waits for it).
                    const uint64_t acquired = stream->acquired_epoch;
                    sFilled.wait(lock, [&] { return filled() || sJoinEpoch > acquired; });
                    // After the join every known filler is done, so this
                    // is only a backstop against a stream nobody fills.
                    complete = filled() || sFilled.wait_for(lock, std::chrono::duration<double, std::milli>(splice_timeout_ms()), filled);
                    if (!complete)
                    {
                        // Its filler still has it: it frees the stream when it ends it.
                        stream->state.store(Stream::ORPHANED, std::memory_order_relaxed);
                    }
                }
                sTally.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                ++sTally.waits;
                if (!complete)
                {
                    ++sTally.timeouts;
                    LL_WARNS("VKRecorder") << "spliced stream still not complete " << splice_timeout_ms()
                                           << " ms after its fillers were joined: the frame is not submitted, the viewer ends at the next present"
                                           << LL_ENDL;
                    // Security audit B 2: the frame lacks the stream's
                    // commands (indirect argument expansion, barriers), and
                    // its later draws would read stale ring contents. As any
                    // lost work: not submitted, the viewer ends at the next
                    // present (finish() joins this packet before the check).
                    LLVKMirror::instance().workLost("spliced stream not complete in time");
                    return;
                }
            }
            // Complete: the filler wrote everything before its release store
            // (or under sMutex, which the wait above took after it).
            uint64_t packets = 0;
            for (Chunk* chunk : stream->chunks)
            {
                packets += replay(chunk);
            }
            ++sTally.splices;
            sTally.packets += packets;
            sTally.bytes += stream->bytes;
            if (packets != stream->packets)
            {
                ++sTally.mismatches;
            }
            std::lock_guard<std::mutex> lock(sMutex);
            for (size_t i = 0; i < LLVKRecorder::MAX_COUNTERS; ++i)
            {
                sStreamCounts[i] += stream->counts[i];
            }
            recycle_locked(stream);
        }
    };

    void worker()
    {
        set_thread_name("VKRecord");
        std::unique_lock<std::mutex> lock(sMutex);
        while (true)
        {
            sWork.wait(lock, [] { return sQuit || !sQueue.empty(); });
            if (sQueue.empty())
            {
                return;     // quit
            }
            Chunk* chunk = sQueue.front();
            sQueue.pop_front();
            sBusy = true;
            lock.unlock();
            sTally = WorkerTally();
            const auto start = std::chrono::steady_clock::now();
            replay(chunk);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            lock.lock();
            sBusyMs += ms - sTally.wait_ms;
            sSplices += sTally.splices;
            sStreamPackets += sTally.packets;
            sStreamBytes += sTally.bytes;
            sSpliceWaits += sTally.waits;
            sSpliceWaitMs += sTally.wait_ms;
            sSpliceTimeouts += sTally.timeouts;
            sSpliceMismatches += sTally.mismatches;
            chunk->used = 0;
            sFree.push_back(chunk);
            sBusy = false;
            if (sQueue.empty())
            {
                sIdle.notify_all();
            }
        }
    }

    // Hands the chunk being filled to the worker.
    void hand_over()
    {
        if (!sCurrent || !sCurrent->used)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sQueue.push_back(sCurrent);
        }
        sCurrent = nullptr;
        sWork.notify_one();
    }

    // Room in the main stream (main thread).
    void* reserve_main(size_t bytes)
    {
        if (sCurrent && sCurrent->used + bytes > CHUNK_SIZE)
        {
            hand_over();
        }
        if (!sCurrent)
        {
            sCurrent = fresh_chunk();
        }
        void* packet = sCurrent->data.get() + sCurrent->used;
        sCurrent->used += bytes;
        ++sPackets;
        sBytes += bytes;
        return packet;
    }

    // Room in a stream (its filler).
    void* reserve_stream(Stream* stream, size_t bytes)
    {
        Chunk* chunk = stream->current;
        if (!chunk || chunk->used + bytes > CHUNK_SIZE)
        {
            // fix-threads4: the new chunk first. If getting it throws, current
            // is still the full chunk, which only Fill's end adds to chunks (it
            // must not be in chunks twice: replayed twice, freed twice).
            Chunk* const fresh = fresh_chunk();
            if (chunk)
            {
                stream->chunks.push_back(chunk);
            }
            chunk = stream->current = fresh;
        }
        void* packet = chunk->data.get() + chunk->used;
        chunk->used += bytes;
        ++stream->packets;
        stream->bytes += bytes;
        return packet;
    }
}

bool LLVKRecorder::enabled()
{
    static const bool on = []()
    {
        const char* env = std::getenv("LLRHI_VK_RECORD_THREAD");
        const char* checkpoints = std::getenv("LLRHI_VK_CHECKPOINTS");
        return !(env && *env == '0') && !(checkpoints && *checkpoints == '1');
    }();
    return on;
}

void LLVKRecorder::start(VkCommandBuffer a, VkCommandBuffer b)
{
    if (!sWorker.joinable())
    {
        sQuit = false;
        sWorker = std::thread(worker);
    }
    sDeferred[0].store(a, std::memory_order_relaxed);
    sDeferred[1].store(b, std::memory_order_relaxed);
}

void (*LLVKRecorder::sJoinFillers)() = nullptr;
void (*LLVKRecorder::sJoinRender)(bool) = nullptr;
void (*LLVKRecorder::sEndRender)() = nullptr;

void LLVKRecorder::finish(bool frame_end)
{
    if (!sDeferred[0].load(std::memory_order_relaxed) && !sDeferred[1].load(std::memory_order_relaxed))
    {
        return;
    }
    {
        // Review retro-vulkan 1: the worker's splice timeouts run from the
        // end of the joins. The epoch moves however they end (review rev-fix3
        // low 1: a join that throws too), or the worker would wait for the
        // splices with no time limit until the next finish().
        struct EpochOnExit
        {
            ~EpochOnExit()
            {
                {
                    std::lock_guard<std::mutex> lock(sMutex);
                    ++sJoinEpoch;
                }
                sFilled.notify_all();
            }
        } epoch_on_exit;
        if (tStream)
        {
            // The worker would wait for this thread's stream, which cannot
            // complete before this returns: after the splice timeout the frame
            // is not submitted and the viewer ends at the next present (no such
            // path is known). The epoch still moves, so it does not wait
            // forever.
            LL_WARNS_ONCE("VKRecorder") << "finish() while this thread fills a stream: the frame cannot be complete" << LL_ENDL;
        }
        else
        {
            if (sJoinRender)
            {
                sJoinRender(frame_end);     // research/208 S2: every job of the render thread is recorded
            }
            if (sJoinFillers)
            {
                sJoinFillers();     // fix-threads4: every filler of the frame is done
            }
        }
    }
    hand_over();
    const auto start = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> lock(sMutex);
        sIdle.wait(lock, [] { return sQueue.empty() && !sBusy; });
        if (frame_end && !sOutstanding.empty())
        {
            LL_WARNS("VKRecorder") << sOutstanding.size() << " stream(s) acquired and never spliced this frame: dropped" << LL_ENDL;
            sStreamsDropped += sOutstanding.size();
            for (Stream* stream : sOutstanding)
            {
                orphan_locked(stream);
            }
            sOutstanding.clear();
        }
    }
    sWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    sDeferred[0].store(VK_NULL_HANDLE, std::memory_order_relaxed);
    sDeferred[1].store(VK_NULL_HANDLE, std::memory_order_relaxed);
}

void LLVKRecorder::shutdown()
{
    if (sEndRender)
    {
        sEndRender();   // research/208 S2: the render thread's jobs joined, the thread ended
    }
    finish(true);
    if (sWorker.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sQuit = true;
        }
        sWork.notify_one();
        sWorker.join();
    }
}

void* LLVKRecorder::reserve(size_t bytes)
{
    if (bytes > CHUNK_SIZE)
    {
        // It would be written past the chunk's end into the heap (a crash in
        // ntdll much later, research/96): stop here, where the cause shows.
        LL_ERRS("VKRecorder") << "a packet of " << bytes << " bytes is larger than a chunk (" << CHUNK_SIZE << ")" << LL_ENDL;
    }
    llassert(bytes % 8 == 0);
    if (Stream* stream = tStream)
    {
        return reserve_stream(stream, bytes);
    }
    return reserve_main(bytes);
}

LLVKRecorder::Stream* LLVKRecorder::acquire()
{
    const VkCommandBuffer a = sDeferred[0].load(std::memory_order_relaxed);
    const VkCommandBuffer b = sDeferred[1].load(std::memory_order_relaxed);
    if (!LLRHI::sRecordSplice || (!a && !b))
    {
        return nullptr;
    }
    Stream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sFreeStreams.empty())
        {
            stream = sFreeStreams.back();
            sFreeStreams.pop_back();
        }
        if (!stream)
        {
            stream = new Stream();
        }
        stream->acquired_epoch = sJoinEpoch;
    }
    stream->cmds[0] = a;
    stream->cmds[1] = b;
    stream->spliced = false;
    stream->state.store(Stream::FILLING, std::memory_order_relaxed);
    sOutstanding.push_back(stream);
    return stream;
}

void LLVKRecorder::splice(Stream* stream)
{
    if (!stream)
    {
        return;
    }
    auto it = std::find(sOutstanding.begin(), sOutstanding.end(), stream);
    llassert(it != sOutstanding.end() && !stream->spliced);
    if (it == sOutstanding.end() || stream->spliced)
    {
        return;     // spliced twice or from another frame: replaying it would be wrong
    }
    sOutstanding.erase(it);
    stream->spliced = true;
    // Into the main stream even when this thread fills a stream of its own.
    SplicePacket* p = (SplicePacket*)reserve_main(align8(sizeof(SplicePacket)));
    p->header = { &SplicePacket::run, (uint32_t)align8(sizeof(SplicePacket)) };
    p->stream = stream;
}

LLVKRecorder::Fill::Fill(Stream* stream)
    : mStream(stream)
{
    if (!stream)
    {
        return;
    }
    llassert(!tStream && stream->state.load(std::memory_order_relaxed) != Stream::FREE);
    if (tStream)
    {
        // Security audit B 12: a fill inside another on this thread (no such
        // path is known; llassert only in development builds). The outer
        // stream is this thread's again when this one ends, so the rest of
        // its commands do not go to the main stream out of order.
        LL_WARNS_ONCE("VKRecorder") << "a stream filled while this thread fills another" << LL_ENDL;
        mOuter = tStream;
    }
    tStream = stream;
    tStreamCmds = stream->cmds;
    tCounts = stream->counts;
}

void LLVKRecorder::Fill::restoreOuter()
{
    Stream* outer = mOuter;
    tStream = outer;
    tStreamCmds = outer ? outer->cmds : nullptr;
    tCounts = outer ? outer->counts : nullptr;
}

LLVKRecorder::Fill::~Fill()
{
    Stream* stream = mStream;
    if (!stream)
    {
        return;
    }
    restoreOuter();
    if (stream->current)
    {
        stream->chunks.push_back(stream->current);
        stream->current = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (stream->state.load(std::memory_order_relaxed) == Stream::ORPHANED)
        {
            recycle_locked(stream);     // nobody replays it any more
            return;
        }
        stream->state.store(Stream::COMPLETE, std::memory_order_release);
    }
    sFilled.notify_all();
}

bool LLVKRecorder::Fill::abandon()
{
    Stream* stream = mStream;
    if (!stream)
    {
        return false;
    }
    mStream = nullptr;  // the end of the scope does nothing
    restoreOuter();
    std::lock_guard<std::mutex> lock(sMutex);
    if (stream->state.load(std::memory_order_relaxed) == Stream::ORPHANED)
    {
        recycle_locked(stream);     // nobody replays it any more (its chunks go back with it)
        return false;
    }
    // Its packets back to the pool; still FILLING (the recording worker waits for it at its splice).
    for (Chunk* chunk : stream->chunks)
    {
        chunk->used = 0;
        sFree.push_back(chunk);
    }
    stream->chunks.clear();
    if (stream->current)
    {
        stream->current->used = 0;
        sFree.push_back(stream->current);
        stream->current = nullptr;
    }
    stream->packets = stream->bytes = 0;
    memset(stream->counts, 0, sizeof(stream->counts));
    return true;
}

void LLVKRecorder::takeStats(uint64_t& packets, uint64_t& bytes, double& wait_ms)
{
    packets = sPackets;
    bytes = sBytes;
    wait_ms = sWaitMs;
    sPackets = sBytes = 0;
    sWaitMs = 0.0;
}

std::string LLVKRecorder::takeWorkerStats(uint32_t frames)
{
    frames = std::max(frames, 1u);
    std::lock_guard<std::mutex> lock(sMutex);
    std::ostringstream out;
    out << "busy " << sBusyMs / frames << " ms";
    if (sSplices || sSpliceWaits || sSpliceTimeouts || sStreamsDropped || sSpliceMismatches)
    {
        out << ", splices " << (double)sSplices / frames << " (packets " << sStreamPackets / frames << ", " << sStreamBytes / frames / 1024
            << " KB, waited for " << (double)sSpliceWaits / frames << " in " << sSpliceWaitMs / frames << " ms";
        if (sSpliceTimeouts)
        {
            out << ", timed out " << sSpliceTimeouts;
        }
        if (sStreamsDropped)
        {
            out << ", never spliced " << sStreamsDropped;
        }
        if (sSpliceMismatches)
        {
            out << ", packet count mismatches " << sSpliceMismatches;
        }
        out << ")";
    }
    sBusyMs = sSpliceWaitMs = 0.0;
    sSplices = sStreamPackets = sStreamBytes = sSpliceWaits = sSpliceTimeouts = sSpliceMismatches = sStreamsDropped = 0;
    return out.str();
}

void LLVKRecorder::takeStreamCounts(unsigned int* counts, size_t count)
{
    std::lock_guard<std::mutex> lock(sMutex);
    for (size_t i = 0; i < count && i < MAX_COUNTERS; ++i)
    {
        counts[i] += sStreamCounts[i];
        sStreamCounts[i] = 0;
    }
}
