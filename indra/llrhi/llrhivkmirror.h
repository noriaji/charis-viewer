/**
 * @file llrhivkmirror.h
 * @brief The Vulkan renderer's frame loop: GL calls go into the Vulkan frame, Vulkan presents it.
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

#ifndef LL_LLRHIVKMIRROR_H
#define LL_LLRHIVKMIRROR_H

#include <string>

// The Vulkan renderer: the GL calls go into LLVKMirror's frame (LLVKGL) and
// Vulkan presents it to the window; GL draws nothing. Frames can be saved as
// <dir>/vk_<n>.png: those listed in LLRHI_VK_CAPTURE (comma separated present
// counts), every LLRHI_VK_CAPTURE_EVERY-th, and the next one after a
// <dir>/capture.now file appears (dir: LLRHI_VK_CAPTURE_DIR, else
// LLRHI_CUE_DIR; nothing is saved or looked for without one).
namespace LLRHIVKMirror
{
    // When Vulkan cannot start on this PC, LLRHI::sVulkanUnavailableExit tells
    // the user why and ends the viewer; this never returns then.
    void install(bool validation);
    // The viewer is closing (or recreating) its GL context: saves the Vulkan
    // pipeline cache, since the Vulkan device itself is never shut down.
    void onGLShutdown();
    // A main loop that shows nothing (minimized): its frame ends unshown (fix-pipeleak).
    void onFrameNotShown();
    // LLRHI::frameRecording (research/261): a Vulkan frame is open (true
    // without a Vulkan build, where the call is not made).
    bool frameRecording();
    // A shader reload is about to release its buffers: every frame in flight
    // completes and what it deferred is freed (fix-round107, LLRHI::freeDeferred).
    void freeDeferred();
    // LLRHI::TrackerHold (research/180 9 3): hold true takes the backend's
    // lock for a batch loop, on the main thread only; true when taken (release
    // with releaseTracker). Either way it counts the loop for the TrackerHold
    // line (every 10 s with the periodic logs).
    bool holdTracker(bool hold);
    void releaseTracker();
}

#endif // LL_LLRHIVKMIRROR_H
