/**
 * @file llvkswapchain.h
 * @brief Vulkan swapchain on the viewer's window (presentation).
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

#ifndef LL_LLVKSWAPCHAIN_H
#define LL_LLVKSWAPCHAIN_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

// Presents the frame's final image on the viewer's window. The frame is
// blitted (and flipped, since frames are stored GL-style bottom row first)
// into the acquired swapchain image at the end of its command buffer.
class LLVKSwapchain
{
public:
    // hwnd: the viewer's window (HWND).
    bool init(void* hwnd);
    void shutdown();
    bool isReady() const { return mSwapchain != VK_NULL_HANDLE; }
    // Why init() or a swapchain creation last failed, for the user (empty if
    // none has since init() began).
    const std::string& failure() const { return mFailure; }
    // The window has a Vulkan surface: Vulkan owns presentation even while the
    // swapchain itself is missing (minimized, or its last creation failed;
    // acquire() keeps trying to create it), and while a lost surface waits for
    // a new one that could not be made yet (fix-surfacelost: acquire() keeps
    // trying, the caller keeps calling it).
    bool hasSurface() const { return mSurface != VK_NULL_HANDLE || mSurfaceNeeded; }

    // Follow the GL swap interval the viewer set (RenderVSyncEnable ->
    // LLWindow::toggleVSync -> wglSwapIntervalEXT): 0 = no vsync (IMMEDIATE,
    // else MAILBOX), > 0 = vsync (FIFO), < 0 = adaptive (FIFO_RELAXED, else
    // FIFO). A change recreates the swapchain at the next acquire.
    void setSwapInterval(S32 interval);
    // Without vsync (swap interval 0), MAILBOX before IMMEDIATE: no tearing,
    // and still no waiting for the display. For windows that show UI only
    // (research/87: a tear across scrolling text shows more than one across
    // the world). Set before init(), or later (the main window's
    // LLRHI_PRESENT_MODE=mailbox, research/179 6.6 4): a change while VSync is
    // off recreates the swapchain at the next acquire.
    void setNoTearing(bool on);
    // The swapchain's minImageCount (research/179 6.6 4, LLRHI_PRESENT_IMAGES):
    // 0 (the default) is the surface's minimum + 1; otherwise this many, within
    // what the surface allows. Set before init().
    void setImageCount(U32 count) { mImageCountWanted = count; }
    // Images that can also be read back (TRANSFER_SRC usage, where the surface
    // allows it), for the probe of recordBlit. Set before init().
    void setReadable(bool on) { mReadableWanted = on; }
    // How the window's compositor treats the images' alpha; OPAQUE (the
    // default) where the surface does not support the one asked for. Set
    // before init().
    void setCompositeAlpha(VkCompositeAlphaFlagBitsKHR alpha) { mAlphaWanted = alpha; }

    // Acquire the next image, (re)creating the swapchain if the window changed.
    // Returns false when nothing can be presented this frame (minimized, lost).
    // slot: the frame-in-flight slot (each has its own acquire semaphore).
    // timeout (ns): how long to wait for an image the display still holds;
    // 0 for a window that must never hold the frame up (false, counted as
    // not_ready, when none is free).
    bool acquire(U32 slot, U32& index, VkSemaphore& acquired, U64 timeout = UINT64_MAX);

    // Where recordBlit copies the middle pixel of the image it blitted (4
    // bytes in getFormat()), when the images are readable (research/105).
    struct Probe
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };

    // Record: blit src (width x height, in TRANSFER_SRC layout) into image index,
    // flipped vertically, leaving it ready to present.
    void recordBlit(VkCommandBuffer cmd, U32 index, VkImage src, U32 width, U32 height, const Probe* probe = nullptr);

    // Semaphore the frame's submission must signal for this image.
    VkSemaphore renderDone(U32 index) const { return mRenderDone[index]; }

    // Queue the present after the frame's submission. On the thread that
    // acquires, or on LLVKMirror's present thread; then nothing else touches
    // the swapchain until it has returned (LLVKMirror::waitExtraPresents,
    // waitMainPresent).
    void present(U32 index);
    // The current swapchain waits for the display (FIFO, FIFO_RELAXED: VSync
    // on), so frame pacing may use its presents.
    bool waitsForDisplay() const
    {
        return mPresentMode == VK_PRESENT_MODE_FIFO_KHR || mPresentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    // The image acquire() gave will not be presented (the frame's submission
    // failed): the next acquire recreates the swapchain, which gives it back.
    void dropUnpresented() { mStale = true; }
    // The window's surface is gone (VK_ERROR_SURFACE_LOST_KHR) and new ones
    // made in its place (acquire() tries every SURFACE_RETRY_DELAY) kept
    // failing for SURFACE_LOST_LIMIT, counted while the window could show
    // anything (fix-surfacelost). For the main window the present hook tells
    // the user and ends the viewer (fix-submitfail C). A detached window
    // keeps trying, less often, and is false again once a present is taken.
    bool surfaceLost() const { return mSurfaceLost.load(std::memory_order_relaxed); }

    // --- Frame pacing (research/94) ---
    // Whether to pace (LLRHI::sFramePacing), given every frame like the swap
    // interval. Pacing applies only to FIFO and FIFO_RELAXED (VSync on) and
    // needs VK_KHR_present_wait; otherwise frames run as before.
    void setPacing(bool on) { mPacingWanted = on; }
    // Once a frame after the present, from the thread that presents: while
    // pacing, waits until the present before the last one is on screen, so
    // the next frame starts on the display's rhythm with at most one frame
    // queued behind the one shown. Returns at once when not pacing, and when
    // the GPU sets the pace (gpu_behind: LLVKMirror::gpuWasBehind) while the
    // display has caught up with it: nothing is queued to wait out, and the
    // wait would only leave the GPU idle until the next vertical blank.
    void waitForDisplay(bool gpu_behind);
    // For the FrameCost line: "on", "off", or why pacing is not in use.
    const char* pacingState() const;
    // Waits skipped as GPU-bound since the last call.
    U32 takeGPUBoundSkips();

    U32 getWidth() const { return mExtent.width; }
    U32 getHeight() const { return mExtent.height; }
    VkFormat getFormat() const { return mFormat; }
    VkPresentModeKHR getPresentMode() const { return mPresentMode; }
    bool isReadable() const { return mReadable; }
    // The current swapchain's compositeAlpha, and those the surface offered.
    VkCompositeAlphaFlagBitsKHR getCompositeAlpha() const { return mAlpha; }
    VkCompositeAlphaFlagsKHR getSupportedCompositeAlpha() const { return mAlphaSupported; }

    // What acquire() and present() came to since the last takeResults(), for
    // logs (research/87: the detached windows' LLRHI_DETACH_LOG).
    struct Results
    {
        U32 acquired = 0;
        U32 no_size = 0;            // minimized or hidden: nothing to present
        U32 out_of_date = 0;
        U32 recreate_failed = 0;    // a creation failed, or waits to be tried again (fix-submitfail D)
        U32 acquire_failed = 0;     // any other error (last_acquire_error)
        VkResult last_acquire_error = VK_SUCCESS;
        U32 presented = 0;
        U32 suboptimal = 0;         // presented, swapchain to be recreated
        U32 present_failed = 0;     // last_present_error
        VkResult last_present_error = VK_SUCCESS;
        U32 not_ready = 0;          // acquire with a timeout found no free image
        F64 acquire_ms = 0.0;       // CPU time in acquire() and present(), with setTimed
        F64 present_ms = 0.0;
    };
    // Whether takeResults() also carries the CPU time of acquire and present.
    void setTimed(bool on) { mTimed = on; }
    Results takeResults();

private:
    bool create();
    void fail(const std::string& reason);
    VkPresentModeKHR choosePresentMode(VkPhysicalDevice gpu) const;
    void report(VkResult result, const char* call);
    // A failed call's result: notes a lost surface or device (fix-submitfail C).
    void noteLost(VkResult result);
    // After VK_ERROR_SURFACE_LOST_KHR: a new surface on the same window, the
    // swapchain to be made again. False when the window is gone or the
    // driver refuses.
    bool recreateSurface();
    // acquire() while a new surface is wanted: counts the time lost, makes a
    // new surface when SURFACE_RETRY_DELAY (SURFACE_SLOW_RETRY_DELAY in a long
    // loss) has passed since the last try. True when a new one was just made
    // (acquire goes on with it).
    bool retrySurface();
    // A present was taken (VK_SUCCESS or VK_SUBOPTIMAL_KHR): the loss, if
    // any, is over. Nothing short of that ends it: a driver whose new
    // surfaces take a swapchain and give images, but whose presents keep
    // returning VK_ERROR_SURFACE_LOST_KHR, stays lost.
    void surfacePresented();
    void destroySwapchain();
    bool pacing() const;

    void* mWindow = nullptr;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D mExtent = { 0, 0 };
    std::vector<VkImage> mImages;
    std::vector<VkSemaphore> mRenderDone;   // one per swapchain image
    VkSemaphore mAcquired[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };   // per frame-in-flight slot
    bool mStale = false;
    bool mCreateFailed = false;          // last creation failed (log the retries once)
    // After a failed creation, the next try waits until then, unless the
    // window's client size changes first (fix-submitfail D). The first retry
    // after a success is at the next acquire.
    std::chrono::steady_clock::time_point mRetryAt;
    U32 mCreateFailures = 0;             // in a row
    // fix-submitfail C: a call returned VK_ERROR_SURFACE_LOST_KHR (any thread);
    // acquire() makes a new surface.
    std::atomic<bool> mSurfaceRetry{ false };
    // fix-surfacelost: the surface is lost and a new one is wanted
    // (mSurfaceNeeded); the loss lasts (mLosing, since mLostSince) until a
    // present is taken (surfacePresented). mLostFor is counted at each
    // acquire while a new surface is wanted (at most SURFACE_LOST_STEP from
    // one to the next, nothing while the window cannot show anything) up to
    // SURFACE_LOST_LIMIT.
    bool mSurfaceNeeded = false;
    bool mLosing = false;
    std::chrono::steady_clock::time_point mLostSince;
    std::chrono::steady_clock::duration mLostFor{};
    std::chrono::steady_clock::time_point mLostSeenAt;      // the last acquire that counted
    std::chrono::steady_clock::time_point mSurfaceRetryAt;  // the next new surface not before then
    std::chrono::steady_clock::time_point mSurfaceTriedAt;  // the last new surface tried
    bool mLongLossNoted = false;    // this loss's line at SURFACE_LONG_LOSS (not counted towards the limit) is logged
    // "Making a new surface" lines: at most one per RECREATE_LOG_PERIOD.
    std::chrono::steady_clock::time_point mSurfaceLoggedAt;
    U32 mSurfaceTriesUnlogged = 0;
    // "Recreating the swapchain" lines: at most one per RECREATE_LOG_PERIOD
    // (a window being dragged to a new size recreates it every frame); the
    // next line counts those left out.
    std::chrono::steady_clock::time_point mRecreateLoggedAt;
    U32 mRecreatesUnlogged = 0;
    // VK_SUBOPTIMAL_KHR from a present asks for a new swapchain (mSuboptimal).
    // A driver that keeps returning it for a swapchain made at the surface's
    // size would get one every frame: after SUBOPTIMAL_RECREATES in a row at
    // the same size (no present returning VK_SUCCESS between), presents go on
    // with it until something else recreates it.
    bool mSuboptimal = false;
    U32 mSuboptimalRecreates = 0;
    std::atomic<bool> mSurfaceLost{ false };    // surfaceLost (presents of detached windows run on a thread of their own)
    S32 mSwapInterval = 1;               // GL swap interval to follow (see setSwapInterval)
    bool mNoTearing = false;             // setNoTearing
    U32 mImageCountWanted = 0;           // setImageCount
    U32 mImageCount = 0;                 // of the current swapchain (for its log line)
    bool mReadableWanted = false;        // setReadable
    bool mTimed = false;                 // setTimed
    bool mReadable = false;              // the current swapchain's images have TRANSFER_SRC
    VkCompositeAlphaFlagBitsKHR mAlphaWanted = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;  // setCompositeAlpha
    VkCompositeAlphaFlagBitsKHR mAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;        // of the current swapchain
    VkCompositeAlphaFlagsKHR mAlphaSupported = 0;
    VkPresentModeKHR mPresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;   // of the current swapchain
    std::vector<VkResult> mReported;     // results already logged
    std::string mFailure;                // failure()
    std::vector<std::string> mFailuresLogged;   // fail() reasons already logged
    // research/83: acquire asks the surface for its size only when the window's
    // client size moved from these (or now and then), not every frame.
    U32 mClientWidth = 0;
    U32 mClientHeight = 0;
    U32 mAcquires = 0;
    // Frame pacing: present ids only grow (across swapchains too); the last
    // three presents queued on this swapchain (0: none, or one without an id).
    bool mPacingWanted = false;
    U64 mPresentId = 0;
    U64 mLatestPresent = 0;
    U64 mPreviousPresent = 0;
    U64 mOlderPresent = 0;          // the one before mPreviousPresent
    U32 mPaceTimeouts = 0;          // waits in a row that timed out
    U32 mPaceSuspended = 0;         // presents left before pacing is tried again
    U32 mGPUBoundSkips = 0;         // takeGPUBoundSkips
    Results mResults;               // takeResults
};

#endif // LL_LLVKSWAPCHAIN_H
