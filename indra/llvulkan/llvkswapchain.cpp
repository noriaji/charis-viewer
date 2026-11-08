/**
 * @file llvkswapchain.cpp
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

#include "linden_common.h"

#include "llvkswapchain.h"
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llrhi.h"

#include <algorithm>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <vulkan/vulkan_win32.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace
{
    // mAcquired: one acquire semaphore per frame in flight, picked with slot % 2.
    static_assert(LLVKMirror::FRAMES_IN_FLIGHT == 2, "LLVKSwapchain keeps two acquire semaphores, picked with slot % 2");

    // research/83: the surface's size is asked for only when the window's
    // client size changed (and every CAPS_CHECK_EVERY acquires, in case),
    // not every frame. LLRHI_SWAPCHAIN_CAPS_EVERY_FRAME=1 asks every frame as before.
    constexpr U32 CAPS_CHECK_EVERY = 120;

    // Frame pacing (research/94). A present not shown within this long is
    // taken as not reported (e.g. a driver that never completes the wait for
    // a hidden window), not as a slow frame.
    constexpr U64 PACE_TIMEOUT_NS = 100ull * 1000 * 1000;
    // That many timeouts in a row stop pacing for PACE_RETRY_PRESENTS presents
    // (about 10 s at 60 Hz), so a window that never reports costs a frame
    // rate of 1 / PACE_TIMEOUT_NS only briefly.
    constexpr U32 PACE_TIMEOUTS_TO_SUSPEND = 2;
    constexpr U32 PACE_RETRY_PRESENTS = 600;

    // fix-submitfail D: after a failed swapchain creation, the next try waits
    // this long (or until the window's client size changes), so a window the
    // driver keeps refusing does not cost a creation every frame.
    constexpr std::chrono::milliseconds CREATE_RETRY_DELAY(500);
    // "Recreating the swapchain" lines at most this often.
    constexpr std::chrono::seconds RECREATE_LOG_PERIOD(2);
    // Recreations in a row for VK_SUBOPTIMAL_KHR alone at an unchanged size
    // before presents go on with the swapchain they have.
    constexpr U32 SUBOPTIMAL_RECREATES = 3;
    // fix-surfacelost: after VK_ERROR_SURFACE_LOST_KHR a new surface is made
    // at once, then again this long after the last try while it stays lost.
    // A loss of tens to hundreds of ms (waking from sleep, displays being
    // reconnected, a full screen switch, a GPU switch, a remote desktop
    // connecting) passes without ending the viewer.
    constexpr std::chrono::milliseconds SURFACE_RETRY_DELAY(500);
    // Lost this long in a row: taken as lost for good (fix-submitfail C).
    constexpr std::chrono::seconds SURFACE_LOST_LIMIT(5);
    // The most one acquire adds to the time lost: a sleep, a stop in the
    // debugger or a long hitch between two frames does not use the limit up.
    constexpr std::chrono::milliseconds SURFACE_LOST_STEP(250);
    // A loss going on longer than this (past the limit, or not counted while
    // the window could not show anything): new surfaces and their log lines
    // less often. A detached window (it does not end the viewer) keeps
    // trying at that pace.
    constexpr std::chrono::seconds SURFACE_LONG_LOSS(60);
    constexpr std::chrono::seconds SURFACE_SLOW_RETRY_DELAY(5);
    constexpr std::chrono::seconds SURFACE_SLOW_LOG_PERIOD(60);

    bool caps_every_frame()
    {
        static const bool on = LLRHI::envSet("LLRHI_SWAPCHAIN_CAPS_EVERY_FRAME");
        return on;
    }

    // A failed call's result in failure(): the name of those a window or
    // driver problem gives, else the number.
    std::string result_text(VkResult result)
    {
        switch (result)
        {
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        default:                                return "VkResult " + std::to_string((S32)result);
        }
    }

    // Adds the time until it goes out of scope to *sum_ms (nothing when null).
    struct CallTimer
    {
        explicit CallTimer(F64* sum_ms) : mSum(sum_ms)
        {
            if (mSum)
            {
                mStart = std::chrono::steady_clock::now();
            }
        }
        ~CallTimer()
        {
            if (mSum)
            {
                *mSum += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - mStart).count();
            }
        }
        F64* mSum;
        std::chrono::steady_clock::time_point mStart;
    };

    // The window's client size now (0 x 0 when unknown).
    void client_size(void* window, U32& width, U32& height)
    {
        width = height = 0;
#if LL_WINDOWS
        RECT rect;
        if (window && GetClientRect((HWND)window, &rect))
        {
            width = (U32)llmax(rect.right - rect.left, (LONG)0);
            height = (U32)llmax(rect.bottom - rect.top, (LONG)0);
        }
#endif
    }

    // Whether the window could show anything now: not while it is minimized
    // or hidden or on no display (displays still waking up or being
    // reconnected), nor in a remote desktop session (its display comes and
    // goes with the connection), nor while this session has no input desktop
    // (the computer locked, a remote desktop disconnected, another user
    // switched to). A lost surface is waited for then without counting the
    // time (fix-surfacelost). A window that is gone counts: it does not come back.
    bool window_can_show(void* window)
    {
#if LL_WINDOWS
        const HWND hwnd = (HWND)window;
        if (!hwnd || !IsWindow(hwnd))
        {
            return true;
        }
        if (IsIconic(hwnd) || !IsWindowVisible(hwnd) || !LLRHI::windowOnADisplay(window, true))    // (shared with plus/background)
        {
            return false;
        }
#endif
        return true;
    }

    S64 ms_of(std::chrono::steady_clock::duration time)
    {
        return (S64)std::chrono::duration_cast<std::chrono::milliseconds>(time).count();
    }
}

bool LLVKSwapchain::init(void* hwnd)
{
#if LL_WINDOWS
    LLVKDevice& device = LLVKDevice::instance();
    mFailure.clear();
    mSurfaceLost.store(false, std::memory_order_relaxed);
    mSurfaceRetry.store(false, std::memory_order_relaxed);
    mSurfaceNeeded = false;
    mLosing = false;
    mLostFor = {};
    mSurfaceRetryAt = {};
    if (!device.isReady() || !hwnd)
    {
        fail(hwnd ? "no Vulkan device" : "no window");
        return false;
    }
    mWindow = hwnd;

    VkWin32SurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    info.hinstance = GetModuleHandle(nullptr);
    info.hwnd = (HWND)hwnd;
    const VkResult surface = vkCreateWin32SurfaceKHR(device.getInstance(), &info, nullptr, &mSurface);
    if (surface != VK_SUCCESS)
    {
        fail("vkCreateWin32SurfaceKHR failed: " + result_text(surface));
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device.getPhysicalDevice(), device.getGraphicsQueueFamily(), mSurface, &supported);
    if (!supported)
    {
        fail("the GPU's graphics queue cannot present to the window");
        shutdown();
        return false;
    }

    VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (VkSemaphore& s : mAcquired)
    {
        const VkResult created = vkCreateSemaphore(device.getDevice(), &semaphore, nullptr, &s);
        if (created != VK_SUCCESS)
        {
            // fix-round71 5d: acquire() would be handed a null semaphore.
            s = VK_NULL_HANDLE;
            fail("vkCreateSemaphore failed: " + result_text(created));
            shutdown();
            return false;
        }
    }
    // Started minimized: keep the surface, acquire() creates the swapchain
    // once the window has a size.
    VkSurfaceCapabilitiesKHR caps = {};
    const VkResult caps_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.getPhysicalDevice(), mSurface, &caps);
    if (caps_result != VK_SUCCESS)
    {
        noteLost(caps_result);
        fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: " + result_text(caps_result));
        shutdown();
        return false;
    }
    const bool minimized = caps.currentExtent.width == 0 || caps.currentExtent.height == 0;
    if (!minimized && !create())
    {
        if (mFailure.empty())
        {
            fail("the swapchain could not be created");
        }
        shutdown();
        return false;
    }
    LL_INFOS("Vulkan") << "Presenting through Vulkan: " << mExtent.width << "x" << mExtent.height << ", "
                       << mImages.size() << " images" << LL_ENDL;
    return true;
#else
    return false;
#endif
}

namespace
{
    const char* presentModeName(VkPresentModeKHR mode)
    {
        switch (mode)
        {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "other";
        }
    }
}

void LLVKSwapchain::setSwapInterval(S32 interval)
{
    if (interval != mSwapInterval)
    {
        mSwapInterval = interval;
        mStale = true;      // the present mode is fixed per swapchain
    }
}

void LLVKSwapchain::setNoTearing(bool on)
{
    if (on != mNoTearing)
    {
        mNoTearing = on;
        // Only the choice without VSync reads it (choosePresentMode).
        if (mSwapchain && mSwapInterval == 0)
        {
            mStale = true;
        }
    }
}

// The present mode for the GL swap interval the viewer set. FIFO is the only
// mode every implementation has, so each choice falls back to it.
VkPresentModeKHR LLVKSwapchain::choosePresentMode(VkPhysicalDevice gpu) const
{
    U32 count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, mSurface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, mSurface, &count, modes.data());
    modes.resize(count);
    auto has = [&modes](VkPresentModeKHR mode)
    {
        return std::find(modes.begin(), modes.end(), mode) != modes.end();
    };
    if (mSwapInterval == 0)
    {
        // VSync off: no waiting for the display, like wglSwapIntervalEXT(0).
        // IMMEDIATE may tear (as GL does); MAILBOX does not tear and still
        // never blocks, the next best for latency (the first choice with
        // setNoTearing).
        if (mNoTearing && has(VK_PRESENT_MODE_MAILBOX_KHR))
        {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
        {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))
        {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    else if (mSwapInterval < 0 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
    {
        // Adaptive vsync (WGL_EXT_swap_control_tear): a late frame shows at once.
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool LLVKSwapchain::create()
{
    LLVKDevice& device = LLVKDevice::instance();
    VkPhysicalDevice gpu = device.getPhysicalDevice();

    VkSurfaceCapabilitiesKHR caps = {};
    const VkResult caps_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, mSurface, &caps);
    if (caps_result != VK_SUCCESS)
    {
        noteLost(caps_result);
        fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: " + result_text(caps_result));
        return false;
    }
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
    {
        return false;   // minimized
    }
    // Only a present's VK_SUBOPTIMAL_KHR asks for this one, at the size the
    // last one had (counted towards SUBOPTIMAL_RECREATES).
    const bool for_suboptimal = mSuboptimal && !mStale && mSwapchain && caps.currentExtent.width == mExtent.width &&
                                caps.currentExtent.height == mExtent.height;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        fail("the window's images cannot be copied to (no TRANSFER_DST usage)");
        return false;
    }

    // The frame holds GL's final (already encoded) values: present them unchanged.
    U32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, mSurface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, mSurface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
    {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = f;
            break;
        }
    }
    mFormat = chosen.format;

    const VkPresentModeKHR mode = choosePresentMode(gpu);

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = mSurface;
    info.minImageCount = llmax(caps.minImageCount, 2u);
    if (mImageCountWanted)
    {
        // research/179 6.6 4 (LLRHI_PRESENT_IMAGES): as asked, within the surface's range.
        info.minImageCount = llmax(mImageCountWanted, info.minImageCount);
        if (caps.maxImageCount)
        {
            info.minImageCount = llmin(info.minImageCount, caps.maxImageCount);
        }
    }
    else if (caps.maxImageCount)
    {
        info.minImageCount = llmin(info.minImageCount + 1, caps.maxImageCount);
    }
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = caps.currentExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const bool readable = mReadableWanted && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (readable)
    {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    const VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & mAlphaWanted) ? mAlphaWanted
                                                                                         : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = mSwapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult result = vkCreateSwapchainKHR(device.getDevice(), &info, nullptr, &swapchain);
    if (mSwapchain)
    {
        destroySwapchain();
    }
    if (result != VK_SUCCESS)
    {
        noteLost(result);   // fix-surfacelost: a lost surface can show first here
        fail("vkCreateSwapchainKHR failed: " + result_text(result));
        return false;
    }
    mSwapchain = swapchain;
    mExtent = caps.currentExtent;
    mReadable = readable;
    mAlpha = alpha;
    mAlphaSupported = caps.supportedCompositeAlpha;
    vkGetSwapchainImagesKHR(device.getDevice(), mSwapchain, &count, nullptr);
    mImages.resize(count);
    vkGetSwapchainImagesKHR(device.getDevice(), mSwapchain, &count, mImages.data());
    if (mode != mPresentMode || count != mImageCount)
    {
        LL_INFOS("Vulkan") << "Swapchain present mode " << presentModeName(mode) << " (swap interval " << mSwapInterval << "), "
                           << count << " images (asked for " << info.minImageCount << ")" << LL_ENDL;
    }
    mPresentMode = mode;
    mImageCount = count;
    VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    mRenderDone.resize(count);
    for (VkSemaphore& s : mRenderDone)
    {
        const VkResult created = vkCreateSemaphore(device.getDevice(), &semaphore, nullptr, &s);
        if (created != VK_SUCCESS)
        {
            // fix-round71 5d: the frame would signal a null semaphore. As a
            // failed creation: the swapchain goes, acquire() tries again.
            s = VK_NULL_HANDLE;
            fail("vkCreateSemaphore failed: " + result_text(created));
            destroySwapchain();
            return false;
        }
    }
    mStale = false;
    mSuboptimal = false;
    mSuboptimalRecreates = for_suboptimal ? mSuboptimalRecreates + 1 : 0;
    client_size(mWindow, mClientWidth, mClientHeight);
    // The old swapchain is retired: its presents can no longer be waited for.
    mLatestPresent = mPreviousPresent = mOlderPresent = 0;
    mPaceTimeouts = 0;
    return true;
}

void LLVKSwapchain::destroySwapchain()
{
    VkDevice device = LLVKDevice::instance().getDevice();
    LLVKDevice::instance().waitIdle();
    // The swapchain first: vkDeviceWaitIdle does not cover the presents, which
    // may still wait on the render-done semaphores until it is gone.
    mImages.clear();
    if (mSwapchain)
    {
        vkDestroySwapchainKHR(device, mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
    for (VkSemaphore s : mRenderDone)
    {
        vkDestroySemaphore(device, s, nullptr);
    }
    mRenderDone.clear();
}

// Each distinct reason is logged once: acquire() retries a failed creation
// (fix-submitfail D).
void LLVKSwapchain::fail(const std::string& reason)
{
    mFailure = reason;
    if (std::find(mFailuresLogged.begin(), mFailuresLogged.end(), reason) == mFailuresLogged.end())
    {
        mFailuresLogged.push_back(reason);
        LL_WARNS("Vulkan") << "Swapchain: " << reason << LL_ENDL;
    }
}

void LLVKSwapchain::noteLost(VkResult result)
{
    if (result == VK_ERROR_SURFACE_LOST_KHR)
    {
        mSurfaceRetry.store(true, std::memory_order_relaxed);     // acquire() makes a new one
    }
    else if (result == VK_ERROR_DEVICE_LOST)
    {
        // Not the fault report: this may be the detached windows' present thread.
        LLVKDevice::instance().noteDeviceLost();
    }
}

bool LLVKSwapchain::recreateSurface()
{
#if LL_WINDOWS
    if (!mWindow || !IsWindow((HWND)mWindow))
    {
        return false;
    }
    LLVKDevice& device = LLVKDevice::instance();
    destroySwapchain();     // before its surface (waits for the GPU)
    if (mSurface)
    {
        vkDestroySurfaceKHR(device.getInstance(), mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }
    VkWin32SurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    info.hinstance = GetModuleHandle(nullptr);
    info.hwnd = (HWND)mWindow;
    const VkResult surface = vkCreateWin32SurfaceKHR(device.getInstance(), &info, nullptr, &mSurface);
    if (surface != VK_SUCCESS)
    {
        mSurface = VK_NULL_HANDLE;
        fail("vkCreateWin32SurfaceKHR failed after the surface was lost: " + result_text(surface));
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device.getPhysicalDevice(), device.getGraphicsQueueFamily(), mSurface, &supported);
    if (!supported)
    {
        fail("the GPU's graphics queue cannot present to the window's new surface");
        return false;
    }
    mStale = true;          // the next acquire makes the swapchain
    mCreateFailed = false;
    mCreateFailures = 0;    // a new surface: its first failed creation is retried at the next acquire
    return true;
#else
    return false;
#endif
}

bool LLVKSwapchain::retrySurface()
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!mLosing)
    {
        mLosing = true;
        mLostFor = {};
        mLostSince = now;
        mLongLossNoted = false;
    }
    else if (window_can_show(mWindow))
    {
        mLostFor += std::min<std::chrono::steady_clock::duration>(now - mLostSeenAt, SURFACE_LOST_STEP);
    }
    mLostSeenAt = now;
    if (mLostFor >= SURFACE_LOST_LIMIT && !surfaceLost())
    {
        LL_WARNS("Vulkan") << "Swapchain: the window's surface stayed lost (VK_ERROR_SURFACE_LOST_KHR) for "
                           << ms_of(mLostFor) << " ms while it could show frames; taken as lost" << LL_ENDL;
        mSurfaceLost.store(true, std::memory_order_relaxed);
    }
    if (!mLongLossNoted && !surfaceLost() && now - mLostSince >= SURFACE_LONG_LOSS)
    {
        // Not counted towards the limit while the window could not show frames
        // (a remote desktop session counts as such for as long as it lasts): the
        // window goes on waiting for its surface, frozen, while the viewer runs
        // and can be closed as usual. Said once a loss, so a frozen window can be
        // told apart in the log (fix-round11 6).
        LL_WARNS("Vulkan") << "Swapchain: the window's surface has been lost for " << ms_of(now - mLostSince) << " ms, "
                           << ms_of(mLostFor) << " ms of it counted towards " << SURFACE_LOST_LIMIT.count()
                           << " s (not while the window is minimized, hidden or on no display, the session is a remote desktop"
                              " or the computer is locked); the window shows nothing until a new surface works"
#if LL_WINDOWS
                           << (GetSystemMetrics(SM_REMOTESESSION) ? " (a remote desktop session now)" : "")
#endif
                           << LL_ENDL;
        mLongLossNoted = true;
    }
    if (now < mSurfaceRetryAt)
    {
        return false;
    }
    const bool long_loss = surfaceLost() || now - mLostSince >= SURFACE_LONG_LOSS;
    const std::chrono::steady_clock::duration retry_delay = long_loss ? std::chrono::steady_clock::duration(SURFACE_SLOW_RETRY_DELAY)
                                                                      : std::chrono::steady_clock::duration(SURFACE_RETRY_DELAY);
    const std::chrono::steady_clock::duration log_period = long_loss ? std::chrono::steady_clock::duration(SURFACE_SLOW_LOG_PERIOD)
                                                                     : std::chrono::steady_clock::duration(RECREATE_LOG_PERIOD);
    mSurfaceRetryAt = now + retry_delay;
    mSurfaceTriedAt = now;
    // A surface that keeps getting lost would log a line every try.
    ++mSurfaceTriesUnlogged;
    if (now - mSurfaceLoggedAt >= log_period)
    {
        LL_WARNS("Vulkan") << "Swapchain: the window's surface was lost (VK_ERROR_SURFACE_LOST_KHR); making a new one ("
                           << ms_of(now - mLostSince) << " ms since the loss, " << ms_of(mLostFor) << " ms of it counted towards "
                           << SURFACE_LOST_LIMIT.count() << " s; the next try in " << ms_of(retry_delay) << " ms"
                           << (mSurfaceTriesUnlogged > 1 ? ", " + std::to_string(mSurfaceTriesUnlogged - 1) + " more tries since the last such line"
                                                         : std::string())
                           << ")" << LL_ENDL;
        mSurfaceLoggedAt = now;
        mSurfaceTriesUnlogged = 0;
    }
    if (!recreateSurface())
    {
        return false;
    }
    mSurfaceNeeded = false;
    return true;
}

void LLVKSwapchain::surfacePresented()
{
    if (mLosing)
    {
        LL_INFOS("Vulkan") << "Swapchain: the window shows frames again " << ms_of(std::chrono::steady_clock::now() - mLostSince)
                           << " ms after its surface was lost" << LL_ENDL;
        mLosing = false;
        mLostFor = {};
        mSurfaceLost.store(false, std::memory_order_relaxed);
        // A new loss is not held to the slow pace of a long one (fix-round11
        // 6): its first try comes SURFACE_RETRY_DELAY after the last one at
        // most (at once when that has passed). Waiting out 5 s, the time
        // counted could reach SURFACE_LOST_LIMIT before the first try; with
        // no wait at all, a surface lost again after each frame shown would
        // be made again (the GPU waited for) each time.
        mSurfaceRetryAt = std::min(mSurfaceRetryAt, mSurfaceTriedAt + std::chrono::steady_clock::duration(SURFACE_RETRY_DELAY));
    }
}

void LLVKSwapchain::shutdown()
{
    mSurfaceNeeded = false;     // hasSurface() is false from here
    VkDevice device = LLVKDevice::instance().getDevice();
    if (!device)
    {
        return;
    }
    destroySwapchain();
    for (VkSemaphore& s : mAcquired)
    {
        if (s)
        {
            vkDestroySemaphore(device, s, nullptr);
            s = VK_NULL_HANDLE;
        }
    }
    if (mSurface)
    {
        vkDestroySurfaceKHR(LLVKDevice::instance().getInstance(), mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }
}

// A failed acquire or present leaves the window showing an old frame: log each
// distinct result once, so a frozen picture can be told apart from a slow one.
void LLVKSwapchain::report(VkResult result, const char* call)
{
    if (std::find(mReported.begin(), mReported.end(), result) == mReported.end())
    {
        mReported.push_back(result);
        LL_WARNS("Vulkan") << call << " returned " << (S32)result << LL_ENDL;
    }
}

bool LLVKSwapchain::acquire(U32 slot, U32& index, VkSemaphore& acquired, U64 timeout)
{
    CallTimer timer(mTimed ? &mResults.acquire_ms : nullptr);
    if (mSurfaceRetry.exchange(false, std::memory_order_relaxed))
    {
        mSurfaceNeeded = true;
    }
    // fix-surfacelost: nothing to present until a new surface works. Like
    // minimized, frames go on without being shown. Past the limit too
    // (surfaceLost: the main window ends the viewer after this frame; a
    // detached window keeps trying, slower).
    if ((mSurfaceNeeded && !retrySurface()) || !mSurface)
    {
        return false;
    }
    if (mCreateFailed && mCreateFailures > 1)
    {
        // fix-submitfail D: after the first retry, the next one after a
        // delay, or once the window's size moves from the one the last try had.
        U32 width = 0, height = 0;
        client_size(mWindow, width, height);
        if (width == mClientWidth && height == mClientHeight && std::chrono::steady_clock::now() < mRetryAt)
        {
            ++mResults.recreate_failed;
            return false;
        }
    }
    // The surface's size is the window's client size (Win32): a driver call
    // every frame only to find it unchanged is skipped (research/83).
    bool check = !mSwapchain || mStale || mSuboptimal || mCreateFailed || caps_every_frame() || ++mAcquires % CAPS_CHECK_EVERY == 0;
    if (!check)
    {
        U32 width = 0, height = 0;
        client_size(mWindow, width, height);
        check = !width || !height || width != mClientWidth || height != mClientHeight;
    }
    if (check)
    {
        VkSurfaceCapabilitiesKHR caps = {};
        const VkResult caps_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(LLVKDevice::instance().getPhysicalDevice(), mSurface, &caps);
        if (caps_result != VK_SUCCESS)
        {
            // Until now a lost surface read as size 0, minimized for good (fix-submitfail C).
            noteLost(caps_result);
            ++mResults.acquire_failed;
            mResults.last_acquire_error = caps_result;
            report(caps_result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
            return false;
        }
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
        {
            ++mResults.no_size;
            return false;   // minimized: nothing to present, and nothing to log every frame
        }
        if (!mSwapchain || mStale || mSuboptimal || caps.currentExtent.width != mExtent.width ||
            caps.currentExtent.height != mExtent.height)
        {
            if (!mCreateFailed)
            {
                // fix-submitfail D: a window dragged to a new size recreates
                // it every frame; one line per RECREATE_LOG_PERIOD at most.
                ++mRecreatesUnlogged;
                const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                if (now - mRecreateLoggedAt >= RECREATE_LOG_PERIOD)
                {
                    LL_INFOS("Vulkan") << "Recreating the swapchain: " << caps.currentExtent.width << "x" << caps.currentExtent.height
                                       << (mStale ? " (out of date, new swap interval or an image not presented)"
                                                  : mSuboptimal ? " (suboptimal)" : "")
                                       << (mRecreatesUnlogged > 1 ? ", " + std::to_string(mRecreatesUnlogged - 1) + " more since the last such line"
                                                                  : std::string())
                                       << LL_ENDL;
                    mRecreateLoggedAt = now;
                    mRecreatesUnlogged = 0;
                }
            }
            // On failure the old swapchain is gone too (it was handed over as
            // oldSwapchain): try again at the next acquire, then after
            // CREATE_RETRY_DELAY.
            mCreateFailed = !create();
            mCreateFailures = mCreateFailed ? mCreateFailures + 1 : 0;
            if (mCreateFailed)
            {
                ++mResults.recreate_failed;
                LL_WARNS_ONCE("Vulkan") << "swapchain recreation failed" << LL_ENDL;
                mRetryAt = std::chrono::steady_clock::now() + CREATE_RETRY_DELAY;
                client_size(mWindow, mClientWidth, mClientHeight);
                return false;
            }
        }
        else
        {
            // The surface is as it was: what the client size reads now goes with it.
            client_size(mWindow, mClientWidth, mClientHeight);
        }
    }
    VkResult result = vkAcquireNextImageKHR(LLVKDevice::instance().getDevice(), mSwapchain, timeout, mAcquired[slot % 2], VK_NULL_HANDLE, &index);
    if (result == VK_NOT_READY || result == VK_TIMEOUT)
    {
        ++mResults.not_ready;   // every image still with the display: nothing to present this frame
        return false;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        ++mResults.out_of_date;
        mStale = true;
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        noteLost(result);
        ++mResults.acquire_failed;
        mResults.last_acquire_error = result;
        report(result, "vkAcquireNextImageKHR");
        return false;
    }
    ++mResults.acquired;
    acquired = mAcquired[slot % 2];
    return true;
}

void LLVKSwapchain::recordBlit(VkCommandBuffer cmd, U32 index, VkImage src, U32 width, U32 height, const Probe* probe)
{
    VkImage dst = mImages[index];
    LLVKMirror::imageBarrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1);

    // Frame rows are bottom-up (GL storage); the window wants top-down.
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { (S32)width, (S32)height, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, (S32)mExtent.height, 0 };
    blit.dstOffsets[1] = { (S32)mExtent.width, 0, 1 };
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (probe && probe->buffer && mReadable)
    {
        LLVKMirror::imageBarrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy region = {};
        region.bufferOffset = probe->offset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageOffset = { (S32)mExtent.width / 2, (S32)mExtent.height / 2, 0 };
        region.imageExtent = { 1, 1, 1 };
        vkCmdCopyImageToBuffer(cmd, dst, layout, probe->buffer, 1, &region);
    }
    LLVKMirror::imageBarrier(cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, 1);
}

void LLVKSwapchain::present(U32 index)
{
    CallTimer timer(mTimed ? &mResults.present_ms : nullptr);
    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &mRenderDone[index];
    info.swapchainCount = 1;
    info.pSwapchains = &mSwapchain;
    info.pImageIndices = &index;
    // While pacing, the present carries an id waitForDisplay can wait for.
    const U64 id = pacing() ? ++mPresentId : 0;
    VkPresentIdKHR present_id = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
    if (id)
    {
        present_id.swapchainCount = 1;
        present_id.pPresentIds = &id;
        info.pNext = &present_id;
    }
    if (mPaceSuspended)
    {
        --mPaceSuspended;
    }
    VkResult result = LLVKDevice::instance().present(info);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
    {
        mOlderPresent = mPreviousPresent;
        mPreviousPresent = mLatestPresent;
        mLatestPresent = id;
    }
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
    {
        surfacePresented();     // also SUBOPTIMAL: a window whose presents always say so still shows them
    }
    if (result == VK_SUCCESS)
    {
        mSuboptimalRecreates = 0;   // they count in a row only
    }
    else if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        mStale = true;
    }
    else if (result == VK_SUBOPTIMAL_KHR)
    {
        if (mSuboptimalRecreates < SUBOPTIMAL_RECREATES)
        {
            mSuboptimal = true;     // recreated at the next acquire
        }
        else
        {
            LL_INFOS_ONCE("Vulkan") << "Presents keep returning VK_SUBOPTIMAL_KHR at the window's size after " << SUBOPTIMAL_RECREATES
                                    << " new swapchains: presenting on with this one" << LL_ENDL;
        }
    }
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
    {
        ++mResults.presented;
        mResults.suboptimal += result == VK_SUBOPTIMAL_KHR ? 1 : 0;
    }
    else
    {
        noteLost(result);
        ++mResults.present_failed;
        mResults.last_present_error = result;
    }
    if (result != VK_SUCCESS)
    {
        report(result, "vkQueuePresentKHR");
    }
}

LLVKSwapchain::Results LLVKSwapchain::takeResults()
{
    Results results = mResults;
    mResults = Results();
    return results;
}

bool LLVKSwapchain::pacing() const
{
    return mPacingWanted && !mPaceSuspended && LLVKDevice::instance().waitForPresent() &&
           (mPresentMode == VK_PRESENT_MODE_FIFO_KHR || mPresentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

const char* LLVKSwapchain::pacingState() const
{
    if (!mPacingWanted)
    {
        return "off";
    }
    if (!LLVKDevice::instance().waitForPresent())
    {
        return "unavailable (no VK_KHR_present_wait)";
    }
    if (mPresentMode != VK_PRESENT_MODE_FIFO_KHR && mPresentMode != VK_PRESENT_MODE_FIFO_RELAXED_KHR)
    {
        return "not used (VSync off)";
    }
    return mPaceSuspended ? "suspended (presents not reported as shown)" : "on";
}

U32 LLVKSwapchain::takeGPUBoundSkips()
{
    const U32 skips = mGPUBoundSkips;
    mGPUBoundSkips = 0;
    return skips;
}

void LLVKSwapchain::waitForDisplay(bool gpu_behind)
{
    // The frame just presented may stay queued; the one before it must be on
    // screen before the next frame starts.
    if (!pacing() || !mSwapchain || !mPreviousPresent)
    {
        return;
    }
    const PFN_vkWaitForPresentKHR wait_for_present = LLVKDevice::instance().waitForPresent();
    const VkDevice device = LLVKDevice::instance().getDevice();
    // GPU-bound (research/103 F1): the GPU has only just finished the one
    // before the last, which shows at the next vertical blank, and the present
    // before that is on screen already: nothing is queued. Waiting would not
    // shorten the queue, only start the next frame late, and the GPU would sit
    // idle between the last frame and that one.
    if (gpu_behind && mOlderPresent)
    {
        const VkResult shown = wait_for_present(device, mSwapchain, mOlderPresent, 0);
        if (shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR)
        {
            ++mGPUBoundSkips;
            return;
        }
    }
    const VkResult result = wait_for_present(device, mSwapchain, mPreviousPresent, PACE_TIMEOUT_NS);
    if (result == VK_TIMEOUT)
    {
        if (++mPaceTimeouts >= PACE_TIMEOUTS_TO_SUSPEND)
        {
            // Once: a driver that never reports would log this every 10 s
            // (the FrameCost line shows each suspended period).
            LL_INFOS_ONCE("Vulkan") << "Frame pacing paused for " << PACE_RETRY_PRESENTS << " presents: "
                                    << mPaceTimeouts << " presents in a row not reported as shown" << LL_ENDL;
            mPaceTimeouts = 0;
            mPaceSuspended = PACE_RETRY_PRESENTS;
        }
        return;
    }
    mPaceTimeouts = 0;
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        mStale = true;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        noteLost(result);
        report(result, "vkWaitForPresentKHR");
    }
}
