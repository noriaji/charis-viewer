/**
 * @file llvkgpulist.cpp
 * @brief GPU-driven drawing: the shadow casters' list on the GPU, culled by a compute shader (research/46 G2, G3).
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

#include "llvkgpulist.h"

#include "llglheaders.h"
#include "llrhihash.h"
#include "llvkattribpool.h"
#include "llvkbindless.h"
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvkshader.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using LLVKGPUList::BUCKETS;

    constexpr U32 MAX_ROWS = 1u << 18;      // 12 MB table
    constexpr U32 MAX_GROUPS = 1u << 16;    // 2 MB table
    constexpr U32 MAX_MODELS = 64;          // region matrices in use at once
    constexpr U32 LOCAL_SIZE = 64;          // compute shader threads per group
    // A freed slot is handed out again this many frames later.
    constexpr U32 RELEASE_DELAY = 3;
    static_assert(RELEASE_DELAY == LLVKMirror::FRAMES_IN_FLIGHT + 1, "RELEASE_DELAY: the frames in flight and one more");
    constexpr VkDeviceSize SCRATCH_BYTES = 16ull << 20;     // per scratch buffer (more are made when a frame needs them)
    constexpr U32 SETS_PER_POOL = 64;
    constexpr U32 BINDINGS = 6;
    constexpr U32 COUNT_WORDS = BUCKETS;    // the output starts with a count per bucket

    // Row flags (GpuRow::flags above the bucket number).
    constexpr U32 ROW_POOL = 0x100;         // drawn with the face pools (terrain)
    constexpr U32 ROW_LOOSE = 0x200;        // bounds may miss vertices: no per-row test
    constexpr U32 DEAD_BUCKET = 0xff;       // a dead row's flags: no bucket (>= BUCKETS), never drawn
    static_assert(BUCKETS < DEAD_BUCKET, "the dead row marker is no bucket");
    // Pass flags (ViewBlock::params[1]).
    constexpr U32 FLAG_VOLUME = 1;
    constexpr U32 FLAG_TERRAIN = 2;
    constexpr U32 FLAG_POOLS = 4;

    // LLRHI_GPU_LIST_BATCH (research/98): the passes' dispatches recorded
    // together between one pair of barriers (default); 0: each one at once.
    const bool sBatchDispatches = LLRHI::envFirstChar("LLRHI_GPU_LIST_BATCH") != '0';

    // std430 layouts of the shader's tables.
    struct GpuRow
    {
        F32 lo[3];
        U32 group;          // group slot (only read when its resident bit is set)
        F32 hi[3];
        U32 model;          // model slot
        U32 index_count;
        U32 first_index;    // in the pool buffer, in its index size
        S32 vertex_offset;
        U32 flags;          // bucket | ROW_*
    };
    static_assert(sizeof(GpuRow) == 48, "matches Row in the shader");

    struct GpuGroup
    {
        F32 center[3];
        U32 model;
        F32 radius[3];
        U32 since;
    };
    static_assert(sizeof(GpuGroup) == 32, "matches Group in the shader");

    struct ViewBlock
    {
        F32 planes[8][4];
        F32 scalers[8][4];      // w: 1 when the plane is tested
        F32 sides[4][4];
        F32 origin[4];
        U32 params[4];          // rows to test, flags, since min, since max
        U32 params2[4];         // first draw matrix row, models, groups (resident bits), 0
        U32 buckets[BUCKETS][4];    // first command, capacity
        F32 models[MAX_MODELS][16];
    };
    static_assert(sizeof(ViewBlock) == 880 + MAX_MODELS * 64, "matches ViewBlock in the shader");

    const char* const CULL_SHADER =
        "#version 450\n"
        "layout(local_size_x = 64) in;\n"
        "struct Row { vec4 lo; vec4 hi; uvec4 draw; };\n"
        "struct Group { vec4 center; vec4 radius; };\n"
        "layout(std430, set = 0, binding = 0) readonly buffer RowTable { Row rows[]; };\n"
        "layout(std430, set = 0, binding = 1) readonly buffer GroupTable { Group groups[]; };\n"
        "layout(std430, set = 0, binding = 2) readonly buffer ViewBlock\n"
        "{\n"
        "    vec4 planes[8];\n"
        "    vec4 scalers[8];\n"
        "    vec4 sides[4];\n"
        "    vec4 origin;\n"
        "    uvec4 params;\n"
        "    uvec4 params2;\n"
        "    uvec4 buckets[32];\n"
        "    mat4 models[64];\n"
        "} view;\n"
        "layout(std430, set = 0, binding = 3) readonly buffer AcceptBits { uint accept[]; };\n"
        "layout(std430, set = 0, binding = 4) readonly buffer ResidentBits { uint resident[]; };\n"
        "layout(std430, set = 0, binding = 5) buffer Output { uint out_data[]; };\n"
        "const uint FLAG_VOLUME = 1u;\n"
        "const uint FLAG_TERRAIN = 2u;\n"
        "const uint FLAG_POOLS = 4u;\n"
        "const uint ROW_POOL = 0x100u;\n"
        "const uint ROW_LOOSE = 0x200u;\n"
        "const uint BUCKET_COUNT = 32u;\n"
        // An axis-aligned box's half size after the matrix (exact for the
        // region matrices, which only translate).
        "vec3 box_radius(mat4 m, vec3 r)\n"
        "{\n"
        "    return abs(m[0].xyz) * r.x + abs(m[1].xyz) * r.y + abs(m[2].xyz) * r.z;\n"
        "}\n"
        // LLCamera::AABBInFrustum on the group's object bounds, as the CPU cull
        // of a shadow camera tests them (LLOctreeCullShadow): outside when the
        // corner the plane's mask picks is in front of the plane. The slack
        // keeps groups the float error could put either way (they only gain).
        "bool group_visible(uint g)\n"
        "{\n"
        "    Group gr = groups[g];\n"
        "    mat4 m = view.models[floatBitsToUint(gr.center.w)];\n"
        "    vec3 c = (m * vec4(gr.center.xyz, 1.0)).xyz;\n"
        "    vec3 r = box_radius(m, gr.radius.xyz);\n"
        "    for (uint i = 0u; i < 8u; ++i)\n"
        "    {\n"
        "        vec4 s = view.scalers[i];\n"
        "        if (s.w == 0.0)\n"
        "        {\n"
        "            continue;\n"
        "        }\n"
        "        vec4 p = view.planes[i];\n"
        "        vec3 minp = c - r * s.xyz;\n"
        "        float dist = dot(p.xyz, minp) + p.w;\n"
        "        float slack = 1e-5 * (abs(p.w) + dot(abs(p.xyz), abs(minp))) + 1e-3;\n"
        "        if (dist > slack)\n"
        "        {\n"
        "            return false;\n"
        "        }\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        // research/44 P1: a batch entirely outside one side plane, by more than
        // the planes' float error, draws no pixel.
        "bool row_outside(Row row, mat4 m)\n"
        "{\n"
        "    vec3 c0 = 0.5 * (row.lo.xyz + row.hi.xyz);\n"
        "    vec3 r0 = 0.5 * (row.hi.xyz - row.lo.xyz);\n"
        "    vec3 c = (m * vec4(c0, 1.0)).xyz;\n"
        "    vec3 r = box_radius(m, r0);\n"
        "    vec3 reach = abs(c - view.origin.xyz) + abs(r);\n"
        "    float margin = 0.01 + 0.001 * (reach.x + reach.y + reach.z);\n"
        "    for (uint i = 0u; i < 4u; ++i)\n"
        "    {\n"
        "        vec4 p = view.sides[i];\n"
        "        float nearest = dot(p.xyz, c) + p.w - dot(abs(p.xyz), abs(r));\n"
        "        if (nearest > margin)\n"
        "        {\n"
        "            return true;\n"
        "        }\n"
        "    }\n"
        "    return false;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    uint i = gl_GlobalInvocationID.x;\n"
        "    if (i >= view.params.x)\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    Row row = rows[i];\n"
        "    uint flags = row.draw.w;\n"
        "    uint b = flags & 0xffu;\n"
        // A dead row (killRow's tombstone), or anything else without a bucket.
        "    if (b >= BUCKET_COUNT)\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    bool draw = (accept[i >> 5u] & (1u << (i & 31u))) != 0u;\n"
        "    if (!draw)\n"
        "    {\n"
        "        uint g = floatBitsToUint(row.lo.w);\n"
        "        bool pool_row = (flags & ROW_POOL) != 0u;\n"
        "        bool kind = pool_row ? ((view.params.y & FLAG_TERRAIN) != 0u && (view.params.y & FLAG_POOLS) != 0u)\n"
        "                             : (view.params.y & FLAG_VOLUME) != 0u;\n"
        "        if (kind && g < view.params2.z && (resident[g >> 5u] & (1u << (g & 31u))) != 0u)\n"
        "        {\n"
        "            uint since = floatBitsToUint(groups[g].radius.w);\n"
        "            draw = (pool_row || (since >= view.params.z && since <= view.params.w)) && group_visible(g);\n"
        "        }\n"
        "    }\n"
        "    if (!draw)\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    uint model = floatBitsToUint(row.hi.w);\n"
        "    mat4 m = view.models[model];\n"
        "    if ((flags & ROW_LOOSE) == 0u && row_outside(row, m))\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    uint n = atomicAdd(out_data[b], 1u);\n"
        "    if (n >= view.buckets[b].y)\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    uint at = BUCKET_COUNT + (view.buckets[b].x + n) * 5u;\n"
        "    out_data[at] = row.draw.x;\n"
        "    out_data[at + 1u] = 1u;\n"
        "    out_data[at + 2u] = row.draw.y;\n"
        "    out_data[at + 3u] = row.draw.z;\n"
        "    out_data[at + 4u] = view.params2.x + model;\n"
        "}\n";

    // out = a * b, column-major 4x4 (the same expression as llvknative's, so the
    // MVP is the same float for float as a packet's).
    void mul4(const F32* a, const F32* b, F32* out)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
            }
        }
    }

    struct RowState
    {
        U32 gen = 0;
        bool live = false;
        U32 group = 0;          // group slot
        U32 bucket = 0;
        U32 model = 0;
        U32 vertex_buffer = 0;
        U32 index_buffer = 0;
    };

    struct GroupState
    {
        bool used = false;
        bool resident = false;  // granted, and none of its rows dropped since (its resident bit)
        // (rows dropped since takeDamaged: List::mDamaged, research/118 10)
        U32 model = U32_MAX;    // model slot held while resident
        std::vector<U32> rows;  // live row slots
    };

    struct Model
    {
        const F32* matrix = nullptr;
        U32 refs = 0;
    };

    struct Release
    {
        U32 slot = 0;
        U32 frame = 0;
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkDeviceSize size = 0;
    };

    // Per frame slot: command output and descriptor sets, reused once the
    // slot's last frame is done (LLVKMirror::beginFrame waited for it).
    struct FrameSlot
    {
        U64 begins = ~0ull;     // LLVKMirror::frameBegins when last used
        std::vector<Buffer> scratch;
        size_t scratch_index = 0;
        VkDeviceSize scratch_head = 0;
        std::vector<VkDescriptorPool> pools;
        size_t pool_index = 0;
    };

    class List final : public LLRHI::ShadowList
    {
    public:
        void lock() override { LLVKTracker::lock().lock(); }
        void unlock() override { LLVKTracker::lock().unlock(); }
        bool ready() override;
        bool broken() const override { return mBroken; }
        U32 addGroup() override;
        void clearGroup(U32 group) override;
        void removeGroup(U32 group) override;
        U64 addRow(U32 group, const Row& row) override;
        bool rowLive(U64 row) override;
        bool takeDamaged(U32 group) override;
        U32 grant(U32 group, const F32 center[3], const F32 radius[3], const F32* model) override;
        void revoke(U32 group) override;
        // research/118 10: takeDamaged, rowLive, serial and rowSpan read
        // atomics kept beside the state the tracker lock guards, so calling
        // them without the lock is free of data races. That is all it is:
        // their answer is the locked one only while nothing else changes the
        // list meanwhile.
        U32 serial() override { return mSerial.load(std::memory_order_acquire); }
        U32 rowSpan() override { return mRowSpan.load(std::memory_order_acquire); }

        bool supported();
        bool prepare(const LLRHI::ShadowListPass& pass, const F32* view_proj, LLVKGPUList::Draws& out);
        void recordPendingDispatches();
        void pinDropped(U32 slot);
        std::string takeStats(U32 frames);

    private:
        bool init();
        bool tables();
        void beginFrame();
        void fail(const char* why);
        void killRow(U32 slot, bool unpin, bool keep_in_group);
        void loseResidency(GroupState& g, U32 slot);
        U32 acquireModel(const F32* matrix);
        void releaseModel(U32 slot);
        bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
        bool scratch(FrameSlot& slot, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset);
        VkDescriptorSet allocateSet(FrameSlot& slot);
        bool flush(VkCommandBuffer cmd);
        void recordDispatches(VkCommandBuffer cmd);

        bool mTried = false;
        bool mReady = false;
        bool mBroken = false;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        LLVKShader::Program mShader;
        Buffer mRowTable;
        Buffer mGroupTable;
        VkDeviceSize mStorageAlign = 16;

        std::vector<RowState> mRows;
        std::vector<GpuRow> mRowData;
        std::vector<U32> mFreeRows;
        std::vector<Release> mRowReleases;
        std::vector<GroupState> mGroups;
        std::vector<GpuGroup> mGroupData;
        std::vector<U32> mFreeGroups;
        std::vector<Release> mGroupReleases;
        std::vector<U32> mResidentBits;
        Model mModels[MAX_MODELS];
        U32 mModelSpan = 0;
        U32 mBucketRows[BUCKETS] = {};
        std::atomic<U32> mSerial{ 0 };
        std::atomic<U32> mRowSpan{ 0 };     // mRows.size()
        // Per group slot: rows dropped since takeDamaged. Per row slot: its
        // handle while live, else 0 (rowLive). Made with the tables, never
        // freed; written under the lock, read without it (research/118 10).
        std::unique_ptr<std::atomic<U8>[]> mDamaged;
        std::unique_ptr<std::atomic<U64>[]> mRowKeys;
        U32 mFrame = U32_MAX;
        FrameSlot mSlots[LLVKMirror::FRAMES_IN_FLIGHT];
        // Table writes not yet in the upload commands (slots).
        std::vector<U32> mPendingRows;
        std::vector<U32> mPendingGroups;
        // Passes' dispatches not yet in the upload commands, of the frame
        // begun as mPendingBegins (LLVKMirror::frameBegins).
        struct PendingDispatch
        {
            VkDescriptorSet set;
            U32 groups;
        };
        std::vector<PendingDispatch> mPendingDispatches;
        U64 mPendingBegins = 0;

        // Since the last takeStats.
        U32 mPasses = 0, mAccepted = 0, mDropped = 0, mAdded = 0, mRefused = 0, mGranted = 0, mRevoked = 0;
        U32 mDispatches = 0, mDispatchBatches = 0;
        U32 mLiveRows = 0, mResident = 0, mUsedGroups = 0;
        bool mUsed = false;
    };

    // Never destroyed, like the pool: GL buffers are still deleted (and the pins
    // dropped) while static objects go away at exit.
    List& sList = *new List();

    void on_pin_dropped(U32 pin)
    {
        sList.pinDropped(pin);
    }

    // LLVKMirror::sBeforeUploadEnd.
    void record_pending_dispatches()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        sList.recordPendingDispatches();
    }

    bool List::supported()
    {
        const LLVKDevice& device = LLVKDevice::instance();
        return device.isReady() && device.hasMultiDrawIndirect() && device.hasDrawIndirectFirstInstance() &&
               device.hasDrawIndirectCount() && device.getLimits().maxDrawIndirectCount >= MAX_ROWS &&
               (U64)device.getLimits().maxComputeWorkGroupCount[0] * LOCAL_SIZE >= MAX_ROWS && LLVKGPUDriven::supported();
    }

    bool List::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = supported() && init();
            if (!mReady)
            {
                LL_WARNS("VKGPUList") << "GPU shadow list unavailable: shadow casters stay packets" << LL_ENDL;
            }
        }
        return mReady && !mBroken;
    }

    bool List::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &gpu, &out.buffer, &out.memory, nullptr) != VK_SUCCESS)
        {
            out = Buffer();
            return false;
        }
        out.size = size;
        return true;
    }

    bool List::init()
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        mStorageAlign = llmax(LLVKDevice::instance().getLimits().minStorageBufferOffsetAlignment, (VkDeviceSize)16);

        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, CULL_SHADER } }, mShader) || mShader.modules.empty())
        {
            LL_WARNS("VKGPUList") << "shadow list cull shader failed: " << mShader.log << LL_ENDL;
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[BINDINGS] = {};
        for (U32 i = 0; i < BINDINGS; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = BINDINGS;
        set_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &mSetLayout;
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &mLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = mShader.modules[0];
        info.stage.pName = "main";
        info.layout = mLayout;
        if (vkCreateComputePipelines(device, LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &mPipeline) != VK_SUCCESS)
        {
            LL_WARNS("VKGPUList") << "shadow list cull pipeline could not be created" << LL_ENDL;
            return false;
        }

        LLVKGPUDriven::setPinDroppedCallback(&on_pin_dropped);
        LLVKMirror::sBeforeUploadEnd = &record_pending_dispatches;
        LL_INFOS("VKGPUList") << "GPU shadow list ready: up to " << MAX_ROWS << " rows, " << MAX_GROUPS << " groups" << LL_ENDL;
        return true;
    }

    // The tables, made on first use (14 MB): whole, so rows and groups never move.
    bool List::tables()
    {
        if (!mRowTable.buffer)
        {
            const VkBufferUsageFlags table = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (!createBuffer((VkDeviceSize)MAX_ROWS * sizeof(GpuRow), table, mRowTable) ||
                !createBuffer((VkDeviceSize)MAX_GROUPS * sizeof(GpuGroup), table, mGroupTable))
            {
                fail("its tables could not be allocated");
                return false;
            }
            // Before any group or row handle is handed out (zeroed: none damaged, none live).
            mDamaged.reset(new std::atomic<U8>[MAX_GROUPS]());
            mRowKeys.reset(new std::atomic<U64>[MAX_ROWS]());
        }
        return true;
    }

    void List::fail(const char* why)
    {
        if (!mBroken)
        {
            LL_WARNS("VKGPUList") << "GPU shadow list stops: " << why << "; shadow casters go back to packets" << LL_ENDL;
        }
        mBroken = true;
    }

    // Slots freed RELEASE_DELAY frames ago can be handed out again.
    void List::beginFrame()
    {
        const U32 frame = LLVKTracker::frame();
        if (frame == mFrame)
        {
            return;
        }
        mFrame = frame;
        auto release = [frame](std::vector<Release>& pending, std::vector<U32>& free)
        {
            size_t kept = 0;
            for (size_t i = 0; i < pending.size(); ++i)
            {
                if (frame - pending[i].frame >= RELEASE_DELAY)
                {
                    free.push_back(pending[i].slot);
                }
                else
                {
                    pending[kept++] = pending[i];
                }
            }
            pending.resize(kept);
        };
        release(mRowReleases, mFreeRows);
        release(mGroupReleases, mFreeGroups);
    }

    U32 List::acquireModel(const F32* matrix)
    {
        U32 empty = U32_MAX;
        for (U32 i = 0; i < mModelSpan; ++i)
        {
            if (mModels[i].refs && mModels[i].matrix == matrix)
            {
                ++mModels[i].refs;
                return i;
            }
            if (!mModels[i].refs && empty == U32_MAX)
            {
                empty = i;
            }
        }
        if (empty == U32_MAX)
        {
            if (mModelSpan >= MAX_MODELS)
            {
                return U32_MAX;
            }
            empty = mModelSpan++;
        }
        mModels[empty].matrix = matrix;
        mModels[empty].refs = 1;
        return empty;
    }

    void List::releaseModel(U32 slot)
    {
        if (slot < mModelSpan && mModels[slot].refs && --mModels[slot].refs == 0)
        {
            mModels[slot].matrix = nullptr;
            while (mModelSpan && !mModels[mModelSpan - 1].refs)
            {
                --mModelSpan;
            }
        }
    }

    U32 List::addGroup()
    {
        if (!ready() || !tables())
        {
            return 0;
        }
        beginFrame();
        U32 slot;
        if (!mFreeGroups.empty())
        {
            slot = mFreeGroups.back();
            mFreeGroups.pop_back();
        }
        else if (mGroups.size() < MAX_GROUPS)
        {
            slot = (U32)mGroups.size();
            mGroups.emplace_back();
            mGroupData.emplace_back();
            if (mResidentBits.size() < (mGroups.size() + 31) / 32)
            {
                mResidentBits.push_back(0);
            }
        }
        else
        {
            return 0;
        }
        GroupState& g = mGroups[slot];
        g = GroupState();
        g.used = true;
        mDamaged[slot].store(0, std::memory_order_release);
        ++mUsedGroups;
        return slot + 1;
    }

    // The group stops being resident (its bit, its model).
    void List::loseResidency(GroupState& g, U32 slot)
    {
        if (g.model != U32_MAX)
        {
            releaseModel(g.model);
            g.model = U32_MAX;
        }
        if (g.resident)
        {
            g.resident = false;
            mResidentBits[slot >> 5] &= ~(1u << (slot & 31));
            --mResident;
        }
    }

    void List::killRow(U32 slot, bool unpin, bool keep_in_group)
    {
        RowState& r = mRows[slot];
        if (!r.live)
        {
            return;
        }
        r.live = false;
        mRowKeys[slot].store(0, std::memory_order_release);
        --mLiveRows;
        --mBucketRows[r.bucket];
        releaseModel(r.model);
        if (unpin)
        {
            LLVKGPUDriven::unpin(r.vertex_buffer, r.index_buffer, slot);
        }
        if (!keep_in_group && r.group < mGroups.size())
        {
            std::vector<U32>& rows = mGroups[r.group].rows;
            auto it = std::find(rows.begin(), rows.end(), slot);
            if (it != rows.end())
            {
                *it = rows.back();
                rows.pop_back();
            }
        }
        // Tombstone: the shader walks every slot below the span and looks at the
        // bucket first, so a dead row is never drawn again whatever its group's
        // bits say (written before the next dispatch, after the ones already
        // recorded).
        mRowData[slot].flags = DEAD_BUCKET;
        mPendingRows.push_back(slot);
        Release release;
        release.slot = slot;
        release.frame = LLVKTracker::frame();
        mRowReleases.push_back(release);
    }

    // The pool dropped a copy a row drew from (GL wrote or deleted a buffer).
    void List::pinDropped(U32 slot)
    {
        if (slot >= mRows.size() || !mRows[slot].live)
        {
            return;
        }
        const U32 group = mRows[slot].group;
        killRow(slot, false, false);
        ++mDropped;
        if (group < mGroups.size() && mGroups[group].used)
        {
            GroupState& g = mGroups[group];
            mDamaged[group].store(1, std::memory_order_release);
            // Not drawn as resident any more: the CPU decides about it again
            // (LLPipeline revokes it at its next shadow pass).
            if (g.resident)
            {
                g.resident = false;
                mResidentBits[group >> 5] &= ~(1u << (group & 31));
                --mResident;
            }
        }
    }

    void List::clearGroup(U32 group)
    {
        const U32 slot = group - 1;
        if (!group || slot >= mGroups.size() || !mGroups[slot].used)
        {
            return;
        }
        GroupState& g = mGroups[slot];
        loseResidency(g, slot);
        for (U32 row : g.rows)
        {
            killRow(row, true, true);
        }
        g.rows.clear();
        mDamaged[slot].store(0, std::memory_order_release);
    }

    void List::removeGroup(U32 group)
    {
        const U32 slot = group - 1;
        if (!group || slot >= mGroups.size() || !mGroups[slot].used)
        {
            return;
        }
        clearGroup(group);
        mGroups[slot].used = false;
        --mUsedGroups;
        Release release;
        release.slot = slot;
        release.frame = LLVKTracker::frame();
        mGroupReleases.push_back(release);
    }

    U64 List::addRow(U32 group, const Row& row)
    {
        const U32 group_slot = group - 1;
        if (!ready() || !group || group_slot >= mGroups.size() || !mGroups[group_slot].used || mGroups[group_slot].resident ||
            !row.model || !row.index_count || !row.vertex_count || !row.index_total ||
            (row.index_type != GL_UNSIGNED_SHORT && row.index_type != GL_UNSIGNED_INT))
        {
            ++mRefused;
            return 0;
        }
        beginFrame();
        U32 slot;
        bool fresh = false;
        if (!mFreeRows.empty())
        {
            slot = mFreeRows.back();
            mFreeRows.pop_back();
        }
        else if (mRows.size() < MAX_ROWS)
        {
            slot = (U32)mRows.size();
            fresh = true;
        }
        else
        {
            ++mRefused;
            return 0;
        }
        auto give_back = [&]()
        {
            if (!fresh)
            {
                mFreeRows.push_back(slot);  // never used: free at once
            }
            ++mRefused;
        };
        const U32 model = acquireModel(row.model);
        if (model == U32_MAX)
        {
            give_back();
            return 0;
        }
        LLVKGPUDriven::Placement placed;
        if (!LLVKGPUDriven::placePinned(row.vertex_buffer, row.index_buffer, row.index_type, row.position_offset, row.vertex_count,
                                        row.index_total, slot, placed))
        {
            releaseModel(model);
            give_back();
            return 0;
        }
        if ((U64)row.first_index + row.index_count > placed.index_count || placed.chunk >= LLVKGPUDriven::MAX_CHUNKS)
        {
            LLVKGPUDriven::unpin(row.vertex_buffer, row.index_buffer, slot);
            releaseModel(model);
            give_back();
            return 0;
        }
        if (fresh)
        {
            mRows.emplace_back();
            mRowData.emplace_back();
            mRowSpan.store((U32)mRows.size(), std::memory_order_release);
        }
        const U32 bucket = placed.chunk * 2 + (placed.index32 ? 1 : 0);
        RowState& r = mRows[slot];
        ++r.gen;
        r.gen = r.gen ? r.gen : 1;
        r.live = true;
        r.group = group_slot;
        r.bucket = bucket;
        r.model = model;
        r.vertex_buffer = row.vertex_buffer;
        r.index_buffer = row.index_buffer;
        GpuRow& d = mRowData[slot];
        for (U32 i = 0; i < 3; ++i)
        {
            d.lo[i] = row.lo[i];
            d.hi[i] = row.hi[i];
        }
        d.group = group_slot;
        d.model = model;
        d.index_count = row.index_count;
        d.first_index = placed.first_index + row.first_index;
        d.vertex_offset = placed.vertex_offset;
        d.flags = bucket | (row.pool ? ROW_POOL : 0) | (row.loose ? ROW_LOOSE : 0);
        mPendingRows.push_back(slot);
        mGroups[group_slot].rows.push_back(slot);
        ++mBucketRows[bucket];
        ++mLiveRows;
        ++mAdded;
        mUsed = true;
        const U64 handle = ((U64)r.gen << 32) | (U64)(slot + 1);
        mRowKeys[slot].store(handle, std::memory_order_release);
        return handle;
    }

    // Without the lock (see serial()): the row's key is its handle while it
    // is live (the generation and slot as before; a slot never used is 0).
    bool List::rowLive(U64 row)
    {
        const U32 slot = (U32)row - 1;
        return (U32)row && slot < MAX_ROWS && mRowKeys && mRowKeys[slot].load(std::memory_order_acquire) == row;
    }

    // Without the lock (see serial()). A group slot past those made was
    // never damaged (0, as the vector's bounds answered before).
    bool List::takeDamaged(U32 group)
    {
        const U32 slot = group - 1;
        if (!group || slot >= MAX_GROUPS || !mDamaged)
        {
            return false;
        }
        return mDamaged[slot].exchange(0, std::memory_order_acq_rel) != 0;
    }

    U32 List::grant(U32 group, const F32 center[3], const F32 radius[3], const F32* model)
    {
        const U32 slot = group - 1;
        if (!ready() || !group || slot >= mGroups.size() || !model)
        {
            return 0;
        }
        GroupState& g = mGroups[slot];
        // Rows dropped and not yet acknowledged: the CPU has not seen it whole.
        if (!g.used || g.resident || mDamaged[slot].load(std::memory_order_acquire) || g.rows.empty())
        {
            return 0;
        }
        const U32 model_slot = acquireModel(model);
        if (model_slot == U32_MAX)
        {
            return 0;
        }
        if (g.model != U32_MAX)
        {
            releaseModel(g.model);
        }
        g.model = model_slot;
        g.resident = true;
        mResidentBits[slot >> 5] |= 1u << (slot & 31);
        ++mResident;
        ++mGranted;
        GpuGroup& d = mGroupData[slot];
        for (U32 i = 0; i < 3; ++i)
        {
            d.center[i] = center[i];
            d.radius[i] = radius[i];
        }
        d.model = model_slot;
        d.since = mSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
        mPendingGroups.push_back(slot);
        return d.since;
    }

    void List::revoke(U32 group)
    {
        const U32 slot = group - 1;
        if (!group || slot >= mGroups.size() || !mGroups[slot].used)
        {
            return;
        }
        GroupState& g = mGroups[slot];
        if (g.resident || g.model != U32_MAX)
        {
            ++mRevoked;
        }
        loseResidency(g, slot);
    }

    bool List::scratch(FrameSlot& slot, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset)
    {
        for (;;)
        {
            if (slot.scratch_index < slot.scratch.size())
            {
                Buffer& b = slot.scratch[slot.scratch_index];
                const VkDeviceSize start = (slot.scratch_head + mStorageAlign - 1) / mStorageAlign * mStorageAlign;
                if (start + size <= b.size)
                {
                    slot.scratch_head = start + size;
                    buffer = b.buffer;
                    offset = start;
                    return true;
                }
                ++slot.scratch_index;
                slot.scratch_head = 0;
                continue;
            }
            Buffer b;
            if (!createBuffer(llmax(SCRATCH_BYTES, size),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, b))
            {
                return false;
            }
            slot.scratch.push_back(b);
        }
    }

    VkDescriptorSet List::allocateSet(FrameSlot& slot)
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        for (;;)
        {
            if (slot.pool_index == slot.pools.size())
            {
                VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS_PER_POOL * BINDINGS };
                VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                info.maxSets = SETS_PER_POOL;
                info.poolSizeCount = 1;
                info.pPoolSizes = &size;
                VkDescriptorPool pool = VK_NULL_HANDLE;
                if (vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                {
                    return VK_NULL_HANDLE;
                }
                slot.pools.push_back(pool);
            }
            VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            alloc.descriptorPool = slot.pools[slot.pool_index];
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &mSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VkResult result = vkAllocateDescriptorSets(device, &alloc, &set);
            if (result == VK_SUCCESS)
            {
                return set;
            }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
            {
                return VK_NULL_HANDLE;
            }
            ++slot.pool_index;
        }
    }

    // Pending table writes into the upload commands, one copy per run of slots.
    bool List::flush(VkCommandBuffer cmd)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        auto write = [&](std::vector<U32>& pending, const U8* data, size_t stride, VkBuffer table) -> bool
        {
            if (pending.empty())
            {
                return true;
            }
            std::sort(pending.begin(), pending.end());
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
            VkBuffer ring = VK_NULL_HANDLE;
            VkDeviceSize ring_offset = 0;
            void* dst = nullptr;
            if (!mirror.allocate((VkDeviceSize)pending.size() * stride, 16, ring, ring_offset, dst))
            {
                return false;
            }
            static std::vector<VkBufferCopy> regions;
            regions.clear();
            U8* out = (U8*)dst;
            for (size_t i = 0; i < pending.size(); ++i)
            {
                const U32 slot = pending[i];
                memcpy(out + i * stride, data + (size_t)slot * stride, stride);
                if (!regions.empty() && pending[i - 1] + 1 == slot)
                {
                    regions.back().size += stride;
                }
                else
                {
                    VkBufferCopy region = { ring_offset + i * stride, (VkDeviceSize)slot * stride, stride };
                    regions.push_back(region);
                }
            }
            vkCmdCopyBuffer(cmd, ring, table, (U32)regions.size(), regions.data());
            pending.clear();
            return true;
        };
        return write(mPendingRows, (const U8*)mRowData.data(), sizeof(GpuRow), mRowTable.buffer) &&
               write(mPendingGroups, (const U8*)mGroupData.data(), sizeof(GpuGroup), mGroupTable.buffer);
    }

    bool List::prepare(const LLRHI::ShadowListPass& pass, const F32* view_proj, LLVKGPUList::Draws& out)
    {
        out = LLVKGPUList::Draws();
        if (!ready())
        {
            fail("not ready");
            return false;
        }
        beginFrame();
        LLVKMirror& mirror = LLVKMirror::instance();
        VkCommandBuffer cmd = mirror.uploadCommands();
        if (!cmd)
        {
            fail("no frame being recorded");
            return false;
        }
        if (mPendingBegins != mirror.frameBegins())
        {
            mPendingDispatches.clear();     // never recorded: that frame's upload commands are gone
        }
        FrameSlot& slot = mSlots[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        if (slot.begins != mirror.frameBegins())
        {
            // A frame began on the slot since its last use here, after waiting
            // for the slot's frame before: its output and sets are free again.
            slot.begins = mirror.frameBegins();
            slot.scratch_index = 0;
            slot.scratch_head = 0;
            for (VkDescriptorPool pool : slot.pools)
            {
                vkResetDescriptorPool(LLVKDevice::instance().getDevice(), pool, 0);
            }
            slot.pool_index = 0;
        }
        ++mPasses;
        // Table writes first, whether or not this pass draws anything: they are
        // in the upload commands before any later pass's dispatch either way,
        // and after the earlier passes' (which read the tables as they were).
        if (!mPendingRows.empty() || !mPendingGroups.empty())
        {
            recordDispatches(cmd);
        }
        if (!flush(cmd))
        {
            fail("the frame's upload ring is full");
            return false;
        }

        const U32 span = (U32)mRows.size();
        U32 total = 0;
        for (U32 b = 0; b < BUCKETS; ++b)
        {
            out.first[b] = total;
            out.capacity[b] = mBucketRows[b];
            total += mBucketRows[b];
            out.any |= mBucketRows[b] != 0;
        }
        if (!span || !total)
        {
            out.any = false;
            return true;    // no live rows: nothing to draw
        }

        // Draw matrix rows for the model slots, in a run (firstInstance = first + model).
        // (One run even when other threads take rows too, research/66.)
        U32 first_row = 0;
        ViewBlock view = {};
        LLVKBindless::DrawMatrices* rows = mModelSpan ? LLVKBindless::matrixRows(mModelSpan, first_row) : nullptr;
        if (mModelSpan && !rows)
        {
            fail("the frame's draw matrix table is full");
            return false;
        }
        for (U32 i = 0; i < mModelSpan; ++i)
        {
            LLVKBindless::DrawMatrices* m = rows + i;
            static const F32 identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            const F32* model = mModels[i].refs ? mModels[i].matrix : identity;
            mul4(view_proj, model, m->modelview_projection);
            memcpy(view.models[i], model, sizeof(view.models[i]));
        }

        for (U32 i = 0; i < 8; ++i)
        {
            const U8 mask = pass.masks[i];
            memcpy(view.planes[i], pass.planes[i], sizeof(view.planes[i]));
            if (mask < 8)
            {
                // LLCamera's sFrustumScaler[mask]: +1 on the axes whose bit is set.
                view.scalers[i][0] = (mask & 1) ? 1.f : -1.f;
                view.scalers[i][1] = (mask & 2) ? 1.f : -1.f;
                view.scalers[i][2] = (mask & 4) ? 1.f : -1.f;
                view.scalers[i][3] = 1.f;
            }
        }
        memcpy(view.sides, pass.sides, sizeof(view.sides));
        view.origin[0] = pass.origin[0];
        view.origin[1] = pass.origin[1];
        view.origin[2] = pass.origin[2];
        const bool groups = pass.volume || pass.terrain;
        const U32 group_span = groups ? (U32)mGroups.size() : 0;
        view.params[0] = span;
        view.params[1] = (pass.volume ? FLAG_VOLUME : 0) | (pass.terrain ? FLAG_TERRAIN : 0) | (pass.pools ? FLAG_POOLS : 0);
        view.params[2] = pass.since_min;
        view.params[3] = pass.since_max;
        view.params2[0] = first_row;
        view.params2[1] = mModelSpan;
        view.params2[2] = group_span;
        for (U32 b = 0; b < BUCKETS; ++b)
        {
            view.buckets[b][0] = out.first[b];
            view.buckets[b][1] = out.capacity[b];
        }

        // Per-pass inputs in the frame's ring.
        const U32 accept_words = (span + 31) / 32;
        const U32 resident_words = llmax((group_span + 31) / 32, 1u);
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize view_offset = 0, accept_offset = 0, resident_offset = 0, zero_offset = 0;
        void* dst = nullptr;
        if (!mirror.allocate(sizeof(ViewBlock), mStorageAlign, ring, view_offset, dst))
        {
            fail("the frame's upload ring is full");
            return false;
        }
        memcpy(dst, &view, sizeof(ViewBlock));
        if (!mirror.allocate((VkDeviceSize)accept_words * 4, mStorageAlign, ring, accept_offset, dst))
        {
            fail("the frame's upload ring is full");
            return false;
        }
        const U32 given = pass.accept ? llmin(pass.accept_words, accept_words) : 0;
        if (given)
        {
            memcpy(dst, pass.accept, (size_t)given * 4);
            mAccepted += 1;
        }
        memset((U8*)dst + (size_t)given * 4, 0, (size_t)(accept_words - given) * 4);
        if (!mirror.allocate((VkDeviceSize)resident_words * 4, mStorageAlign, ring, resident_offset, dst))
        {
            fail("the frame's upload ring is full");
            return false;
        }
        if (group_span)
        {
            memcpy(dst, mResidentBits.data(), (size_t)resident_words * 4);
        }
        else
        {
            memset(dst, 0, (size_t)resident_words * 4);
        }
        if (!mirror.allocate(COUNT_WORDS * 4, 16, ring, zero_offset, dst))
        {
            fail("the frame's upload ring is full");
            return false;
        }
        memset(dst, 0, COUNT_WORDS * 4);

        // Output: the counts, then the commands (5 words each).
        const VkDeviceSize output_bytes = (VkDeviceSize)(COUNT_WORDS + total * 5) * 4;
        VkBuffer output = VK_NULL_HANDLE;
        VkDeviceSize output_offset = 0;
        if (!scratch(slot, output_bytes, output, output_offset))
        {
            fail("no memory for the culled commands");
            return false;
        }
        VkDescriptorSet set = allocateSet(slot);
        if (!set)
        {
            fail("no descriptor set");
            return false;
        }
        VkDescriptorBufferInfo infos[BINDINGS] = {
            { mRowTable.buffer, 0, (VkDeviceSize)span * sizeof(GpuRow) },
            { mGroupTable.buffer, 0, (VkDeviceSize)llmax((U32)mGroups.size(), 1u) * sizeof(GpuGroup) },
            { ring, view_offset, sizeof(ViewBlock) },
            { ring, accept_offset, (VkDeviceSize)accept_words * 4 },
            { ring, resident_offset, (VkDeviceSize)resident_words * 4 },
            { output, output_offset, output_bytes },
        };
        VkWriteDescriptorSet writes[BINDINGS] = {};
        for (U32 i = 0; i < BINDINGS; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), BINDINGS, writes, 0, nullptr);

        // Zero the counts now; the dispatch follows with the other passes' (recordDispatches).
        VkBufferCopy zero = { zero_offset, output_offset, COUNT_WORDS * 4 };
        vkCmdCopyBuffer(cmd, ring, output, 1, &zero);
        mPendingDispatches.push_back({ set, (span + LOCAL_SIZE - 1) / LOCAL_SIZE });
        mPendingBegins = mirror.frameBegins();
        if (!sBatchDispatches)
        {
            recordDispatches(cmd);
        }

        out.buffer = output;
        out.counts = output_offset;
        out.commands = output_offset + COUNT_WORDS * 4;
        mUsed = true;
        return true;
    }

    // The pending dispatches into the upload commands, between one pair of
    // barriers: they write disjoint outputs and only read the tables and the ring.
    void List::recordDispatches(VkCommandBuffer cmd)
    {
        if (mPendingDispatches.empty())
        {
            return;
        }
        {
            // The table writes and the zeroed counts before the dispatches.
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
        for (const PendingDispatch& d : mPendingDispatches)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &d.set, 0, nullptr);
            vkCmdDispatch(cmd, d.groups, 1, 1);
        }
        {
            // The commands and counts to the frame's indirect draws; and the
            // table writes recorded after this wait until they have read them.
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        mDispatches += (U32)mPendingDispatches.size();
        ++mDispatchBatches;
        mPendingDispatches.clear();
    }

    // LLVKMirror::sBeforeUploadEnd: what is pending goes into the upload
    // commands before they are submitted (endFrame, submitAndWait).
    void List::recordPendingDispatches()
    {
        if (mPendingDispatches.empty())
        {
            return;
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        VkCommandBuffer cmd = mirror.uploadCommands();
        if (!cmd || mPendingBegins != mirror.frameBegins())
        {
            mPendingDispatches.clear();     // not this frame's (not expected)
            return;
        }
        recordDispatches(cmd);
    }

    std::string List::takeStats(U32 frames)
    {
        if (!mUsed)
        {
            return std::string();
        }
        frames = llmax(frames, 1u);
        std::ostringstream out;
        out << "list rows " << mLiveRows << " (span " << mRows.size() << "), groups " << mUsedGroups << " (resident " << mResident
            << "), passes " << mPasses / frames << " a frame (" << mAccepted / frames << " with CPU rows), rows added " << mAdded
            << ", refused " << mRefused << ", dropped by GL writes " << mDropped << ", granted " << mGranted << ", revoked " << mRevoked
            << ", dispatches " << (F32)mDispatches / frames << " in " << (F32)mDispatchBatches / frames << " batches a frame"
            << (mBroken ? ", BROKEN" : "");
        mPasses = mAccepted = mDropped = mAdded = mRefused = mGranted = mRevoked = 0;
        mDispatches = mDispatchBatches = 0;
        return out.str();
    }
}

void LLVKGPUList::install()
{
    // Nothing is made yet: ready() builds the shader and pipeline on first use
    // (LLPipeline::rhiListPhase, once the switches are on).
    LLRHI::sShadowList = &sList;
}

bool LLVKGPUList::prepare(const LLRHI::ShadowListPass& pass, const F32* view_proj, Draws& out)
{
    return sList.prepare(pass, view_proj, out);
}

std::string LLVKGPUList::takeStats(U32 frames)
{
    return sList.takeStats(frames);
}

// ----------------------------------------------------------------------------
// Stage G5 of research/46: the reflection probe faces' G-buffer list.
//
// A block is a static group's batches of one render type (the viewer's
// LLPipeline::rhiProbe*), registered at once in their draw order: a row per
// batch in a contiguous range of the row table (its index range in G4b's
// attribute pool, its region space bounds, its model slot and the texture
// handles of the units it names). Blocks die with their group's draw map or
// when the pool drops a copy they draw from (GL wrote or deleted a buffer).
//
// A face draws its blocks in the order the CPU would have drawn their
// batches. The viewer first draws the face's first batch the usual way: it
// opens a G4b draw run, which binds the pass's pipeline, set, push constants
// (rows: read the table), pool streams and index buffer. prepare() then
// takes the words that draw would push for units it does not name (the
// program's bindings, its material row), a matrix row per model with the
// face's matrices, the slot of every texture the blocks name, and records one
// compute dispatch into the frame's upload commands (which run before every
// draw of the frame). The dispatch writes, for every batch of the face, its
// VkDrawIndexedIndirectCommand (no instance when its bounds lie outside the
// face, as research/44 P1 leaves it out) and its row of words in the frame's
// palette table, at the batch's position in the face's order. drawRun()
// records the indirect draws: after each run's first batch went out as a
// draw run, one vkCmdDrawIndexedIndirect for the run's other batches.
//
// Research/50 step 4 (LLRHI::sGPUDrivenDGC, VK_EXT_device_generated_commands):
// - Blocks of any opaque G-buffer program on the shared layout, registered
//   from captured draws (addCapturedBlock, LLVKGLProbe::Capture): per batch
//   the GL texture each sampler's unit held (all of the program's units are
//   named), its material row (kept with the block, copied into the frame's
//   palette table once a frame, the batch's material word pointing at it)
//   and its program. Words a capture does not name (the constants row) come
//   from the face's first draw as for G5.
// - With Face::generated the dispatch writes, per batch, a sequence for
//   vkCmdExecuteGeneratedCommandsEXT instead: an index buffer token (its pool
//   chunk), a vertex buffer token per pool stream the face's batches read
//   (that chunk's), and the indexed draw (the same command as above). The
//   viewer cuts the face's batches into segments of one vertex format and
//   render state; after a segment's first batch went out as a draw run,
//   drawSegment() binds the run's pipeline made with dynamic vertex strides
//   and records one execute for the segment's other batches, any chunks, in
//   order. Descriptor sets, push constants (rows: read the table), dynamic
//   state and constant attributes stay as the run's first draw bound them.
// - No indirect execution set: one face pass is one program, and pipelines of
//   different programs differ in their vertex input (static state, VUID-
//   vkUpdateIndirectExecutionSetPipelineEXT-None-11040). See research/50 §9.

namespace
{
    constexpr U32 PROBE_MAX_ROWS = 1u << 18;            // 80 B each: 20 MB
    constexpr U32 PROBE_MAX_BLOCK_ROWS = 1u << 12;      // batches a block may have
    constexpr U32 PROBE_MAX_HANDLES = 0xffffu;          // handles are 16 bits in a row
    constexpr U32 PROBE_NO_HANDLE = 0xffffu;            // a unit no sampler of the program reads
    constexpr U32 PROBE_BINDINGS = 7;
    constexpr U32 PROBE_UNITS = LLRHI::ProbeList::MAX_UNITS;
    // Palette table vec4s the list's draw rows may take a frame (shared with
    // the draw runs' rows, palettes, materials and lights).
    constexpr U32 PROBE_ROW_VEC4S_PER_FRAME = 1u << 18;
    // GpuProbeRow::flags.
    constexpr U32 PROBE_DEAD = 1;                       // no batch
    constexpr U32 PROBE_MATERIAL = 2;                   // material: its material row in its block's materials
    constexpr U32 PROBE_CHUNK_SHIFT = 8;                // the pool chunk, 8 bits
    constexpr U32 PROBE_MAX_CHUNKS = 256;
    // Device generated commands: vertex buffer token slots a sequence may have.
    constexpr U32 PROBE_TOKENS = LLVKDevice::GeneratedCommands::MAX_VERTEX_TOKENS;
    constexpr VkDeviceSize PREPROCESS_BYTES = 8ull << 20;  // per preprocess buffer (more are made when a frame needs them)
    static_assert(PROBE_UNITS == 8 && PROBE_UNITS == LLVKGLProbe::Capture::UNITS, "a row packs 8 16-bit handles");
    static_assert(PROBE_TOKENS == LLVKGLProbe::Streams::MAX, "one vertex token per stream");

    // std430 layouts of the shader's tables.
    struct GpuProbeRow
    {
        F32 lo[3];
        U32 model;          // model slot
        F32 hi[3];
        U32 texture_count;  // units the batch names
        U32 index_count;
        U32 first_index;    // in the chunk's index buffer
        S32 vertex_offset;
        U32 flags;          // PROBE_DEAD, PROBE_MATERIAL, chunk << PROBE_CHUNK_SHIFT
        U32 handles[4];     // 8 texture handles, 16 bits each (unit 2k low, 2k + 1 high)
        U32 material;       // PROBE_MATERIAL: its material row's first vec4 in its block's materials
        U32 pad[3];
    };
    static_assert(sizeof(GpuProbeRow) == 80, "matches Row in the probe shader");

    struct ProbeView
    {
        F32 sides[4][4];
        F32 origin[4];
        U32 params[4];      // batches, blocks, words per row, vec4s per row
        U32 params2[4];     // matrix word, samplers, first vec4 of the rows in the palette table, handles
        U32 params3[4];     // command stride (words), generated, vertex token slots, material word
        U32 params4[4];     // chunks in the stream table, 0, 0, 0
        U32 words[32];
        S32 units[32];
        U32 model_rows[MAX_MODELS];
        F32 models[MAX_MODELS][16];
    };
    static_assert(sizeof(ProbeView) == 4752, "matches ViewBlock in the probe shader");

    const char* const PROBE_SHADER =
        "#version 450\n"
        "layout(local_size_x = 64) in;\n"
        "struct Row { vec4 lo; vec4 hi; uvec4 draw; uvec4 tex; uvec4 extra; };\n"
        "layout(std430, set = 0, binding = 0) readonly buffer RowTable { Row rows[]; };\n"
        "layout(std430, set = 0, binding = 1) readonly buffer BlockList { uvec4 blocks[]; };\n"
        "layout(std430, set = 0, binding = 2) readonly buffer SlotTable { uint slots[]; };\n"
        "layout(std430, set = 0, binding = 3) readonly buffer ViewBlock\n"
        "{\n"
        "    vec4 sides[4];\n"
        "    vec4 origin;\n"
        "    uvec4 params;\n"
        "    uvec4 params2;\n"
        "    uvec4 params3;\n"
        "    uvec4 params4;\n"
        "    uvec4 words[8];\n"
        "    ivec4 units[8];\n"
        "    uvec4 model_rows[16];\n"
        "    mat4 models[64];\n"
        "} view;\n"
        "layout(std430, set = 0, binding = 4) writeonly buffer Commands { uint cmd[]; };\n"
        "layout(std430, set = 0, binding = 5) writeonly buffer Words { uvec4 out_words[]; };\n"
        // Per pool chunk: its index buffer token (address, size, index type),
        // then a vertex buffer token (address, size, stride) per slot.
        "layout(std430, set = 0, binding = 6) readonly buffer StreamTable { uint streams[]; };\n"
        "vec3 box_radius(mat4 m, vec3 r)\n"
        "{\n"
        "    return abs(m[0].xyz) * r.x + abs(m[1].xyz) * r.y + abs(m[2].xyz) * r.z;\n"
        "}\n"
        // research/44 P1, as LLPipeline::postSort tests a probe face's batches:
        // a batch entirely outside one side plane, by more than the planes'
        // float error, draws no pixel.
        "bool row_outside(Row row, mat4 m)\n"
        "{\n"
        "    vec3 c0 = 0.5 * (row.lo.xyz + row.hi.xyz);\n"
        "    vec3 r0 = 0.5 * (row.hi.xyz - row.lo.xyz);\n"
        "    vec3 c = (m * vec4(c0, 1.0)).xyz;\n"
        "    vec3 r = box_radius(m, r0);\n"
        "    vec3 reach = abs(c - view.origin.xyz) + abs(r);\n"
        "    float margin = 0.01 + 0.001 * (reach.x + reach.y + reach.z);\n"
        "    for (uint i = 0u; i < 4u; ++i)\n"
        "    {\n"
        "        vec4 p = view.sides[i];\n"
        "        float nearest = dot(p.xyz, c) + p.w - dot(abs(p.xyz), abs(r));\n"
        "        if (nearest > margin)\n"
        "        {\n"
        "            return true;\n"
        "        }\n"
        "    }\n"
        "    return false;\n"
        "}\n"
        "uint handle_of(Row row, uint unit)\n"
        "{\n"
        "    uint w = row.tex[unit >> 1u];\n"
        "    return (unit & 1u) != 0u ? (w >> 16u) : (w & 0xffffu);\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    uint i = gl_GlobalInvocationID.x;\n"
        "    if (i >= view.params.x || view.params.y == 0u)\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        // The block of batch i: the last one starting at or before it.
        "    uint lo = 0u;\n"
        "    uint hi = view.params.y;\n"
        "    while (hi - lo > 1u)\n"
        "    {\n"
        "        uint mid = (lo + hi) >> 1u;\n"
        "        if (blocks[mid].z <= i)\n"
        "        {\n"
        "            lo = mid;\n"
        "        }\n"
        "        else\n"
        "        {\n"
        "            hi = mid;\n"
        "        }\n"
        "    }\n"
        "    uvec4 b = blocks[lo];\n"
        "    uint r = b.x + (i - b.z);\n"
        "    bool live = i >= b.z && i - b.z < b.y && r < uint(rows.length());\n"
        "    Row row = rows[live ? r : 0u];\n"
        "    uint model = floatBitsToUint(row.lo.w);\n"
        "    uint flags = row.draw.w;\n"
        "    uint chunk = (flags >> 8u) & 0xffu;\n"
        "    bool generated = view.params3.y != 0u;\n"
        "    live = live && (flags & 1u) == 0u && model < 64u && (!generated || chunk < view.params4.x);\n"
        "    model = min(model, 63u);\n"
        "    bool draw = live && !row_outside(row, view.models[model]);\n"
        "    uint vec4s = view.params.w;\n"
        "    uint first = i * vec4s;\n"
        "    uint at = i * view.params3.x;\n"
        "    if (generated)\n"
        "    {\n"
        // The sequence's tokens: its chunk's index buffer and vertex streams
        // (a batch not drawn binds chunk 0's: its draw has no indices).
        "        uint words = 4u + 4u * view.params3.z;\n"
        "        uint s = (live ? chunk : 0u) * words;\n"
        "        for (uint k = 0u; k < words; ++k)\n"
        "        {\n"
        "            cmd[at + k] = streams[s + k];\n"
        "        }\n"
        "        at += words;\n"
        "    }\n"
        "    cmd[at] = live ? row.draw.x : 0u;\n"
        "    cmd[at + 1u] = draw ? 1u : 0u;\n"
        "    cmd[at + 2u] = live ? row.draw.y : 0u;\n"
        "    cmd[at + 3u] = live ? row.draw.z : 0u;\n"
        "    cmd[at + 4u] = view.params2.z + first;\n"
        // The words the batch's own draw would push: the template, with the
        // slots of the units it names, the face's matrix row of its model and
        // its own material row.
        "    uint named = min(floatBitsToUint(row.hi.w), 8u);\n"
        "    for (uint v = 0u; v < vec4s; ++v)\n"
        "    {\n"
        "        uvec4 o = uvec4(0u);\n"
        "        for (uint k = 0u; k < 4u; ++k)\n"
        "        {\n"
        "            uint w = v * 4u + k;\n"
        "            uint value = w < view.params.z ? view.words[w >> 2u][w & 3u] : 0u;\n"
        "            if (w < view.params2.y)\n"
        "            {\n"
        "                int unit = view.units[w >> 2u][w & 3u];\n"
        "                if (unit >= 0 && uint(unit) < named)\n"
        "                {\n"
        "                    uint h = handle_of(row, uint(unit));\n"
        "                    value = h < view.params2.w ? slots[h] : 0u;\n"
        "                }\n"
        "            }\n"
        "            else if (w == view.params2.x)\n"
        "            {\n"
        "                value = view.model_rows[model >> 2u][model & 3u];\n"
        "            }\n"
        "            else if (w == view.params3.w && (flags & 2u) != 0u)\n"
        "            {\n"
        "                value = b.w + row.extra.x;\n"
        "            }\n"
        "            o[k] = value;\n"
        "        }\n"
        "        out_words[first + v] = o;\n"
        "    }\n"
        "}\n";

    struct ProbeBlock
    {
        bool used = false;
        bool damaged = false;               // the pool dropped a copy it draws from, or a texture it names was deleted
        bool captured = false;              // addCapturedBlock: its words are for `program`
        U32 first = 0;                      // row range
        U32 count = 0;
        U32 model = U32_MAX;                // model slot
        const void* program = nullptr;      // captured: the program (an identity) and sProgramChanges then
        U32 program_changes = 0;
        U32 material_vec4s = 0;             // captured: the program's material row length (0: none)
        U64 consts_hash = 0;                // captured: the program's constants row its draws had
        std::vector<U32> handles;           // distinct texture handles its rows name
        std::vector<std::pair<U32, U32> > pins;     // (vertex buffer, pin) per distinct vertex buffer
        // (location, stride) of the pool streams its batches read, with (research/293 branch 1) the stream's key and the bytes in it
        struct Stream { U32 location, stride, key, shift; };
        std::vector<Stream> streams;
        std::vector<F32> materials;         // captured: its distinct material rows
        U32 material_frame = U32_MAX;       // the frame its materials were copied into the palette table
        U32 material_base = 0;              // where (first vec4)
    };

    struct ProbeModel
    {
        const F32* matrix = nullptr;
        U32 refs = 0;
    };

    struct ProbeHandle
    {
        U32 name = 0;       // GL texture name
        U32 refs = 0;       // blocks naming it
    };

    // A preprocess buffer for device generated commands (one frame slot's).
    struct PreprocessBuffer
    {
        Buffer buffer;
        VkDeviceAddress address = 0;
        U32 memory_type = U32_MAX;
    };

    // Per frame slot: command output, descriptor sets and preprocess memory,
    // reused once the slot's last frame is done (LLVKMirror::beginFrame waited for it).
    struct ProbeFrameSlot
    {
        U64 begins = ~0ull;
        std::vector<Buffer> scratch;
        size_t scratch_index = 0;
        VkDeviceSize scratch_head = 0;
        std::vector<VkDescriptorPool> pools;
        size_t pool_index = 0;
        std::vector<PreprocessBuffer> preprocess;
        size_t preprocess_index = 0;
        VkDeviceSize preprocess_head = 0;
    };

    // What prepare() set up for the face being drawn.
    struct ProbePrepared
    {
        bool valid = false;
        U32 frame = U32_MAX;
        LLVKGLProbe::Run run;
        VkBuffer commands = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        U32 count = 0;
        U32 drops = 0;                      // ProbeListImpl::mDrops when prepared
        // Face::generated: sequences of `stride` words, the vertex token slots
        // (pool stream location and stride each) they carry.
        bool generated = false;
        U32 stride = 5;
        U32 slots = 0;
        U32 slot_location[PROBE_TOKENS] = {};
        U32 slot_stride[PROBE_TOKENS] = {};
    };

    // An indirect commands layout: its sequence stride and token slots, and
    // which vertex binding each used slot binds.
    struct ProbeLayoutKey
    {
        U32 stride = 0;
        U32 slots = 0;
        U32 binding_of_slot[PROBE_TOKENS] = {};     // U32_MAX: the slot is not bound
        bool operator<(const ProbeLayoutKey& other) const
        {
            if (stride != other.stride)
            {
                return stride < other.stride;
            }
            if (slots != other.slots)
            {
                return slots < other.slots;
            }
            return memcmp(binding_of_slot, other.binding_of_slot, sizeof(binding_of_slot)) < 0;
        }
    };

    class ProbeListImpl final : public LLRHI::ProbeList
    {
    public:
        bool ready() override;
        U32 addBlock(const Batch* batches, U32 count, const F32* model, Run* runs, U32& run_count) override;
        const char* refusal() const override { return mRefusal; }
        void removeBlock(U32 block) override;
        bool blockLive(U32 block) override;
        bool prepare(const Face& face) override;
        bool drawRun(U32 first, U32 count, U32 chunk, U64 format) override;
        bool generatedReady() override;
        bool drawSegment(U32 first, U32 count, U64 format) override;
        void beginCapture() override;
        U32 captured() override;
        void endCapture() override;
        U32 addCapturedBlock(const Bounds* bounds, U32 count, const F32* model, Run* runs, U32& run_count) override;

        void pairDropped(U32 vertex_buffer);
        void textureDeleted(U32 name);
        std::string takeStats(U32 frames);

    private:
        bool supported();
        bool init();
        bool tables();
        void beginFrame();
        bool allocateRows(U32 count, U32& first);
        void releaseRows(U32 first, U32 count);
        U32 acquireModel(const F32* matrix);
        void releaseModel(U32 slot);
        U32 acquireHandle(U32 name);
        void releaseHandle(U32 handle);
        void forgetBlock(ProbeBlock& b, U32 slot);
        U32 newBlock();
        bool pinBuffer(ProbeBlock& b, U32 slot, U32 vertex_buffer);
        U32 blockHandle(ProbeBlock& b, U32 name);
        void addStream(ProbeBlock& b, U32 location, U32 stride, U32 key, U32 shift);
        bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
        bool scratch(ProbeFrameSlot& slot, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset);
        VkDescriptorSet allocateSet(ProbeFrameSlot& slot);
        ProbeFrameSlot& frameSlot();
        bool preprocessMemory(ProbeFrameSlot& slot, const VkMemoryRequirements& need, VkDeviceAddress& address);
        VkIndirectCommandsLayoutEXT layoutFor(const ProbeLayoutKey& key);
        bool flush(VkCommandBuffer cmd);
        bool fail(const char* why);
        bool refuseSegment(const char* why);

        bool mTried = false;
        bool mReady = false;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        LLVKShader::Program mShader;
        Buffer mRowTable;
        VkDeviceSize mStorageAlign = 16;

        std::vector<GpuProbeRow> mRowData;
        U32 mRowSpan = 0;                               // rows below this were handed out at some point
        std::map<U32, U32> mFreeRows;                   // free row ranges: first -> count
        std::vector<Release> mRowReleases;              // slot: first row; frame (count in mReleaseCounts)
        std::vector<U32> mReleaseCounts;
        std::vector<U32> mPendingRows;                  // row slots to write into the table
        std::vector<ProbeBlock> mBlocks;
        std::vector<U32> mFreeBlocks;
        std::unordered_map<U32, std::vector<U32> > mBlocksByBuffer;    // vertex buffer -> block slots
        std::vector<ProbeHandle> mHandles;
        std::vector<U32> mFreeHandles;
        std::unordered_map<U32, U32> mHandleOf;         // GL name -> handle
        std::vector<U32> mHandleSlot;                   // handle -> bindless slot (as last resolved)
        std::vector<U32> mHandleStamp;                  // handle -> prepare serial it was resolved for
        ProbeModel mModels[MAX_MODELS];   // (not LLRHI::ProbeList::Model)
        U32 mModelSpan = 0;
        U32 mFrame = U32_MAX;
        U32 mRowVec4sFrame = U32_MAX;
        U32 mRowVec4s = 0;                              // palette table vec4s taken this frame
        U32 mPrepareSerial = 0;
        U32 mDrops = 0;                                 // pairs dropped (pairDropped calls)
        U32 mCaptureDrops = 0;                          // mDrops at beginCapture
        ProbeFrameSlot mSlots[LLVKMirror::FRAMES_IN_FLIGHT];
        ProbePrepared mPrepared;
        std::map<ProbeLayoutKey, VkIndirectCommandsLayoutEXT> mLayouts;     // never destroyed, like the pipeline

        // Since the last takeStats.
        U32 mFaces = 0, mBatches = 0, mIndirect = 0, mRunsRefused = 0, mAdded = 0, mRefused = 0, mDamaged = 0, mRemoved = 0;
        U32 mFailed = 0;
        const char* mLastFailure = nullptr;
        U32 mGeneratedFaces = 0, mExecutes = 0, mExecuted = 0, mSegmentsRefused = 0, mCapturedAdded = 0, mCaptureRefused = 0;
        U32 mTexturesDeleted = 0;
        const char* mLastSegmentRefusal = nullptr;
        const char* mLastCaptureRefusal = nullptr;
        const char* mRefusal = "";                      // why the last addBlock or addCapturedBlock refused
        U32 mLiveBlocks = 0, mLiveRows = 0;
        bool mUsed = false;
    };

    ProbeListImpl& sProbeListImpl = *new ProbeListImpl();   // never destroyed, like the shadow list

    void on_pair_dropped(U32 vertex_buffer)
    {
        sProbeListImpl.pairDropped(vertex_buffer);
    }

    void on_texture_deleted(LLVKTracker::GLName name)
    {
        sProbeListImpl.textureDeleted(name);
    }

    bool ProbeListImpl::supported()
    {
        const LLVKDevice& device = LLVKDevice::instance();
        return device.isReady() && device.hasMultiDrawIndirect() && device.hasDrawIndirectFirstInstance() &&
               device.getLimits().maxDrawIndirectCount >= PROBE_MAX_ROWS && LLVKAttribPool::supported() &&
               (U64)device.getLimits().maxComputeWorkGroupCount[0] * LOCAL_SIZE >= PROBE_MAX_ROWS;
    }

    bool ProbeListImpl::ready()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!mTried)
        {
            mTried = true;
            mReady = supported() && init();
            if (!mReady)
            {
                LL_WARNS("VKGPUList") << "GPU probe list unavailable: probe faces draw their batches on the CPU" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool ProbeListImpl::generatedReady()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        return ready() && LLVKDevice::instance().generatedCommands() != nullptr;
    }

    bool ProbeListImpl::init()
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        mStorageAlign = llmax(LLVKDevice::instance().getLimits().minStorageBufferOffsetAlignment, (VkDeviceSize)16);
        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, PROBE_SHADER } }, mShader) || mShader.modules.empty())
        {
            LL_WARNS("VKGPUList") << "probe list shader failed: " << mShader.log << LL_ENDL;
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[PROBE_BINDINGS] = {};
        for (U32 i = 0; i < PROBE_BINDINGS; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = PROBE_BINDINGS;
        set_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &mSetLayout;
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &mLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = mShader.modules[0];
        info.stage.pName = "main";
        info.layout = mLayout;
        if (vkCreateComputePipelines(device, LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &mPipeline) != VK_SUCCESS)
        {
            LL_WARNS("VKGPUList") << "probe list pipeline could not be created" << LL_ENDL;
            return false;
        }
        LLVKAttribPool::setDropCallback(&on_pair_dropped);
        LLVKTracker::addTextureDeletedCallback(&on_texture_deleted);
        LL_INFOS("VKGPUList") << "GPU probe list ready: up to " << PROBE_MAX_ROWS << " batches, device generated commands "
                              << (LLVKDevice::instance().generatedCommands() ? "available" : "not enabled") << LL_ENDL;
        return true;
    }

    bool ProbeListImpl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        if (LLVKDevice::instance().generatedCommands())
        {
            // Device generated commands read the sequences by address (research/50 step 4).
            info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &gpu, &out.buffer, &out.memory, nullptr) != VK_SUCCESS)
        {
            out = Buffer();
            return false;
        }
        out.size = size;
        return true;
    }

    // The row table, made on first use (20 MB): whole, so rows never move.
    bool ProbeListImpl::tables()
    {
        if (!mRowTable.buffer &&
            !createBuffer((VkDeviceSize)PROBE_MAX_ROWS * sizeof(GpuProbeRow),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, mRowTable))
        {
            return false;
        }
        return true;
    }

    // Row ranges freed RELEASE_DELAY frames ago can be handed out again.
    void ProbeListImpl::beginFrame()
    {
        const U32 frame = LLVKTracker::frame();
        if (frame == mFrame)
        {
            return;
        }
        mFrame = frame;
        size_t kept = 0;
        for (size_t i = 0; i < mRowReleases.size(); ++i)
        {
            if (frame - mRowReleases[i].frame >= RELEASE_DELAY)
            {
                // Back into the free ranges, merged with its neighbours.
                U32 first = mRowReleases[i].slot;
                U32 count = mReleaseCounts[i];
                auto next = mFreeRows.lower_bound(first);
                if (next != mFreeRows.end() && first + count == next->first)
                {
                    count += next->second;
                    next = mFreeRows.erase(next);
                }
                if (next != mFreeRows.begin())
                {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second == first)
                    {
                        first = prev->first;
                        count += prev->second;
                        mFreeRows.erase(prev);
                    }
                }
                mFreeRows[first] = count;
            }
            else
            {
                mRowReleases[kept] = mRowReleases[i];
                mReleaseCounts[kept] = mReleaseCounts[i];
                ++kept;
            }
        }
        mRowReleases.resize(kept);
        mReleaseCounts.resize(kept);
    }

    bool ProbeListImpl::allocateRows(U32 count, U32& first)
    {
        // First fit among the freed ranges, else the end of the table.
        for (auto it = mFreeRows.begin(); it != mFreeRows.end(); ++it)
        {
            if (it->second >= count)
            {
                first = it->first;
                const U32 left = it->second - count;
                mFreeRows.erase(it);
                if (left)
                {
                    mFreeRows[first + count] = left;
                }
                return true;
            }
        }
        if (mRowSpan + count > PROBE_MAX_ROWS)
        {
            return false;
        }
        first = mRowSpan;
        mRowSpan += count;
        if (mRowData.size() < mRowSpan)
        {
            mRowData.resize(mRowSpan);
        }
        return true;
    }

    // A range no dispatch recorded from now on reads (frames in flight may).
    void ProbeListImpl::releaseRows(U32 first, U32 count)
    {
        Release release;
        release.slot = first;
        release.frame = LLVKTracker::frame();
        mRowReleases.push_back(release);
        mReleaseCounts.push_back(count);
    }

    U32 ProbeListImpl::acquireModel(const F32* matrix)
    {
        U32 empty = U32_MAX;
        for (U32 i = 0; i < mModelSpan; ++i)
        {
            if (mModels[i].refs && mModels[i].matrix == matrix)
            {
                ++mModels[i].refs;
                return i;
            }
            if (!mModels[i].refs && empty == U32_MAX)
            {
                empty = i;
            }
        }
        if (empty == U32_MAX)
        {
            if (mModelSpan >= MAX_MODELS)
            {
                return U32_MAX;
            }
            empty = mModelSpan++;
        }
        mModels[empty].matrix = matrix;
        mModels[empty].refs = 1;
        return empty;
    }

    void ProbeListImpl::releaseModel(U32 slot)
    {
        if (slot < mModelSpan && mModels[slot].refs && --mModels[slot].refs == 0)
        {
            mModels[slot].matrix = nullptr;
            while (mModelSpan && !mModels[mModelSpan - 1].refs)
            {
                --mModelSpan;
            }
        }
    }

    U32 ProbeListImpl::acquireHandle(U32 name)
    {
        auto it = mHandleOf.find(name);
        if (it != mHandleOf.end())
        {
            ++mHandles[it->second].refs;
            return it->second;
        }
        U32 handle;
        if (!mFreeHandles.empty())
        {
            handle = mFreeHandles.back();
            mFreeHandles.pop_back();
        }
        else if (mHandles.size() < PROBE_MAX_HANDLES)
        {
            handle = (U32)mHandles.size();
            mHandles.emplace_back();
            mHandleSlot.push_back(LLVKBindless::WHITE_SLOT);
            mHandleStamp.push_back(0);
        }
        else
        {
            return U32_MAX;
        }
        mHandles[handle].name = name;
        mHandles[handle].refs = 1;
        mHandleStamp[handle] = 0;
        mHandleOf[name] = handle;
        return handle;
    }

    void ProbeListImpl::releaseHandle(U32 handle)
    {
        if (handle < mHandles.size() && mHandles[handle].refs && --mHandles[handle].refs == 0)
        {
            mHandleOf.erase(mHandles[handle].name);
            mHandles[handle] = ProbeHandle();
            mFreeHandles.push_back(handle);
        }
    }

    // Lets go of everything the block holds (its rows go back after the delay).
    void ProbeListImpl::forgetBlock(ProbeBlock& b, U32 slot)
    {
        for (const auto& pin : b.pins)
        {
            LLVKAttribPool::unpin(pin.first, pin.second);
            auto it = mBlocksByBuffer.find(pin.first);
            if (it != mBlocksByBuffer.end())
            {
                std::vector<U32>& blocks = it->second;
                blocks.erase(std::remove(blocks.begin(), blocks.end(), slot), blocks.end());
                if (blocks.empty())
                {
                    mBlocksByBuffer.erase(it);
                }
            }
        }
        for (U32 handle : b.handles)
        {
            releaseHandle(handle);
        }
        if (b.model != U32_MAX)
        {
            releaseModel(b.model);
        }
        if (b.count)
        {
            // Dead rows, in case anything reads them before they are reused.
            for (U32 r = b.first; r < b.first + b.count; ++r)
            {
                mRowData[r].flags = PROBE_DEAD;
                mPendingRows.push_back(r);
            }
            releaseRows(b.first, b.count);
            mLiveRows -= b.count;
        }
        b = ProbeBlock();
    }

    U32 ProbeListImpl::newBlock()
    {
        U32 slot;
        if (!mFreeBlocks.empty())
        {
            slot = mFreeBlocks.back();
            mFreeBlocks.pop_back();
        }
        else
        {
            slot = (U32)mBlocks.size();
            mBlocks.emplace_back();
        }
        mBlocks[slot] = ProbeBlock();
        mBlocks[slot].used = true;
        return slot;
    }

    // The block keeps the pool copy of the vertex buffer (pinned: not dropped for being idle).
    bool ProbeListImpl::pinBuffer(ProbeBlock& b, U32 slot, U32 vertex_buffer)
    {
        for (const auto& pin : b.pins)
        {
            if (pin.first == vertex_buffer)
            {
                return true;
            }
        }
        const U32 pin = LLVKAttribPool::pin(vertex_buffer);
        if (!pin)
        {
            return false;
        }
        b.pins.emplace_back(vertex_buffer, pin);
        mBlocksByBuffer[vertex_buffer].push_back(slot);
        return true;
    }

    // The handle of a GL texture name the block's rows name (U32_MAX: no room).
    U32 ProbeListImpl::blockHandle(ProbeBlock& b, U32 name)
    {
        for (U32 known : b.handles)
        {
            if (mHandles[known].name == name)
            {
                return known;
            }
        }
        const U32 handle = acquireHandle(name);
        if (handle != U32_MAX)
        {
            b.handles.push_back(handle);
        }
        return handle;
    }

    void ProbeListImpl::addStream(ProbeBlock& b, U32 location, U32 stride, U32 key, U32 shift)
    {
        for (const auto& s : b.streams)
        {
            if (s.location == location && s.stride == stride && s.key == key && s.shift == shift)
            {
                return;
            }
        }
        b.streams.push_back({ location, stride, key, shift });
    }

    U32 ProbeListImpl::addBlock(const Batch* batches, U32 count, const F32* model, Run* runs, U32& run_count)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        run_count = 0;
        if (!ready() || !tables())
        {
            mRefusal = "not ready";
            ++mRefused;
            return 0;
        }
        if (!batches || !runs || !model || !count || count > PROBE_MAX_BLOCK_ROWS)
        {
            mRefusal = "batch count";
            ++mRefused;
            return 0;
        }
        beginFrame();
        mUsed = true;
        const U32 slot = newBlock();
        ProbeBlock& b = mBlocks[slot];
        auto refuse = [&](const char* why)
        {
            forgetBlock(b, slot);
            mFreeBlocks.push_back(slot);
            ++mRefused;
            mRefusal = why;
            return 0u;
        };
        U32 first = 0;
        if (!allocateRows(count, first))
        {
            return refuse("row table full");
        }
        b.first = first;
        b.count = count;
        mLiveRows += count;
        b.model = acquireModel(model);
        if (b.model == U32_MAX)
        {
            return refuse("model table full");
        }
        // A pool drop while placing (another layout of a pair, say) may take a
        // copy placed for an earlier batch: then refuse, and try again later.
        const U32 drops = mDrops;
        for (U32 i = 0; i < count; ++i)
        {
            const Batch& batch = batches[i];
            const LLRHI::DirectDraw& d = batch.draw;
            LLVKAttribPool::Placement place;
            if (batch.texture_count > PROBE_UNITS || d.mode != 0x0004 || d.instance_count > 1 || !d.index_count ||
                d.table_base != LLRHI::DirectDraw::NO_TABLE)
            {
                return refuse("batch shape");
            }
            if (!LLVKAttribPool::place(d, place))
            {
                return refuse("not in the pool (copy budget, room, layout or GL writes)");
            }
            if (place.chunk >= PROBE_MAX_CHUNKS)
            {
                return refuse("pool chunk beyond the list's");
            }
            if (!pinBuffer(b, slot, d.vertex_buffer))
            {
                return refuse("pool pin");
            }
            GpuProbeRow& row = mRowData[first + i];
            row = GpuProbeRow();
            for (U32 k = 0; k < 3; ++k)
            {
                row.lo[k] = batch.lo[k];
                row.hi[k] = batch.hi[k];
            }
            row.model = b.model;
            row.texture_count = batch.texture_count;
            row.index_count = d.index_count;
            row.first_index = place.first_index + d.first_index;
            row.vertex_offset = place.vertex_offset;
            row.flags = place.chunk << PROBE_CHUNK_SHIFT;
            for (U32 unit = 0; unit < batch.texture_count; ++unit)
            {
                const U32 handle = blockHandle(b, batch.textures[unit]);
                if (handle == U32_MAX)
                {
                    return refuse("texture handles full");
                }
                row.handles[unit >> 1] |= (handle & 0xffffu) << ((unit & 1) * 16);
            }
            // The pool streams it may read (every attribute it brings is placed).
            for (U32 k = 0; k < d.attribute_count; ++k)
            {
                addStream(b, d.attributes[k].location, d.attributes[k].stride, d.attributes[k].streamKey(), d.attributes[k].streamShift());
            }
            mPendingRows.push_back(first + i);
            // Runs of batches one indirect draw can take (one chunk, one vertex format).
            const U64 format = LLVKGLProbe::drawFormat(d);
            if (run_count && runs[run_count - 1].chunk == place.chunk && runs[run_count - 1].format == format)
            {
                ++runs[run_count - 1].count;
            }
            else
            {
                Run& run = runs[run_count++];
                run.first = i;
                run.count = 1;
                run.chunk = place.chunk;
                run.format = format;
                run.state = 0;
            }
        }
        if (mDrops != drops)
        {
            run_count = 0;
            return refuse("a pool copy dropped while placing");
        }
        ++mLiveBlocks;
        ++mAdded;
        return slot + 1;
    }

    void ProbeListImpl::beginCapture()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        LLVKGLProbe::beginCapture();
        // A pool drop during the captures (one placing a pair another way) may
        // take a copy an earlier capture placed: addCapturedBlock refuses then.
        mCaptureDrops = mDrops;
    }

    U32 ProbeListImpl::captured()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        return LLVKGLProbe::captureCount();
    }

    void ProbeListImpl::endCapture()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        LLVKGLProbe::endCapture();
    }

    // Research/50 step 4: a block of the draws captured so far, each named in
    // full (every unit its program samples, its material row).
    U32 ProbeListImpl::addCapturedBlock(const Bounds* bounds, U32 count, const F32* model, Run* runs, U32& run_count)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        run_count = 0;
        const LLVKGLProbe::Capture* first_capture = LLVKGLProbe::capture(0);
        if (!ready() || !tables() || !bounds || !runs || !model || !count || count > PROBE_MAX_BLOCK_ROWS ||
            LLVKGLProbe::captureCount() != count || !first_capture || !first_capture->ok ||
            first_capture->program_changes != LLVKGLProbe::programChanges())
        {
            mLastCaptureRefusal = !first_capture ? "nothing captured" :
                                  !first_capture->ok ? first_capture->why :
                                  LLVKGLProbe::captureCount() != count ? "draws captured" : "not ready";
            mRefusal = mLastCaptureRefusal ? mLastCaptureRefusal : "capture";
            ++mCaptureRefused;
            return 0;
        }
        beginFrame();
        mUsed = true;
        const U32 slot = newBlock();
        ProbeBlock& b = mBlocks[slot];
        auto refuse = [&](const char* why)
        {
            forgetBlock(b, slot);
            mFreeBlocks.push_back(slot);
            mLastCaptureRefusal = why;
            mRefusal = why;
            ++mCaptureRefused;
            return 0u;
        };
        U32 first = 0;
        if (!allocateRows(count, first))
        {
            return refuse("row table full");
        }
        b.first = first;
        b.count = count;
        mLiveRows += count;
        b.model = acquireModel(model);
        if (b.model == U32_MAX)
        {
            return refuse("models");
        }
        b.captured = true;
        b.program = first_capture->program;
        b.program_changes = first_capture->program_changes;
        b.material_vec4s = first_capture->material_vec4s;
        b.consts_hash = first_capture->consts_hash;
        // Distinct material rows of the block: hash of the bits -> offsets with that hash.
        std::unordered_map<U64, std::vector<U32> > materials;
        const U32 drops = mDrops;
        for (U32 i = 0; i < count; ++i)
        {
            const LLVKGLProbe::Capture* c = LLVKGLProbe::capture(i);
            if (!c || !c->ok)
            {
                return refuse(c && c->why ? c->why : "a draw was not captured");
            }
            if (c->program != b.program || c->program_changes != b.program_changes || c->material_vec4s != b.material_vec4s)
            {
                return refuse("draws of another program");
            }
            if (c->consts_hash != b.consts_hash)
            {
                // A value the loop sets per batch is in the constants row (not a
                // material member): the list would draw them all with one.
                return refuse("constants that change per batch");
            }
            if (c->chunk >= PROBE_MAX_CHUNKS || !pinBuffer(b, slot, c->vertex_buffer))
            {
                return refuse("pool copy");
            }
            GpuProbeRow& row = mRowData[first + i];
            row = GpuProbeRow();
            for (U32 k = 0; k < 3; ++k)
            {
                row.lo[k] = bounds[i].lo[k];
                row.hi[k] = bounds[i].hi[k];
            }
            row.model = b.model;
            row.texture_count = PROBE_UNITS;    // every unit is named (no template word for a sampler)
            row.index_count = c->index_count;
            row.first_index = c->first_index;
            row.vertex_offset = c->vertex_offset;
            row.flags = c->chunk << PROBE_CHUNK_SHIFT;
            for (U32 unit = 0; unit < PROBE_UNITS; ++unit)
            {
                U32 handle = PROBE_NO_HANDLE;
                if (c->sampled & (1u << unit))
                {
                    handle = blockHandle(b, c->names[unit]);
                    if (handle == U32_MAX)
                    {
                        return refuse("texture handles");
                    }
                }
                row.handles[unit >> 1] |= (handle & 0xffffu) << ((unit & 1) * 16);
            }
            if (c->material_vec4s)
            {
                const size_t floats = (size_t)c->material_vec4s * 4;
                const U64 hash = LLRHI::fnv1aFloats(LLRHI::FNV1A_BASIS_SHORT, c->material, floats);
                std::vector<U32>& same = materials[hash];
                U32 offset = U32_MAX;
                for (U32 known : same)
                {
                    if (!memcmp(&b.materials[(size_t)known * 4], c->material, floats * 4))
                    {
                        offset = known;
                        break;
                    }
                }
                if (offset == U32_MAX)
                {
                    offset = (U32)(b.materials.size() / 4);
                    b.materials.insert(b.materials.end(), c->material, c->material + floats);
                    same.push_back(offset);
                }
                row.flags |= PROBE_MATERIAL;
                row.material = offset;
            }
            for (U32 k = 0; k < c->streams; ++k)
            {
                addStream(b, c->stream_location[k], c->stream_stride[k], c->stream_key[k], c->stream_shift[k]);
            }
            mPendingRows.push_back(first + i);
            // Runs as addBlock makes them, also cut where the render state changes.
            if (run_count && runs[run_count - 1].chunk == c->chunk && runs[run_count - 1].format == c->format &&
                runs[run_count - 1].state == c->state)
            {
                ++runs[run_count - 1].count;
            }
            else
            {
                Run& run = runs[run_count++];
                run.first = i;
                run.count = 1;
                run.chunk = c->chunk;
                run.format = c->format;
                run.state = c->state;
            }
        }
        if (mDrops != drops || mDrops != mCaptureDrops || b.materials.size() / 4 > PROBE_ROW_VEC4S_PER_FRAME / 4)
        {
            run_count = 0;
            return refuse("pool drop");
        }
        ++mLiveBlocks;
        ++mCapturedAdded;
        return slot + 1;
    }

    void ProbeListImpl::removeBlock(U32 block)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const U32 slot = block - 1;
        if (!block || slot >= mBlocks.size() || !mBlocks[slot].used)
        {
            return;
        }
        beginFrame();
        forgetBlock(mBlocks[slot], slot);
        mFreeBlocks.push_back(slot);
        --mLiveBlocks;
        ++mRemoved;
    }

    bool ProbeListImpl::blockLive(U32 block)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const U32 slot = block - 1;
        if (!block || slot >= mBlocks.size() || !mBlocks[slot].used || mBlocks[slot].damaged)
        {
            return false;
        }
        // A captured block's words are for one link of its program.
        return !mBlocks[slot].captured || mBlocks[slot].program_changes == LLVKGLProbe::programChanges();
    }

    // The pool dropped a copy (GL wrote or deleted a buffer): blocks drawing from it are done.
    void ProbeListImpl::pairDropped(U32 vertex_buffer)
    {
        ++mDrops;
        auto it = mBlocksByBuffer.find(vertex_buffer);
        if (it == mBlocksByBuffer.end())
        {
            return;
        }
        for (U32 slot : it->second)
        {
            if (slot < mBlocks.size() && mBlocks[slot].used && !mBlocks[slot].damaged)
            {
                mBlocks[slot].damaged = true;
                ++mDamaged;
                // Its pin went with the copy: nothing to give back for it.
                for (auto& pin : mBlocks[slot].pins)
                {
                    if (pin.first == vertex_buffer)
                    {
                        pin.second = 0;
                    }
                }
            }
        }
    }

    // GL deletes a texture (its name may come back for another one): blocks
    // naming it are done, as the viewer's name check would find later.
    void ProbeListImpl::textureDeleted(U32 name)
    {
        auto it = mHandleOf.find(name);
        if (it == mHandleOf.end())
        {
            return;
        }
        const U32 handle = it->second;
        ++mTexturesDeleted;
        for (ProbeBlock& b : mBlocks)
        {
            if (b.used && !b.damaged && std::find(b.handles.begin(), b.handles.end(), handle) != b.handles.end())
            {
                b.damaged = true;
                ++mDamaged;
            }
        }
    }

    bool ProbeListImpl::scratch(ProbeFrameSlot& slot, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset)
    {
        for (;;)
        {
            if (slot.scratch_index < slot.scratch.size())
            {
                Buffer& b = slot.scratch[slot.scratch_index];
                const VkDeviceSize start = (slot.scratch_head + mStorageAlign - 1) / mStorageAlign * mStorageAlign;
                if (start + size <= b.size)
                {
                    slot.scratch_head = start + size;
                    buffer = b.buffer;
                    offset = start;
                    return true;
                }
                ++slot.scratch_index;
                slot.scratch_head = 0;
                continue;
            }
            Buffer b;
            if (!createBuffer(llmax(SCRATCH_BYTES, size), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, b))
            {
                return false;
            }
            slot.scratch.push_back(b);
        }
    }

    VkDescriptorSet ProbeListImpl::allocateSet(ProbeFrameSlot& slot)
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        for (;;)
        {
            if (slot.pool_index == slot.pools.size())
            {
                VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS_PER_POOL * PROBE_BINDINGS };
                VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                info.maxSets = SETS_PER_POOL;
                info.poolSizeCount = 1;
                info.pPoolSizes = &size;
                VkDescriptorPool pool = VK_NULL_HANDLE;
                if (vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                {
                    return VK_NULL_HANDLE;
                }
                slot.pools.push_back(pool);
            }
            VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            alloc.descriptorPool = slot.pools[slot.pool_index];
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &mSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VkResult result = vkAllocateDescriptorSets(device, &alloc, &set);
            if (result == VK_SUCCESS)
            {
                return set;
            }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
            {
                return VK_NULL_HANDLE;
            }
            ++slot.pool_index;
        }
    }

    // This frame's slot: its scratch, sets and preprocess memory start over
    // when a new frame uses it (the slot's last frame is done by then).
    ProbeFrameSlot& ProbeListImpl::frameSlot()
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        ProbeFrameSlot& fslot = mSlots[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        if (fslot.begins != mirror.frameBegins())
        {
            fslot.begins = mirror.frameBegins();
            fslot.scratch_index = 0;
            fslot.scratch_head = 0;
            for (VkDescriptorPool pool : fslot.pools)
            {
                vkResetDescriptorPool(LLVKDevice::instance().getDevice(), pool, 0);
            }
            fslot.pool_index = 0;
            fslot.preprocess_index = 0;
            fslot.preprocess_head = 0;
        }
        return fslot;
    }

    // Preprocess memory for one execute (each its own range: the implicit
    // preprocess writes it while the commands run).
    bool ProbeListImpl::preprocessMemory(ProbeFrameSlot& slot, const VkMemoryRequirements& need, VkDeviceAddress& address)
    {
        const VkDeviceSize align = llmax(need.alignment, (VkDeviceSize)16);
        for (;;)
        {
            if (slot.preprocess_index < slot.preprocess.size())
            {
                PreprocessBuffer& p = slot.preprocess[slot.preprocess_index];
                const VkDeviceAddress start = (p.address + slot.preprocess_head + align - 1) / align * align;
                if (p.memory_type < 32 && (need.memoryTypeBits & (1u << p.memory_type)) &&
                    start + need.size <= p.address + p.buffer.size)
                {
                    slot.preprocess_head = start + need.size - p.address;
                    address = start;
                    return true;
                }
                ++slot.preprocess_index;
                slot.preprocess_head = 0;
                continue;
            }
            // A new buffer with the preprocess usage (VkBufferUsageFlags2, maintenance5);
            // VMA reads the plain usage (device address memory).
            VkBufferUsageFlags2CreateInfoKHR usage2 = { VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR };
            usage2.usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR;
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.pNext = &usage2;
            info.size = llmax(PREPROCESS_BYTES, need.size + align);
            info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VmaAllocationCreateInfo gpu = {};
            gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            gpu.memoryTypeBits = need.memoryTypeBits;
            PreprocessBuffer p;
            VmaAllocationInfo allocation = {};
            if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &gpu, &p.buffer.buffer, &p.buffer.memory, &allocation) !=
                VK_SUCCESS)
            {
                return false;
            }
            p.buffer.size = info.size;
            p.memory_type = allocation.memoryType;
            VkBufferDeviceAddressInfo where = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            where.buffer = p.buffer.buffer;
            p.address = vkGetBufferDeviceAddress(LLVKDevice::instance().getDevice(), &where);
            if (!p.address || !(need.memoryTypeBits & (1u << p.memory_type)))
            {
                vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), p.buffer.buffer, p.buffer.memory);
                return false;
            }
            // Into the slot's list at the current index (the earlier ones are spent this frame).
            slot.preprocess.insert(slot.preprocess.begin() + (std::ptrdiff_t)slot.preprocess_index, p);
            slot.preprocess_head = 0;
        }
    }

    // The indirect commands layout of a sequence: the index buffer token at 0,
    // a vertex buffer token per bound slot (16 bytes each, after it), the
    // indexed draw after all the slots. Made on first use.
    VkIndirectCommandsLayoutEXT ProbeListImpl::layoutFor(const ProbeLayoutKey& key)
    {
        auto it = mLayouts.find(key);
        if (it != mLayouts.end())
        {
            return it->second;
        }
        const LLVKDevice::GeneratedCommands* dgc = LLVKDevice::instance().generatedCommands();
        VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
        const U32 draw_offset = 16 + 16 * key.slots;
        if (dgc && key.stride * 4 <= dgc->properties.maxIndirectCommandsIndirectStride &&
            draw_offset <= dgc->properties.maxIndirectCommandsTokenOffset &&
            (dgc->properties.supportedIndirectCommandsInputModes & VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT))
        {
            VkIndirectCommandsIndexBufferTokenEXT index = { VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT };
            VkIndirectCommandsVertexBufferTokenEXT vertex[PROBE_TOKENS] = {};
            VkIndirectCommandsLayoutTokenEXT tokens[PROBE_TOKENS + 2] = {};
            U32 count = 0;
            tokens[count].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
            tokens[count].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT;
            tokens[count].data.pIndexBuffer = &index;
            tokens[count].offset = 0;
            ++count;
            for (U32 j = 0; j < key.slots; ++j)
            {
                if (key.binding_of_slot[j] == U32_MAX)
                {
                    continue;
                }
                vertex[j].vertexBindingUnit = key.binding_of_slot[j];
                tokens[count].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
                tokens[count].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT;
                tokens[count].data.pVertexBuffer = &vertex[j];
                tokens[count].offset = 16 + 16 * j;
                ++count;
            }
            tokens[count].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
            tokens[count].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
            tokens[count].offset = draw_offset;
            ++count;
            VkIndirectCommandsLayoutCreateInfoEXT info = { VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT };
            info.flags = 0;     // preprocessed by the execute; sequences in order (G-buffer order matters)
            info.shaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            info.indirectStride = key.stride * 4;
            info.pipelineLayout = VK_NULL_HANDLE;   // no push constant tokens
            info.tokenCount = count;
            info.pTokens = tokens;
            if (count > dgc->properties.maxIndirectCommandsTokenCount ||
                dgc->createLayout(LLVKDevice::instance().getDevice(), &info, nullptr, &layout) != VK_SUCCESS)
            {
                layout = VK_NULL_HANDLE;
            }
        }
        mLayouts[key] = layout;     // a failure is remembered too
        return layout;
    }

    // Pending row writes into the upload commands, one copy per run of rows.
    bool ProbeListImpl::flush(VkCommandBuffer cmd)
    {
        if (mPendingRows.empty())
        {
            return true;
        }
        std::sort(mPendingRows.begin(), mPendingRows.end());
        mPendingRows.erase(std::unique(mPendingRows.begin(), mPendingRows.end()), mPendingRows.end());
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize ring_offset = 0;
        void* dst = nullptr;
        const size_t stride = sizeof(GpuProbeRow);
        if (!LLVKMirror::instance().allocate((VkDeviceSize)mPendingRows.size() * stride, 16, ring, ring_offset, dst))
        {
            return false;
        }
        static std::vector<VkBufferCopy> regions;
        regions.clear();
        U8* out = (U8*)dst;
        for (size_t i = 0; i < mPendingRows.size(); ++i)
        {
            const U32 row = mPendingRows[i];
            memcpy(out + i * stride, &mRowData[row], stride);
            if (!regions.empty() && mPendingRows[i - 1] + 1 == row)
            {
                regions.back().size += stride;
            }
            else
            {
                VkBufferCopy region = { ring_offset + i * stride, (VkDeviceSize)row * stride, stride };
                regions.push_back(region);
            }
        }
        vkCmdCopyBuffer(cmd, ring, mRowTable.buffer, (U32)regions.size(), regions.data());
        mPendingRows.clear();
        return true;
    }

    bool ProbeListImpl::fail(const char* why)
    {
        ++mFailed;
        mLastFailure = why;
        mPrepared = ProbePrepared();
        return false;
    }

    bool ProbeListImpl::refuseSegment(const char* why)
    {
        ++mSegmentsRefused;
        mLastSegmentRefusal = why;
        return false;
    }

    bool ProbeListImpl::prepare(const Face& face)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        mPrepared = ProbePrepared();
        if (!ready() || !tables() || !face.blocks || !face.block_count)
        {
            return fail("not ready");
        }
        const LLVKDevice::GeneratedCommands* dgc = LLVKDevice::instance().generatedCommands();
        if (face.generated && !dgc)
        {
            return fail("no device generated commands");
        }
        beginFrame();
        mUsed = true;
        LLVKMirror& mirror = LLVKMirror::instance();
        const U32 frame = LLVKTracker::frame();

        // The draw run the face's first batch went into, and the words the
        // pass's draws push for what a batch does not name.
        LLVKGLProbe::Run run;
        LLVKGLProbe::Words words;
        if (!LLVKGLProbe::lastRun(run))
        {
            return fail("the first batch is not a draw run's last draw");
        }
        if (!LLVKGLProbe::templateWords(words) || words.matrix_word == U32_MAX || !words.count)
        {
            return fail("the program's words");
        }

        // The palette table rows of this frame (the list's share).
        if (mRowVec4sFrame != frame)
        {
            mRowVec4sFrame = frame;
            mRowVec4s = 0;
        }
        VkBuffer palette = LLVKBindless::paletteBuffer();
        if (!palette)
        {
            return fail("the frame's palette table");
        }

        // The face's batches in order: each block's range, at its place in the
        // order, and where its material rows are in the palette table.
        static std::vector<U32> block_list;     // first row, count, first batch, material rows
        block_list.clear();
        U32 total = 0;
        U32 model_used[MAX_MODELS] = {};
        U32 stream_location[PROBE_TOKENS] = {};
        U32 stream_stride[PROBE_TOKENS] = {};
        U32 stream_key[PROBE_TOKENS] = {};      // research/293 branch 1: the pool stream a slot reads, and the bytes in it
        U32 stream_shift[PROBE_TOKENS] = {};
        U32 streams = 0;
        const U32 program_changes = LLVKGLProbe::programChanges();
        for (U32 i = 0; i < face.block_count; ++i)
        {
            const U32 slot = face.blocks[i] - 1;
            if (!face.blocks[i] || slot >= mBlocks.size() || !mBlocks[slot].used || mBlocks[slot].damaged ||
                mBlocks[slot].model >= MAX_MODELS)
            {
                return fail("a block is gone");
            }
            ProbeBlock& b = mBlocks[slot];
            if (b.captured && (b.program != run.program || b.program_changes != program_changes ||
                               b.material_vec4s != (words.material_word != U32_MAX ? words.material_vec4s : 0)))
            {
                return fail("a block of another program");
            }
            if (b.captured && b.consts_hash != words.consts_hash)
            {
                return fail("constants other than the block's");
            }
            U32 material_base = 0;
            if (!b.materials.empty())
            {
                // Copied once a frame (the palette table is the frame's).
                if (b.material_frame != frame)
                {
                    const U32 vec4s = (U32)(b.materials.size() / 4);
                    U32 base = 0;
                    F32* dst = mRowVec4s + vec4s <= PROBE_ROW_VEC4S_PER_FRAME ? LLVKBindless::paletteRows(vec4s, base) : nullptr;
                    if (!dst)
                    {
                        return fail("the frame's palette table");
                    }
                    mRowVec4s += vec4s;
                    memcpy(dst, b.materials.data(), b.materials.size() * sizeof(F32));
                    b.material_frame = frame;
                    b.material_base = base;
                }
                material_base = b.material_base;
            }
            block_list.push_back(b.first);
            block_list.push_back(b.count);
            block_list.push_back(total);
            block_list.push_back(material_base);
            total += b.count;
            model_used[b.model] = 1;
            if (face.generated)
            {
                // The pool streams the face's batches read: a token slot each.
                for (const auto& s : b.streams)
                {
                    U32 j = 0;
                    while (j < streams && stream_location[j] != s.location)
                    {
                        ++j;
                    }
                    if (j == streams)
                    {
                        if (streams == PROBE_TOKENS)
                        {
                            return fail("more vertex streams than tokens");
                        }
                        stream_location[streams] = s.location;
                        stream_stride[streams] = s.stride;
                        stream_key[streams] = s.key;
                        stream_shift[streams] = s.shift;
                        ++streams;
                    }
                    else if (stream_stride[j] != s.stride)
                    {
                        return fail("a vertex stream with two strides");
                    }
                    else if (stream_key[j] != s.key || stream_shift[j] != s.shift)
                    {
                        return fail("a vertex stream read from two arrays");
                    }
                }
            }
        }
        const U32 vec4s = (words.count + 3) / 4;
        if (!total || total > PROBE_MAX_ROWS)
        {
            return fail("no batches");
        }

        ProbeView view = {};
        memcpy(view.sides, face.sides, sizeof(view.sides));
        view.origin[0] = face.origin[0];
        view.origin[1] = face.origin[1];
        view.origin[2] = face.origin[2];
        // A matrix row per model the blocks use, with the face's matrices.
        for (U32 m = 0; m < mModelSpan; ++m)
        {
            if (!model_used[m])
            {
                continue;
            }
            const LLRHI::ProbeList::Model* given = nullptr;
            for (U32 k = 0; k < face.model_count && !given; ++k)
            {
                if (face.models[k].model == mModels[m].matrix)
                {
                    given = &face.models[k];
                }
            }
            U32 row = 0;
            if (!given || !mModels[m].matrix || !LLVKGLProbe::matrixRow(given->matrices, row))
            {
                return fail("a model's matrix row");
            }
            view.model_rows[m] = row;
            memcpy(view.models[m], mModels[m].matrix, sizeof(view.models[m]));
        }

        // The slot of every texture the blocks name, as their draws would resolve it.
        ++mPrepareSerial;
        mPrepareSerial = mPrepareSerial ? mPrepareSerial : 1;
        for (U32 i = 0; i < face.block_count; ++i)
        {
            for (U32 handle : mBlocks[face.blocks[i] - 1].handles)
            {
                if (mHandleStamp[handle] == mPrepareSerial)
                {
                    continue;
                }
                mHandleStamp[handle] = mPrepareSerial;
                bool readable = true;
                mHandleSlot[handle] = LLVKGLProbe::textureSlot(mHandles[handle].name, readable);
                if (!readable)
                {
                    LLVKGLProbe::flushSlots();
                    return fail("a texture is not in shader read layout");
                }
            }
        }
        LLVKGLProbe::flushSlots();

        // The rows of words in the frame's palette table, aligned for the descriptor.
        const U32 align = (U32)(mStorageAlign / 16);
        const U32 want = total * vec4s + align - 1;
        U32 base = 0;
        if (mRowVec4s + want > PROBE_ROW_VEC4S_PER_FRAME || !LLVKBindless::paletteRows(want, base))
        {
            return fail("the frame's palette table");
        }
        mRowVec4s += want;
        base = (base + align - 1) / align * align;

        // Device generated commands: per pool chunk, its index buffer token and
        // a vertex buffer token per slot (a chunk without a slot's stream takes
        // another chunk's: only batches of chunks with that stream read it).
        static std::vector<U32> stream_table;
        stream_table.clear();
        const U32 chunk_words = 4 + 4 * streams;
        U32 chunks = 0;
        if (face.generated)
        {
            chunks = llmin(LLVKAttribPool::chunkCount(), PROBE_MAX_CHUNKS);
            if (!chunks || !streams)
            {
                return fail("no pool chunks");
            }
            stream_table.assign((size_t)chunks * chunk_words, 0u);
            auto put = [](U32* at, VkDeviceAddress address, VkDeviceSize size, U32 last)
            {
                at[0] = (U32)(address & 0xffffffffull);
                at[1] = (U32)(address >> 32);
                at[2] = (U32)llmin(size, (VkDeviceSize)0xffffffffull);
                at[3] = last;
            };
            std::vector<U32> fallback(streams, U32_MAX);   // a chunk that has slot j's stream
            for (U32 c = 0; c < chunks; ++c)
            {
                U32* at = &stream_table[(size_t)c * chunk_words];
                VkDeviceAddress address = 0;
                VkDeviceSize size = 0;
                if (!LLVKAttribPool::indexAddress(c, address, size))
                {
                    return fail("a chunk's index buffer address");
                }
                put(at, address, size, (U32)VK_INDEX_TYPE_UINT32);
                for (U32 j = 0; j < streams; ++j)
                {
                    if (LLVKAttribPool::streamAddress(c, stream_key[j], stream_stride[j], address, size) && size > stream_shift[j])
                    {
                        put(at + 4 + 4 * j, address + stream_shift[j], size - stream_shift[j], stream_stride[j]);
                        fallback[j] = fallback[j] == U32_MAX ? c : fallback[j];
                    }
                }
            }
            for (U32 j = 0; j < streams; ++j)
            {
                if (fallback[j] == U32_MAX)
                {
                    return fail("a vertex stream in no chunk");
                }
                const U32* from = &stream_table[(size_t)fallback[j] * chunk_words + 4 + 4 * j];
                for (U32 c = 0; c < chunks; ++c)
                {
                    U32* at = &stream_table[(size_t)c * chunk_words + 4 + 4 * j];
                    if (!at[0] && !at[1])
                    {
                        memcpy(at, from, 16);
                    }
                }
            }
        }
        // Sequence: the tokens, then the indexed draw (20 bytes), padded to 16 bytes.
        const U32 stride = face.generated ? (chunk_words + 5 + 3) / 4 * 4 : 5;

        for (U32 i = 0; i < words.count; ++i)
        {
            view.words[i] = words.words[i];
        }
        for (U32 i = 0; i < LLVKGLProbe::Words::MAX; ++i)
        {
            view.units[i] = i < words.samplers ? words.units[i] : -1;
        }
        view.params[0] = total;
        view.params[1] = face.block_count;
        view.params[2] = words.count;
        view.params[3] = vec4s;
        view.params2[0] = words.matrix_word;
        view.params2[1] = words.samplers;
        view.params2[2] = base;
        view.params2[3] = (U32)mHandles.size();
        view.params3[0] = stride;
        view.params3[1] = face.generated ? 1 : 0;
        view.params3[2] = face.generated ? streams : 0;
        view.params3[3] = words.material_word;
        view.params4[0] = chunks;

        // Inputs in the frame's ring, output in this frame slot's scratch.
        ProbeFrameSlot& fslot = frameSlot();
        VkCommandBuffer cmd = mirror.uploadCommands();
        if (!cmd)
        {
            return fail("no frame being recorded");
        }
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize view_offset = 0, blocks_offset = 0, slots_offset = 0, streams_offset = 0;
        void* dst = nullptr;
        if (!mirror.allocate(sizeof(ProbeView), mStorageAlign, ring, view_offset, dst))
        {
            return fail("the frame's upload ring");
        }
        memcpy(dst, &view, sizeof(ProbeView));
        const VkDeviceSize blocks_bytes = (VkDeviceSize)block_list.size() * 4;
        if (!mirror.allocate(blocks_bytes, mStorageAlign, ring, blocks_offset, dst))
        {
            return fail("the frame's upload ring");
        }
        memcpy(dst, block_list.data(), (size_t)blocks_bytes);
        const VkDeviceSize slots_bytes = (VkDeviceSize)llmax((U32)mHandleSlot.size(), 1u) * 4;
        if (!mirror.allocate(slots_bytes, mStorageAlign, ring, slots_offset, dst))
        {
            return fail("the frame's upload ring");
        }
        memset(dst, 0, (size_t)slots_bytes);
        if (!mHandleSlot.empty())
        {
            memcpy(dst, mHandleSlot.data(), mHandleSlot.size() * 4);
        }
        // The stream table (G5's draws do not read it: a word stands in).
        const VkDeviceSize streams_bytes = (VkDeviceSize)llmax((U32)stream_table.size(), 4u) * 4;
        if (!mirror.allocate(streams_bytes, mStorageAlign, ring, streams_offset, dst))
        {
            return fail("the frame's upload ring");
        }
        memset(dst, 0, (size_t)streams_bytes);
        if (!stream_table.empty())
        {
            memcpy(dst, stream_table.data(), stream_table.size() * 4);
        }
        const VkDeviceSize command_bytes = (VkDeviceSize)total * stride * 4;
        VkBuffer output = VK_NULL_HANDLE;
        VkDeviceSize output_offset = 0;
        if (!scratch(fslot, command_bytes, output, output_offset))
        {
            return fail("no memory for the commands");
        }
        VkDescriptorSet set = allocateSet(fslot);
        if (!set)
        {
            return fail("no descriptor set");
        }
        VkDescriptorBufferInfo infos[PROBE_BINDINGS] = {
            { mRowTable.buffer, 0, (VkDeviceSize)llmax(mRowSpan, 1u) * sizeof(GpuProbeRow) },
            { ring, blocks_offset, blocks_bytes },
            { ring, slots_offset, slots_bytes },
            { ring, view_offset, sizeof(ProbeView) },
            { output, output_offset, command_bytes },
            { palette, (VkDeviceSize)base * 16, (VkDeviceSize)total * vec4s * 16 },
            { ring, streams_offset, streams_bytes },
        };
        VkWriteDescriptorSet writes[PROBE_BINDINGS] = {};
        for (U32 i = 0; i < PROBE_BINDINGS; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), PROBE_BINDINGS, writes, 0, nullptr);

        // Into the upload commands, which run before every draw of the frame:
        // they leave the open run's draws where they are (forgiveUploads).
        const U64 before = LLVKGLProbe::commands();
        if (!flush(cmd))
        {
            return fail("the frame's upload ring");
        }
        {
            // The row writes (and earlier uploads) before the dispatch reads them.
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, (total + LOCAL_SIZE - 1) / LOCAL_SIZE, 1, 1);
        {
            // The commands to the indirect draws (and, with generated commands,
            // to their preprocessing, which reads the sequences in the execute),
            // the rows to the shaders that read the palette table; row writes
            // recorded later wait until the dispatch has read the table.
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT;
            if (dgc)
            {
                barrier.dstStageMask |= VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT;
                barrier.dstAccessMask |= VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT;
            }
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        LLVKGLProbe::forgiveUploads(before);

        mPrepared.valid = true;
        mPrepared.frame = frame;
        mPrepared.run = run;
        mPrepared.commands = output;
        mPrepared.offset = output_offset;
        mPrepared.count = total;
        mPrepared.drops = mDrops;
        mPrepared.generated = face.generated;
        mPrepared.stride = stride;
        mPrepared.slots = face.generated ? streams : 0;
        for (U32 j = 0; j < mPrepared.slots; ++j)
        {
            mPrepared.slot_location[j] = stream_location[j];
            mPrepared.slot_stride[j] = stream_stride[j];
        }
        ++mFaces;
        mGeneratedFaces += face.generated ? 1 : 0;
        mBatches += total;
        return true;
    }

    bool ProbeListImpl::drawRun(U32 first, U32 count, U32 chunk, U64 format)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const ProbePrepared& p = mPrepared;
        // No pool copy dropped since prepare: the blocks still draw what GL has.
        if (!p.valid || p.generated || p.frame != LLVKTracker::frame() || p.drops != mDrops || !count ||
            (U64)first + count > p.count)
        {
            ++mRunsRefused;
            return false;
        }
        LLVKGLProbe::Run run;
        if (!LLVKGLProbe::lastRun(run) || run.program != p.run.program || run.chunk != chunk || run.format != format ||
            !LLVKGLProbe::drawIndirect(run, p.commands, p.offset + (VkDeviceSize)first * sizeof(VkDrawIndexedIndirectCommand),
                                       count))
        {
            ++mRunsRefused;
            return false;
        }
        ++mIndirect;
        return true;
    }

    // Research/50 step 4: the segment's other batches by one execute, after
    // its first batch went out as a draw run (with this segment's format).
    bool ProbeListImpl::drawSegment(U32 first, U32 count, U64 format)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const ProbePrepared& p = mPrepared;
        const LLVKDevice::GeneratedCommands* dgc = LLVKDevice::instance().generatedCommands();
        // No pool copy dropped since prepare: the blocks still draw what GL has.
        if (!dgc || !p.valid || !p.generated || p.frame != LLVKTracker::frame() || p.drops != mDrops || !count ||
            (U64)first + count > p.count || count > dgc->properties.maxIndirectSequenceCount)
        {
            return refuseSegment("not prepared");
        }
        LLVKGLProbe::Run run;
        if (!LLVKGLProbe::lastRun(run) || run.program != p.run.program || run.format != format)
        {
            return refuseSegment("the segment's first batch is not a draw run's last draw");
        }
        // The run's pipeline with dynamic strides, and a token slot for each pool stream it binds.
        VkPipeline pipeline = VK_NULL_HANDLE;
        LLVKGLProbe::Streams streams;
        if (!LLVKGLProbe::generatedPipeline(run, pipeline, streams))
        {
            return refuseSegment("no pipeline for generated commands");
        }
        ProbeLayoutKey key;
        key.stride = p.stride;
        key.slots = p.slots;
        for (U32 j = 0; j < PROBE_TOKENS; ++j)
        {
            key.binding_of_slot[j] = U32_MAX;
        }
        for (U32 k = 0; k < streams.count; ++k)
        {
            U32 j = 0;
            while (j < p.slots && (p.slot_location[j] != streams.location[k] || p.slot_stride[j] != streams.stride[k]))
            {
                ++j;
            }
            if (j == p.slots || key.binding_of_slot[j] != U32_MAX)
            {
                return refuseSegment("a vertex stream without a token slot");
            }
            key.binding_of_slot[j] = streams.binding[k];
        }
        const VkIndirectCommandsLayoutEXT layout = layoutFor(key);
        if (!layout)
        {
            return refuseSegment("no indirect commands layout");
        }
        VkDevice device = LLVKDevice::instance().getDevice();
        // Preprocess memory for this execute (the execute preprocesses: not explicit).
        VkGeneratedCommandsPipelineInfoEXT bound = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT };
        bound.pipeline = pipeline;
        VkGeneratedCommandsMemoryRequirementsInfoEXT need_info = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT };
        need_info.pNext = &bound;
        need_info.indirectExecutionSet = VK_NULL_HANDLE;
        need_info.indirectCommandsLayout = layout;
        need_info.maxSequenceCount = count;
        need_info.maxDrawCount = 0;
        VkMemoryRequirements2 need = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        dgc->getMemoryRequirements(device, &need_info, &need);
        VkDeviceAddress preprocess = 0;
        if (need.memoryRequirements.size && !preprocessMemory(frameSlot(), need.memoryRequirements, preprocess))
        {
            return refuseSegment("no preprocess memory");
        }
        VkBufferDeviceAddressInfo where = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        where.buffer = p.commands;
        const VkDeviceAddress commands = vkGetBufferDeviceAddress(device, &where);
        if (!commands)
        {
            return refuseSegment("no address for the sequences");
        }
        VkGeneratedCommandsInfoEXT info = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT };
        info.shaderStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        info.indirectExecutionSet = VK_NULL_HANDLE;
        info.indirectCommandsLayout = layout;
        info.indirectAddress = commands + p.offset + (VkDeviceSize)first * p.stride * 4;
        info.indirectAddressSize = (VkDeviceSize)count * p.stride * 4;
        info.preprocessAddress = preprocess;
        info.preprocessSize = need.memoryRequirements.size;
        info.maxSequenceCount = count;
        info.sequenceCountAddress = 0;
        info.maxDrawCount = 0;
        if (!LLVKGLProbe::executeGenerated(run, pipeline, info))
        {
            return refuseSegment("the run changed");
        }
        ++mExecutes;
        mExecuted += count;
        return true;
    }

    std::string ProbeListImpl::takeStats(U32 frames)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!mUsed)
        {
            return std::string();
        }
        frames = llmax(frames, 1u);
        std::ostringstream out;
        out << "probe list blocks " << mLiveBlocks << " (batches " << mLiveRows << ", span " << mRowSpan << "), faces "
            << mFaces / frames << " a frame, batches " << mBatches / frames << ", indirect draws " << mIndirect / frames
            << ", runs refused " << mRunsRefused << ", blocks added " << mAdded << ", refused " << mRefused << ", removed "
            << mRemoved << ", dropped by GL writes " << mDamaged << ", prepares failed " << mFailed;
        if (mFailed && mLastFailure)
        {
            out << " (last: " << mLastFailure << ")";
        }
        out << "; generated: faces " << mGeneratedFaces / frames << " a frame, executes " << mExecutes / frames << " (batches "
            << mExecuted / frames << "), segments refused " << mSegmentsRefused;
        if (mSegmentsRefused && mLastSegmentRefusal)
        {
            out << " (last: " << mLastSegmentRefusal << ")";
        }
        out << ", captured blocks added " << mCapturedAdded << ", refused " << mCaptureRefused;
        if (mCaptureRefused && mLastCaptureRefusal)
        {
            out << " (last: " << mLastCaptureRefusal << ")";
        }
        out << ", textures deleted " << mTexturesDeleted << ", layouts " << mLayouts.size();
        mFaces = mBatches = mIndirect = mRunsRefused = mAdded = mRefused = mDamaged = mRemoved = mFailed = 0;
        mGeneratedFaces = mExecutes = mExecuted = mSegmentsRefused = mCapturedAdded = mCaptureRefused = mTexturesDeleted = 0;
        mLastFailure = mLastSegmentRefusal = mLastCaptureRefusal = nullptr;
        return out.str();
    }
}

void LLVKGPUList::installProbe()
{
    // Nothing is made yet: ready() builds the shader and pipeline on first use.
    LLRHI::sProbeList = &sProbeListImpl;
}

std::string LLVKGPUList::takeProbeStats(U32 frames)
{
    return sProbeListImpl.takeStats(frames);
}
