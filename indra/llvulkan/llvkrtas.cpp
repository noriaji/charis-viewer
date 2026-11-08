/**
 * @file llvkrtas.cpp
 * @brief Hardware ray tracing for photos, stage R2 (research/230): the static scene's
 * acceleration structures.
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

#include "llvkrtas.h"

#include "llrhi.h"
#include "llrhihash.h"
#include "llvkattribpool.h"
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvkscene.h"
#include "llvkshader.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Limits (research/230 9.2). The triangle budget stands for "about 2 ms of
    // builds a frame"; what does not fit waits for the next frame.
    constexpr U32 MAX_INSTANCES = 65536;
    constexpr U64 BUILD_BUDGET_TRIANGLES = 500000;
    constexpr U32 MAX_BUILDS_A_FRAME = 256;
    constexpr VkDeviceSize SCRATCH_START = 8ull << 20;
    constexpr VkDeviceSize SCRATCH_CAP = 256ull << 20;
    constexpr U64 BLAS_MEMORY_BUDGET = 768ull << 20;
    constexpr U32 QUERY_SLOTS = 512;            // compacted size queries
    constexpr U32 TS_PAIRS = 8;                 // timestamp pairs (frames whose builds are not read back yet)
    constexpr U32 COLLECT_INTERVAL = 4;         // frames between two looks at the scene
    constexpr U32 TLAS_INTERVAL = 4;            // frames between two top level builds
    constexpr U32 UNUSED_FRAMES = 240;          // a structure no near record names for this long is freed
    constexpr U32 COMPACTS_A_FRAME = 32;
    constexpr U32 COMPACT_TRIES = 8;
    constexpr U32 NO_QUERY = U32_MAX;
    // Always with compaction allowed: the sizes asked for and the builds agree on the flags.
    constexpr VkBuildAccelerationStructureFlagsKHR BLAS_FLAGS =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    constexpr U32 SELFTEST_RUNS = 3;
    constexpr U32 SELFTEST_LANES = 8;

    struct Key
    {
        U32 number = 0;
        U32 writes = 0;
        U64 serial = 0;
        U32 chunk = 0;
        U32 vertex_base = 0;
        U32 first_index = 0;
        U32 index_count = 0;
        bool operator==(const Key& o) const
        {
            return number == o.number && writes == o.writes && serial == o.serial && chunk == o.chunk &&
                   vertex_base == o.vertex_base && first_index == o.first_index && index_count == o.index_count;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            U64 h = LLRHI::FNV1A_BASIS_SHORT;
            const U64 words[] = { k.number, k.writes, k.serial, k.chunk, k.vertex_base, k.first_index, k.index_count };
            for (U64 w : words)
            {
                h = LLRHI::fnv1aMix(h, w);
                h ^= h >> 29;
            }
            return (size_t)h;
        }
    };

    struct Buf
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    // Compaction: 0 nothing more to do, 1 the size is asked for and the GPU has
    // not finished the frame, 2 the frame is done (the size can be read).
    enum : U8
    {
        STAGE_DONE = 0,
        STAGE_WAIT_GPU = 1,
        STAGE_READ = 2,
    };

    struct Blas
    {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Buf storage;
        VkDeviceAddress address = 0;
        VkDeviceSize bytes = 0;
        U32 triangles = 0;
        U32 id = 0;
        U32 last_used = 0;
        U32 query = NO_QUERY;
        U32 tries = 0;
        U8 stage = STAGE_DONE;
        bool failed = false;        // no structure (too large, out of memory): not tried again while its key lives
    };

    struct Selected
    {
        U32 record = 0;     // index in State::records
        F32 distance = 0.f;
        Key key;
    };

    struct Pending
    {
        Blas* blas = nullptr;
        U32 selected = 0;       // the index in State::selected
        VkAccelerationStructureGeometryKHR geometry = {};
        VkAccelerationStructureBuildRangeInfoKHR range = {};
        VkDeviceSize scratch_offset = 0;
        VkDeviceSize scratch_size = 0;
        VkDeviceSize build_bytes = 0;
        bool compact = false;
    };

    struct Compaction
    {
        VkAccelerationStructureKHR from = VK_NULL_HANDLE;    // freed with its buffer once the frames in flight are done
        Buf from_storage;
        VkAccelerationStructureKHR to = VK_NULL_HANDLE;
    };

    // What the self test's frame leaves behind for its callback (the frame has finished by then).
    struct SelfRun
    {
        Buf io;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        U32 instances = 0;
        U32 targeted = 0;
        VkDevice device = VK_NULL_HANDLE;
    };

    struct State
    {
        // Loaded once.
        bool tried = false;
        bool loaded = false;
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = nullptr;
        PFN_vkCreateAccelerationStructureKHR fnCreate = nullptr;
        PFN_vkDestroyAccelerationStructureKHR fnDestroy = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR fnSizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR fnAddress = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR fnBuild = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR fnCopy = nullptr;
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR fnWriteProps = nullptr;
        VkDeviceSize scratch_align = 128;
        bool selftest_wanted = false;
        bool timestamps = false;
        F64 timestamp_ms = 0.0;

        // Built while on.
        bool active = false;
        bool ever_on = false;
        bool suspended = false;     // an allocation failed: nothing more until it is turned off and on
        std::unordered_map<Key, Blas, KeyHash> blases;
        U32 next_id = 1;
        U64 blas_bytes = 0;
        VkQueryPool query_pool = VK_NULL_HANDLE;
        std::vector<U32> free_queries;
        VkQueryPool ts_pool = VK_NULL_HANDLE;
        U32 ts_head = 0;
        Buf scratch;
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        Buf tlas_storage;
        Buf instance_buffer;
        U32 tlas_instances = 0;
        U64 tlas_hash = 0;
        U32 tlas_frame = 0;
        bool tlas_dirty = false;
        bool budget_logged = false;
        bool compaction_due = false;

        // The look at the scene.
        std::vector<LLVKScene::RayRecord> records;
        std::vector<LLVKScene::RayRecord> looked;      // a look being made (swapped into records when it worked)
        std::vector<Selected> selected;
        bool collected = false;
        bool more_pending = false;
        U32 last_collect = 0;
        U64 last_scene_serial = 0;
        U64 last_pool_serial = 0;
        F32 last_camera[3] = {};
        F32 last_range = 0.f;
        U32 last_masks[4] = {};

        // Reused every frame.
        std::vector<Pending> pending;
        std::unordered_set<Key, KeyHash> pending_keys;      // the keys in `pending` (two records may name the same triangles)
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_ptrs;
        std::vector<Compaction> compactions;
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        std::vector<U32> notify_ids;

        // GPU finished a frame (any thread: the mirror's deferred callbacks).
        std::mutex done_mutex;
        std::vector<U32> done_ids;
        std::vector<U32> done_slots;
        std::vector<U32> taken_ids;     // take_done's (swapped with done_*: no allocation once grown)
        std::vector<U32> taken_slots;
        // What a frame with work hands over when the GPU has finished it, so that its notification
        // holds two numbers (no allocation): a ring reused after TS_PAIRS such frames, long after the
        // GPU finished them (as the timestamp slots are). Under done_mutex.
        struct Flight
        {
            std::vector<U32> ids;
            U32 ts_slot = 0;
            bool timed = false;
        };
        Flight flights[TS_PAIRS];
        U32 flight_head = 0;
        // Moved by release() (under done_mutex): a notification of a frame before it is dropped, so
        // that a timestamp slot of the freed query pool is never read in a new one (review rt-as-post
        // low 2: turned off and on again before the GPU finished).
        U32 generation = 0;

        // Self test.
        U32 selftest_runs = 0;
        LLVKShader::Program selftest_shader;
        VkDescriptorSetLayout selftest_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout selftest_layout = VK_NULL_HANDLE;
        VkPipeline selftest_pipeline = VK_NULL_HANDLE;
        bool selftest_failed = false;

        // Since the last report.
        U32 stat_built = 0, stat_compacted = 0, stat_freed = 0, stat_tlas_builds = 0, stat_failed = 0;
        F64 stat_record_ms = 0.0, stat_gpu_ms = 0.0;
        U32 stat_gpu_builds = 0;
    };

    // Never destroyed: the mirror's deferred callbacks may run at exit.
    State& S = *new State();

    bool is_selftest_env()
    {
        const char* env = std::getenv("LLRHI_RT_SELFTEST");
        return env && *env == '1';
    }

    bool load()
    {
        if (S.tried)
        {
            return S.loaded;
        }
        S.tried = true;
        LLVKDevice& device = LLVKDevice::instance();
        S.device = device.getDevice();
        S.allocator = device.getAllocator();
        auto get = [&](const char* name) { return vkGetDeviceProcAddr(S.device, name); };
        S.fnCreate = (PFN_vkCreateAccelerationStructureKHR)get("vkCreateAccelerationStructureKHR");
        S.fnDestroy = (PFN_vkDestroyAccelerationStructureKHR)get("vkDestroyAccelerationStructureKHR");
        S.fnSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)get("vkGetAccelerationStructureBuildSizesKHR");
        S.fnAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)get("vkGetAccelerationStructureDeviceAddressKHR");
        S.fnBuild = (PFN_vkCmdBuildAccelerationStructuresKHR)get("vkCmdBuildAccelerationStructuresKHR");
        S.fnCopy = (PFN_vkCmdCopyAccelerationStructureKHR)get("vkCmdCopyAccelerationStructureKHR");
        S.fnWriteProps = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)get("vkCmdWriteAccelerationStructuresPropertiesKHR");
        if (!S.fnCreate || !S.fnDestroy || !S.fnSizes || !S.fnAddress || !S.fnBuild || !S.fnCopy || !S.fnWriteProps)
        {
            LL_WARNS("VKRt") << "ray tracing: the acceleration structure entry points are missing" << LL_ENDL;
            return false;
        }
        VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &as_props;
        vkGetPhysicalDeviceProperties2(device.getPhysicalDevice(), &props);
        S.scratch_align = llmax((VkDeviceSize)as_props.minAccelerationStructureScratchOffsetAlignment, (VkDeviceSize)16);
        S.timestamps = device.getLimits().timestampComputeAndGraphics == VK_TRUE;
        S.timestamp_ms = (F64)device.getLimits().timestampPeriod * 1e-6;
        S.selftest_wanted = is_selftest_env();
        S.loaded = true;
        return true;
    }

    VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
    {
        return (v + a - 1) / a * a;
    }

    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool readback, Buf& out)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo where = {};
        if (host)
        {
            where.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            where.flags = (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        else
        {
            where.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        }
        VmaAllocationInfo allocated = {};
        Buf made;
        if (vmaCreateBuffer(S.allocator, &info, &where, &made.buffer, &made.memory, &allocated) != VK_SUCCESS || (host && !allocated.pMappedData))
        {
            if (made.buffer)
            {
                vmaDestroyBuffer(S.allocator, made.buffer, made.memory);
            }
            return false;
        }
        VkBufferDeviceAddressInfo where_is = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        where_is.buffer = made.buffer;
        made.address = vkGetBufferDeviceAddress(S.device, &where_is);
        made.size = size;
        made.mapped = allocated.pMappedData;
        out = made;
        return true;
    }

    // Done with the frames recorded so far: destroy then (or now, at shutdown, with the device idle).
    template <class F>
    void retire(bool now, F&& destroy)
    {
        if (now)
        {
            destroy();
        }
        else
        {
            LLVKMirror::instance().defer(std::function<void()>(std::forward<F>(destroy)));
        }
    }

    void retire_buffer(bool now, Buf& b)
    {
        if (b.buffer)
        {
            const VmaAllocator allocator = S.allocator;
            const VkBuffer buffer = b.buffer;
            const VmaAllocation memory = b.memory;
            retire(now, [allocator, buffer, memory] { vmaDestroyBuffer(allocator, buffer, memory); });
        }
        b = Buf();
    }

    void retire_structure(bool now, VkAccelerationStructureKHR& as, Buf& storage)
    {
        if (as)
        {
            const VkDevice device = S.device;
            const VkAccelerationStructureKHR handle = as;
            const PFN_vkDestroyAccelerationStructureKHR destroy = S.fnDestroy;
            retire(now, [device, handle, destroy] { destroy(device, handle, nullptr); });
        }
        as = VK_NULL_HANDLE;
        retire_buffer(now, storage);
    }

    void retire_query_pool(bool now, VkQueryPool& pool)
    {
        if (pool)
        {
            const VkDevice device = S.device;
            const VkQueryPool handle = pool;
            retire(now, [device, handle] { vkDestroyQueryPool(device, handle, nullptr); });
        }
        pool = VK_NULL_HANDLE;
    }

    void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access)
    {
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    U64 as_memory()
    {
        return S.blas_bytes + S.tlas_storage.size + S.scratch.size + S.instance_buffer.size;
    }

    // Everything built, freed (now: at shutdown). Not the entry points or the self test's pipeline.
    void release(bool now, const char* why)
    {
        const size_t blas_count = S.blases.size();
        const U64 bytes = as_memory();
        for (auto& kv : S.blases)
        {
            retire_structure(now, kv.second.as, kv.second.storage);
        }
        S.blases.clear();
        S.blas_bytes = 0;
        retire_structure(now, S.tlas, S.tlas_storage);
        retire_buffer(now, S.instance_buffer);
        retire_buffer(now, S.scratch);
        retire_query_pool(now, S.query_pool);
        retire_query_pool(now, S.ts_pool);
        S.free_queries.clear();
        S.ts_head = 0;
        S.tlas_instances = 0;
        S.tlas_hash = 0;
        S.tlas_dirty = false;
        S.records.clear();
        S.selected.clear();
        S.collected = false;
        S.more_pending = false;
        S.suspended = false;
        S.budget_logged = false;
        S.compaction_due = false;
        S.selftest_runs = 0;
        {
            std::lock_guard<std::mutex> lock(S.done_mutex);
            S.done_ids.clear();
            S.done_slots.clear();
            ++S.generation;
        }
        if (S.active)
        {
            LL_INFOS("VKRt") << "ray tracing " << why << ": freed " << blas_count << " bottom level structures and the top level one, "
                             << (bytes >> 20) << " MB" << LL_ENDL;
        }
        S.active = false;
    }

    // F32 distance from the camera to a record's world space box (the box's centre point when its bounds are not known).
    F32 distance_to(const LLVKScene::RayRecord& r, const F32 camera[3])
    {
        const F32* m = r.matrix;
        if (r.loose)
        {
            const F32 dx = m[12] - camera[0], dy = m[13] - camera[1], dz = m[14] - camera[2];
            return sqrtf(dx * dx + dy * dy + dz * dz);
        }
        const F32 centre[3] = { 0.5f * (r.lo[0] + r.hi[0]), 0.5f * (r.lo[1] + r.hi[1]), 0.5f * (r.lo[2] + r.hi[2]) };
        const F32 half[3] = { 0.5f * fabsf(r.hi[0] - r.lo[0]), 0.5f * fabsf(r.hi[1] - r.lo[1]), 0.5f * fabsf(r.hi[2] - r.lo[2]) };
        F32 sum = 0.f;
        for (U32 i = 0; i < 3; ++i)
        {
            const F32 world = m[i] * centre[0] + m[4 + i] * centre[1] + m[8 + i] * centre[2] + m[12 + i];
            const F32 extent = fabsf(m[i]) * half[0] + fabsf(m[4 + i]) * half[1] + fabsf(m[8 + i]) * half[2];
            const F32 outside = llmax(fabsf(camera[i] - world) - extent, 0.f);
            sum += outside * outside;
        }
        return sqrtf(sum);
    }

    Key key_of(const LLVKScene::RayRecord& r)
    {
        Key k;
        k.number = r.number;
        k.writes = r.writes;
        k.serial = r.serial;
        k.chunk = r.chunk;
        k.vertex_base = r.vertex_base;
        k.first_index = r.first_index;
        k.index_count = r.index_count;
        return k;
    }

    // The triangles of a record, as the pool has them, for a bottom level build. False: not valid.
    bool describe(const LLVKScene::RayRecord& r, VkAccelerationStructureGeometryKHR& geometry, VkAccelerationStructureBuildRangeInfoKHR& range)
    {
        VkDeviceAddress positions = 0, indices = 0;
        VkDeviceSize position_bytes = 0, index_bytes = 0;
        if (!r.vertices || r.index_count < 3 || !LLVKAttribPool::streamAddress(r.chunk, 0, 16, positions, position_bytes) ||
            !LLVKAttribPool::indexAddress(r.chunk, indices, index_bytes))
        {
            return false;
        }
        // The place is inside the chunk's buffers (the pool made it so; a wrong one must not reach the driver).
        if (((U64)r.vertex_base + r.vertices) * 16 > position_bytes || ((U64)r.first_index + r.index_count) * 4 > index_bytes)
        {
            return false;
        }
        geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
        geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = positions + (VkDeviceSize)r.vertex_base * 16;
        geometry.geometry.triangles.vertexStride = 16;
        geometry.geometry.triangles.maxVertex = r.vertices - 1;
        geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geometry.geometry.triangles.indexData.deviceAddress = indices + (VkDeviceSize)r.first_index * 4;
        range = {};
        range.primitiveCount = r.index_count / 3;
        return true;
    }

    bool ensure_scratch(VkDeviceSize need)
    {
        if (need == 0)
        {
            return true;
        }
        // The buffer holds the room and the slack that aligns its address.
        if (S.scratch.size >= need + S.scratch_align)
        {
            return true;
        }
        if (need > SCRATCH_CAP)
        {
            return false;
        }
        VkDeviceSize size = llmax(SCRATCH_START, S.scratch.size * 2);
        size = llmin(llmax(size, need), SCRATCH_CAP);
        Buf grown;
        if (!make_buffer(size + S.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, false, grown))
        {
            return false;
        }
        retire_buffer(false, S.scratch);    // the frames in flight may still build into it
        S.scratch = grown;
        return true;
    }

    VkDeviceAddress scratch_at(VkDeviceSize offset)
    {
        return align_up(S.scratch.address, S.scratch_align) + offset;
    }

    // A structure of the sizes the build asked for.
    bool make_structure(VkAccelerationStructureTypeKHR type, VkDeviceSize bytes, VkAccelerationStructureKHR& as, Buf& storage,
                        VkDeviceAddress& address)
    {
        Buf made;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, false, made))
        {
            return false;
        }
        VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        info.buffer = made.buffer;
        info.size = bytes;
        info.type = type;
        VkAccelerationStructureKHR created = VK_NULL_HANDLE;
        if (S.fnCreate(S.device, &info, nullptr, &created) != VK_SUCCESS)
        {
            vmaDestroyBuffer(S.allocator, made.buffer, made.memory);
            return false;
        }
        VkAccelerationStructureDeviceAddressInfoKHR where = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        where.accelerationStructure = created;
        address = S.fnAddress(S.device, &where);
        as = created;
        storage = made;
        return true;
    }

    void take_done()
    {
        std::vector<U32>& ids = S.taken_ids;
        std::vector<U32>& slots = S.taken_slots;
        ids.clear();
        slots.clear();
        {
            std::lock_guard<std::mutex> lock(S.done_mutex);
            ids.swap(S.done_ids);
            slots.swap(S.done_slots);
        }
        if (!ids.empty())
        {
            std::sort(ids.begin(), ids.end());
            for (auto& kv : S.blases)
            {
                if (kv.second.stage == STAGE_WAIT_GPU && std::binary_search(ids.begin(), ids.end(), kv.second.id))
                {
                    kv.second.stage = STAGE_READ;
                    S.compaction_due = true;
                }
            }
        }
        for (U32 slot : slots)
        {
            U64 t[2] = {};
            if (S.ts_pool &&
                vkGetQueryPoolResults(S.device, S.ts_pool, slot * 2, 2, sizeof(t), t, sizeof(U64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                t[1] >= t[0])
            {
                S.stat_gpu_ms += (F64)(t[1] - t[0]) * S.timestamp_ms;
                ++S.stat_gpu_builds;
            }
        }
    }

    // Looks at the scene when something may have changed (research/230 9.2: only then).
    void collect(const LLRHI::RayTracingWanted& w, U32 frame, bool& changed)
    {
        const U64 scene_serial = LLVKScene::writeSerial();
        const U64 pool_serial = LLVKAttribPool::numberWriteSerial();
        const F32 dx = w.camera[0] - S.last_camera[0], dy = w.camera[1] - S.last_camera[1], dz = w.camera[2] - S.last_camera[2];
        const bool moved = dx * dx + dy * dy + dz * dz > (w.range * 0.125f) * (w.range * 0.125f);
        const bool passes_changed = memcmp(S.last_masks, w.opaque_passes, sizeof(S.last_masks)) != 0;
        const bool due = !S.collected || (frame - S.last_collect >= COLLECT_INTERVAL &&
                                          (S.more_pending || scene_serial != S.last_scene_serial || pool_serial != S.last_pool_serial ||
                                           moved || w.range != S.last_range || passes_changed));
        if (!due)
        {
            return;
        }
        U64 serial = 0;
        if (!LLVKScene::collectRayRecords(w.opaque_passes, S.looked, serial))
        {
            return;     // the scene cannot be read now: the last look (and the selection naming its records) stays
        }
        S.records.swap(S.looked);
        S.collected = true;
        S.last_collect = frame;
        S.last_scene_serial = serial;      // after the scene took its tables back (that writes rows)
        S.last_pool_serial = pool_serial;
        memcpy(S.last_camera, w.camera, sizeof(S.last_camera));
        S.last_range = w.range;
        memcpy(S.last_masks, w.opaque_passes, sizeof(S.last_masks));
        S.selected.clear();
        for (U32 i = 0; i < (U32)S.records.size(); ++i)
        {
            const LLVKScene::RayRecord& r = S.records[i];
            if (r.id >= (1u << 24))
            {
                continue;       // the instance's custom index holds 24 bits
            }
            const F32 d = distance_to(r, w.camera);
            if (d <= w.range)
            {
                Selected s;
                s.record = i;
                s.distance = d;
                s.key = key_of(r);
                S.selected.push_back(s);
            }
        }
        if (S.selected.size() > MAX_INSTANCES)
        {
            std::nth_element(S.selected.begin(), S.selected.begin() + MAX_INSTANCES, S.selected.end(),
                             [](const Selected& a, const Selected& b) { return a.distance < b.distance; });
            S.selected.resize(MAX_INSTANCES);
        }
        std::sort(S.selected.begin(), S.selected.end(), [](const Selected& a, const Selected& b) { return a.distance < b.distance; });
        changed = true;
    }

    // The frame's bottom level builds: the missing structures of the nearest records, within the budget.
    // Sizes only (and the scratch they need); nothing is made yet.
    void plan_builds(U32 frame, VkDeviceSize& scratch_needed)
    {
        S.pending.clear();
        S.pending_keys.clear();
        scratch_needed = 0;
        U64 triangles = 0;
        bool waiting = false;
        for (U32 index = 0; index < (U32)S.selected.size(); ++index)
        {
            const Selected& s = S.selected[index];
            const auto it = S.blases.find(s.key);
            if (it != S.blases.end())
            {
                it->second.last_used = frame;
                continue;
            }
            if (S.pending.size() >= MAX_BUILDS_A_FRAME || (triangles >= BUILD_BUDGET_TRIANGLES && !S.pending.empty()) ||
                S.blas_bytes >= BLAS_MEMORY_BUDGET)
            {
                if (S.blas_bytes >= BLAS_MEMORY_BUDGET && !S.budget_logged)
                {
                    S.budget_logged = true;
                    LL_INFOS("VKRt") << "ray tracing: the bottom level structures reached " << (BLAS_MEMORY_BUDGET >> 20)
                                     << " MB; the farther records stay out" << LL_ENDL;
                }
                waiting = true;
                continue;
            }
            if (S.pending_keys.count(s.key))
            {
                continue;   // another record of this frame names the same triangles: one structure
            }
            // The look at the scene may be some frames old (more records waiting): the place must be what it was.
            {
                const LLVKScene::RayRecord& r = S.records[s.record];
                LLVKAttribPool::NumberPlace now;
                if (!LLVKAttribPool::numberPlace(r.number, now) || now.serial != r.serial || now.writes != r.writes || now.chunk != r.chunk ||
                    now.vertex_base != r.vertex_base || now.vertices != r.vertices || r.first_index < now.index_base ||
                    (U64)r.first_index + r.index_count > (U64)now.index_base + now.indices)
                {
                    continue;
                }
            }
            Pending p;
            if (!describe(S.records[s.record], p.geometry, p.range))
            {
                // Not valid now (the pool's place changed since): the next look finds it again.
                continue;
            }
            VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = BLAS_FLAGS;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &p.geometry;     // only for the size query
            VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            S.fnSizes(S.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &p.range.primitiveCount, &sizes);
            p.scratch_size = align_up(sizes.buildScratchSize, S.scratch_align);
            p.build_bytes = align_up(sizes.accelerationStructureSize, 256);
            if (p.scratch_size > SCRATCH_CAP)
            {
                // Too large to ever build: remembered as failed so it is not asked again.
                Blas failed;
                failed.id = S.next_id++;
                failed.failed = true;
                failed.last_used = frame;
                S.blases.emplace(s.key, failed);
                ++S.stat_failed;
                continue;
            }
            if (scratch_needed + p.scratch_size > SCRATCH_CAP && !S.pending.empty())
            {
                waiting = true;
                continue;
            }
            p.scratch_offset = scratch_needed;
            scratch_needed += p.scratch_size;
            p.selected = index;
            S.pending_keys.insert(s.key);
            p.compact = !S.free_queries.empty();   // a query slot is taken when the structure is made
            triangles += p.range.primitiveCount;
            S.pending.push_back(p);
        }
        S.more_pending = waiting;
    }

    // The compacted size of every structure whose frame the GPU has finished: a smaller copy, recorded later.
    void plan_compactions()
    {
        S.compactions.clear();
        bool left = false;
        for (auto& kv : S.blases)
        {
            Blas& b = kv.second;
            if (b.stage != STAGE_READ)
            {
                continue;
            }
            if (S.compactions.size() >= COMPACTS_A_FRAME)
            {
                left = true;
                break;
            }
            U64 size = 0;
            const VkResult r = vkGetQueryPoolResults(S.device, S.query_pool, b.query, 1, sizeof(size), &size, sizeof(U64), VK_QUERY_RESULT_64_BIT);
            if (r == VK_NOT_READY && ++b.tries < COMPACT_TRIES)
            {
                left = true;
                continue;
            }
            const U32 slot = b.query;
            b.query = NO_QUERY;
            b.stage = STAGE_DONE;
            if (slot != NO_QUERY)
            {
                S.free_queries.push_back(slot);
            }
            size = align_up(size, 256);
            if (r != VK_SUCCESS || size == 0 || size + (size >> 2) >= b.bytes)
            {
                continue;   // not smaller by a fifth: not worth a copy
            }
            Compaction c;
            c.from = b.as;
            c.from_storage = b.storage;
            Buf storage;
            VkAccelerationStructureKHR to = VK_NULL_HANDLE;
            VkDeviceAddress address = 0;
            if (!make_structure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, size, to, storage, address))
            {
                continue;
            }
            c.to = to;
            // The new one takes the old one's place now (the top level structure of this frame is built after the copy);
            // the old one lives on for the frames in flight.
            S.blas_bytes -= b.bytes;
            b.storage = storage;
            b.as = to;
            b.address = address;
            b.bytes = size;
            S.blas_bytes += size;
            S.compactions.push_back(c);
        }
        S.compaction_due = left;
    }

    // The old structures go (handle, then buffer) once the frames in flight are done with them.
    void retire_compacted(std::vector<Compaction>& done)
    {
        for (Compaction& c : done)
        {
            retire_structure(false, c.from, c.from_storage);
        }
    }

    U64 hash_instances()
    {
        U64 h = LLRHI::FNV1A_BASIS_SHORT ^ S.instances.size();
        for (const VkAccelerationStructureInstanceKHR& i : S.instances)
        {
            const U64 words[] = { i.accelerationStructureReference, (U64)i.instanceCustomIndex };
            h = LLRHI::fnv1aWords(h, words, 2);
            h = LLRHI::fnv1aWords(h, (const U32*)&i.transform, 12);
        }
        return h;
    }

    void fill_instances()
    {
        S.instances.clear();
        for (const Selected& s : S.selected)
        {
            const auto it = S.blases.find(s.key);
            if (it == S.blases.end() || it->second.failed || !it->second.as)
            {
                continue;
            }
            const LLVKScene::RayRecord& r = S.records[s.record];
            bool finite = true;
            for (U32 k = 0; k < 16; ++k)
            {
                finite = finite && std::isfinite(r.matrix[k]);
            }
            if (!finite)
            {
                continue;
            }
            VkAccelerationStructureInstanceKHR inst = {};
            for (U32 row = 0; row < 3; ++row)
            {
                for (U32 col = 0; col < 4; ++col)
                {
                    inst.transform.matrix[row][col] = r.matrix[col * 4 + row];
                }
            }
            inst.instanceCustomIndex = r.id;
            inst.mask = 0xFF;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst.accelerationStructureReference = it->second.address;
            S.instances.push_back(inst);
        }
    }

    // ==== the self test (LLRHI_RT_SELFTEST=1) ====
    const char* SELFTEST_GLSL = R"GLSL(#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x = 8) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) buffer Io
{
    vec4 rays[16];      // per lane: origin (w: 0, a lane that shoots nothing), direction (w: the farthest)
    uvec4 hits[8];      // per lane: hit (1/0), instance custom index, primitive, t
} io;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= 8u)
    {
        return;
    }
    vec4 origin = io.rays[2u * i];
    vec4 dir = io.rays[2u * i + 1u];
    if (origin.w == 0.0)
    {
        io.hits[i] = uvec4(0xffffffffu);
        return;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin.xyz, 0.001, dir.xyz, dir.w);
    while (rayQueryProceedEXT(rq))
    {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
    {
        io.hits[i] = uvec4(1u, uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                           uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)),
                           floatBitsToUint(rayQueryGetIntersectionTEXT(rq, true)));
    }
    else
    {
        io.hits[i] = uvec4(0u);
    }
}
)GLSL";

    bool selftest_pipeline()
    {
        if (S.selftest_pipeline)
        {
            return true;
        }
        if (S.selftest_failed)
        {
            return false;
        }
        S.selftest_failed = true;
        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, SELFTEST_GLSL } }, S.selftest_shader) || S.selftest_shader.modules.empty())
        {
            LL_WARNS("VKRt") << "ray tracing self test: the shader failed: " << S.selftest_shader.log << LL_ENDL;
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 2;
        set_info.pBindings = bindings;
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        if (vkCreateDescriptorSetLayout(S.device, &set_info, nullptr, &S.selftest_set_layout) != VK_SUCCESS)
        {
            return false;
        }
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &S.selftest_set_layout;
        if (vkCreatePipelineLayout(S.device, &layout_info, nullptr, &S.selftest_layout) != VK_SUCCESS)
        {
            return false;
        }
        pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipe.stage.module = S.selftest_shader.modules[0];
        pipe.stage.pName = "main";
        pipe.layout = S.selftest_layout;
        if (vkCreateComputePipelines(S.device, LLVKDevice::instance().usePipelineCache(), 1, &pipe, nullptr, &S.selftest_pipeline) != VK_SUCCESS)
        {
            LL_WARNS("VKRt") << "ray tracing self test: the pipeline could not be created" << LL_ENDL;
            return false;
        }
        S.selftest_failed = false;
        return true;
    }

    void selftest_destroy()
    {
        if (S.selftest_pipeline)
        {
            vkDestroyPipeline(S.device, S.selftest_pipeline, nullptr);
            S.selftest_pipeline = VK_NULL_HANDLE;
        }
        if (S.selftest_layout)
        {
            vkDestroyPipelineLayout(S.device, S.selftest_layout, nullptr);
            S.selftest_layout = VK_NULL_HANDLE;
        }
        if (S.selftest_set_layout)
        {
            vkDestroyDescriptorSetLayout(S.device, S.selftest_set_layout, nullptr);
            S.selftest_set_layout = VK_NULL_HANDLE;
        }
        LLVKShader::destroy(S.selftest_shader);
    }

    // Rays into the new top level structure: at the centre of the nearest records from the camera (they should hit),
    // and one that leaves the world straight up (it must miss). The answer is logged when the frame has finished.
    void selftest_record(VkCommandBuffer cmd, const LLRHI::RayTracingWanted& w)
    {
        if (!S.selftest_wanted || S.selftest_runs >= SELFTEST_RUNS || !S.tlas || S.instances.empty() || !selftest_pipeline())
        {
            return;
        }
        ++S.selftest_runs;
        std::shared_ptr<SelfRun> run = std::make_shared<SelfRun>();
        run->device = S.device;
        if (!make_buffer(512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true, run->io))
        {
            return;
        }
        F32* rays = (F32*)run->io.mapped;
        U32* hits = (U32*)((U8*)run->io.mapped + 256);
        memset(run->io.mapped, 0, 512);
        U32 lane = 0;
        for (const Selected& s : S.selected)
        {
            if (lane >= 4)
            {
                break;
            }
            const auto aimed = S.blases.find(s.key);
            if (aimed == S.blases.end() || aimed->second.failed || !aimed->second.as)
            {
                continue;
            }
            const LLVKScene::RayRecord& r = S.records[s.record];
            bool finite = true;
            for (U32 k = 0; k < 16; ++k)
            {
                finite = finite && std::isfinite(r.matrix[k]);
            }
            if (!finite)
            {
                continue;
            }
            const F32 c[3] = { 0.5f * (r.lo[0] + r.hi[0]), 0.5f * (r.lo[1] + r.hi[1]), 0.5f * (r.lo[2] + r.hi[2]) };
            F32 to[3];
            for (U32 i = 0; i < 3; ++i)
            {
                to[i] = r.matrix[i] * c[0] + r.matrix[4 + i] * c[1] + r.matrix[8 + i] * c[2] + r.matrix[12 + i] - w.camera[i];
            }
            const F32 length = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
            if (length < 0.01f)
            {
                continue;
            }
            F32* ray = rays + lane * 8;
            ray[0] = w.camera[0];
            ray[1] = w.camera[1];
            ray[2] = w.camera[2];
            ray[3] = 1.f;
            ray[4] = to[0] / length;
            ray[5] = to[1] / length;
            ray[6] = to[2] / length;
            ray[7] = length * 2.f + 1.f;
            ++lane;
        }
        run->targeted = lane;
        run->instances = (U32)S.instances.size();
        F32* up = rays + 7 * 8;     // the last lane leaves the world
        up[0] = w.camera[0];
        up[1] = w.camera[1];
        up[2] = w.camera[2] + 100000.f;
        up[3] = 1.f;
        up[6] = 1.f;
        up[7] = 1000.f;
        vmaFlushAllocation(S.allocator, run->io.memory, 0, VK_WHOLE_SIZE);

        VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &S.selftest_set_layout;
        if (vkCreateDescriptorPool(S.device, &pool_info, nullptr, &run->pool) != VK_SUCCESS)
        {
            vmaDestroyBuffer(S.allocator, run->io.buffer, run->io.memory);
            return;
        }
        alloc.descriptorPool = run->pool;
        if (vkAllocateDescriptorSets(S.device, &alloc, &set) != VK_SUCCESS)
        {
            vkDestroyDescriptorPool(S.device, run->pool, nullptr);
            vmaDestroyBuffer(S.allocator, run->io.buffer, run->io.memory);
            return;
        }
        VkWriteDescriptorSetAccelerationStructureKHR as_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &S.tlas;
        VkDescriptorBufferInfo io_info = { run->io.buffer, 0, 512 };
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &as_write;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &io_info;
        vkUpdateDescriptorSets(S.device, 2, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.selftest_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.selftest_layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_HOST_READ_BIT);

        const VmaAllocator allocator = S.allocator;
        LLVKMirror::instance().defer(
            [run, allocator, hits]
            {
                vmaInvalidateAllocation(allocator, run->io.memory, 0, VK_WHOLE_SIZE);
                U32 hit_count = 0;
                std::string seen;
                for (U32 i = 0; i < run->targeted; ++i)
                {
                    if (hits[i * 4] == 1)
                    {
                        ++hit_count;
                        F32 t;
                        memcpy(&t, &hits[i * 4 + 3], sizeof(t));
                        seen += " [record " + std::to_string(hits[i * 4 + 1]) + " t=" + std::to_string(t) + "]";
                    }
                }
                const bool miss_ok = hits[7 * 4] == 0;
                const bool pass = miss_ok && (run->targeted == 0 || hit_count > 0);
                LL_INFOS("VKRt") << "ray tracing self test " << (pass ? "PASS" : "FAIL") << ": " << hit_count << " of " << run->targeted
                                 << " rays at the nearest records hit" << seen << "; the ray out of the world "
                                 << (miss_ok ? "missed" : "HIT (wrong)") << " (" << run->instances << " instances)" << LL_ENDL;
                vkDestroyDescriptorPool(run->device, run->pool, nullptr);
                vmaDestroyBuffer(allocator, run->io.buffer, run->io.memory);
            });
    }
}

void LLVKRtAs::frame(VkCommandBuffer cmd)
{
    if (!LLRHI::sRayTracingSource && !S.active)
    {
        return;     // never asked for
    }
    LLRHI::RayTracingWanted w;
    const bool on = LLRHI::sRayTracingSource && LLRHI::sRayTracing && LLVKDevice::instance().hasRayTracing() &&
                    LLRHI::sRayTracingSource(w) && w.on;
    if (!on && !S.active)
    {
        S.suspended = false;    // turned off: the next time it is turned on may try again
        return;     // off, and nothing built: one test and out
    }
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    if (!on)
    {
        release(false, "turned off");
        return;
    }
    if (!cmd || S.suspended || !load())
    {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    if (!S.active)
    {
        S.active = true;
        S.ever_on = true;
        S.tlas_frame = LLVKTracker::frame();
        LL_INFOS("VKRt") << "ray tracing on: range " << w.range << " m" << (S.selftest_wanted ? ", self test" : "") << LL_ENDL;
    }
    const U32 frame = LLVKTracker::frame();
    w.range = llclamp(w.range, 8.f, 512.f);
    if (!S.query_pool)
    {
        VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        info.queryCount = QUERY_SLOTS;
        if (vkCreateQueryPool(S.device, &info, nullptr, &S.query_pool) == VK_SUCCESS)
        {
            S.free_queries.clear();
            for (U32 i = QUERY_SLOTS; i > 0; --i)
            {
                S.free_queries.push_back(i - 1);
            }
        }
        VkQueryPoolCreateInfo ts = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        ts.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ts.queryCount = TS_PAIRS * 2;
        if (S.timestamps && vkCreateQueryPool(S.device, &ts, nullptr, &S.ts_pool) != VK_SUCCESS)
        {
            S.ts_pool = VK_NULL_HANDLE;
        }
    }
    take_done();

    bool changed = false;
    collect(w, frame, changed);

    // Structures nothing near names any more (what the selection still names is used).
    if ((frame & 31) == 0)
    {
        for (const Selected& s : S.selected)
        {
            const auto named = S.blases.find(s.key);
            if (named != S.blases.end())
            {
                named->second.last_used = frame;
            }
        }
        for (auto it = S.blases.begin(); it != S.blases.end();)
        {
            if (frame - it->second.last_used > UNUSED_FRAMES)
            {
                Blas& b = it->second;
                if (b.query != NO_QUERY)
                {
                    S.free_queries.push_back(b.query);
                }
                S.blas_bytes -= b.bytes;
                retire_structure(false, b.as, b.storage);
                it = S.blases.erase(it);
                ++S.stat_freed;
                S.tlas_dirty = true;    // the top level structure is looked at again (it must not name a freed one)
            }
            else
            {
                ++it;
            }
        }
    }

    VkDeviceSize scratch_needed = 0;
    if (changed || S.more_pending)
    {
        plan_builds(frame, scratch_needed);
    }
    else
    {
        S.pending.clear();
    }
    if (S.query_pool && S.compaction_due)
    {
        plan_compactions();
    }
    else
    {
        S.compactions.clear();
    }
    // Either of those changes the instances.
    changed = changed || !S.pending.empty() || !S.compactions.empty() || S.tlas_dirty;

    // The top level structure's size is asked for with the most instances it can have now.
    const bool may_build_tlas = changed;
    VkDeviceSize tlas_scratch = 0;
    if (may_build_tlas && !S.selected.empty())
    {
        VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
        VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        const U32 upper = (U32)S.selected.size();
        VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        S.fnSizes(S.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &upper, &sizes);
        tlas_scratch = align_up(sizes.buildScratchSize, S.scratch_align);
    }
    if (!ensure_scratch(llmax(scratch_needed, tlas_scratch)))
    {
        if (scratch_needed || tlas_scratch)
        {
            LL_WARNS("VKRt") << "ray tracing: scratch memory of " << (llmax(scratch_needed, tlas_scratch) >> 20)
                             << " MB could not be allocated; ray tracing stops until it is turned off and on" << LL_ENDL;
            retire_compacted(S.compactions);
            S.suspended = true;
            release(false, "stopped (out of memory)");
            S.suspended = true;
            return;
        }
    }

    const bool any_work = !S.pending.empty() || !S.compactions.empty() || may_build_tlas;
    if (!any_work)
    {
        S.stat_record_ms += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
        return;
    }

    // ---- the bottom level structures of the batch are made (their addresses are needed by the top level one) ----
    S.notify_ids.clear();
    S.infos.clear();
    S.range_ptrs.clear();
    std::vector<U32> reset_slots;
    for (size_t i = 0; i < S.pending.size(); ++i)
    {
        Pending& p = S.pending[i];
        const Selected& sel = S.selected[p.selected];
        Blas blas;
        blas.id = S.next_id++;
        blas.last_used = frame;
        blas.triangles = p.range.primitiveCount;
        blas.bytes = p.build_bytes;
        if (!make_structure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, p.build_bytes, blas.as, blas.storage, blas.address))
        {
            LL_WARNS("VKRt") << "ray tracing: a bottom level structure of " << (p.build_bytes >> 10)
                             << " KB could not be made; ray tracing stops until it is turned off and on" << LL_ENDL;
            // Nothing recorded yet: the ones made so far in this loop are in the map and unbuilt, so all of it goes.
            retire_compacted(S.compactions);
            S.suspended = true;
            release(false, "stopped (out of memory)");
            S.suspended = true;
            return;
        }
        if (p.compact && !S.free_queries.empty())
        {
            blas.query = S.free_queries.back();
            S.free_queries.pop_back();
            blas.stage = STAGE_WAIT_GPU;
            reset_slots.push_back(blas.query);
            S.notify_ids.push_back(blas.id);
        }
        else
        {
            p.compact = false;
        }
        S.blas_bytes += blas.bytes;
        const auto made = S.blases.emplace(sel.key, blas);
        p.blas = &made.first->second;
        ++S.stat_built;
    }
    for (Pending& p : S.pending)
    {
        VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = BLAS_FLAGS;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = p.blas->as;
        info.geometryCount = 1;
        info.pGeometries = &p.geometry;     // S.pending is not touched until the frame's commands are recorded
        info.scratchData.deviceAddress = scratch_at(p.scratch_offset);
        S.infos.push_back(info);
    }
    for (Pending& p : S.pending)
    {
        S.range_ptrs.push_back(&p.range);
    }

    // ---- the top level structure's instances (after compaction, so the new addresses) ----
    bool build_tlas = false;
    VkAccelerationStructureKHR new_tlas = VK_NULL_HANDLE;
    Buf new_storage, new_instances;
    VkDeviceSize tlas_bytes = 0;
    if (may_build_tlas)
    {
        fill_instances();
        const U64 hash = hash_instances();
        // A compacted structure replaced its old one, which goes once this frame is done: the top level
        // structure must name the new one now, not after the interval.
        const bool throttled = S.tlas && S.compactions.empty() && frame - S.tlas_frame < TLAS_INTERVAL;
        if (S.instances.empty())
        {
            if (S.tlas)
            {
                retire_structure(false, S.tlas, S.tlas_storage);
                retire_buffer(false, S.instance_buffer);
                S.tlas_instances = 0;
                S.tlas_hash = 0;
            }
            S.tlas_dirty = false;
        }
        else if (hash == S.tlas_hash && S.tlas)
        {
            S.tlas_dirty = false;
        }
        else if (throttled)
        {
            S.tlas_dirty = true;    // later
        }
        else
        {
            const U32 count = (U32)S.instances.size();
            const VkDeviceSize bytes = (VkDeviceSize)count * sizeof(VkAccelerationStructureInstanceKHR);
            VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            geometry.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
            VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geometry;
            VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            S.fnSizes(S.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
            tlas_bytes = align_up(sizes.accelerationStructureSize, 256);
            VkDeviceAddress tlas_address = 0;
            if (align_up(sizes.buildScratchSize, S.scratch_align) <= S.scratch.size - S.scratch_align && S.scratch.size > S.scratch_align &&
                make_buffer(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, false, new_instances) &&
                make_structure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlas_bytes, new_tlas, new_storage, tlas_address))
            {
                memcpy(new_instances.mapped, S.instances.data(), bytes);
                vmaFlushAllocation(S.allocator, new_instances.memory, 0, VK_WHOLE_SIZE);
                build_tlas = true;
                S.tlas_hash = hash;
                S.tlas_dirty = false;
            }
            else
            {
                retire_buffer(false, new_instances);
                retire_structure(false, new_tlas, new_storage);
                S.tlas_dirty = true;    // tried again; the old one stays
                {
                    // It may name structures that go with this frame or were freed: no top level structure until a new one is built.
                    retire_structure(false, S.tlas, S.tlas_storage);
                    retire_buffer(false, S.instance_buffer);
                    S.tlas_instances = 0;
                    S.tlas_hash = 0;
                }
            }
        }
    }

    if (S.pending.empty() && S.compactions.empty() && !build_tlas)
    {
        S.stat_record_ms += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
        return;     // nothing to record (the top level structure did not change)
    }

    // ---- recording ----
    const bool timed = S.ts_pool != VK_NULL_HANDLE && (!S.pending.empty() || build_tlas);
    U32 ts_slot = 0;
    if (timed)
    {
        ts_slot = S.ts_head++ % TS_PAIRS;
        vkCmdResetQueryPool(cmd, S.ts_pool, ts_slot * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.ts_pool, ts_slot * 2);
    }
    // Earlier copies into the pool's buffers (the frame's uploads), and earlier frames' builds and scratch.
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                   VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                       VK_ACCESS_2_SHADER_READ_BIT);   // the build's geometry input is read as shader reads
    for (U32 slot : reset_slots)
    {
        vkCmdResetQueryPool(cmd, S.query_pool, slot, 1);
    }
    if (!S.infos.empty())
    {
        S.fnBuild(cmd, (U32)S.infos.size(), S.infos.data(), S.range_ptrs.data());
    }
    for (const Compaction& c : S.compactions)
    {
        VkCopyAccelerationStructureInfoKHR copy = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
        copy.src = c.from;
        copy.dst = c.to;
        copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        S.fnCopy(cmd, &copy);
        ++S.stat_compacted;
    }
    retire_compacted(S.compactions);
    // Builds and copies done before the sizes are asked for and before the top level one reads them
    // (and the scratch is used again).
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    for (const Pending& p : S.pending)
    {
        if (p.compact)
        {
            S.fnWriteProps(cmd, 1, &p.blas->as, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, S.query_pool, p.blas->query);
        }
    }
    if (build_tlas)
    {
        VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
        geometry.geometry.instances.data.deviceAddress = new_instances.address;
        VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.dstAccelerationStructure = new_tlas;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        info.scratchData.deviceAddress = scratch_at(0);
        VkAccelerationStructureBuildRangeInfoKHR range = {};
        range.primitiveCount = (U32)S.instances.size();
        const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
        S.fnBuild(cmd, 1, &info, &range_ptr);
        // The frame's draws and compute passes may read it (ray queries).
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        // The old one is read by frames in flight: freed once they are done.
        retire_structure(false, S.tlas, S.tlas_storage);
        retire_buffer(false, S.instance_buffer);
        S.tlas = new_tlas;
        S.tlas_storage = new_storage;
        S.instance_buffer = new_instances;
        S.tlas_instances = (U32)S.instances.size();
        S.tlas_frame = frame;
        ++S.stat_tlas_builds;
        selftest_record(cmd, w);
    }
    else if (!S.pending.empty())
    {
        // Builds with no top level one: their writes still have to be done before the next frame's uses.
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }
    if (timed)
    {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.ts_pool, ts_slot * 2 + 1);
    }
    // Told when the GPU has finished this frame: the compacted sizes can be read, the build time too.
    if (!S.notify_ids.empty() || timed)
    {
        const U32 flight = S.flight_head++ % TS_PAIRS;
        U32 generation = 0;
        {
            std::lock_guard<std::mutex> lock(S.done_mutex);
            State::Flight& f = S.flights[flight];
            f.ids.assign(S.notify_ids.begin(), S.notify_ids.end());
            f.ts_slot = ts_slot;
            f.timed = timed;
            generation = S.generation;
        }
        LLVKMirror::instance().defer(
            [flight, generation]
            {
                std::lock_guard<std::mutex> lock(S.done_mutex);
                if (generation != S.generation)
                {
                    return;     // released since: its structures and query pools are gone
                }
                const State::Flight& f = S.flights[flight];
                S.done_ids.insert(S.done_ids.end(), f.ids.begin(), f.ids.end());
                if (f.timed)
                {
                    S.done_slots.push_back(f.ts_slot);
                }
            });
    }
    S.stat_record_ms += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void LLVKRtAs::shutdown()
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    if (!S.loaded)
    {
        return;
    }
    release(true, "shut down");
    selftest_destroy();
    S.loaded = false;
    S.tried = false;
}

std::string LLVKRtAs::takeStats(U32 frames)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    if (!S.ever_on)
    {
        return std::string();
    }
    std::ostringstream out;
    if (!S.active)
    {
        out << "RT: off";
    }
    else
    {
        out << "RT: " << S.blases.size() << " BLAS (+" << S.stat_built << " built, " << S.stat_compacted << " compacted, " << S.stat_freed
            << " freed, " << S.stat_failed << " refused), TLAS " << S.tlas_instances << " instances (" << S.stat_tlas_builds
            << " builds), AS " << (as_memory() >> 20) << " MB";
        if (S.stat_gpu_builds)
        {
            out << ", build " << S.stat_gpu_ms / S.stat_gpu_builds << " ms GPU (" << S.stat_gpu_builds << " frames)";
        }
        out << ", record " << S.stat_record_ms / llmax(frames, 1u) << " ms/frame";
        if (S.more_pending)
        {
            out << ", more waiting";
        }
    }
    S.stat_built = S.stat_compacted = S.stat_freed = S.stat_tlas_builds = S.stat_failed = 0;
    S.stat_record_ms = S.stat_gpu_ms = 0.0;
    S.stat_gpu_builds = 0;
    return out.str();
}
