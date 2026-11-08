/**
 * @file llvkmirror.h
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

#ifndef LL_LLVKMIRROR_H
#define LL_LLVKMIRROR_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;
class LLVKSwapchain;
namespace LLRHI { struct CensusCount; }

// While GL still draws the window, every GL call is replayed into this
// offscreen frame so the two can be compared pixel for pixel. The same frame
// object becomes the swapchain-backed one once Vulkan presents on its own.
//
// Coordinates: images are stored exactly like GL stores them (row 0 = bottom),
// so GL viewport/scissor rectangles and gl_FragCoord-based lookups need no
// conversion. Only the final capture/present flips rows.
//
// The frame owns the default framebuffer (color + depth/stencil) and the
// command stream. llvkgl decides what is rendered where and keeps image
// layouts; this class only opens and closes rendering scopes.
class LLVKMirror
{
public:
    static const VkFormat COLOR_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
    static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT_S8_UINT;
    // CPU records frame N while the GPU still runs frame N-1.
    static const U32 FRAMES_IN_FLIGHT = 2;

    // A set of attachments to render into (dynamic rendering).
    struct Target
    {
        VkImageView colors[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkFormat color_formats[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
        U32 color_count = 0;
        VkImageView depth = VK_NULL_HANDLE;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        bool stencil = false;
        // Depth attached read-only (depth writes off) so it can be sampled at the same time.
        bool depth_read_only = false;
        U32 width = 0;
        U32 height = 0;

        bool operator==(const Target& other) const;
        bool operator!=(const Target& other) const { return !(*this == other); }
    };

    static LLVKMirror& instance();

    bool init();
    void shutdown();
    // fix-threads4: the other windows' present thread presents what it has
    // and ends (shutdown() does it too; LLRHIVKMirror::onGLShutdown calls it
    // alone, as the device and the targets stay until exit). A later frame
    // with another window starts it again.
    void stopPresentThread();
    bool isReady() const { return mCommandPool != VK_NULL_HANDLE; }

    // Frame lifetime. beginFrame resizes the default framebuffer if needed.
    void beginFrame(U32 width, U32 height);
    // Whether the last beginFrame found the GPU still on the slot's previous
    // frame (the one before last) and had to wait for it: the GPU, not the
    // CPU, set the pace (frame pacing, research/103 F1).
    bool gpuWasBehind() const { return mGPUBehind; }
    // capture: read the default color buffer back as RGBA8, top row first.
    // swapchain: also present the frame on the window. Returns true if presented.
    bool endFrame(std::vector<U8>* capture, LLVKSwapchain* swapchain = nullptr);
    // fix-submitfail B/C, fix-round6 1: a submission failed without the
    // device being lost (out of memory, or any other result), once: the
    // frame's, one inside it (submitAndWait) or one between frames
    // (immediate). What it uploaded and the layouts it changed are lost while
    // their owners count them as written, so nothing more is submitted and
    // the present hook tells the user and ends the viewer. 0 while they go
    // through, else the first failure's result.
    VkResult stuckSubmitResult() const { return mStuckSubmit.load(std::memory_order_relaxed); }
    // Commands the frame needs were lost before its submission (a spliced
    // stream not complete in time, security audit B 2): as a failed
    // submission (submitFailed, VK_TIMEOUT): not submitted, the viewer ends
    // at the next present. Any thread.
    void workLost(const char* what) { submitFailed(VK_TIMEOUT, what); }
    // research/87 (plus/detach): another window shown with the frame being
    // recorded. image (width x height, rows stored GL-style, left in
    // COLOR_ATTACHMENT_OPTIMAL by the commands recorded before endFrame) is
    // blitted into swapchain at the end of the frame, in the same submission,
    // and presented after the window's own. Ignored while nothing is recorded.
    // probe (research/105, may be null): 8 bytes the frame fills with the
    // middle pixel of image (RGBA8), then of the swapchain image after the
    // blit (4 bytes in the swapchain's format; left as it was when the
    // swapchain's images are not readable).
    void presentExtraWindow(LLVKSwapchain* swapchain, VkImage image, U32 width, U32 height,
                            VkBuffer probe = VK_NULL_HANDLE);
    // The swapchain is about to go: drop it from the frame being recorded
    // (and wait for its presents on the present thread).
    void forgetExtraWindow(LLVKSwapchain* swapchain);
    // The other windows are presented on a thread of their own (research/105:
    // a window's present cost the main thread 1 to 3 ms on NVIDIA). Returns
    // once it has presented all it was handed: before anything else touches
    // those swapchains (acquire, recreation, results, destruction).
    void waitExtraPresents();
    // research/179 6.6 4 (LLRHI::sPresentThread): without VSync, the main
    // window's present goes to the present thread too (first, before the
    // other windows'), so the frame's thread does not sit in vkQueuePresentKHR.
    // Until it has returned nothing may touch that swapchain: the thread that
    // presents calls waitMainPresent before it does (endFrame does too, before
    // its acquire). Its own thread only.
    bool mainPresentQueued() const { return mMainPresentQueued; }
    void waitMainPresent();
    bool isRecording() const { return mRecording; }
    // Submits what the frame recorded so far and waits until the GPU has done
    // it; recording then goes on in the same frame (the command buffers start
    // over, the ring, descriptor sets and deferred work stay). For synchronous
    // readbacks (glReadPixels). Nothing stays bound in the command buffers:
    // callers drop what they cached about it. The upload commands recorded
    // after this run after the part already submitted. False when nothing is
    // being recorded or the submission failed.
    bool submitAndWait();
    // Moves whenever commands may have been recorded (a command buffer handed
    // out, a frame begun or ended, a submission): while it stays the same, the
    // images hold what they held.
    U64 recordSerial() const { return mRecordSerial; }
    // Which frame-in-flight slot is being recorded (per-frame resources index by it).
    U32 getSlot() const { return mSlot; }
    // Moves each time beginFrame starts recording a slot (after waiting for the
    // slot's previous frame): per-slot resources last used at another value
    // are free again.
    U64 frameBegins() const { return mFrameBegins; }

    U32 getWidth() const { return mWidth; }
    U32 getHeight() const { return mHeight; }

    // Default framebuffer. Its layouts are tracked by the caller between
    // beginFrame (both start as attachment layouts) and endFrame.
    Target defaultTarget() const;
    // Moves whenever defaultTarget() may give other views or another size
    // (the target made again or destroyed; research/154 W2). Starts at 1.
    U32 defaultTargetSerial() const { return mDefaultTargetSerial; }
    VkImage getColorImage() const { return mColor; }
    VkImage getDepthImage() const { return mDepth; }
    VkImageLayout mColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout mDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Command buffer inside a rendering scope on target (re-opened when the
    // target changes, and after a scope for secondary command buffers).
    VkCommandBuffer beginRendering(const Target& target);
    // research/208 13.3 (vk-s4a4): a new rendering scope on target whose
    // contents are secondary command buffers only (VK_RENDERING_CONTENTS_
    // SECONDARY_COMMAND_BUFFERS_BIT): the open one is ended first, and the
    // caller records nothing into it but executeLater, then endRendering.
    VkCommandBuffer beginSecondaryRendering(const Target& target);
    // Executes the secondary command buffer *buffer holds when the recording
    // worker replays this (if *replayed: see LLVKRecorder::CmdExecuteLater).
    void executeLater(const std::atomic<VkCommandBuffer>* buffer, const std::atomic<bool>* replayed);
    bool isSecondaryRendering() const { return mRendering && mCurrentSecondary; }
    // Close the rendering scope (for barriers and transfers).
    void endRendering();
    bool isRendering() const { return mRendering; }
    // Moves whenever a rendering scope begins or ends, and with every frame:
    // what was recorded since an equal value is still in the same scope.
    U32 mScopeEpoch = 0;
    // The frame's command buffer outside any rendering scope.
    VkCommandBuffer transferCommands();
    // Commands that run before everything else in this frame (buffer uploads for
    // data not yet used this frame: no rendering scope has to be broken for them).
    VkCommandBuffer uploadCommands() const { ++mRecordSerial; return mRecording ? mUploadBuffer : VK_NULL_HANDLE; }

    // Per-frame linear upload memory, valid until the frame's GPU work is done.
    // Returns false when the frame's budget is exhausted. Any thread may call
    // it while a frame is being recorded (the head moves atomically; research/66
    // step 1); called from one thread it hands out exactly what it did before.
    bool allocate(VkDeviceSize size, VkDeviceSize alignment, VkBuffer& buffer, VkDeviceSize& offset, void*& data);

    // A thread's own block of the frame's upload ring, for a worker that
    // allocates often while it records a stream (llvkrecorder.h): one atomic
    // step per block instead of per allocation. Same lifetime as allocate()'s
    // memory: a block taken in one frame is not used in the next (it starts
    // over when beginFrame has moved on). Not shared between threads.
    class UploadBlock
    {
    public:
        static constexpr VkDeviceSize BLOCK_SIZE = 256 * 1024;
        static constexpr VkDeviceSize BLOCK_ALIGNMENT = 256;   // at least any minimum offset alignment
        bool allocate(VkDeviceSize size, VkDeviceSize alignment, VkBuffer& buffer, VkDeviceSize& offset, void*& data);

    private:
        U64 mFrame = UINT64_MAX;    // frameBegins() it was taken in
        VkBuffer mBuffer = VK_NULL_HANDLE;
        U8* mData = nullptr;        // the ring's mapping (offsets are the ring's)
        VkDeviceSize mHead = 0;
        VkDeviceSize mEnd = 0;
    };

    // Per-frame descriptor set. Main thread only (the pools are not locked;
    // stream fillers take their sets from the main thread, research/66).
    VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);
    U32 getDescriptorPoolCount() const { return (U32)mSlots[mSlot].pools.size(); }

    // research/156 A-DS (LLRHI_SET_PERSIST=1): descriptor sets that outlive
    // the frame, from pools of their own that beginFrame leaves alone. Null
    // when every such pool is full (PERSISTENT_POOLS of them): the caller
    // retires them all and asks again. Main thread only, as allocateSet.
    VkDescriptorSet allocatePersistentSet(VkDescriptorSetLayout layout);
    // Every persistent set goes: their pools are destroyed once the GPU is
    // done with the frames recorded so far (defer). Callers forget the sets.
    void retirePersistentSets();
    U32 persistentPoolCount() const { return (U32)mPersistentPools.size(); }

    // Destroy once the GPU no longer uses anything recorded so far.
    void defer(std::function<void()> destroy);

    // Called first by submitAndWait and endFrame, before the frame's commands
    // are submitted (research/70: staging memory still to be written).
    static void (*sBeforeSubmit)();
    // Called next (also on the render thread): the last commands for the
    // frame's upload commands, which some recorders gather over the frame
    // and record together (research/98: the GPU shadow list's dispatches).
    static void (*sBeforeUploadEnd)();
    // endFrame only, once the frame's draws are all recorded (the recorder
    // finished, no rendering scope open): the last commands of the frame's
    // own command buffer, before the present's blit (research/179 A: the
    // occlusion marks copied for the CPU).
    static void (*sBeforeFrameEnd)(VkCommandBuffer cmd);
    // research/209 E4 (vk-s4a4): endFrame and submitAndWait, right after
    // LLVKRecorder::finish (every stream joined, the recording worker done:
    // what it records into upload goes in directly, after everything else
    // there): the retained scene's queued choices and expands. frame_end:
    // endFrame's.
    static void (*sAfterRecorderFinish)(VkCommandBuffer upload, bool frame_end);

    // Record transfer work: into the frame while one is being recorded (so it
    // stays ordered with the draws), otherwise as a one-off submission.
    void record(const std::function<void(VkCommandBuffer)>& commands);

    static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                             VkImageLayout from, VkImageLayout to, U32 levels, U32 layers);

    // Research/111 A-BB: while one is open on this thread, the image barriers
    // for its command buffer (imageBarrier: the layout transitions before a
    // draw, the attachments' layouts) are kept and recorded as one
    // vkCmdPipelineBarrier2 when it closes. Each barrier keeps its own stages
    // and accesses, so the dependencies are the ones the separate barriers
    // made; a second barrier of an image already kept flushes the first ones.
    // Only around code that records nothing else into cmd meanwhile.
    // LLRHI_BARRIER_BATCH=0: every barrier is recorded at once, as before.
    class BarrierBatch
    {
    public:
        explicit BarrierBatch(VkCommandBuffer cmd);
        ~BarrierBatch();
        BarrierBatch(const BarrierBatch&) = delete;
        BarrierBatch& operator=(const BarrierBatch&) = delete;

    private:
        bool mOpened = false;
    };

    // GPU pass timing (LLRHI::sGPUTiming, research/53). A frame begun while it
    // is on gets a timestamp at its start ("upload": the upload buffer), at the
    // start of the main command buffer ("other"), at each gpuMark, before the
    // final blit ("present") and at its end. The slot's results are read when
    // beginFrame comes round to it again (its fence has passed: no stall).
    // gpuMark starts a segment named scope + name (literals, kept by pointer)
    // that lasts until the next mark; marking the open name writes nothing.
    void gpuMark(const char* scope, const char* name);
    // "GPU ms: upload 0.10, other 0.20, ..., total 15.20 (290 frames)": the
    // average per timed frame since the last call, in frame order; resets. The
    // total leaves out "sync" (waits in submitAndWait). Empty: nothing timed.
    std::string takeGPUTimes();

    // Alpha census (LLRHI::sAlphaCensus, research/54): a pipeline statistics
    // query (primitives in, primitives clipped, fragment shader invocations)
    // around one draw, for frames begun while the census is on and the device
    // has the queries. censusBegin returns the query to pass to censusEnd, or
    // U32_MAX: not counted (nothing recorded). Call both inside the same
    // rendering scope, with only the draw between them.
    U32 censusBegin(VkCommandBuffer cmd, U32 tag);
    void censusEnd(VkCommandBuffer cmd, U32 query);
    // LLRHI::sCensusTake: what was read back since the last call. False: no queries.
    bool takeCensus(std::vector<LLRHI::CensusCount>& counts, U32& frames, U32& dropped);

    // Frame cost (LLRHI::sFrameCost, research/83): CPU ms of the steps of
    // endFrame and beginFrame, summed over `frames` frames, and the waits
    // inside a frame. Measured only while the flag is on.
    struct FrameCost
    {
        U32 frames = 0;             // endFrame calls measured
        F64 before_submit = 0.0;    // sBeforeSubmit (texture creation joined)
        F64 recorder_wait = 0.0;    // LLVKRecorder::finish: the worker drains the frame
        F64 acquire = 0.0;          // the swapchain image (blocks when none is free)
        F64 record_end = 0.0;       // end of scope, blit, capture, end of command buffers
        F64 submit = 0.0;           // vkQueueSubmit
        F64 present = 0.0;          // vkQueuePresentKHR
        F64 other_windows = 0.0;    // detached windows (research/87): acquire (never waits) and blit recorded
        F64 other_presents = 0.0;   // handing their presents to the present thread
        // research/179 6.6 4: the main window's presents on the present thread.
        U32 main_presents = 0;      // presented there (the period's)
        F64 main_present_call = 0.0;    // its vkQueuePresentKHR (on that thread)
        F64 main_present_wait = 0.0;    // waitMainPresent: the next frame waiting for it
        F64 fence_wait = 0.0;       // beginFrame: the slot's last frame on the GPU
        F64 deferred = 0.0;         // deferred destruction
        F64 reset = 0.0;            // query results read, descriptor pools reset
        F64 resize = 0.0;           // default framebuffer remade (window size changed)
        F64 begin = 0.0;            // command buffers begun, first barriers
        U32 resizes = 0;
        U32 sync_submits = 0;       // submitAndWait (readbacks, snapshot OIT checks)
        F64 sync_ms = 0.0;
        U32 immediates = 0;         // one-off submissions waited for (record() outside a frame)
        F64 immediate_ms = 0.0;
        U32 gpu_frames = 0;         // timed frames read back (GPU timing on)
        F64 gpu_span_ms = 0.0;      // first to last timestamp of a frame
        F64 gpu_idle_ms = 0.0;      // the previous frame's last timestamp to this frame's first
        U32 gpu_gaps = 0;
    };
    FrameCost takeFrameCost();
    // Monotonic counters (main thread), for per-segment deltas.
    U64 renderScopes() const { return mRenderScopes; }     // rendering scopes begun
    U64 setsAllocated() const { return mSetsAllocated; }   // per-frame descriptor sets

private:
    LLVKMirror() = default;

    bool createTarget(U32 width, U32 height);
    void destroyTarget();
    void runDeferred();
    void immediate(const std::function<void(VkCommandBuffer)>& commands);

    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    // Per-frame resources; the m* aliases below point at the current slot.
    struct Slot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer upload = VK_NULL_HANDLE;   // runs before cmd in the same submission
        // The detached windows' blits (research/87): a batch of their own
        // after cmd's, so that only it waits for their swapchain images.
        VkCommandBuffer windows = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        // The fence was reset for a frame whose submission failed (and so did
        // the empty one meant to signal it in its place): nothing will signal
        // it, so it is not waited for (fix-submitfail B).
        bool unsubmitted = false;
        VkBuffer ring = VK_NULL_HANDLE;
        VmaAllocation ring_memory = nullptr;
        U8* ring_data = nullptr;
        std::vector<VkDescriptorPool> pools;      // grows when a frame needs more sets
        size_t pool_index = 0;
        std::vector<std::function<void()>> deferred;
        // GPU timing: the segment each timestamp starts (the last one ends the frame).
        struct Mark
        {
            const char* scope;
            const char* name;
        };
        VkQueryPool queries = VK_NULL_HANDLE;
        std::vector<Mark> marks;                  // one per timestamp written this frame
        bool timed = false;                       // this frame is being timed
        // Alpha census: the tag of each query begun this frame.
        VkQueryPool census = VK_NULL_HANDLE;
        std::vector<U32> census_tags;
        bool census_on = false;                   // this frame counts tagged draws
    };
    Slot mSlots[FRAMES_IN_FLIGHT];
    std::vector<std::function<void()>> mRunningDeferred;   // runDeferred's list while it runs (empty otherwise)
    U32 mSlot = 0;
    void selectSlot(U32 slot);
    static void reportResult(VkResult result, const char* call);
    VkDescriptorPool createDescriptorPool();
    // research/156 A-DS: allocatePersistentSet's pools (each as big as a
    // frame's pool), the one it tries first, and how many it may make.
    static const U32 PERSISTENT_POOLS = 4;
    std::vector<VkDescriptorPool> mPersistentPools;
    U32 mPersistentIndex = 0;
    void waitAll();
    // The end of frame's submission failed (fix-submitfail B): an empty
    // submission waits for the swapchain images' acquire semaphores and
    // signals the slot's fence in its place (the slot is marked unsubmitted
    // if that fails too), and the windows recreate their swapchains (their
    // acquired images were never presented). swapchain: the main window's,
    // when an image was acquired from it.
    void abandonFrame(LLVKSwapchain* swapchain);
    // A submission failed (fix-round6 1): notes the first result for
    // stuckSubmitResult, which ends the viewer at the next present, and from
    // then on nothing more is submitted. what: which submission, for the log.
    void submitFailed(VkResult result, const char* what);
    // Reset and begin the frame's two command buffers (recording worker, upload barrier).
    void beginCommandBuffers();
    // End both: what the frame wrote is visible to the host, its uploads to its draws.
    void endCommandBuffers();

    // presentExtraWindow: this frame's other windows, and what endFrame
    // presents (kept to reuse their memory).
    struct ExtraWindow
    {
        LLVKSwapchain* swapchain = nullptr;
        VkImage image = VK_NULL_HANDLE;
        U32 width = 0;
        U32 height = 0;
        U32 index = 0;      // the swapchain image endFrame acquired
        VkBuffer probe = VK_NULL_HANDLE;
        bool main = false;  // the main window's (waitMainPresent; no image or probe)
    };
    std::vector<ExtraWindow> mExtraWindows;
    std::vector<ExtraWindow> mExtraPresents;
    // Drops swapchain from mExtraWindows only (fix-round71 1): no wait for the
    // present thread, which works on its own copies (mPresentQueue) and never
    // reads mExtraWindows. forgetExtraWindow adds the wait for a swapchain
    // that is about to go.
    void dropExtraWindow(LLVKSwapchain* swapchain);
    // The present thread (waitExtraPresents): started at the first other
    // window, stopped in stopPresentThread() (onGLShutdown, shutdown()). Detached: this object is a static that
    // may be destroyed at exit without shutdown() (a joinable std::thread
    // would end the process there).
    void presentLoop();
    bool mPresentThreadRunning = false;
    std::mutex mPresentMutex;
    std::condition_variable mPresentWake;   // work, or stop
    std::condition_variable mPresentDone;   // the queue is empty and nothing is being presented
    std::vector<ExtraWindow> mPresentQueue;
    bool mPresenting = false;
    bool mPresentStop = false;
    bool mMainPresentQueued = false;    // mainPresentQueued (the frame's thread only)
    bool mPresentThreadStopped = false; // stopPresentThread was called: the main window's presents stay on the frame's thread
    // The present thread's count and time of main window presents (under
    // mPresentMutex), taken into mCost by waitMainPresent.
    U32 mMainPresents = 0;
    F64 mMainPresentMs = 0.0;
    std::vector<VkSemaphore> mSubmitWaits;
    std::vector<VkPipelineStageFlags> mSubmitWaitStages;
    std::vector<VkSemaphore> mSubmitSignals;
    std::vector<VkSemaphore> mWindowWaits;          // the windows' batch (Slot::windows)
    std::vector<VkPipelineStageFlags> mWindowWaitStages;
    std::vector<VkSemaphore> mWindowSignals;

    // GPU timing (gpuMark).
    void beginGPUTiming();      // beginFrame, after beginCommandBuffers
    void endGPUTiming();        // endFrame: the timestamp that ends the frame
    void readGPUTimes();        // beginFrame, after the slot's fence
    void writeTimestamp(VkCommandBuffer cmd, const char* scope, const char* name);
    U32 mTimestampBits = 0;     // the graphics queue's timestampValidBits (0: no timestamps)
    F64 mTimestampPeriod = 0.0; // ns per tick
    bool mGPUTimingLogged = false;
    struct GPUTime
    {
        const char* scope;
        const char* name;
        F64 ms;
    };
    std::vector<GPUTime> mGPUTimes;     // since takeGPUTimes, in order of first appearance
    F64 mGPUTotalMs = 0.0;
    U32 mGPUFrames = 0;
    U32 mGPUMarksDropped = 0;   // marks past MAX_TIMESTAMPS since takeGPUTimes
    std::vector<U64> mGPUTicks;

    // Alpha census (censusBegin).
    void beginCensus();         // beginFrame, after beginGPUTiming
    void readCensus();          // beginFrame, after the slot's fence
    bool mCensusLogged = false;
    struct CensusSum
    {
        U32 queries = 0;
        U64 primitives = 0;
        U64 clipped = 0;
        U64 fragments = 0;
    };
    std::unordered_map<U32, CensusSum> mCensus;     // per tag, since takeCensus
    U32 mCensusFrames = 0;
    U32 mCensusDropped = 0;
    std::vector<U64> mCensusResults;

    // Frame cost (takeFrameCost).
    FrameCost mCost;
    U64 mRenderScopes = 0;
    U64 mSetsAllocated = 0;
    U64 mLastGPUEnd = 0;        // the last timed frame's last timestamp (ticks)
    bool mLastGPUEndValid = false;

    VkCommandBuffer mCommandBuffer = VK_NULL_HANDLE;
    VkCommandBuffer mUploadBuffer = VK_NULL_HANDLE;
    VkFence mFence = VK_NULL_HANDLE;
    VkCommandPool mImmediatePool = VK_NULL_HANDLE;
    VkFence mImmediateFence = VK_NULL_HANDLE;

    VkImage mColor = VK_NULL_HANDLE;
    VkImageView mColorView = VK_NULL_HANDLE;
    VmaAllocation mColorMemory = nullptr;
    VkImage mDepth = VK_NULL_HANDLE;
    VkImageView mDepthView = VK_NULL_HANDLE;
    VmaAllocation mDepthMemory = nullptr;
    VkBuffer mReadback = VK_NULL_HANDLE;
    VmaAllocation mReadbackMemory = nullptr;

    VkBuffer mRing = VK_NULL_HANDLE;
    VmaAllocation mRingMemory = nullptr;
    U8* mRingData = nullptr;
    VkDeviceSize mRingSize = 0;
    std::atomic<VkDeviceSize> mRingHead{ 0 };
    std::atomic<bool> mRingWarned{ false };



    U32 mWidth = 0;
    // After the window target could not be made at a size (VRAM full), the
    // same size is tried again only after TARGET_RETRY_SECONDS; meanwhile the
    // frames are skipped as on the failure itself, without waiting for the GPU
    // or warning each frame. (fix-round95 B) Another size waits too, unless it
    // is smaller than the failed one (fix-round100).
    static constexpr std::chrono::seconds TARGET_RETRY_SECONDS{ 5 };
    U32 mFailedWidth = 0;
    U32 mFailedHeight = 0;
    std::chrono::steady_clock::time_point mTargetRetryAt;
    U32 mDefaultTargetSerial = 1;   // defaultTargetSerial()
    U32 mHeight = 0;
    bool mRecording = false;
    bool mGPUBehind = false;        // gpuWasBehind
    // stuckSubmitResult: the first failed submission's result (submitFailed).
    std::atomic<VkResult> mStuckSubmit{ VK_SUCCESS };
    bool mRendering = false;
    bool mCurrentSecondary = false;     // the open scope's contents are secondary command buffers (research/208 13.3)
    mutable U64 mRecordSerial = 0;     // uploadCommands() is const
    U64 mFrameBegins = 0;
    Target mCurrent;
};

#endif // LL_LLVKMIRROR_H
