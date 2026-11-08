/**
 * @file llvkrender.h
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

#ifndef LL_LLVKRENDER_H
#define LL_LLVKRENDER_H

#include "stdtypes.h"
#include "llrhihash.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <string>

namespace LLVKRecorder
{
    struct Stream;
}

// research/208 S2 (section 11). LLRHI_RENDER_THREAD (llrhiframe.h) off: nothing
// here runs (use() says Inline at once). Otherwise the main thread hands the
// thread "jobs": a piece of a pass whose commands it would have recorded
// itself, made of values only (handles, offsets, counts, the words to push:
// no pointer to anything the main thread goes on changing). The thread only
// appends packets to a stream of its own (LLVKRecorder::Fill), which the main
// thread spliced where those commands belong; it calls nothing of Vulkan, takes
// no lock of the backend (LLVKTracker) and reads no state of the GL
// translation. Every LLVKRecorder::finish() (the frame's end, a submit and
// wait in the frame) joins first: the jobs the thread has not taken are
// recorded by the main thread, a running one is waited for, one that failed is
// recorded again by the main thread (its stream emptied). A failure turns the
// thread off for the session.
//
// Who does what
// - Main thread (under the backend lock): use(), post(), join(), takeReport().
// - The thread ("VKRender"): Job::record into the job's stream, and Job::digest.
// - A job posted is not touched by its owner until a join returned (the owner
//   takes a free one: idle()).
namespace LLVKRender
{
    // How the main thread records a job now.
    enum class Use : U8
    {
        Inline,     // itself (the thread is off, failed, or not for this PC)
        Verify,     // itself, and the thread reads the same values to compare (post with verify)
        Thread,     // the thread (post)
    };

    class Job
    {
    public:
        virtual ~Job() = default;
        // Records the job's commands into the frame's command buffers
        // (through llvkdispatch.h; on the thread, into its stream). Reads the
        // job's values only.
        virtual void record() = 0;
        // FNV-1a of every value record() reads (verify).
        virtual U64 digest() const = 0;
        // What it is, for the log ("expand", "draws").
        virtual const char* what() const = 0;

        // Free for its owner to fill (not posted, or joined).
        bool idle() const { return mState.load(std::memory_order_acquire) == IDLE; }

        // The join's (main thread) and the thread's bookkeeping.
        enum State : int
        {
            IDLE,
            QUEUED,
            RUNNING,
            DONE,
            FAILED,     // the thread could not record it (an exception): the join records it again
            MAIN,       // research/208 S3: queued for the main thread's join (postDeferred); the thread leaves it
        };
        std::atomic<int> mState{ IDLE };
        bool mVerify = false;               // compared only: the main thread recorded it
        LLVKRecorder::Stream* mStream = nullptr;
        U64 mMainDigest = 0;                // verify: what the main thread recorded from
        U64 mThreadDigest = 0;              // verify: what the thread read
        U32 mPosted = 0;                    // the frame's posting order
        U32 mTaken = 0;                     // the order the thread took it in (verify)
        U32 mPass = 0;                      // for the log
        bool mOnMain = false;               // the join recorded it
    };

    Use use();
    // Hands the job to the thread where the main thread stands in the frame's
    // recording: a stream is acquired and spliced here. False: no stream
    // (nothing deferred, splicing off, the main thread fills a stream of its
    // own): the caller records it itself.
    bool post(Job& job);
    // Verify: the main thread recorded the job (from the values digest()
    // reads, mMainDigest); the thread reads them again to compare at the join.
    void postVerify(Job& job);
    // research/208 S3 (verify): as post, but the job is the main thread's:
    // the thread leaves it in the queue and the next join records it, in its
    // place among the others (a job whose values the thread compares follows
    // it with postVerify). False as post.
    bool postDeferred(Job& job);
    // Every job posted so far is recorded (or compared). LLVKRecorder::finish
    // calls it (sJoinRender); also at the present (llrhivkmirror.cpp).
    // research/208 S3: the main thread may also call it in the frame (outside
    // a stream of its own), to take back what the thread holds.
    void join(bool frame_end);
    // research/208 S3: called by the main thread at the end of every join of
    // a frame's end (after its jobs), set once by the scene: what the thread
    // held for the frame goes back to the main thread there.
    void setFrameJoinHook(void (*hook)());
    // research/208 S3: this thread is the render thread (the scene's owner check).
    bool onThread();
    // research/208 S3: the thread is off for the session from now on (a job
    // that cannot be recorded again: its owner's state is not known).
    void failSession(const char* why);
    // Its jobs joined, the thread ends. LLVKRecorder::shutdown calls it first
    // (sEndRender, set when the thread starts); later uses record inline.
    void shutdown();
    // The VKPerf scene line's part (empty while off and unused).
    std::string takeReport(U32 frames);

    // ---- research/208 13.3 (vk-s4a4): secondary command buffers ----------
    // A thread's own: from a command pool per thread and frame slot that only
    // that thread touches, reset by it at its first use in a later frame of
    // the slot (frame_begins changed: LLVKMirror::beginFrame waited for the
    // slot's last frame). For the main thread too (a pass recorded at a join,
    // or without a stream).
    constexpr U32 SECONDARY_SLOTS = 2;      // LLVKMirror::FRAMES_IN_FLIGHT (checked where both are known)
    struct SecondaryTarget
    {
        U32 color_count = 0;
        VkFormat colors[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
        VkFormat depth = VK_FORMAT_UNDEFINED;
        VkFormat stencil = VK_FORMAT_UNDEFINED;     // the depth's format when the scope has a stencil attachment
    };
    // Begun (ONE_TIME_SUBMIT, RENDER_PASS_CONTINUE, inheriting a rendering
    // scope of target's formats and one sample); null: none (no device or
    // memory, the pools gone at shutdown).
    VkCommandBuffer beginSecondary(const SecondaryTarget& target, U32 frame_slot, U64 frame_begins);
    bool endSecondary(VkCommandBuffer buffer);
    // Every thread's pools destroyed (LLVKMirror::shutdown, the device idle); later begins give null.
    void destroySecondaryPools();

    // FNV-1a over values, one at a time (no padding bytes).
    using Digest = LLRHI::Digest;
}

#endif // LL_LLVKRENDER_H
