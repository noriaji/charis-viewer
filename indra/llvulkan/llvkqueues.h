/**
 * @file llvkqueues.h
 * @brief The compute queue beside the graphics queue, and the timeline semaphores that order them (research/200).
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

#ifndef LL_LLVKQUEUES_H
#define LL_LLVKQUEUES_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// research/200 (stage 6 of research/197): a queue of a compute-only family
// next to the graphics queue, on a GPU that has one (folded into the
// graphics queue otherwise: nothing changes then).
//
// Order between the two (research/200 3.2):
//  - every graphics submission (LLVKDevice::submit) signals the graphics
//    timeline in its first batch; the value is published once the submission
//    went through. A compute batch waits for the last published value: all
//    the graphics work submitted before it.
//  - a compute batch signals the compute timeline; graphicsWaitsFor(value)
//    makes every later graphics submission's first batch wait for it.
//  - a graphics submission with a fence ends with an empty batch that waits
//    for every published compute value no graphics submission waited for yet:
//    once a frame's fence has passed, so has the compute work submitted before
//    the frame, and LLVKMirror::defer keeps its meaning for what compute used.
//  - only published values are waited for (no wait before its signal), so a
//    failed submission leaves nobody waiting.
// What both queues touch is made VK_SHARING_MODE_CONCURRENT (shareWithCompute):
// no queue family ownership transfers.
//
// LLRHI_ASYNC_QUEUES (read once, before the device is made):
//   off (default, also 0): no compute queue, every submission as before.
//   verify: the compute queue is made; its users do their work as before and
//           also on the compute queue, and compare (LLVKTexCompress: BC7).
//   auto: verify until the users' comparisons passed, then on (off on a mismatch).
//   on (also 1): the users' work goes to the compute queue.
class LLVKQueues
{
public:
    enum Mode : U8 { MODE_OFF, MODE_VERIFY, MODE_AUTO, MODE_ON };
    // Compute command buffers in flight at most (a batch per frame).
    static constexpr U32 COMPUTE_RING = 3;

    static LLVKQueues& instance();

    // Before vkCreateDevice: reads the switch, picks the families (one log
    // line) and, when there is a compute-only family and the switch is not
    // off, adds its queue to infos (which holds the graphics queue's).
    void planQueues(VkPhysicalDevice gpu, U32 graphics_family, std::vector<VkDeviceQueueCreateInfo>& infos);
    // After vkCreateDevice: the queue, the timelines, the command pool and its
    // ring. False (logged): folded, the graphics queue does everything as before.
    bool init(VkDevice device);
    // The device is idle (LLVKDevice::shutdown).
    void shutdown();

    Mode mode() const { return mMode; }
    // The timelines are in use (every graphics submission goes through submitGraphics).
    bool active() const { return mActive; }
    // A compute batch may be begun: active, and no compute submission failed.
    bool computeUsable() const { return mActive && !mComputeBroken.load(std::memory_order_relaxed); }

    // An image the compute queue will also use: concurrent between the
    // graphics and compute families (no ownership transfers). The family list
    // lives as long as this object. False (info untouched) while not active.
    bool shareWithCompute(VkImageCreateInfo& info) const;

    // ---- compute batches (render thread) ----
    struct Batch
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        U32 index = U32_MAX;    // in the ring: users keep per-batch resources by it
    };
    // The next command buffer of the ring, begun, when its last submission has
    // completed (a host wait). Its first command is a barrier after every
    // earlier compute submission's memory accesses. False: none free (no
    // stall), or not usable.
    bool beginCompute(Batch& batch);
    // Ends and submits batch: it waits for every graphics submission
    // published so far, and signals the compute timeline. The value, or 0
    // when the submission failed: nothing of it runs, no compute batch is
    // begun again (computeUsable), and the caller drops what it recorded.
    U64 submitCompute(Batch& batch);
    // A begun batch with nothing to submit: ended, its command buffer free again.
    void abandonCompute(Batch& batch);
    // The compute timeline's value reached, made a host wait (what the
    // batches up to it wrote may be read by the host after
    // vmaInvalidateAllocation). Render thread.
    U64 computeCompleted();
    U64 computeSubmitted() const { return mComputeSubmitted.load(std::memory_order_acquire); }

    // Every later graphics submission waits for the compute timeline to reach
    // value (at least) in its first batch, at all stages: what it records
    // reads what that compute batch wrote, or writes what it reads. value must
    // be one submitCompute returned. Any thread.
    void graphicsWaitsFor(U64 value);

    // LLVKDevice::submit, while active, with the graphics queue held: the
    // batches with the timelines added (research/200 3.2). The callers'
    // batches carry no timeline submit info of their own.
    VkResult submitGraphics(VkQueue queue, const VkSubmitInfo* infos, U32 count, VkFence fence);

    // Held for the compute queue's calls; vkDeviceWaitIdle holds it too
    // (after the graphics queue's, never the other way round).
    std::mutex& computeMutex() { return mComputeMutex; }

    // "async queues (research/200): on, compute family 2; since the start: graphics submits n, ..." (totals: any caller).
    std::string report();

private:
    LLVKQueues() = default;

    Mode mMode = MODE_OFF;
    bool mPlanned = false;      // planQueues added the compute queue
    bool mActive = false;       // init made everything
    U32 mGraphicsFamily = 0;
    U32 mComputeFamily = U32_MAX;
    U32 mTransferFamily = U32_MAX;  // logged only (research/200 8.1)
    U32 mFamilies[2] = { 0, 0 };    // shareWithCompute's list
    float mComputePriority = 0.5f;  // planQueues' create info points at it

    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mComputeQueue = VK_NULL_HANDLE;
    VkSemaphore mGraphicsTimeline = VK_NULL_HANDLE;
    VkSemaphore mComputeTimeline = VK_NULL_HANDLE;
    VkCommandPool mComputePool = VK_NULL_HANDLE;
    std::mutex mComputeMutex;

    struct Ring
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        U64 value = 0;          // its last submission's compute value (0: none)
        bool open = false;      // begun, not submitted or abandoned yet
    };
    Ring mRing[COMPUTE_RING];
    U32 mNextRing = 0;

    // Render thread (the compute side).
    U64 mComputeNext = 1;       // the value the next compute submission signals
    std::atomic<U64> mHostWaited{ 0 };  // computeCompleted (written by the render thread; report reads it)
    std::atomic<U64> mComputeSubmitted{ 0 };    // the last compute value submitted (published)
    std::atomic<bool> mComputeBroken{ false };
    // Graphics side: written under the graphics queue's mutex, read by the compute side.
    std::atomic<U64> mGraphicsPublished{ 0 };   // the last graphics value submitted
    std::atomic<U64> mGraphicsWait{ 0 };        // graphicsWaitsFor's largest value
    U64 mGraphicsCovered = 0;   // the largest compute value a graphics submission waited for

    // submitGraphics' arrays, kept (under the graphics queue's mutex): no allocation per submission.
    std::vector<VkSubmitInfo> mBatches;
    std::vector<VkSemaphore> mWaits;
    std::vector<VkPipelineStageFlags> mWaitStages;
    std::vector<U64> mWaitValues;
    std::vector<VkSemaphore> mSignals;
    std::vector<U64> mSignalValues;
    VkTimelineSemaphoreSubmitInfo mTimelineInfo[2] = {};
    U64 mTrailValue = 0;

    // report()
    std::atomic<U64> mGraphicsSubmits{ 0 };
    std::atomic<U64> mTrailingWaits{ 0 };
    std::atomic<U64> mComputeSubmits{ 0 };
    std::atomic<U64> mRingFull{ 0 };
};

#endif // LL_LLVKQUEUES_H
