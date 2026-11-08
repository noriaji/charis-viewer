/**
 * @file llvkmirror.cpp
 * @brief Offscreen Vulkan frame that replays the viewer's GL calls (shadow mode).
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

#include "llvkmirror.h"
#include "llrhi.h"
#include "llvkdevice.h"
#include "llvkswapchain.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    constexpr VkDeviceSize RING_SIZE = 256ull * 1024 * 1024;
    constexpr U32 MAX_SETS = 65536;
    // GPU timing: timestamps a frame may write (marks past that join the last
    // segment; the report counts them). The alpha pool's split by kind of draw
    // (research/54) marks each switch between kinds.
    constexpr U32 MAX_TIMESTAMPS = 4096;
    // Alpha census: draws a frame may count (research/54), and what each query counts
    // (results come in bit order: primitives in, primitives clipped, fragments).
    constexpr U32 MAX_CENSUS_QUERIES = 8192;
    constexpr VkQueryPipelineStatisticFlags CENSUS_STATISTICS = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                                                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    constexpr U32 CENSUS_VALUES = 3;
    // The segment from a submitAndWait's submission to the next command buffer
    // (the GPU mostly idles while the CPU waits): left out of the total.
    const char* const SYNC_MARK = "sync";

    bool same_text(const char* a, const char* b)
    {
        return a == b || (a && b && !strcmp(a, b));
    }

    // Frame cost (LLRHI::sFrameCost): each call adds the time since the last
    // one (or since construction) to a step. Reads no clock while off.
    using CostClock = std::chrono::steady_clock;
    struct CostLap
    {
        explicit CostLap(bool on) : mOn(on), mMark(on ? CostClock::now() : CostClock::time_point()) {}
        void operator()(F64& into)
        {
            if (mOn)
            {
                const CostClock::time_point now = CostClock::now();
                into += std::chrono::duration<F64, std::milli>(now - mMark).count();
                mMark = now;
            }
        }
        // Starts over without counting (the time went to a step timed elsewhere).
        void restart()
        {
            if (mOn)
            {
                mMark = CostClock::now();
            }
        }
        const bool mOn;
        CostClock::time_point mMark;
    };

    // Image barriers kept by an open LLVKMirror::BarrierBatch (research/111 A-BB).
    const bool sBarrierBatch = LLRHI::envFirstChar("LLRHI_BARRIER_BATCH") != '0';
    struct PendingBarriers
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;   // the open batch's (null: none open)
        std::vector<VkImageMemoryBarrier2> barriers;
    };
    thread_local PendingBarriers tPendingBarriers;

    void record_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers, U32 count)
    {
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = count;
        dependency.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // The frame upload ring's bytes written so far made visible to the GPU
    // before a submission. A no-op on HOST_COHERENT memory (what desktop GPUs
    // give this allocation); needed only where the host heap is not coherent.
    void flush_ring(VmaAllocation ring, VkDeviceSize used)
    {
        if (ring && used)
        {
            vmaFlushAllocation(LLVKDevice::instance().getAllocator(), ring, 0, used);
        }
    }

    void flush_pending_barriers()
    {
        PendingBarriers& p = tPendingBarriers;
        if (!p.barriers.empty())
        {
            record_barriers(p.cmd, p.barriers.data(), (U32)p.barriers.size());
            p.barriers.clear();
        }
    }

    // Stage/access masks implied by a layout, for conservative barriers.
    void layout_masks(VkImageLayout layout, VkPipelineStageFlags2& stage, VkAccessFlags2& access)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            // Contents are discarded, but earlier frames may still be reading
            // the image (frames in flight): keep an execution dependency.
            stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; access = 0; break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; access = VK_ACCESS_2_TRANSFER_READ_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; access = VK_ACCESS_2_TRANSFER_WRITE_BIT; break;
        default:
            stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; break;
        }
    }
}

bool LLVKMirror::Target::operator==(const Target& other) const
{
    if (color_count != other.color_count || depth != other.depth || stencil != other.stencil ||
        depth_read_only != other.depth_read_only ||
        width != other.width || height != other.height)
    {
        return false;
    }
    for (U32 i = 0; i < color_count; ++i)
    {
        if (colors[i] != other.colors[i])
        {
            return false;
        }
    }
    return true;
}

void LLVKMirror::imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                              VkImageLayout from, VkImageLayout to, U32 levels, U32 layers)
{
    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    layout_masks(from, barrier.srcStageMask, barrier.srcAccessMask);
    layout_masks(to, barrier.dstStageMask, barrier.dstAccessMask);
    if (from == to)
    {
        // Same layout, still order the previous writes before the next use.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, levels, 0, layers };

    PendingBarriers& pending = tPendingBarriers;
    if (pending.cmd != cmd)
    {
        record_barriers(cmd, &barrier, 1);
        return;
    }
    for (const VkImageMemoryBarrier2& kept : pending.barriers)
    {
        if (kept.image == image)
        {
            flush_pending_barriers();   // its transitions stay in order
            break;
        }
    }
    pending.barriers.push_back(barrier);
}

LLVKMirror::BarrierBatch::BarrierBatch(VkCommandBuffer cmd)
{
    PendingBarriers& pending = tPendingBarriers;
    if (sBarrierBatch && cmd && !pending.cmd)
    {
        pending.cmd = cmd;
        mOpened = true;
    }
}

LLVKMirror::BarrierBatch::~BarrierBatch()
{
    if (mOpened)
    {
        flush_pending_barriers();
        tPendingBarriers.cmd = VK_NULL_HANDLE;
    }
}

LLVKMirror& LLVKMirror::instance()
{
    static LLVKMirror mirror;
    return mirror;
}

bool LLVKMirror::init()
{
    LLVKDevice& device = LLVKDevice::instance();
    if (!device.isReady() || isReady())
    {
        return isReady();
    }
    VkDevice vk = device.getDevice();
    LLVKDispatch::load(vk);
    LL_INFOS("Vulkan") << "image barriers before a draw: " << (sBarrierBatch ? "one call per batch" : "one call each (LLRHI_BARRIER_BATCH=0)")
                       << LL_ENDL;

    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = device.getGraphicsQueueFamily();
    if (vkCreateCommandPool(vk, &pool, nullptr, &mCommandPool) != VK_SUCCESS)
    {
        return false;
    }
    // fix-round71 5d: every object the frame needs is checked; a failure ends
    // the setup here (the caller shows vulkan_unavailable).
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(vk, &pool, nullptr, &mImmediatePool) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Could not create the one-off command pool" << LL_ENDL;
        return false;
    }

    VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fence.flags = 0;
    if (vkCreateFence(vk, &fence, nullptr, &mImmediateFence) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Could not create the one-off fence" << LL_ENDL;
        return false;
    }

    for (Slot& slot : mSlots)
    {
    VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    alloc.commandPool = mCommandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk, &alloc, &slot.cmd) != VK_SUCCESS ||
        vkAllocateCommandBuffers(vk, &alloc, &slot.upload) != VK_SUCCESS ||
        vkAllocateCommandBuffers(vk, &alloc, &slot.windows) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Could not allocate the frame's command buffers" << LL_ENDL;
        return false;
    }

    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(vk, &fence, nullptr, &slot.fence) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Could not create the frame's fence" << LL_ENDL;
        return false;
    }

    VkBufferCreateInfo ring = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ring.size = RING_SIZE;
    ring.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |    // indirect draw commands (research/46)
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;      // the GPU shadow list's per-pass inputs (research/46 G2)
    VmaAllocationCreateInfo host = {};
    // System memory: the CPU-visible VRAM window (BAR) is too small for this.
    host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(device.getAllocator(), &ring, &host, &slot.ring, &slot.ring_memory, &info) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Could not allocate the frame upload ring" << LL_ENDL;
        return false;
    }
    slot.ring_data = (U8*)info.pMappedData;
    mRingSize = RING_SIZE;

    const VkDescriptorPool first_pool = createDescriptorPool();
    if (!first_pool)
    {
        LL_WARNS("Vulkan") << "Could not create the frame's descriptor pool" << LL_ENDL;
        return false;
    }
    slot.pools.push_back(first_pool);
    }
    selectSlot(0);

    // GPU timing (research/53): timestamps need valid bits on the graphics queue.
    U32 families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.getPhysicalDevice(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_properties(families);
    vkGetPhysicalDeviceQueueFamilyProperties(device.getPhysicalDevice(), &families, family_properties.data());
    const U32 family = device.getGraphicsQueueFamily();
    mTimestampBits = family < families ? family_properties[family].timestampValidBits : 0;
    mTimestampPeriod = device.getLimits().timestampPeriod;
    return true;
}

void LLVKMirror::selectSlot(U32 slot)
{
    mSlot = slot;
    Slot& s = mSlots[slot];
    mCommandBuffer = s.cmd;
    mUploadBuffer = s.upload;
    mFence = s.fence;
    mRing = s.ring;
    mRingMemory = s.ring_memory;
    mRingData = s.ring_data;
}

VkDescriptorPool LLVKMirror::createDescriptorPool()
{
    VkDescriptorPoolSize sizes[] =
    {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_SETS * 2 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SETS * 8 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, MAX_SETS },
        // The scene records' select and expand compute passes (llvkscene.cpp) take sets with storage buffers
        // (research/198; VUID found by the 10-27 verify run: a pool without STORAGE_BUFFER).
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_SETS * 8 },
    };
    VkDescriptorPoolCreateInfo descriptors = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    descriptors.maxSets = MAX_SETS;
    descriptors.poolSizeCount = 4;
    descriptors.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(LLVKDevice::instance().getDevice(), &descriptors, nullptr, &pool) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;  // callers treat a null pool as no sets (fix-round71 5d)
    }
    return pool;
}

void LLVKMirror::waitAll()
{
    VkDevice device = LLVKDevice::instance().getDevice();
    for (Slot& slot : mSlots)
    {
        if (slot.fence && !slot.unsubmitted)    // nothing will signal an unsubmitted slot's fence
        {
            vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        }
    }
}

void LLVKMirror::stopPresentThread()
{
    std::unique_lock<std::mutex> lock(mPresentMutex);
    mPresentThreadStopped = true;   // research/179 6.6 4: from here the main window presents on the frame's thread again
    if (mPresentThreadRunning)
    {
        mPresentStop = true;
        mPresentWake.notify_one();
        mPresentDone.wait(lock, [this]() { return !mPresentThreadRunning; });
    }
}

void LLVKMirror::shutdown()
{
    VkDevice device = LLVKDevice::instance().getDevice();
    if (!device)
    {
        return;
    }
    LLVKRecorder::shutdown();
    stopPresentThread();
    // With the queue mutex, as every submit and present takes it (vkDeviceWaitIdle
    // needs all queues externally synchronized). The mutex is only held inside
    // LLVKDevice's own short calls, so nothing here already holds it.
    LLVKDevice::instance().waitIdle();
    for (U32 i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        selectSlot(i);
        runDeferred();
    }
    destroyTarget();
    // research/156 A-DS: after the wait and the deferred work (retired pools
    // went with it), the persistent pools still in use.
    for (VkDescriptorPool pool : mPersistentPools)
    {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    mPersistentPools.clear();
    mPersistentIndex = 0;
    for (Slot& slot : mSlots)
    {
        if (slot.ring)
        {
            vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), slot.ring, slot.ring_memory);
        }
        for (VkDescriptorPool pool : slot.pools)
        {
            vkDestroyDescriptorPool(device, pool, nullptr);
        }
        if (slot.fence)
        {
            vkDestroyFence(device, slot.fence, nullptr);
        }
        if (slot.queries)
        {
            vkDestroyQueryPool(device, slot.queries, nullptr);
        }
        if (slot.census)
        {
            vkDestroyQueryPool(device, slot.census, nullptr);
        }
        slot = Slot();
    }
    selectSlot(0);
    if (mImmediateFence)
    {
        vkDestroyFence(device, mImmediateFence, nullptr);
        mImmediateFence = VK_NULL_HANDLE;
    }
    if (mCommandPool)
    {
        vkDestroyCommandPool(device, mCommandPool, nullptr);
        vkDestroyCommandPool(device, mImmediatePool, nullptr);
        mCommandPool = mImmediatePool = VK_NULL_HANDLE;
        mCommandBuffer = VK_NULL_HANDLE;
    }
}

bool LLVKMirror::createTarget(U32 width, U32 height)
{
    ++mDefaultTargetSerial;     // research/154 W2: defaultTarget() changes from here
    VmaAllocator allocator = LLVKDevice::instance().getAllocator();
    VkDevice device = LLVKDevice::instance().getDevice();
    VmaAllocationCreateInfo gpu = {};
    gpu.usage = VMA_MEMORY_USAGE_AUTO;

    VkImageCreateInfo image = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = COLOR_FORMAT;
    image.extent = { width, height, 1 };
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateImage(allocator, &image, &gpu, &mColor, &mColorMemory, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = mColor;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = COLOR_FORMAT;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device, &view, nullptr, &mColorView) != VK_SUCCESS)
    {
        mColorView = VK_NULL_HANDLE;    // the caller destroys what was made (fix-round71 5d)
        return false;
    }

    image.format = DEPTH_FORMAT;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vmaCreateImage(allocator, &image, &gpu, &mDepth, &mDepthMemory, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    view.image = mDepth;
    view.format = DEPTH_FORMAT;
    view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device, &view, nullptr, &mDepthView) != VK_SUCCESS)
    {
        mDepthView = VK_NULL_HANDLE;
        return false;
    }

    VkBufferCreateInfo buffer = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer.size = (VkDeviceSize)width * height * 4;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo host = {};
    host.usage = VMA_MEMORY_USAGE_AUTO;
    host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (vmaCreateBuffer(allocator, &buffer, &host, &mReadback, &mReadbackMemory, nullptr) != VK_SUCCESS)
    {
        return false;
    }

    mWidth = width;
    mHeight = height;
    return true;
}

void LLVKMirror::destroyTarget()
{
    ++mDefaultTargetSerial;     // research/154 W2
    VmaAllocator allocator = LLVKDevice::instance().getAllocator();
    VkDevice device = LLVKDevice::instance().getDevice();
    if (mColorView)
    {
        vkDestroyImageView(device, mColorView, nullptr);
        mColorView = VK_NULL_HANDLE;
    }
    if (mColor)
    {
        vmaDestroyImage(allocator, mColor, mColorMemory);
        mColor = VK_NULL_HANDLE;
        mColorMemory = nullptr;
    }
    if (mDepthView)
    {
        vkDestroyImageView(device, mDepthView, nullptr);
        mDepthView = VK_NULL_HANDLE;
    }
    if (mDepth)
    {
        vmaDestroyImage(allocator, mDepth, mDepthMemory);
        mDepth = VK_NULL_HANDLE;
        mDepthMemory = nullptr;
    }
    if (mReadback)
    {
        vmaDestroyBuffer(allocator, mReadback, mReadbackMemory);
        mReadback = VK_NULL_HANDLE;
        mReadbackMemory = nullptr;
    }
    mWidth = mHeight = 0;
}

LLVKMirror::Target LLVKMirror::defaultTarget() const
{
    Target target;
    target.colors[0] = mColorView;
    target.color_formats[0] = COLOR_FORMAT;
    target.color_count = 1;
    target.depth = mDepthView;
    target.depth_format = DEPTH_FORMAT;
    target.stencil = true;
    target.width = mWidth;
    target.height = mHeight;
    return target;
}

void LLVKMirror::runDeferred()
{
    // Swapped out first: a destroy may defer again (into the slot's emptied
    // list). The two lists trade their memory instead of one being freed
    // and the other allocated anew every frame (fix-pipeleak).
    std::vector<std::function<void()>>& deferred = mRunningDeferred;
    deferred.clear();   // left over only when a destroy threw: dropped, never run twice (as the local list was before)
    deferred.swap(mSlots[mSlot].deferred);
    for (auto& destroy : deferred)
    {
        destroy();
    }
    deferred.clear();
}

void LLVKMirror::defer(std::function<void()> destroy)
{
    if (mRecording)
    {
        // Done once this frame completes (it finishes after every earlier frame).
        mSlots[mSlot].deferred.push_back(std::move(destroy));
    }
    else
    {
        waitAll();
        destroy();
    }
}

// A failed submit or fence wait: log each result once. After VK_ERROR_DEVICE_LOST
// (the GPU faulted) nothing renders again: the device is marked lost, and the
// present hook tells the user and ends the viewer (fix-submitfail C).
void LLVKMirror::reportResult(VkResult result, const char* call)
{
    if (result == VK_SUCCESS || result == VK_TIMEOUT)
    {
        return;
    }
    static std::vector<VkResult> reported;
    if (std::find(reported.begin(), reported.end(), result) == reported.end())
    {
        reported.push_back(result);
        LL_WARNS("Vulkan") << call << " returned " << (S32)result
                           << (result == VK_ERROR_DEVICE_LOST ? " (device lost: the GPU stopped; nothing more will render)" : "") << LL_ENDL;
    }
    if (result == VK_ERROR_DEVICE_LOST)
    {
        LLVKDevice::instance().reportDeviceLost();
    }
}

void LLVKMirror::beginFrame(U32 width, U32 height)
{
    mGPUBehind = false;
    if (!isReady() || mRecording || !width || !height)
    {
        return;
    }
    VkDevice device = LLVKDevice::instance().getDevice();
    CostLap lap(LLRHI::sFrameCost);
    selectSlot((mSlot + 1) % FRAMES_IN_FLIGHT);
    if (mSlots[mSlot].unsubmitted)
    {
        // Its frame never reached the GPU (fix-submitfail B): what it deferred
        // waits for the frames before it instead. Still unsignaled, its fence
        // is passed over until it is reset for this frame.
        waitAll();
    }
    else
    {
        mGPUBehind = vkGetFenceStatus(device, mFence) == VK_NOT_READY;
        reportResult(vkWaitForFences(device, 1, &mFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }
    lap(mCost.fence_wait);
    runDeferred();
    lap(mCost.deferred);
    readGPUTimes();
    readCensus();
    mRingHead.store(0, std::memory_order_relaxed);
    for (VkDescriptorPool pool : mSlots[mSlot].pools)
    {
        vkResetDescriptorPool(device, pool, 0);
    }
    mSlots[mSlot].pool_index = 0;
    lap(mCost.reset);

    if (width != mWidth || height != mHeight)
    {
        const bool failed_before = width == mFailedWidth && height == mFailedHeight;
        // fix-round100: after a failure, any size waits for the retry (not just the one that failed:
        // a window frame being dragged, or sizes back and forth, would wait for the GPU and try again
        // on each new size). Only a smaller one is tried at once: it needs less memory, so it may fit.
        const bool smaller = mFailedWidth && width <= mFailedWidth && height <= mFailedHeight && !failed_before;
        if (mFailedWidth && !smaller && std::chrono::steady_clock::now() < mTargetRetryAt)
        {
            return;     // as below on the failure: no target, the frame is skipped (fix-round95 B)
        }
        ++mCost.resizes;
        char event[64];
        snprintf(event, sizeof(event), "window target %ux%u -> %ux%u", mWidth, mHeight, width, height);
        LLVKDevice::instance().noteEvent(event);    // research/109
        waitAll();  // the other frame may still use the old target
        destroyTarget();
        if (!createTarget(width, height))
        {
            if (!failed_before)     // once per size, not on each later try (fix-round95 B)
            {
                LL_WARNS("Vulkan") << "Could not create " << width << "x" << height << " mirror target; trying again every "
                                   << TARGET_RETRY_SECONDS.count() << " s" << LL_ENDL;
            }
            destroyTarget();
            mFailedWidth = width;
            mFailedHeight = height;
            mTargetRetryAt = std::chrono::steady_clock::now() + TARGET_RETRY_SECONDS;
            return;
        }
        if (failed_before)
        {
            LL_INFOS("Vulkan") << "Mirror target of " << width << "x" << height << " made after an earlier failure" << LL_ENDL;
        }
        mFailedWidth = mFailedHeight = 0;
        lap(mCost.resize);
    }

    vkResetFences(device, 1, &mFence);
    mSlots[mSlot].unsubmitted = false;  // this frame's submission (or the one in its place) signals it
    ++mFrameBegins;
    beginCommandBuffers();
    beginGPUTiming();
    beginCensus();

    // Previous contents are not kept between frames, as with a GL swap. As
    // imageBarrier from UNDEFINED (all earlier commands), plus the earlier
    // frames' writes of these images made available: without them the
    // synchronization validation reports write-after-write at the first draw.
    {
        VkImageMemoryBarrier2 head[2] = { { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 }, { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 } };
        const VkImage images[2] = { mColor, mDepth };
        const VkImageAspectFlags aspects[2] = { VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT };
        const VkImageLayout layouts[2] = { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        for (U32 i = 0; i < 2; ++i)
        {
            layout_masks(VK_IMAGE_LAYOUT_UNDEFINED, head[i].srcStageMask, head[i].srcAccessMask);
            layout_masks(layouts[i], head[i].dstStageMask, head[i].dstAccessMask);
            head[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT;
            head[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            head[i].newLayout = layouts[i];
            head[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            head[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            head[i].image = images[i];
            head[i].subresourceRange = { aspects[i], 0, 1, 0, 1 };
        }
        if (tPendingBarriers.cmd == mCommandBuffer)
        {
            flush_pending_barriers();
        }
        record_barriers(mCommandBuffer, head, 2);
    }
    lap(mCost.begin);
    mColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    mDepthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    mRecording = true;
    ++mScopeEpoch;
    ++mRecordSerial;
}

void LLVKMirror::beginCommandBuffers()
{
    vkResetCommandBuffer(mCommandBuffer, 0);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(mCommandBuffer, &begin);
    vkResetCommandBuffer(mUploadBuffer, 0);
    vkBeginCommandBuffer(mUploadBuffer, &begin);
    if (LLVKRecorder::enabled())
    {
        LLVKRecorder::start(mCommandBuffer, mUploadBuffer);     // the frame's commands go to the recording worker
    }
    {
        // Uploads overwrite buffers earlier frames may still read.
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(mUploadBuffer, &dependency);
    }
}

// After LLVKRecorder::finish() and endRendering(): these commands go directly.
void LLVKMirror::endCommandBuffers()
{
    {
        // What the frame wrote for the CPU (the capture, readbacks, occlusion
        // marks) is visible to it once the fence is waited for.
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(mCommandBuffer, &dependency);
    }

    vkEndCommandBuffer(mCommandBuffer);
    {
        // Everything uploaded is visible to the frame's draws.
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(mUploadBuffer, &dependency);
    }
    vkEndCommandBuffer(mUploadBuffer);
}

void (*LLVKMirror::sBeforeSubmit)() = nullptr;
void (*LLVKMirror::sBeforeUploadEnd)() = nullptr;
void (*LLVKMirror::sBeforeFrameEnd)(VkCommandBuffer) = nullptr;
void (*LLVKMirror::sAfterRecorderFinish)(VkCommandBuffer, bool) = nullptr;

void LLVKMirror::submitFailed(VkResult result, const char* what)
{
    // fix-round6 1: a submission that fails runs nothing, but whoever
    // recorded into it already counts its work as done: the GPU list's rows
    // and tombstones (LLVKGPUList), the vertex pools' placements
    // (LLVKAttribPool, llvkgpudriven), BC7 results and the tracker's image
    // layouts. Nothing tells them, and they would draw on with what was
    // never written (a row table that is not zero filled, without
    // robustBufferAccess: indirect draws from garbage, a lost device; images
    // left in another layout than tracked). So any failed submission, of any
    // VkResult, ends the viewer at the next present (exit_if_lost), as one
    // that keeps failing did before. The first result is kept.
    VkResult expected = VK_SUCCESS;
    if (mStuckSubmit.compare_exchange_strong(expected, result, std::memory_order_relaxed))
    {
        LL_WARNS("Vulkan") << "A submission failed (" << what << ", VkResult " << (S32)result
                           << "): what it uploaded and the layouts it changed are lost; the viewer ends at the next present"
                           << LL_ENDL;
    }
}

bool LLVKMirror::submitAndWait()
{
    if (!mRecording)
    {
        return false;
    }
    CostLap lap(LLRHI::sFrameCost);
    ++mCost.sync_submits;
    {
        // research/109: which submission a GPU fault came in (the frame's own submit is not noted).
        static U64 syncs = 0;
        char event[64];
        snprintf(event, sizeof(event), "in-frame submit and wait #%llu (readback or OIT check)", (unsigned long long)++syncs);
        LLVKDevice::instance().noteEvent(event);
    }
    if (sBeforeSubmit)
    {
        sBeforeSubmit();
    }
    if (sBeforeUploadEnd)
    {
        sBeforeUploadEnd();
    }
    LLVKRecorder::finish();     // everything so far is in the command buffers
    if (sAfterRecorderFinish)
    {
        sAfterRecorderFinish(mUploadBuffer, false);     // research/209 E4: the scene's queued work, before this submission
    }
    endRendering();
    // GPU timing: the wait gets a segment of its own, then the open one goes on.
    const bool timed = mSlots[mSlot].timed;
    const Slot::Mark resume = timed ? mSlots[mSlot].marks.back() : Slot::Mark{ nullptr, nullptr };
    if (timed)
    {
        gpuMark(nullptr, SYNC_MARK);
    }
    endCommandBuffers();
    VkCommandBuffer buffers[2] = { mUploadBuffer, mCommandBuffer };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = buffers;
    VkDevice device = LLVKDevice::instance().getDevice();
    // After a failed submission (submitFailed) nothing more is submitted:
    // what was recorded since may draw on what it lost. Dropped here.
    const VkResult stuck = stuckSubmitResult();
    flush_ring(mRingMemory, mRingHead.load(std::memory_order_relaxed));
    const VkResult result = stuck != VK_SUCCESS ? stuck : LLVKDevice::instance().submit(submit, mFence);
    if (stuck == VK_SUCCESS)
    {
        reportResult(result, "vkQueueSubmit (in-frame submit and wait)");
        if (result != VK_SUCCESS)
        {
            submitFailed(result, "in-frame submit and wait");
        }
    }
    VkResult waited = VK_ERROR_UNKNOWN;
    if (result == VK_SUCCESS)
    {
        waited = vkWaitForFences(device, 1, &mFence, VK_TRUE, UINT64_MAX);
        reportResult(waited, "vkWaitForFences");
    }
    // The frame goes on in fresh command buffers; endFrame submits the rest with the same fence.
    vkResetFences(device, 1, &mFence);
    beginCommandBuffers();
    if (timed)
    {
        gpuMark(resume.scope, resume.name);
    }
    mCurrent = Target();
    ++mScopeEpoch;
    ++mRecordSerial;
    lap(mCost.sync_ms);
    return result == VK_SUCCESS && waited == VK_SUCCESS;
}

VkCommandBuffer LLVKMirror::beginRendering(const Target& target)
{
    ++mRecordSerial;
    if (!mRecording)
    {
        return VK_NULL_HANDLE;
    }
    if (mRendering && mCurrent == target && !mCurrentSecondary)
    {
        return mCommandBuffer;
    }
    endRendering();
    ++mRenderScopes;

    VkRenderingAttachmentInfo colors[4] = {};
    for (U32 i = 0; i < target.color_count; ++i)
    {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = target.colors[i];
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depth.imageView = target.depth;
    depth.imageLayout = target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    // Read-only: nothing is written (depth and stencil writes are off, VUID
    // 06886/06887), so no store either; the contents stay as they were and the
    // depth sampled in the same scope is not seen as written (Vulkan 1.3 core).
    // fix-round24 used STORE_OP_NONE for a read-only depth; the sea flickered after it (2026-10-22), so STORE again until the cause is known
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo info = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    info.renderArea = { { 0, 0 }, { target.width, target.height } };
    info.layerCount = 1;
    info.colorAttachmentCount = target.color_count;
    info.pColorAttachments = colors;
    info.pDepthAttachment = target.depth ? &depth : nullptr;
    info.pStencilAttachment = target.depth && target.stencil ? &depth : nullptr;
    info.flags = mCurrentSecondary ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0;
    vkCmdBeginRendering(mCommandBuffer, &info);
    mRendering = true;
    mCurrent = target;
    ++mScopeEpoch;
    return mCommandBuffer;
}

VkCommandBuffer LLVKMirror::beginSecondaryRendering(const Target& target)
{
    if (!mRecording)
    {
        return VK_NULL_HANDLE;
    }
    endRendering();     // never the open scope: its contents are inline
    mCurrentSecondary = true;
    const VkCommandBuffer cmd = beginRendering(target);
    if (!cmd)
    {
        mCurrentSecondary = false;
    }
    return cmd;
}

void LLVKMirror::executeLater(const std::atomic<VkCommandBuffer>* buffer, const std::atomic<bool>* replayed)
{
    ++mRecordSerial;
    if (mRendering && mCurrentSecondary)
    {
        ++LLVKDispatch::counts()[LLVKDispatch::C_CmdExecuteCommands];
        LLVKRecorder::CmdExecuteLater(mCommandBuffer, buffer, replayed);
    }
}

void LLVKMirror::endRendering()
{
    if (mRendering)
    {
        vkCmdEndRendering(mCommandBuffer);
        mRendering = false;
        mCurrentSecondary = false;
        ++mScopeEpoch;
    }
}

VkCommandBuffer LLVKMirror::transferCommands()
{
    ++mRecordSerial;
    endRendering();
    return mCommandBuffer;
}

void LLVKMirror::record(const std::function<void(VkCommandBuffer)>& commands)
{
    ++mRecordSerial;
    if (mRecording)
    {
        commands(transferCommands());
    }
    else
    {
        immediate(commands);
    }
}

bool LLVKMirror::allocate(VkDeviceSize size, VkDeviceSize alignment, VkBuffer& buffer, VkDeviceSize& offset, void*& data)
{
    // The same arithmetic as one thread doing head = start + size, made atomic:
    // a failed request leaves the head where it was.
    alignment = llmax(alignment, (VkDeviceSize)1);  // 0 from a direct caller: no division by zero (fix-round71 5c)
    VkDeviceSize head = mRingHead.load(std::memory_order_relaxed);
    VkDeviceSize start;
    do
    {
        start = (head + alignment - 1) / alignment * alignment;
        if (start + size > mRingSize)
        {
            if (!mRingWarned.exchange(true, std::memory_order_relaxed))
            {
                LL_WARNS("Vulkan") << "Frame upload ring exhausted (" << mRingSize / (1024 * 1024) << " MB); draws dropped this frame" << LL_ENDL;
            }
            return false;
        }
    } while (!mRingHead.compare_exchange_weak(head, start + size, std::memory_order_relaxed));
    buffer = mRing;
    offset = start;
    data = mRingData + start;
    return true;
}

bool LLVKMirror::UploadBlock::allocate(VkDeviceSize size, VkDeviceSize alignment, VkBuffer& buffer, VkDeviceSize& offset, void*& data)
{
    LLVKMirror& mirror = LLVKMirror::instance();
    alignment = llmax(alignment, (VkDeviceSize)1);
    if (mFrame != mirror.frameBegins())
    {
        mFrame = mirror.frameBegins();
        mBuffer = VK_NULL_HANDLE;
        mHead = mEnd = 0;
    }
    VkDeviceSize start = (mHead + alignment - 1) / alignment * alignment;
    if (!mBuffer || start + size > mEnd)
    {
        // A new block (the rest of this one is left unused). Requests larger
        // than a block get one of their own size.
        const VkDeviceSize want = llmax(BLOCK_SIZE, size + alignment);
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize at = 0;
        void* mapped = nullptr;
        if (!mirror.allocate(want, llmax(alignment, BLOCK_ALIGNMENT), ring, at, mapped))
        {
            return false;
        }
        mBuffer = ring;
        mData = (U8*)mapped - at;
        mHead = at;
        mEnd = at + want;
        start = (mHead + alignment - 1) / alignment * alignment;
    }
    mHead = start + size;
    buffer = mBuffer;
    offset = start;
    data = mData + start;
    return true;
}

VkDescriptorSet LLVKMirror::allocateSet(VkDescriptorSetLayout layout)
{
    // A full pool moves on to the slot's next one (created on demand), so busy frames never run out.
    Slot& slot = mSlots[mSlot];
    ++mSetsAllocated;
    VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (slot.pool_index < slot.pools.size() || slot.pool_index < 64)
    {
        if (slot.pool_index == slot.pools.size())
        {
            VkDescriptorPool pool = createDescriptorPool();
            if (!pool)
            {
                return VK_NULL_HANDLE;
            }
            slot.pools.push_back(pool);
        }
        info.descriptorPool = slot.pools[slot.pool_index];
        VkResult result = vkAllocateDescriptorSets(LLVKDevice::instance().getDevice(), &info, &set);
        if (result == VK_SUCCESS)
        {
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            return VK_NULL_HANDLE;
        }
        ++slot.pool_index;
    }
    return VK_NULL_HANDLE;
}

VkDescriptorSet LLVKMirror::allocatePersistentSet(VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    while (mPersistentIndex < PERSISTENT_POOLS)
    {
        if (mPersistentIndex == mPersistentPools.size())
        {
            VkDescriptorPool pool = createDescriptorPool();
            if (!pool)
            {
                return VK_NULL_HANDLE;
            }
            mPersistentPools.push_back(pool);
        }
        info.descriptorPool = mPersistentPools[mPersistentIndex];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(LLVKDevice::instance().getDevice(), &info, &set);
        if (result == VK_SUCCESS)
        {
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            return VK_NULL_HANDLE;
        }
        ++mPersistentIndex;
    }
    return VK_NULL_HANDLE;
}

void LLVKMirror::retirePersistentSets()
{
    if (mPersistentPools.empty())
    {
        return;
    }
    std::vector<VkDescriptorPool> pools;
    pools.swap(mPersistentPools);
    mPersistentIndex = 0;
    // The frames recorded so far may still bind their sets: gone after them
    // (the frame before this one completes first).
    defer([pools]()
    {
        const VkDevice device = LLVKDevice::instance().getDevice();
        for (VkDescriptorPool pool : pools)
        {
            vkDestroyDescriptorPool(device, pool, nullptr);
        }
    });
}

void LLVKMirror::immediate(const std::function<void(VkCommandBuffer)>& commands)
{
    VkDevice device = LLVKDevice::instance().getDevice();
    CostLap lap(LLRHI::sFrameCost);
    ++mCost.immediates;
    VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    alloc.commandPool = mImmediatePool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &alloc, &cmd) != VK_SUCCESS || !cmd)
    {
        // Host or device memory full (fix-round95 C): the commands are dropped,
        // as a failed submission drops them, instead of recording into null.
        LL_WARNS_ONCE("Vulkan") << "Could not allocate a one-off command buffer: its commands are dropped" << LL_ENDL;
        return;
    }

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    commands(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    // research/109: a fault in a one-off submission is reported as such.
    // After a failed submission nothing more goes to the GPU (submitFailed).
    const VkResult stuck = stuckSubmitResult();
    flush_ring(mRingMemory, mRingHead.load(std::memory_order_relaxed));
    const VkResult submitted = stuck != VK_SUCCESS ? stuck : LLVKDevice::instance().submit(submit, mImmediateFence);
    if (stuck == VK_SUCCESS)
    {
        reportResult(submitted, "vkQueueSubmit (one-off, outside a frame)");
        if (submitted != VK_SUCCESS)
        {
            submitFailed(submitted, "one-off, outside a frame");
        }
    }
    if (submitted == VK_SUCCESS)    // a failed one signals nothing (fix-submitfail B)
    {
        reportResult(vkWaitForFences(device, 1, &mImmediateFence, VK_TRUE, UINT64_MAX), "vkWaitForFences (one-off, outside a frame)");
        vkResetFences(device, 1, &mImmediateFence);
    }
    vkFreeCommandBuffers(device, mImmediatePool, 1, &cmd);
    lap(mCost.immediate_ms);
}

void LLVKMirror::presentExtraWindow(LLVKSwapchain* swapchain, VkImage image, U32 width, U32 height, VkBuffer probe)
{
    if (mRecording && swapchain && image && width && height)
    {
        // One image per window and frame: the last one. No wait for the
        // present thread here (fix-round71 1): endFrame waits before it
        // acquires from these swapchains again.
        dropExtraWindow(swapchain);
        mExtraWindows.push_back({ swapchain, image, width, height, 0, probe });
    }
}

void LLVKMirror::waitExtraPresents()
{
    std::unique_lock<std::mutex> lock(mPresentMutex);
    mPresentDone.wait(lock, [this]() { return mPresentQueue.empty() && !mPresenting; });
}

void LLVKMirror::presentLoop()
{
    std::vector<ExtraWindow> work;
    std::unique_lock<std::mutex> lock(mPresentMutex);
    for (;;)
    {
        mPresentWake.wait(lock, [this]() { return mPresentStop || !mPresentQueue.empty(); });
        if (mPresentQueue.empty())
        {
            mPresentThreadRunning = false;  // stopping, nothing left
            mPresentDone.notify_all();
            return;
        }
        work.swap(mPresentQueue);
        mPresenting = true;
        lock.unlock();
        // LLVKDevice::present holds the queue: submits wait while the driver presents.
        U32 main_presents = 0;
        F64 main_ms = 0.0;
        for (const ExtraWindow& window : work)
        {
            if (window.main)
            {
                // research/179 6.6 4: timed for the FrameCost line (two clock reads a frame).
                const CostClock::time_point start = CostClock::now();
                window.swapchain->present(window.index);
                main_ms += std::chrono::duration<F64, std::milli>(CostClock::now() - start).count();
                ++main_presents;
            }
            else
            {
                window.swapchain->present(window.index);
            }
        }
        work.clear();
        lock.lock();
        mMainPresents += main_presents;
        mMainPresentMs += main_ms;
        mPresenting = false;
        if (mPresentQueue.empty())
        {
            mPresentDone.notify_all();
        }
    }
}

void LLVKMirror::waitMainPresent()
{
    if (!mMainPresentQueued)
    {
        return;
    }
    CostLap lap(LLRHI::sFrameCost);
    {
        std::unique_lock<std::mutex> lock(mPresentMutex);
        mPresentDone.wait(lock, [this]() { return mPresentQueue.empty() && !mPresenting; });
        mCost.main_presents += mMainPresents;
        mCost.main_present_call += mMainPresentMs;
        mMainPresents = 0;
        mMainPresentMs = 0.0;
    }
    mMainPresentQueued = false;
    lap(mCost.main_present_wait);
}

void LLVKMirror::forgetExtraWindow(LLVKSwapchain* swapchain)
{
    waitExtraPresents();    // the present thread may still be presenting to it
    dropExtraWindow(swapchain);
}

void LLVKMirror::dropExtraWindow(LLVKSwapchain* swapchain)
{
    mExtraWindows.erase(std::remove_if(mExtraWindows.begin(), mExtraWindows.end(),
                                       [swapchain](const ExtraWindow& window) { return window.swapchain == swapchain; }),
                        mExtraWindows.end());
}

void LLVKMirror::abandonFrame(LLVKSwapchain* swapchain)
{
    // A failed submission leaves its semaphores as they were: the acquired
    // images' semaphores stay signaled, and acquire() must not be given one
    // again until it is waited for. An empty batch waits for them and signals
    // the fence after every earlier frame, as the frame would have.
    std::vector<VkSemaphore> waits(mSubmitWaits);
    waits.insert(waits.end(), mWindowWaits.begin(), mWindowWaits.end());
    const std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = (U32)waits.size();
    submit.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    submit.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    const VkResult result = LLVKDevice::instance().submit(submit, mFence);
    reportResult(result, "vkQueueSubmit (empty, in place of a failed frame)");
    Slot& slot = mSlots[mSlot];
    slot.unsubmitted = result != VK_SUCCESS;
    // Its timestamps and counts were never reset or written: not read back.
    slot.timed = false;
    slot.census_on = false;
    LL_WARNS_ONCE("Vulkan") << "A frame's submission failed: the frame is dropped (its uploads and layout changes are lost)" << LL_ENDL;
    if (slot.unsubmitted)
    {
        // The GPU takes no work at all; the acquire semaphores stay signaled
        // and must not be acquired with again: the frame's fence is never
        // waited for. The viewer ends at this present (submitFailed).
        LL_WARNS("Vulkan") << "The empty submission in a failed frame's place failed too (" << (S32)result
                           << "): the frame's fence is not waited for" << LL_ENDL;
        submitFailed(result, "empty, in place of a failed frame");
    }
    // The images acquired for this frame are never presented: the windows'
    // next acquire recreates their swapchains, which gives them back.
    if (swapchain)
    {
        swapchain->dropUnpresented();
    }
    for (const ExtraWindow& window : mExtraPresents)
    {
        window.swapchain->dropUnpresented();
    }
    mExtraPresents.clear();
}

bool LLVKMirror::endFrame(std::vector<U8>* capture, LLVKSwapchain* swapchain)
{
    if (!mRecording)
    {
        mExtraWindows.clear();
        return false;
    }
    CostLap lap(LLRHI::sFrameCost);
    ++mCost.frames;
    if (sBeforeSubmit)
    {
        sBeforeSubmit();
    }
    if (sBeforeUploadEnd)
    {
        sBeforeUploadEnd();
    }
    lap(mCost.before_submit);
    LLVKRecorder::finish(true); // the worker has recorded the frame; the rest is recorded here
    lap(mCost.recorder_wait);
    if (sAfterRecorderFinish)
    {
        sAfterRecorderFinish(mUploadBuffer, true);      // research/209 E4: the scene's queued work, before the submission
    }
    endRendering();
    if (sBeforeFrameEnd)
    {
        sBeforeFrameEnd(mCommandBuffer);
    }
    gpuMark(nullptr, "present");
    lap(mCost.record_end);

    U32 image_index = 0;
    VkSemaphore acquired = VK_NULL_HANDLE;
    if (swapchain)
    {
        waitMainPresent();  // research/179 6.6 4: the last frame's, before this acquire (at once when already waited for)
        lap.restart();      // waitMainPresent timed its wait
    }
    bool presenting = swapchain && swapchain->acquire(mSlot, image_index, acquired);
    lap(mCost.acquire);
    if (capture || presenting)
    {
        imageBarrier(mCommandBuffer, mColor, VK_IMAGE_ASPECT_COLOR_BIT, mColorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
        mColorLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (presenting)
    {
        swapchain->recordBlit(mCommandBuffer, image_index, mColor, mWidth, mHeight);
    }
    if (capture)
    {
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { mWidth, mHeight, 1 };
        vkCmdCopyImageToBuffer(mCommandBuffer, mColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mReadback, 1, &region);
    }

    // The swapchain images the submission waits for and signals: the
    // window's, then the frame's other windows' (research/87).
    mSubmitWaits.clear();
    mSubmitWaitStages.clear();
    mSubmitSignals.clear();
    if (presenting)
    {
        mSubmitWaits.push_back(acquired);
        mSubmitWaitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
        mSubmitSignals.push_back(swapchain->renderDone(image_index));
    }
    mExtraPresents.clear();
    mWindowWaits.clear();
    mWindowWaitStages.clear();
    mWindowSignals.clear();
    lap(mCost.record_end);
    if (!mExtraWindows.empty())
    {
        waitExtraPresents();    // the last frame's: before their swapchains are acquired from again
    }
    // The windows' blits go to a batch of their own, submitted after the
    // frame's: waiting for a window's swapchain image (until the display lets
    // it go) then holds up that batch only, never the frame's own commands,
    // whose transfers would otherwise wait for it from their start (research/105).
    VkCommandBuffer windows_cmd = VK_NULL_HANDLE;
    for (ExtraWindow& window : mExtraWindows)
    {
        // Never waits for an image: a window whose images are all still with
        // the display (presented faster than it shows them, or on a slower
        // monitor) skips this frame and keeps its picture, instead of holding
        // the main window's frame until the display lets one go (research/105).
        VkSemaphore window_acquired = VK_NULL_HANDLE;
        if (!window.swapchain->acquire(mSlot, window.index, window_acquired, 0))
        {
            continue;   // minimized, being recreated or no free image: it keeps its last picture
        }
        if (!windows_cmd)
        {
            windows_cmd = mSlots[mSlot].windows;
            vkResetCommandBuffer(windows_cmd, 0);
            VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(windows_cmd, &begin);
        }
        // After the frame's batch in submission order: this waits for the copy
        // written there.
        imageBarrier(windows_cmd, window.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
        LLVKSwapchain::Probe probe;
        if (window.probe)
        {
            VkBufferImageCopy region = {};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { (S32)window.width / 2, (S32)window.height / 2, 0 };
            region.imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer(windows_cmd, window.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, window.probe, 1, &region);
            probe.buffer = window.probe;
            probe.offset = 4;
        }
        window.swapchain->recordBlit(windows_cmd, window.index, window.image, window.width, window.height, &probe);
        mWindowWaits.push_back(window_acquired);
        mWindowWaitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
        mWindowSignals.push_back(window.swapchain->renderDone(window.index));
        mExtraPresents.push_back(window);
    }
    mExtraWindows.clear();
    if (windows_cmd)
    {
        // The probes' readbacks (LLRHI_DETACH_LOG=3) are for the host.
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(windows_cmd, &dependency);
        vkEndCommandBuffer(windows_cmd);
    }
    lap(mCost.other_windows);

    endGPUTiming();
    endCommandBuffers();
    VkCommandBuffer buffers[2] = { mUploadBuffer, mCommandBuffer };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = buffers;
    submit.waitSemaphoreCount = (U32)mSubmitWaits.size();
    submit.pWaitSemaphores = mSubmitWaits.data();
    submit.pWaitDstStageMask = mSubmitWaitStages.data();
    submit.signalSemaphoreCount = (U32)mSubmitSignals.size();
    submit.pSignalSemaphores = mSubmitSignals.data();
    lap(mCost.record_end);
    VkResult submitted = VK_SUCCESS;
    const VkResult stuck = stuckSubmitResult();
    flush_ring(mRingMemory, mRingHead.load(std::memory_order_relaxed));
    if (stuck != VK_SUCCESS)
    {
        // An earlier submission of this frame (submitAndWait) or between
        // frames (immediate) failed: this frame may draw on what it lost, so
        // it is not submitted but dropped as a failed one (submitFailed).
        submitted = stuck;
    }
    else if (windows_cmd)
    {
        VkSubmitInfo batches[2] = { submit, { VK_STRUCTURE_TYPE_SUBMIT_INFO } };
        VkSubmitInfo& windows = batches[1];
        windows.commandBufferCount = 1;
        windows.pCommandBuffers = &windows_cmd;
        windows.waitSemaphoreCount = (U32)mWindowWaits.size();
        windows.pWaitSemaphores = mWindowWaits.data();
        windows.pWaitDstStageMask = mWindowWaitStages.data();
        windows.signalSemaphoreCount = (U32)mWindowSignals.size();
        windows.pSignalSemaphores = mWindowSignals.data();
        submitted = LLVKDevice::instance().submit(batches, 2, mFence);
    }
    else
    {
        submitted = LLVKDevice::instance().submit(submit, mFence);
    }
    if (stuck == VK_SUCCESS)
    {
        reportResult(submitted, "vkQueueSubmit (end of frame)");
    }
    if (submitted != VK_SUCCESS)
    {
        // fix-round6 1: the first failure ends the viewer at this present
        // (before: only when every frame failed for 5 s); what the frame
        // uploaded is lost and nothing tells its owners.
        submitFailed(submitted, "end of frame");
        // fix-submitfail B: nothing of it runs, so its render-done semaphores
        // are never signaled: no present (it would never show), and the
        // fence is signaled in its place.
        abandonFrame(presenting ? swapchain : nullptr);
        presenting = false;
    }
    lap(mCost.submit);
    // research/179 6.6 4: without VSync the present may go to the present
    // thread. Not while the swapchain waits for the display: frame pacing
    // (waitForDisplay) reads the presents' ids right after this.
    // Nor after stopPresentThread (the viewer shutting down): the thread is
    // not started again for the main window.
    const bool main_on_thread = presenting && LLRHI::sPresentThread && !mPresentThreadStopped && !swapchain->waitsForDisplay();
    if (presenting && !main_on_thread)
    {
        swapchain->present(image_index);
        lap(mCost.present);
    }
    if (main_on_thread || !mExtraPresents.empty())
    {
        // After the window's own present (queued first): the present thread
        // holds the queue while the driver presents, and the main thread does
        // not submit again before the next frame.
        {
            std::lock_guard<std::mutex> lock(mPresentMutex);
            if (!mPresentThreadRunning)
            {
                mPresentStop = false;
                mPresentThreadRunning = true;
                std::thread([this]() { presentLoop(); }).detach();
            }
            if (main_on_thread)
            {
                ExtraWindow window;
                window.swapchain = swapchain;
                window.index = image_index;
                window.main = true;
                mPresentQueue.push_back(window);
            }
            mPresentQueue.insert(mPresentQueue.end(), mExtraPresents.begin(), mExtraPresents.end());
        }
        mMainPresentQueued = mMainPresentQueued || main_on_thread;
        mPresentWake.notify_one();
        lap(main_on_thread ? mCost.present : mCost.other_presents);
    }
    mRecording = false;
    mRingWarned.store(false, std::memory_order_relaxed);
    mCurrent = Target();
    ++mScopeEpoch;
    ++mRecordSerial;

    if (capture && submitted != VK_SUCCESS)
    {
        capture->clear();   // the copy never ran (fix-submitfail B)
    }
    else if (capture)
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        const VkResult waited = vkWaitForFences(device, 1, &mFence, VK_TRUE, UINT64_MAX);
        reportResult(waited, "vkWaitForFences (capture)");
        if (waited != VK_SUCCESS)
        {
            capture->clear();
            return presenting;
        }
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        void* data = nullptr;
        vmaMapMemory(allocator, mReadbackMemory, &data);
        vmaInvalidateAllocation(allocator, mReadbackMemory, 0, VK_WHOLE_SIZE);
        // Stored GL-style (row 0 = bottom); hand back top row first.
        size_t row = (size_t)mWidth * 4;
        capture->resize(row * mHeight);
        for (U32 y = 0; y < mHeight; ++y)
        {
            memcpy(&(*capture)[y * row], (U8*)data + (mHeight - 1 - y) * row, row);
        }
        vmaUnmapMemory(allocator, mReadbackMemory);
    }
    return presenting;
}

// ----------------------------------------------------------------------------
// GPU pass timing (LLRHI::sGPUTiming, research/53)

void LLVKMirror::writeTimestamp(VkCommandBuffer cmd, const char* scope, const char* name)
{
    Slot& slot = mSlots[mSlot];
    // Written once every command before it has completed: the segments add up to the frame.
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, (uint32_t)slot.marks.size());
    slot.marks.push_back({ scope, name });
}

void LLVKMirror::beginGPUTiming()
{
    Slot& slot = mSlots[mSlot];
    slot.timed = false;
    slot.marks.clear();
    if (!LLRHI::sGPUTiming)
    {
        return;
    }
    if (!mGPUTimingLogged)
    {
        mGPUTimingLogged = true;
        LL_INFOS("VKPerf") << "GPU timing: " << mTimestampBits << " valid timestamp bits, " << mTimestampPeriod << " ns per tick"
                           << (mTimestampBits ? "" : "; the graphics queue has no timestamps, so nothing is timed") << LL_ENDL;
    }
    if (!mTimestampBits || mTimestampPeriod <= 0.0)
    {
        return;
    }
    if (!slot.queries)
    {
        VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = MAX_TIMESTAMPS;
        if (vkCreateQueryPool(LLVKDevice::instance().getDevice(), &info, nullptr, &slot.queries) != VK_SUCCESS)
        {
            slot.queries = VK_NULL_HANDLE;
            return;
        }
    }
    // The upload buffer runs first: reset there, and time the uploads too.
    vkCmdResetQueryPool(mUploadBuffer, slot.queries, 0, MAX_TIMESTAMPS);
    slot.timed = true;
    writeTimestamp(mUploadBuffer, nullptr, "upload");
    writeTimestamp(mCommandBuffer, nullptr, "other");
}

void LLVKMirror::gpuMark(const char* scope, const char* name)
{
    Slot& slot = mSlots[mSlot];
    if (!mRecording || !slot.timed || !name)
    {
        return;
    }
    const Slot::Mark& open = slot.marks.back();     // beginGPUTiming wrote two
    if (same_text(open.scope, scope) && same_text(open.name, name))
    {
        return;
    }
    if (slot.marks.size() + 1 >= MAX_TIMESTAMPS)
    {
        ++mGPUMarksDropped;
        return;     // the last timestamp is kept for the end of the frame
    }
    writeTimestamp(mCommandBuffer, scope, name);
}

void LLVKMirror::endGPUTiming()
{
    if (mSlots[mSlot].timed)
    {
        writeTimestamp(mCommandBuffer, nullptr, nullptr);
    }
}

void LLVKMirror::readGPUTimes()
{
    Slot& slot = mSlots[mSlot];
    const bool timed = slot.timed;
    slot.timed = false;
    const U32 count = (U32)slot.marks.size();
    if (timed && count >= 2)
    {
        mGPUTicks.resize(count);
        // No wait flag: the slot's fence has passed. A frame that never reached
        // the GPU (failed submission) reports VK_NOT_READY and is skipped.
        const VkResult result = vkGetQueryPoolResults(LLVKDevice::instance().getDevice(), slot.queries, 0, count, count * sizeof(U64),
                                                      mGPUTicks.data(), sizeof(U64), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS)
        {
            const U64 mask = mTimestampBits >= 64 ? ~0ull : ((1ull << mTimestampBits) - 1);
            const F64 ms_per_tick = mTimestampPeriod * 1e-6;
            F64 total = 0.0;
            for (U32 i = 0; i + 1 < count; ++i)
            {
                F64 ms = (F64)((mGPUTicks[i + 1] - mGPUTicks[i]) & mask) * ms_per_tick;
                if (ms > 10000.0)
                {
                    ms = 0.0;   // out of order: not a duration
                }
                const Slot::Mark& mark = slot.marks[i];
                GPUTime* entry = nullptr;
                for (GPUTime& time : mGPUTimes)
                {
                    if (same_text(time.scope, mark.scope) && same_text(time.name, mark.name))
                    {
                        entry = &time;
                        break;
                    }
                }
                if (!entry)
                {
                    mGPUTimes.push_back({ mark.scope, mark.name, 0.0 });
                    entry = &mGPUTimes.back();
                }
                entry->ms += ms;
                if (mark.scope || !same_text(mark.name, SYNC_MARK))
                {
                    total += ms;
                }
            }
            mGPUTotalMs += total;
            ++mGPUFrames;
            // Frame cost: the frame's whole span and the GPU's idle time since
            // the frame before (frames are read back in order). Timestamps wait
            // for all earlier work, so the first one marks when the GPU got
            // here: a gap means it had nothing to do.
            const U64 first = mGPUTicks[0];
            const U64 last = mGPUTicks[count - 1];
            const F64 span = (F64)((last - first) & mask) * ms_per_tick;
            if (span < 10000.0)
            {
                mCost.gpu_span_ms += span;
                ++mCost.gpu_frames;
            }
            if (mLastGPUEndValid)
            {
                const F64 gap = (F64)((first - mLastGPUEnd) & mask) * ms_per_tick;
                if (gap < 1000.0)
                {
                    mCost.gpu_idle_ms += gap;
                    ++mCost.gpu_gaps;
                }
            }
            mLastGPUEnd = last;
            mLastGPUEndValid = true;
        }
        else
        {
            mLastGPUEndValid = false;
        }
    }
    else
    {
        mLastGPUEndValid = false;   // the next gap would span an untimed frame
    }
    slot.marks.clear();
}

LLVKMirror::FrameCost LLVKMirror::takeFrameCost()
{
    FrameCost cost = mCost;
    mCost = FrameCost();
    return cost;
}

std::string LLVKMirror::takeGPUTimes()
{
    std::string report;
    if (mGPUFrames)
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << "GPU ms:";
        for (const GPUTime& time : mGPUTimes)
        {
            out << ' ';
            if (time.scope)
            {
                out << time.scope << ' ';
            }
            out << time.name << ' ' << time.ms / mGPUFrames << ',';
        }
        out << " total " << mGPUTotalMs / mGPUFrames << " (" << mGPUFrames << " frames";
        if (mGPUMarksDropped)
        {
            out << ", " << mGPUMarksDropped << " marks past " << MAX_TIMESTAMPS << " timestamps went to the segment before";
        }
        out << ")";
        report = out.str();
    }
    mGPUMarksDropped = 0;
    mGPUTimes.clear();
    mGPUTotalMs = 0.0;
    mGPUFrames = 0;
    return report;
}

// ----------------------------------------------------------------------------
// Alpha census (LLRHI::sAlphaCensus, research/54)

void LLVKMirror::beginCensus()
{
    Slot& slot = mSlots[mSlot];
    slot.census_on = false;
    slot.census_tags.clear();
    if (!LLRHI::sAlphaCensus)
    {
        return;
    }
    if (!mCensusLogged)
    {
        mCensusLogged = true;
        LL_INFOS("AlphaCensus") << (LLVKDevice::instance().hasPipelineStatistics()
                                    ? "fragment counts from pipeline statistics queries"
                                    : "no pipeline statistics queries (LLRHI_ALPHA_CENSUS was not set at startup, or the GPU lacks them): bounding box estimates only")
                                << LL_ENDL;
    }
    if (!LLVKDevice::instance().hasPipelineStatistics())
    {
        return;
    }
    if (!slot.census)
    {
        VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = MAX_CENSUS_QUERIES;
        info.pipelineStatistics = CENSUS_STATISTICS;
        if (vkCreateQueryPool(LLVKDevice::instance().getDevice(), &info, nullptr, &slot.census) != VK_SUCCESS)
        {
            slot.census = VK_NULL_HANDLE;
            return;
        }
    }
    // Reset outside any rendering scope, before the frame's draws (the upload buffer runs first).
    vkCmdResetQueryPool(mUploadBuffer, slot.census, 0, MAX_CENSUS_QUERIES);
    slot.census_on = true;
}

U32 LLVKMirror::censusBegin(VkCommandBuffer cmd, U32 tag)
{
    Slot& slot = mSlots[mSlot];
    if (!slot.census_on || !tag || !mRecording)
    {
        return U32_MAX;
    }
    if (slot.census_tags.size() >= MAX_CENSUS_QUERIES)
    {
        ++mCensusDropped;
        return U32_MAX;
    }
    const U32 query = (U32)slot.census_tags.size();
    vkCmdBeginQuery(cmd, slot.census, query, 0);
    slot.census_tags.push_back(tag);
    return query;
}

void LLVKMirror::censusEnd(VkCommandBuffer cmd, U32 query)
{
    if (query != U32_MAX)
    {
        vkCmdEndQuery(cmd, mSlots[mSlot].census, query);
    }
}

void LLVKMirror::readCensus()
{
    Slot& slot = mSlots[mSlot];
    const bool on = slot.census_on;
    slot.census_on = false;
    const U32 count = (U32)slot.census_tags.size();
    if (on && count)
    {
        mCensusResults.resize((size_t)count * CENSUS_VALUES);
        // No wait flag: the slot's fence has passed (a frame that never reached
        // the GPU reports VK_NOT_READY and is skipped).
        const VkResult result = vkGetQueryPoolResults(LLVKDevice::instance().getDevice(), slot.census, 0, count,
                                                      mCensusResults.size() * sizeof(U64), mCensusResults.data(),
                                                      CENSUS_VALUES * sizeof(U64), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS)
        {
            for (U32 i = 0; i < count; ++i)
            {
                CensusSum& sum = mCensus[slot.census_tags[i]];
                const U64* values = &mCensusResults[(size_t)i * CENSUS_VALUES];
                ++sum.queries;
                sum.primitives += values[0];
                sum.clipped += values[1];
                sum.fragments += values[2];
            }
            ++mCensusFrames;
        }
    }
    slot.census_tags.clear();
}

bool LLVKMirror::takeCensus(std::vector<LLRHI::CensusCount>& counts, U32& frames, U32& dropped)
{
    for (const auto& [tag, sum] : mCensus)
    {
        counts.push_back({ tag, sum.queries, sum.primitives, sum.clipped, sum.fragments });
    }
    frames = mCensusFrames;
    dropped = mCensusDropped;
    mCensus.clear();
    mCensusFrames = 0;
    mCensusDropped = 0;
    return LLVKDevice::instance().hasPipelineStatistics();
}
