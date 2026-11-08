/**
 * @file llvkgpudriven.cpp
 * @brief GPU-driven drawing: vertex and index buffers pooled into large buffers (research/46).
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

#include "llvkgpudriven.h"

#include "llglheaders.h"
#include "llrhi.h"      // LLRHI::envNumber
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using GLName = LLVKTracker::GLName;
    using LLVKGPUDriven::POSITION_STRIDE;

    constexpr U32 CHUNK_BYTES = 64u * 1024 * 1024;
    // Allocation unit: a multiple of the position stride and of both index
    // sizes, so every range starts at a whole vertex and a whole index from
    // the start of its pool buffer (vertexOffset and firstIndex count those).
    constexpr U32 GRANULE = 48;
    // Bytes copied into the pool per frame, through the frame's upload ring
    // (256 MB, shared with everything else uploaded in the frame).
    constexpr U64 COPY_BUDGET = 8ull * 1024 * 1024;
    // A range dropped while frame f is recorded was last read by frame f at the
    // latest; frame f + 2 starts after frame f finished (two in flight). One
    // more for drops between frames, after the counter moved.
    constexpr U32 RELEASE_DELAY = 3;
    static_assert(RELEASE_DELAY == LLVKMirror::FRAMES_IN_FLIGHT + 1, "RELEASE_DELAY: the frames in flight and one more");
    constexpr U32 IDLE_FRAMES = 1800;   // copies not drawn for this long go; the pair is copied again when drawn
    constexpr U32 SWEEP_FRAMES = 256;
    // A GL buffer whose copies were dropped twice within this many frames is
    // being rewritten (flexible prims, scripted changes): it is drawn from GL
    // until it has been left alone this long.
    constexpr U32 CHURN_FRAMES = 120;

    U64 round_up(U64 value, U64 unit)
    {
        return (value + unit - 1) / unit * unit;
    }

    // Free ranges of one pool buffer: best fit, freed ranges merged with their neighbours.
    class RangeAllocator
    {
    public:
        void reset(U32 size)
        {
            mBySize.clear();
            mByOffset.clear();
            insert(0, size);
        }

        bool allocate(U32 size, U32& offset)
        {
            auto it = mBySize.lower_bound(size);
            if (it == mBySize.end())
            {
                return false;
            }
            const U32 have = it->first;
            const U32 at = it->second;
            mBySize.erase(it);
            mByOffset.erase(at);
            if (have > size)
            {
                insert(at + size, have - size);
            }
            offset = at;
            return true;
        }

        void release(U32 offset, U32 size)
        {
            auto next = mByOffset.lower_bound(offset);
            if (next != mByOffset.end() && offset + size == next->first)
            {
                size += next->second;
                eraseBySize(next->first, next->second);
                next = mByOffset.erase(next);
            }
            if (next != mByOffset.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    offset = prev->first;
                    size += prev->second;
                    eraseBySize(prev->first, prev->second);
                    mByOffset.erase(prev);
                }
            }
            insert(offset, size);
        }

    private:
        void insert(U32 offset, U32 size)
        {
            mByOffset[offset] = size;
            mBySize.emplace(size, offset);
        }

        void eraseBySize(U32 offset, U32 size)
        {
            auto range = mBySize.equal_range(size);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (it->second == offset)
                {
                    mBySize.erase(it);
                    return;
                }
            }
        }

        std::multimap<U32, U32> mBySize;    // size -> offset
        std::map<U32, U32> mByOffset;       // offset -> size
    };

    // A vertex/index buffer pair as copied into the pool (bytes 0: it could not be).
    struct Entry
    {
        GLName vertex_buffer = 0;
        GLName index_buffer = 0;
        U32 index_type = 0;
        U32 position_offset = 0;
        U32 vertex_count = 0;
        U32 index_count = 0;
        U32 chunk = 0;
        U32 offset = 0;
        U32 bytes = 0;
        U32 used_frame = 0;
        LLVKGPUDriven::Placement placement;
        // Holders that keep the copy (the GPU shadow list's rows): told when it
        // is dropped, and the copy is never dropped for being idle.
        std::vector<U32> pins;
    };

    struct Chunk
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        RangeAllocator free;
    };

    // A dropped copy's range, free again once no frame in flight reads it.
    struct Release
    {
        U32 chunk = 0;
        U32 offset = 0;
        U32 bytes = 0;
        U32 frame = 0;
    };

    struct Churn
    {
        U32 frame = 0;      // when the buffer's copies were last dropped
        U32 strikes = 0;    // drops within CHURN_FRAMES of the one before
    };

    enum class CopyResult
    {
        Placed,
        Later,      // the pool, the frame's budget or the upload ring is full: try again later
        Never,      // the data does not fit the description: drawn from GL until GL writes it
    };

    struct Pool
    {
        bool tried = false;
        bool supported = false;
        bool callback = false;
        bool used = false;
        U32 max_chunks = 4;
        std::vector<Chunk> chunks;
        std::unordered_map<U64, Entry> entries;                     // (vertex buffer << 32) | index buffer
        std::unordered_map<GLName, std::vector<U64>> by_buffer;     // GL buffer -> keys of the entries made from it
        std::unordered_map<GLName, Churn> churn;
        std::vector<Release> releases;
        U32 frame = U32_MAX;            // the frame place() last ran in
        U64 copied_this_frame = 0;
        U32 last_sweep = 0;
        U64 used_bytes = 0;
        // Since the last takeStats.
        U32 copies = 0, drops = 0, never = 0, later_budget = 0, later_full = 0, later_ring = 0, rewritten = 0;
        U64 copied_bytes = 0;
        void (*pin_dropped)(U32 pin) = nullptr;
    };
    // Never destroyed: GL buffers can still be deleted (and reported) while
    // static objects go away at exit.
    Pool& sPool = *new Pool();

    void drop(Pool& p, Entry& e)
    {
        if (p.pin_dropped)
        {
            for (U32 pin : e.pins)
            {
                p.pin_dropped(pin);
            }
        }
        e.pins.clear();
        if (e.bytes)
        {
            Release r;
            r.chunk = e.chunk;
            r.offset = e.offset;
            r.bytes = e.bytes;
            r.frame = LLVKTracker::frame();
            p.releases.push_back(r);
            e.bytes = 0;
            ++p.drops;
        }
    }

    bool churning(const Pool& p, GLName name, U32 frame)
    {
        auto it = p.churn.find(name);
        return it != p.churn.end() && it->second.strikes >= 2 && frame - it->second.frame < CHURN_FRAMES;
    }

    void remember(Pool& p, GLName name, U64 key)
    {
        std::vector<U64>& keys = p.by_buffer[name];
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
        {
            keys.push_back(key);
        }
    }

    // LLVKTracker's buffer callback: GL wrote the buffer or deleted it.
    void on_buffer_changed(GLName name)
    {
        Pool& p = sPool;
        auto it = p.by_buffer.find(name);
        if (it == p.by_buffer.end())
        {
            return;     // nothing was copied from it (most writes: UI, immediate mode, uniform blocks)
        }
        for (U64 key : it->second)
        {
            auto e = p.entries.find(key);
            if (e != p.entries.end() && (e->second.vertex_buffer == name || e->second.index_buffer == name))
            {
                drop(p, e->second);
                p.entries.erase(e);
            }
        }
        p.by_buffer.erase(it);
        const U32 frame = LLVKTracker::frame();
        Churn& c = p.churn[name];
        c.strikes = c.strikes && frame - c.frame < CHURN_FRAMES ? c.strikes + 1 : 1;
        c.frame = frame;
    }

    void sweep(Pool& p, U32 frame)
    {
        p.last_sweep = frame;
        for (auto it = p.entries.begin(); it != p.entries.end(); )
        {
            if (frame - it->second.used_frame > IDLE_FRAMES && it->second.pins.empty())
            {
                drop(p, it->second);
                it = p.entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
        // The buffer index again, without the keys of entries gone since.
        p.by_buffer.clear();
        for (const auto& kv : p.entries)
        {
            p.by_buffer[kv.second.vertex_buffer].push_back(kv.first);
            if (kv.second.index_buffer != kv.second.vertex_buffer)
            {
                p.by_buffer[kv.second.index_buffer].push_back(kv.first);
            }
        }
        for (auto it = p.churn.begin(); it != p.churn.end(); )
        {
            if (frame - it->second.frame >= CHURN_FRAMES)
            {
                it = p.churn.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void begin_frame(Pool& p, U32 frame)
    {
        p.frame = frame;
        p.copied_this_frame = 0;
        // Ranges no frame in flight reads any more go back to their pool buffer.
        size_t kept = 0;
        for (size_t i = 0; i < p.releases.size(); ++i)
        {
            const Release r = p.releases[i];
            if (frame - r.frame >= RELEASE_DELAY)
            {
                p.chunks[r.chunk].free.release(r.offset, r.bytes);
                p.used_bytes -= r.bytes;
            }
            else
            {
                p.releases[kept++] = r;
            }
        }
        p.releases.resize(kept);
        if (frame - p.last_sweep >= SWEEP_FRAMES)
        {
            sweep(p, frame);
        }
    }

    bool add_chunk(Pool& p)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = CHUNK_BYTES;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        Chunk chunk;
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &gpu, &chunk.buffer, &chunk.memory, nullptr) != VK_SUCCESS)
        {
            LL_WARNS("VKGPUDriven") << "pool buffer " << p.chunks.size() + 1 << " could not be allocated; the pool stays at "
                                    << p.chunks.size() * (CHUNK_BYTES >> 20) << " MB" << LL_ENDL;
            p.max_chunks = (U32)p.chunks.size();
            return false;
        }
        chunk.free.reset(CHUNK_BYTES);
        p.chunks.push_back(std::move(chunk));
        LL_INFOS("VKGPUDriven") << "pool buffer " << p.chunks.size() << " of " << p.max_chunks << " (" << (CHUNK_BYTES >> 20) << " MB each)" << LL_ENDL;
        return true;
    }

    // Copies the pair into the pool through this frame's upload commands.
    CopyResult copy_in(Pool& p, Entry& e)
    {
        size_t vertex_bytes_gl = 0, index_bytes_gl = 0;
        const U8* vertices = LLVKTracker::bufferContents(e.vertex_buffer, vertex_bytes_gl);
        const U8* indices = LLVKTracker::bufferContents(e.index_buffer, index_bytes_gl);
        const bool index32 = e.index_type == GL_UNSIGNED_INT;
        if (!vertices || !indices || (!index32 && e.index_type != GL_UNSIGNED_SHORT))
        {
            return CopyResult::Never;
        }
        const U32 index_size = index32 ? 4 : 2;
        if ((U64)e.position_offset + (U64)e.vertex_count * 16 > vertex_bytes_gl || (U64)e.index_count * index_size > index_bytes_gl)
        {
            return CopyResult::Never;
        }
        const U64 vertex_bytes = round_up((U64)e.vertex_count * POSITION_STRIDE, GRANULE);
        const U64 index_bytes = (U64)e.index_count * index_size;
        const U64 total = vertex_bytes + round_up(index_bytes, GRANULE);
        if (total > CHUNK_BYTES)
        {
            return CopyResult::Never;
        }
        // Every index has to name one of the pair's vertices: past them the pool
        // holds other pairs (where GL would read the buffer's other arrays).
        U32 max_index = 0;
        if (index32)
        {
            const U32* src = (const U32*)indices;
            for (U32 i = 0; i < e.index_count; ++i)
            {
                max_index = llmax(max_index, src[i]);
            }
        }
        else
        {
            const U16* src = (const U16*)indices;
            for (U32 i = 0; i < e.index_count; ++i)
            {
                max_index = llmax(max_index, (U32)src[i]);
            }
        }
        if (max_index >= e.vertex_count)
        {
            return CopyResult::Never;
        }

        if (p.copied_this_frame + total > COPY_BUDGET)
        {
            ++p.later_budget;
            return CopyResult::Later;
        }
        U32 chunk = U32_MAX;
        U32 offset = 0;
        for (U32 c = 0; c < (U32)p.chunks.size() && chunk == U32_MAX; ++c)
        {
            if (p.chunks[c].free.allocate((U32)total, offset))
            {
                chunk = c;
            }
        }
        if (chunk == U32_MAX)
        {
            if (p.chunks.size() >= p.max_chunks || !add_chunk(p) || !p.chunks.back().free.allocate((U32)total, offset))
            {
                ++p.later_full;
                return CopyResult::Later;
            }
            chunk = (U32)p.chunks.size() - 1;
        }

        LLVKMirror& mirror = LLVKMirror::instance();
        VkCommandBuffer cmd = mirror.uploadCommands();
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceSize staging_offset = 0;
        void* dst = nullptr;
        if (!cmd || !mirror.allocate(total, 16, staging, staging_offset, dst))
        {
            p.chunks[chunk].free.release(offset, (U32)total);     // never written: free at once
            ++p.later_ring;
            return CopyResult::Later;
        }
        U8* out = (U8*)dst;
        const U8* positions = vertices + e.position_offset;
        for (U32 i = 0; i < e.vertex_count; ++i)
        {
            memcpy(out + (size_t)i * POSITION_STRIDE, positions + (size_t)i * 16, POSITION_STRIDE);  // x, y, z of an LLVector4a
        }
        const size_t vertex_used = (size_t)e.vertex_count * POSITION_STRIDE;
        memset(out + vertex_used, 0, (size_t)vertex_bytes - vertex_used);
        memcpy(out + vertex_bytes, indices, (size_t)index_bytes);
        memset(out + vertex_bytes + index_bytes, 0, (size_t)(total - vertex_bytes - index_bytes));
        VkBufferCopy region = { staging_offset, offset, total };
        vkCmdCopyBuffer(cmd, staging, p.chunks[chunk].buffer, 1, &region);

        e.chunk = chunk;
        e.offset = offset;
        e.bytes = (U32)total;
        e.placement.chunk = chunk;
        e.placement.vertex_offset = (S32)(offset / POSITION_STRIDE);
        e.placement.first_index = (U32)((offset + vertex_bytes) / index_size);
        e.placement.index_count = e.index_count;
        e.placement.index32 = index32;
        p.copied_this_frame += total;
        p.copied_bytes += total;
        p.used_bytes += total;
        ++p.copies;
        return CopyResult::Placed;
    }
}

bool LLVKGPUDriven::supported()
{
    Pool& p = sPool;
    if (!p.tried)
    {
        p.tried = true;
        const LLVKDevice& device = LLVKDevice::instance();
        p.supported = device.isReady() && device.hasMultiDrawIndirect() && device.hasDrawIndirectFirstInstance();
        // LLRHI_GPU_DRIVEN_POOL_MB: the most the pool grows to (64 MB buffers, default 256 MB).
        U64 mb = 256;
        LLRHI::envNumber("LLRHI_GPU_DRIVEN_POOL_MB", 0, ~(U64)0, mb);
        p.max_chunks = (U32)llclamp(mb / (CHUNK_BYTES >> 20), (U64)1, (U64)MAX_CHUNKS);
        if (p.supported)
        {
            LL_INFOS("VKGPUDriven") << "geometry pool up to " << p.max_chunks * (CHUNK_BYTES >> 20) << " MB" << LL_ENDL;
        }
        else
        {
            LL_WARNS("VKGPUDriven") << "no multi-draw indirect with a first instance: GPU-driven draws unavailable" << LL_ENDL;
        }
    }
    return p.supported;
}

bool LLVKGPUDriven::place(U32 vertex_buffer, U32 index_buffer, U32 index_type, U32 position_offset, U32 vertex_count, U32 index_count,
                          Placement& out)
{
    Pool& p = sPool;
    if (!vertex_buffer || !index_buffer || !vertex_count || !index_count || !supported())
    {
        return false;
    }
    if (!p.callback)
    {
        // From the first copy on; it stays when the switch goes off, so copies
        // never outlive a GL write.
        LLVKTracker::setBufferChangedCallback(&on_buffer_changed);
        p.callback = true;
    }
    p.used = true;
    const U32 frame = LLVKTracker::frame();
    if (frame != p.frame)
    {
        begin_frame(p, frame);
    }

    const U64 key = ((U64)vertex_buffer << 32) | index_buffer;
    auto it = p.entries.find(key);
    if (it != p.entries.end())
    {
        Entry& e = it->second;
        if (e.index_type == index_type && e.position_offset == position_offset && e.vertex_count == vertex_count &&
            e.index_count == index_count)
        {
            e.used_frame = frame;
            if (!e.bytes)
            {
                return false;
            }
            out = e.placement;
            return true;
        }
        // The viewer describes the pair differently than when it was copied: copy it again.
        drop(p, e);
        p.entries.erase(it);
    }
    if (churning(p, vertex_buffer, frame) || churning(p, index_buffer, frame))
    {
        ++p.rewritten;
        return false;
    }

    Entry e;
    e.vertex_buffer = vertex_buffer;
    e.index_buffer = index_buffer;
    e.index_type = index_type;
    e.position_offset = position_offset;
    e.vertex_count = vertex_count;
    e.index_count = index_count;
    e.used_frame = frame;
    const CopyResult result = copy_in(p, e);
    if (result == CopyResult::Later)
    {
        return false;
    }
    p.entries[key] = e;
    remember(p, vertex_buffer, key);
    if (index_buffer != vertex_buffer)
    {
        remember(p, index_buffer, key);
    }
    if (result == CopyResult::Never)
    {
        ++p.never;
        return false;
    }
    out = e.placement;
    return true;
}

bool LLVKGPUDriven::placePinned(U32 vertex_buffer, U32 index_buffer, U32 index_type, U32 position_offset, U32 vertex_count,
                                U32 index_count, U32 pin, Placement& out)
{
    if (!place(vertex_buffer, index_buffer, index_type, position_offset, vertex_count, index_count, out))
    {
        return false;
    }
    auto it = sPool.entries.find(((U64)vertex_buffer << 32) | index_buffer);
    if (it == sPool.entries.end() || !it->second.bytes)
    {
        return false;   // cannot happen: place() just found or made it
    }
    it->second.pins.push_back(pin);
    return true;
}

void LLVKGPUDriven::unpin(U32 vertex_buffer, U32 index_buffer, U32 pin)
{
    auto it = sPool.entries.find(((U64)vertex_buffer << 32) | index_buffer);
    if (it == sPool.entries.end())
    {
        return;
    }
    std::vector<U32>& pins = it->second.pins;
    auto found = std::find(pins.begin(), pins.end(), pin);
    if (found != pins.end())
    {
        *found = pins.back();
        pins.pop_back();
        it->second.used_frame = LLVKTracker::frame();   // idle from now on
    }
}

void LLVKGPUDriven::setPinDroppedCallback(void (*callback)(U32 pin))
{
    sPool.pin_dropped = callback;
}

VkBuffer LLVKGPUDriven::chunkBuffer(U32 chunk)
{
    return chunk < sPool.chunks.size() ? sPool.chunks[chunk].buffer : VK_NULL_HANDLE;
}

std::string LLVKGPUDriven::takeStats(U32 frames)
{
    Pool& p = sPool;
    if (!p.used)
    {
        return std::string();
    }
    frames = llmax(frames, 1u);
    std::ostringstream out;
    out << "pool " << (p.used_bytes >> 20) << " of " << (p.chunks.size() * (CHUNK_BYTES >> 20)) << " MB, pairs " << p.entries.size()
        << ", copied " << p.copies << " (" << (p.copied_bytes >> 20) << " MB), dropped " << p.drops << ", not copyable " << p.never
        << ", deferred (budget " << p.later_budget << ", full " << p.later_full << ", ring " << p.later_ring << ", rewritten "
        << p.rewritten << ") over " << frames << " frames";
    p.copies = p.drops = p.never = p.later_budget = p.later_full = p.later_ring = p.rewritten = 0;
    p.copied_bytes = 0;
    return out.str();
}

std::string LLVKGPUDriven::memoryReport()
{
    Pool& p = sPool;
    if (!p.used)
    {
        return std::string();
    }
    std::ostringstream out;
    out << "geometry pool " << (p.chunks.size() * (CHUNK_BYTES >> 20)) << " MB in " << p.chunks.size() << " buffers ("
        << (p.used_bytes >> 20) << " MB used, at most " << (p.max_chunks * (CHUNK_BYTES >> 20)) << " MB)";
    return out.str();
}
