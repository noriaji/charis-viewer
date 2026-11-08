/**
 * @file llvkqueues.cpp
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

#include "linden_common.h"

#include "llvkqueues.h"

#include "llvkdevice.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace
{
    LLVKQueues::Mode read_mode()
    {
        const char* env = std::getenv("LLRHI_ASYNC_QUEUES");
        if (!env || !*env)
        {
            return LLVKQueues::MODE_OFF;
        }
        std::string value(env);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (value == "verify")
        {
            return LLVKQueues::MODE_VERIFY;
        }
        if (value == "auto")
        {
            return LLVKQueues::MODE_AUTO;
        }
        if (value == "on" || value == "1")
        {
            return LLVKQueues::MODE_ON;
        }
        if (value != "off" && value != "0")
        {
            LL_WARNS("Vulkan") << "LLRHI_ASYNC_QUEUES=" << env << " is not off, verify, auto or on: off" << LL_ENDL;
        }
        return LLVKQueues::MODE_OFF;
    }

    const char* mode_name(LLVKQueues::Mode mode)
    {
        switch (mode)
        {
        case LLVKQueues::MODE_VERIFY: return "verify";
        case LLVKQueues::MODE_AUTO: return "auto";
        case LLVKQueues::MODE_ON: return "on";
        default: return "off";
        }
    }

    void atomic_max(std::atomic<U64>& target, U64 value)
    {
        U64 current = target.load(std::memory_order_relaxed);
        while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }
}

LLVKQueues& LLVKQueues::instance()
{
    static LLVKQueues queues;
    return queues;
}

void LLVKQueues::planQueues(VkPhysicalDevice gpu, U32 graphics_family, std::vector<VkDeviceQueueCreateInfo>& infos)
{
    mMode = read_mode();
    mGraphicsFamily = graphics_family;
    mComputeFamily = U32_MAX;
    mTransferFamily = U32_MAX;
    mPlanned = false;

    U32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (U32 i = 0; i < count; ++i)
    {
        const VkQueueFlags flags = families[i].queueFlags;
        if (i == graphics_family || !families[i].queueCount || (flags & VK_QUEUE_GRAPHICS_BIT))
        {
            continue;
        }
        if (mComputeFamily == U32_MAX && (flags & VK_QUEUE_COMPUTE_BIT))
        {
            mComputeFamily = i;
        }
        else if (mTransferFamily == U32_MAX && (flags & VK_QUEUE_TRANSFER_BIT) && !(flags & VK_QUEUE_COMPUTE_BIT))
        {
            mTransferFamily = i;
        }
    }

    std::ostringstream line;
    line << "async queues (research/200): " << mode_name(mMode) << "; graphics family " << graphics_family << " ("
         << (graphics_family < count ? families[graphics_family].queueCount : 0) << " queues), compute family ";
    if (mComputeFamily != U32_MAX)
    {
        const VkExtent3D& granularity = families[mComputeFamily].minImageTransferGranularity;
        line << mComputeFamily << " (" << families[mComputeFamily].queueCount << " queues, transfer granularity " << granularity.width
             << "x" << granularity.height << "x" << granularity.depth << ")";
    }
    else
    {
        line << "none";
    }
    line << ", transfer family ";
    if (mTransferFamily != U32_MAX)
    {
        line << mTransferFamily << " (" << families[mTransferFamily].queueCount << " queues, not used yet)";
    }
    else
    {
        line << "none";
    }
    if (mMode != MODE_OFF && mComputeFamily == U32_MAX)
    {
        line << "; no compute-only family: folded into the graphics queue";
    }
    LL_INFOS("Vulkan") << line.str() << LL_ENDL;

    if (mMode == MODE_OFF || mComputeFamily == U32_MAX)
    {
        return;
    }
    VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = mComputeFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &mComputePriority;     // under the graphics queue's 1.0
    infos.push_back(queue);
    mPlanned = true;
}

bool LLVKQueues::init(VkDevice device)
{
    mActive = false;
    if (!mPlanned || !device)
    {
        return false;
    }
    mDevice = device;
    vkGetDeviceQueue(device, mComputeFamily, 0, &mComputeQueue);

    VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphore.pNext = &type;
    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = mComputeFamily;
    bool made = mComputeQueue && vkCreateSemaphore(device, &semaphore, nullptr, &mGraphicsTimeline) == VK_SUCCESS &&
                vkCreateSemaphore(device, &semaphore, nullptr, &mComputeTimeline) == VK_SUCCESS &&
                vkCreateCommandPool(device, &pool, nullptr, &mComputePool) == VK_SUCCESS;
    if (made)
    {
        VkCommandBuffer buffers[COMPUTE_RING] = {};
        VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = mComputePool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = COMPUTE_RING;
        made = vkAllocateCommandBuffers(device, &alloc, buffers) == VK_SUCCESS;
        for (U32 i = 0; made && i < COMPUTE_RING; ++i)
        {
            mRing[i] = Ring();
            mRing[i].cmd = buffers[i];
        }
    }
    if (!made)
    {
        LL_WARNS("Vulkan") << "async queues (research/200): the compute queue's objects could not be made; folded into the graphics queue"
                           << LL_ENDL;
        shutdown();
        return false;
    }
    mFamilies[0] = mGraphicsFamily;
    mFamilies[1] = mComputeFamily;
    mNextRing = 0;
    mComputeNext = 1;
    mHostWaited.store(0, std::memory_order_relaxed);
    mComputeSubmitted.store(0, std::memory_order_relaxed);
    mComputeBroken.store(false, std::memory_order_relaxed);
    mGraphicsPublished.store(0, std::memory_order_relaxed);
    mGraphicsWait.store(0, std::memory_order_relaxed);
    mGraphicsCovered = 0;
    // A frame's submission has 2 batches (the frame, the other windows) and
    // the trailing wait; a few semaphores each.
    mBatches.reserve(4);
    mWaits.reserve(8);
    mWaitStages.reserve(8);
    mWaitValues.reserve(8);
    mSignals.reserve(8);
    mSignalValues.reserve(8);
    mActive = true;
    LL_INFOS("Vulkan") << "async queues (research/200): compute queue of family " << mComputeFamily << " in use (" << mode_name(mMode)
                       << ")" << LL_ENDL;
    return true;
}

void LLVKQueues::shutdown()
{
    mActive = false;
    if (mDevice)
    {
        if (mComputePool)
        {
            vkDestroyCommandPool(mDevice, mComputePool, nullptr);     // frees the ring's command buffers
        }
        if (mGraphicsTimeline)
        {
            vkDestroySemaphore(mDevice, mGraphicsTimeline, nullptr);
        }
        if (mComputeTimeline)
        {
            vkDestroySemaphore(mDevice, mComputeTimeline, nullptr);
        }
    }
    mComputePool = VK_NULL_HANDLE;
    mGraphicsTimeline = VK_NULL_HANDLE;
    mComputeTimeline = VK_NULL_HANDLE;
    mComputeQueue = VK_NULL_HANDLE;
    for (Ring& ring : mRing)
    {
        ring = Ring();
    }
    mDevice = VK_NULL_HANDLE;
}

bool LLVKQueues::shareWithCompute(VkImageCreateInfo& info) const
{
    if (!mActive)
    {
        return false;
    }
    info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = 2;
    info.pQueueFamilyIndices = mFamilies;
    return true;
}

bool LLVKQueues::beginCompute(Batch& batch)
{
    batch = Batch();
    if (!computeUsable())
    {
        return false;
    }
    Ring& ring = mRing[mNextRing];
    if (ring.open || ring.value > computeCompleted())
    {
        mRingFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(ring.cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(ring.cmd, &begin) != VK_SUCCESS)
    {
        return false;
    }
    // research/200 H7: the ring's buffers and the users' per-batch memory are
    // used again only after their last submission completed (a host wait);
    // this orders the new batch's accesses after every earlier one on the
    // queue as well.
    VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(ring.cmd, &dependency);
    ring.open = true;
    batch.cmd = ring.cmd;
    batch.index = mNextRing;
    mNextRing = (mNextRing + 1) % COMPUTE_RING;
    return true;
}

void LLVKQueues::abandonCompute(Batch& batch)
{
    if (batch.index < COMPUTE_RING && mRing[batch.index].open)
    {
        vkEndCommandBuffer(mRing[batch.index].cmd);     // reset at its next begin
        mRing[batch.index].open = false;
    }
    batch = Batch();
}

U64 LLVKQueues::submitCompute(Batch& batch)
{
    if (batch.index >= COMPUTE_RING || !mRing[batch.index].open)
    {
        batch = Batch();
        return 0;
    }
    Ring& ring = mRing[batch.index];
    batch = Batch();
    ring.open = false;
    VkResult result = vkEndCommandBuffer(ring.cmd);
    const U64 value = mComputeNext;
    if (result == VK_SUCCESS)
    {
        // Every graphics submission published so far (research/200 H1): what
        // they wrote is what this batch reads.
        const U64 graphics = mGraphicsPublished.load(std::memory_order_acquire);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timeline.waitSemaphoreValueCount = graphics ? 1 : 0;
        timeline.pWaitSemaphoreValues = &graphics;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = graphics ? 1 : 0;
        submit.pWaitSemaphores = &mGraphicsTimeline;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &ring.cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &mComputeTimeline;
        std::lock_guard<std::mutex> lock(mComputeMutex);
        result = vkQueueSubmit(mComputeQueue, 1, &submit, VK_NULL_HANDLE);
    }
    if (result != VK_SUCCESS)
    {
        // Nothing of it runs and its value is not published: nobody waits for
        // it. The users fall back to the graphics queue for the session.
        if (!mComputeBroken.exchange(true, std::memory_order_relaxed))
        {
            LL_WARNS("Vulkan") << "async queues (research/200): a compute submission failed (VkResult " << (S32)result
                               << "); the compute queue is not used again this session" << LL_ENDL;
        }
        if (result == VK_ERROR_DEVICE_LOST)
        {
            LLVKDevice::instance().reportDeviceLost();
        }
        return 0;
    }
    ring.value = value;
    ++mComputeNext;
    mComputeSubmitted.store(value, std::memory_order_release);
    mComputeSubmits.fetch_add(1, std::memory_order_relaxed);
    return value;
}

U64 LLVKQueues::computeCompleted()
{
    const U64 waited = mHostWaited.load(std::memory_order_relaxed);
    if (!mActive)
    {
        return waited;
    }
    U64 now = 0;
    if (vkGetSemaphoreCounterValue(mDevice, mComputeTimeline, &now) != VK_SUCCESS || now <= waited)
    {
        return waited;      // (a lost device: nothing completes any more)
    }
    // A host wait, already satisfied, so that what the batches wrote is the
    // host's to read (a counter query alone is not a wait).
    VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1;
    wait.pSemaphores = &mComputeTimeline;
    wait.pValues = &now;
    if (vkWaitSemaphores(mDevice, &wait, 0) != VK_SUCCESS)
    {
        return waited;
    }
    mHostWaited.store(now, std::memory_order_relaxed);
    return now;
}

void LLVKQueues::graphicsWaitsFor(U64 value)
{
    if (!mActive || !value)
    {
        return;
    }
    if (value > mComputeSubmitted.load(std::memory_order_acquire))
    {
        // Never a value not submitted yet (a wait before its signal could hang the graphics queue).
        LL_WARNS_ONCE("Vulkan") << "async queues (research/200): a graphics wait for a compute value not submitted yet was dropped"
                                << LL_ENDL;
        return;
    }
    atomic_max(mGraphicsWait, value);
}

VkResult LLVKQueues::submitGraphics(VkQueue queue, const VkSubmitInfo* infos, U32 count, VkFence fence)
{
    const U64 value = mGraphicsPublished.load(std::memory_order_relaxed) + 1;  // the only writer holds the graphics queue
    const U64 wait = mGraphicsWait.load(std::memory_order_acquire);
    const U64 submitted = mComputeSubmitted.load(std::memory_order_acquire);
    const bool trail = fence && submitted > std::max(wait, mGraphicsCovered);

    mBatches.clear();
    if (count)
    {
        mBatches.assign(infos, infos + count);
    }
    else
    {
        mBatches.push_back({ VK_STRUCTURE_TYPE_SUBMIT_INFO });
    }
    VkSubmitInfo& first = mBatches[0];

    // The first batch: the caller's waits, then the compute timeline's
    // (graphicsWaitsFor: at once when reached); the caller's signals, then the
    // graphics timeline's. The values of binary semaphores are ignored.
    mWaits.assign(first.pWaitSemaphores, first.pWaitSemaphores + first.waitSemaphoreCount);
    mWaitStages.assign(first.pWaitDstStageMask, first.pWaitDstStageMask + first.waitSemaphoreCount);
    mWaitValues.assign(first.waitSemaphoreCount, 0);
    if (wait)
    {
        mWaits.push_back(mComputeTimeline);
        mWaitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        mWaitValues.push_back(wait);
    }
    mSignals.assign(first.pSignalSemaphores, first.pSignalSemaphores + first.signalSemaphoreCount);
    mSignalValues.assign(first.signalSemaphoreCount, 0);
    mSignals.push_back(mGraphicsTimeline);
    mSignalValues.push_back(value);

    VkTimelineSemaphoreSubmitInfo& timeline = mTimelineInfo[0];
    timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timeline.pNext = first.pNext;
    timeline.waitSemaphoreValueCount = (U32)mWaitValues.size();
    timeline.pWaitSemaphoreValues = mWaitValues.data();
    timeline.signalSemaphoreValueCount = (U32)mSignalValues.size();
    timeline.pSignalSemaphoreValues = mSignalValues.data();
    first.pNext = &timeline;
    first.waitSemaphoreCount = (U32)mWaits.size();
    first.pWaitSemaphores = mWaits.empty() ? nullptr : mWaits.data();
    first.pWaitDstStageMask = mWaitStages.empty() ? nullptr : mWaitStages.data();
    first.signalSemaphoreCount = (U32)mSignals.size();
    first.pSignalSemaphores = mSignals.data();

    if (trail)
    {
        // research/200 3.2: the fence also waits for the compute work
        // submitted before this submission (an empty batch; the frame's own
        // batches do not wait for it).
        mTrailValue = submitted;
        VkTimelineSemaphoreSubmitInfo& tail_timeline = mTimelineInfo[1];
        tail_timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        tail_timeline.waitSemaphoreValueCount = 1;
        tail_timeline.pWaitSemaphoreValues = &mTrailValue;
        static const VkPipelineStageFlags all_commands = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo tail = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        tail.pNext = &tail_timeline;
        tail.waitSemaphoreCount = 1;
        tail.pWaitSemaphores = &mComputeTimeline;
        tail.pWaitDstStageMask = &all_commands;
        mBatches.push_back(tail);   // (first is not used past this point: the vector may move)
    }

    const VkResult result = vkQueueSubmit(queue, (U32)mBatches.size(), mBatches.data(), fence);
    if (result == VK_SUCCESS)
    {
        mGraphicsPublished.store(value, std::memory_order_release);
        mGraphicsCovered = std::max(mGraphicsCovered, trail ? submitted : wait);
        mGraphicsSubmits.fetch_add(1, std::memory_order_relaxed);
        if (trail)
        {
            mTrailingWaits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // A failed submission changed no semaphore: value is signaled by the next one.
    return result;
}

std::string LLVKQueues::report()
{
    std::ostringstream out;
    out << "async queues (research/200): " << mode_name(mMode);
    if (!mActive)
    {
        out << (mMode == MODE_OFF ? "" : ", folded into the graphics queue");
        return out.str();
    }
    out << ", compute family " << mComputeFamily << (mComputeBroken.load(std::memory_order_relaxed) ? " (stopped after a failure)" : "")
        << "; since the start: graphics submits " << mGraphicsSubmits.load(std::memory_order_relaxed) << " (fence waits for compute "
        << mTrailingWaits.load(std::memory_order_relaxed) << "), compute submits " << mComputeSubmits.load(std::memory_order_relaxed)
        << " (ring full " << mRingFull.load(std::memory_order_relaxed) << "), compute value "
        << mComputeSubmitted.load(std::memory_order_relaxed) << " submitted, " << mHostWaited.load(std::memory_order_relaxed) << " done";
    return out.str();
}
