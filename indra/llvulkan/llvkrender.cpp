/**
 * @file llvkrender.cpp
 * @brief The render thread (research/208 S2, stage 4a): parts of the frame recorded from values on a thread of their own.
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

#include "llvkrender.h"

#include "llrhi.h"          // sRenderThread (renderthread.toggle)
#include "llrhiframe.h"     // LLRHI_RENDER_THREAD
#include "llthread.h"       // set_thread_name
#include "llvkdevice.h"     // research/208 13.3: the secondary command buffers' pools
#include "llvkrecorder.h"

#if LL_WINDOWS
#include "llwin32headers.h"     // GetProcessAffinityMask
#endif

#include <bit>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <intrin.h>     // _mm_pause
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    using LLVKRender::Job;
    using Clock = std::chrono::steady_clock;

    constexpr U32 AUTO_FRAMES = 600;            // auto: frames of compared jobs without a difference before the thread records
    constexpr U32 MIN_PROCESSORS = 8;           // research/208 4.3: fewer, and auto leaves the thread unused
    constexpr F64 WAIT_WARN_MS = 1000.0;        // a job the thread runs this long at a join: the thread is off from then on
    constexpr auto SPIN = std::chrono::microseconds(20);    // research/208 3.3: spun before the thread sleeps
    constexpr U32 DIFFS_KEPT = 8;
    constexpr U32 KINDS = 4;

    // The queue (posting order) and the thread's place in it.
    std::mutex sMutex;
    std::condition_variable sDone;      // a job the join waits for ended
    std::vector<Job*> sQueue;           // under sMutex: posted since the last join
    size_t sNext = 0;                   // under sMutex: the next job the thread takes
    bool sJoinWaiting = false;          // under sMutex
    bool sQuit = false;                 // under sMutex
    F64 sBusyMs = 0.0;                  // under sMutex: the thread recording (since the last report)
    std::atomic<U32> sSignal{ 0 };      // bumped by every post and the quit; the thread sleeps on it
    std::atomic<bool> sSleeping{ false };
    std::atomic<bool> sFailed{ false };     // off for the session (a job failed, or ran too long)
    // Never destroyed with the statics (a joinable std::thread there ends the process, research/104).
    std::thread* sThread = nullptr;
    U32 sTaken = 0;                     // the thread's: the order it took verify jobs in
    thread_local bool tRenderThread = false;    // research/208 S3: set on the thread itself
    void (*sFrameJoinHook)() = nullptr;         // research/208 S3: main thread only

    // Main thread only.
    std::vector<Job*> sJoining;
    bool sShutDown = false;
    bool sChecked = false;              // the processors and the mode logged
    bool sNotForThisPC = false;         // auto with fewer than MIN_PROCESSORS
    U32 sProcessors = 0;
    bool sAutoOn = false;
    U32 sCleanFrames = 0;
    bool sFrameCompared = false, sFrameDiffered = false;
    U32 sFramePosted = 0;               // posting order within the frame
    U32 sLastTaken = 0;                 // the join's: the last verify job's mTaken (order check)
    bool sAnyTaken = false;
    const char* sFailure = nullptr;     // why the thread is off (the first)

    struct Counts
    {
        U32 frames = 0;
        U32 posted = 0, verified = 0;
        U32 on_thread = 0, taken_back = 0, again = 0, lost = 0;
        U32 deferred = 0;               // research/208 S3: recorded by the join as posted (verify)
        U32 mid_joins = 0;              // research/208 S3: joins in the frame that took jobs back
        U32 compared = 0, differ = 0, order_differ = 0, not_compared = 0;
        U32 waits = 0;
        F64 wait_ms = 0.0;
        const char* kind[KINDS] = {};
        U32 kind_count[KINDS] = {};
        const char* diff_what[DIFFS_KEPT] = {};
        U32 diff_pass[DIFFS_KEPT] = {};
        U32 diff_kept = 0;
    };
    Counts sCounts;

    U32 logical_processors()
    {
        U32 n = std::thread::hardware_concurrency();
#if LL_WINDOWS
        DWORD_PTR process = 0, system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process)
        {
            const U32 allowed = (U32)std::popcount((U64)process);
            n = n ? llmin(n, allowed) : allowed;
        }
#endif
        return n;
    }

    void fail(const char* why)
    {
        if (!sFailed.exchange(true, std::memory_order_relaxed))
        {
            sFailure = why;
            try
            {
                LL_WARNS("VKRender") << "render thread (research/208 S2): " << why
                                     << "; its jobs are recorded by the main thread from now on" << LL_ENDL;
            }
            catch (...)
            {
            }
        }
    }

    // The thread: a job it takes (the join has not), recorded into the job's
    // stream (or, verify, its values read).
    void run(Job& job)
    {
        int expected = Job::QUEUED;
        if (!job.mState.compare_exchange_strong(expected, Job::RUNNING, std::memory_order_acq_rel))
        {
            return;     // the join took it
        }
        const Clock::time_point start = Clock::now();
        int result = Job::DONE;
        if (job.mVerify)
        {
            try
            {
                job.mThreadDigest = job.digest();
                job.mTaken = ++sTaken;
            }
            catch (...)
            {
                result = Job::FAILED;
            }
        }
        else
        {
            LLVKRecorder::Fill fill(job.mStream);
            bool recorded = true;
            try
            {
                job.record();
            }
            catch (...)
            {
                recorded = false;
            }
            if (!recorded)
            {
                // What it put in is dropped; the join records it again (or, the frame gave the stream up, counts it lost).
                job.mStream = fill.abandon() ? job.mStream : nullptr;
                result = Job::FAILED;
            }
        }
        const F64 ms = std::chrono::duration<F64, std::milli>(Clock::now() - start).count();
        job.mState.store(result, std::memory_order_release);
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sBusyMs += ms;
            notify = sJoinWaiting;
        }
        if (notify)
        {
            sDone.notify_all();
        }
    }

    void thread_main()
    {
        set_thread_name("VKRender");
        tRenderThread = true;
        for (;;)
        {
            // Read before looking at the queue: a post after this moves it, so it is not slept through.
            const U32 seen = sSignal.load(std::memory_order_acquire);
            Job* job = nullptr;
            {
                std::lock_guard<std::mutex> lock(sMutex);
                if (sQuit)
                {
                    return;
                }
                if (sNext < sQueue.size())
                {
                    job = sQueue[sNext++];
                }
            }
            if (job)
            {
                run(*job);
                continue;
            }
            // Nothing queued: the passes come in bursts, so a short spin, then sleep until a post.
            const Clock::time_point spin_end = Clock::now() + SPIN;
            bool moved = false;
            while (Clock::now() < spin_end)
            {
                if (sSignal.load(std::memory_order_acquire) != seen)
                {
                    moved = true;
                    break;
                }
                _mm_pause();
            }
            if (moved)
            {
                continue;
            }
            sSleeping.store(true, std::memory_order_seq_cst);
            if (sSignal.load(std::memory_order_seq_cst) == seen)
            {
                sSignal.wait(seen, std::memory_order_seq_cst);
            }
            sSleeping.store(false, std::memory_order_relaxed);
        }
    }

    void wake()
    {
        sSignal.fetch_add(1, std::memory_order_seq_cst);
        if (sSleeping.load(std::memory_order_seq_cst))
        {
            sSignal.notify_one();
        }
    }

    void join_hook(bool frame_end)
    {
        LLVKRender::join(frame_end);
    }

    bool ensure_thread()
    {
        if (sThread)
        {
            return true;
        }
        if (sShutDown || sFailed.load(std::memory_order_relaxed))
        {
            return false;
        }
        try
        {
            sThread = new std::thread(thread_main);
        }
        catch (...)
        {
            fail("the thread could not be started");
            return false;
        }
        LLVKRecorder::sJoinRender = &join_hook;     // every finish() joins from now on
        LLVKRecorder::sEndRender = &LLVKRender::shutdown;   // and the worker's shutdown ends the thread first
        LL_INFOS("VKRender") << "render thread (research/208 S2, LLRHI_RENDER_THREAD="
                             << LLRHI::renderThreadModeName(LLRHI::renderThreadMode()) << ") started on " << sProcessors
                             << " logical processors: the retained scene's expands and indirect draws of the main and alpha views"
                             << LL_ENDL;
        return true;
    }

    void count_kind(const Job& job)
    {
        const char* what = job.what();
        for (U32 i = 0; i < KINDS; ++i)
        {
            if (sCounts.kind[i] == what || !sCounts.kind[i])
            {
                sCounts.kind[i] = what;
                ++sCounts.kind_count[i];
                return;
            }
        }
    }

    // The job is the thread's from here until the join (nothing of it is read or written here after).
    void enqueue(Job& job)
    {
        count_kind(job);
        job.mPosted = sFramePosted++;
        job.mOnMain = false;
        job.mState.store(Job::QUEUED, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sQueue.push_back(&job);
        }
        wake();
    }

    // The join: a job the thread is running, waited for (it owns its stream until it ends it).
    void wait_for(Job& job)
    {
        const Clock::time_point start = Clock::now();
        bool warned = false;
        std::unique_lock<std::mutex> lock(sMutex);
        sJoinWaiting = true;
        while (job.mState.load(std::memory_order_acquire) == Job::RUNNING)
        {
            sDone.wait_for(lock, std::chrono::milliseconds(100));
            if (!warned && std::chrono::duration<F64, std::milli>(Clock::now() - start).count() >= WAIT_WARN_MS)
            {
                warned = true;
                fail("a job ran longer than 1 s at a join (waited for: its stream is the thread's until it ends it)");
            }
        }
        sJoinWaiting = false;
        ++sCounts.waits;
        sCounts.wait_ms += std::chrono::duration<F64, std::milli>(Clock::now() - start).count();
    }

    // The join's recording of a job on the main thread (taken back, deferred, or again after the thread's
    // failed). One that throws does not leave the join (review rev-fix3 low 1: the other jobs would not be
    // joined, and finish() would end without its usual end): what it put in is dropped and the stream ends
    // empty, so the recording worker's splice does not wait for it; the frame lacks its commands (as a lost
    // job's) and the jobs are recorded inline from then on.
    void record_on_main(Job& job)
    {
        {
            LLVKRecorder::Fill fill(job.mStream);
            try
            {
                job.record();
                return;
            }
            catch (...)
            {
            }
            if (!fill.abandon())
            {
                job.mStream = nullptr;  // the frame gave it up meanwhile (freed)
            }
        }
        if (job.mStream)
        {
            LLVKRecorder::Fill empty(job.mStream);     // complete, with nothing in it
        }
        fail("a job failed (an exception) on the main thread at a join: its commands are missing from that frame");
        ++sCounts.lost;
    }

    // A joined job's counts and checks; it is free again.
    void fold(Job& job)
    {
        if (job.mVerify)
        {
            if (job.mOnMain || job.mState.load(std::memory_order_relaxed) != Job::DONE)
            {
                ++sCounts.not_compared;
            }
            else
            {
                ++sCounts.compared;
                sFrameCompared = true;
                const bool differs = job.mThreadDigest != job.mMainDigest;
                const bool out_of_order = sAnyTaken && job.mTaken <= sLastTaken;
                sLastTaken = job.mTaken;
                sAnyTaken = true;
                if (differs || out_of_order)
                {
                    sFrameDiffered = true;
                    sCleanFrames = 0;
                    sCounts.differ += differs ? 1 : 0;
                    sCounts.order_differ += out_of_order ? 1 : 0;
                    if (sCounts.diff_kept < DIFFS_KEPT)
                    {
                        sCounts.diff_what[sCounts.diff_kept] = job.what();
                        sCounts.diff_pass[sCounts.diff_kept] = job.mPass;
                        ++sCounts.diff_kept;
                    }
                }
            }
        }
        else if (!job.mOnMain)
        {
            ++sCounts.on_thread;
        }
        job.mStream = nullptr;
        job.mState.store(Job::IDLE, std::memory_order_release);
    }
}

LLVKRender::Use LLVKRender::use()
{
    const LLRHI::RenderThreadMode mode = LLRHI::renderThreadMode();
    // Without the recording worker (LLRHI_VK_RECORD_THREAD=0, checkpoints) nothing is spliced and finish() never joins.
    if (mode == LLRHI::RenderThreadMode::Off || !LLRHI::sRenderThread || sShutDown || sFailed.load(std::memory_order_relaxed) ||
        !LLVKRecorder::enabled())
    {
        return Use::Inline;
    }
    if (!sChecked)
    {
        sChecked = true;
        sProcessors = logical_processors();
        sNotForThisPC = mode == LLRHI::RenderThreadMode::Auto && sProcessors < MIN_PROCESSORS;
        if (sProcessors < MIN_PROCESSORS)
        {
            LL_INFOS("VKRender") << "render thread (research/208 S2): " << sProcessors << " logical processors for this process (fewer than "
                                 << MIN_PROCESSORS << "): " << (sNotForThisPC ? "auto leaves the thread unused" : "used as LLRHI_RENDER_THREAD asks")
                                 << LL_ENDL;
        }
    }
    if (sNotForThisPC)
    {
        return Use::Inline;
    }
    if (mode == LLRHI::RenderThreadMode::Verify || (mode == LLRHI::RenderThreadMode::Auto && !sAutoOn))
    {
        return Use::Verify;
    }
    return Use::Thread;
}

bool LLVKRender::post(Job& job)
{
    if (LLVKRecorder::tStream || !ensure_thread())
    {
        return false;   // the main thread fills a stream of its own (the splice would go before its packets), or no thread
    }
    LLVKRecorder::Stream* stream = LLVKRecorder::acquire();
    if (!stream)
    {
        return false;
    }
    LLVKRecorder::splice(stream);
    job.mStream = stream;
    job.mVerify = false;
    ++sCounts.posted;
    enqueue(job);
    return true;
}

bool LLVKRender::postDeferred(Job& job)
{
    if (LLVKRecorder::tStream || !ensure_thread())
    {
        return false;   // as post
    }
    LLVKRecorder::Stream* stream = LLVKRecorder::acquire();
    if (!stream)
    {
        return false;
    }
    LLVKRecorder::splice(stream);
    job.mStream = stream;
    job.mVerify = false;
    ++sCounts.posted;
    count_kind(job);
    job.mPosted = sFramePosted++;
    job.mOnMain = false;
    job.mState.store(Job::MAIN, std::memory_order_release);     // run() cannot take it (its exchange wants QUEUED)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sQueue.push_back(&job);
    }
    return true;    // nothing for the thread to do: not woken
}

void LLVKRender::setFrameJoinHook(void (*hook)())
{
    sFrameJoinHook = hook;
}

bool LLVKRender::onThread()
{
    return tRenderThread;
}

void LLVKRender::failSession(const char* why)
{
    fail(why);
}

void LLVKRender::postVerify(Job& job)
{
    if (!ensure_thread())
    {
        ++sCounts.not_compared;
        return;     // left idle
    }
    job.mStream = nullptr;
    job.mVerify = true;
    ++sCounts.verified;
    enqueue(job);
}

void LLVKRender::join(bool frame_end)
{
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sJoining.swap(sQueue);  // the thread takes nothing of these any more (sQueue was the join's empty one)
        sNext = 0;
    }
    sCounts.mid_joins += !frame_end && !sJoining.empty() ? 1 : 0;
    for (Job* job : sJoining)
    {
        int state = Job::QUEUED;
        if (job->mState.compare_exchange_strong(state, Job::RUNNING, std::memory_order_acq_rel))
        {
            // The thread had not taken it: recorded here (a verify job only goes uncompared: the main thread drew it).
            job->mOnMain = true;
            if (!job->mVerify)
            {
                ++sCounts.taken_back;
                record_on_main(*job);
            }
            job->mState.store(Job::DONE, std::memory_order_relaxed);
        }
        else if (state == Job::MAIN)
        {
            // research/208 S3: the main thread's (postDeferred), recorded here in its place.
            job->mOnMain = true;
            job->mState.store(Job::RUNNING, std::memory_order_relaxed);
            ++sCounts.deferred;
            record_on_main(*job);
            job->mState.store(Job::DONE, std::memory_order_relaxed);
        }
        else if (state == Job::RUNNING)
        {
            wait_for(*job);
        }
        if (job->mState.load(std::memory_order_acquire) == Job::FAILED)
        {
            if (job->mVerify)
            {
                fail("a job's values could not be read");
            }
            else if (job->mStream)
            {
                // Its stream emptied (Fill::abandon): recorded again here, the frame keeps its commands.
                fail("a job failed (an exception) and was recorded again by the main thread");
                job->mOnMain = true;
                ++sCounts.again;
                record_on_main(*job);
            }
            else
            {
                fail("a job failed after its splice had timed out: its commands are missing from that frame");
                ++sCounts.lost;
            }
        }
        fold(*job);
    }
    sJoining.clear();
    if (frame_end)
    {
        ++sCounts.frames;
        sFramePosted = 0;
        sAnyTaken = false;
        if (sFrameCompared)
        {
            sCleanFrames = sFrameDiffered ? 0 : sCleanFrames + 1;
        }
        sFrameCompared = false;
        sFrameDiffered = false;
        if (LLRHI::renderThreadMode() == LLRHI::RenderThreadMode::Auto && !sAutoOn && sCleanFrames >= AUTO_FRAMES)
        {
            sAutoOn = true;
            LL_INFOS("VKRender") << "render thread (research/208 S2): " << sCleanFrames
                                 << " frames of compared jobs without a difference; the thread records them from now on" << LL_ENDL;
        }
        if (sFrameJoinHook)
        {
            sFrameJoinHook();   // research/208 S3: the scene's tables back to the main thread
        }
    }
}

void LLVKRender::shutdown()
{
    if (!sThread)
    {
        sShutDown = true;
        return;
    }
    join(true);     // nothing is left to the thread
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sQuit = true;
    }
    sSignal.fetch_add(1, std::memory_order_seq_cst);
    sSignal.notify_all();
    sThread->join();
    delete sThread;
    sThread = nullptr;
    sShutDown = true;
}

std::string LLVKRender::takeReport(U32 frames)
{
    const LLRHI::RenderThreadMode mode = LLRHI::renderThreadMode();
    const Counts c = sCounts;
    F64 busy = 0.0;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        busy = sBusyMs;
        sBusyMs = 0.0;
    }
    sCounts = Counts();
    if (mode == LLRHI::RenderThreadMode::Off || (!c.posted && !c.verified && !sFailed.load(std::memory_order_relaxed)))
    {
        return std::string();
    }
    frames = llmax(frames, 1u);
    std::ostringstream out;
    out << "render thread (research/208 S2, " << LLRHI::renderThreadModeName(mode);
    if (mode == LLRHI::RenderThreadMode::Auto)
    {
        out << (sAutoOn ? ", recording" : ", comparing");
    }
    if (!LLRHI::sRenderThread)
    {
        out << ", toggled off";
    }
    if (sFailed.load(std::memory_order_relaxed))
    {
        out << ", OFF: " << (sFailure ? sFailure : "?");
    }
    out << "): jobs " << (F32)(c.posted + c.verified) / frames << " a frame (";
    for (U32 i = 0; i < KINDS && c.kind[i]; ++i)
    {
        out << (i ? ", " : "") << c.kind[i] << " " << (F32)c.kind_count[i] / frames;
    }
    out << "), recorded by the thread " << c.on_thread << ", taken back at the join " << c.taken_back;
    if (c.deferred || c.mid_joins)
    {
        // research/208 S3: verify's passes recorded by the joins, and the joins in the frame that took jobs back.
        out << ", recorded by the join as posted " << c.deferred << ", joins in the frame " << c.mid_joins;
    }
    if (c.again || c.lost)
    {
        out << ", RECORDED AGAIN after a failure " << c.again << ", LOST " << c.lost;
    }
    if (c.verified)
    {
        out << "; verify: compared " << c.compared << ", differ " << c.differ << ", out of order " << c.order_differ << ", not compared "
            << c.not_compared;
        for (U32 i = 0; i < c.diff_kept; ++i)
        {
            out << (i ? "; " : " (") << c.diff_what[i] << " of pass " << c.diff_pass[i];
        }
        out << (c.diff_kept ? ")" : "");
        if (mode == LLRHI::RenderThreadMode::Auto && !sAutoOn)
        {
            out << ", frames without a difference " << sCleanFrames << " of " << AUTO_FRAMES;
        }
    }
    out << "; join waits " << c.waits << " (" << c.wait_ms << " ms), thread busy " << busy / frames << " ms a frame";
    return out.str();
}

// ==== research/208 13.3 (vk-s4a4): the threads' secondary command buffers ====

namespace
{
    struct SecondaryPools
    {
        VkCommandPool pool[LLVKRender::SECONDARY_SLOTS] = {};
        U64 begins[LLVKRender::SECONDARY_SLOTS] = { ~0ull, ~0ull };
        std::vector<VkCommandBuffer> buffers[LLVKRender::SECONDARY_SLOTS];
        size_t next[LLVKRender::SECONDARY_SLOTS] = {};
    };
    std::mutex sPoolMutex;                      // sPools
    std::vector<SecondaryPools*> sPools;        // every thread's (never freed: a thread may still hold its pointer)
    std::atomic<bool> sPoolsGone{ false };
    thread_local SecondaryPools* tPools = nullptr;
}

VkCommandBuffer LLVKRender::beginSecondary(const SecondaryTarget& target, U32 frame_slot, U64 frame_begins)
{
    if (frame_slot >= SECONDARY_SLOTS || sPoolsGone.load(std::memory_order_acquire))
    {
        return VK_NULL_HANDLE;
    }
    LLVKDevice& device = LLVKDevice::instance();
    const VkDevice vk = device.getDevice();
    if (!vk)
    {
        return VK_NULL_HANDLE;
    }
    if (!tPools)
    {
        std::lock_guard<std::mutex> lock(sPoolMutex);
        tPools = new SecondaryPools();
        sPools.push_back(tPools);
    }
    SecondaryPools& p = *tPools;
    if (!p.pool[frame_slot])
    {
        VkCommandPoolCreateInfo create = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        create.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        create.queueFamilyIndex = device.getGraphicsQueueFamily();
        if (vkCreateCommandPool(vk, &create, nullptr, &p.pool[frame_slot]) != VK_SUCCESS)
        {
            p.pool[frame_slot] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
    }
    if (p.begins[frame_slot] != frame_begins)
    {
        // The slot's last frame is done on the GPU: its buffers are free again.
        vkResetCommandPool(vk, p.pool[frame_slot], 0);
        p.begins[frame_slot] = frame_begins;
        p.next[frame_slot] = 0;
    }
    std::vector<VkCommandBuffer>& buffers = p.buffers[frame_slot];
    if (p.next[frame_slot] == buffers.size())
    {
        VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = p.pool[frame_slot];
        alloc.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(vk, &alloc, &buffer) != VK_SUCCESS || !buffer)
        {
            return VK_NULL_HANDLE;
        }
        buffers.push_back(buffer);
    }
    const VkCommandBuffer buffer = buffers[p.next[frame_slot]++];
    VkCommandBufferInheritanceRenderingInfo rendering = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO };
    rendering.colorAttachmentCount = llmin(target.color_count, 4u);
    rendering.pColorAttachmentFormats = target.colors;
    rendering.depthAttachmentFormat = target.depth;
    rendering.stencilAttachmentFormat = target.stencil;
    rendering.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo inheritance = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
    inheritance.pNext = &rendering;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin.pInheritanceInfo = &inheritance;
    return vkBeginCommandBuffer(buffer, &begin) == VK_SUCCESS ? buffer : VK_NULL_HANDLE;
}

bool LLVKRender::endSecondary(VkCommandBuffer buffer)
{
    return buffer && vkEndCommandBuffer(buffer) == VK_SUCCESS;
}

void LLVKRender::destroySecondaryPools()
{
    const VkDevice vk = LLVKDevice::instance().getDevice();
    std::lock_guard<std::mutex> lock(sPoolMutex);
    sPoolsGone.store(true, std::memory_order_release);
    for (SecondaryPools* p : sPools)
    {
        for (U32 s = 0; s < SECONDARY_SLOTS; ++s)
        {
            if (p->pool[s] && vk)
            {
                vkDestroyCommandPool(vk, p->pool[s], nullptr);     // frees its buffers
            }
            p->pool[s] = VK_NULL_HANDLE;
            p->buffers[s].clear();
            p->next[s] = 0;
        }
    }
}
