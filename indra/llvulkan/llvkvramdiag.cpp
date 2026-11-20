/**
 * @file llvkvramdiag.cpp
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

#include "linden_common.h"

#include "llvkvramdiag.h"

#include "llrhi.h"
#include "llvkdevice.h"

#include "vk_mem_alloc.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>

const bool LLVKVramDiag::sOn = LLRHI::envSet("LLRHI_VRAM_DIAG");

namespace
{
    constexpr U64 MB = 1024ull * 1024ull;
    constexpr U32 HEAPS = VK_MAX_MEMORY_HEAPS;
    constexpr S64 REPORT_NS = 10ll * 1000000000ll;

    // What an image or buffer noteResource was told about is (see classify).
    enum Kind : U32
    {
        K_IMAGE = 0,        // an image that is not drawn into (the textures, the BC7 images, the maps)
        K_IMAGE_BIG,        // ... over 8 MB (what gets memory of its own)
        K_TARGET,           // an image that is drawn into (attachable)
        K_TARGET_BIG,
        K_OIT_NODES,        // the OIT node buffer
        K_OIT_IMAGE,        // the OIT heads and copy (one layer)
        K_OIT_REST,         // the OIT rest image (layers)
        K_OTHER,
        K_COUNT
    };
    const char* const KIND_NAMES[K_COUNT] = { "image, not drawn into (textures)", "image, not drawn into, over 8 MB",
                                              "render target (attachable)",      "render target, over 8 MB",
                                              "OIT node buffer",                 "OIT image (heads, copy)",
                                              "OIT rest image",                  "other" };
    constexpr U64 BIG_BYTES = 8 * MB;

    // vkAllocateMemory sizes: under 1 MB, 1 to 8, 8 to 64, 64 MB and up (the texture and buffer pools' blocks, the
    // attribute pool, memory of its own for a large image).
    constexpr U32 BANDS = 4;
    const char* const BAND_NAMES[BANDS] = { "under 1 MB", "1-8 MB", "8-64 MB", "64 MB and up" };

    U32 band_of(U64 bytes)
    {
        return bytes < MB ? 0 : bytes < 8 * MB ? 1 : bytes < 64 * MB ? 2 : 3;
    }

    // A count and a byte sum, added to from any thread.
    struct Tally
    {
        std::atomic<U64> count{ 0 };
        std::atomic<U64> bytes{ 0 };
        void add(U64 b)
        {
            count.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(b, std::memory_order_relaxed);
        }
    };
    // The same, read: the message's window is the difference of two of them.
    struct Seen
    {
        U64 count = 0;
        U64 bytes = 0;
        Seen() = default;
        explicit Seen(const Tally& t) : count(t.count.load(std::memory_order_relaxed)), bytes(t.bytes.load(std::memory_order_relaxed)) {}
        Seen since(const Seen& before) const
        {
            Seen d;
            d.count = count - before.count;
            d.bytes = bytes - before.bytes;
            return d;
        }
    };

    struct KindTally
    {
        Tally all;
        Tally over;                                 // of them, made while over the budget
        std::atomic<U64> last_usage{ 0 };           // VRAM and budget (bytes) when one was last made over the budget
        std::atomic<U64> last_budget{ 0 };
        std::atomic<U64> last_frame{ 0 };           // the frame count (sFrames) then
    };
    struct HeapTally
    {
        Tally all;
        Tally over;
        Tally band_all[BANDS];
        Tally band_over[BANDS];
    };

    KindTally sKinds[K_COUNT];
    HeapTally sHeaps[HEAPS];

    // The over-budget state, as of the last frame (tick). Written by the render thread, read by any thread.
    std::atomic<bool> sOver{ false };
    std::atomic<bool> sKnown{ false };              // the driver gave a budget at the last frame
    std::atomic<U64> sUsage{ 0 };
    std::atomic<U64> sBudget{ 0 };
    std::atomic<U64> sFrames{ 0 };
    std::atomic<U64> sFramesOver{ 0 };
    std::atomic<U64> sPeakOver{ 0 };                // the most over the budget (bytes) since the last message

    // The counters at one moment: the message's window is the difference of two of them.
    struct Snapshot
    {
        Seen kind_all[K_COUNT], kind_over[K_COUNT];
        Seen heap_all[HEAPS], heap_over[HEAPS];
        U64 frames = 0, frames_over = 0;

        static Snapshot take()
        {
            Snapshot s;
            for (U32 k = 0; k < K_COUNT; ++k)
            {
                s.kind_all[k] = Seen(sKinds[k].all);
                s.kind_over[k] = Seen(sKinds[k].over);
            }
            for (U32 h = 0; h < HEAPS; ++h)
            {
                s.heap_all[h] = Seen(sHeaps[h].all);
                s.heap_over[h] = Seen(sHeaps[h].over);
            }
            s.frames = sFrames.load(std::memory_order_relaxed);
            s.frames_over = sFramesOver.load(std::memory_order_relaxed);
            return s;
        }
    };

    // The report (render thread, under sReportLock).
    std::mutex sReportLock;
    std::atomic<S64> sNextReport{ 0 };              // steady clock ns; 0: not started
    std::atomic<S64> sLastReport{ 0 };              // when the last message was written (or the first frame came)
    Snapshot sPrevious;                             // as of the last message

    S64 now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    U32 read_number(const char*& p)
    {
        char* end = nullptr;
        const unsigned long value = strtoul(p, &end, 10);
        p = end ? end : p;
        return (U32)value;
    }

    // noteResource's descriptions: createImage's "image WxHxL format F, N levels[, attachable]", the OIT's
    // "OIT node buffer" and "OIT image WxHxL format F".
    Kind classify(const char* what, U64 bytes)
    {
        if (!what)
        {
            return K_OTHER;
        }
        if (!strncmp(what, "OIT node buffer", 15))
        {
            return K_OIT_NODES;
        }
        if (!strncmp(what, "OIT image ", 10))
        {
            const char* p = what + 10;
            read_number(p);                         // width
            U32 layers = 0;
            if (*p == 'x')
            {
                read_number(++p);                   // height
                if (*p == 'x')
                {
                    layers = read_number(++p);
                }
            }
            return layers > 1 ? K_OIT_REST : K_OIT_IMAGE;
        }
        if (!strncmp(what, "image ", 6))
        {
            const bool target = strstr(what, ", attachable") != nullptr;
            const bool big = bytes > BIG_BYTES;
            return target ? (big ? K_TARGET_BIG : K_TARGET) : (big ? K_IMAGE_BIG : K_IMAGE);
        }
        return K_OTHER;
    }

    void put_seen(std::ostream& out, const Seen& s)
    {
        out << s.count << " (" << (s.bytes / MB) << " MB)";
    }

    // The heaps (VK_EXT_memory_budget's numbers as VMA holds them, read now: VMA asks the driver again only on a new
    // frame index or after 30 allocations, which it does without us).
    void put_heaps(std::ostream& out, VmaAllocator allocator)
    {
        const VkPhysicalDeviceMemoryProperties* properties = nullptr;
        vmaGetMemoryProperties(allocator, &properties);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(allocator, budgets);
        for (U32 heap = 0; properties && heap < properties->memoryHeapCount && heap < HEAPS; ++heap)
        {
            const VmaBudget& b = budgets[heap];
            const bool local = (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            out << "\n  heap #" << heap << " (" << (local ? "device local" : "not device local") << ", "
                << (properties->memoryHeaps[heap].size / MB) << " MB): heapUsage " << (b.usage / MB) << " MB of heapBudget "
                << (b.budget / MB) << " MB; " << b.statistics.blockCount << " device memory objects (blocks and memory of their own) "
                << (b.statistics.blockBytes / MB) << " MB, " << b.statistics.allocationCount << " allocations "
                << (b.statistics.allocationBytes / MB) << " MB";
        }
    }

    void put_kinds(std::ostream& out, const Snapshot& now, const Snapshot& before)
    {
        out << "\n  images and buffers made (LLVKDevice::noteResource: images, the OIT), while over the driver's budget / all:";
        for (U32 k = 0; k < K_COUNT; ++k)
        {
            const Seen& all = now.kind_all[k];
            const Seen& over = now.kind_over[k];
            if (!all.count)
            {
                continue;
            }
            out << "\n    " << KIND_NAMES[k] << ": window ";
            put_seen(out, over.since(before.kind_over[k]));
            out << " of ";
            put_seen(out, all.since(before.kind_all[k]));
            out << ", since the start ";
            put_seen(out, over);
            out << " of ";
            put_seen(out, all);
            if (over.count)
            {
                out << "; last over at VRAM " << (sKinds[k].last_usage.load(std::memory_order_relaxed) / MB) << " of "
                    << (sKinds[k].last_budget.load(std::memory_order_relaxed) / MB) << " MB, frame "
                    << sKinds[k].last_frame.load(std::memory_order_relaxed) << " of the diag";
            }
        }
    }

    void put_memory(std::ostream& out, VmaAllocator allocator, const Snapshot& now, const Snapshot& before)
    {
        const VkPhysicalDeviceMemoryProperties* properties = nullptr;
        vmaGetMemoryProperties(allocator, &properties);
        out << "\n  vkAllocateMemory by heap (VMA's callback: pool blocks, buffers and memory of their own too), while over / all:";
        for (U32 heap = 0; heap < HEAPS; ++heap)
        {
            const Seen& all = now.heap_all[heap];
            const Seen& over = now.heap_over[heap];
            if (!all.count)
            {
                continue;
            }
            const bool local = properties && heap < properties->memoryHeapCount &&
                               (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
            out << "\n    heap #" << heap << (local ? " (device local)" : "") << ": window ";
            put_seen(out, over.since(before.heap_over[heap]));
            out << " of ";
            put_seen(out, all.since(before.heap_all[heap]));
            out << ", since the start ";
            put_seen(out, over);
            out << " of ";
            put_seen(out, all);
            out << "; since the start by size, over / all:";
            for (U32 b = 0; b < BANDS; ++b)
            {
                out << " " << BAND_NAMES[b] << " " << sHeaps[heap].band_over[b].count.load(std::memory_order_relaxed) << "/"
                    << sHeaps[heap].band_all[b].count.load(std::memory_order_relaxed);
                if (b + 1 < BANDS)
                {
                    out << ",";
                }
            }
        }
    }

    void report(S64 now)
    {
        LLVKDevice& device = LLVKDevice::instance();
        VmaAllocator allocator = device.getAllocator();
        if (!allocator)
        {
            return;
        }
        const Snapshot current = Snapshot::take();
        const U64 peak = sPeakOver.exchange(0, std::memory_order_relaxed);
        const U64 usage = sUsage.load(std::memory_order_relaxed);
        const U64 budget = sBudget.load(std::memory_order_relaxed);
        std::ostringstream out;
        out << "VRAM diag (research/354, LLRHI_VRAM_DIAG): last " << llformat("%.1f", (F64)(now - sLastReport.load(std::memory_order_relaxed)) / 1e9)
            << " s, " << (current.frames - sPrevious.frames) << " frames";
        if (sKnown.load(std::memory_order_relaxed))
        {
            out << ", over the driver's budget in " << (current.frames_over - sPrevious.frames_over) << " of them (at most " << (peak / MB)
                << " MB over); VRAM " << (usage / MB) << " of " << (budget / MB) << " MB as LLVKDevice::vramBudget counts ("
                << (device.sharedMemory() ? "every heap, the GPU shares the PC's memory" : "device-local heaps") << ") at the last frame";
        }
        else
        {
            out << ", the driver gives no budget (VK_EXT_memory_budget): nothing counts as over";
        }
        put_heaps(out, allocator);
        put_kinds(out, current, sPrevious);
        put_memory(out, allocator, current, sPrevious);
        sPrevious = current;
        sLastReport.store(now, std::memory_order_relaxed);
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }
}

void LLVKVramDiag::noteMade(const char* what, U64 bytes)
{
    KindTally& kind = sKinds[classify(what, bytes)];
    kind.all.add(bytes);
    if (sOver.load(std::memory_order_relaxed))
    {
        kind.over.add(bytes);
        kind.last_usage.store(sUsage.load(std::memory_order_relaxed), std::memory_order_relaxed);
        kind.last_budget.store(sBudget.load(std::memory_order_relaxed), std::memory_order_relaxed);
        kind.last_frame.store(sFrames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

void LLVKVramDiag::noteMemory(VmaAllocator allocator, U32 memory_type, U64 bytes)
{
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(allocator, &properties);     // a pointer to what VMA read at its start
    if (!properties || memory_type >= properties->memoryTypeCount)
    {
        return;
    }
    const U32 heap = properties->memoryTypes[memory_type].heapIndex;
    if (heap >= HEAPS)
    {
        return;
    }
    HeapTally& tally = sHeaps[heap];
    const U32 band = band_of(bytes);
    tally.all.add(bytes);
    tally.band_all[band].add(bytes);
    if (sOver.load(std::memory_order_relaxed))
    {
        tally.over.add(bytes);
        tally.band_over[band].add(bytes);
    }
}

void LLVKVramDiag::tick()
{
    U64 images = 0, usage = 0, budget = 0;
    const bool known = LLVKDevice::instance().vramBudget(images, usage, budget);
    const bool over = known && usage > budget;
    sKnown.store(known, std::memory_order_relaxed);
    sUsage.store(usage, std::memory_order_relaxed);
    sBudget.store(budget, std::memory_order_relaxed);
    sOver.store(over, std::memory_order_relaxed);
    sFrames.fetch_add(1, std::memory_order_relaxed);
    if (over)
    {
        sFramesOver.fetch_add(1, std::memory_order_relaxed);
        U64 peak = sPeakOver.load(std::memory_order_relaxed);
        while (usage - budget > peak && !sPeakOver.compare_exchange_weak(peak, usage - budget, std::memory_order_relaxed))
        {
        }
    }
    const S64 now = now_ns();
    const S64 next = sNextReport.load(std::memory_order_relaxed);
    if (!next)
    {
        sNextReport.store(now + REPORT_NS, std::memory_order_relaxed);     // the first message: 10 s after the first frame
        sLastReport.store(now, std::memory_order_relaxed);
        return;
    }
    if (now < next)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(sReportLock, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return;
    }
    sNextReport.store(now + REPORT_NS, std::memory_order_relaxed);
    report(now);
}
