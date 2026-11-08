/**
 * @file llvkavatar.cpp
 * @brief Stage 4b (research/199): the resident avatar palette rows and the
 * compute skinning check.
 *
 * Resident rows (LLRHI_AVATAR_PALETTE): the palette table of each frame slot
 * (LLVKBindless, set 0 binding 2) keeps a region at its head whose rows keep
 * their number. Each avatar x skin palette gets one for its life; the viewer
 * writes its joints there once per build, into the table of the frame being
 * recorded, and (on) names that row wherever it named the frame's rolling row
 * before. Verify keeps the hash of every row written and reads the rows again
 * at the next frame's head, before anything writes the region again: a row
 * that changed was stepped on.
 *
 * Compute skinning check (LLRHI_COMPUTE_SKIN=verify): a few rigged faces a
 * frame are skinned by a compute kernel that reads the palette table the
 * draws read, at the row they read, and the result is compared with the CPU
 * (LLRHIAvatar::skinPosition, the shaders' formula) when the frame slot comes
 * round again (its fence waited for).
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

#include "llvkavatar.h"

#include "llrhiavatar.h"
#include "llrhihash.h"
#include "llrhiscene.h"         // research/219: a rigged record names its row's base
#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"

#include <array>

// research/219: a scene record holds a resident row's base in 24 bits of its
// flags, and "no row" is the same value on both sides.
static_assert((U64)LLRHIAvatar::SlotRule::MAX_ROWS * llvkgl::PREBUILT_PALETTE_ROOM <= LLRHI::SCENE_PALETTE_LIMIT,
              "a resident palette row's base must fit a scene record's 24 bits (research/219 3.1)");
static_assert(LLRHIAvatar::SLOT_NONE == LLRHI::SCENE_NO_PALETTE, "no resident row is no record palette");

namespace
{
    // ------------------------------------------------------------------
    // The compute skinning kernel: getObjectSkinnedTransform
    // (objectSkinV.glsl) applied to the position, the palette read from the
    // palette table at the draws' row (3 vec4s per joint, each a column of
    // the mat3x4: xyz the 3x3 column, w that row's translation). Positions and
    // weights are vec4 arrays (LLVector4a), as the attribute pool's streams
    // hold them (llvkattribpool.h), so stage 1 can bind those instead.
    const char* SKIN_SOURCE = R"GLSL(
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer Positions { vec4 positions[]; };
layout(std430, set = 0, binding = 1) readonly buffer Weights { vec4 weights[]; };
layout(std430, set = 0, binding = 2) readonly buffer Palette { vec4 palette[]; };
layout(std430, set = 0, binding = 3) writeonly buffer Skinned { vec4 skinned[]; };
layout(push_constant) uniform Params
{
    uint count;
    uint palette_base;
    uint out_base;
    uint pad;
} params;

const int MAX_JOINTS_PER_MESH_OBJECT = 110;

mat3 joint_mat(int j)
{
    return mat3(palette[j].xyz, palette[j + 1].xyz, palette[j + 2].xyz);
}

vec3 joint_trans(int j)
{
    return vec3(palette[j].w, palette[j + 1].w, palette[j + 2].w);
}

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= params.count)
    {
        return;
    }
    vec4 weight4 = weights[i];
    vec4 w = fract(weight4);
    vec4 index = floor(weight4);
    // A NaN weight (a broken mesh) takes joint 0: min and max may pass a NaN
    // on, and ivec4 of one is undefined (security audit B low 9).
    index = mix(index, vec4(0.0), isnan(index));
    index = min(index, vec4(MAX_JOINTS_PER_MESH_OBJECT - 1));
    index = max(index, vec4(0.0));
    w *= 1.0 / (w.x + w.y + w.z + w.w);
    ivec4 j = ivec4(index) * 3 + int(params.palette_base);

    mat3 mat = joint_mat(j.x) * w.x;
    mat += joint_mat(j.y) * w.y;
    mat += joint_mat(j.z) * w.z;
    mat += joint_mat(j.w) * w.w;
    vec3 trans = joint_trans(j.x) * w.x;
    trans += joint_trans(j.y) * w.y;
    trans += joint_trans(j.z) * w.z;
    trans += joint_trans(j.w) * w.w;

    skinned[params.out_base + i] = vec4(mat * positions[i].xyz + trans, 1.0);
}
)GLSL";

    struct SkinParams
    {
        U32 count;
        U32 palette_base;
        U32 out_base;
        U32 pad;
    };

    constexpr U32 LOGGED_MISMATCHES = 8;
    constexpr U32 CHECKS_PER_FRAME = 64;        // resident rows read again a frame at most (host reads of write-combined memory)
    constexpr F64 REPORT_SECONDS = 5.0;

    U64 hash_words(const F32* data, U32 vec4s)
    {
        // FNV-1a over the 32-bit words.
        return LLRHI::fnv1aFloats(LLRHI::FNV1A_BASIS, data, vec4s * 4);
    }

    class Backend final : public LLRHIAvatar::Backend
    {
    public:
        U32 rowRoom() const override { return llvkgl::PREBUILT_PALETTE_ROOM; }

        U32 allocateRow(U64 owner) override
        {
            std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
            frame_head();
            const U32 row = mRule.allocate(owner);
            if (row == LLRHIAvatar::SlotRule::NONE)
            {
                ++mAllocFailed;
                if (mRule.wantedRows() > mRule.rows)
                {
                    LLVKBindless::setPaletteReserve(mRule.wantedRows() * rowRoom());
                }
                return LLRHIAvatar::SLOT_NONE;
            }
            return row * rowRoom();
        }

        void releaseRow(U32 base, U64 owner) override
        {
            std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
            frame_head();
            if (base % rowRoom() || !mRule.release(base / rowRoom(), owner))
            {
                note_mismatch("release of a row not the owner's", base, owner);
            }
        }

        bool writeRow(U32 base, U64 owner, const F32* data, U32 vec4s, LLRHI::PaletteRow& row) override
        {
            std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
            frame_head();
            const U32 room = rowRoom();
            if (base % room || !mRule.owns(base / room, owner))
            {
                note_mismatch("write into a row not the owner's", base, owner);
                return false;
            }
            if (!data || !vec4s || vec4s > room || !LLVKMirror::instance().isRecording())
            {
                return false;
            }
            const U32 table = LLVKBindless::paletteTableSlot();
            F32* dst = LLVKBindless::paletteReservedRows(table, base, room);     // the whole room: readers may index all of it
            if (!dst)
            {
                ++mWriteRefused;
                return false;
            }
            memcpy(dst, data, (size_t)vec4s * 16);
            row.base = base;
            row.room = room;
            row.frame = llvkgl::sFrame;
            ++mWrites;
            if (LLRHIAvatar::paletteWrites() && !LLRHIAvatar::paletteDraws())
            {
                // verify: read again at the next frame's head
                mWritten.push_back({ base, vec4s, owner, hash_words(data, vec4s), table });
            }
            return true;
        }

        // research/219: what writeRow puts in row.frame.
        U32 frame() const override
        {
            std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
            return llvkgl::sFrame;
        }

        // research/219 (verify): the row as this frame's table holds it (the
        // host's copy of write-combined memory: a few rows a check at most).
        bool readRow(U32 base, U32 vec4s, F32* out) const override
        {
            std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
            if (!out || !vec4s || vec4s > rowRoom() || !LLVKMirror::instance().isRecording())
            {
                return false;
            }
            const F32* rows = LLVKBindless::paletteReservedRows(LLVKBindless::paletteTableSlot(), base, vec4s);
            if (!rows)
            {
                return false;
            }
            memcpy(out, rows, (size_t)vec4s * 16);
            return true;
        }

        bool skinVerify(const LLRHIAvatar::SkinVerifyJob& job) override;

    private:
        struct Written
        {
            U32 base;
            U32 vec4s;
            U64 owner;
            U64 hash;
            U32 table;
        };

        struct Pending
        {
            std::vector<F32> expected;      // 3 per vertex; NaN: not compared (index past the joints)
            U32 vertices = 0;
            U32 out_base = 0;
            U64 label = 0;
        };

        struct SkinSlot
        {
            U64 head = ~0ull;               // frame head its jobs were recorded at
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkBuffer output = VK_NULL_HANDLE;
            VmaAllocation output_memory = VK_NULL_HANDLE;
            F32* output_data = nullptr;
            std::vector<Pending> pending;
        };

        void frame_head();
        void check_written();
        bool skin_ready();
        bool make_skin_slot(SkinSlot& slot);
        void read_skin_results(SkinSlot& slot);
        void note_mismatch(const char* what, U32 base, U64 owner);
        void report();

        LLRHIAvatar::SlotRule mRule;
        U64 mHead = ~0ull;                  // LLVKMirror::frameBegins last seen
        std::vector<Written> mWritten;      // this frame's writes (verify)
        std::vector<Written> mToCheck;      // the frame before's, checked at this head
        U64 mWrittenHead = ~0ull;
        size_t mCheckCursor = 0;

        // counts since the last report
        U32 mFrames = 0;
        U32 mWrites = 0;
        U32 mWriteRefused = 0;
        U32 mAllocFailed = 0;
        U32 mChecked = 0;
        U32 mMismatches = 0;
        U32 mSkinJobs = 0;
        U32 mSkinVertices = 0;
        U32 mSkinCompared = 0;
        U32 mSkinOutside = 0;
        U32 mSkinMismatchJobs = 0;
        U32 mSkinMismatchVertices = 0;
        U32 mSkinRefused = 0;
        U32 mLoggedMismatches = 0;
        U32 mLoggedSkinMismatches = 0;
        std::chrono::steady_clock::time_point mReportStart = std::chrono::steady_clock::now();

        // the kernel
        bool mSkinTried = false;
        bool mSkinBuilt = false;
        LLVKShader::Program mSkinProgram;
        VkDescriptorSetLayout mSkinSets = VK_NULL_HANDLE;
        VkPipelineLayout mSkinLayout = VK_NULL_HANDLE;
        VkPipeline mSkinPipeline = VK_NULL_HANDLE;
        std::array<SkinSlot, 2> mSkinSlots;
        U32 mJobsThisFrame = 0;

        friend bool bake_skin_kernel();
    };

    Backend sBackend;

    // At the first call of a new frame (LLVKMirror::beginFrame moved
    // frameBegins; LLVKBindless::beginFrame has applied the region's size):
    // the frame before's writes are read again (verify), released rows that
    // waited long enough are free, the region asks to grow if it must, and
    // the frame slot's compute checks (done: its fence was waited for) are read.
    void Backend::frame_head()
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        const U64 head = mirror.frameBegins();
        if (head == mHead)
        {
            return;
        }
        // Verify: only the writes of the frame just before (their table is the
        // other slot, which no frame head since has made again; nothing has
        // written the region in this frame yet).
        mToCheck.clear();
        if (mWrittenHead != ~0ull && mWrittenHead + 1 == head)
        {
            mToCheck.swap(mWritten);
        }
        mWritten.clear();
        mWrittenHead = head;
        if (mirror.isRecording())
        {
            check_written();
        }
        mToCheck.clear();

        mRule.frameHead(head, LLVKBindless::paletteReserved() / rowRoom());
        if (mRule.wantedRows() > mRule.rows)
        {
            LLVKBindless::setPaletteReserve(mRule.wantedRows() * rowRoom());
        }
        mHead = head;
        ++mFrames;

        mJobsThisFrame = 0;
        SkinSlot& slot = mSkinSlots[mirror.getSlot() & 1];
        // Only while frames are recorded (not after the device is gone: rows
        // are still given back then, by avatars destroyed late).
        if (slot.head != ~0ull && slot.head != head && mirror.isRecording())
        {
            read_skin_results(slot);
            if (slot.pool)
            {
                vkResetDescriptorPool(LLVKDevice::instance().getDevice(), slot.pool, 0);
            }
            slot.head = ~0ull;
        }

        if (std::chrono::duration<F64>(std::chrono::steady_clock::now() - mReportStart).count() >= REPORT_SECONDS)
        {
            report();
        }
    }

    void Backend::check_written()
    {
        if (mToCheck.empty())
        {
            return;
        }
        U32 checked = 0;
        U32 mismatches = 0;
        const size_t count = mToCheck.size();
        const size_t take = llmin(count, (size_t)CHECKS_PER_FRAME);
        for (size_t k = 0; k < take; ++k)
        {
            const Written& w = mToCheck[(mCheckCursor + k) % count];
            const F32* rows = LLVKBindless::paletteReservedRows(w.table, w.base, w.vec4s);
            ++checked;
            if (!rows || hash_words(rows, w.vec4s) != w.hash)
            {
                ++mismatches;
                note_mismatch(rows ? "a resident row changed after its write" : "a resident row no longer in its table", w.base,
                              w.owner);
            }
        }
        mCheckCursor += take;
        mChecked += checked;
        LLRHIAvatar::notePaletteChecks(checked, mismatches);
        mToCheck.clear();
    }

    void Backend::note_mismatch(const char* what, U32 base, U64 owner)
    {
        ++mMismatches;
        if (mLoggedMismatches < LOGGED_MISMATCHES)
        {
            ++mLoggedMismatches;
            LL_WARNS("RHIAvatar") << "avatar palette rows mismatch: " << what << " (row " << base / rowRoom() << ", owner "
                                  << std::hex << owner << std::dec << ")" << LL_ENDL;
        }
        if (LLRHIAvatar::paletteWrites() && !LLRHIAvatar::paletteDraws())
        {
            LLRHIAvatar::notePaletteChecks(1, 1);   // auto: off at once
        }
    }

    void Backend::report()
    {
        const F64 per = 1.0 / llmax(mFrames, 1u);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        if (LLRHIAvatar::paletteMode() != LLRHIAvatar::MODE_OFF)
        {
            out << "RHIAvatarPalette (research/199): " << LLRHIAvatar::modeName(LLRHIAvatar::paletteMode()) << " now "
                << (LLRHIAvatar::paletteDraws() ? "on" : LLRHIAvatar::paletteWrites() ? "verify" : "off") << ", rows "
                << (mRule.rows - (U32)mRule.free_rows.size() - (U32)mRule.waiting.size()) << " / " << mRule.rows << " (region "
                << LLVKBindless::paletteReserved() << " vec4s)"
                << ", writes/frame " << mWrites * per << ", refused " << mWriteRefused << ", no free row " << mAllocFailed
                << ", checked " << mChecked << ", mismatches " << mMismatches;
            LL_INFOS("RHIAvatar") << out.str() << LL_ENDL;
            out.str("");
        }
        if (LLRHIAvatar::computeSkinMode() != LLRHIAvatar::MODE_OFF)
        {
            out << "RHIComputeSkin (research/199): " << (mSkinBuilt ? "verify" : mSkinTried ? "kernel failed" : "no job yet")
                << ", jobs " << mSkinJobs << ", vertices " << mSkinVertices << ", compared " << mSkinCompared
                << ", out of range " << mSkinOutside << ", mismatched jobs " << mSkinMismatchJobs << " (vertices "
                << mSkinMismatchVertices << "), refused " << mSkinRefused;
            LL_INFOS("RHIAvatar") << out.str() << LL_ENDL;
        }
        mFrames = 0;
        mWrites = mWriteRefused = mAllocFailed = mChecked = mMismatches = 0;
        mSkinJobs = mSkinVertices = mSkinCompared = mSkinOutside = mSkinMismatchJobs = mSkinMismatchVertices = mSkinRefused = 0;
        mReportStart = std::chrono::steady_clock::now();
    }

    // ------------------------------------------------------------------
    // The compute skinning check

    bool Backend::skin_ready()
    {
        if (mSkinTried)
        {
            return mSkinBuilt;
        }
        mSkinTried = true;
        VkDevice device = LLVKDevice::instance().getDevice();
        VkDescriptorSetLayoutBinding bindings[4] = {};
        for (U32 i = 0; i < 4; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 4;
        set_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &mSkinSets) != VK_SUCCESS)
        {
            mSkinSets = VK_NULL_HANDLE;
            return false;
        }
        VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinParams) };
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &mSkinSets;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &mSkinLayout) != VK_SUCCESS)
        {
            mSkinLayout = VK_NULL_HANDLE;
            return false;
        }
        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, std::string("#version 450\n") + SKIN_SOURCE } }, mSkinProgram) ||
            mSkinProgram.modules.empty())
        {
            LL_WARNS("RHIAvatar") << "compute skinning kernel failed: " << mSkinProgram.log << LL_ENDL;
            return false;
        }
        VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = mSkinProgram.modules[0];
        info.stage.pName = "main";
        info.layout = mSkinLayout;
        if (vkCreateComputePipelines(device, LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &mSkinPipeline) != VK_SUCCESS)
        {
            mSkinPipeline = VK_NULL_HANDLE;
            return false;
        }
        mSkinBuilt = true;
        return true;
    }

    bool Backend::make_skin_slot(SkinSlot& slot)
    {
        if (slot.pool && slot.output)
        {
            return true;
        }
        VkDevice device = LLVKDevice::instance().getDevice();
        if (!slot.pool)
        {
            const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, LLRHIAvatar::SKIN_JOBS_PER_FRAME * 4 };
            VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            info.maxSets = LLRHIAvatar::SKIN_JOBS_PER_FRAME;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;
            if (vkCreateDescriptorPool(device, &info, nullptr, &slot.pool) != VK_SUCCESS)
            {
                slot.pool = VK_NULL_HANDLE;
                return false;
            }
        }
        if (!slot.output)
        {
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = (VkDeviceSize)LLRHIAvatar::SKIN_JOBS_PER_FRAME * LLRHIAvatar::SKIN_VERTICES_PER_JOB * 16;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo alloc = {};
            alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped = {};
            if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &alloc, &slot.output, &slot.output_memory, &mapped) != VK_SUCCESS ||
                !mapped.pMappedData)
            {
                if (slot.output)
                {
                    vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), slot.output, slot.output_memory);
                }
                slot.output = VK_NULL_HANDLE;
                slot.output_memory = VK_NULL_HANDLE;
                return false;
            }
            slot.output_data = (F32*)mapped.pMappedData;
        }
        return true;
    }

    bool Backend::skinVerify(const LLRHIAvatar::SkinVerifyJob& job)
    {
        std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
        frame_head();
        LLVKMirror& mirror = LLVKMirror::instance();
        const U32 room = rowRoom();
        if (!job.positions || !job.weights || !job.palette || !job.vertices || !job.joints || job.joints * 3 > room ||
            mJobsThisFrame >= LLRHIAvatar::SKIN_JOBS_PER_FRAME || !mirror.isRecording())
        {
            ++mSkinRefused;
            return false;
        }
        // The draws' row, of this frame (the GPU reads the table the frame binds).
        if (job.row_frame != llvkgl::sFrame)
        {
            ++mSkinRefused;
            return false;
        }
        VkBuffer palette = LLVKBindless::paletteBuffer();
        SkinSlot& slot = mSkinSlots[mirror.getSlot() & 1];
        // The slot's output and sets hold one frame's jobs: none while the
        // jobs of an earlier frame there have not been read (frame_head).
        if (slot.pending.size() >= LLRHIAvatar::SKIN_JOBS_PER_FRAME || (slot.head != ~0ull && slot.head != mHead))
        {
            ++mSkinRefused;
            return false;
        }
        if (!palette || !skin_ready() || !make_skin_slot(slot))
        {
            ++mSkinRefused;
            return false;
        }
        const U32 vertices = llmin(job.vertices, LLRHIAvatar::SKIN_VERTICES_PER_JOB);
        const VkDeviceSize bytes = (VkDeviceSize)vertices * 16;
        const VkDeviceSize align = llmax(LLVKDevice::instance().getLimits().minStorageBufferOffsetAlignment, (VkDeviceSize)16);
        VkBuffer pos_buffer = VK_NULL_HANDLE, weight_buffer = VK_NULL_HANDLE;
        VkDeviceSize pos_offset = 0, weight_offset = 0;
        void* pos_data = nullptr;
        void* weight_data = nullptr;
        if (!mirror.allocate(bytes, align, pos_buffer, pos_offset, pos_data) ||
            !mirror.allocate(bytes, align, weight_buffer, weight_offset, weight_data))
        {
            ++mSkinRefused;
            return false;
        }
        VkDescriptorSetAllocateInfo set_alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        set_alloc.descriptorPool = slot.pool;
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts = &mSkinSets;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDevice device = LLVKDevice::instance().getDevice();
        if (vkAllocateDescriptorSets(device, &set_alloc, &set) != VK_SUCCESS)
        {
            ++mSkinRefused;
            return false;
        }
        memcpy(pos_data, job.positions, (size_t)bytes);
        memcpy(weight_data, job.weights, (size_t)bytes);

        const U32 out_base = (U32)slot.pending.size() * LLRHIAvatar::SKIN_VERTICES_PER_JOB;
        const VkDescriptorBufferInfo infos[4] = {
            { pos_buffer, pos_offset, bytes },
            { weight_buffer, weight_offset, bytes },
            { palette, 0, VK_WHOLE_SIZE },
            { slot.output, 0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet writes[4] = {};
        for (U32 i = 0; i < 4; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);

        // The CPU's answer, from the joints as built (not the table): a row
        // that does not hold them shows as a mismatch.
        Pending pending;
        pending.vertices = vertices;
        pending.out_base = out_base;
        pending.label = job.label;
        pending.expected.resize((size_t)vertices * 3);
        for (U32 v = 0; v < vertices; ++v)
        {
            F32* e = &pending.expected[(size_t)v * 3];
            if (!LLRHIAvatar::skinPosition(job.palette, job.joints, job.weights + (size_t)v * 4, job.positions + (size_t)v * 4, e))
            {
                e[0] = e[1] = e[2] = std::numeric_limits<F32>::quiet_NaN();
            }
        }

        const SkinParams params = { vertices, job.row_base, out_base, 0 };
        // Asked for only now: handing it out moves the mirror's record serial.
        VkCommandBuffer cmd = mirror.uploadCommands();
        if (!cmd)
        {
            ++mSkinRefused;
            return false;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSkinPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSkinLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, mSkinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(cmd, (vertices + 63) / 64, 1, 1);
        // The results to the host, read once the slot's fence has been waited for.
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        slot.pending.push_back(std::move(pending));
        slot.head = mHead;
        ++mJobsThisFrame;
        ++mSkinJobs;
        mSkinVertices += vertices;
        return true;
    }

    void Backend::read_skin_results(SkinSlot& slot)
    {
        if (slot.pending.empty() || !slot.output_data)
        {
            slot.pending.clear();
            return;
        }
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), slot.output_memory, 0, VK_WHOLE_SIZE);
        for (const Pending& p : slot.pending)
        {
            U32 bad = 0;
            U32 first_bad = U32_MAX;
            for (U32 v = 0; v < p.vertices; ++v)
            {
                const F32* e = &p.expected[(size_t)v * 3];
                if (std::isnan(e[0]))
                {
                    ++mSkinOutside;
                    continue;
                }
                const F32* g = slot.output_data + ((size_t)p.out_base + v) * 4;
                ++mSkinCompared;
                for (U32 c = 0; c < 3; ++c)
                {
                    const F32 tolerance = 1e-4f + 1e-5f * std::fabs(e[c]);
                    if (!(std::fabs(g[c] - e[c]) <= tolerance))     // NaN on the GPU's side fails too
                    {
                        if (first_bad == U32_MAX)
                        {
                            first_bad = v;
                        }
                        ++bad;
                        break;
                    }
                }
            }
            if (bad)
            {
                ++mSkinMismatchJobs;
                mSkinMismatchVertices += bad;
                if (mLoggedSkinMismatches < LOGGED_MISMATCHES)
                {
                    ++mLoggedSkinMismatches;
                    const F32* e = &p.expected[(size_t)first_bad * 3];
                    const F32* g = slot.output_data + ((size_t)p.out_base + first_bad) * 4;
                    LL_WARNS("RHIAvatar") << "compute skinning mismatch: job " << std::hex << p.label << std::dec << ", " << bad << " of "
                                          << p.vertices << " vertices, first " << first_bad << ": CPU (" << e[0] << ", " << e[1] << ", "
                                          << e[2] << ") GPU (" << g[0] << ", " << g[1] << ", " << g[2] << ")" << LL_ENDL;
                }
            }
        }
        slot.pending.clear();
    }

    // research/159 3.1: the kernel built ahead of its first use (shader bake).
    bool bake_skin_kernel()
    {
        std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
        return sBackend.skin_ready();
    }
}

void LLVKAvatar::install()
{
    LLVKShader::addBakeStep("avatar compute skinning (research/199)", &bake_skin_kernel);
    if (LLRHIAvatar::paletteMode() == LLRHIAvatar::MODE_OFF && LLRHIAvatar::computeSkinMode() == LLRHIAvatar::MODE_OFF)
    {
        return;
    }
    LLRHIAvatar::setBackend(&sBackend);
}
