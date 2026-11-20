/**
 * @file llvkvramdiag.h
 * @brief What the device made while VRAM was over the driver's budget (research/354 vram-stall-diag, the device's half).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLVKVRAMDIAG_H
#define LL_LLVKVRAMDIAG_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

typedef struct VmaAllocator_T* VmaAllocator;

// LLRHI_VRAM_DIAG=1: a VKPerf "VRAM diag" log message every 10 s, counting only (research/354 section 5.1, 2 and 3):
//   (2) the images the device made (LLVKDevice::noteResource) and the device memory it took from the driver
//       (vkAllocateMemory, by heap) while VRAM was over the driver's budget, by kind, in the window since the last
//       message and since the start;
//   (3) every memory heap's usage, budget, device memory objects and allocations (VK_EXT_memory_budget, the values
//       VMA holds: VMA asks the driver again for them on every frame index, which LLVKDevice::setFrame already
//       does; this adds no driver query).
// "Over the driver's budget": the usage above the budget as LLVKDevice::vramBudget counts them (the numbers the
// texture budget uses: the sum of the device-local heaps, or of every heap on a GPU that shares the PC's memory
// (setSharedMemoryBudget), with its held budget), taken once a frame at LLVKDevice::setFrame; an
// allocation counts as over when the last such value was. What it does not do: change anything, take a lock beyond
// the ones the hooks are in, or allocate (the counters are fixed atomics; the message is built every 10 s only).
// Off: each hook is one flag test (the environment is read once, at start). The three hooks are in llvkdevice.cpp.
// Threads: resourceMade and memoryAllocated from whichever thread makes the image or the memory (atomic adds); frame
// from the thread that calls LLVKDevice::setFrame (the render thread; the message is built there, under a try-lock,
// so a second caller would skip it). setFrame is called by LLVKGL::resetFrameStats with the tracker lock (LLVKGL::sLock)
// held, so once every 10 s the message is built (a string of a few KB, VMA's heap numbers) and written (one log call)
// under that lock: a log line's cost, once in 10 s, on a diagnostic that is off by default.
// The viewer's half (the textures: what they are, why not BC7) is plus/vramdiag in newview, which cannot be reached
// from here (llvulkan is linked before newview and newview does not include this library).
namespace LLVKVramDiag
{
    // Set once from the environment (LLRHI_VRAM_DIAG).
    extern const bool sOn;

    // What LLVKDevice::noteResource was told (RESOURCE_MADE only): the description it copies ("image WxHxL format F,
    // N levels[, attachable]", "OIT node buffer", "OIT image WxHxL format F") and the bytes of the allocation.
    void noteMade(const char* what, U64 bytes);
    inline void resourceMade(const char* what, U64 bytes)
    {
        if (sOn)
        {
            noteMade(what, bytes);
        }
    }

    // A vkAllocateMemory VMA made (LLVKDevice::onDeviceAllocate): the memory type and the size.
    void noteMemory(VmaAllocator allocator, U32 memory_type, U64 bytes);
    inline void memoryAllocated(VmaAllocator allocator, U32 memory_type, U64 bytes)
    {
        if (sOn)
        {
            noteMemory(allocator, memory_type, bytes);
        }
    }

    // Once a frame (LLVKDevice::setFrame, after VMA's new frame index): refreshes the over-budget state, and every
    // 10 s writes the message.
    void tick();
    inline void frame()
    {
        if (sOn)
        {
            tick();
        }
    }
}

#endif // LL_LLVKVRAMDIAG_H
