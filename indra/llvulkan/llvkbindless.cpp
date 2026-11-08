/**
 * @file llvkbindless.cpp
 * @brief One descriptor array holding every sampled 2D texture, indexed by slot.
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

#include "llvkbindless.h"

#include "llrhi.h"           // LLRHI::sPeriodicLogs
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <vector>

LLVKBindless::UsedMarks LLVKBindless::sUsedMarks;

namespace
{
    // Slots in the table (table_size): 16384 unless LLRHI_BINDLESS_SLOTS asks for
    // another number, at most what the device allows and 65536 (tableLimit).
    // 16384 filled up at a heavy place once textures loaded quickly: a full
    // table now takes back slots.
    using LLVKBindless::MIN_TABLE_SIZE;
    U32 TABLE_SIZE = MIN_TABLE_SIZE;
    // A full table takes back slots no draw sampled for this many frames.
    constexpr U32 RECLAIM_AFTER_FRAMES = 3;
    // How often the table's counters go to the log, as a VKPerf line.
    constexpr F64 REPORT_SECONDS = 5.0;
    // Without LLRHI::sPeriodicLogs: this often, and only when the table ran full (report_counters).
    constexpr F64 QUIET_REPORT_SECONDS = 60.0;

    // What the table went through since the last report (report_counters).
    struct Counters
    {
        U32 frames = 0;
        U32 full = 0;           // new textures that found no free slot
        U32 scans = 0;          // reclaim scans
        U32 empty_scans = 0;    // scans that took back nothing
        U32 skipped = 0;        // scans skipped: none could take anything back yet
        U32 taken_back = 0;     // slots the scans took back
        U32 white = 0;          // new textures handed the white slot for want of one
        U32 peak_in_use = 0;    // most slots held by textures at once
    };

    struct Entry
    {
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        // texture-leak (2026-10-27): the view's serial (Texture::view_serial,
        // never the same twice). A destroyed view's handle value can come back
        // for a new view; compared by handle alone, a set still holding the
        // old (destroyed) one looked up to date and was not written (ABA).
        U64 serial = 0;
        bool same(const Entry& other) const
        {
            return view == other.view && sampler == other.sampler && serial == other.serial;
        }
    };

    struct Table
    {
        bool tried = false;
        bool ready = false;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[2] = {};
        std::unordered_map<unsigned int, U32> slots;   // GL name -> slot
        std::vector<U32> free;
        // Slots given back, reusable once the frames that may still read them are done.
        std::vector<std::pair<U32, U32>> quarantine;    // slot, tracker frame it was released in
        U32 next = LLVKBindless::WHITE_SLOT + 1;
        std::vector<Entry> wanted;
        std::vector<Entry> written[2];
        std::vector<U32> used;                          // slots whose set entry may be stale
        std::vector<U32> used_frame;                    // tracker frame each slot was last sampled in
        std::vector<unsigned int> owner;                // GL texture name holding each slot, 0: none
        bool warned_full = false;
        bool logged_reclaim = false;
        // A scan that took back nothing knows the earliest frame a slot can
        // come free: until then a full table hands out white without scanning
        // again (LLRHI_BINDLESS_RECLAIM_BACKOFF=0: scan every time, as before).
        bool reclaim_backoff = true;
        U32 reclaim_failed_frame = 0;
        U32 reclaim_wait = 0;                           // frames after reclaim_failed_frame; 0: none
        Counters counters;
        std::chrono::steady_clock::time_point report_start;
        // The matrix table of each frame slot (host memory, written as draws are recorded).
        VkBuffer matrix_buffers[2] = {};
        VmaAllocation matrix_memory[2] = {};
        LLVKBindless::DrawMatrices* matrix_rows[2] = {};
        // Rows handed out this frame, in blocks (take_matrix_block): tracker
        // frame << 32 | rows. Atomic so that any thread may take rows (research/66).
        std::atomic<U64> matrix_use{ (U64)U32_MAX << 32 };
        std::atomic<bool> warned_matrices{ false };
        // The palette table of each frame slot (vec4s, written as draws are recorded).
        VkBuffer palette_buffers[2] = {};
        VmaAllocation palette_memory[2] = {};
        F32* palette_rows[2] = {};
        U32 palette_size[2] = {};                       // vec4s
        U32 palette_wanted = LLVKBindless::PALETTE_INITIAL_VEC4S;  // what the slots grow to
        // Any thread may take rows (research/66): each per-frame count is one
        // atomic word, tracker frame << 32 | count, started over by whichever
        // call first sees a new frame.
        std::atomic<U64> palette_use{ (U64)U32_MAX << 32 };     // vec4s taken
        std::atomic<U64> palette_need{ (U64)U32_MAX << 32 };    // used + what did not fit (saturating)
        std::atomic<U32> palette_last_demand{ 0 };              // the need of the frame before
        std::atomic<U32> palette_peak{ 0 };                     // paletteStats
        std::atomic<U32> palette_full_frame{ U32_MAX };         // the frame that last ran out of room
        // After a slot's table could not be grown (memory full), growing waits
        // PALETTE_RETRY_SECONDS instead of trying (and warning) every frame;
        // meanwhile draws past the table are skipped as when it is full.
        // (fix-round95 A)
        std::atomic<bool> palette_grow_waiting{ false };
        std::chrono::steady_clock::time_point palette_retry_at;
        std::atomic<U32> palette_full_warned{ 0 };              // PaletteFullState last warned
        // research/199 (stage 4b): the resident region at the head of every
        // slot's table (avatar palette rows that keep their number). The
        // rolling rows of a frame start after it. Changed only at a frame's
        // head (beginFrame), before the frame takes any rolling row.
        std::atomic<U32> palette_reserved{ 0 };                 // vec4s
        std::atomic<U32> palette_reserve_wanted{ 0 };           // from the next frame head on
        U32 moves = 0;                                  // LLVKBindless::moves()
        // research/180 stage 1: llvkgl's slot publication (setSlotLeft,
        // setPendingTracking, writePending).
        void (*slot_left)(unsigned int) = nullptr;
        bool pending_on = false;
        std::vector<U32> pending[2];                    // slots set [i] may not hold as wanted
        std::vector<U8> pending_in[2];                  // queued in pending[i]
    };
    Table sTable;

    // research/180: `texture` left its slot, or the slot's set entries were
    // forgotten: a publication of it no longer holds.
    inline void left_slot(const Table& t, unsigned int texture)
    {
        if (t.slot_left && texture)
        {
            t.slot_left(texture);
        }
    }
    // The sets, written entries, matrix and palette tables are [2], one per
    // frame in flight, picked with getSlot() & 1; the pool holds two sets;
    // released slots wait two frames (the quarantine in acquire).
    static_assert(LLVKMirror::FRAMES_IN_FLIGHT == 2, "LLVKBindless keeps two of each, picked with getSlot() & 1");

    // (Re)makes a slot's palette buffer with room for `vec4s` and points the
    // slot's set at it. Only while nothing pending or being recorded binds
    // the set (creation, LLVKBindless::beginFrame): binding 2 is not
    // update-after-bind. Keeps the old buffer when the new one cannot be made
    // (warning when `warn`).
    bool make_palette_buffer(Table& t, U32 slot, U32 vec4s, bool warn = true)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = (VkDeviceSize)vec4s * 16;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // research/198 5.5: the scene's verify copies its rows
        VmaAllocationCreateInfo host = {};
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped = {};
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = VK_NULL_HANDLE;
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        if (vmaCreateBuffer(allocator, &info, &host, &buffer, &memory, &mapped) != VK_SUCCESS || !mapped.pMappedData)
        {
            if (buffer)
            {
                vmaDestroyBuffer(allocator, buffer, memory);
            }
            if (warn)
            {
                LL_WARNS("VKBindless") << "palette table of " << vec4s << " vec4s unavailable" << LL_ENDL;
            }
            return false;
        }
        VkDescriptorBufferInfo buffer_info = { buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = t.sets[slot];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), 1, &write, 0, nullptr);
        if (t.palette_buffers[slot])
        {
            vmaDestroyBuffer(allocator, t.palette_buffers[slot], t.palette_memory[slot]);
        }
        t.palette_buffers[slot] = buffer;
        t.palette_memory[slot] = memory;
        t.palette_rows[slot] = (F32*)mapped.pMappedData;
        t.palette_size[slot] = vec4s;
        return true;
    }

    U32 table_size()
    {
        VkPhysicalDeviceDescriptorIndexingProperties indexing = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &indexing;
        vkGetPhysicalDeviceProperties2(LLVKDevice::instance().getPhysicalDevice(), &props);
        // Not below MIN_TABLE_SIZE: a GPU with lower limits is passed over
        // (LLVKDevice's required features, fix-round6 2).
        const U32 size = LLVKBindless::tableLimit(indexing);
        LL_INFOS("VKBindless") << "update-after-bind limits: sampled images " << indexing.maxDescriptorSetUpdateAfterBindSampledImages
                               << " (stage " << indexing.maxPerStageDescriptorUpdateAfterBindSampledImages << "), samplers "
                               << indexing.maxDescriptorSetUpdateAfterBindSamplers << " (stage "
                               << indexing.maxPerStageDescriptorUpdateAfterBindSamplers << "), resources per stage "
                               << indexing.maxPerStageUpdateAfterBindResources << LL_ENDL;
        // 16384 by default: with 65536 slots (well inside the reported limits) the
        // RTX 2080 lost the device at a heavy place within a minute, reading driver
        // memory outside anything allocated (research/37). A full table takes back
        // slots unused for a few frames instead. LLRHI_BINDLESS_SLOTS overrides it.
        U32 wanted = MIN_TABLE_SIZE;
        U64 slots = 0;
        if (LLRHI::envNumber("LLRHI_BINDLESS_SLOTS", 0, ~(U64)0, slots))
        {
            wanted = (U32)llclamp(slots, (U64)1024, (U64)U32_MAX);
        }
        return llmin(wanted, size);
    }

    // Slots no draw sampled for RECLAIM_AFTER_FRAMES frames go back to the free
    // list; their textures get a slot again when drawn (LLVKBindless::use finds
    // the owner changed). A scan that finds none notes how many frames pass
    // before the least recently sampled slot qualifies (scan_may_find).
    void reclaim(Table& t, U32 frame)
    {
        U32 reclaimed = 0;
        U32 oldest_age = 0;     // frames since the least recently sampled slot kept
        for (U32 slot = LLVKBindless::WHITE_SLOT + 1; slot < t.next; ++slot)
        {
            const unsigned int owner = t.owner[slot];
            if (!owner)
            {
                continue;
            }
            if (t.used_frame[slot] != U32_MAX)
            {
                const U32 age = frame - t.used_frame[slot];
                if (age < RECLAIM_AFTER_FRAMES)
                {
                    oldest_age = llmax(oldest_age, age);
                    continue;
                }
            }
            left_slot(t, owner);
            t.slots.erase(owner);
            t.owner[slot] = 0;
            t.wanted[slot] = Entry();
            t.written[0][slot] = Entry();
            t.written[1][slot] = Entry();
            t.free.push_back(slot);
            ++reclaimed;
        }
        ++t.counters.scans;
        t.counters.taken_back += reclaimed;
        if (reclaimed)
        {
            t.reclaim_wait = 0;
        }
        else
        {
            ++t.counters.empty_scans;
            t.reclaim_failed_frame = frame;
            t.reclaim_wait = RECLAIM_AFTER_FRAMES - oldest_age;    // 1 .. RECLAIM_AFTER_FRAMES
        }
        // Textures left their slots. Without the backoff every scan counted as
        // a move, as before it (the comparison switch keeps that cost too).
        if (reclaimed || !t.reclaim_backoff)
        {
            ++t.moves;
        }
        if (!t.logged_reclaim)
        {
            t.logged_reclaim = true;
            LL_INFOS("VKBindless") << "texture table full (" << TABLE_SIZE << "): took back " << reclaimed << " slots unused for "
                                   << RECLAIM_AFTER_FRAMES << " frames; later scans are counted in the VKPerf bindless line" << LL_ENDL;
        }
    }

    // False while a scan cannot take anything back: since the last empty scan
    // no frame has passed that lets a slot qualify. Slots only qualify as
    // frames pass (sampling a slot makes it younger, never older), and slots
    // of deleted textures come back through the free list without a scan.
    bool scan_may_find(const Table& t, U32 frame)
    {
        return !t.reclaim_backoff || frame - t.reclaim_failed_frame >= t.reclaim_wait;
    }

    // A slot for a new texture: a free one, one never used, or one taken back
    // from textures not drawn lately. WHITE_SLOT: none.
    U32 take_slot(Table& t, U32 frame)
    {
        if (t.free.empty())
        {
            if (t.next < TABLE_SIZE)
            {
                return t.next++;
            }
            ++t.counters.full;
            if (scan_may_find(t, frame))
            {
                reclaim(t, frame);
            }
            else
            {
                ++t.counters.skipped;
            }
            if (t.free.empty())
            {
                return LLVKBindless::WHITE_SLOT;
            }
        }
        const U32 slot = t.free.back();
        t.free.pop_back();
        // The scan's wait stands: every caller follows acquire with use, which
        // marks the slot sampled this frame (over its last texture's frame), so
        // it qualifies no sooner than the slots the last empty scan saw.
        return slot;
    }

    // Once every REPORT_SECONDS, from beginFrame: one VKPerf line instead of
    // a log line per table-full request (research/93).
    void report_counters(Table& t)
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();
        ++t.counters.frames;
        if (t.report_start == Clock::time_point())
        {
            t.report_start = now;
            return;
        }
        // fix-logcache: without LLRHI::sPeriodicLogs (a plain start) a line
        // once a minute, and only when the table ran full in it.
        const bool log = LLRHI::sPeriodicLogs;
        if (std::chrono::duration<F64>(now - t.report_start).count() < (log ? REPORT_SECONDS : QUIET_REPORT_SECONDS))
        {
            return;
        }
        const Counters& c = t.counters;
        if (log || c.full || c.white)
        {
            LL_INFOS("VKPerf") << "bindless table: " << c.frames << " frames; full " << c.full << " times (scans " << c.scans
                               << ", took back nothing " << c.empty_scans << ", skipped by backoff " << c.skipped << "), slots taken back "
                               << c.taken_back << ", white handed out " << c.white << "; slots in use peak " << c.peak_in_use << " of "
                               << TABLE_SIZE << ", waiting " << t.quarantine.size() << ", backoff "
                               << (t.reclaim_backoff ? "on" : "off") << LL_ENDL;
        }
        t.counters = Counters();
        t.counters.peak_in_use = (U32)t.slots.size();
        t.report_start = now;
    }

    bool create(Table& t)
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        TABLE_SIZE = table_size();
        U64 backoff = 1;
        LLRHI::envNumber("LLRHI_BINDLESS_RECLAIM_BACKOFF", 0, ~(U64)0, backoff);
        t.reclaim_backoff = backoff != 0;
        LL_INFOS("VKBindless") << "texture table: " << TABLE_SIZE << " slots, reclaim backoff " << (t.reclaim_backoff ? "on" : "off")
                               << LL_ENDL;
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = TABLE_SIZE;
        bindings[0].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        // The draw matrices: written once, when the set is made.
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        // The skinning palettes: the same.
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        VkDescriptorBindingFlags flags[3] = { VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0 };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        flags_info.bindingCount = 3;
        flags_info.pBindingFlags = flags;
        VkDescriptorSetLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.pNext = &flags_info;
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.bindingCount = 3;
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &t.layout) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TABLE_SIZE * 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 } };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &pool_info, nullptr, &t.pool) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorSetLayout layouts[2] = { t.layout, t.layout };
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = t.pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device, &alloc, t.sets) != VK_SUCCESS)
        {
            return false;
        }
        for (U32 i = 0; i < 2; ++i)
        {
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = (VkDeviceSize)LLVKBindless::MATRIX_ROWS * sizeof(LLVKBindless::DrawMatrices);
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo host = {};
            host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped = {};
            if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &host, &t.matrix_buffers[i], &t.matrix_memory[i], &mapped) != VK_SUCCESS ||
                !mapped.pMappedData)
            {
                LL_WARNS("VKBindless") << "draw matrix table unavailable" << LL_ENDL;
                return false;
            }
            t.matrix_rows[i] = (LLVKBindless::DrawMatrices*)mapped.pMappedData;
            VkDescriptorBufferInfo buffer_info = { t.matrix_buffers[i], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = t.sets[i];
            write.dstBinding = 1;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer_info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            if (!make_palette_buffer(t, i, LLVKBindless::PALETTE_INITIAL_VEC4S))
            {
                return false;
            }
        }
        t.wanted.assign(TABLE_SIZE, Entry());
        t.used_frame.assign(TABLE_SIZE, U32_MAX);
        t.owner.assign(TABLE_SIZE, 0);
        t.written[0].assign(TABLE_SIZE, Entry());
        t.written[1].assign(TABLE_SIZE, Entry());
        t.pending_in[0].assign(TABLE_SIZE, 0);
        t.pending_in[1].assign(TABLE_SIZE, 0);
        LLVKBindless::sUsedMarks.frames = t.used_frame.data();     // never resized after this
        return true;
    }
}

bool LLVKBindless::init()
{
    Table& t = sTable;
    if (!t.tried)
    {
        t.tried = true;
        t.ready = create(t);
        if (!t.ready)
        {
            LL_WARNS("VKBindless") << "bindless texture table unavailable" << LL_ENDL;
        }
    }
    return t.ready;
}

VkDescriptorSetLayout LLVKBindless::layout()
{
    return sTable.layout;
}

VkDescriptorSet LLVKBindless::set()
{
    return sTable.sets[LLVKMirror::instance().getSlot() & 1];
}

namespace
{
    // A thread's block of the frame's matrix rows: the next one to hand out,
    // the end of the block, and the tracker frame they belong to. Blocks are
    // taken one after another from the table's count, so a frame whose rows
    // are all taken by one thread numbers them 0, 1, 2, ... exactly as when
    // the count was a plain integer; with more threads, each fills its own
    // blocks (a thread's last block of a frame may be left part unused).
    struct MatrixCursor
    {
        U32 frame = U32_MAX;
        U32 next = 0;
        U32 end = 0;
    };
    thread_local MatrixCursor tMatrixCursor;
    constexpr U32 MATRIX_BLOCK = 64;

    void take_matrix_block(Table& t, MatrixCursor& c, U32 frame)
    {
        U64 use = t.matrix_use.load(std::memory_order_relaxed);
        while (true)
        {
            const U32 used = (U32)(use >> 32) == frame ? (U32)use : 0;
            if (used >= LLVKBindless::MATRIX_ROWS)
            {
                c = { frame, LLVKBindless::MATRIX_ROWS, LLVKBindless::MATRIX_ROWS };     // full
                return;
            }
            const U32 take = llmin(MATRIX_BLOCK, LLVKBindless::MATRIX_ROWS - used);
            if (t.matrix_use.compare_exchange_weak(use, ((U64)frame << 32) | (used + take), std::memory_order_relaxed))
            {
                c = { frame, used, used + take };
                return;
            }
        }
    }
}

LLVKBindless::DrawMatrices* LLVKBindless::matrixRow(U32& index)
{
    Table& t = sTable;
    const U32 frame = LLVKTracker::frame();
    MatrixCursor& c = tMatrixCursor;
    if (c.frame != frame || c.next == c.end)
    {
        take_matrix_block(t, c, frame);
    }
    if (c.next < c.end)
    {
        index = c.next++;
    }
    else
    {
        if (!t.warned_matrices.exchange(true, std::memory_order_relaxed))
        {
            LL_WARNS("VKBindless") << "draw matrix table full (" << MATRIX_ROWS << " rows a frame); later draws share its last row" << LL_ENDL;
        }
        index = MATRIX_ROWS - 1;
    }
    return &t.matrix_rows[LLVKMirror::instance().getSlot() & 1][index];
}

LLVKBindless::DrawMatrices* LLVKBindless::matrixRows(U32 count, U32& first)
{
    Table& t = sTable;
    const U32 frame = LLVKTracker::frame();
    MatrixCursor& c = tMatrixCursor;
    if (c.frame != frame)
    {
        c = { frame, 0, 0 };    // nothing of this frame's yet
    }
    if (count <= c.end - c.next && (U64)c.next + count > MATRIX_ROWS - 1)
    {
        // In this thread's block but reaching the last row: used up, as by matrixRow calls.
        c.next = c.end;
        first = 0;
        return nullptr;
    }
    if (count > c.end - c.next)
    {
        // Rows past this thread's block: right after it while nobody took rows
        // since (as count matrixRow calls would get them), else a run of their own.
        U64 use = t.matrix_use.load(std::memory_order_relaxed);
        while (true)
        {
            const U32 used = (U32)(use >> 32) == frame ? (U32)use : 0;
            const U32 start = used == c.end ? c.next : used;
            if ((U64)start + count > MATRIX_ROWS - 1)
            {
                // No room before the last row (which full tables share): the
                // rest of the table is used up, as by that many matrixRow calls.
                if (!t.matrix_use.compare_exchange_weak(use, ((U64)frame << 32) | MATRIX_ROWS, std::memory_order_relaxed))
                {
                    continue;
                }
                c = { frame, MATRIX_ROWS, MATRIX_ROWS };
                first = 0;
                return nullptr;
            }
            const U32 end = llmin(llmax(start + count, used + MATRIX_BLOCK), MATRIX_ROWS);
            if (t.matrix_use.compare_exchange_weak(use, ((U64)frame << 32) | end, std::memory_order_relaxed))
            {
                c = { frame, start, end };
                break;
            }
        }
    }
    first = c.next;
    c.next += count;
    return &t.matrix_rows[LLVKMirror::instance().getSlot() & 1][first];
}

namespace
{
    constexpr std::chrono::seconds PALETTE_RETRY_SECONDS{ 5 };

    // Why the palette table ran out of room, as last warned (palette_full_warned):
    // the warning comes once per state, not every frame. (fix-round95 A)
    enum PaletteFullState : U32
    {
        PALETTE_FULL_NONE = 0,
        PALETTE_FULL_GROWING,       // the next frames grow the table
        PALETTE_FULL_WAITING,       // growing failed (memory full): waits, then tries again
        PALETTE_FULL_AT_LIMIT,      // at palette_limit()
    };

    // The most a slot's palette table grows to: also what one descriptor may cover.
    U32 palette_limit()
    {
        const U64 range = LLVKDevice::instance().getLimits().maxStorageBufferRange / 16;
        return (U32)llmin((U64)LLVKBindless::PALETTE_MAX_VEC4S, range);
    }

    // Adds vec4s to the tracker frame's demand (0: only starts the frame's
    // count). The call that starts a new frame's count keeps the old count as
    // palette_last_demand. A frame's count starts at the resident region
    // (research/199), so the tables grow to hold it and the rolling rows.
    void add_palette_need(Table& t, U32 frame, U32 vec4s)
    {
        U64 need = t.palette_need.load(std::memory_order_relaxed);
        while (true)
        {
            const bool same = (U32)(need >> 32) == frame;
            const U32 before = same ? (U32)need : t.palette_reserved.load(std::memory_order_relaxed);
            const U32 after = (U32)llmin((U64)before + vec4s, (U64)U32_MAX);
            if (same && after == before)
            {
                return;     // nothing to add (or saturated)
            }
            if (t.palette_need.compare_exchange_weak(need, ((U64)frame << 32) | after, std::memory_order_relaxed))
            {
                if (!same)
                {
                    t.palette_last_demand.store((U32)need, std::memory_order_relaxed);
                }
                U32 peak = t.palette_peak.load(std::memory_order_relaxed);
                while (after > peak && !t.palette_peak.compare_exchange_weak(peak, after, std::memory_order_relaxed))
                {
                }
                return;
            }
        }
    }
}

F32* LLVKBindless::paletteRows(U32 vec4s, U32& base)
{
    Table& t = sTable;
    base = 0;
    if (!t.ready || !vec4s || vec4s > PALETTE_INITIAL_VEC4S)
    {
        return nullptr;
    }
    const U32 frame = LLVKTracker::frame();
    const U32 slot = LLVKMirror::instance().getSlot() & 1;
    add_palette_need(t, frame, vec4s);
    // The same test and step as with a plain count, made atomic: a request
    // that does not fit leaves the count as it was. A frame's rows start after
    // the resident region (research/199; 0 unless avatar palette rows are on).
    U64 use = t.palette_use.load(std::memory_order_relaxed);
    while (true)
    {
        const U32 used = (U32)(use >> 32) == frame ? (U32)use : t.palette_reserved.load(std::memory_order_relaxed);
        if (used > t.palette_size[slot] || vec4s > t.palette_size[slot] - used)
        {
            break;
        }
        if (t.palette_use.compare_exchange_weak(use, ((U64)frame << 32) | (used + vec4s), std::memory_order_relaxed))
        {
            base = used;
            return t.palette_rows[slot] + (size_t)base * 4;
        }
    }
    // Full: the caller skips the draw; the table grows for the next frames.
    if (t.palette_full_frame.exchange(frame, std::memory_order_relaxed) != frame)
    {
        // Once per state (fix-round95 A): a table that cannot grow ran out of
        // room every frame and warned every frame.
        const U32 state = t.palette_grow_waiting.load(std::memory_order_relaxed) ? PALETTE_FULL_WAITING
                        : t.palette_wanted < palette_limit()                       ? PALETTE_FULL_GROWING
                                                                                    : PALETTE_FULL_AT_LIMIT;
        if (t.palette_full_warned.exchange(state, std::memory_order_relaxed) != state)
        {
            if (state == PALETTE_FULL_GROWING)
            {
                LL_WARNS("VKBindless") << "palette table full (" << t.palette_size[slot]
                                       << " vec4s): skipping this frame's later skinned draws, growing the table" << LL_ENDL;
            }
            else if (state == PALETTE_FULL_WAITING)
            {
                LL_WARNS("VKBindless") << "palette table full (" << t.palette_size[slot]
                                       << " vec4s) and could not grow: skinned draws past it are skipped until it can" << LL_ENDL;
            }
            else
            {
                LL_WARNS_ONCE("VKBindless") << "palette table full at its limit (" << t.palette_size[slot]
                                            << " vec4s): skinned draws past it are skipped" << LL_ENDL;
            }
        }
    }
    return nullptr;
}

VkBuffer LLVKBindless::paletteBuffer()
{
    Table& t = sTable;
    return t.ready ? t.palette_buffers[LLVKMirror::instance().getSlot() & 1] : VK_NULL_HANDLE;
}

void LLVKBindless::setPaletteReserve(U32 vec4s)
{
    Table& t = sTable;
    U32 wanted = t.palette_reserve_wanted.load(std::memory_order_relaxed);
    while (vec4s > wanted && !t.palette_reserve_wanted.compare_exchange_weak(wanted, vec4s, std::memory_order_relaxed))
    {
    }
}

U32 LLVKBindless::paletteReserved()
{
    return sTable.palette_reserved.load(std::memory_order_relaxed);
}

U32 LLVKBindless::paletteTableSlot()
{
    return LLVKMirror::instance().getSlot() & 1;
}

F32* LLVKBindless::paletteReservedRows(U32 table_slot, U32 base, U32 vec4s)
{
    Table& t = sTable;
    table_slot &= 1;
    const U64 end = (U64)base + vec4s;
    if (!t.ready || !t.palette_rows[table_slot] || end > t.palette_reserved.load(std::memory_order_relaxed) ||
        end > t.palette_size[table_slot])
    {
        return nullptr;
    }
    return t.palette_rows[table_slot] + (size_t)base * 4;
}

void LLVKBindless::beginFrame()
{
    Table& t = sTable;
    if (!t.ready)
    {
        return;
    }
    report_counters(t);
    // research/199: the resident region grows here only, before this frame
    // takes any rolling row (they start after it). The tables are made to hold
    // the larger region below, with the frame before's demand. Security audit
    // B low 11: the region takes only room both slots' tables have already
    // (they never shrink), so no resident row, nor a scene record naming one,
    // is ever past the table the GPU reads, also when growing a table fails;
    // until then the tables grow first, and the rows not given meanwhile are
    // asked for again (the avatars' palettes go the rolling way).
    const U32 reserved_before = t.palette_reserved.load(std::memory_order_relaxed);
    const U32 reserve_wanted = t.palette_reserve_wanted.load(std::memory_order_relaxed);
    U32 reserve_target = reserved_before;   // the region the tables are to hold
    if (reserve_wanted > reserved_before && reserve_wanted <= palette_limit() / 2 &&
        !t.palette_grow_waiting.load(std::memory_order_relaxed))    // not while the tables cannot grow
    {
        reserve_target = reserve_wanted;
        if (reserve_wanted <= llmin(t.palette_size[0], t.palette_size[1]))
        {
            t.palette_reserved.store(reserve_wanted, std::memory_order_relaxed);
        }
    }
    const U32 reserve_growth = reserve_target - reserved_before;
    add_palette_need(t, LLVKTracker::frame(), 0);    // the previous frame's demand is now palette_last_demand
    if (t.palette_grow_waiting.load(std::memory_order_relaxed))
    {
        // Growing failed lately (memory full): the table stays as it is, and
        // what does not fit is skipped, until the wait is over. (fix-round95 A)
        if (std::chrono::steady_clock::now() < t.palette_retry_at)
        {
            return;
        }
        t.palette_grow_waiting.store(false, std::memory_order_relaxed);
    }
    const U32 last_demand = (U32)llmin((U64)t.palette_last_demand.load(std::memory_order_relaxed) + reserve_growth, (U64)U32_MAX);
    if (last_demand > llmin(t.palette_size[0], t.palette_size[1]))
    {
        // Room for the busiest frame and a quarter more, in powers of two.
        const U64 need = (U64)last_demand + last_demand / 4;
        const U32 limit = palette_limit();
        U32 wanted = t.palette_wanted;
        while (wanted < need && wanted < limit)
        {
            wanted *= 2;
        }
        t.palette_wanted = llmax(llmin(wanted, limit), t.palette_wanted);
    }
    const U32 slot = LLVKMirror::instance().getSlot() & 1;
    if (t.palette_size[slot] < t.palette_wanted)
    {
        // This slot's last frame is done (LLVKMirror::beginFrame waited for
        // it) and this frame has bound nothing yet: the set can be rewritten.
        // Warns on the first failure only, not on each later try (fix-round95 A).
        const bool first_failure = t.palette_full_warned.load(std::memory_order_relaxed) != PALETTE_FULL_WAITING;
        if (make_palette_buffer(t, slot, t.palette_wanted, first_failure))
        {
            LL_INFOS("VKBindless") << "palette table of frame slot " << slot << ": " << t.palette_wanted << " vec4s" << LL_ENDL;
            t.palette_full_warned.store(PALETTE_FULL_NONE, std::memory_order_relaxed);
        }
        else
        {
            // Stay as is, and try again only after PALETTE_RETRY_SECONDS:
            // the next frame's demand would ask for the same size again.
            t.palette_wanted = t.palette_size[slot];
            t.palette_retry_at = std::chrono::steady_clock::now() + PALETTE_RETRY_SECONDS;
            t.palette_grow_waiting.store(true, std::memory_order_relaxed);
        }
    }
}

void LLVKBindless::paletteStats(U32& peak, U32& capacity)
{
    Table& t = sTable;
    peak = t.palette_peak.exchange(0, std::memory_order_relaxed);
    capacity = t.palette_size[LLVKMirror::instance().getSlot() & 1];
}

VkPushConstantRange LLVKBindless::pushConstantRange()
{
    return { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PUSH_CONSTANT_BYTES };
}

U32 LLVKBindless::acquire(unsigned int texture)
{
    Table& t = sTable;
    if (!texture)
    {
        return WHITE_SLOT;
    }
    auto it = t.slots.find(texture);
    if (it != t.slots.end())
    {
        return it->second;
    }
    // Released slots come back two frames later: draws recorded earlier in the
    // frame they were released in may still read them.
    const U32 frame = LLVKTracker::frame();
    for (size_t i = 0; i < t.quarantine.size(); )
    {
        if (frame - t.quarantine[i].second >= 2)
        {
            t.free.push_back(t.quarantine[i].first);
            t.quarantine[i] = t.quarantine.back();
            t.quarantine.pop_back();
        }
        else
        {
            ++i;
        }
    }
    const U32 slot = take_slot(t, frame);
    if (slot == WHITE_SLOT)
    {
        if (!t.warned_full)
        {
            LL_WARNS("VKBindless") << "texture table full (" << TABLE_SIZE << "); further textures sample white" << LL_ENDL;
            t.warned_full = true;
        }
        ++t.counters.white;
        return WHITE_SLOT;
    }
    t.slots.emplace(texture, slot);
    t.owner[slot] = texture;
    t.counters.peak_in_use = llmax(t.counters.peak_in_use, (U32)t.slots.size());
    return slot;
}

U32 LLVKBindless::use(U32 slot, unsigned int texture, VkImageView view, VkSampler sampler, U64 serial)
{
    Table& t = sTable;
    const U32 frame = LLVKTracker::frame();
    const U32 set = LLVKMirror::instance().getSlot() & 1;
    const Entry wanted_now{ view, sampler, serial };
    if (slot != WHITE_SLOT && t.owner[slot] != texture)
    {
        // The caller's slot moved (another path moved this texture): look it up.
        ++t.moves;
        slot = acquire(texture);
        if (slot == WHITE_SLOT)
        {
            return WHITE_SLOT;
        }
    }
    if (t.used_frame[slot] == frame)
    {
        const Entry& current = t.wanted[slot];
        if (!current.same(wanted_now))
        {
            // Earlier draws of this frame read the slot's entry: leave it to them.
            ++t.moves;
            left_slot(t, texture);
            t.quarantine.emplace_back(slot, frame);
            t.owner[slot] = 0;
            forget(slot);
            t.slots.erase(texture);
            slot = acquire(texture);
            if (slot == WHITE_SLOT)
            {
                return WHITE_SLOT;
            }
        }
    }
    t.used_frame[slot] = frame;
    t.wanted[slot] = wanted_now;
    const Entry& have = t.written[set][slot];
    if (!have.same(wanted_now))
    {
        t.used.push_back(slot);
    }
    if (t.pending_on)
    {
        // research/180: the other frame's set gets it at its next frame
        // (writePending), for draws that will take the slot without use().
        const U32 other = set ^ 1;
        const Entry& next = t.written[other][slot];
        if (!next.same(wanted_now) && !t.pending_in[other][slot])
        {
            t.pending_in[other][slot] = 1;
            t.pending[other].push_back(slot);
        }
    }
    return slot;
}

U32 LLVKBindless::slotFor(unsigned int texture)
{
    // The view changes when mip levels arrive or the image is reallocated.
    U32 slot = acquire(texture);
    LLVKTracker::Sampled sampled = LLVKTracker::sampledTexture(slot == WHITE_SLOT ? 0 : texture);
    if (slot == WHITE_SLOT)
    {
        // The stand-in never changes: write it, never move it.
        sTable.wanted[WHITE_SLOT] = Entry{ sampled.view, sampled.sampler, sampled.serial };
        sTable.used.push_back(WHITE_SLOT);
        return WHITE_SLOT;
    }
    return use(slot, texture, sampled.view, sampled.sampler, sampled.serial);
}

void LLVKBindless::flush()
{
    Table& t = sTable;
    if (t.used.empty())
    {
        return;
    }
    const U32 frame = LLVKMirror::instance().getSlot() & 1;
    static std::vector<VkDescriptorImageInfo> images;
    static std::vector<VkWriteDescriptorSet> writes;
    images.clear();
    writes.clear();
    std::vector<Entry>& written = t.written[frame];
    for (U32 slot : t.used)
    {
        const Entry& want = t.wanted[slot];
        Entry& have = written[slot];
        if (!want.view || want.same(have))
        {
            continue;
        }
        have = want;
        images.push_back({ want.sampler, want.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = t.sets[frame];
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.push_back(write);
    }
    for (size_t i = 0; i < writes.size(); ++i)
    {
        writes[i].pImageInfo = &images[i];
    }
    if (!writes.empty())
    {
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), (U32)writes.size(), writes.data(), 0, nullptr);
    }
    t.used.clear();
}

std::string LLVKBindless::describe(U32 slot)
{
    Table& t = sTable;
    if (!t.ready || slot >= TABLE_SIZE)
    {
        return "slot " + std::to_string(slot) + " (no table)";
    }
    char text[160];
    snprintf(text, sizeof(text), "slot %u want %p set0 %p set1 %p (current %u)", slot, (void*)t.wanted[slot].view,
             (void*)t.written[0][slot].view, (void*)t.written[1][slot].view, LLVKMirror::instance().getSlot() & 1);
    return text;
}

void LLVKBindless::forget(U32 slot)
{
    Table& t = sTable;
    ++t.moves;
    if (!t.ready || slot == WHITE_SLOT || slot >= TABLE_SIZE)
    {
        return;
    }
    left_slot(t, t.owner[slot]);
    t.written[0][slot] = Entry();
    t.written[1][slot] = Entry();
    // texture-leak (2026-10-27): nor is what it wanted (the view going away):
    // writePending would write it into the other set after this, and when the
    // next view got the same handle value that set looked up to date (ABA).
    // The next use() wants the new view.
    t.wanted[slot] = Entry();
}

void LLVKBindless::forgetTexture(unsigned int texture)
{
    Table& t = sTable;
    auto it = t.slots.find(texture);
    if (it != t.slots.end())
    {
        forget(it->second);
    }
    // No slot: nothing of it in the sets (it left its last one through a
    // release, reclaim or move, which cleared that slot's entries).
}

U32 LLVKBindless::moves()
{
    return sTable.moves;
}

void LLVKBindless::slotStats(U32& in_use, U32& waiting, U32& table)
{
    const Table& t = sTable;
    in_use = (U32)t.slots.size();
    waiting = (U32)t.quarantine.size();
    table = t.ready ? TABLE_SIZE : 0;
}

void LLVKBindless::release(unsigned int texture)
{
    Table& t = sTable;
    auto it = t.slots.find(texture);
    if (it != t.slots.end())
    {
        ++t.moves;
        left_slot(t, texture);
        t.quarantine.emplace_back(it->second, LLVKTracker::frame());
        t.wanted[it->second] = Entry();
        t.owner[it->second] = 0;
        forget(it->second);
        t.slots.erase(it);
    }
}

// ---- research/180 stage 1: the slot publication (bindless side) ----

void LLVKBindless::setSlotLeft(void (*left)(unsigned int texture))
{
    sTable.slot_left = left;
}

void LLVKBindless::setPendingTracking(bool on)
{
    Table& t = sTable;
    if (t.pending_on == on)
    {
        return;
    }
    t.pending_on = on;
    for (U32 set = 0; set < 2; ++set)
    {
        // Started over: what use() queues from now on (the publication starts
        // empty whenever this changes).
        for (U32 slot : t.pending[set])
        {
            t.pending_in[set][slot] = 0;
        }
        t.pending[set].clear();
    }
}

U32 LLVKBindless::writePending()
{
    Table& t = sTable;
    const U32 set = LLVKMirror::instance().getSlot() & 1;
    std::vector<U32>& pending = t.pending[set];
    if (!t.ready || pending.empty())
    {
        return 0;
    }
    // As flush(), for the slots use() queued for this frame's set while the
    // other one was recorded.
    static std::vector<VkDescriptorImageInfo> images;
    static std::vector<VkWriteDescriptorSet> writes;
    images.clear();
    writes.clear();
    std::vector<Entry>& written = t.written[set];
    for (U32 slot : pending)
    {
        t.pending_in[set][slot] = 0;
        const Entry& want = t.wanted[slot];
        Entry& have = written[slot];
        if (!want.view || want.same(have))
        {
            continue;
        }
        have = want;
        images.push_back({ want.sampler, want.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = t.sets[set];
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.push_back(write);
    }
    pending.clear();
    for (size_t i = 0; i < writes.size(); ++i)
    {
        writes[i].pImageInfo = &images[i];
    }
    if (!writes.empty())
    {
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), (U32)writes.size(), writes.data(), 0, nullptr);
    }
    return (U32)writes.size();
}

U32 LLVKBindless::checkSlot(U32 slot, unsigned int texture, VkImageView view, VkSampler sampler, U64 serial)
{
    const Table& t = sTable;
    if (!t.ready || slot == WHITE_SLOT || slot >= TABLE_SIZE)
    {
        return 0;
    }
    const Entry& want = t.wanted[slot];
    const Entry& have = t.written[LLVKMirror::instance().getSlot() & 1][slot];
    U32 ok = 0;
    ok |= t.owner[slot] == texture ? SLOT_OWNER_OK : 0;
    ok |= want.same(Entry{ view, sampler, serial }) ? SLOT_WANTED_OK : 0;
    ok |= have.same(want) && want.view ? SLOT_WRITTEN_OK : 0;
    return ok;
}
// ---- end of the slot publication (bindless side) ----
