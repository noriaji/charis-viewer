/**
 * @file llvkdevice.h
 * @brief Vulkan instance, physical/logical device and memory allocator.
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

#ifndef LL_LLVKDEVICE_H
#define LL_LLVKDEVICE_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <map>
#include <sstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;
typedef struct VmaPool_T* VmaPool;

// The single Vulkan device the viewer renders with. Requires Vulkan 1.3
// (dynamic rendering, synchronization2) plus descriptor indexing for bindless
// textures; init() fails cleanly on anything older so the viewer stays on GL.
class LLVKDevice
{
public:
    static LLVKDevice& instance();

    // validation: enable VK_LAYER_KHRONOS_validation (needs the Vulkan SDK installed).
    bool init(bool validation);
    void shutdown();
    bool isReady() const { return mDevice != VK_NULL_HANDLE; }

    VkInstance getInstance() const { return mInstance; }
    VkPhysicalDevice getPhysicalDevice() const { return mPhysicalDevice; }
    VkDevice getDevice() const { return mDevice; }
    VkQueue getGraphicsQueue() const { return mGraphicsQueue; }
    U32 getGraphicsQueueFamily() const { return mGraphicsQueueFamily; }
    VmaAllocator getAllocator() const { return mAllocator; }
    const std::string& getDeviceName() const { return mDeviceName; }
    const VkPhysicalDeviceLimits& getLimits() const { return mLimits; }
    // After a failed init(): why this PC cannot run the Vulkan renderer, one
    // reason per line (no Vulkan driver, the GPU's Vulkan version, the missing
    // features). Empty after a successful init (research/82).
    const std::string& getUnsupportedReport() const { return mUnsupported; }
    // After a failed init(): a real GPU (not CPU rendering) with Vulkan 1.3
    // was passed over for lacking a required feature (research/124 V1), and
    // either no GPU had everything or the one chosen in its place (of a lower
    // kind: integrated for discrete) failed too. The report then has a line
    // "<GPU>: missing <feature> (<what has it>), ..." for each such GPU.
    bool lacksRequiredFeatures() const { return mLacksRequiredFeatures; }

    // VK_EXT_depth_clip_control is required (research/124 V1, 2026-10-21):
    // every pipeline takes GL's -1..1 clip depth (negativeOneToOne), and the
    // depth buffer holds (z + 1) / 2 as GL's does.
    bool hasFragmentStores() const { return mFragmentStores; }
    // Indirect draws: several per call, and a first instance other than 0.
    bool hasMultiDrawIndirect() const { return mMultiDrawIndirect; }
    bool hasDrawIndirectFirstInstance() const { return mDrawIndirectFirstInstance; }
    // vkCmdDrawIndexedIndirectCount (Vulkan 1.2 drawIndirectCount).
    bool hasDrawIndirectCount() const { return mDrawIndirectCount; }
    // What the alpha shader's early out needs (research/54, LLRHI::sAlphaFast):
    // terminateInvocation and demote (enabled), and quad operations in
    // fragment shaders.
    bool hasAlphaEarlyOut() const { return mAlphaEarlyOut; }
    // Basic subgroup operations (subgroupElect) in fragment shaders: the
    // occlusion marks store once per subgroup (research/111 A-OC).
    bool hasFragmentSubgroupBasic() const { return mFragmentSubgroupBasic; }
    // pipelineStatisticsQuery, enabled only for the alpha census (LLRHI::sAlphaCensus at startup).
    bool hasPipelineStatistics() const { return mPipelineStatistics; }
    // VK_KHR_maintenance5 enabled (with DGC or on its own): points drawn by a
    // vertex shader that does not write gl_PointSize are of size 1.
    bool hasMaintenance5() const { return mMaintenance5; }
    // Hardware ray tracing (research/230 R1): VK_KHR_acceleration_structure +
    // VK_KHR_ray_query (+ deferred_host_operations) and their features were
    // enabled at device creation. Only a capability: nothing is built or kept
    // until a feature (PlusRayTracing, an opt-in for photos) asks for it.
    bool hasRayTracing() const { return mRayTracing; }
    // One line for the UI and the log: "on" or why not.
    const std::string& rayTracingStatus() const { return mRayTracingStatus; }
    // fillModeNonSolid enabled: VK_POLYGON_MODE_LINE / POINT (glPolygonMode).
    bool hasFillModeNonSolid() const { return mFillModeNonSolid; }
    // The polygon mode a pipeline may take: FILL without fillModeNonSolid, and
    // for POINT also without maintenance5 (the translated vertex shaders do
    // not write gl_PointSize).
    VkPolygonMode usablePolygonMode(VkPolygonMode mode) const
    {
        if (!mFillModeNonSolid || (mode == VK_POLYGON_MODE_POINT && !mMaintenance5))
        {
            return VK_POLYGON_MODE_FILL;
        }
        return mode;
    }

    // VK_EXT_device_generated_commands (research/50 step 4): enabled at device
    // creation only with LLRHI::sGPUDriven and LLRHI::sGPUDrivenDGCChoice on
    // for this GPU (auto: NVIDIA only; dgc-nvidia-only) when the GPU has it,
    // VK_KHR_maintenance5 and both features. Null otherwise.
    struct GeneratedCommands
    {
        // Vertex buffer tokens a sequence may hold (one per pool stream a G-buffer program reads).
        static constexpr U32 MAX_VERTEX_TOKENS = 8;
        PFN_vkGetGeneratedCommandsMemoryRequirementsEXT getMemoryRequirements = nullptr;
        PFN_vkCmdExecuteGeneratedCommandsEXT cmdExecute = nullptr;
        PFN_vkCreateIndirectCommandsLayoutEXT createLayout = nullptr;
        PFN_vkDestroyIndirectCommandsLayoutEXT destroyLayout = nullptr;
        VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT properties = {};
    };
    const GeneratedCommands* generatedCommands() const { return mDGCReady ? &mDGC : nullptr; }

    // VK_KHR_present_id + VK_KHR_present_wait (research/94, frame pacing):
    // vkWaitForPresentKHR, or null when the GPU lacks either. Enabled whenever
    // the GPU has both (they change nothing until a present carries an id).
    PFN_vkWaitForPresentKHR waitForPresent() const { return mWaitForPresent; }

    // The pipeline cache every vkCreateGraphicsPipelines / vkCreateComputePipelines
    // passes (VK_NULL_HANDLE with LLRHI_PIPELINE_CACHE=0). Loaded from and saved to
    // <LLVKShader::cacheDir()>/pipelines.vkpc. Internally synchronised, so any
    // thread may create pipelines with it. Calling this marks the cache as
    // possibly grown, for the next periodic save.
    VkPipelineCache usePipelineCache()
    {
        if (mPipelineCache)
        {
            mPipelineCacheDirty.store(true, std::memory_order_relaxed);
        }
        return mPipelineCache;
    }
    // Render thread, once per frame: saves when pipelines were created and the
    // last save is at least 60 s old, so a crash loses little. The cache data
    // is read and the file written on a background thread (nothing is read
    // here), and a save is skipped while the previous one is still running.
    void savePipelineCacheIfDue();
    // Saves now, on the calling thread, if pipelines were created since the
    // last save, a periodic save went by the size alone, or the driver data
    // size differs from the last read (shutdown). Waits for a background save
    // still in flight.
    void savePipelineCache();

    // Queue access is shared by the render thread and texture upload threads.
    // With the compute queue in use (research/200, LLVKQueues), the batches
    // get the timeline semaphores added; callers' batches carry no timeline
    // submit info of their own.
    VkResult submit(const VkSubmitInfo& info, VkFence fence);
    // Several batches in one submission (fence signalled once all are done).
    VkResult submit(const VkSubmitInfo* infos, U32 count, VkFence fence);
    VkResult present(const VkPresentInfoKHR& info);
    // vkDeviceWaitIdle with the queues held (it must not run while another
    // thread submits or presents).
    void waitIdle();

    // Validation errors seen so far (warnings are logged, not counted), for
    // probes and logs. Any thread.
    U32 getValidationErrorCount() const { return mValidationErrors.load(std::memory_order_relaxed); }

    // After VK_ERROR_DEVICE_LOST: what the driver says the GPU faulted on
    // (VK_EXT_device_fault) and the last checkpoints it passed. Logged once.
    // Also marks the device lost (isLost).
    void reportDeviceLost();
    // A call returned VK_ERROR_DEVICE_LOST where the fault report cannot be
    // made (a present thread): marks the device lost only. Any thread.
    void noteDeviceLost() { mLost.store(true, std::memory_order_relaxed); }
    // The device is lost: nothing will render or show again. The present hook
    // then tells the user and ends the viewer (fix-submitfail C).
    bool isLost() const { return mLost.load(std::memory_order_relaxed); }
    // LLRHI_VK_CHECKPOINTS=1 (VK_NV_device_diagnostic_checkpoints): mark a draw,
    // so reportDeviceLost() names the last ones the GPU reached.
    bool checkpointsEnabled() const { return mCheckpoints; }
    // LLRHI_VK_PIPELINE_STATS=1 (VK_KHR_pipeline_executable_properties, bc7-fault2):
    // a pipeline made with these flags can tell the driver's figures for its
    // shaders (registers, local and shared memory and so on), one line.
    // 0 and empty otherwise.
    VkPipelineCreateFlags pipelineStatisticsFlags() const { return mPipelineStats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0; }
    std::string pipelineStatistics(VkPipeline pipeline) const;
    // Called at the end of reportDeviceLost() with the checkpoint markers the
    // GPU reached and, for each, the pipeline stage it was the last one of
    // (VkPipelineStageFlagBits; bc7-fault reads BOTTOM_OF_PIPE's).
    using DeviceLostHook = void (*)(const std::vector<U64>& markers, const std::vector<U32>& stages);
    void setDeviceLostHook(DeviceLostHook hook) { mDeviceLostHook = hook; }
    void checkpoint(VkCommandBuffer cmd, U64 marker) const
    {
        if (mCheckpoints)
        {
            mCmdSetCheckpoint(cmd, (const void*)(uintptr_t)marker);
        }
    }
    // The validation callback runs on whichever thread made the call
    // (uploads, shader workers, the present thread): atomic (fix-round71 2).
    void countValidationError() { mValidationErrors.fetch_add(1, std::memory_order_relaxed); }

    // Always on (research/109), for the fault report: the last images and
    // buffers made, queued for release and freed (kind, size, VkDeviceMemory
    // and offset, frame), and the last events of the frame loop the viewer
    // names (snapshots, screen and shadow map resizes, texture pressure). A
    // fixed ring each; a copy and a lock per entry, no allocation.
    enum ResourceStep : U8 { RESOURCE_MADE, RESOURCE_RELEASED, RESOURCE_FREED };
    // what: a short description (copied, at most 55 characters).
    void noteResource(ResourceStep step, U64 handle, VmaAllocation allocation, const char* what);
    void noteEvent(const char* text);
    // Totals of noteResource's steps since the start.
    U64 resourceCount(ResourceStep step) const { return mStepCounts[step].load(std::memory_order_relaxed); }

    // VK_EXT_device_address_binding_report (on unless LLRHI_VK_ADDRESS_LOG=0,
    // research/109): every GPU address range bound or unbound is logged (the
    // last 131072), so a fault report names what used to live at the
    // faulting address and in which frame it went away.
    struct AddressEvent
    {
        U64 base = 0, size = 0, handle = 0;
        VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
        bool bound = false;
        U32 frame = 0;
    };
    void addAddressEvent(const AddressEvent& event);
    // The render thread's frame number (LLVKGL::resetFrameStats): stamps the
    // logs above and is VMA's frame index, so vramBudget() gets the driver's
    // budget again every frame (fix-round71 4).
    void setFrame(U32 frame);
    // Device-local memory: what the process uses against the budget the
    // driver gives it (VK_EXT_memory_budget; VMA's estimate without it), and
    // what this device's allocations take (research/54). Render thread.
    // The counts in it (research/285 X3) run from the start: what one minute
    // took is the difference of two TextureMemory lines, whoever else asks.
    std::string memoryReport();
    // Every live VMA allocation, grouped and sized (research/75: VRAM growth
    // bug). LLRHI_VMA_DUMP writes this to a file on a timer; expensive, debug
    // only (VMA walks every block).
    void dumpAllocations(const std::string& path);
    // research/77: VMA pools by kind and size class. GL buffers (~100k at a
    // heavy place, most under 4 KB, created and freed all the time) and
    // textures of every size shared VMA's 256 MB blocks: the holes freed
    // memory left fitted nothing (1.1 GB free inside the blocks while new
    // 1024x1024 textures took memory of their own past the driver's budget).
    // In pools of their own, a freed allocation leaves a hole the next one of
    // its kind and size fits. LLRHI_VK_POOLS=0: VMA's default pools, as before.
    enum PoolKind { POOL_TEXTURE, POOL_GL_BUFFER, POOL_KIND_COUNT };
    // An optimal-tiling image and its memory, like vmaCreateImage with
    // VMA_MEMORY_USAGE_AUTO; in the texture pool of its size unless the driver
    // wants it in memory of its own (render targets) or it is over 8 MB.
    // where: the kinds of memory it may take (one value, not flags: a caller
    // names what it means, never a bare true or false).
    enum class ImageMemory : U8
    {
        Any,                    // as above
        // research/285 X3: its contents move to another image a few frames on
        // (the BC7 encoder's, research/88): an image over 8 MB stays in VMA's
        // default blocks rather than getting memory of its own for so short.
        Interim,
        // research/285 X2 (a texture moved out of a block): only in a block
        // the texture pool of its size has already, never a new block, a
        // default pool or memory of its own; VK_ERROR_OUT_OF_DEVICE_MEMORY when
        // no such block has room. VMA tries the blocks with the least free
        // space first, but keeps them in that order only roughly (one swap of
        // neighbours a call): mostly a fuller block, now and then an emptier one.
        ExistingPoolBlocks,
        // research/285 (compact-hysteresis, an image over 8 MB moved out of a
        // default block): memory of its own while the frame's few and the cap
        // allow (bigImageOwnMemory), else VK_ERROR_OUT_OF_DEVICE_MEMORY (never a
        // default block). Not counted with the images made (the caller counts
        // what it keeps: noteBigMoved).
        OwnOnly,
    };
    VkResult createImage(const VkImageCreateInfo& info, VkImage& image, VmaAllocation& memory, ImageMemory where = ImageMemory::Any);
    // research/78: the memory of an image createImage made is about to be
    // freed (call before vmaDestroyImage): it no longer counts in vramBudget.
    void forgetImage(VmaAllocation memory);
    // research/78 (LLRHI::sVRAMBudget): device-local memory in use and the
    // driver's budget for it, and the bytes of the images createImage made
    // that are not forgotten (llvkgl's GL textures, as allocated). False
    // without VK_EXT_memory_budget. Any thread; no driver call (VMA fetches
    // the driver's numbers again every 30 allocations and on a new frame index).
    // igpu-safe: on a GPU that shares the PC's memory (setSharedMemoryBudget),
    // the memory of every heap in use, against the drivers' budgets held to
    // the share of the PC's memory set, but never below 1 GB over what is not
    // a texture image. `floored` (optional): that floor is the budget now, so
    // it falls as much as the use when memory other than texture images goes
    // (research/285 compact-hysteresis: no use to run down to).
    bool vramBudget(U64& image_bytes, U64& usage, U64& budget, bool* floored = nullptr);
    // igpu-safe: the GPU has no VRAM of its own (integrated, virtual, CPU);
    // bytes: the share of the PC's memory it may take (llvkgl_glquery's VRAM
    // estimate). From then on vramBudget counts every heap against it, and new
    // memory leaves a device-local heap whose budget is spent for the PC's
    // memory heap (the same memory to such a GPU; past the budget the driver
    // pages). Once, when the GPU is first asked about (main thread, before any
    // texture). LLRHI_VK_SHARED_MEMORY=0: neither (the device-local heap alone,
    // as on a discrete GPU).
    void setSharedMemoryBudget(U64 bytes);
    bool sharedMemory() const { return mSharedBudget.load(std::memory_order_relaxed) != 0; }
    // A buffer and its memory, like vmaCreateBuffer with `usage`; in the pool
    // of `kind` for its size unless it is too large for one. Any thread.
    VkResult createBuffer(PoolKind kind, const VkBufferCreateInfo& info, U32 usage, VkBuffer& buffer, VmaAllocation& memory);
    // Names a Vulkan object for the fault report (llvkgl: GL texture or buffer).
    void setDescribeHook(std::string (*hook)(U64 handle, VkObjectType type)) { mDescribeHook = hook; }
    // research/79: one line per sized pool (kind, size class, used, blocks,
    // allocations), for the texture census (LLRHI::sPoolDetail).
    std::string poolDetail();
    // research/285 X2 (tex-compact): one block of a sized pool, as VMA counts
    // it (every allocation in it, whoever made it).
    struct PoolBlock
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;     // the block (an allocation's VmaAllocationInfo::deviceMemory)
        U64 size = 0;           // the block's bytes
        U64 used = 0;           // of them, allocated
        U32 allocations = 0;
        U32 pool = 0;           // the pool it is in, 1 on (blocks of one pool share it)
    };
    // The blocks of the sized pools of `kind`, sorted by memory (`out` is
    // emptied first and kept by the caller: no allocation once it has grown).
    // False: no sized pools. Any thread (the pool lock, then each pool's
    // block list for reading). Reads VMA 3.2.1's insides (see the .cpp).
    bool poolBlocks(PoolKind kind, std::vector<PoolBlock>& out);
    // research/285 (compact-hysteresis): the same for VMA's default blocks of
    // the heaps vramBudget counts (pool: the memory type + 1), sorted by
    // memory. Any thread; VMA 3.2.1's insides.
    void defaultBlocks(std::vector<PoolBlock>& out);
    // research/245: the bytes allocated in the sized pools of `kind` and the
    // bytes of their blocks (false: no sized pools). Any thread (the pool lock).
    bool poolBytes(PoolKind kind, U64& used, U64& blocks);
    // research/285 X3: `memory` lies in a block of one of the sized pools (not
    // in VMA's default blocks nor in memory of its own). Any thread, by the
    // owner of the allocation: it reads VMA's record of it, so it must not be
    // freed meanwhile.
    bool inSizedPool(VmaAllocation memory) const;
    // research/285 X4 (compact-hysteresis): while on, VMA frees a block that
    // becomes empty at once, also within the heap's budget (VMA otherwise keeps
    // one empty block a pool and memory type: the GL buffer and texture
    // compactions emptied blocks that did not go back). The compactions turn
    // it on for the frames after a pass that gave blocks back
    // (vram_compaction_freed); only for the memory types of the heaps
    // vramBudget counts that are not host visible and the sized pools' types,
    // and only with the sized pools. A local change to vk_mem_alloc.h
    // (m_TrimEmptyBlockTypes).
    void setTrimEmptyBlocks(bool on);
    // The sized pools are on (not LLRHI_VK_POOLS=0): the compactions have something to do.
    bool sizedPools() const { return mSizedPools; }
    // research/285 X3: images over 8 MB get memory of their own (createImage;
    // not with LLRHI_VK_BIG_BLOCKS=0 or LLRHI_VK_POOLS=0).
    bool bigImagesOwnMemory() const { return mBigBlocks && mSizedPools; }
    // research/285 (compact-hysteresis): `count` images over 8 MB were moved
    // into memory of their own (ImageMemory::OwnOnly, taken): memoryReport's count.
    void noteBigMoved(U32 count) { mBigMoved.fetch_add(count, std::memory_order_relaxed); }
    // The empty blocks VMA keeps now, in the sized pools and its default
    // blocks (VMA 3.2.1's insides, read as its statistics read them). Any
    // thread; for the compactions' log lines (every few seconds at most).
    void emptyBlocks(U32& blocks, U64& bytes);

private:
    LLVKDevice() = default;

    bool createInstance(bool validation);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createAllocator();
    void destroyPools();
    VmaPool sizedPool(PoolKind kind, U32 memory_type, U64 size);
    U32 imageMemoryType(const VkImageCreateInfo& info, U32 type_bits);
    std::string poolReport();
    // research/285 X0: where the free room inside the memory blocks is. For
    // each device-local memory type, the texture pools and the GL buffer pools
    // (each size class too), VMA's default blocks and the allocations in
    // memory of their own: blocks, MB of blocks, MB allocated, MB free. The
    // types add up to memoryReport's "Vulkan allocations X MB in Y MB".
    // memoryReport's lines only (once a minute and rarer): no walk over the
    // allocations (a sum each block keeps), nothing allocated but the text.
    std::string blockReport();
    // research/285 X3: an image too large for the texture pools (over 8 MB:
    // 2048x2048 RGBA with its mips) gets memory of its own instead of a part
    // of VMA's default blocks, while the device memory objects stay under
    // mOwnMemoryCap. Its memory goes back to the driver when it is freed (in
    // a default block it held the block, up to 256 MB, until all else in it
    // went). False: as before (LLRHI_VK_BIG_BLOCKS=0, the cap reached, or
    // OWN_MEMORY_PER_FRAME of them already this frame: each is a driver call).
    // frame_limit false (a render target): the frame's limit neither applies
    // nor counts it (research/285 compact-hysteresis).
    bool bigImageOwnMemory(bool frame_limit);
    // VMA's vkAllocateMemory / vkFreeMemory, counted for memoryReport.
    static void VKAPI_PTR onDeviceAllocate(VmaAllocator, uint32_t, VkDeviceMemory, VkDeviceSize, void* self);
    static void VKAPI_PTR onDeviceFree(VmaAllocator, uint32_t, VkDeviceMemory, VkDeviceSize, void* self);
    void countImage(VmaAllocation memory);  // createImage's image counts in vramBudget
    VkResult createImageInPool(const VkImageCreateInfo& info, VkImage& image, VmaAllocation& memory, ImageMemory where);  // createImage, unnoted
    // igpu-safe: the memory types of the device-local heaps that have no room
    // in their budget for `bytes` more, when another heap can take it (a GPU
    // sharing the PC's memory only; 0 otherwise). Any thread.
    U32 spentDeviceLocalTypes(U64 bytes);
    void createPipelineCache();
    // False when a background save still reads the cache after 10 s: the
    // cache is then left alone, and so must the device be (shutdown).
    bool destroyPipelineCache();
    // mPipelineCacheSaving taken, waiting up to 10 s for a save in flight.
    bool claimPipelineCacheSave();
    // The save at shutdown, with mPipelineCacheSaving taken.
    void saveClaimedPipelineCache();
    bool readPipelineCacheData(std::vector<U8>& data);

    VkInstance mInstance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mAddressMessengerHandle = VK_NULL_HANDLE;  // address binding log (research/109)
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mGraphicsQueue = VK_NULL_HANDLE;
    U32 mGraphicsQueueFamily = 0;
    VmaAllocator mAllocator = nullptr;
    std::string mDeviceName;
    VkPhysicalDeviceLimits mLimits = {};
    std::string mUnsupported;       // getUnsupportedReport()
    void addUnsupported(const std::string& reason);
    bool mLacksRequiredFeatures = false;    // lacksRequiredFeatures()
    // pickPhysicalDevice: a GPU passed over for lacking required features that
    // would have been chosen otherwise, one line each; logged once init()
    // succeeds on the other GPU (depthclip-wording).
    std::vector<std::string> mPassedOverNotes;
    bool mFragmentStores = false;
    bool mMultiDrawIndirect = false;
    bool mDrawIndirectFirstInstance = false;
    bool mDrawIndirectCount = false;
    bool mAlphaEarlyOut = false;
    bool mFragmentSubgroupBasic = false;
    bool mPipelineStatistics = false;
    bool mMaintenance5 = false;     // hasMaintenance5()
    bool mFillModeNonSolid = false; // hasFillModeNonSolid()
    bool mRayTracing = false;       // hasRayTracing()
    std::string mRayTracingStatus = "not checked";
    bool mDGCEnabled = false;       // the extension and features were enabled
    bool mDGCReady = false;         // and its entry points found
    GeneratedCommands mDGC;
    bool mPresentWaitEnabled = false;   // both extensions and features were enabled
    PFN_vkWaitForPresentKHR mWaitForPresent = nullptr;
    bool mDeviceFault = false;
    bool mCheckpoints = false;
    std::atomic<bool> mLostReported{ false };
    std::atomic<bool> mLost{ false };   // isLost
    PFN_vkGetDeviceFaultInfoEXT mGetDeviceFaultInfo = nullptr;
    PFN_vkCmdSetCheckpointNV mCmdSetCheckpoint = nullptr;
    PFN_vkGetQueueCheckpointDataNV mGetQueueCheckpointData = nullptr;
    bool mPipelineStats = false;    // LLRHI_VK_PIPELINE_STATS=1, the extension and its feature there (bc7-fault2)
    PFN_vkGetPipelineExecutablePropertiesKHR mGetPipelineExecutableProperties = nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR mGetPipelineExecutableStatistics = nullptr;
    DeviceLostHook mDeviceLostHook = nullptr;
    std::string (*mDescribeHook)(U64 handle, VkObjectType type) = nullptr;
    bool mAddressLog = false;
    bool mAddressMessenger = false;     // the instance has the address binding messenger (research/109)
    bool mMemoryBudget = false;     // VK_EXT_memory_budget enabled
    std::atomic<U64> mImageBytes{ 0 };  // createImage's images, less forgetImage's (research/78)
    std::atomic<U64> mSharedBudget{ 0 };    // setSharedMemoryBudget; 0: a discrete GPU (igpu-safe)
    // Written by the render thread (setFrame), read by any thread inside the
    // logs' locks: atomic, relaxed (only a stamp, fix-round71 3).
    std::atomic<U32> mFrame{ 0 };
    std::mutex mAddressMutex;
    std::vector<AddressEvent> mAddressEvents;
    std::map<U64, AddressEvent> mLiveBindings;      // by base address: bound and not unbound since (research/109)
    U64 mAddressEventCount = 0;         // every event so far, and as memoryReport last told it (research/125 #13)
    U64 mAddressEventsReported = 0;
    void describeLiveBindings(std::ostringstream& out, U64 low, U64 high);
    // noteResource / noteEvent (research/109).
    struct ResourceEvent
    {
        U64 handle = 0;
        U64 memory = 0;         // VkDeviceMemory
        U64 offset = 0;
        U64 bytes = 0;
        U32 frame = 0;
        ResourceStep step = RESOURCE_MADE;
        char what[56] = {};
    };
    struct FrameEvent
    {
        U32 frame = 0;
        char text[120] = {};
    };
    static constexpr size_t RESOURCE_RING = 4096;      // every resource noted
    static constexpr size_t LARGE_RING = 512;          // those of 8 MB and up (render targets, shadow maps, OIT)
    static constexpr size_t EVENT_RING = 64;
    static constexpr U64 LARGE_BYTES = 8ull << 20;
    std::mutex mHistoryMutex;
    std::vector<ResourceEvent> mResources;      // rings, filled on first use
    std::vector<ResourceEvent> mLargeResources;
    std::vector<FrameEvent> mEvents;
    U64 mResourceCount = 0, mLargeCount = 0, mEventCount = 0;
    std::atomic<U64> mStepCounts[3] = {};
    std::string reportHistory();
    std::string describeFromHistory(U64 handle);
    std::atomic<U32> mValidationErrors{ 0 };
    std::mutex mQueueMutex;
    VkPipelineCache mPipelineCache = VK_NULL_HANDLE;
    std::string mPipelineCachePath;
    std::atomic<bool> mPipelineCacheDirty{ false };
    std::atomic<U64> mPipelineCacheHash{ 0 };       // of the data in the file, to skip rewriting it unchanged
    std::atomic<bool> mPipelineCacheSaving{ false };  // a save is in flight (at most one)
    std::atomic<size_t> mPipelineCacheReadSize{ 0 };  // of the data last read (or loaded): unchanged means nothing new
    std::atomic<bool> mPipelineCacheSizeSkipped{ false };  // a periodic save went by the size alone: read it whole at shutdown
    std::chrono::steady_clock::time_point mPipelineCacheSaved;
    // research/77: the sized pools (null where one could not be made) by kind,
    // memory type and size class, and the memory type VMA picks for images
    // by their memoryTypeBits and for buffers by kind.
    struct SizedPool
    {
        PoolKind kind;
        VmaPool pool;
    };
    bool mSizedPools = false;
    // research/285 X3 (createAllocator): images over 8 MB in memory of their
    // own, VMA's default blocks smaller (64 MB, LLRHI_VK_DEFAULT_BLOCK_MB);
    // LLRHI_VK_BIG_BLOCKS=0: neither. With LLRHI_VK_POOLS=0 only the blocks'
    // size changes (images over 8 MB are told apart where the pools are). The
    // cap: half the device's maxMemoryAllocationCount, at most 2048 (VMA
    // itself stops preferring memory of its own past three quarters).
    bool mBigBlocks = false;
    U32 mOwnMemoryCap = 0;
    std::atomic<bool> mOwnMemoryCapLogged{ false };
    // bigImageOwnMemory's frame (mFrame) and how many it let through in it.
    std::atomic<U32> mOwnMemoryFrame{ U32_MAX };
    std::atomic<U32> mOwnMemoryInFrame{ 0 };
    // Counted for memoryReport: VMA's vkAllocateMemory and vkFreeMemory calls,
    // and the images over 8 MB by where they went (memory of their own, a
    // default block for the frame's limit or the cap, a default block as
    // interim), since the start. VMA itself gives memory of its own to what
    // is over half a default block, whatever was asked: counted by where
    // each image went in the end.
    std::atomic<U64> mDeviceAllocations{ 0 };
    std::atomic<U64> mDeviceFrees{ 0 };
    std::atomic<U64> mBigOwn{ 0 };
    std::atomic<U64> mBigHeld{ 0 };
    std::atomic<U64> mBigInterim{ 0 };
    std::atomic<U64> mBigMoved{ 0 };    // research/285 compact-hysteresis: moved into memory of their own later (OwnOnly)
    std::mutex mPoolMutex;
    std::unordered_map<U64, SizedPool> mPools;
    std::unordered_map<U32, U32> mImageMemoryTypes;
    U32 mBufferMemoryTypes[POOL_KIND_COUNT] = {};
    U32 mBufferMemoryUsage[POOL_KIND_COUNT] = {};   // the buffer usage flags the type was found for
};

#endif // LL_LLVKDEVICE_H
