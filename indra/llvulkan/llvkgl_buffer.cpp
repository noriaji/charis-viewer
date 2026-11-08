/**
 * @file llvkgl_buffer.cpp
 * @brief GL buffers on Vulkan: the buffer table and bindings, the Vulkan copies and their uploads, buffer writes, uniform block bindings (research/91).
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

#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // ------------------------------------------------------------------
    // Buffers: every GL buffer has a device-local Vulkan copy that persists
    // across frames. GL writes only mark a byte range dirty; the range is
    // uploaded the first time a draw needs it, through the frame's upload
    // commands (which run before the frame's draws). A buffer rewritten after
    // a draw in the same frame already used it gets a fresh Vulkan buffer, so
    // earlier draws keep the data they were issued with. A CPU copy is kept
    // for those re-creations and for uniform blocks.

    NameTable<Buffer> sBuffers;
    thread_local GLuint sArrayBuffer = 0;     // binding state is per GL context (= per thread)
    thread_local GLuint sElementBuffer = 0;
    thread_local GLuint sUniformBuffer = 0;
    thread_local UniformBinding sUniformBindings[MAX_UNIFORM_BINDINGS];        // binding point -> buffer
    thread_local Buffer* sElementResolved = nullptr;
    thread_local GLuint sElementResolvedName = 0;
    thread_local U32 sElementResolvedAt = U32_MAX;
namespace
{
    // Buffer::version values, the same way as Texture::version's (llvkgl_texture.cpp;
    // a buffer's data is written under sLock).
    std::atomic<U32> sBufferVersions{ 0 };
    U32 next_buffer_version() { return sBufferVersions.fetch_add(1, std::memory_order_relaxed) + 1; }

    constexpr U64 MB = 1024ull * 1024ull;

    // research/285: the Vulkan copies compact_buffers gave back are made again
    // at most this many bytes a frame (a quarter of the 256 MB upload ring,
    // which the frame's uniforms and other uploads share). One past the cap is
    // drawn from the next frame on. Under sLock.
    constexpr U64 REMADE_BYTES_PER_FRAME = 64 * MB;
    U32 sRemadeFrame = U32_MAX;
    U64 sRemadeBytes = 0;
    // Buffer-frames whose remake was left to a later frame (a buffer once a
    // frame), since the start. Written under sLock, read without it
    // (log_held_remakes).
    std::atomic<U64> sRemakesHeld{ 0 };

    bool remake_allowed(Buffer& buffer, U64 size)
    {
        if (sRemadeFrame != sFrame)
        {
            sRemadeFrame = sFrame;
            sRemadeBytes = 0;
        }
        if (sRemadeBytes && sRemadeBytes + size > REMADE_BYTES_PER_FRAME)
        {
            if (buffer.held_frame != sFrame)
            {
                buffer.held_frame = sFrame;     // counted once a frame, however many passes ask for it
                sRemakesHeld.fetch_add(1, std::memory_order_relaxed);
            }
            return false;       // the first of a frame always goes (one larger than the cap is not held for ever)
        }
        sRemadeBytes += size;
        return true;
    }
} // namespace

    bool held_this_frame(const Buffer* buffer)
    {
        return buffer && !buffer->gpu && buffer->held_frame == sFrame;
    }

    GLuint* binding_for(GLenum target)
    {
        switch (target)
        {
        case GL_ARRAY_BUFFER:         return &sArrayBuffer;
        case GL_ELEMENT_ARRAY_BUFFER: return &sElementBuffer;
        case GL_UNIFORM_BUFFER:       return &sUniformBuffer;
        default:                      return nullptr;
        }
    }

    GLuint bound_name(GLenum target)
    {
        GLuint* binding = binding_for(target);
        return binding ? *binding : 0;
    }

    Buffer* bound_buffer(GLenum target)
    {
        GLuint* binding = binding_for(target);
        if (!binding || !*binding)
        {
            return nullptr;
        }
        return &sBuffers.get(*binding);
    }

    void retire_gpu(Buffer& buffer)
    {
        if (buffer.gpu)
        {
            VkBuffer gpu = buffer.gpu;
            VmaAllocation memory = buffer.gpu_memory;
            LLVKMirror::instance().defer([gpu, memory]()
            {
                vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), gpu, memory);
            });
        }
        buffer.gpu = VK_NULL_HANDLE;
        buffer.gpu_memory = nullptr;
        // gpu_size (the capacity) is kept: a buffer retired because a draw of this
        // frame still reads it is re-created right away, at the same capacity.
    }

    // The draw-ready Vulkan buffer for a GL buffer, uploading what changed.
    VkBuffer gpu_buffer(Buffer& buffer)
    {
        if (LLRHIVBCensus::sOn && buffer.owner_number)  // research/293 branch 0: counting only
        {
            LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_GPU_BUFFER, buffer.owner_number);
        }
        if (buffer.data.empty())
        {
            return VK_NULL_HANDLE;
        }
        const bool dirty = buffer.dirty_begin < buffer.dirty_end;
        if (buffer.gpu && dirty && buffer.used_frame == sFrame)
        {
            retire_gpu(buffer);     // a draw this frame still needs the old contents
        }
        // Grow only. Re-creating whenever the size changed by a byte made the
        // dynamic buffers (particles, UI, animation) allocate a new oddly sized
        // VkBuffer nearly every frame: VMA's blocks fragmented and committed VRAM
        // climbed without bound (research/77). Spare capacity is never given back.
        if (!buffer.gpu || buffer.gpu_size < buffer.data.size())
        {
            if (!buffer.gpu && buffer.compacted && !remake_allowed(buffer, llmax(buffer.gpu_size, buffer.data.size())))
            {
                return VK_NULL_HANDLE;      // research/285: over this frame's cap; drawn from a later frame
            }
            size_t want = buffer.gpu_size;      // kept by retire_gpu
            if (want < buffer.data.size())
            {
                want = llmax(buffer.data.size(), want + want / 2);
                want = (want + 255) & ~(size_t)255;
            }
            retire_gpu(buffer);
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = want;
            info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            // In the GL buffer pools (research/77): ~100k small buffers, made and
            // freed all the time, no longer splinter the blocks textures live in.
            if (LLVKDevice::instance().createBuffer(LLVKDevice::POOL_GL_BUFFER, info, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, buffer.gpu,
                                                    buffer.gpu_memory) != VK_SUCCESS)
            {
                buffer.gpu = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
            buffer.gpu_serial = next_object_serial();   // research/156 A-DS
            if (sNameObjects)   // the name is built only when it is kept
            {
                name_object((U64)buffer.gpu, "GL buffer, " + std::to_string(info.size) + " bytes");
            }
            buffer.gpu_size = want;
            buffer.compacted = false;
            buffer.dirty_begin = 0;
            buffer.dirty_end = buffer.data.size();
        }
        if (buffer.dirty_begin < buffer.dirty_end)
        {
            LLVKMirror& mirror = LLVKMirror::instance();
            VkCommandBuffer cmd = mirror.uploadCommands();
            size_t begin = buffer.dirty_begin & ~(size_t)3;
            size_t end = llmin((buffer.dirty_end + 3) & ~(size_t)3, buffer.data.size());
            size_t size = end - begin;
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceSize staging_offset = 0;
            void* dst = nullptr;
            if (!cmd || !size || !mirror.allocate(size, 16, staging, staging_offset, dst))
            {
                return VK_NULL_HANDLE;
            }
            memcpy(dst, buffer.data.data() + begin, size);
            VkBufferCopy region = { staging_offset, begin, size };
            vkCmdCopyBuffer(cmd, staging, buffer.gpu, 1, &region);
            buffer.dirty_begin = SIZE_MAX;
            buffer.dirty_end = 0;
        }
        buffer.used_frame = sFrame;
        return buffer.gpu;
    }

    ResolvedBuffer sResolvedBuffers[RESOLVED_BUFFERS] = {};

    // gpu_buffer for the GL buffer `name` (null: no such buffer, or not drawable).
    VkBuffer gpu_buffer_named(GLuint name)
    {
        if (!name)
        {
            return VK_NULL_HANDLE;
        }
        ResolvedBuffer& e = sResolvedBuffers[name & (RESOLVED_BUFFERS - 1)];
        if (e.name == name && e.frame == sFrame)
        {
            if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only
            {
                const U32 owner = census_owner(name);
                if (owner)
                {
                    LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_GPU_NAMED_CACHED, owner);
                }
            }
            return e.gpu;
        }
        Buffer* b = sBuffers.find(name);
        const VkBuffer gpu = b ? gpu_buffer(*b) : VK_NULL_HANDLE;
        if (gpu)
        {
            e.name = name;
            e.frame = sFrame;
            e.gpu = gpu;
        }
        return gpu;
    }

    // ---- research/293 branch 0 (LLRHI_VB_CENSUS): the owner marks and the reads that count them. Counting only. ----
    void census_set_owner(U32 name, U32 number)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (Buffer* b = sBuffers.find(name))
        {
            b->owner_number = number;
        }
    }

    void census_clear_owner(U32 name, U32 number)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (Buffer* b = sBuffers.find(name))
        {
            if (b->owner_number == number)
            {
                b->owner_number = 0;
            }
        }
    }

    U32 census_owner(GLuint name)
    {
        const Buffer* b = name ? sBuffers.find(name) : nullptr;
        return b ? b->owner_number : 0;
    }

    void census_changed(GLuint name)
    {
        const U32 owner = census_owner(name);
        if (owner)
        {
            LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_CHANGED, owner);
        }
    }
    // ---- end of research/293 branch 0 ----

    // name: b's GL name (its gpu_buffer_named entry is forgotten).
    void buffer_data(GLuint name, Buffer* b, size_t size, const void* data)
    {
        if (b)
        {
            forget_resolved_buffer(name);
            buffer_changed(name);
            b->data.resize(size);
            if (data) memcpy(b->data.data(), data, size);
            b->markDirty(0, size);
            b->version = next_buffer_version();
            ++sVertexState;
            if (b->uniform_source) ++sUniformBlockState;
        }
    }

    void buffer_sub_data(GLuint name, Buffer* b, size_t offset, size_t size, const void* data)
    {
        if (b && data && size <= b->data.size() && offset <= b->data.size() - size)   // no wrap-around (security audit B 7)
        {
            forget_resolved_buffer(name);
            buffer_changed(name);
            memcpy(b->data.data() + offset, data, size);
            b->markDirty(offset, offset + size);
            b->version = next_buffer_version();
            ++sVertexState;
            if (b->uniform_source) ++sUniformBlockState;
        }
    }

    // Native buffer data (research/39 N4): by name, no binding or GL entry point.
    void native_buffer_data(U32 name, size_t size, const void* data)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (name)
        {
            buffer_data(name, &sBuffers.get(name), size, data);
        }
    }

    BufferWriteCounts sBufferWrites;

    void native_buffer_sub_data(U32 name, size_t offset, size_t size, const void* data)
    {
        const U64 start = sPerf ? __rdtsc() : 0;
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (name)
        {
            buffer_sub_data(name, sBuffers.find(name), offset, size, data);
        }
        // research/137 N14: counted, to set against the writes by handle.
        BufferWriteCounts& c = sBufferWrites;
        ++c.calls;
        ++c.ranges;
        c.bytes += size;
        if (start)
        {
            c.ticks += __rdtsc() - start;
        }
    }

    // research/137 N14: LLRHI::sBufferWriteRanges' work. Each range as
    // buffer_sub_data writes one (skipped past the storage, the same bytes, the
    // same dirty range); what buffer_sub_data does besides, once a call, is
    // done once for all of them: forgetting the frame's resolved entry, telling
    // the copies made from the buffer, a new version and the change counters
    // only say that the buffer changed, which once says as well as once a range.
    void buffer_write_ranges(GLuint name, Buffer& b, const U8* data, const LLRHI::BufferRange* ranges, U32 count)
    {
        bool changed = false;
        for (U32 i = 0; i < count; ++i)
        {
            const size_t offset = ranges[i].offset;
            const size_t size = ranges[i].size;
            if (size > b.data.size() || offset > b.data.size() - size)     // no wrap-around (security audit B 7)
            {
                continue;
            }
            if (!changed)
            {
                forget_resolved_buffer(name);
                buffer_changed(name);
                changed = true;
            }
            memcpy(b.data.data() + offset, data + offset, size);
            b.markDirty(offset, offset + size);
        }
        if (changed)
        {
            b.version = next_buffer_version();
            ++sVertexState;
            if (b.uniform_source) ++sUniformBlockState;
        }
    }

    thread_local VkBuffer tDirectVertexBuffer = VK_NULL_HANDLE;

    // A uniform block binding point. Binding what it already holds (the reflection
    // probe block is bound again with every shader) keeps sUniformBlockState:
    // writes to a buffer read as a uniform block move it themselves (buffer_data).
    void bind_uniform_buffer(GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
    {
        if (index >= MAX_UNIFORM_BINDINGS)
        {
            ++sUniformBlockState;
            return;
        }
        UniformBinding& binding = sUniformBindings[index];
        if (binding.buffer == buffer && binding.offset == offset && binding.size == size)
        {
            return;
        }
        binding = { buffer, offset, size };
        ++sUniformBlockState;
    }

    // glBindBufferBase as tracked (under sLock); also LLRHI::sBindBufferBase.
    void bind_buffer_base(GLenum target, GLuint index, GLuint buffer)
    {
        if (target == GL_UNIFORM_BUFFER)
        {
            sUniformBuffer = buffer;
            bind_uniform_buffer(index, buffer, 0, 0);
        }
    }

    void native_bind_buffer_base(U32 target, U32 index, U32 buffer)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        bind_buffer_base(target, index, buffer);
    }

    // ------------------------------------------------------------------
    // research/245: the GL buffer pools given back while VRAM is short
    // (LLRHI::sBufferCompact). After a teleport from a heavy place the old
    // region's buffers went once the simulator closed it, but the GL buffer
    // pools' blocks stayed part full (977 MB used in 3168 MB for minutes): VMA
    // gives a block back only once it is empty. VRAM stayed past the driver's
    // budget (paging, 1-5 fps), the discard bias at 4 and texture pressure at 3.
    //
    // Every buffer keeps its whole CPU copy (Buffer::data), and gpu_buffer()
    // makes the Vulkan copy again from it whenever there is none (the same
    // re-creation as a growth or a rewrite after a draw). So the buffers of the
    // blocks with the least in use drop their Vulkan copy (deferred to the end
    // of the frames in flight, as every retirement): one drawn is made again
    // at its next draw, in the fullest block with room (VMA tries blocks with
    // the least free space first), one not drawn lately only when drawn again.
    // The emptied blocks go back to the driver (VMA frees an empty block while
    // over the budget, or when it has another empty one; for the frames after
    // a pass any, research/285 X4). Nothing reads a GL buffer's Vulkan copy
    // but through gpu_buffer() each frame (draws, uniform blocks by
    // gpu_serial; the pools copy from the CPU copy).
    namespace
    {
        constexpr U64 DRAWN_BYTES_PER_PASS = 24 * MB;   // made again at their next draw (the upload ring is 256 MB a frame)
        constexpr F64 DRAWN_SECONDS = 2.0;              // drawn this lately: made again at once
        constexpr U64 MAX_POOLED_SIZE = 4 * MB;         // the GL buffer pools take buffers up to 4 MB (llvkdevice.cpp)
        constexpr U32 MAX_BUFFERS_PER_PASS = 8192;      // copies dropped a pass (each a deferred vmaDestroyBuffer)
        constexpr F32 IDLE_SECONDS_MAX = 60.f;          // PlusBufferCompactIdle's top: the frame history below reaches 63 s
        constexpr U8 USE_PINNED = 3;                    // a buffer of the block that stays (uniform, no CPU copy)

        struct CompactItem
        {
            VkDeviceMemory memory;
            Buffer* buffer;
            GLuint name;
            U8 use;                 // 2: drawn lately, 1: drawn within the idle time, 0: idle, USE_PINNED: stays
            U64 size;
            U64 block_size;
        };
        struct CompactBlock
        {
            U32 first = 0;          // into sCompactItems (sorted by block)
            U32 count = 0;
            U64 block_size = 0;
            U64 bytes = 0;          // what the GL buffers take of it
            U64 active = 0;         // of that, drawn within the idle time (or lately when idle buffers stay)
            U64 drawn = 0;          // of that, drawn lately
            bool pinned = false;    // holds a buffer that stays: the block cannot empty
        };
        // Kept between passes (no allocation once grown). Under sLock.
        std::vector<CompactItem> sCompactItems;
        std::vector<CompactBlock> sCompactBlocks;
        std::vector<U32> sCompactOrder;
        LLRHI::BufferCompactCounts sCompactCounts;
        LLRHI::BufferCompactCounts sCompactLogged;     // the totals at the last log line

        // The tracker frame at about each second, the last 64 s
        // (IDLE_SECONDS_MAX and DRAWN_SECONDS fit in it): a buffer's age in
        // frames against a time (the frame rate varies a lot here).
        FrameHistory sFrameHistory;
        F64 sNextCheck = 0.0;
        F64 sLastLog = -1.0e9;
        U64 sLastPassBlocks = 0;        // the GL buffer pools' block bytes at the last pass that dropped copies
        U32 sLastPassFrame = 0;         // the tracker frame of that pass
        U32 sNoProgressPasses = 0;
    } // namespace

    // Called by LLVKGL::compactBuffers while compaction is on (the switches
    // are tested there, before the lock).
    void compact_buffers()
    {
        const F64 now = steady_seconds();
        sFrameHistory.note(now, sFrame);
        if (now < sNextCheck || (sLastPassBlocks && sFrame - sLastPassFrame < COMPACT_PASS_FRAMES))
        {
            return;
        }
        sNextCheck = now + COMPACT_CHECK_SECONDS;

        LLVKDevice& device = LLVKDevice::instance();
        if (!device.sizedPools())
        {
            sNextCheck = now + COMPACT_REST_SECONDS;
            return;     // LLRHI_VK_POOLS=0: GL buffers share VMA's blocks with the rest (no state started)
        }
        VmaAllocator allocator = device.getAllocator();
        // research/285 (compact-hysteresis): when to run and how far, shared
        // with the texture pools' compaction.
        CompactCheck check;
        if (!allocator || !vram_compaction_check(check))
        {
            if (!check.compacting)
            {
                sNoProgressPasses = 0;
            }
            return;     // VRAM has room (or the last passes' frees are still to come): nothing moves
        }
        const U64 usage = check.usage, budget = check.budget;
        U64 pool_used = 0, pool_blocks = 0;
        if (!device.poolBytes(LLVKDevice::POOL_GL_BUFFER, pool_used, pool_blocks))
        {
            sNextCheck = now + COMPACT_REST_SECONDS;
            return;     // no allocator any more
        }

        // Windows in frames: drawn lately, and drawn within the idle time.
        U32 frame = 0;
        const U32 drawn_window = sFrameHistory.frameBefore(now, DRAWN_SECONDS, frame) ? llmax(sFrame - frame, 2u) : 8u;
        const F32 idle_seconds = llclamp(LLRHI::sBufferCompactIdleSeconds.load(std::memory_order_relaxed), 0.f, IDLE_SECONDS_MAX);
        U32 idle_window = U32_MAX;      // none idle
        if (idle_seconds > 0.f && sFrameHistory.frameBefore(now, idle_seconds, frame))
        {
            idle_window = llmax(sFrame - frame, drawn_window);
        }
        const bool idle_on = idle_window != U32_MAX;
        if (!idle_on && pool_blocks < pool_used + 64 * MB)
        {
            sNextCheck = now + COMPACT_REST_SECONDS;
            vram_compaction_rested(COMPACT_BUFFERS, true);
            return;     // the blocks are full and idle buffers stay: nothing to give back
        }

        // The GL buffers' Vulkan copies in the GL buffer pools, by block.
        sCompactItems.clear();
        sBuffers.forEachNamed([&](GLuint name, Buffer& b)
        {
            if (!b.gpu || !b.gpu_memory)
            {
                return;
            }
            VmaAllocationInfo2 info = {};
            vmaGetAllocationInfo2(allocator, b.gpu_memory, &info);
            const U64 block_size = info.blockSize;
            // Only the pools' blocks (16, 32, 64 MB; buffers up to 4 MB): a larger
            // buffer, or one in memory of its own, would only come back where it was.
            // By the block's pool, not its size: VMA's default blocks come in
            // these sizes too (its first blocks are 1/8 to 1/2 of the preferred
            // size; 64 MB since research/285 X3), and other resources share
            // them: they would not empty.
            if (info.dedicatedMemory || !info.allocationInfo.deviceMemory || info.allocationInfo.size > MAX_POOLED_SIZE ||
                !device.inSizedPool(b.gpu_memory))
            {
                return;
            }
            const U32 age = sFrame - b.used_frame;     // used_frame is a frame up to sFrame
            // A uniform block's buffer, or one with no CPU copy to make it again
            // from, stays: its block is left alone (it could not empty).
            const U8 use = (b.uniform_source || b.data.empty()) ? USE_PINNED
                         : age <= drawn_window ? 2 : (!idle_on || age <= idle_window) ? 1 : 0;
            sCompactItems.push_back({ info.allocationInfo.deviceMemory, &b, name, use, info.allocationInfo.size, block_size });
        });
        std::sort(sCompactItems.begin(), sCompactItems.end(),
                  [](const CompactItem& a, const CompactItem& b) { return a.memory < b.memory; });
        sCompactBlocks.clear();
        for (U32 i = 0; i < (U32)sCompactItems.size(); ++i)
        {
            const CompactItem& item = sCompactItems[i];
            if (sCompactBlocks.empty() || sCompactItems[sCompactBlocks.back().first].memory != item.memory)
            {
                CompactBlock block;
                block.first = i;
                block.block_size = item.block_size;
                sCompactBlocks.push_back(block);
            }
            CompactBlock& block = sCompactBlocks.back();
            ++block.count;
            block.bytes += item.size;
            block.active += item.use > 0 ? item.size : 0;
            block.drawn += item.use == 2 ? item.size : 0;
            block.pinned = block.pinned || item.use == USE_PINNED;
        }
        // The blocks at most half in use (idle buffers not counted while they
        // may go), the least in use first.
        sCompactOrder.clear();
        for (U32 i = 0; i < (U32)sCompactBlocks.size(); ++i)
        {
            if (!sCompactBlocks[i].pinned && sCompactBlocks[i].active * 2 <= sCompactBlocks[i].block_size)
            {
                sCompactOrder.push_back(i);
            }
        }
        std::sort(sCompactOrder.begin(), sCompactOrder.end(), [](U32 a, U32 b)
        {
            const CompactBlock& x = sCompactBlocks[a];
            const CompactBlock& y = sCompactBlocks[b];
            return x.active != y.active ? x.active < y.active : x.bytes < y.bytes;
        });

        const U64 target = check.to_free;
        U64 freed = 0, drawn_bytes = 0, other_bytes = 0;
        U32 buffers = 0, blocks = 0;
        for (U32 index : sCompactOrder)
        {
            if (freed >= target)
            {
                break;
            }
            const CompactBlock& block = sCompactBlocks[index];
            if (drawn_bytes + block.drawn > DRAWN_BYTES_PER_PASS)
            {
                continue;   // too much to make again this pass; a later pass
            }
            if (buffers && buffers + block.count > MAX_BUFFERS_PER_PASS)
            {
                break;      // enough deferred destructions for one frame; a later pass
            }
            for (U32 i = block.first; i < block.first + block.count; ++i)
            {
                CompactItem& item = sCompactItems[i];
                forget_resolved_buffer(item.name);
                retire_gpu(*item.buffer);   // gpu_buffer() makes it again (all of it dirty) when next drawn
                item.buffer->compacted = true;
                item.buffer->markDirty(0, item.buffer->data.size());
                ++buffers;
                (item.use == 2 ? drawn_bytes : other_bytes) += item.size;
            }
            // An estimate from above: the block goes back once every allocation in
            // it is gone, and only the GL buffers counted here are known (no other
            // kind shares the pool; for the frames after a pass VMA keeps no empty
            // block, research/285 compact-hysteresis). What is in use lately comes
            // back elsewhere.
            freed += block.block_size - block.active;
            ++blocks;
        }
        if (!buffers)
        {
            sNextCheck = now + COMPACT_REST_SECONDS;
            vram_compaction_rested(COMPACT_BUFFERS, true);
            return;
        }
        // No progress over a few passes (the blocks fill again as fast as they
        // empty): rest, so buffers are not made again and again.
        bool no_progress = false;
        if (sLastPassBlocks && pool_blocks >= sLastPassBlocks)
        {
            if (++sNoProgressPasses >= 6)
            {
                sNoProgressPasses = 0;
                sNextCheck = now + COMPACT_REST_SECONDS;
                no_progress = true;
            }
        }
        else
        {
            sNoProgressPasses = 0;
        }
        sLastPassBlocks = pool_blocks;
        sLastPassFrame = sFrame;
        vram_compaction_freed(freed);   // research/285: the texture pass does not ask for it again meanwhile
        vram_compaction_rested(COMPACT_BUFFERS, no_progress);

        ++sCompactCounts.passes;
        sCompactCounts.buffers += buffers;
        sCompactCounts.drawn_bytes += drawn_bytes;
        sCompactCounts.other_bytes += other_bytes;
        if (now - sLastLog >= 5.0)
        {
            sLastLog = now;
            const LLRHI::BufferCompactCounts& c = sCompactCounts;
            const LLRHI::BufferCompactCounts& l = sCompactLogged;
            LL_INFOS("VKBuffers") << "VRAM short (" << (usage / MB) << " of " << (budget / MB)
                                  << " MB): GL buffer copies given back (research/245): this pass " << buffers << " buffers from "
                                  << blocks << " blocks (" << (drawn_bytes / MB) << " MB drawn lately, " << (other_bytes / MB)
                                  << " MB not), GL buffer pools " << (pool_used / MB) << " MB in " << (pool_blocks / MB)
                                  << " MB before it; since the last line " << (c.passes - l.passes) << " passes, "
                                  << (c.buffers - l.buffers) << " buffers, " << ((c.drawn_bytes - l.drawn_bytes) / MB) << " MB drawn, "
                                  << ((c.other_bytes - l.other_bytes) / MB) << " MB not; idle after "
                                  << (idle_on ? llformat("%.0f s", idle_seconds) : std::string("never (kept)")) << LL_ENDL;
            sCompactLogged = sCompactCounts;
        }
    }

    // research/285: remakes left to a later frame (most often after the
    // compaction stopped, when the camera turns to what it gave back; also
    // after PlusBufferCompact was turned off, for the copies it gave back
    // before): a line of their own, at most every 5 s and only when there were
    // any. Render thread, no lock (a load and a compare a frame).
    void log_held_remakes()
    {
        static U64 logged = 0;
        static F64 last_log = -1.0e9;
        const U64 held = sRemakesHeld.load(std::memory_order_relaxed);
        if (held == logged)
        {
            return;
        }
        const F64 now = steady_seconds();
        if (now - last_log < 5.0)
        {
            return;
        }
        last_log = now;
        LL_INFOS("VKBuffers") << (held - logged) << " buffer-frames drawn a frame or more late since the last line (a GL buffer each frame"
                              << " its copy, given back while VRAM was short, waited for the remakes' " << (REMADE_BYTES_PER_FRAME / MB)
                              << " MB a frame, research/285)" << LL_ENDL;
        logged = held;
    }

    void buffer_compact_counts(LLRHI::BufferCompactCounts& out)
    {
        out = sCompactCounts;
        LLVKDevice& device = LLVKDevice::instance();
        device.poolBytes(LLVKDevice::POOL_GL_BUFFER, out.gl_pool_used, out.gl_pool_blocks);
        device.poolBytes(LLVKDevice::POOL_TEXTURE, out.texture_pool_used, out.texture_pool_blocks);
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
