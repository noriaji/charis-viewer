/**
 * @file llvkscene.cpp
 * @brief The retained scene on the GPU (research/197 stage 2): LLRHI::Scene for Vulkan.
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

#include "llvkscene.h"

#include "llrhiscene.h"
#include "llrhiframe.h"     // research/208 S0: the render thread census
#include "llvkattribpool.h"
#include "llvkbindless.h"
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvkrtas.h"        // research/230 R2
#include "llvkscenetable.h"
#include "llvkshader.h"
#include "llvkrender.h"      // research/208 S2: the render thread
#include "llvkrecorder.h"    // research/208 S3: LLVKRecorder::tStream
#include "llvktracker.h"
#include "llvktranslate.h"   // research/206: no shadow draws with a translation thread

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <atomic>
#include <bit>          // research/206 5b: std::countr_zero
#include <cmath>
#include <cstring>
#include <deque>        // research/208 13.3: the frame's secondary slots (they never move)
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>       // research/208 S3: the main thread's id
#include <unordered_map>
#include <vector>

namespace
{
    // scene_cull.comp as llvulkan.cmake embeds it.
    const char* const SCENE_CULL_GLSL =
#include "llvkscene_cull_glsl.inl"
        ;

    // research/198 5.5, the expand: per command a drawn pass's buckets have
    // in the view's choice, the record's row of words (the pass's words with
    // the slot of the record's texture for each sampler the viewer names, and
    // the record's matrix row: its transform's, or its texture transform's)
    // and its command naming that row as its first instance. Threads past a
    // bucket's count write nothing (the draw reads the count). The structs
    // are scene_cull.comp's Record and LLRHI::SceneMaterialDesc (std430).
    // research/198 5.6: with a material row word, the record's own material
    // row follows its words (the program's material as it is now, the
    // record's values in the floats material_at names), and the word names it.
    // research/205 3a-3: with a priority word (an OIT session's variant
    // reads its priority from the row), that word gets the record's group's
    // priority this frame (binding 7, by the record's group slot) with its
    // place in the group's draw map (a material value) in the low bits.
    // research/205 3a-5: sorted (params6.w), a record's command goes to its
    // group's base (binding 7 after the priorities, by group slot) plus its
    // place, in an array the frame cleared first (the commands no record
    // writes draw nothing); its row is still the thread's.
    // research/205 H5 (vk-s3a6): a word whose unit takes a texture list's
    // entry (word_list) names that entry of the record's list (binding 8, its
    // row in the record's flags from bit 8 on) when the record has a list,
    // else the material texture (word_texture) as before. The glow pass
    // (params5.w) gives the priority 0xfff in its low bits instead of the place.
    // research/219: a rigged record (flags & SCENE_SKINNED) has no list; its
    // row's palette word (params4.w) names its resident palette row (flags
    // >> 8); with no palette word in the pass's program its command draws
    // nothing (drawPass refuses such a pass first: main_pass_palette_ok).
    const char* const SCENE_EXPAND_GLSL =
        "#version 450\n"
        "layout(local_size_x = 64) in;\n"
        "struct Record\n"
        "{\n"
        "    float lo[3]; uint group_slot; float hi[3]; uint transform;\n"
        "    uint index_count; uint first_index; int vertex_offset; uint bucket;\n"
        "    uint uv_transform; uint material; uint flags; uint record_id;\n"
        "};\n"
        "struct Material { uint tex[4]; uint samplers[2]; uint values[18]; };     // values: the floats' bits\n"
        "layout(std430, set = 0, binding = 0) readonly buffer RecordTable { Record records[]; };\n"
        "layout(std430, set = 0, binding = 1) readonly buffer MaterialTable { Material materials[]; };\n"
        "layout(std430, set = 0, binding = 2) readonly buffer TexSlotTable { uint tex_slots[]; };\n"
        "layout(std430, set = 0, binding = 3) readonly buffer ChoiceWords { uint choice[]; };\n"
        "layout(std430, set = 0, binding = 4) readonly buffer ExpandBlock\n"
        "{\n"
        "    uvec4 params;          // buckets, words per row, vec4s per row, the rows' first vec4 in the palette table\n"
        "    uvec4 params2;         // the matrix word, the view's first matrix row, transform rows, texture slot table size\n"
        "    uvec4 params3;         // the choice's count base, command base, threads, the white slot\n"
        "    uvec4 words[8];\n"
        "    ivec4 word_texture[8];\n"
        "    uvec4 params4;         // the material row's word (0xffffffff: none), its vec4s, the words' vec4s, the palette word (0xffffffff: none)\n"
        "    uvec4 material_template[16];   // the floats' bits (copied as they are, no float arithmetic)\n"
        "    ivec4 material_at[5];  // per material value: its float in the material row, -1: none\n"
        "    uvec4 params5;         // the priority word (0xffffffff: none), group priorities, the place's material value, 1: glow\n"
        "    uvec4 params6;         // sorted: the groups' bases' first word in binding 7, their count, the array's commands, 1: sorted\n"
        "    ivec4 word_list[8];    // per word: the texture list's entry its unit takes, -1: none\n"
        "    uvec4 buckets[];       // choice bucket, its first command in the choice, its first out, its capacity\n"
        "};\n"
        "layout(std430, set = 0, binding = 5) writeonly buffer RowWords { uint rows[]; };\n"
        "layout(std430, set = 0, binding = 6) writeonly buffer CommandWords { uint commands[]; };\n"
        "layout(std430, set = 0, binding = 7) readonly buffer GroupPriority { uint group_priority[]; };\n"
        "layout(std430, set = 0, binding = 8) readonly buffer TexListTable { uint lists[]; };     // 16 numbers a row\n"
        "void main()\n"
        "{\n"
        "    uint g = gl_GlobalInvocationID.x;\n"
        "    if (g >= params3.z)\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    uint lo = 0u;\n"
        "    uint hi = params.x;\n"
        "    while (hi - lo > 1u)\n"
        "    {\n"
        "        uint mid = (lo + hi) >> 1u;\n"
        "        if (buckets[mid].z <= g)\n"
        "        {\n"
        "            lo = mid;\n"
        "        }\n"
        "        else\n"
        "        {\n"
        "            hi = mid;\n"
        "        }\n"
        "    }\n"
        "    uvec4 b = buckets[lo];\n"
        "    uint i = g - b.z;\n"
        "    if (i >= b.w || i >= choice[params3.x + b.x])\n"
        "    {\n"
        "        return;\n"
        "    }\n"
        "    uint at = params3.y + (b.y + i) * 5u;\n"
        "    Record r = records[choice[at + 4u]];\n"
        "    uint row = g * params.z;\n"
        "    uint matrix_row = params2.y + (r.uv_transform != 0u ? params2.z + r.uv_transform : r.transform);\n"
        "    bool skinned = (r.flags & 2u) != 0u;\n"
        "    uint list = skinned ? 0u : r.flags >> 8u;\n"
        "    for (uint w = 0u; w < params4.z * 4u; ++w)\n"
        "    {\n"
        "        uint v = 0u;\n"
        "        if (w < params.y)\n"
        "        {\n"
        "            v = words[w >> 2u][w & 3u];\n"
        "            int t = word_texture[w >> 2u][w & 3u];\n"
        "            int u = word_list[w >> 2u][w & 3u];\n"
        "            if (u >= 0 && list != 0u)\n"
        "            {\n"
        "                uint id = lists[list * 16u + uint(u)];\n"
        "                v = id != 0u && id < params2.w ? tex_slots[id] : params3.w;\n"
        "            }\n"
        "            else if (t >= 0)\n"
        "            {\n"
        "                uint id = materials[r.material].tex[t];\n"
        "                v = id != 0u && id < params2.w ? tex_slots[id] : params3.w;\n"
        "            }\n"
        "            if (w == params2.x)\n"
        "            {\n"
        "                v = matrix_row;\n"
        "            }\n"
        "            if (w == params4.w && skinned)\n"
        "            {\n"
        "                v = r.flags >> 8u;\n"
        "            }\n"
        "            if (w == params4.x)\n"
        "            {\n"
        "                v = params.w + row + params4.z;\n"
        "            }\n"
        "        }\n"
        "        else if (w == params5.x)\n"
        "        {\n"
        "            uint place = uint(uintBitsToFloat(materials[r.material].values[params5.z]));\n"
        "            v = (r.group_slot < params5.y ? group_priority[r.group_slot] : 0u) | (params5.w != 0u ? 0xfffu : min(place, 4094u));\n"
        "        }\n"
        "        rows[row * 4u + w] = v;\n"
        "    }\n"
        "    if (params4.x != 0xffffffffu)\n"
        "    {\n"
        "        uint m = (row + params4.z) * 4u;\n"
        "        for (uint f = 0u; f < params4.y * 4u; ++f)\n"
        "        {\n"
        "            rows[m + f] = material_template[f >> 2u][f & 3u];\n"
        "        }\n"
        "        for (uint k = 0u; k < 18u; ++k)\n"
        "        {\n"
        "            int at = material_at[k >> 2u][k & 3u];\n"
        "            if (at >= 0)\n"
        "            {\n"
        "                rows[m + uint(at)] = materials[r.material].values[k];\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    uint o = g * 5u;\n"
        "    if (params6.w != 0u)\n"
        "    {\n"
        "        uint place = min(uint(uintBitsToFloat(materials[r.material].values[params5.z])), 4094u);\n"
        "        uint fixed_at = r.group_slot < params6.y ? group_priority[params6.x + r.group_slot] + place : 0xffffffffu;\n"
        "        if (fixed_at >= params6.z)\n"
        "        {\n"
        "            return;\n"
        "        }\n"
        "        o = fixed_at * 5u;\n"
        "    }\n"
        "    commands[o] = skinned && params4.w == 0xffffffffu ? 0u : choice[at];\n"
        "    commands[o + 1u] = 1u;\n"
        "    commands[o + 2u] = choice[at + 2u];\n"
        "    commands[o + 3u] = choice[at + 3u];\n"
        "    commands[o + 4u] = params.w + row;\n"
        "}\n";

    using LLVKSceneShader::ViewData;
    using LLVKSceneShader::BucketInfo;

    constexpr U32 BINDINGS = 8;
    constexpr U32 EXPAND_BINDINGS = 9;      // research/205 H5: binding 8, the texture lists
    constexpr U32 PHASES = 3;
    constexpr U32 COMMAND_BYTES = 20;                       // VkDrawIndexedIndirectCommand
    constexpr U32 VERIFY_EVERY = 8;                         // frames between compared choices
    constexpr U32 VERIFY_LOG = 8;                           // differences written to the log
    constexpr F32 VERIFY_SLACK = 0.005f;                    // |excess| below this: the GPU and the CPU may differ
    constexpr VkDeviceSize SCRATCH_BYTES = 4ull << 20;      // per output buffer (more are made when a frame needs them)
    constexpr U32 EXPAND_LOCAL = 64;                        // the expand's workgroup
    constexpr U32 TEX_SLOTS = 1u << 18;                     // texture numbers the slot table holds (1 MB)
    // The scene's share of the frame's palette table (8 MB; the table starts
    // at 16 MB a frame slot and grows when a frame runs out): past it the pass
    // is drawn the usual way that frame.
    constexpr U32 ROW_VEC4S_PER_FRAME = 1u << 19;
    constexpr U32 PASS_RETRY_FRAMES = 256;                  // a pass not drawable for now is tried again this much later
    constexpr U32 PASS_RETRY_LONG = 1u << 14;               // ... one whose program reads no rows (a shader reload may link it again)
    constexpr U32 NONE = LLVKSceneTable::NONE;
    constexpr U32 WEIGHT4_LOCATION = 10;                    // research/219: LLVertexBuffer::TYPE_WEIGHT4's stream (vec4)
    static_assert(LLVKSceneTable::RELEASE_DELAY >= LLVKMirror::FRAMES_IN_FLIGHT + 1,
                  "RELEASE_DELAY: the frames in flight and one more");
    static_assert(sizeof(ViewData) % 16 == 0, "the view block is whole vec4s");
    static_assert(LLVKScene::Binder::Words::MAX == 32, "the expand block holds 32 words");
    static_assert(LLRHI::SceneTextureList::MAX == 16, "the expand reads 16 numbers a texture list row");
    static_assert(LLVKSceneShader::SCENE_TEXLIST_SHIFT == 8, "the expand reads a record's texture list row from flags >> 8");
    static_assert(LLVKSceneShader::SCENE_SKINNED == 2 && LLVKSceneShader::SCENE_PALETTE_SHIFT == 8,
                  "the expand reads a rigged record's palette from flags & 2 and flags >> 8 (research/219)");
    static_assert(LLRHI::SceneDrawContext::UNITS <= 32, "a unit is a bit of a word");

    // research/205 H5 (vk-s3a6): per word of a pass's program, which of a
    // record's textures its row names there: the material texture
    // (SceneDrawContext::unit_texture of the word's unit) and the texture
    // list's entry (unit_list), -1: none.
    struct WordTextures
    {
        S32 texture[LLVKScene::Binder::Words::MAX];
        S32 list[LLVKScene::Binder::Words::MAX];
    };

    // The words' textures for the pass (the program's units as the binder
    // found them). False: a unit the viewer names a material texture for is
    // not one of the program's 2D samplers (the pass cannot be drawn from
    // rows). A list's unit the program does not sample is left out (no
    // vertex of a record can name what the program has no sampler for).
    bool find_word_textures(const LLVKScene::Binder::Words& words, const LLRHI::SceneDrawContext& ctx, WordTextures& out)
    {
        U32 wanted_units = 0;
        U32 found_units = 0;
        for (U32 u = 0; u < LLRHI::SceneDrawContext::UNITS; ++u)
        {
            wanted_units |= ctx.unit_texture[u] >= 0 ? 1u << u : 0u;
        }
        for (U32 w = 0; w < LLVKScene::Binder::Words::MAX; ++w)
        {
            const S32 unit = w < words.count ? words.units[w] : -1;
            const bool named = unit >= 0 && unit < (S32)LLRHI::SceneDrawContext::UNITS;
            out.texture[w] = named ? ctx.unit_texture[unit] : -1;
            out.list[w] = named && ctx.unit_list[unit] >= 0 && ctx.unit_list[unit] < (S32)LLRHI::SceneTextureList::MAX ? ctx.unit_list[unit]
                                                                                                                         : -1;
            found_units |= out.texture[w] >= 0 ? 1u << unit : 0u;
        }
        return found_units == wanted_units;
    }

    // ExpandBlock in SCENE_EXPAND_GLSL; a uvec4 per bucket follows it.
    struct ExpandBlock
    {
        U32 params[4];
        U32 params2[4];
        U32 params3[4];
        U32 words[32];
        S32 word_texture[32];
        U32 params4[4];
        F32 material[LLVKScene::Binder::Words::MATERIAL_VEC4S * 4];     // read as their bits (uvec4)
        S32 material_at[20];
        U32 params5[4];     // research/205 3a-3 (vk-s3a6: and the glow pass)
        U32 params6[4];     // research/205 3a-5
        S32 word_list[32];  // research/205 H5
    };
    static_assert(sizeof(ExpandBlock) == 816, "matches ExpandBlock in the expand shader");
    static_assert(LLVKScene::Binder::Words::MATERIAL_VEC4S == 16, "the expand block holds a material row of 16 vec4s");
    static_assert(LLRHI::SceneMaterialDesc::VALUES == 18 && LLRHI::SceneMaterialDesc::VALUES <= 20,
                  "the expand reads 18 material values and holds 20 places");

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    // research/208 S2 (10.4): what an expand records once the main thread has
    // taken its room (the rows, the command array, the block and the groups'
    // priorities in the upload room, its descriptor set), written them, and
    // flushed the rows written since the choice: values only, so the render
    // thread records it where the main thread spliced it in.
    struct ExpandRecord
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;   // the frame's upload commands
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool sorted = false;                    // research/205 3a-5: the command array cleared first
        VkBuffer commands = VK_NULL_HANDLE;
        VkDeviceSize commands_offset = 0;
        VkDeviceSize commands_bytes = 0;
        U32 groups = 0;                         // the dispatch's workgroups
    };

    void record_expand(const ExpandRecord& e)
    {
        if (e.sorted)
        {
            // research/205 3a-5: the array's places no record writes draw nothing.
            vkCmdFillBuffer(e.cmd, e.commands, e.commands_offset, e.commands_bytes, 0);
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(e.cmd, &dependency);
        }
        vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e.pipeline);
        vkCmdBindDescriptorSets(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e.layout, 0, 1, &e.set, 0, nullptr);
        vkCmdDispatch(e.cmd, e.groups, 1, 1);
        {
            // The commands to the draws, the rows to the shaders that read the
            // palette table (and both to the verify copy).
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(e.cmd, &dependency);
        }
    }

    U64 expand_digest(const ExpandRecord& e)
    {
        LLVKRender::Digest d;
        d << e.cmd << e.pipeline << e.layout << e.set << (U8)e.sorted << e.commands << e.commands_offset << e.commands_bytes << e.groups;
        return d.h;
    }

    // research/208 S2: an expand for the render thread (or compared with it).
    class ExpandJob final : public LLVKRender::Job
    {
    public:
        ExpandRecord rec;
        void record() override { record_expand(rec); }
        U64 digest() const override { return expand_digest(rec); }
        const char* what() const override { return "expand"; }
    };
    constexpr size_t MAX_RENDER_JOBS = 256;     // a frame's jobs of one kind (past them: recorded by the main thread)

    // ==== research/209 E4 (vk-s4a4): the queued choices and expands ====
    // With LLRHI_SCENE_BATCH a choice's three phases and an expand are not
    // recorded where they are made: their room, blocks, sets and outputs are
    // taken and written there (as before, so each can always be recorded
    // alone), and the commands wait in the scene's queue until a cut point
    // records them together, the phases of every queued choice between the
    // same barriers. The cut points: a flush about to copy rows into the
    // tables (the queued work reads them as they are), and every submission
    // (LLVKMirror::sAfterRecorderFinish). Nothing queued is ever dropped while
    // its frame can still be submitted: the draws read the outputs.
    struct QueuedChoice
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        U32 groups[PHASES] = {};
        // verify (startVerify): the output copied to the host after the choice.
        VkBuffer out = VK_NULL_HANDLE;
        VkDeviceSize out_offset = 0;
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceSize readback_bytes = 0;
        U64 begins = 0;         // LLVKMirror::frameBegins when queued
        U32 flushes = 0;        // Scene::mFlushSerial when queued (the tables' copies it reads)
    };
    struct QueuedExpand
    {
        ExpandRecord rec;
        // verify: the commands and the rows copied to the host after the expand.
        VkBuffer readback = VK_NULL_HANDLE;
        VkBufferCopy command_copy = {};
        VkBuffer palette = VK_NULL_HANDLE;
        VkBufferCopy row_copy = {};
        U64 begins = 0;
        U32 flushes = 0;
    };
    // ==== research/209 E4: the batch's auto (scene batch rules) ====
    // (tools/selftest/checks/scenebatch_check.py reads this section.)
    // verify and auto queue as on does; auto turns to "on" after
    // BATCH_AUTO_FRAMES frames that compared something of a batch without a
    // difference, and off for the run at the first difference or epoch fault.
    constexpr U32 BATCH_AUTO_FRAMES = 600;
    enum BatchState : U8
    {
        BATCH_OFF,          // recorded where made (the switch off, or auto found a difference)
        BATCH_CHECKING,     // queued, its frames' comparisons counted (verify, auto before on)
        BATCH_ON,           // queued
    };
    // The state after a frame that compared `compared` things of batches with
    // `differed` differences and `faults` epoch faults.
    inline U8 batch_step(U8 state, bool auto_mode, U32& clean, U32 compared, U32 differed, U32 faults)
    {
        if (state != BATCH_CHECKING)
        {
            return state;
        }
        if (differed || faults)
        {
            clean = 0;
            return auto_mode ? (U8)BATCH_OFF : state;
        }
        clean += compared ? 1 : 0;
        return auto_mode && clean >= BATCH_AUTO_FRAMES ? (U8)BATCH_ON : state;
    }
    // ==== research/209 E4: end ====

    // ==== research/208 S3 (stage 4a): the tables' owner ====
    // While the render thread holds passes of the frame (handed over at their
    // entries, not yet taken back by Scene::own), the scene's tables are the
    // thread's: the main thread's few writes in that time (a transform row, a
    // view, a buffer number's place) are queued in program order as events,
    // which whoever runs the next pass applies first (the thread), or the
    // main thread when it takes the tables back (an entry that must answer,
    // the frame's end). The queue's arrays are made large while the main
    // thread holds the tables and never move while the thread may read them:
    // a write that would not fit takes the tables back first.
    enum SceneEventKind : U8
    {
        EVENT_TRANSFORM,        // a: slot, at: 16 floats
        EVENT_TRANSFORM_GONE,
        EVENT_UV,
        EVENT_UV_GONE,
        EVENT_VIEW,             // a: view, at: a view
        EVENT_BITS,             // a: view, at, count: words
        EVENT_PLACE,            // a: buffer number, at: its place as the pool had it then
    };
    struct SceneEvent
    {
        U8 kind = 0;
        U32 a = 0;
        U32 at = 0;
        U32 count = 0;
        U32 frame = 0;          // LLVKTracker::frame() when it was made (a view's)
    };
    // A number's place as the pool had it (found false: none).
    struct ScenePlace
    {
        bool found = false;
        LLVKAttribPool::NumberPlace place;
    };
    constexpr size_t EVENTS_ROOM = 4096;        // events a frame's hand-over holds at first (doubled when a frame needs more)
    constexpr size_t EVENT_BITS_ROOM = 16384;   // words of visible groups

    // The queue's arrays as the main thread made them before handing the tables over.
    struct SceneEventView
    {
        const SceneEvent* events = nullptr;
        const F32* floats = nullptr;
        const LLRHI::SceneViewDesc* views = nullptr;
        const U32* bits = nullptr;
        const ScenePlace* places = nullptr;
    };

    // True on the thread that runs a pass (PassJob) now: the tables are its own.
    thread_local bool tRunningPass = false;

    // research/208 13.3 (vk-s4a4): where a pass's secondary command buffer is
    // handed to the main thread's execution (LLVKMirror::executeLater, read
    // when the recording worker replays it), and whether the pass's stream was
    // replayed. Kept for the frame (a pass job may be used again within it).
    struct SecondarySlot
    {
        std::atomic<VkCommandBuffer> buffer{ VK_NULL_HANDLE };
        std::atomic<bool> replayed{ false };
    };
    constexpr size_t SECONDARY_SLOTS_MAX = 1024;    // a frame's (past them: the pass's draws go into its stream)
    static_assert(LLVKRender::SECONDARY_SLOTS == LLVKMirror::FRAMES_IN_FLIGHT, "a secondary command pool per frame slot");

    // research/208 S3: a drawn pass of the main or alpha view. The main thread
    // makes it at the pass's entry (the binder's half: the head, the records'
    // textures' slots, the admission, the layouts and the rendering begun, and
    // what the draws bind per chunk); the render thread runs the rest from the
    // scene's tables (the choice, the matrix rows, the expand, the draws) into
    // the stream spliced at the entry. Values only: nothing of the viewer or
    // of the main thread's state.
    class PassJob final : public LLVKRender::Job
    {
    public:
        void record() override;
        U64 digest() const override;
        const char* what() const override { return "pass"; }

        U32 view = 0;
        U32 pass = 0;
        U32 frame = 0;
        VkCommandBuffer upload = VK_NULL_HANDLE;    // the frame's upload commands
        LLVKScene::Binder::Words words;
        WordTextures word_textures = {};            // research/205 H5: the list's entries too
        std::vector<U32> tex_ids;                   // research/208 H22: the pass's texture numbers and their slots
        std::vector<U32> tex_slots;
        F32 view_matrix[16] = {};
        F32 projection[16] = {};
        LLRHI::SceneMatrixFill fill = nullptr;      // H23: the viewer's arithmetic reads its arguments only
        U8 place_value = 0;
        bool glow_priority = false;                 // research/205 (vk-s3a6)
        bool once = false;
        bool slot_lists = false;                    // the viewer gave the view's rows (research/209 S5)
        std::vector<U32> transform_slots;
        std::vector<U32> uv_slots;
        SceneEventView events;
        size_t events_begin = 0;                    // the events queued since the pass before
        size_t events_end = 0;
        LLVKScene::ThreadDraws* draws = nullptr;
        U64 draws_digest = 0;                       // draws' entry values (made with them)
        bool broken = false;                        // a run threw part way: never run again
        // research/208 13.3 (vk-s4a4): its draws into a secondary command
        // buffer of its runner's (execute: the one the main thread's
        // executeLater runs; else, checking, recorded and thrown away), from
        // the pools of the frame slot it was handed over in.
        SecondarySlot* secondary = nullptr;
        bool secondary_execute = false;
        U32 frame_slot = 0;
        U64 frame_begins = 0;
    };

    // research/208 S3, verify: what the thread reads of a pass the main thread's join records.
    class PassDigestJob final : public LLVKRender::Job
    {
    public:
        const PassJob* of = nullptr;
        void record() override {}
        U64 digest() const override { return of ? of->digest() : 0; }
        const char* what() const override { return "pass values"; }
    };

    // research/208 H19: a view's share of a frame table, taken at its first
    // pass handed over in the frame; its passes take from it on the thread.
    struct SceneBudget
    {
        U32 frame = U32_MAX;
        U32 base = 0;
        U32 count = 0;
        U32 used = 0;
        LLVKBindless::DrawMatrices* rows = nullptr;     // the matrix rows'
    };
    constexpr U32 OWNER_FAULTS_KEPT = 8;
    constexpr U32 MISSING_KEPT = 8;
    constexpr U32 THREAD_POOL_SETS = 128;       // H20: sets of a pool of the passes' own (8 storage buffers each)
    constexpr size_t THREAD_POOLS_MAX = 16;     // ... pools a frame slot may grow to

    // A view's choice, valid in the frame it was made in.
    struct Choice
    {
        bool valid = false;
        U32 frame = 0;
        U32 serial = 0;                     // research/209: the choice's own number (a kind's faces choose one after another in a frame)
        VkBuffer out = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;            // the output's word 0
        U32 words = 0;                      // the output's words
        ViewData view;
        std::vector<U32> first_command;     // per bucket
        std::vector<U32> capacity;          // per bucket
        std::vector<BucketInfo> bucket_info;    // the plan's, for the expand's checks
        std::vector<U32> page_list;
    };

    struct ViewState
    {
        LLRHI::SceneViewDesc desc;
        U32 desc_frame = U32_MAX;           // LLVKTracker::frame() it was set in
        std::vector<U32> bits;
        U32 bits_frame = U32_MAX;
        Choice choice;
        // research/198 5.5: the view's matrix rows in the frame's matrix table
        // (a row per transform row, then one per texture transform row).
        U32 matrix_frame = U32_MAX;
        U32 matrix_base = 0;
        U32 matrix_transforms = 0;
        U32 matrix_uvs = 0;
        F32 matrix_view[16] = {};
        F32 matrix_projection[16] = {};
    };

    // A choice copied to the host for verify, and what the CPU makes of it.
    struct Verify
    {
        bool pending = false;
        U32 frame = 0;
        U32 serial = 0;                     // research/209: the choice's (Choice::serial)
        U32 view_index = 0;                 // research/206: the main view's, or a sun shadow view's
        bool batched = false;               // research/209 E4: its choice was queued (the batch's checks count it)
        U32 words = 0;
        ViewData view;
        std::vector<U32> expected;
        std::vector<U32> first_command;
        std::vector<U8> slack;              // per slot: too close to a plane to compare
    };

    // research/198 5.5: an expand's output copied to the host (with that
    // frame's choice, Verify), and the rows the CPU makes of the same rows.
    struct ExpandVerify
    {
        bool pending = false;
        U32 frame = 0;
        U32 serial = 0;                     // research/209: the choice it expanded (Choice::serial)
        U32 view_index = 0;                 // research/205 3a-3: the main view's, or the alpha view's
        U32 pass = 0;
        bool batched = false;               // research/209 E4: the expand was queued
        U32 total = 0;                      // commands (the dispatch's threads)
        U32 vec4s = 0;                      // a record's: its words', then its material row's
        U32 word_vec4s = 0;
        U32 material_word = U32_MAX;        // research/198 5.6: names the record's own material row
        U32 rows_base = 0;
        U32 count_base = 0;
        U32 command_base = 0;
        std::vector<U32> buckets;           // 4 per bucket: choice bucket, first command, first out, capacity
        std::vector<U32> page_local;        // by page: its place among the pass's pages (NONE: not the pass's)
        std::vector<U32> expected;          // per slot of those pages: its row (vec4s * 4 words)
        // research/205 3a-5: sorted, the array's commands, and per slot of
        // those pages the command its record's goes to (NONE: none).
        U32 commands = 0;                   // the output's commands (total, or the sorted array's)
        bool sorted = false;
        std::vector<U32> expected_at;
        // research/219: per slot of those pages, 1: its command draws nothing
        // (a rigged record, the pass's program reads no palette).
        std::vector<U8> expected_none;
        Buffer readback;                    // the commands (commands * 5 words), then the rows (total * vec4s * 4 words)
    };

    struct FrameSlot
    {
        U64 begins = ~0ull;                 // LLVKMirror::frameBegins when last used
        std::vector<Buffer> scratch;
        size_t scratch_index = 0;
        VkDeviceSize scratch_head = 0;
        // fix-r8: the scene's own upload room (host memory; blocks made as the frames need them, up to mUploadCap).
        std::vector<Buffer> upload;
        size_t upload_index = 0;
        VkDeviceSize upload_head = 0;
        VkDeviceSize upload_bytes = 0;      // the blocks' sizes together
        Buffer readback;
        Verify verify;
        ExpandVerify expand;
    };

    // ==== scene place rules: begin ====
    // (tools/selftest/sim/gen_scene.py copies this section into sim_scene.)
    // research/198 5.5: where a record's buffer is in the pool.
    enum PlaceState : U8
    {
        PLACE_OK,           // placed with every attribute its pass reads, its indices inside the place; the row has it
        PLACE_NONE,         // its buffer's number has no place (none made, gone, not copyable, numbers off)
        PLACE_ATTRIBUTES,   // the place lacks an attribute its pass reads (a draw of another program made it)
        PLACE_RANGE,        // its indices reach past the place's
        PLACE_STALE,        // the place moved or was made since its row was written (written at the next choice)
        PLACE_STATES,
    };

    // numberChanged: the state a record takes when its number's place
    // changed. Drawable at another place than its row's: stale (not drawn)
    // until applyPlaces writes the row; else what the pool says now.
    inline U8 place_state_on_change(U8 pool_state, bool same_row)
    {
        return pool_state == PLACE_OK && !same_row ? (U8)PLACE_STALE : pool_state;
    }

    enum PlaceCheck : U8
    {
        PLACE_CHECK_SKIP,       // nothing to compare (not counted)
        PLACE_CHECK_SAME,
        PLACE_CHECK_DIFFERS,
    };

    // checkPlaces: a record's row (its place and state) against its
    // number's place in the pool now. Skipped: a row without a place or
    // waiting for applyPlaces, a place that went (the row keeps it until the
    // frame's head), and (fix-r6) a record drawn from neither side: both
    // lack an attribute its pass reads, so its row may still name an older
    // place (numberChanged marks only drawable rows stale; applyPlaces
    // writes the row at the next choice).
    inline U8 place_check(bool row_placed, U8 row_state, bool pool_placed, U8 pool_state, bool same_range)
    {
        if (!row_placed || row_state == PLACE_STALE || !pool_placed)
        {
            return PLACE_CHECK_SKIP;
        }
        if (row_state != PLACE_OK && pool_state != PLACE_OK)
        {
            return PLACE_CHECK_SKIP;
        }
        return same_range && pool_state == row_state ? PLACE_CHECK_SAME : PLACE_CHECK_DIFFERS;
    }
    // ==== scene place rules: end ====
    const char* const PLACE_NAMES[PLACE_STATES] = { "placed", "no place", "attributes missing", "past the place", "waiting" };

    // research/198 5.5: what the backend keeps of a record beside its row.
    struct RecordInfo
    {
        U32 group = 0;
        U32 number = 0;                     // its vertex buffer's number
        U32 first = 0;                      // its first index in the buffer
        U32 count = 0;
        U32 attributes = 0;                 // the locations its pass reads
        U32 transform = 0;
        U32 uv = 0;
        U32 pass = 0;                       // research/219: its pass, and whether it is rigged (counted per pass)
        bool skinned = false;
        U8 state = PLACE_NONE;
        LLRHI::SceneRange row;              // the range its row has (chunk SCENE_UNPLACED: none)
    };

    // A texture transform row's model matrix (the matrix rows pair them):
    // every record of the row has the same transform (a face's texture
    // matrix belongs to its drawable).
    struct UVOwner
    {
        U32 transform = 0;
        U32 users = 0;
    };

    // ==== scene view rules: begin ====
    // (tools/selftest/sim/gen_scene.py copies this section into sim_scene.)
    // research/209 S6: the main view and its alpha view, which come last in
    // the frame (the probe faces of the display before, the mirrors and the
    // water mirror are drawn before them).
    inline bool view_is_main(U32 view)
    {
        return view == LLRHI::SCENE_VIEW_MAIN || view == LLRHI::SCENE_VIEW_ALPHA;
    }

    // research/209 S6: whether want more of the frame's palette rows fit the
    // scene's share: the main view may take what the others left of it; the
    // probe faces and the mirrors together at most half of it, each kind at
    // most a quarter (the probe faces come first in the frame), so the main
    // view always finds half of the share.
    inline bool rows_fit(U32 view, U64 all_used, U64 others_used, U64 kind_used, U64 want, U64 share)
    {
        if (all_used + want > share)
        {
            return false;
        }
        return view_is_main(view) || (others_used + want <= share / 2 && kind_used + want <= share / 4);
    }

    // research/209 S6: the passes the scene could not draw. Its program's
    // reasons (it reads no rows, a unit outside the table) hold in every
    // view, until never_retry; the frame's (a full share or table, a bind
    // that failed) in the view they came in only, until later_retry: a probe
    // face's full share does not send the main view's pass the usual way.
    struct PassRefusals
    {
        U8 never[LLRHI::SCENE_MAX_PASSES] = {};
        U32 never_retry[LLRHI::SCENE_MAX_PASSES] = {};
        U8 later[LLRHI::SCENE_VIEWS][LLRHI::SCENE_MAX_PASSES] = {};
        U32 later_retry[LLRHI::SCENE_VIEWS][LLRHI::SCENE_MAX_PASSES] = {};
    };

    inline void pass_refuse(PassRefusals& r, U32 view, U32 pass, bool permanent, U32 frame, U32 wait)
    {
        if (permanent)
        {
            r.never[pass] = 1;
            r.never_retry[pass] = frame + wait;
        }
        else
        {
            r.later[view][pass] = 1;
            r.later_retry[view][pass] = frame + wait;
        }
    }

    inline void pass_drew(PassRefusals& r, U32 view, U32 pass)
    {
        r.never[pass] = 0;
        r.later[view][pass] = 0;
    }

    inline bool pass_open(const PassRefusals& r, U32 view, U32 pass, U32 frame)
    {
        return (!r.never[pass] || (S32)(frame - r.never_retry[pass]) >= 0) &&
               (!r.later[view][pass] || (S32)(frame - r.later_retry[view][pass]) >= 0);
    }

    // fix-r8 (bugloop round 9): a view's choice that found no room for its
    // uploads (the scene's own upload room and the frame ring both full, or
    // no output or descriptor set) is drawn the usual way for the rest of
    // that frame only, and chosen again the next. Only after UPLOAD_STREAK
    // shorts in a row does the view wait longer, doubling up to
    // UPLOAD_WAIT_MAX frames; one choice that fits starts the count again.
    // Never for good, and never another view.
    constexpr U32 UPLOAD_STREAK = 4;
    constexpr U32 UPLOAD_WAIT_MAX = 64;

    struct UploadShorts
    {
        U32 streak[LLRHI::SCENE_VIEWS] = {};    // shorts in a row
        U8 waiting[LLRHI::SCENE_VIEWS] = {};
        U32 retry[LLRHI::SCENE_VIEWS] = {};     // the first frame it is chosen again
    };

    inline U32 upload_wait(U32 streak)
    {
        if (streak < UPLOAD_STREAK)
        {
            return 1;
        }
        const U32 doublings = streak - UPLOAD_STREAK + 1;     // 2, 4, 8, ... frames
        return doublings >= 6 ? UPLOAD_WAIT_MAX : 1u << doublings;
    }

    inline void upload_short(UploadShorts& s, U32 view, U32 frame)
    {
        s.streak[view] += s.streak[view] < 0xFFFFu ? 1 : 0;
        s.waiting[view] = 1;
        s.retry[view] = frame + upload_wait(s.streak[view]);
    }

    inline void upload_fit(UploadShorts& s, U32 view)
    {
        s.streak[view] = 0;
        s.waiting[view] = 0;
    }

    inline bool upload_open(const UploadShorts& s, U32 view, U32 frame)
    {
        return !s.waiting[view] || (S32)(frame - s.retry[view]) >= 0;
    }

    // fix-r8: the scene's own upload room a frame slot may grow to (host
    // memory, made in blocks as the frames need it): UPLOAD_CAP, at most a
    // 256th of the device-local budget (0: unknown) and the device's storage
    // buffer range, at least one block.
    constexpr U64 UPLOAD_BLOCK = 1ull << 20;
    constexpr U64 UPLOAD_CAP = 16ull << 20;

    inline U64 upload_cap(U64 vram_budget, U64 max_storage_range)
    {
        U64 cap = UPLOAD_CAP;
        if (vram_budget && vram_budget / 256 < cap)
        {
            cap = vram_budget / 256;
        }
        if (max_storage_range && max_storage_range < cap)
        {
            cap = max_storage_range;
        }
        return cap < UPLOAD_BLOCK ? UPLOAD_BLOCK : cap;
    }
    // ==== scene view rules: end ====
    const char* const VIEW_NAMES[LLRHI::SCENE_VIEWS] = { "main",  "probe", "mirror", "water",  "sun 0", "sun 1",
                                                         "sun 2", "sun 3", "alpha",  "spot 0", "spot 1" };
    // research/209 S5: the matrix rows the probe faces and the mirrors may take a frame (the usual path's draws
    // share the table: a full table gives later draws its last row).
    constexpr U32 OTHER_MATRIX_ROWS = LLVKBindless::MATRIX_ROWS / 4;

    // The vec4s of a record's words in its row: the program's words and
    // (research/205 3a-3) the priority word an OIT variant reads after them.
    U32 row_word_vec4s(const LLVKScene::Binder::Words& words)
    {
        const U32 used = words.priority_word != U32_MAX ? llmax(words.count, words.priority_word + 1) : words.count;
        return (used + 3) / 4;
    }

    bool same_range(const LLRHI::SceneRange& a, const LLRHI::SceneRange& b)
    {
        return a.chunk == b.chunk && a.vertex_offset == b.vertex_offset && a.first_index == b.first_index &&
               a.index_count == b.index_count && a.index32 == b.index32;
    }

    class Scene final : public LLRHI::Scene
    {
    public:
        bool ready() override;
        U32 addGroup() override;
        void removeGroup(U32 group) override;
        U32 addRecord(const LLRHI::SceneRecordDesc& desc) override;
        bool updateRecord(U32 record, const LLRHI::SceneRecordDesc& desc) override;
        void removeRecord(U32 record) override;
        U32 addTransform(const F32 matrix[16]) override;
        void setTransform(U32 slot, const F32 matrix[16]) override;
        void removeTransform(U32 slot) override;
        U32 addUVTransform(const F32 words[16]) override;
        void setUVTransform(U32 slot, const F32 words[16]) override;
        void removeUVTransform(U32 slot) override;
        void setView(U32 view, const LLRHI::SceneViewDesc& desc) override;
        void setGroupVisible(U32 view, const U32* bits, U32 words) override;
        bool drawPass(U32 view, U32 pass, const LLRHI::SceneDrawContext& ctx) override;
        bool drawSorted(U32 view, U32 pass, U32 first, const U32* records, U32 count) override;     // research/205 3a-5
        bool drawShadow(U32 view, const LLRHI::SceneShadowContext& ctx) override;
        bool shadowMasks() const override;  // research/206 5c
        bool shadowSkins() const override;  // research/219
        void setDrawing(bool on) override;
        // Only with a binder: without one nothing can be drawn, and the
        // viewer must keep every batch in its render map.
        bool drawing() const override { return mDrawing && mBinder != nullptr && mExpandReady; }
        bool canDrawPass(U32 view, U32 pass) const override;
        void forgetPass(U32 pass) override;     // research/204 10 (vk-s4c2)
        bool groupDrawable(U32 group) const override;
        bool viewTurn(U32 view) const override;
        U32 problems() const override { return mProblems; }
        void collect(U32 view, U32 pass, std::vector<U32>& drawn, std::vector<U32>& bounded_out) override;
        std::string takeStats(U32 frames) override;

        void setBinder(LLVKScene::Binder* binder) { mBinder = binder; }
        void shutdown();
        // research/206: the shadow renderer's side (under LLVKTracker::lock()).
        bool takeShadowDraws(LLVKScene::ShadowDraws& out);
        void shadowDrawn(bool drawn, U32 buckets);
        void setShadowMasks(bool ready) { mShadowMasksReady = ready; }     // research/206 5c
        void setShadowSkins(bool ready) { mShadowSkinsReady = ready; }     // research/219
        // The pool's listener (LLVKAttribPool::setNumberListener).
        void numberChanged(U32 number);
        // research/230 R2 (LLVKScene::collectRayRecords).
        U64 tableWriteSerial() const { return mTable.writeSerial(); }
        bool collectRayRecords(const U32 pass_mask[4], std::vector<LLVKScene::RayRecord>& out, U64& serial);

    private:
        struct ExpandOut
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            U32 total = 0;
            bool no_room = false;       // fix-r8: it failed for upload room (the pass is drawn the usual way this frame only)
        };

        bool supported() const;
        bool init();
        bool initExpand();
        void syncFrame();
        bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out);
        FrameSlot& frameSlot();
        bool scratch(FrameSlot& slot, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset);
        bool uploadRoom(VkDeviceSize size, VkDeviceSize alignment, VkBuffer& buffer, VkDeviceSize& offset, void*& data);
        void noRoom(U32 view, const char* what, VkDeviceSize size);
        bool flush(VkCommandBuffer cmd);
        bool choose(U32 view, bool verify_view);
        void startVerify(FrameSlot& slot, const ViewData& view, U32 words, const LLVKSceneTable::Plan& plan, U32 view_index,
                         VkCommandBuffer cmd, VkBuffer out, VkDeviceSize out_offset, QueuedChoice* queued);
        void compare(FrameSlot& slot);

        // research/198 5.5
        bool describeInfo(const LLRHI::SceneRecordDesc& desc, RecordInfo& info, U32 replacing) const;
        U8 resolvePlace(const RecordInfo& info, LLRHI::SceneRange& range) const;
        void setState(RecordInfo& info, U8 state);
        void link(U32 id, const RecordInfo& info);
        void unlink(U32 id, const RecordInfo& info);
        void applyPlaces(bool frame_head);
        void refusePass(U32 view, U32 pass, const char* why, bool permanent);
        void resolveTextures(const LLRHI::SceneDrawContext& ctx);
        bool shadowTextures(const LLRHI::SceneShadowContext& ctx, std::vector<U32>& sampled);     // research/206 5c
        bool matrixRows(U32 view_index, ViewState& v, const LLRHI::SceneDrawContext& ctx);
        bool expand(U32 view_index, U32 pass, const LLVKScene::Binder::Words& words, const WordTextures& word_textures,
                    const LLRHI::SceneDrawContext& ctx, bool verify, ExpandOut& out);
        void expectedRow(U32 slot, const LLVKScene::Binder::Words& words, const WordTextures& word_textures,
                         const LLRHI::SceneDrawContext& ctx, const ViewState& v, U32 vec4s, U32* out) const;
        U32 sortedPlace(U32 slot, const LLRHI::SceneDrawContext& ctx, U32 commands) const;     // research/205 3a-5
        void checkPlaces(U32 view_index, U32 pass);
        void compareExpand(FrameSlot& slot);
        // research/208 S2: the expand's commands, by the render thread or here.
        void recordExpand(const ExpandRecord& rec, U32 view_index, U32 pass, bool verify);
        ExpandJob* freeExpandJob();
        // research/209 E4 (vk-s4a4): whether new choices and expands are queued now.
        bool batching() const { return mBatchState != BATCH_OFF; }
        // Records the queue into cmd (the frame's upload commands, or a stream
        // of them): the tables' owner, or the main thread at a submission.
        void recordBatch(VkCommandBuffer cmd);
        std::string batchStats(U32 frames);
        // research/208 13.3: a slot for a pass's secondary command buffer this frame (main thread); null: none left.
        SecondarySlot* takeSecondarySlot();
        void secondaryStep();
        std::string secondaryStats(U32 frames);

    public:
        // research/209 E4: a submission's cut point (LLVKMirror::sAfterRecorderFinish).
        void submissionCut(VkCommandBuffer upload, bool frame_end);
        // research/208 S3: the pass's run (PassJob::record, on its runner).
        void runPass(PassJob& job);
        // The frame's end (LLVKRender's frame join hook): the tables back to the main thread.
        void frameJoined();
        // The scene's shutdown: the same (under the backend lock).
        void takeBack() { own("the scene's shutdown"); }

    private:
        // research/208 S3.
        LLVKRender::Use threadPassUse(U32 view_index, const LLRHI::SceneDrawContext& ctx) const;
        bool postPass(U32 view_index, U32 pass, const LLRHI::SceneDrawContext& ctx, LLVKRender::Use use);
        void runPassBody(PassJob& job);
        void passMissing(PassJob& job, const char* why);
        void own(const char* why);
        void ownerCheck(const char* where) const;
        void ownerFault(const char* where) const;
        void makeEventRoom();
        bool eventRoom(size_t floats, size_t views, size_t bits, size_t places) const;
        void queueEvent(U8 kind, U32 a, U32 at, U32 count);
        void applyEvents(const SceneEventView& events, size_t end);
        void applyEvent(const SceneEventView& events, const SceneEvent& e);
        void placeChanged(U32 number);
        bool placeOf(U32 number, LLVKAttribPool::NumberPlace& out) const;
        void takeBudgets(U32 view_index, U32 frame);
        bool takePaletteRows(U32 view_index, U32 vec4s, U32& base);
        LLVKBindless::DrawMatrices* takeMatrixRows(U32 view_index, U32 count, U32& first);
        VkCommandBuffer uploadCmd() const;
        VkDescriptorSet allocSet(VkDescriptorSetLayout layout);
        PassJob* freePassJob();
        PassDigestJob* freeDigestJob();
        std::string threadStats(U32 frames);

        LLVKSceneTable mTable;
        bool mTried = false;
        bool mReady = false;
        bool mBroken = false;
        bool mDrawing = LLRHI::sceneMode() == LLRHI::SceneMode::On;
        LLVKScene::Binder* mBinder = nullptr;
        U32 mFrame = 0;
        bool mFrameKnown = false;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        LLVKShader::Program mShader;
        VkDeviceSize mStorageAlign = 16;
        Buffer mTables[LLVKSceneTable::TABLE_COUNT];
        VkDeviceAddress mTableAddress[LLVKSceneTable::TABLE_COUNT] = {};    // research/206
        ViewState mViews[LLRHI::SCENE_VIEWS];     // research/206: and the sun shadow views
        FrameSlot mSlots[LLVKMirror::FRAMES_IN_FLIGHT];
        std::vector<LLVKSceneTable::Run> mRuns;
        std::vector<VkBufferCopy> mRegions;
        std::vector<U32> mGot;
        std::vector<U32> mWant;

        // research/198 5.5: drawing.
        bool mExpandReady = false;
        VkDescriptorSetLayout mExpandSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mExpandLayout = VK_NULL_HANDLE;
        VkPipeline mExpandPipeline = VK_NULL_HANDLE;
        LLVKShader::Program mExpandShader;
        Buffer mTexTable;                       // the slot per texture number (U32)
        VkDeviceAddress mTexTableAddress = 0;   // research/206 5c: the shadow renderer's mask shaders read it
        bool mTexTableCleared = false;
        std::vector<U32> mTexSlots;             // what mTexTable holds (after the next flush)
        std::vector<U32> mTexDirty;
        std::unordered_map<U32, RecordInfo> mInfos;                 // by record id
        std::unordered_map<U32, std::vector<U32>> mNumberRecords;   // buffer number -> record ids
        std::unordered_map<U32, UVOwner> mUVOwners;                 // texture transform row -> its transform
        std::vector<U32> mDirtyNumbers;
        std::vector<U32> mGroupRecords;         // by group id: records
        std::vector<U32> mGroupUndrawable;      // by group id: records not PLACE_OK
        // research/219: by pass, its rigged records and the others (link, unlink: the tables' owner).
        U32 mPassSkinned[LLRHI::SCENE_MAX_PASSES] = {};
        U32 mPassPlain[LLRHI::SCENE_MAX_PASSES] = {};
        bool mNumbersTaken = false;
        PassRefusals mRefusals;                 // research/209 S6: by view
        const char* mPassWhy[LLRHI::SCENE_VIEWS][LLRHI::SCENE_MAX_PASSES] = {};
        U32 mRowFrame = U32_MAX;
        U32 mRowVec4s = 0;                      // of the frame's palette table, this frame
        U32 mRowVec4sOthers = 0;                // research/209 S6: ... the probe faces' and mirrors'
        U32 mRowVec4sView[LLRHI::SCENE_VIEWS] = {};
        U32 mMatrixFrame = U32_MAX;             // research/209 S5: the probe faces' and mirrors' matrix rows this frame
        U32 mMatrixOthers = 0;
        U32 mChoiceSerial = 0;                  // research/209: Choice::serial
        std::vector<U32> mExpandBuckets;        // 4 per bucket (ExpandBlock::buckets)
        std::vector<U32> mExpandUpload;         // the block's words
        // research/208 S2: the expands handed to the render thread (each free again after the join).
        std::vector<std::unique_ptr<ExpandJob>> mExpandJobs;
        size_t mExpandJobNext = 0;
        U32 mExpandsByThread = 0, mDrawsByThread = 0;   // since the last takeStats: passes whose commands went to the thread
        std::atomic<U32> mProblems{ 0 };    // research/208 S3: also counted by the render thread

        // research/209 E4 (vk-s4a4): the queue (the tables' owner's; the
        // main thread's at a submission's cut, when no pass runs), and the
        // switch's state (main thread at the frame's cut; read by the owner).
        std::vector<QueuedChoice> mQueuedChoices;
        std::vector<QueuedExpand> mQueuedExpands;
        U32 mFlushSerial = 0;               // flushes that copied rows into the tables
        std::atomic<U8> mBatchState{ (U8)(LLRHI::sceneBatchMode() == LLRHI::RenderThreadMode::Off      ? BATCH_OFF
                                          : LLRHI::sceneBatchMode() == LLRHI::RenderThreadMode::On ? BATCH_ON
                                                                                                    : BATCH_CHECKING) };
        U32 mBatchClean = 0;                // auto: frames compared without a difference
        // This frame's (the frame's cut adds them up): comparisons of queued choices and expands, their differences.
        std::atomic<U32> mBatchCompared{ 0 }, mBatchDiffered{ 0 };
        // Since the last takeStats.
        U32 mBatchCuts = 0, mBatchCutsAtFlush = 0, mBatchChoices = 0, mBatchExpands = 0, mBatchBarriers = 0;
        U32 mBatchEpochFaults = 0, mBatchStale = 0, mBatchComparedTotal = 0, mBatchDifferedTotal = 0;
        U32 mBatchFaultsLogged = 0;
        U32 mBatchFaultsSeen = 0;           // mBatchEpochFaults at the last frame's cut (auto's step)

        // research/208 13.3 (vk-s4a4): the passes' secondary command buffers.
        // The state as the batch's (main thread at the frame's cut; read at
        // the entries); the counts by the runners.
        std::atomic<U8> mSecondaryState{ (U8)(LLRHI::passSecondaryMode() == LLRHI::RenderThreadMode::Off      ? BATCH_OFF
                                              : LLRHI::passSecondaryMode() == LLRHI::RenderThreadMode::On ? BATCH_ON
                                                                                                         : BATCH_CHECKING) };
        U32 mSecondaryClean = 0;
        std::atomic<U32> mSecondaryRecorded{ 0 }, mSecondaryChecked{ 0 }, mSecondaryDiffered{ 0 }, mSecondaryFailed{ 0 };
        // Since the last takeStats (the frame's cut adds them up).
        U32 mSecondaryRecordedTotal = 0, mSecondaryCheckedTotal = 0, mSecondaryDifferedTotal = 0, mSecondaryFailedTotal = 0;
        U32 mSecondaryNoSlot = 0;
        std::deque<SecondarySlot> mSecondarySlots;      // main thread (the entries)
        size_t mSecondaryNext = 0;
        U64 mSecondaryBegins = ~0ull;

        // research/208 S3: the tables' owner. Main thread: mOwnedByThread
        // (passes handed over since the tables were last taken back),
        // mJobsPosted, the queue's writes, mPosted*, mSeen*, mGiven*, mKnown*.
        // The tables' owner (the runner of a pass, or the main thread once it
        // took them back): everything else here.
        bool mS3Used = false;               // a pass was handed over once (the owner checks run from then on)
        bool mOwnedByThread = false;
        U32 mJobsPosted = 0;
        std::atomic<U32> mJobsRun{ 0 };
        std::atomic<bool> mThreadBroke{ false };     // a pass's run threw: the scene stops at the next take-back
        std::thread::id mMainThread;
        PassJob* mJob = nullptr;            // the pass being run (its runner's)
        std::vector<std::unique_ptr<PassJob>> mPassJobs;
        size_t mPassJobNext = 0;
        std::vector<std::unique_ptr<PassDigestJob>> mDigestJobs;
        size_t mDigestJobNext = 0;
        std::vector<SceneEvent> mEvents;
        std::vector<F32> mEventFloats;
        std::vector<LLRHI::SceneViewDesc> mEventViews;
        std::vector<U32> mEventBits;
        std::vector<ScenePlace> mEventPlaces;
        size_t mEventsApplied = 0;          // the owner's
        size_t mEventsPosted = 0;           // main: the events the passes handed over apply
        bool mEventsGrow = false;           // main: a write found the queue full (doubled at the take-back)
        bool mApplyingEvents = false;       // the owner applies events (a number's place is the event's)
        bool mEventDropped = false;         // a place from another thread found the queue full (every number again at the take-back)
        std::unordered_map<U32, ScenePlace> mCaptured;  // the owner's: the numbers' places as their last events had them
        PassRefusals mRefusalsSeen;         // main: canDrawPass while the thread holds the tables
        UploadShorts mShortsSeen;
        U32 mGivenDesc[LLRHI::SCENE_VIEWS] = {};    // main: the frame (+ 1) each view's setView came in
        U32 mGivenBits[LLRHI::SCENE_VIEWS] = {};    // ... its setGroupVisible
        U32 mKnownTransforms = 0, mKnownUVs = 0;    // main: the rows' spans when it last held the tables
        SceneBudget mPaletteBudget[LLRHI::SCENE_VIEWS];     // H19
        SceneBudget mMatrixBudget[LLRHI::SCENE_VIEWS];
        U32 mPaletteFrame = U32_MAX;        // the owner's: the frame mPaletteWanted counts
        U32 mPaletteWanted[LLRHI::SCENE_VIEWS] = {};
        U32 mPaletteWantedLast[LLRHI::SCENE_VIEWS] = {};
        std::vector<VkDescriptorPool> mThreadPools[LLVKMirror::FRAMES_IN_FLIGHT];  // H20: the passes' sets (the owner's)
        size_t mThreadPoolIndex[LLVKMirror::FRAMES_IN_FLIGHT] = {};
        // Since the last takeStats.
        U32 mS3Posted = 0, mS3Deferred = 0, mS3Here = 0, mS3Ran = 0, mS3Fallback = 0, mS3Takebacks = 0, mS3Events = 0;
        U32 mS3Missing = 0, mS3BudgetShort = 0, mS3Dropped = 0;
        const char* mS3MissingWhy[MISSING_KEPT] = {};
        U32 mS3MissingKept = 0;
        const char* mS3TakebackWhy = nullptr;   // the last entry that took the tables back in a frame
        U32 mS3MissingLogged = 0;
        U32 mPostFrame = U32_MAX;           // main: the frame of the last pass handed over
        mutable std::atomic<U32> mOwnerFaults{ 0 };
        mutable std::atomic<const char*> mOwnerFaultWhere[OWNER_FAULTS_KEPT];
        // research/198 5.6: which pass of a verified frame has its expand compared.
        U32 mVerifyTurnFrame = U32_MAX;
        U32 mVerifyPasses = 0;                  // the scope passes seen this verified frame
        U32 mVerifyPassesLast = 0;              // ... the verified frame before
        U32 mVerifyRound = 0;
        // research/205 3a-3: which alpha pass of the alpha view's verified frames has its expand compared.
        U32 mAlphaVerifyFrame = U32_MAX;
        U32 mAlphaVerifyRound = 0;
        U32 mAlphaVerifyPasses = 0;
        // research/209 S7: a probe face's or mirror's turn (viewTurn), and which of its passes has its expand compared.
        U32 mOtherTurnFrame = U32_MAX;
        U32 mOtherVerifySerial = U32_MAX;
        U32 mOtherVerifyRound = 0;
        U32 mOtherVerifyPasses = 0;
        U32 mOtherVerifyPassesLast = 0;
        U32 mPriorityRows = 0, mOnceMissed = 0;     // since the last takeStats: rows with a priority word, buckets left out
        // research/205 3a-5: the sorted passes expanded this frame (by pass), and drawSorted's runs.
        struct SortedPass
        {
            U32 frame = U32_MAX;
            U32 serial = 0;                         // the choice's
            VkBuffer commands = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            U32 count = 0;                          // the array's commands
            LLVKScene::Binder::Words words;         // what the rows were made from
            WordTextures word_textures = {};
            LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
            U32 attribute_count = 0;
            const LLRHI::SceneMaterialUniform* materials = nullptr;
            U32 material_count = 0;
        };
        struct SortedRun
        {
            U32 chunk = 0;
            U32 first = 0;                          // in the span
            U32 count = 0;
        };
        std::vector<SortedPass> mSorted;
        std::vector<SortedRun> mSortedRuns;
        std::vector<U8> mSortedWritten;             // compareExpand's, by command
        U32 mSortedPrepared = 0, mSortedSpans = 0, mSortedDraws = 0, mSortedRefused = 0, mSortedWordsChanged = 0;
        U64 mSortedRecords = 0;
        // research/206: the sun shadow draws made ready for the next native shadow pass.
        LLVKScene::ShadowDraws mShadowPending;
        U32 mShadowPendingFrame = U32_MAX;
        bool mShadowMasksReady = false;         // research/206 5c: the shadow renderer's mask shaders
        bool mShadowSkinsReady = false;         // research/219: the shadow renderer's skinned shaders
        U32 mShadowSkinBuckets = 0, mShadowSkinRefused = 0;     // research/219: since the last takeStats
        U32 mPaletteRefused = 0;                // research/219: passes drawn the usual way, their records' palettes and program disagreeing
        // fix-r8: the scene's uploads (its own room per frame slot, then the frame ring) and the views that found no room.
        VkDeviceSize mUploadCap = UPLOAD_BLOCK;
        UploadShorts mShorts;
        U64 mUploadFrameBegins = ~0ull;
        U64 mUploadFrame = 0;                   // this frame's scene uploads (bytes)
        U64 mUploadPeak = 0;                    // since the last takeStats: the largest frame
        U32 mUploadShared = 0;                  // ... uploads that went to the frame ring (the own room full)
        U32 mUploadLogged = 0, mUploadNearLogged = 0;
        U32 mViewShorts[LLRHI::SCENE_VIEWS] = {};   // ... choices or expands without room, by view
        U32 mViewShortSkips[LLRHI::SCENE_VIEWS] = {};   // ... choices not tried (the view waits after shorts in a row)

        // Since the last takeStats.
        U32 mChoices = 0, mDrawnBuckets = 0, mNoBinder = 0, mBindFailed = 0, mNotChosen = 0;
        U64 mUploadBytes = 0;
        U32 mCompared = 0, mMismatches = 0, mLogged = 0;
        U32 mPassesDrawn = 0, mPassesRefused = 0, mExpands = 0, mPlaceUpdates = 0, mPlaceUpdateFailed = 0, mRefusedRecords = 0;
        U64 mMatrixRows = 0, mExpandCommands = 0;
        U32 mTexResolved = 0, mTexWhite = 0, mTexChanged = 0;
        U32 mExpandCompared = 0, mExpandMismatches = 0, mPlaceChecked = 0, mPlaceMismatches = 0;
        // research/206
        U32 mShadowChoices = 0, mShadowReady = 0, mShadowRefused = 0, mShadowDrawn = 0, mShadowLost = 0;
        U64 mShadowBuckets = 0;
        U64 mShadowMaskBuckets = 0, mShadowTextures = 0;   // research/206 5c: made ready (mask buckets, textures resolved)
        U32 mShadowMaskRefused = 0;                         // ... masks that could not be made ready
        // research/209: by view (the probe faces and the mirrors).
        U32 mViewChoices[LLRHI::SCENE_VIEWS] = {}, mViewPassesDrawn[LLRHI::SCENE_VIEWS] = {}, mViewPassesRefused[LLRHI::SCENE_VIEWS] = {};
        U32 mViewVerified[LLRHI::SCENE_VIEWS] = {};
        U64 mViewMatrixRows[LLRHI::SCENE_VIEWS] = {};
        bool mUsed = false;
    };

    // Never destroyed: the viewer may still remove records while static objects go away at exit.
    Scene& sScene = *new Scene();

    // LLVKAttribPool's listener (under LLVKTracker::lock(), from inside the pool).
    void on_number_changed(U32 number)
    {
        sScene.numberChanged(number);
    }

    bool Scene::supported() const
    {
        const LLVKDevice& device = LLVKDevice::instance();
        const LLVKSceneTable::Limits& limits = mTable.limits();
        return device.isReady() && device.hasMultiDrawIndirect() && device.hasDrawIndirectFirstInstance() && device.hasDrawIndirectCount() &&
               device.getLimits().maxDrawIndirectCount >= limits.pages * LLVKSceneTable::PAGE &&
               device.getLimits().maxComputeWorkGroupCount[0] >= limits.pages &&
               device.getLimits().maxComputeWorkGroupCount[0] >= limits.buckets;
    }

    bool Scene::ready()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());   // recursive: the entries below hold it already
        if (!mTried)
        {
            mTried = true;
            mReady = supported() && init();
            if (!mReady)
            {
                LL_WARNS("VKScene") << "retained scene unavailable: every batch is drawn the usual way" << LL_ENDL;
            }
        }
        return mReady && !mBroken;
    }

    bool Scene::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        VmaAllocationCreateInfo where = {};
        where.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (host)
        {
            where.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        VmaAllocationInfo allocated = {};
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &where, &out.buffer, &out.memory, &allocated) != VK_SUCCESS ||
            (host && !allocated.pMappedData))
        {
            out = Buffer();
            return false;
        }
        out.size = size;
        out.mapped = allocated.pMappedData;
        return true;
    }

    bool Scene::init()
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        mStorageAlign = llmax(LLVKDevice::instance().getLimits().minStorageBufferOffsetAlignment, (VkDeviceSize)16);

        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, SCENE_CULL_GLSL } }, mShader) || mShader.modules.empty())
        {
            LL_WARNS("VKScene") << "scene cull shader failed: " << mShader.log << LL_ENDL;
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
        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = sizeof(U32);
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &mSetLayout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
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
            LL_WARNS("VKScene") << "scene cull pipeline could not be created" << LL_ENDL;
            return false;
        }

        // The tables, whole (rows never move): the draws read them by row.
        // research/206: and by address (the sun shadow draws' vertex shader reads records and transforms).
        const VkBufferUsageFlags table_usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkDeviceSize total = 0;
        for (U32 t = 0; t < LLVKSceneTable::TABLE_COUNT; ++t)
        {
            const LLVKSceneTable::Table table = (LLVKSceneTable::Table)t;
            const VkDeviceSize bytes = (VkDeviceSize)mTable.rowCapacity(table) * mTable.rowBytes(table);
            if (!createBuffer(bytes, table_usage, false, mTables[t]))
            {
                LL_WARNS("VKScene") << "scene table " << t << " (" << (bytes >> 10) << " KB) could not be allocated" << LL_ENDL;
                return false;
            }
            total += bytes;
        }
        for (U32 t = 0; t < LLVKSceneTable::TABLE_COUNT; ++t)
        {
            VkBufferDeviceAddressInfo where = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            where.buffer = mTables[t].buffer;
            mTableAddress[t] = vkGetBufferDeviceAddress(device, &where);
        }
        // fix-r8: the own upload room a frame slot may grow to.
        {
            U64 images = 0, usage = 0, budget = 0;
            if (!LLVKDevice::instance().vramBudget(images, usage, budget))
            {
                budget = 0;
            }
            mUploadCap = upload_cap(budget, LLVKDevice::instance().getLimits().maxStorageBufferRange);
        }
        // research/198 5.5: drawing (the scene still chooses without it).
        mExpandReady = initExpand();
        if (mExpandReady)
        {
            total += mTexTable.size;
        }
        LL_INFOS("VKScene") << "retained scene ready (" << LLRHI::sceneModeName(LLRHI::sceneMode()) << "): up to "
                            << mTable.limits().pages * LLVKSceneTable::PAGE << " records, tables " << (total >> 20) << " MB, upload room up to "
                            << (mUploadCap >> 10) << " KB a frame slot"
                            << (mExpandReady ? "" : "; it cannot draw (the expand could not be made): choosing only") << LL_ENDL;
        return true;
    }

    // research/198 5.5: the expand's pipeline and the texture slot table.
    bool Scene::initExpand()
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, SCENE_EXPAND_GLSL } }, mExpandShader) || mExpandShader.modules.empty())
        {
            LL_WARNS("VKScene") << "scene expand shader failed: " << mExpandShader.log << LL_ENDL;
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[EXPAND_BINDINGS] = {};
        for (U32 i = 0; i < EXPAND_BINDINGS; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = EXPAND_BINDINGS;
        set_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &mExpandSetLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &mExpandSetLayout;
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &mExpandLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = mExpandShader.modules[0];
        info.stage.pName = "main";
        info.layout = mExpandLayout;
        if (vkCreateComputePipelines(device, LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &mExpandPipeline) != VK_SUCCESS)
        {
            LL_WARNS("VKScene") << "scene expand pipeline could not be created" << LL_ENDL;
            return false;
        }
        // research/206 5c: also read by address (the shadow renderer's mask shaders).
        if (!createBuffer((VkDeviceSize)TEX_SLOTS * 4,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, mTexTable))
        {
            LL_WARNS("VKScene") << "scene texture slot table could not be allocated" << LL_ENDL;
            return false;
        }
        {
            VkBufferDeviceAddressInfo where = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            where.buffer = mTexTable.buffer;
            mTexTableAddress = vkGetBufferDeviceAddress(device, &where);
        }
        mTexSlots.assign(TEX_SLOTS, LLVKBindless::WHITE_SLOT);
        mTexTableCleared = false;   // cleared to the white slot (0) by the first flush
        return true;
    }

    // The table hands out again what was freed RELEASE_DELAY frames ago. At a
    // new frame's head the records' places are brought up to date, those that
    // went too (their old place was readable until the frame before ended).
    void Scene::syncFrame()
    {
        const U32 frame = LLVKTracker::frame();
        if (!mFrameKnown || frame != mFrame)
        {
            mFrameKnown = true;
            mFrame = frame;
            mTable.beginFrame(frame);
            const bool taken = LLVKAttribPool::numbersTaken();
            if (taken != mNumbersTaken)
            {
                // Draws take places by number now (or no more): verify's sample
                // places are handed out without a word from the pool.
                mNumbersTaken = taken;
                for (const auto& kv : mNumberRecords)
                {
                    mDirtyNumbers.push_back(kv.first);
                }
            }
            applyPlaces(true);
            LLVKAttribPool::sceneDrawing(drawing());
        }
    }

    U32 Scene::addGroup()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a group added");     // research/208 S3: the answer is the tables'
        if (!ready())
        {
            return 0;
        }
        syncFrame();
        mUsed = true;
        return mTable.addGroup();
    }

    void Scene::removeGroup(U32 group)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a group removed");
        syncFrame();
        if (group < mGroupRecords.size() && mGroupRecords[group])
        {
            // The viewer removes a group's records first; any left go with it here.
            for (auto it = mInfos.begin(); it != mInfos.end(); )
            {
                if (it->second.group == group)
                {
                    unlink(it->first, it->second);
                    it = mInfos.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        mTable.removeGroup(group);
    }

    // --- research/198 5.5: records and their places ---------------------------------

    // What the backend keeps of the record. False: it cannot be drawn ever
    // (a texture number past the slot table, a texture transform row whose
    // records have another transform).
    bool Scene::describeInfo(const LLRHI::SceneRecordDesc& desc, RecordInfo& info, U32 replacing) const
    {
        info = RecordInfo();
        info.group = desc.group;
        info.number = desc.range.buffer;
        info.first = desc.range.first_index;
        info.count = desc.range.index_count;
        info.attributes = desc.range.attributes;
        info.transform = desc.transform;
        info.uv = desc.uv_transform;
        info.pass = desc.pass < LLRHI::SCENE_MAX_PASSES ? desc.pass : 0;     // (the table refuses a pass past them)
        info.skinned = desc.palette != LLRHI::SCENE_NO_PALETTE;
        for (U32 k = 0; k < LLRHI::SceneMaterialDesc::TEXTURES; ++k)
        {
            if (desc.material.textures[k].id >= TEX_SLOTS)
            {
                return false;
            }
        }
        if (info.uv)
        {
            const auto owner = mUVOwners.find(info.uv);
            if (owner != mUVOwners.end() && owner->second.transform != info.transform)
            {
                // Another transform's: only if this record is the row's only user.
                const auto old = replacing ? mInfos.find(replacing) : mInfos.end();
                if (old == mInfos.end() || old->second.uv != info.uv || owner->second.users != 1)
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Where the record's indices are now: its number's place, or none.
    U8 Scene::resolvePlace(const RecordInfo& info, LLRHI::SceneRange& range) const
    {
        range = LLRHI::SceneRange();
        range.buffer = info.number;
        range.index_count = info.count;
        range.index32 = true;
        range.attributes = info.attributes;
        range.chunk = LLRHI::SCENE_UNPLACED;
        range.first_index = info.first;
        LLVKAttribPool::NumberPlace place;
        if (!info.number || !placeOf(info.number, place) || place.chunk >= LLRHI::SCENE_UNPLACED)
        {
            return PLACE_NONE;
        }
        if ((U64)info.first + info.count > place.indices)
        {
            return PLACE_RANGE;     // the row stays unplaced: other indices lie there
        }
        range.chunk = place.chunk;
        range.vertex_offset = (S32)place.vertex_base;
        range.first_index = place.index_base + info.first;
        return (info.attributes & ~place.copied) ? PLACE_ATTRIBUTES : PLACE_OK;
    }

    void Scene::setState(RecordInfo& info, U8 state)
    {
        if ((info.state == PLACE_OK) != (state == PLACE_OK) && info.group < mGroupUndrawable.size())
        {
            if (state == PLACE_OK)
            {
                --mGroupUndrawable[info.group];
            }
            else
            {
                ++mGroupUndrawable[info.group];
            }
        }
        info.state = state;
    }

    void Scene::link(U32 id, const RecordInfo& info)
    {
        RecordInfo& r = mInfos[id];
        r = info;
        if (info.group >= mGroupRecords.size())
        {
            const size_t size = std::max((size_t)info.group + 1, mGroupRecords.size() * 2);    // doubling: not per record
            mGroupRecords.resize(size, 0);
            mGroupUndrawable.resize(size, 0);
        }
        ++mGroupRecords[info.group];
        if (info.state != PLACE_OK)
        {
            ++mGroupUndrawable[info.group];
        }
        ++(info.skinned ? mPassSkinned : mPassPlain)[info.pass];     // research/219
        if (info.number)
        {
            mNumberRecords[info.number].push_back(id);
            LLVKAttribPool::numberInterest(info.number, true);
        }
        if (info.uv)
        {
            UVOwner& owner = mUVOwners[info.uv];
            owner.transform = info.transform;
            ++owner.users;
        }
    }

    void Scene::unlink(U32 id, const RecordInfo& info)
    {
        if (info.group < mGroupRecords.size())
        {
            --mGroupRecords[info.group];
            if (info.state != PLACE_OK)
            {
                --mGroupUndrawable[info.group];
            }
        }
        U32& in_pass = (info.skinned ? mPassSkinned : mPassPlain)[info.pass];   // research/219
        in_pass -= in_pass ? 1 : 0;
        if (info.number)
        {
            const auto list = mNumberRecords.find(info.number);
            if (list != mNumberRecords.end())
            {
                std::vector<U32>& ids = list->second;
                const auto at = std::find(ids.begin(), ids.end(), id);
                if (at != ids.end())
                {
                    *at = ids.back();
                    ids.pop_back();
                }
                if (ids.empty())
                {
                    mNumberRecords.erase(list);
                }
            }
            LLVKAttribPool::numberInterest(info.number, false);
        }
        const auto owner = info.uv ? mUVOwners.find(info.uv) : mUVOwners.end();
        if (owner != mUVOwners.end() && !--owner->second.users)
        {
            mUVOwners.erase(owner);
        }
    }

    // The records ray tracing builds from. The tables back from the render
    // thread first (a join, only while ray tracing is on); each record's place
    // is the pool's now, not the row's (a place the row has not caught up with
    // yet is what the pool has).
    bool Scene::collectRayRecords(const U32 pass_mask[4], std::vector<LLVKScene::RayRecord>& out, U64& serial)
    {
        if (!mReady || mBroken)
        {
            return false;       // out is left as it was
        }
        own("ray tracing's records");
        if (mOwnedByThread)
        {
            return false;   // could not take them back here
        }
        out.clear();
        serial = mTable.writeSerial();
        const LLVKSceneTable::Record* records = (const LLVKSceneTable::Record*)mTable.rows(LLVKSceneTable::TABLE_RECORDS);
        const LLVKSceneTable::Transform* transforms = (const LLVKSceneTable::Transform*)mTable.rows(LLVKSceneTable::TABLE_TRANSFORMS);
        const U32 record_span = mTable.rowSpan(LLVKSceneTable::TABLE_RECORDS);
        const U32 transform_span = mTable.rowSpan(LLVKSceneTable::TABLE_TRANSFORMS);
        out.reserve(mInfos.size());
        for (const auto& kv : mInfos)
        {
            const RecordInfo& info = kv.second;
            if (info.skinned || !info.number || info.pass >= LLRHI::SCENE_MAX_PASSES || info.count < 3 ||
                !(pass_mask[info.pass >> 5] & (1u << (info.pass & 31))))
            {
                continue;
            }
            const U32 slot = mTable.slotOf(kv.first);
            if (slot == LLVKSceneTable::NONE || slot >= record_span)
            {
                continue;
            }
            LLVKAttribPool::NumberPlace place;
            if (!LLVKAttribPool::numberPlace(info.number, place) || (place.copied & 1u) == 0 ||
                (U64)info.first + info.count > place.indices)
            {
                continue;
            }
            const LLVKSceneTable::Record& row = records[slot];
            if (row.transform >= transform_span)
            {
                continue;
            }
            LLVKScene::RayRecord r;
            r.id = kv.first;
            r.number = info.number;
            r.chunk = place.chunk;
            r.vertex_base = place.vertex_base;
            r.vertices = place.vertices;
            r.first_index = place.index_base + info.first;
            r.index_count = info.count - info.count % 3;
            r.serial = place.serial;
            r.writes = place.writes;
            memcpy(r.matrix, transforms[row.transform].m, sizeof(r.matrix));
            memcpy(r.lo, row.lo, sizeof(r.lo));
            memcpy(r.hi, row.hi, sizeof(r.hi));
            r.loose = (row.flags & LLVKSceneShader::SCENE_LOOSE) != 0;
            out.push_back(r);
        }
        return true;
    }

    // The pool's word that a number's place changed: its records are not
    // drawable until their rows have the new place (applyPlaces, before the
    // next choice); a record whose place went stops being drawable now.
    void Scene::numberChanged(U32 number)
    {
        if (!mReady || mBroken)
        {
            return;
        }
        if (mOwnedByThread)
        {
            // research/208 S3: the place as the pool has it now, for the tables' owner.
            if (eventRoom(0, 0, 0, 1))
            {
                ScenePlace p;
                p.found = LLVKAttribPool::numberPlace(number, p.place);
                mEventPlaces.push_back(p);
                queueEvent(EVENT_PLACE, number, (U32)mEventPlaces.size() - 1, 0);
                return;
            }
            if (std::this_thread::get_id() != mMainThread)
            {
                mEventDropped = true;   // every number is placed again when the main thread takes the tables back
                ++mS3Dropped;
                return;
            }
            mEventsGrow = true;
            own("a buffer number's place (the event queue full)");
            if (mOwnedByThread)
            {
                mEventDropped = true;   // could not take them back here (inside a stream of its own): as above
                ++mS3Dropped;
                return;
            }
        }
        placeChanged(number);
    }

    // numberChanged's work, by the tables' owner (research/208 S3: also an
    // EVENT_PLACE applied, whose place placeOf then gives).
    void Scene::placeChanged(U32 number)
    {
        ownerCheck("a buffer number's place");
        const auto list = mNumberRecords.find(number);
        if (list == mNumberRecords.end())
        {
            return;
        }
        mDirtyNumbers.push_back(number);
        for (U32 id : list->second)
        {
            const auto it = mInfos.find(id);
            if (it == mInfos.end())
            {
                continue;
            }
            RecordInfo& info = it->second;
            LLRHI::SceneRange range;
            const U8 state = resolvePlace(info, range);
            setState(info, place_state_on_change(state, same_range(range, info.row)));
        }
    }

    // The rows of records whose place changed. A place that went keeps its
    // row until the frame's head (frame_head): the choices of this frame
    // may still draw it from there (the pool hands its ranges out again only
    // frames later), while its group is no longer cut (setState).
    void Scene::applyPlaces(bool frame_head)
    {
        if (mDirtyNumbers.empty())
        {
            return;
        }
        ownerCheck("the records' places");
        std::sort(mDirtyNumbers.begin(), mDirtyNumbers.end());
        mDirtyNumbers.erase(std::unique(mDirtyNumbers.begin(), mDirtyNumbers.end()), mDirtyNumbers.end());
        size_t kept = 0;
        for (size_t n = 0; n < mDirtyNumbers.size(); ++n)
        {
            const U32 number = mDirtyNumbers[n];
            const auto list = mNumberRecords.find(number);
            if (list == mNumberRecords.end())
            {
                continue;
            }
            if (tRunningPass && mCaptured.find(number) == mCaptured.end())
            {
                // research/208 S3: no event told the render thread this number's place (it waits since the main
                // thread last placed it): placed again when the main thread holds the tables.
                mDirtyNumbers[kept++] = number;
                continue;
            }
            bool wait = false;
            for (U32 id : list->second)
            {
                const auto it = mInfos.find(id);
                if (it == mInfos.end())
                {
                    continue;
                }
                RecordInfo& info = it->second;
                LLRHI::SceneRange range;
                const U8 state = resolvePlace(info, range);
                if (same_range(range, info.row))
                {
                    setState(info, state);
                    continue;
                }
                if (range.chunk == LLRHI::SCENE_UNPLACED && info.row.chunk != LLRHI::SCENE_UNPLACED && !frame_head)
                {
                    setState(info, state);
                    wait = true;    // the old place stays readable this frame
                    continue;
                }
                if (mTable.updateRange(id, range))
                {
                    info.row = range;
                    setState(info, state);
                    ++mPlaceUpdates;
                }
                else
                {
                    setState(info, PLACE_STALE);    // the row keeps the old place: not drawn, tried again
                    wait = true;
                    ++mPlaceUpdateFailed;
                }
            }
            if (wait)
            {
                mDirtyNumbers[kept++] = number;
            }
        }
        mDirtyNumbers.resize(kept);
    }

    U32 Scene::addRecord(const LLRHI::SceneRecordDesc& desc)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a record added");
        if (!ready())
        {
            return 0;
        }
        syncFrame();
        RecordInfo info;
        if (!describeInfo(desc, info, 0))
        {
            ++mRefusedRecords;
            return 0;
        }
        LLRHI::SceneRecordDesc placed = desc;
        info.state = resolvePlace(info, placed.range);
        const U32 id = mTable.addRecord(placed);
        if (!id)
        {
            return 0;
        }
        info.row = placed.range;
        link(id, info);
        return id;
    }

    bool Scene::updateRecord(U32 record, const LLRHI::SceneRecordDesc& desc)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a record updated");
        syncFrame();
        const auto it = mInfos.find(record);
        RecordInfo info;
        if (it == mInfos.end() || !describeInfo(desc, info, record))
        {
            ++mRefusedRecords;
            return false;
        }
        LLRHI::SceneRecordDesc placed = desc;
        info.state = resolvePlace(info, placed.range);
        if (!mTable.updateRecord(record, placed))
        {
            return false;
        }
        info.row = placed.range;
        unlink(record, it->second);
        link(record, info);
        return true;
    }

    void Scene::removeRecord(U32 record)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a record removed");    // (the pool's interest in its number)
        syncFrame();
        const auto it = mInfos.find(record);
        if (it != mInfos.end())
        {
            unlink(record, it->second);
            mInfos.erase(it);
        }
        mTable.removeRecord(record);
    }

    U32 Scene::addTransform(const F32 matrix[16])
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a transform row added");
        if (!ready())
        {
            return 0;
        }
        syncFrame();
        return mTable.addTransform(matrix);
    }

    void Scene::setTransform(U32 slot, const F32 matrix[16])
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (mOwnedByThread)     // research/208 S3: for the tables' owner, in order
        {
            if (eventRoom(16, 0, 0, 0))
            {
                mEventFloats.insert(mEventFloats.end(), matrix, matrix + 16);
                queueEvent(EVENT_TRANSFORM, slot, (U32)mEventFloats.size() - 16, 16);
                return;
            }
            mEventsGrow = true;
            own("a transform row written (the event queue full)");
        }
        mTable.setTransform(slot, matrix);
    }

    void Scene::removeTransform(U32 slot)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (mOwnedByThread)     // research/208 S3 (the frame is the owner's already: syncFrame has nothing to do)
        {
            if (eventRoom(0, 0, 0, 0))
            {
                queueEvent(EVENT_TRANSFORM_GONE, slot, 0, 0);
                return;
            }
            mEventsGrow = true;
            own("a transform row removed (the event queue full)");
        }
        syncFrame();
        mTable.removeTransform(slot);
    }

    U32 Scene::addUVTransform(const F32 words[16])
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a texture transform row added");
        if (!ready())
        {
            return 0;
        }
        syncFrame();
        return mTable.addUVTransform(words);
    }

    void Scene::setUVTransform(U32 slot, const F32 words[16])
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (mOwnedByThread)     // research/208 S3
        {
            if (eventRoom(16, 0, 0, 0))
            {
                mEventFloats.insert(mEventFloats.end(), words, words + 16);
                queueEvent(EVENT_UV, slot, (U32)mEventFloats.size() - 16, 16);
                return;
            }
            mEventsGrow = true;
            own("a texture transform row written (the event queue full)");
        }
        mTable.setUVTransform(slot, words);
    }

    void Scene::removeUVTransform(U32 slot)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (mOwnedByThread)     // research/208 S3
        {
            if (eventRoom(0, 0, 0, 0))
            {
                queueEvent(EVENT_UV_GONE, slot, 0, 0);
                return;
            }
            mEventsGrow = true;
            own("a texture transform row removed (the event queue full)");
        }
        syncFrame();
        mTable.removeUVTransform(slot);
    }

    void Scene::setView(U32 view, const LLRHI::SceneViewDesc& desc)
    {
        if (view >= LLRHI::SCENE_VIEWS)
        {
            return;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        mGivenDesc[view] = LLVKTracker::frame() + 1;    // research/208 S3: the main thread's own note
        if (mOwnedByThread)
        {
            if (eventRoom(0, 1, 0, 0))
            {
                mEventViews.push_back(desc);
                queueEvent(EVENT_VIEW, view, (U32)mEventViews.size() - 1, 0);
                return;
            }
            mEventsGrow = true;
            own("a view (the event queue full)");
        }
        ViewState& v = mViews[view];
        v.desc = desc;
        v.desc_frame = LLVKTracker::frame();
        v.choice.valid = false;
        v.matrix_frame = U32_MAX;   // research/209 S5: the rows are made for the view's groups
    }

    void Scene::setGroupVisible(U32 view, const U32* bits, U32 words)
    {
        if (view >= LLRHI::SCENE_VIEWS)
        {
            return;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        mGivenBits[view] = LLVKTracker::frame() + 1;    // research/208 S3
        if (mOwnedByThread)
        {
            const U32 n = bits ? words : 0;
            if (eventRoom(0, 0, n, 0))
            {
                const U32 at = (U32)mEventBits.size();
                mEventBits.insert(mEventBits.end(), bits, bits + n);
                queueEvent(EVENT_BITS, view, at, n);
                return;
            }
            mEventsGrow = true;
            own("a view's groups (the event queue full)");
        }
        ViewState& v = mViews[view];
        v.bits.assign(bits, bits + (bits ? words : 0));
        v.bits_frame = LLVKTracker::frame();
        v.choice.valid = false;
        v.matrix_frame = U32_MAX;
    }

    void Scene::setDrawing(bool on)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("drawing turned on or off");
        mDrawing = on && !mBroken && mBinder && mExpandReady && LLRHI::sceneMode() != LLRHI::SceneMode::Verify;
        LLVKAttribPool::sceneDrawing(drawing());
    }

    bool Scene::canDrawPass(U32 view, U32 pass) const
    {
        if (pass >= LLRHI::SCENE_MAX_PASSES || view >= LLRHI::SCENE_VIEWS)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const U32 frame = LLVKTracker::frame();
        if (mOwnedByThread)
        {
            // research/208 S3: as the tables were when the main thread last held them (the thread refuses
            // a pass this frame for the frames after: the next take-back brings it).
            return pass_open(mRefusalsSeen, view, pass, frame) && upload_open(mShortsSeen, view, frame);
        }
        // fix-r8: not while the view waits after upload shorts (its batches stay in the render map).
        return pass_open(mRefusals, view, pass, frame) && upload_open(mShorts, view, frame);
    }

    // research/204 10 (vk-s4c2): a world pool's pass given to another pool,
    // or its pool binding another program: its refusals start over.
    void Scene::forgetPass(U32 pass)
    {
        if (pass >= LLRHI::SCENE_MAX_PASSES)
        {
            return;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a pass forgotten");    // research/208 S3: the refusals are the tables' owner's
        for (U32 view = 0; view < LLRHI::SCENE_VIEWS; ++view)
        {
            mRefusals.later[view][pass] = 0;
            mRefusalsSeen.later[view][pass] = 0;
            mPassWhy[view][pass] = nullptr;
        }
        mRefusals.never[pass] = 0;
        mRefusalsSeen.never[pass] = 0;
    }

    // research/209 S7: the frame's turn of one kind of the probe faces and
    // mirrors (each kind in turn, on a frame the main view, the shadows and
    // the alpha view leave the verify copy to), until a face of it took it.
    bool Scene::viewTurn(U32 view) const
    {
        if (view == LLRHI::SCENE_VIEW_MAIN || view >= LLRHI::SCENE_VIEW_COUNT)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const U32 frame = LLVKTracker::frame();
        return frame % VERIFY_EVERY == VERIFY_EVERY * 3 / 4 && view == 1 + (frame / VERIFY_EVERY) % (LLRHI::SCENE_VIEW_COUNT - 1) &&
               mOtherTurnFrame != frame;
    }

    bool Scene::groupDrawable(U32 group) const
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const_cast<Scene*>(this)->own("a group's records drawable");     // research/208 S3: the places are the tables'
        return group && group < mGroupRecords.size() && mGroupRecords[group] && !mGroupUndrawable[group];
    }

    // A pass the scene could not draw: not before PASS_RETRY_LONG frames when
    // its program cannot be drawn from rows (permanent: until a shader
    // reload links it again), else not before PASS_RETRY_FRAMES frames.
    void Scene::refusePass(U32 view, U32 pass, const char* why, bool permanent)
    {
        ++mPassesRefused;
        ++mViewPassesRefused[view];
        const bool was = permanent ? mRefusals.never[pass] != 0 : mRefusals.later[view][pass] != 0;
        if (!was || mPassWhy[view][pass] != why)
        {
            LL_INFOS("VKScene") << "retained scene: pass " << pass << " is drawn the usual way "
                                << (permanent ? "(its program, in every view)" : "for now in the view ") << (permanent ? "" : VIEW_NAMES[view])
                                << ": " << (why ? why : "?") << LL_ENDL;
        }
        pass_refuse(mRefusals, view, pass, permanent, LLVKTracker::frame(), permanent ? PASS_RETRY_LONG : PASS_RETRY_FRAMES);
        mPassWhy[view][pass] = why;
    }

    FrameSlot& Scene::frameSlot()
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        FrameSlot& slot = mSlots[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        if (slot.begins != mirror.frameBegins())
        {
            // The slot's last frame is done (beginFrame waited for its fence):
            // its verify copies can be read, its output buffers used again.
            if (slot.verify.pending)
            {
                compare(slot);
            }
            if (slot.expand.pending)
            {
                compareExpand(slot);
            }
            slot.begins = mirror.frameBegins();
            // research/208 S3 (H20): the passes' own descriptor sets of the slot's last frame are done with too.
            const U32 s = mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT;
            for (VkDescriptorPool pool : mThreadPools[s])
            {
                vkResetDescriptorPool(LLVKDevice::instance().getDevice(), pool, 0);
            }
            mThreadPoolIndex[s] = 0;
            slot.scratch_index = 0;
            slot.scratch_head = 0;
            slot.upload_index = 0;
            slot.upload_head = 0;
        }
        return slot;
    }

    bool Scene::scratch(FrameSlot& slot, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset)
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
            // (research/205 3a-5: transfer destination for a sorted pass's command array, cleared before its expand)
            if (!createBuffer(llmax(SCRATCH_BYTES, size),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              false, b))
            {
                return false;
            }
            slot.scratch.push_back(b);
        }
    }

    // fix-r8 (bugloop round 9): room for the scene's uploads this frame (its
    // rows, a view's inputs, an expand's block): the frame slot's own host
    // blocks first, made as the frames need them up to mUploadCap a slot, so
    // what the rest of the frame streams through the frame ring (textures,
    // vertex buffers while a region loads) cannot leave the scene without
    // room; then the frame ring. False: neither has the room (the caller
    // draws that view or pass the usual way this frame).
    bool Scene::uploadRoom(VkDeviceSize size, VkDeviceSize alignment, VkBuffer& buffer, VkDeviceSize& offset, void*& data)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        FrameSlot& slot = frameSlot();
        if (mUploadFrameBegins != slot.begins)
        {
            mUploadFrameBegins = slot.begins;
            mUploadFrame = 0;
        }
        mUploadFrame += size;
        mUploadPeak = llmax(mUploadPeak, mUploadFrame);
        alignment = llmax(alignment, (VkDeviceSize)16);
        for (;;)
        {
            if (slot.upload_index < slot.upload.size())
            {
                Buffer& b = slot.upload[slot.upload_index];
                const VkDeviceSize start = (slot.upload_head + alignment - 1) / alignment * alignment;
                if (start + size <= b.size)
                {
                    slot.upload_head = start + size;
                    buffer = b.buffer;
                    offset = start;
                    data = (U8*)b.mapped + start;
                    return true;
                }
                ++slot.upload_index;
                slot.upload_head = 0;
                continue;
            }
            const VkDeviceSize block = llmax((VkDeviceSize)UPLOAD_BLOCK, size);
            if (slot.upload_bytes + block > mUploadCap)
            {
                break;
            }
            Buffer b;
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = block;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo where = {};
            where.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            where.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            where.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;     // written and read in the frame: no flush
            VmaAllocationInfo allocated = {};
            if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &where, &b.buffer, &b.memory, &allocated) != VK_SUCCESS)
            {
                break;
            }
            if (!allocated.pMappedData)
            {
                vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), b.buffer, b.memory);
                break;
            }
            b.size = block;
            b.mapped = allocated.pMappedData;
            slot.upload.push_back(b);
            slot.upload_bytes += block;
        }
        if (mirror.allocate(size, alignment, buffer, offset, data))
        {
            ++mUploadShared;
            return true;
        }
        return false;
    }

    // fix-r8: a view's choice or pass without upload room this frame (counted, the first few logged).
    void Scene::noRoom(U32 view, const char* what, VkDeviceSize size)
    {
        ++mViewShorts[view];
        if (mUploadLogged < VERIFY_LOG)
        {
            ++mUploadLogged;
            const FrameSlot& slot = mSlots[LLVKMirror::instance().getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
            LL_WARNS("VKScene") << "retained scene: the " << VIEW_NAMES[view] << " view's " << what
                                << " is drawn the usual way this frame: no upload room";
            if (size)
            {
                LL_CONT << " for " << size << " B";
            }
            LL_CONT << " (the scene's uploads this frame " << mUploadFrame << " B, its own room " << (slot.upload_bytes >> 10) << " of "
                    << (mUploadCap >> 10) << " KB a frame slot, the frame ring full); tried again from the next frame (after shorts in a row, "
                    << "up to " << UPLOAD_WAIT_MAX << " frames later)" << LL_ENDL;
        }
    }

    // The rows written since the last choice into the tables (the frame's
    // upload commands run before all of its draws), and the texture slots
    // that changed (research/198 5.5). The copies wait for the draws, choices
    // and expands recorded before that may still read the rows (frames in
    // flight; a row is written in place by setTransform and updateRecord).
    // fix-r8: a table (or the slots) without upload room keeps its rows for
    // the next flush; false then (nothing may be chosen from the tables).
    bool Scene::flush(VkCommandBuffer cmd)
    {
        ownerCheck("the rows' copies");
        const bool tex_writes = mExpandReady && (!mTexTableCleared || !mTexDirty.empty());
        if (!mTable.anyWrites() && !tex_writes)
        {
            return true;
        }
        // research/209 E4: the queued work reads the tables as they are: recorded before the copies change them.
        if (!mQueuedChoices.empty() || !mQueuedExpands.empty())
        {
            ++mBatchCutsAtFlush;
            recordBatch(cmd);
        }
        ++mFlushSerial;
        bool all = true;
        {
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        for (U32 t = 0; t < LLVKSceneTable::TABLE_COUNT; ++t)
        {
            const LLVKSceneTable::Table table = (LLVKSceneTable::Table)t;
            mTable.takeWrites(table, mRuns);
            if (mRuns.empty())
            {
                continue;
            }
            const VkDeviceSize stride = mTable.rowBytes(table);
            VkDeviceSize bytes = 0;
            for (const LLVKSceneTable::Run& run : mRuns)
            {
                bytes += run.count * stride;
            }
            VkBuffer ring = VK_NULL_HANDLE;
            VkDeviceSize ring_offset = 0;
            void* dst = nullptr;
            if (!uploadRoom(bytes, 16, ring, ring_offset, dst))
            {
                mTable.keepWrites(table, mRuns);    // fix-r8: written by a later flush
                all = false;
                continue;
            }
            const U8* rows = (const U8*)mTable.rows(table);
            mRegions.clear();
            VkDeviceSize at = 0;
            for (const LLVKSceneTable::Run& run : mRuns)
            {
                const VkDeviceSize size = run.count * stride;
                memcpy((U8*)dst + at, rows + run.first * stride, (size_t)size);
                VkBufferCopy region = { ring_offset + at, run.first * stride, size };
                mRegions.push_back(region);
                at += size;
            }
            vkCmdCopyBuffer(cmd, ring, mTables[t].buffer, (U32)mRegions.size(), mRegions.data());
            mUploadBytes += bytes;
        }
        if (tex_writes)
        {
            if (!mTexTableCleared)
            {
                // The whole table to the white slot (0) once, before the first slots.
                vkCmdFillBuffer(cmd, mTexTable.buffer, 0, VK_WHOLE_SIZE, LLVKBindless::WHITE_SLOT);
                mTexTableCleared = true;
                VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
                // Every slot not white yet goes in below.
                for (U32 id = 0; id < (U32)mTexSlots.size(); ++id)
                {
                    if (mTexSlots[id] != LLVKBindless::WHITE_SLOT)
                    {
                        mTexDirty.push_back(id);
                    }
                }
            }
            if (!mTexDirty.empty())
            {
                std::sort(mTexDirty.begin(), mTexDirty.end());
                mTexDirty.erase(std::unique(mTexDirty.begin(), mTexDirty.end()), mTexDirty.end());
                const VkDeviceSize bytes = (VkDeviceSize)mTexDirty.size() * 4;
                VkBuffer ring = VK_NULL_HANDLE;
                VkDeviceSize ring_offset = 0;
                void* dst = nullptr;
                if (!uploadRoom(bytes, 16, ring, ring_offset, dst))
                {
                    all = false;    // fix-r8: mTexDirty kept for a later flush
                }
                else
                {
                    // Runs of consecutive numbers: one region each.
                    mRegions.clear();
                    U32* words = (U32*)dst;
                    for (size_t i = 0; i < mTexDirty.size(); ++i)
                    {
                        const U32 id = mTexDirty[i];
                        words[i] = mTexSlots[id];
                        if (!mRegions.empty() && mRegions.back().dstOffset + mRegions.back().size == (VkDeviceSize)id * 4 &&
                            mRegions.back().srcOffset + mRegions.back().size == ring_offset + (VkDeviceSize)i * 4)
                        {
                            mRegions.back().size += 4;
                        }
                        else
                        {
                            mRegions.push_back({ ring_offset + (VkDeviceSize)i * 4, (VkDeviceSize)id * 4, 4 });
                        }
                    }
                    vkCmdCopyBuffer(cmd, ring, mTexTable.buffer, (U32)mRegions.size(), mRegions.data());
                    mUploadBytes += bytes;
                    mTexDirty.clear();
                }
            }
        }
        {
            // (Also after a table without room: the tables that were copied.)
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        return all;
    }

    // The view's choice for this frame, into the upload commands.
    // fix-r8: without upload room (or an output, a descriptor set) the view
    // is drawn the usual way this frame and chosen again the next (later
    // only after shorts in a row: UploadShorts); the scene goes on.
    bool Scene::choose(U32 view_index, bool verify_view)
    {
        ownerCheck("a view's choice");
        ViewState& v = mViews[view_index];
        VkCommandBuffer cmd = uploadCmd();
        if (!cmd)
        {
            return false;   // no frame being recorded
        }
        FrameSlot& slot = frameSlot();
        const U32 frame = LLVKTracker::frame();
        if (!upload_open(mShorts, view_index, frame))
        {
            ++mViewShortSkips[view_index];  // fix-r8: it ran short in the tries before; drawn the usual way a little longer
            return false;
        }
        if (!flush(cmd))
        {
            upload_short(mShorts, view_index, frame);
            noRoom(view_index, "choice (the rows written since the last)", 0);
            return false;
        }

        const LLVKSceneTable::Plan& plan = mTable.plan();
        ViewData view;
        const U32 words = LLVKSceneTable::makeView(v.desc, (U32)v.bits.size(), plan, view);

        // The view's inputs, one block in the upload room (fix-r8: one buffer for the four bindings).
        const VkDeviceSize bits_bytes = llmax((VkDeviceSize)v.bits.size() * 4, (VkDeviceSize)4);
        const VkDeviceSize buckets_bytes = llmax((VkDeviceSize)plan.bucket_info.size() * sizeof(BucketInfo), (VkDeviceSize)sizeof(BucketInfo));
        const VkDeviceSize list_bytes = llmax((VkDeviceSize)plan.page_list.size() * 4, (VkDeviceSize)4);
        auto aligned = [this](VkDeviceSize bytes) { return (bytes + mStorageAlign - 1) / mStorageAlign * mStorageAlign; };
        const VkDeviceSize bits_at = aligned(sizeof(ViewData));
        const VkDeviceSize buckets_at = bits_at + aligned(bits_bytes);
        const VkDeviceSize list_at = buckets_at + aligned(buckets_bytes);
        const VkDeviceSize input_bytes = list_at + list_bytes;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize base = 0;
        void* block = nullptr;
        if (!uploadRoom(input_bytes, mStorageAlign, ring, base, block))
        {
            upload_short(mShorts, view_index, frame);
            noRoom(view_index, "choice", input_bytes);
            return false;
        }
        U8* dst = (U8*)block;
        memset(dst, 0, (size_t)input_bytes);
        memcpy(dst, &view, sizeof(ViewData));
        if (!v.bits.empty())
        {
            memcpy(dst + bits_at, v.bits.data(), v.bits.size() * 4);
        }
        if (!plan.bucket_info.empty())
        {
            memcpy(dst + buckets_at, plan.bucket_info.data(), plan.bucket_info.size() * sizeof(BucketInfo));
        }
        if (!plan.page_list.empty())
        {
            memcpy(dst + list_at, plan.page_list.data(), plan.page_list.size() * 4);
        }
        const VkDeviceSize view_offset = base, bits_offset = base + bits_at, buckets_offset = base + buckets_at, list_offset = base + list_at;

        const VkDeviceSize out_bytes = llmax((VkDeviceSize)words * 4, (VkDeviceSize)4);
        VkBuffer out = VK_NULL_HANDLE;
        VkDeviceSize out_offset = 0;
        if (!scratch(slot, out_bytes, out, out_offset))
        {
            upload_short(mShorts, view_index, frame);
            noRoom(view_index, "choice (no memory for its commands)", out_bytes);
            return false;
        }
        VkDescriptorSet set = allocSet(mSetLayout);
        if (!set)
        {
            upload_short(mShorts, view_index, frame);
            noRoom(view_index, "choice (no descriptor set)", 0);
            return false;
        }
        upload_fit(mShorts, view_index);
        auto table_info = [&](LLVKSceneTable::Table t)
        {
            const VkDeviceSize span = (VkDeviceSize)llmax(mTable.rowSpan(t), 1u) * mTable.rowBytes(t);
            VkDescriptorBufferInfo info = { mTables[t].buffer, 0, llmin(span, mTables[t].size) };
            return info;
        };
        const VkDescriptorBufferInfo infos[BINDINGS] = {
            table_info(LLVKSceneTable::TABLE_RECORDS),
            table_info(LLVKSceneTable::TABLE_TRANSFORMS),
            table_info(LLVKSceneTable::TABLE_PAGES),
            { ring, view_offset, sizeof(ViewData) },
            { ring, bits_offset, bits_bytes },
            { ring, buckets_offset, buckets_bytes },
            { ring, list_offset, list_bytes },
            { out, out_offset, out_bytes },
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

        const U32 groups[PHASES] = { plan.pages, plan.buckets, plan.pages };
        // research/209 E4: queued (recorded at the next cut with the other queued work), or here.
        QueuedChoice* queued = nullptr;
        if (batching())
        {
            mQueuedChoices.emplace_back();
            queued = &mQueuedChoices.back();
            queued->set = set;
            memcpy(queued->groups, groups, sizeof(groups));
            queued->begins = LLVKMirror::instance().frameBegins();
            queued->flushes = mFlushSerial;
            ++mBatchChoices;
        }
        else if (!mQueuedChoices.empty() || !mQueuedExpands.empty())
        {
            recordBatch(cmd);   // what was queued before (the switch went off since) goes first
        }
        // The three phases (scene_cull.comp), each waiting for the one before.
        if (!queued)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &set, 0, nullptr);
            for (U32 phase = 0; phase < PHASES; ++phase)
            {
                if (phase)
                {
                    VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    dependency.memoryBarrierCount = 1;
                    dependency.pMemoryBarriers = &barrier;
                    vkCmdPipelineBarrier2(cmd, &dependency);
                }
                vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(U32), &phase);
                if (groups[phase])
                {
                    vkCmdDispatch(cmd, groups[phase], 1, 1);
                }
            }
            {
                // The counts and commands to the frame's indirect draws, the
                // expands that read them and the verify copy.
                VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            }
        }

        Choice& c = v.choice;
        c.valid = true;
        c.frame = LLVKTracker::frame();
        c.serial = ++mChoiceSerial;
        c.out = out;
        c.offset = out_offset;
        c.words = words;
        c.view = view;
        c.first_command.resize(plan.bucket_info.size());
        c.capacity.resize(plan.bucket_info.size());
        for (size_t b = 0; b < plan.bucket_info.size(); ++b)
        {
            c.first_command[b] = plan.bucket_info[b].first_command;
            c.capacity[b] = plan.bucket_info[b].page_count * LLVKSceneTable::PAGE;
        }
        c.bucket_info = plan.bucket_info;   // keeps its capacity: not per frame once grown
        c.page_list = plan.page_list;
        ++mChoices;
        ++mViewChoices[view_index];
        mUsed = true;

        // The main view's choice (also while drawing: then only the cut groups are visible).
        // research/206: a sun shadow view's on the frames halfway between, one cascade in turn.
        // research/205 3a-3: the alpha view's a quarter of the way.
        const bool shadow_turn = view_index >= LLRHI::SCENE_VIEW_SUN0 && view_index < LLRHI::SCENE_VIEW_SUN_END &&
                                 c.frame % VERIFY_EVERY == VERIFY_EVERY / 2 &&
                                 view_index - LLRHI::SCENE_VIEW_SUN0 ==
                                     (c.frame / VERIFY_EVERY) % (LLRHI::SCENE_VIEW_SUN_END - LLRHI::SCENE_VIEW_SUN0);
        const bool alpha_turn = view_index == LLRHI::SCENE_VIEW_ALPHA && c.frame % VERIFY_EVERY == VERIFY_EVERY / 4;
        // research/209 S7: a probe face's or mirror's turn (the viewer asked viewTurn), three quarters of the way.
        const bool other_turn = verify_view && view_index != LLRHI::SCENE_VIEW_MAIN && view_index < LLRHI::SCENE_VIEW_COUNT &&
                                c.frame % VERIFY_EVERY == VERIFY_EVERY * 3 / 4 && mOtherTurnFrame != c.frame;
        if (other_turn)
        {
            mOtherTurnFrame = c.frame;  // one face a turn, whether the frame's copy is free or not
        }
        if (((view_index == LLRHI::SCENE_VIEW_MAIN && c.frame % VERIFY_EVERY == 0) || shadow_turn || alpha_turn || other_turn) &&
            !slot.verify.pending)
        {
            startVerify(slot, view, words, plan, view_index, cmd, out, out_offset, queued);
        }
        return true;
    }

    // The choice to the host, and the CPU's run of the same functions over the same rows.
    // research/209 E4: queued, the copy waits with the choice (recordBatch).
    void Scene::startVerify(FrameSlot& slot, const ViewData& view, U32 words, const LLVKSceneTable::Plan& plan, U32 view_index,
                            VkCommandBuffer cmd, VkBuffer out, VkDeviceSize out_offset, QueuedChoice* queued)
    {
        const VkDeviceSize bytes = (VkDeviceSize)words * 4;
        if (!bytes)
        {
            return;
        }
        if (slot.readback.size < bytes)
        {
            if (slot.readback.buffer)
            {
                const Buffer old = slot.readback;
                LLVKMirror::instance().defer([old]() { vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), old.buffer, old.memory); });
                slot.readback = Buffer();
            }
            if (!createBuffer(llmax(bytes, (VkDeviceSize)(1u << 20)), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, slot.readback))
            {
                return;     // verify skips this frame
            }
        }
        if (queued)
        {
            queued->out = out;
            queued->out_offset = out_offset;
            queued->readback = slot.readback.buffer;
            queued->readback_bytes = bytes;
        }
        else
        {
            VkBufferCopy region = { out_offset, 0, bytes };
            vkCmdCopyBuffer(cmd, out, slot.readback.buffer, 1, &region);
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        Verify& verify = slot.verify;
        verify.pending = true;
        verify.batched = queued != nullptr;
        verify.frame = LLVKTracker::frame();
        verify.serial = mViews[view_index].choice.serial;
        verify.view_index = view_index;
        verify.words = words;
        verify.view = view;
        const ViewState& v = mViews[view_index];
        mTable.emulate(view, v.bits.empty() ? nullptr : v.bits.data(), plan, verify.expected);
        verify.first_command.resize(plan.bucket_info.size());
        for (size_t b = 0; b < plan.bucket_info.size(); ++b)
        {
            verify.first_command[b] = plan.bucket_info[b].first_command;
        }
        // Slots whose bounds lie within the float slack of a plane: the GPU's
        // float may decide them the other way.
        const U32 slots = plan.pages * LLVKSceneTable::PAGE;
        verify.slack.assign(slots, 0);
        for (U32 s = 0; s < slots; ++s)
        {
            if (mTable.recordAt(s) && std::fabs(mTable.boxExcess(view, s)) < VERIFY_SLACK)
            {
                verify.slack[s] = 1;
            }
        }
    }

    // A verify copy whose frame is done: the GPU's choice against the CPU's.
    void Scene::compare(FrameSlot& slot)
    {
        Verify& verify = slot.verify;
        verify.pending = false;
        if (!slot.readback.mapped)
        {
            return;
        }
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), slot.readback.memory, 0, (VkDeviceSize)verify.words * 4);
        const U32* got = (const U32*)slot.readback.mapped;
        const U32* want = verify.expected.data();
        const ViewData& view = verify.view;
        ++mCompared;
        U32 differences = 0;
        for (U32 b = 0; b < view.buckets; ++b)
        {
            // Each side's commands for the bucket, leaving out the slots within the slack.
            auto gather = [&](const U32* words, std::vector<U32>& out)
            {
                out.clear();
                const U32 count = words[view.count_base + b];
                const U32 first = verify.first_command[b];
                for (U32 i = 0; i < count && view.command_base + (first + i) * 5 + 4 < verify.words; ++i)
                {
                    const U32* c = words + view.command_base + (first + i) * 5;
                    if (c[4] < verify.slack.size() && verify.slack[c[4]])
                    {
                        continue;
                    }
                    out.insert(out.end(), c, c + 5);
                }
            };
            gather(got, mGot);
            gather(want, mWant);
            if (mGot != mWant)
            {
                ++differences;
                if (mLogged < VERIFY_LOG)
                {
                    ++mLogged;
                    LL_WARNS("VKScene") << "verify: frame " << verify.frame << " view " << verify.view_index << " bucket " << b << " (pass "
                                        << mTable.bucketKey(b).pass << "): the GPU drew " << mGot.size() / 5 << " records, the CPU "
                                        << mWant.size() / 5 << LL_ENDL;
                }
            }
        }
        mMismatches += differences;
        mProblems += differences;
        if (verify.batched)
        {
            mBatchCompared.fetch_add(1, std::memory_order_relaxed);     // research/209 E4
            mBatchDiffered.fetch_add(differences, std::memory_order_relaxed);
        }
    }

    // --- research/198 5.5: drawing ------------------------------------------------

    // The slot of every texture the view's drawn records sample (the viewer's
    // list: the cut groups' textures), as the usual way resolves it for this
    // pass's target; the GPU table follows at the next flush. A texture no
    // record draws this frame is left alone (its old slot stays in the
    // table, read by no draw), so it keeps no bindless slot in use.
    void Scene::resolveTextures(const LLRHI::SceneDrawContext& ctx)
    {
        for (U32 i = 0; i < ctx.texture_count; ++i)
        {
            const U32 id = ctx.textures[i];
            if (!id || id >= TEX_SLOTS)
            {
                continue;
            }
            const U32 slot = mBinder->textureSlot(id);
            ++mTexResolved;
            mTexWhite += slot == LLVKBindless::WHITE_SLOT ? 1 : 0;
            if (mTexSlots[id] != slot)
            {
                mTexSlots[id] = slot;
                mTexDirty.push_back(id);
                ++mTexChanged;
            }
        }
        mBinder->texturesDone();
    }

    // The view's matrix rows in the frame's matrix table, as the usual path's
    // draws would have them (the viewer's arithmetic, ctx.fill): a row per
    // transform row (row 0: no model matrix, the view as it is), then a row
    // per texture transform row with its records' transform and the texture
    // matrix (a texture animation's description turned into its matrix now).
    // research/209 S5: with the viewer's lists, only the rows its visible
    // groups' records read are made (row 0 always; the table keeps the
    // places of the others, which no record of the view reads).
    bool Scene::matrixRows(U32 view_index, ViewState& v, const LLRHI::SceneDrawContext& ctx)
    {
        if (!ctx.fill || !ctx.view_matrix || !ctx.projection)
        {
            return false;
        }
        const U32 frame = LLVKTracker::frame();
        const U32 transforms = mTable.rowSpan(LLVKSceneTable::TABLE_TRANSFORMS);
        const U32 uvs = mTable.rowSpan(LLVKSceneTable::TABLE_UV);
        if (v.matrix_frame == frame && v.matrix_transforms == transforms && v.matrix_uvs == uvs &&
            !memcmp(v.matrix_view, ctx.view_matrix, sizeof(v.matrix_view)) &&
            !memcmp(v.matrix_projection, ctx.projection, sizeof(v.matrix_projection)))
        {
            return true;    // this frame's, for the same view
        }
        // research/209 S5: the probe faces and the mirrors take at most
        // OTHER_MATRIX_ROWS of the table a frame (the main view's draws come
        // after them and share the table).
        const bool other = !view_is_main(view_index);
        if (mMatrixFrame != frame)
        {
            mMatrixFrame = frame;
            mMatrixOthers = 0;
        }
        if (other && (U64)mMatrixOthers + transforms + uvs > OTHER_MATRIX_ROWS)
        {
            return false;
        }
        ownerCheck("a view's matrix rows");
        U32 first = 0;
        LLVKBindless::DrawMatrices* rows = takeMatrixRows(view_index, transforms + uvs, first);
        if (!rows)
        {
            return false;
        }
        mMatrixOthers += other ? transforms + uvs : 0;
        static const F32 IDENTITY[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };
        const LLVKSceneTable::Transform* t = (const LLVKSceneTable::Transform*)mTable.rows(LLVKSceneTable::TABLE_TRANSFORMS);
        const LLVKSceneTable::Transform* uv = (const LLVKSceneTable::Transform*)mTable.rows(LLVKSceneTable::TABLE_UV);
        auto write = [&](LLVKBindless::DrawMatrices& row, U32 transform, const F32* texture)
        {
            F32 normal[9];
            ctx.fill(ctx.view_matrix, ctx.projection, transform ? t[transform].m : nullptr, row.modelview, row.modelview_projection,
                     normal);
            for (U32 c = 0; c < 3; ++c)     // mat3 in std430: three columns of four floats
            {
                row.normal[c * 4 + 0] = normal[c * 3 + 0];
                row.normal[c * 4 + 1] = normal[c * 3 + 1];
                row.normal[c * 4 + 2] = normal[c * 3 + 2];
                row.normal[c * 4 + 3] = 0.f;
            }
            memcpy(row.texture0, texture, sizeof(row.texture0));
        };
        auto write_uv = [&](U32 u)
        {
            const auto owner = u ? mUVOwners.find(u) : mUVOwners.end();
            const U32 transform = owner != mUVOwners.end() && owner->second.transform < transforms ? owner->second.transform : 0;
            F32 scratch[16];
            const F32* texture = u ? LLRHI::texAnimMatrix(uv[u].m, scratch) : IDENTITY;
            write(rows[transforms + u], transform, texture);
        };
        U32 made = 0;
        if (ctx.transform_slots && ctx.uv_slots)
        {
            if (transforms)
            {
                write(rows[0], 0, IDENTITY);
                ++made;
            }
            if (uvs)
            {
                write_uv(0);
                ++made;
            }
            for (U32 k = 0; k < ctx.transform_slot_count; ++k)
            {
                const U32 i = ctx.transform_slots[k];
                if (i && i < transforms)
                {
                    write(rows[i], i, IDENTITY);
                    ++made;
                }
            }
            for (U32 k = 0; k < ctx.uv_slot_count; ++k)
            {
                const U32 u = ctx.uv_slots[k];
                if (u && u < uvs)
                {
                    write_uv(u);
                    ++made;
                }
            }
        }
        else
        {
            for (U32 i = 0; i < transforms; ++i)
            {
                write(rows[i], i, IDENTITY);
            }
            for (U32 u = 0; u < uvs; ++u)
            {
                write_uv(u);
            }
            made = transforms + uvs;
        }
        v.matrix_frame = frame;
        v.matrix_base = first;
        v.matrix_transforms = transforms;
        v.matrix_uvs = uvs;
        memcpy(v.matrix_view, ctx.view_matrix, sizeof(v.matrix_view));
        memcpy(v.matrix_projection, ctx.projection, sizeof(v.matrix_projection));
        mMatrixRows += made;
        mViewMatrixRows[view_index] += made;
        return true;
    }

    // What the expand writes for a record's slot (the CPU's copy of the same
    // rows: verify).
    void Scene::expectedRow(U32 slot, const LLVKScene::Binder::Words& words, const WordTextures& word_textures,
                            const LLRHI::SceneDrawContext& ctx, const ViewState& v, U32 vec4s, U32* out) const
    {
        const LLVKSceneTable::Record& r = ((const LLVKSceneTable::Record*)mTable.rows(LLVKSceneTable::TABLE_RECORDS))[slot];
        const LLRHI::SceneMaterialDesc* materials = (const LLRHI::SceneMaterialDesc*)mTable.rows(LLVKSceneTable::TABLE_MATERIALS);
        const LLVKSceneTable::TextureList* lists = (const LLVKSceneTable::TextureList*)mTable.rows(LLVKSceneTable::TABLE_TEXLISTS);
        const U32 list = LLVKSceneShader::record_list(r.flags);     // research/205 H5 (research/219: none when rigged)
        const U32 lists_held = mTable.rowSpan(LLVKSceneTable::TABLE_TEXLISTS);
        const U32 matrix_row = v.matrix_base + (r.uv_transform ? v.matrix_transforms + r.uv_transform : r.transform);
        const U32 word_vec4s = row_word_vec4s(words);
        for (U32 w = 0; w < word_vec4s * 4; ++w)
        {
            U32 value = 0;
            if (w == words.priority_word && w >= words.count)
            {
                // research/205 3a-3: as the expand (the place a whole number in a float).
                const U32 place = (U32)materials[r.material].values[ctx.place_value];
                value = (r.group_slot < ctx.group_priority_count ? ctx.group_priority[r.group_slot] : 0u) |
                        (ctx.glow_priority ? 0xfffu : llmin(place, 4094u));
            }
            else if (w < words.count)
            {
                value = words.words[w];
                if (word_textures.list[w] >= 0 && list && list < lists_held)
                {
                    const U32 id = lists[list].textures[word_textures.list[w]];
                    value = id && id < TEX_SLOTS ? mTexSlots[id] : LLVKBindless::WHITE_SLOT;
                }
                else if (word_textures.texture[w] >= 0)
                {
                    const U32 id = materials[r.material].textures[word_textures.texture[w]].id;
                    value = id && id < TEX_SLOTS ? mTexSlots[id] : LLVKBindless::WHITE_SLOT;
                }
                if (w == words.matrix_word)
                {
                    value = matrix_row;
                }
                value = LLVKSceneShader::expand_palette_word(r.flags, w, words.palette_word, value);    // research/219
                if (w == words.material_word)
                {
                    value = 0;  // where the row lands (compareExpand puts it in)
                }
            }
            out[w] = value;
        }
        // research/198 5.6: the record's material row.
        for (U32 f = word_vec4s * 4; f < vec4s * 4; ++f)
        {
            out[f] = 0;
        }
        if (words.material_word != U32_MAX)
        {
            U32* m = out + word_vec4s * 4;
            memcpy(m, words.material, (size_t)llmin(words.material_vec4s, vec4s - word_vec4s) * 16);
            for (U32 k = 0; k < LLRHI::SceneMaterialDesc::VALUES; ++k)
            {
                const S32 at = words.material_at[k];
                if (at >= 0 && (U32)at < words.material_vec4s * 4)
                {
                    memcpy(&m[at], &materials[r.material].values[k], 4);
                }
            }
        }
    }

    // research/205 3a-5: as the expand, the command a record's goes to in a
    // sorted pass's array of `commands` (its group's base plus its place;
    // NONE: none).
    U32 Scene::sortedPlace(U32 slot, const LLRHI::SceneDrawContext& ctx, U32 commands) const
    {
        const LLVKSceneTable::Record& r = ((const LLVKSceneTable::Record*)mTable.rows(LLVKSceneTable::TABLE_RECORDS))[slot];
        const LLRHI::SceneMaterialDesc* materials = (const LLRHI::SceneMaterialDesc*)mTable.rows(LLVKSceneTable::TABLE_MATERIALS);
        if (!ctx.group_base || r.group_slot >= ctx.group_base_count)
        {
            return NONE;
        }
        const U32 place = llmin((U32)materials[r.material].values[ctx.place_value], 4094u);
        const U32 at = ctx.group_base[r.group_slot] + place;
        return at < commands ? at : NONE;
    }

    // Verify: the records of the pass's buckets in the choice against the
    // pool (a row's place is its number's place now). Rows whose place went
    // keep it until the frame's head (applyPlaces): not counted; nor records
    // that neither side would draw (place_check).
    void Scene::checkPlaces(U32 view_index, U32 pass)
    {
        const Choice& c = mViews[view_index].choice;
        for (U32 b : mTable.passBuckets(pass))
        {
            if (b >= c.bucket_info.size())
            {
                continue;
            }
            const BucketInfo& info = c.bucket_info[b];
            for (U32 p = 0; p < info.page_count && info.first_listed + p < c.page_list.size(); ++p)
            {
                const U32 page = c.page_list[info.first_listed + p];
                for (U32 lane = 0; lane < LLVKSceneTable::PAGE; ++lane)
                {
                    const U32 id = mTable.recordAt(page * LLVKSceneTable::PAGE + lane);
                    const auto it = id ? mInfos.find(id) : mInfos.end();
                    if (it == mInfos.end())
                    {
                        continue;
                    }
                    LLRHI::SceneRange range;
                    const U8 state = resolvePlace(it->second, range);
                    const U8 check = place_check(it->second.row.chunk != LLRHI::SCENE_UNPLACED, it->second.state,
                                                 range.chunk != LLRHI::SCENE_UNPLACED, state, same_range(range, it->second.row));
                    if (check == PLACE_CHECK_SKIP)
                    {
                        continue;
                    }
                    ++mPlaceChecked;
                    if (check == PLACE_CHECK_DIFFERS)
                    {
                        ++mPlaceMismatches;
                        ++mProblems;
                        if (mLogged < VERIFY_LOG)
                        {
                            ++mLogged;
                            LL_WARNS("VKScene") << "verify: record " << id << " of buffer number " << it->second.number << " has chunk "
                                                << it->second.row.chunk << " vertex " << it->second.row.vertex_offset << " index "
                                                << it->second.row.first_index << " (" << PLACE_NAMES[it->second.state] << "), the pool chunk "
                                                << range.chunk << " vertex " << range.vertex_offset << " index " << range.first_index << " ("
                                                << PLACE_NAMES[state] << ")" << LL_ENDL;
                        }
                    }
                }
            }
        }
    }

    // The pass's rows and commands from the view's choice (the expand, into
    // the upload commands, which run before every draw of the frame).
    bool Scene::expand(U32 view_index, U32 pass, const LLVKScene::Binder::Words& words, const WordTextures& word_textures,
                       const LLRHI::SceneDrawContext& ctx, bool verify, ExpandOut& out)
    {
        out = ExpandOut();
        ViewState& v = mViews[view_index];
        const Choice& c = v.choice;
        mExpandBuckets.clear();
        U32 total = 0;
        for (U32 b : mTable.passBuckets(pass))
        {
            if (b >= c.capacity.size() || !c.capacity[b] || mTable.bucketKey(b).chunk >= LLRHI::SCENE_UNPLACED)
            {
                continue;   // no pages in the choice, or records without a place (never drawn)
            }
            mExpandBuckets.push_back(b);
            mExpandBuckets.push_back(c.first_command[b]);
            mExpandBuckets.push_back(total);
            mExpandBuckets.push_back(c.capacity[b]);
            total += c.capacity[b];
        }
        if (!total)
        {
            return true;    // nothing of the pass to draw
        }
        const U32 frame = LLVKTracker::frame();
        // A record's row: its words (research/205 3a-3: and its priority
        // word), then (research/198 5.6) its own material row.
        const U32 word_vec4s = row_word_vec4s(words);
        const U32 material_vec4s = words.material_word != U32_MAX ? words.material_vec4s : 0;
        const U32 vec4s = word_vec4s + material_vec4s;
        const U32 buckets = (U32)mExpandBuckets.size() / 4;
        ownerCheck("an expand");
        LLVKMirror& mirror = LLVKMirror::instance();
        VkCommandBuffer cmd = uploadCmd();
        VkBuffer palette = LLVKBindless::paletteBuffer();
        if (!cmd || !palette)
        {
            return false;
        }

        // The rows in the frame's palette table, aligned for the descriptor.
        // research/209 S6: within the view's share.
        if (mRowFrame != frame)
        {
            mRowFrame = frame;
            mRowVec4s = 0;
            mRowVec4sOthers = 0;
            memset(mRowVec4sView, 0, sizeof(mRowVec4sView));
        }
        const U32 align = (U32)(mStorageAlign / 16);
        const U32 want = total * vec4s + align - 1;
        U32 base = 0;
        if (!rows_fit(view_index, mRowVec4s, mRowVec4sOthers, mRowVec4sView[view_index], want, ROW_VEC4S_PER_FRAME) ||
            !takePaletteRows(view_index, want, base))
        {
            return false;
        }
        mRowVec4s += want;
        mRowVec4sOthers += view_is_main(view_index) ? 0 : want;
        mRowVec4sView[view_index] += want;
        base = (base + align - 1) / align * align;

        // research/205 3a-5: sorted, the commands go to fixed places of an
        // array (the groups' bases plus the records' places).
        const bool sorted = ctx.group_base != nullptr;
        const U32 command_count = sorted ? ctx.sorted_commands : total;
        if (!command_count)
        {
            return false;
        }
        FrameSlot& slot = frameSlot();
        VkBuffer commands = VK_NULL_HANDLE;
        VkDeviceSize commands_offset = 0;
        if (!scratch(slot, (VkDeviceSize)command_count * COMMAND_BYTES, commands, commands_offset))
        {
            out.no_room = true;     // fix-r8
            return false;
        }

        // The block: what every thread reads, and the pass's buckets.
        ExpandBlock block = {};
        block.params[0] = buckets;
        block.params[1] = words.count;
        block.params[2] = vec4s;
        block.params[3] = base;
        block.params2[0] = words.matrix_word;
        block.params2[1] = v.matrix_base;
        block.params2[2] = v.matrix_transforms;
        block.params2[3] = TEX_SLOTS;
        block.params3[0] = c.view.count_base;
        block.params3[1] = c.view.command_base;
        block.params3[2] = total;
        block.params3[3] = LLVKBindless::WHITE_SLOT;
        for (U32 w = 0; w < LLVKScene::Binder::Words::MAX; ++w)
        {
            block.words[w] = w < words.count ? words.words[w] : 0;
            block.word_texture[w] = w < words.count ? word_textures.texture[w] : -1;
            block.word_list[w] = w < words.count ? word_textures.list[w] : -1;
        }
        block.params4[0] = material_vec4s ? words.material_word : U32_MAX;
        block.params4[1] = material_vec4s;
        block.params4[2] = word_vec4s;
        block.params4[3] = words.palette_word;     // research/219
        memcpy(block.material, words.material, sizeof(block.material));
        // research/205 3a-3: the priority word, the groups' priorities, the place's material value.
        block.params5[0] = words.priority_word;
        block.params5[1] = words.priority_word != U32_MAX ? ctx.group_priority_count : 0;
        block.params5[2] = ctx.place_value;
        block.params5[3] = ctx.glow_priority ? 1 : 0;   // research/205 (vk-s3a6): the glow pass
        // research/205 3a-5: the groups' bases after the priorities in binding 7.
        const U32 priorities = words.priority_word != U32_MAX ? ctx.group_priority_count : 0;
        const U32 bases = sorted ? ctx.group_base_count : 0;
        block.params6[0] = priorities;
        block.params6[1] = bases;
        block.params6[2] = command_count;
        block.params6[3] = sorted ? 1 : 0;
        for (U32 k = 0; k < 20; ++k)
        {
            block.material_at[k] = k < LLRHI::SceneMaterialDesc::VALUES && material_vec4s ? words.material_at[k] : -1;
        }
        const VkDeviceSize block_bytes = sizeof(ExpandBlock) + (VkDeviceSize)mExpandBuckets.size() * 4;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize block_offset = 0;
        void* dst = nullptr;
        if (!uploadRoom(block_bytes, mStorageAlign, ring, block_offset, dst))
        {
            out.no_room = true;     // fix-r8
            return false;
        }
        memcpy(dst, &block, sizeof(ExpandBlock));
        memcpy((U8*)dst + sizeof(ExpandBlock), mExpandBuckets.data(), mExpandBuckets.size() * 4);

        // research/205 3a-3: the groups' priorities this frame (a word when
        // there are none: the binding needs a range); 3a-5: then their bases.
        const VkDeviceSize priority_bytes = llmax((VkDeviceSize)(priorities + bases) * 4, (VkDeviceSize)4);
        VkBuffer priority_ring = VK_NULL_HANDLE;
        VkDeviceSize priority_offset = 0;
        if (!uploadRoom(priority_bytes, mStorageAlign, priority_ring, priority_offset, dst))
        {
            out.no_room = true;     // fix-r8
            return false;
        }
        memset(dst, 0, (size_t)priority_bytes);
        if (priorities)
        {
            memcpy(dst, ctx.group_priority, (size_t)priorities * 4);
        }
        if (bases)
        {
            memcpy((U8*)dst + (size_t)priorities * 4, ctx.group_base, (size_t)bases * 4);
        }

        VkDescriptorSet set = allocSet(mExpandSetLayout);
        if (!set)
        {
            out.no_room = true;     // fix-r8
            return false;
        }
        // The rows and records written since the choice (places, slots) go in first.
        if (!flush(cmd))
        {
            out.no_room = true;     // fix-r8: the rows wait for a later flush
            return false;
        }
        auto table_info = [&](LLVKSceneTable::Table t)
        {
            const VkDeviceSize span = (VkDeviceSize)llmax(mTable.rowSpan(t), 1u) * mTable.rowBytes(t);
            VkDescriptorBufferInfo info = { mTables[t].buffer, 0, llmin(span, mTables[t].size) };
            return info;
        };
        const VkDescriptorBufferInfo infos[EXPAND_BINDINGS] = {
            table_info(LLVKSceneTable::TABLE_RECORDS),
            table_info(LLVKSceneTable::TABLE_MATERIALS),
            { mTexTable.buffer, 0, mTexTable.size },
            { c.out, c.offset, llmax((VkDeviceSize)c.words * 4, (VkDeviceSize)4) },
            { ring, block_offset, block_bytes },
            { palette, (VkDeviceSize)base * 16, (VkDeviceSize)total * vec4s * 16 },
            { commands, commands_offset, (VkDeviceSize)command_count * COMMAND_BYTES },
            { priority_ring, priority_offset, priority_bytes },
            table_info(LLVKSceneTable::TABLE_TEXLISTS),     // research/205 H5
        };
        VkWriteDescriptorSet writes[EXPAND_BINDINGS] = {};
        for (U32 i = 0; i < EXPAND_BINDINGS; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), EXPAND_BINDINGS, writes, 0, nullptr);
        // research/208 S2 (10.4): all the room is taken and written above (this
        // thread, at the pass's entry); the commands are values from here.
        ExpandRecord rec;
        rec.cmd = cmd;
        rec.pipeline = mExpandPipeline;
        rec.layout = mExpandLayout;
        rec.set = set;
        rec.sorted = sorted;
        rec.commands = commands;
        rec.commands_offset = commands_offset;
        rec.commands_bytes = (VkDeviceSize)command_count * COMMAND_BYTES;
        rec.groups = (total + EXPAND_LOCAL - 1) / EXPAND_LOCAL;
        // research/209 E4: queued for the next cut (its verify copies too), or recorded now.
        QueuedExpand* queued = nullptr;
        if (batching())
        {
            mQueuedExpands.emplace_back();
            queued = &mQueuedExpands.back();
            queued->rec = rec;
            queued->begins = mirror.frameBegins();
            queued->flushes = mFlushSerial;
            ++mBatchExpands;
        }
        else
        {
            if (!mQueuedChoices.empty() || !mQueuedExpands.empty())
            {
                recordBatch(cmd);   // what was queued before (the switch went off since) goes first
            }
            recordExpand(rec, view_index, pass, verify);
        }
        ++mExpands;
        mExpandCommands += total;

        if (verify)
        {
            // The output to the host, beside the frame's choice (compareExpand),
            // and the rows the CPU makes of the same records (as flushed above).
            ExpandVerify& e = slot.expand;
            const VkDeviceSize command_bytes = (VkDeviceSize)command_count * COMMAND_BYTES;
            const VkDeviceSize row_bytes = (VkDeviceSize)total * vec4s * 16;
            if (e.readback.size < command_bytes + row_bytes)
            {
                if (e.readback.buffer)
                {
                    const Buffer old = e.readback;
                    mirror.defer([old]() { vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), old.buffer, old.memory); });
                    e.readback = Buffer();
                }
                createBuffer(llmax(command_bytes + row_bytes, (VkDeviceSize)(1u << 20)), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, e.readback);
            }
            if (e.readback.buffer)
            {
                const VkBufferCopy command_region = { commands_offset, 0, command_bytes };
                const VkBufferCopy row_region = { (VkDeviceSize)base * 16, command_bytes, row_bytes };
                if (queued)
                {
                    queued->readback = e.readback.buffer;
                    queued->command_copy = command_region;
                    queued->palette = palette;
                    queued->row_copy = row_region;
                }
                else
                {
                    vkCmdCopyBuffer(cmd, commands, e.readback.buffer, 1, &command_region);
                    vkCmdCopyBuffer(cmd, palette, e.readback.buffer, 1, &row_region);
                    VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    dependency.memoryBarrierCount = 1;
                    dependency.pMemoryBarriers = &barrier;
                    vkCmdPipelineBarrier2(cmd, &dependency);
                }

                e.pending = true;
                e.batched = queued != nullptr;
                e.frame = frame;
                e.serial = c.serial;
                e.view_index = view_index;
                e.pass = pass;
                e.total = total;
                e.commands = command_count;
                e.sorted = sorted;
                e.vec4s = vec4s;
                e.word_vec4s = word_vec4s;
                e.material_word = material_vec4s ? words.material_word : U32_MAX;
                e.rows_base = base;
                e.count_base = c.view.count_base;
                e.command_base = c.view.command_base;
                e.buckets = mExpandBuckets;
                // The pass's pages as the choice laid them out, and each slot's row.
                U32 max_page = 0;
                for (U32 k = 0; k < buckets; ++k)
                {
                    const BucketInfo& info = c.bucket_info[mExpandBuckets[k * 4]];
                    for (U32 p = 0; p < info.page_count && info.first_listed + p < c.page_list.size(); ++p)
                    {
                        max_page = llmax(max_page, c.page_list[info.first_listed + p]);
                    }
                }
                e.page_local.assign((size_t)max_page + 1, NONE);
                U32 local = 0;
                for (U32 k = 0; k < buckets; ++k)
                {
                    const BucketInfo& info = c.bucket_info[mExpandBuckets[k * 4]];
                    for (U32 p = 0; p < info.page_count && info.first_listed + p < c.page_list.size(); ++p)
                    {
                        e.page_local[c.page_list[info.first_listed + p]] = local++;
                    }
                }
                const U32 row_words = vec4s * 4;
                e.expected.resize((size_t)local * LLVKSceneTable::PAGE * row_words);
                e.expected_at.assign(sorted ? (size_t)local * LLVKSceneTable::PAGE : 0, NONE);
                e.expected_none.assign((size_t)local * LLVKSceneTable::PAGE, 0);
                const LLVKSceneTable::Record* records = (const LLVKSceneTable::Record*)mTable.rows(LLVKSceneTable::TABLE_RECORDS);
                for (U32 page = 0; page <= max_page; ++page)
                {
                    const U32 at = e.page_local[page];
                    if (at == NONE)
                    {
                        continue;
                    }
                    for (U32 lane = 0; lane < LLVKSceneTable::PAGE; ++lane)
                    {
                        expectedRow(page * LLVKSceneTable::PAGE + lane, words, word_textures, ctx, v, vec4s,
                                    &e.expected[((size_t)at * LLVKSceneTable::PAGE + lane) * row_words]);
                        e.expected_none[(size_t)at * LLVKSceneTable::PAGE + lane] =
                            LLVKSceneShader::expand_draws_none(records[page * LLVKSceneTable::PAGE + lane].flags, words.palette_word) ? 1 : 0;
                        if (sorted)
                        {
                            e.expected_at[(size_t)at * LLVKSceneTable::PAGE + lane] =
                                sortedPlace(page * LLVKSceneTable::PAGE + lane, ctx, command_count);
                        }
                    }
                }
            }
            checkPlaces(view_index, pass);
        }
        mPriorityRows += priorities ? total : 0;

        out.buffer = commands;
        out.offset = commands_offset;
        out.total = total;
        return true;
    }

    // research/208 S2: a job no one holds (each is free again after the join), null: none (the main thread records).
    ExpandJob* Scene::freeExpandJob()
    {
        const size_t n = mExpandJobs.size();
        for (size_t i = 0; i < n; ++i)
        {
            ExpandJob* job = mExpandJobs[(mExpandJobNext + i) % n].get();
            if (job->idle())
            {
                mExpandJobNext = (mExpandJobNext + i + 1) % n;
                return job;
            }
        }
        if (n >= MAX_RENDER_JOBS)
        {
            return nullptr;
        }
        mExpandJobs.push_back(std::make_unique<ExpandJob>());
        return mExpandJobs.back().get();
    }

    // research/208 S2: the expand's commands where the main thread stands in
    // the frame's upload commands: by the render thread into a stream spliced
    // here (the main and alpha views' passes, not an expand compared with the
    // CPU's rows: its copies follow at once), or here; verify also hands the
    // same values to the thread to compare at the join.
    void Scene::recordExpand(const ExpandRecord& rec, U32 view_index, U32 pass, bool verify)
    {
        if (tRunningPass)
        {
            record_expand(rec);     // research/208 S3: a pass's run records it where its stream is
            return;
        }
        const LLVKRender::Use use = !verify && view_is_main(view_index) ? LLVKRender::use() : LLVKRender::Use::Inline;
        if (use == LLVKRender::Use::Thread)
        {
            ExpandJob* job = freeExpandJob();
            if (job)
            {
                job->rec = rec;
                job->mPass = pass;
                if (LLVKRender::post(*job))
                {
                    ++mExpandsByThread;
                    return;
                }
            }
        }
        record_expand(rec);
        if (use == LLVKRender::Use::Verify)
        {
            ExpandJob* job = freeExpandJob();
            if (job)
            {
                job->rec = rec;
                job->mPass = pass;
                job->mMainDigest = expand_digest(rec);
                LLVKRender::postVerify(*job);
            }
        }
    }

    // An expand's output whose frame is done, against the CPU's rows for the
    // GPU's own choice of that frame (Verify's copy): the commands (count,
    // order, ranges, rows named) and every row's words.
    void Scene::compareExpand(FrameSlot& slot)
    {
        ExpandVerify& e = slot.expand;
        e.pending = false;
        if (!e.readback.mapped || !slot.readback.mapped || slot.verify.frame != e.frame || slot.verify.view_index != e.view_index ||
            slot.verify.serial != e.serial)
        {
            return;     // research/209: another face's choice of the kind
        }
        const VkDeviceSize command_bytes = (VkDeviceSize)e.commands * COMMAND_BYTES;
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), e.readback.memory, 0,
                                command_bytes + (VkDeviceSize)e.total * e.vec4s * 16);
        const U32* choice = (const U32*)slot.readback.mapped;
        const U32* got = (const U32*)e.readback.mapped;
        const U32* rows = got + (size_t)e.commands * 5;
        // research/205 3a-5: sorted, which of the array's commands a chosen record wrote.
        mSortedWritten.assign(e.sorted ? e.commands : 0, 0);
        const U32 row_words = e.vec4s * 4;
        const U32 choice_words = slot.verify.words;
        U32 differences = 0;
        const char* what = nullptr;
        for (size_t k = 0; k + 3 < e.buckets.size(); k += 4)
        {
            const U32 b = e.buckets[k];
            const U32 first_command = e.buckets[k + 1];
            const U32 first_out = e.buckets[k + 2];
            const U32 capacity = e.buckets[k + 3];
            if (e.count_base + b >= choice_words)
            {
                ++differences;
                what = "the choice's count";
                continue;
            }
            const U32 count = llmin(choice[e.count_base + b], capacity);
            for (U32 i = 0; i < count; ++i)
            {
                const size_t at = e.command_base + (size_t)(first_command + i) * 5;
                if (at + 4 >= choice_words)
                {
                    ++differences;
                    what = "the choice's commands";
                    break;
                }
                const U32* cmd = choice + at;
                const U32 page = cmd[4] / LLVKSceneTable::PAGE;
                const U32 local = page < e.page_local.size() ? e.page_local[page] : NONE;
                if (local == NONE)
                {
                    ++differences;
                    what = "a record outside the pass's pages";
                    continue;
                }
                // research/205 3a-5: sorted, at its group's base plus its place.
                U32 out_at = first_out + i;
                if (e.sorted)
                {
                    out_at = e.expected_at[(size_t)local * LLVKSceneTable::PAGE + cmd[4] % LLVKSceneTable::PAGE];
                    if (out_at == NONE || out_at >= e.commands || mSortedWritten[out_at])
                    {
                        ++differences;
                        what = out_at == NONE || out_at >= e.commands ? "a record outside the sorted array" : "two records at one place";
                        continue;
                    }
                    mSortedWritten[out_at] = 1;
                }
                const U32* out = got + (size_t)out_at * 5;
                const U32 row = e.rows_base + (first_out + i) * e.vec4s;
                // research/219: a rigged record of a pass whose program reads no palette draws nothing.
                const U32 index_count = e.expected_none[(size_t)local * LLVKSceneTable::PAGE + cmd[4] % LLVKSceneTable::PAGE] ? 0u : cmd[0];
                if (out[0] != index_count || out[1] != 1 || out[2] != cmd[2] || out[3] != cmd[3] || out[4] != row)
                {
                    ++differences;
                    what = "a command";
                    continue;
                }
                U32* want = &e.expected[((size_t)local * LLVKSceneTable::PAGE + cmd[4] % LLVKSceneTable::PAGE) * row_words];
                if (e.material_word != U32_MAX && e.material_word < e.word_vec4s * 4)
                {
                    want[e.material_word] = row + e.word_vec4s;     // research/198 5.6: its own material row, after its words
                }
                if (memcmp(rows + (size_t)(first_out + i) * row_words, want, row_words * 4))
                {
                    ++differences;
                    what = "a row's words";
                }
            }
        }
        // research/205 3a-5: every other command of a sorted array draws nothing.
        for (size_t at = 0; at < mSortedWritten.size(); ++at)
        {
            if (!mSortedWritten[at] && got[at * 5 + 1] != 0)
            {
                ++differences;
                what = "a sorted array's command no record wrote";
            }
        }
        ++mExpandCompared;
        if (e.batched)
        {
            mBatchCompared.fetch_add(1, std::memory_order_relaxed);     // research/209 E4
            mBatchDiffered.fetch_add(differences, std::memory_order_relaxed);
        }
        if (differences)
        {
            mExpandMismatches += differences;
            mProblems += differences;
            if (mLogged < VERIFY_LOG)
            {
                ++mLogged;
                LL_WARNS("VKScene") << "verify: frame " << e.frame << " view " << e.view_index << " pass " << e.pass
                                    << ": the expand's output differs from the CPU's in "
                                    << differences << " commands (last: " << what << ")" << LL_ENDL;
            }
        }
    }

    // ==== research/209 E4 (vk-s4a4): the queue recorded at a cut ====

    namespace
    {
        void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                            VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access)
        {
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = src_stages;
            barrier.srcAccessMask = src_access;
            barrier.dstStageMask = dst_stages;
            barrier.dstAccessMask = dst_access;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
    }

    // Every queued choice's phases between the same barriers (the barriers
    // each choice had, once), their verify copies, then every queued
    // expand's (the sorted ones' clears first), as choose and record_expand
    // record one: the same stages and accesses, fewer barriers. The queue is
    // emptied; work queued in another frame (it cannot be: every submission
    // cuts) is counted and left out, as its sets and room went with that frame.
    void Scene::recordBatch(VkCommandBuffer cmd)
    {
        if ((mQueuedChoices.empty() && mQueuedExpands.empty()) || !cmd)
        {
            return;
        }
        const U64 begins = LLVKMirror::instance().frameBegins();
        auto keep = [&](U64 queued_begins, U32 queued_flushes)
        {
            if (queued_begins != begins)
            {
                ++mBatchStale;
                return false;
            }
            if (queued_flushes != mFlushSerial)
            {
                // The tables were copied into after it was queued (a flush that did not cut first).
                ++mBatchEpochFaults;
                if (mBatchFaultsLogged < VERIFY_LOG)
                {
                    ++mBatchFaultsLogged;
                    LL_WARNS("VKScene") << "retained scene: queued work recorded after the tables it reads were copied into "
                                           "(research/209 E4: an epoch fault); recorded all the same" << LL_ENDL;
                }
            }
            return true;
        };
        ++mBatchCuts;
        bool host_copies = false;
        if (!mQueuedChoices.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
            for (U32 phase = 0; phase < PHASES; ++phase)
            {
                if (phase)
                {
                    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                    ++mBatchBarriers;
                }
                vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(U32), &phase);
                for (const QueuedChoice& q : mQueuedChoices)
                {
                    if ((phase == 0 && !keep(q.begins, q.flushes)) || (phase && q.begins != begins))
                    {
                        continue;
                    }
                    if (q.groups[phase])
                    {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &q.set, 0, nullptr);
                        vkCmdDispatch(cmd, q.groups[phase], 1, 1);
                    }
                }
            }
            // The counts and commands to the frame's indirect draws, the expands that read them and the verify copies.
            memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            ++mBatchBarriers;
            for (const QueuedChoice& q : mQueuedChoices)
            {
                if (q.readback && q.begins == begins)
                {
                    const VkBufferCopy region = { q.out_offset, 0, q.readback_bytes };
                    vkCmdCopyBuffer(cmd, q.out, q.readback, 1, &region);
                    host_copies = true;
                }
            }
            mQueuedChoices.clear();
        }
        if (!mQueuedExpands.empty())
        {
            bool sorted = false;
            for (const QueuedExpand& q : mQueuedExpands)
            {
                if (keep(q.begins, q.flushes) && q.rec.sorted)
                {
                    // research/205 3a-5: the array's places no record writes draw nothing.
                    vkCmdFillBuffer(cmd, q.rec.commands, q.rec.commands_offset, q.rec.commands_bytes, 0);
                    sorted = true;
                }
            }
            if (sorted)
            {
                memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                ++mBatchBarriers;
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mExpandPipeline);
            for (const QueuedExpand& q : mQueuedExpands)
            {
                if (q.begins == begins)
                {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, q.rec.layout, 0, 1, &q.rec.set, 0, nullptr);
                    vkCmdDispatch(cmd, q.rec.groups, 1, 1);
                }
            }
            // The commands to the draws, the rows to the shaders that read the palette table (and both to the verify copies).
            memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
            ++mBatchBarriers;
            for (const QueuedExpand& q : mQueuedExpands)
            {
                if (q.readback && q.begins == begins)
                {
                    vkCmdCopyBuffer(cmd, q.rec.commands, q.readback, 1, &q.command_copy);
                    vkCmdCopyBuffer(cmd, q.palette, q.readback, 1, &q.row_copy);
                    host_copies = true;
                }
            }
            mQueuedExpands.clear();
        }
        if (host_copies)
        {
            memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                           VK_ACCESS_2_HOST_READ_BIT);
            ++mBatchBarriers;
        }
    }

    // A submission's cut (endFrame and submitAndWait, right after
    // LLVKRecorder::finish: the render thread and every filler joined, the
    // recording worker done, so the upload commands are recorded directly on
    // this thread). Whatever the switch says now, all that is queued is
    // recorded: the frame's draws read it. No pass runs here, so the queue is
    // this thread's even while S3's tables have not been taken back yet (a
    // submit and wait in the frame). At the frame's end: auto's step.
    void Scene::submissionCut(VkCommandBuffer upload, bool frame_end)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        recordBatch(upload);
        if (!frame_end)
        {
            return;
        }
        secondaryStep();    // research/208 13.3
        const U32 compared = mBatchCompared.exchange(0, std::memory_order_relaxed);
        const U32 differed = mBatchDiffered.exchange(0, std::memory_order_relaxed);
        const U32 faults = mBatchEpochFaults;
        mBatchComparedTotal += compared;
        mBatchDifferedTotal += differed;
        const U8 before = mBatchState.load(std::memory_order_relaxed);
        const bool auto_mode = LLRHI::sceneBatchMode() == LLRHI::RenderThreadMode::Auto;
        U32 clean = mBatchClean;
        const U8 now = batch_step(before, auto_mode, clean, compared, differed, faults - mBatchFaultsSeen);
        mBatchFaultsSeen = faults;
        mBatchClean = clean;
        if (now != before)
        {
            mBatchState.store(now, std::memory_order_relaxed);
            if (now == BATCH_ON)
            {
                LL_INFOS("VKScene") << "LLRHI_SCENE_BATCH=auto (research/209 E4): " << clean
                                    << " frames compared without a difference; the choices and expands stay queued, unchecked" << LL_ENDL;
            }
            else
            {
                LL_WARNS("VKScene") << "LLRHI_SCENE_BATCH=auto (research/209 E4): a difference (" << differed << ") or an epoch fault ("
                                    << faults << "); recorded where they are made for the rest of the run" << LL_ENDL;
            }
        }
    }

    // The VKPerf scene line's part (empty while the switch is off and nothing was queued).
    std::string Scene::batchStats(U32 frames)
    {
        if (LLRHI::sceneBatchMode() == LLRHI::RenderThreadMode::Off && !mBatchCuts)
        {
            return std::string();
        }
        const U8 state = mBatchState.load(std::memory_order_relaxed);
        std::ostringstream out;
        out << "batch (research/209 E4, " << LLRHI::renderThreadModeName(LLRHI::sceneBatchMode())
            << (state == BATCH_OFF ? ", OFF: recorded where made" : state == BATCH_ON ? ", queued" : ", queued and checked")
            << "): cuts " << (F32)mBatchCuts / frames << " a frame (" << mBatchCutsAtFlush << " before a flush's copies), choices "
            << (F32)mBatchChoices / frames << ", expands " << (F32)mBatchExpands / frames << ", barriers " << (F32)mBatchBarriers / frames
            << " a frame; compared " << mBatchComparedTotal << ", differ " << mBatchDifferedTotal << ", epoch faults " << mBatchEpochFaults;
        if (mBatchStale)
        {
            out << ", LEFT OUT from another frame " << mBatchStale;
        }
        if (state == BATCH_CHECKING && LLRHI::sceneBatchMode() == LLRHI::RenderThreadMode::Auto)
        {
            out << ", clean frames " << mBatchClean << " of " << BATCH_AUTO_FRAMES;
        }
        mBatchCuts = mBatchCutsAtFlush = mBatchChoices = mBatchExpands = mBatchBarriers = 0;
        mBatchComparedTotal = mBatchDifferedTotal = mBatchStale = 0;
        return out.str();
    }

    SecondarySlot* Scene::takeSecondarySlot()
    {
        const U64 begins = LLVKMirror::instance().frameBegins();
        if (mSecondaryBegins != begins)
        {
            // The frame before was replayed by its finish: its slots are free again.
            mSecondaryBegins = begins;
            mSecondaryNext = 0;
        }
        if (mSecondaryNext == mSecondarySlots.size())
        {
            if (mSecondarySlots.size() >= SECONDARY_SLOTS_MAX)
            {
                ++mSecondaryNoSlot;
                return nullptr;
            }
            mSecondarySlots.emplace_back();
        }
        SecondarySlot& slot = mSecondarySlots[mSecondaryNext++];
        slot.buffer.store(VK_NULL_HANDLE, std::memory_order_relaxed);
        slot.replayed.store(false, std::memory_order_relaxed);
        return &slot;
    }

    // research/208 13.3: auto's step at the frame's cut (the frame's runs joined).
    void Scene::secondaryStep()
    {
        const U32 checked = mSecondaryChecked.exchange(0, std::memory_order_relaxed);
        const U32 differed = mSecondaryDiffered.exchange(0, std::memory_order_relaxed);
        const U32 failed = mSecondaryFailed.exchange(0, std::memory_order_relaxed);
        const U32 recorded = mSecondaryRecorded.exchange(0, std::memory_order_relaxed);
        mSecondaryCheckedTotal += checked;
        mSecondaryDifferedTotal += differed;
        mSecondaryFailedTotal += failed;
        mSecondaryRecordedTotal += recorded;
        if (LLRHI::passSecondaryMode() != LLRHI::RenderThreadMode::Auto)
        {
            return;
        }
        const U8 before = mSecondaryState.load(std::memory_order_relaxed);
        U32 clean = mSecondaryClean;
        // A secondary command buffer that could not be recorded while executing them: off as a difference.
        U8 now = before == BATCH_ON && failed ? (U8)BATCH_OFF : batch_step(before, true, clean, checked, differed + failed, 0);
        mSecondaryClean = clean;
        if (now != before)
        {
            mSecondaryState.store(now, std::memory_order_relaxed);
            if (now == BATCH_ON)
            {
                LL_INFOS("VKScene") << "LLRHI_PASS_SECONDARY=auto (research/208 13.3): " << clean
                                    << " frames of secondary command buffers recorded alike; executed from now on" << LL_ENDL;
            }
            else
            {
                LL_WARNS("VKScene") << "LLRHI_PASS_SECONDARY=auto (research/208 13.3): " << failed << " not recorded, " << differed
                                    << " different; the passes' draws go into their streams for the rest of the run" << LL_ENDL;
            }
        }
    }

    std::string Scene::secondaryStats(U32 frames)
    {
        if (LLRHI::passSecondaryMode() == LLRHI::RenderThreadMode::Off)
        {
            return std::string();
        }
        const U8 state = mSecondaryState.load(std::memory_order_relaxed);
        std::ostringstream out;
        out << "secondary command buffers (research/208 13.3, " << LLRHI::renderThreadModeName(LLRHI::passSecondaryMode())
            << (state == BATCH_OFF ? ", OFF" : state == BATCH_ON ? ", executed" : ", recorded and thrown away")
            << "): executed " << (F32)mSecondaryRecordedTotal / frames << " a frame, checked " << mSecondaryCheckedTotal << ", differ "
            << mSecondaryDifferedTotal << ", NOT RECORDED " << mSecondaryFailedTotal;
        if (mSecondaryNoSlot)
        {
            out << ", no slot " << mSecondaryNoSlot;
        }
        mSecondaryRecordedTotal = mSecondaryCheckedTotal = mSecondaryDifferedTotal = mSecondaryFailedTotal = mSecondaryNoSlot = 0;
        return out.str();
    }

    void scene_submission_cut(VkCommandBuffer upload, bool frame_end)
    {
        sScene.submissionCut(upload, frame_end);
        if (frame_end)
        {
            LLVKRtAs::frame(upload);    // research/230 R2: the acceleration structures, after the frame's own upload commands
        }
    }

    // ==== research/208 S3 (stage 4a): the passes handed to the render thread ====

    void PassJob::record()
    {
        sScene.runPass(*this);
    }

    // Every value the pass's run reads of the job, and the events it applies first.
    U64 PassJob::digest() const
    {
        LLVKRender::Digest d;
        d << view << pass << frame << upload;
        const LLVKScene::Binder::Words& w = words;
        d << w.count << w.matrix_word << w.material_word << w.material_vec4s << w.priority_word << w.palette_word;
        d.bytes(w.words, sizeof(w.words));
        d.bytes(w.units, sizeof(w.units));
        d.bytes(w.material, sizeof(w.material));
        d.bytes(w.material_at, sizeof(w.material_at));
        d.bytes(&word_textures, sizeof(word_textures));
        d << (U64)tex_ids.size() << (U64)tex_slots.size();
        d.bytes(tex_ids.data(), tex_ids.size() * sizeof(U32));
        d.bytes(tex_slots.data(), tex_slots.size() * sizeof(U32));
        d.bytes(view_matrix, sizeof(view_matrix));
        d.bytes(projection, sizeof(projection));
        d << (U64)(uintptr_t)fill << place_value << (U8)glow_priority << (U8)once << (U8)slot_lists << (U64)transform_slots.size() << (U64)uv_slots.size();
        d.bytes(transform_slots.data(), transform_slots.size() * sizeof(U32));
        d.bytes(uv_slots.data(), uv_slots.size() * sizeof(U32));
        d << (U64)events_begin << (U64)events_end;
        for (size_t i = events_begin; i < events_end && events.events; ++i)
        {
            const SceneEvent& e = events.events[i];
            d << e.kind << e.a << e.at << e.count << e.frame;
            switch (e.kind)
            {
            case EVENT_TRANSFORM:
            case EVENT_UV:
                d.bytes(events.floats + e.at, 16 * sizeof(F32));
                break;
            case EVENT_VIEW:
                d.bytes(events.views + e.at, sizeof(LLRHI::SceneViewDesc));
                break;
            case EVENT_BITS:
                d.bytes(events.bits + e.at, (size_t)e.count * sizeof(U32));
                break;
            case EVENT_PLACE:
                d << (U8)events.places[e.at].found;
                d.bytes(&events.places[e.at].place, sizeof(LLVKAttribPool::NumberPlace));
                break;
            default:
                break;
            }
        }
        d << draws_digest;
        d << (U8)(secondary != nullptr) << (U8)secondary_execute << frame_slot << frame_begins;     // research/208 13.3
        return d.h;
    }

    void scene_frame_joined()
    {
        sScene.frameJoined();
    }

    // Whether the pass goes to the render thread (Thread), or, verify, to the
    // main thread's join as the thread would run it (Verify). Main thread.
    LLVKRender::Use Scene::threadPassUse(U32 view_index, const LLRHI::SceneDrawContext& ctx) const
    {
        // The main and alpha views' passes the viewer cut and draws from the
        // scene with the program's own words (not an OIT session's, not the
        // sorted way: those answer later in the frame, drawSorted).
        // research/208 13.4 (vk-s4a4): with LLRHI_RENDER_THREAD_VIEWS=all, the probe faces', mirrors' and water
        // mirror's passes too (each face's view and bits reach the thread as events, in order).
        const bool other_view = !view_is_main(view_index) && view_index < LLRHI::SCENE_VIEW_COUNT;
        if ((!view_is_main(view_index) && !(other_view && LLRHI::renderThreadAllViews())) || !ctx.draw || !ctx.scope || ctx.group_base ||
            ctx.group_priority || !ctx.fill || !ctx.view_matrix || !ctx.projection)
        {
            return LLVKRender::Use::Inline;
        }
        if (!mReady || mBroken || !mBinder || !mExpandReady || !drawing() || LLVKRecorder::tStream)
        {
            return LLVKRender::Use::Inline;
        }
        // The scene's own verify turns of these views (their choices compared
        // every VERIFY_EVERY frames, the alpha view's a quarter of the way)
        // stay on the main thread, and so does a view the viewer did not give.
        const U32 frame = LLVKTracker::frame();
        // (research/209 S7: the probe faces' and mirrors' turn three quarters of the way.)
        if (frame % VERIFY_EVERY == 0 || frame % VERIFY_EVERY == VERIFY_EVERY / 4 ||
            (other_view && frame % VERIFY_EVERY == VERIFY_EVERY * 3 / 4) || mGivenDesc[view_index] != frame + 1 ||
            mGivenBits[view_index] != frame + 1)
        {
            return LLVKRender::Use::Inline;
        }
        return LLVKRender::use();
    }

    // The pass's entry on the main thread: the binder's half, then the pass to
    // the thread (or the join). False: nothing of it is the thread's (the
    // caller draws it as before: at most the binder resolved and moved what
    // the usual way's entry does again).
    bool Scene::postPass(U32 view_index, U32 pass, const LLRHI::SceneDrawContext& ctx, LLVKRender::Use use)
    {
        const U32 frame = LLVKTracker::frame();
        if (mOwnedByThread && mPostFrame != frame)
        {
            own("a new frame");     // (the frame's join gives them back: this only if it did not)
        }
        if (!mOwnedByThread)
        {
            // The frame's head as drawPass makes it, while the tables are the main thread's.
            if (!ready())
            {
                return false;
            }
            syncFrame();
            frameSlot();
            ViewState& v = mViews[view_index];
            if (v.desc_frame != frame || v.bits_frame != frame)
            {
                return false;
            }
            if (!v.choice.valid || v.choice.frame != frame)
            {
                applyPlaces(false);     // as drawPass right before a choice (the thread places only what events tell it)
            }
            mMainThread = std::this_thread::get_id();
            makeEventRoom();
            mRefusalsSeen = mRefusals;
            mShortsSeen = mShorts;
            mKnownTransforms = mTable.rowSpan(LLVKSceneTable::TABLE_TRANSFORMS);
            mKnownUVs = mTable.rowSpan(LLVKSceneTable::TABLE_UV);
            takeBudgets(view_index, frame);
        }
        if (!pass_open(mRefusalsSeen, view_index, pass, frame) || !upload_open(mShortsSeen, view_index, frame))
        {
            return false;
        }
        PassJob* job = freePassJob();
        PassDigestJob* check = use == LLVKRender::Use::Verify ? freeDigestJob() : nullptr;
        if (!job || (use == LLVKRender::Use::Verify && !check))
        {
            return false;
        }

        // The binder's half: what the main thread's GL context and the
        // backend's shared tables give at the entry (research/208 H22: the
        // records' textures' slots as values; H21's layouts and the rendering
        // stay here).
        struct PassScope
        {
            LLVKScene::Binder* binder;
            ~PassScope() { binder->endPass(); }
        } scope{ mBinder };
        LLVKScene::Binder::Words& words = job->words;
        words = LLVKScene::Binder::Words();
        const char* why = nullptr;
        bool permanent = false;
        if (!mBinder->beginPass(ctx.attributes, ctx.attribute_count, ctx.materials, ctx.material_count, words, why, permanent) ||
            !mBinder->headWay() || words.matrix_word == U32_MAX || words.matrix_word >= words.count || words.priority_word != U32_MAX ||
            !LLVKSceneShader::main_pass_palette_ok(mPassSkinned[pass], mPassPlain[pass], words.palette_word != U32_MAX))     // research/219
        {
            ++mS3Fallback;
            return false;   // the usual way's entry refuses it with its reason, or draws it from the context
        }
        if (!find_word_textures(words, ctx, job->word_textures))
        {
            ++mS3Fallback;
            return false;
        }
        job->tex_ids.clear();
        job->tex_slots.clear();
        for (U32 i = 0; i < ctx.texture_count; ++i)
        {
            const U32 id = ctx.textures[i];
            if (id && id < TEX_SLOTS)
            {
                job->tex_ids.push_back(id);
                job->tex_slots.push_back(mBinder->textureSlot(id));
            }
        }
        mBinder->texturesDone();
        if (!mBinder->admit())
        {
            ++mS3Fallback;
            return false;
        }
        // research/208 13.3: the draws into a secondary command buffer (executed at the end of this entry, on), or
        // into the stream and also one to throw away (checking).
        const U8 secondary_state = mSecondaryState.load(std::memory_order_relaxed);
        SecondarySlot* secondary = secondary_state != BATCH_OFF ? takeSecondarySlot() : nullptr;
        const bool execute = secondary && secondary_state == BATCH_ON;
        LLVKScene::ThreadDraws* draws = mBinder->beginThreadDraws(pass, execute);
        if (!draws)
        {
            ++mS3Fallback;
            return false;
        }
        // Whatever happens from here, the scope opened for the secondary command buffer is executed and ended
        // (nothing executed when none was recorded: an empty scope).
        struct SecondaryScope
        {
            SecondarySlot* slot;
            ~SecondaryScope()
            {
                if (slot)
                {
                    LLVKMirror& mirror = LLVKMirror::instance();
                    mirror.executeLater(&slot->buffer, &slot->replayed);
                    mirror.endRendering();
                }
            }
        } secondary_scope{ execute ? secondary : nullptr };

        // The rest is the tables' owner's: the values it reads.
        job->view = view_index;
        job->pass = pass;
        job->mPass = pass;
        job->frame = frame;
        job->upload = LLVKMirror::instance().uploadCommands();
        memcpy(job->view_matrix, ctx.view_matrix, sizeof(job->view_matrix));
        memcpy(job->projection, ctx.projection, sizeof(job->projection));
        job->fill = ctx.fill;
        job->place_value = ctx.place_value;
        job->glow_priority = ctx.glow_priority;
        job->once = ctx.once;
        job->slot_lists = ctx.transform_slots && ctx.uv_slots;
        job->transform_slots.assign(ctx.transform_slots, ctx.transform_slots + (job->slot_lists ? ctx.transform_slot_count : 0));
        job->uv_slots.assign(ctx.uv_slots, ctx.uv_slots + (job->slot_lists ? ctx.uv_slot_count : 0));
        job->draws = draws;
        job->draws_digest = draws->entryDigest();
        job->broken = false;
        job->secondary = secondary;
        job->secondary_execute = execute;
        job->frame_slot = LLVKMirror::instance().getSlot() % LLVKMirror::FRAMES_IN_FLIGHT;
        job->frame_begins = LLVKMirror::instance().frameBegins();
        job->events = { mEvents.data(), mEventFloats.data(), mEventViews.data(), mEventBits.data(), mEventPlaces.data() };
        job->events_begin = mEventsPosted;
        job->events_end = mEvents.size();

        if (!mS3Used)
        {
            mS3Used = true;
            LLVKRender::setFrameJoinHook(&scene_frame_joined);
        }
        ++mJobsPosted;      // before the post: the thread may run it at once
        const bool posted = use == LLVKRender::Use::Thread ? LLVKRender::post(*job) : LLVKRender::postDeferred(*job);
        if (posted)
        {
            mEventsPosted = job->events_end;
            mOwnedByThread = true;
            mPostFrame = frame;
            if (check)
            {
                check->of = job;
                check->mPass = pass;
                check->mMainDigest = job->digest();
                LLVKRender::postVerify(*check);
            }
            ++(use == LLVKRender::Use::Thread ? mS3Posted : mS3Deferred);
        }
        else
        {
            // No stream for it now: run here, after what the thread holds
            // (the events queued so far are applied by the take-back).
            --mJobsPosted;
            own("a pass without a stream");
            job->events = SceneEventView();
            job->events_begin = job->events_end = 0;
            ++mJobsPosted;      // runPass counts it run
            runPass(*job);
            ++mS3Here;
        }
        LLVKAttribPool::sceneDrew();
        return true;
    }

    // The pass on its runner (the render thread; the main thread's join for
    // verify, a job the thread had not taken, or a pass without a stream).
    void Scene::runPass(PassJob& job)
    {
        if (job.broken)
        {
            return;     // its first run threw: its stream was emptied (the scene stops at the take-back)
        }
        struct RunScope
        {
            Scene& scene;
            PassJob& job;
            RunScope(Scene& s, PassJob& j) : scene(s), job(j)
            {
                tRunningPass = true;
                scene.mJob = &j;
            }
            ~RunScope()
            {
                if (job.draws)
                {
                    job.draws->release();
                    job.draws = nullptr;
                }
                scene.mJob = nullptr;
                tRunningPass = false;
                scene.mJobsRun.fetch_add(1, std::memory_order_release);
            }
            RunScope(const RunScope&) = delete;
            RunScope& operator=(const RunScope&) = delete;
        } run(*this, job);
        try
        {
            runPassBody(job);
            if (job.secondary)
            {
                // research/208 13.3: the stream's last packet (the execution checks it was replayed, not skipped).
                LLVKRecorder::CmdMarkReplayed(job.upload, &job.secondary->replayed);
            }
        }
        catch (...)
        {
            job.broken = true;
            mThreadBroke.store(true, std::memory_order_relaxed);
            LLVKRender::failSession("a pass threw part way (the scene's tables are not known after it: the scene stops)");
            if (LLVKRender::onThread())
            {
                throw;  // the thread empties the job's stream; the join's second run records nothing
            }
        }
    }

    // drawPass's part after the entry, from the job's values and the tables.
    void Scene::runPassBody(PassJob& job)
    {
        applyEvents(job.events, job.events_end);
        const U32 view_index = job.view;
        const U32 pass = job.pass;
        const U32 frame = job.frame;
        ViewState& v = mViews[view_index];
        if (v.desc_frame != frame || v.bits_frame != frame)
        {
            ++mNotChosen;
            passMissing(job, "its view was not given this frame");
            return;
        }
        if (!v.choice.valid || v.choice.frame != frame)
        {
            applyPlaces(false);     // the rows have the places the groups were cut for
            if (!choose(view_index, false))
            {
                passMissing(job, "its view's choice (no room, or a view waiting after shorts)");
                return;
            }
        }
        // The records' textures' slots as the main thread resolved them at the entry (resolveTextures).
        for (size_t i = 0; i < job.tex_ids.size(); ++i)
        {
            const U32 id = job.tex_ids[i];
            const U32 slot = job.tex_slots[i];
            ++mTexResolved;
            mTexWhite += slot == LLVKBindless::WHITE_SLOT ? 1 : 0;
            if (mTexSlots[id] != slot)
            {
                mTexSlots[id] = slot;
                mTexDirty.push_back(id);
                ++mTexChanged;
            }
        }
        LLRHI::SceneDrawContext ctx;
        ctx.draw = true;
        ctx.scope = true;
        ctx.view_matrix = job.view_matrix;
        ctx.projection = job.projection;
        ctx.fill = job.fill;
        ctx.place_value = job.place_value;
        ctx.glow_priority = job.glow_priority;
        ctx.once = job.once;
        static const U32 NO_ROWS = 0;   // the viewer's lists start with row 0 (never empty)
        if (job.slot_lists)
        {
            ctx.transform_slots = job.transform_slots.empty() ? &NO_ROWS : job.transform_slots.data();
            ctx.transform_slot_count = (U32)job.transform_slots.size();
            ctx.uv_slots = job.uv_slots.empty() ? &NO_ROWS : job.uv_slots.data();
            ctx.uv_slot_count = (U32)job.uv_slots.size();
        }
        if (!matrixRows(view_index, v, ctx))
        {
            refusePass(view_index, pass, "the frame's matrix table (or the view's share of it) is full", false);
            passMissing(job, "its matrix rows");
            return;
        }
        ExpandOut out;
        if (!expand(view_index, pass, job.words, job.word_textures, ctx, false, out))
        {
            if (out.no_room)
            {
                noRoom(view_index, "pass", 0);
            }
            else
            {
                refusePass(view_index, pass, "the frame's palette table (or the view's share of it) is full or missing", false);
            }
            passMissing(job, out.no_room ? "its expand (no upload room)" : "its expand (the palette table)");
            return;
        }
        const Choice& c = v.choice;
        U32 added = 0;
        for (size_t k = 0; k + 3 < mExpandBuckets.size(); k += 4)
        {
            const U32 b = mExpandBuckets[k];
            if (!job.draws->addBucket(mTable.bucketKey(b).chunk, out.buffer, out.offset + (VkDeviceSize)mExpandBuckets[k + 2] * COMMAND_BYTES,
                                      c.out, c.offset + (VkDeviceSize)(c.view.count_base + b) * 4, mExpandBuckets[k + 3], COMMAND_BYTES))
            {
                ++mBindFailed;
                refusePass(view_index, pass, "a chunk's index buffer or streams", false);
                passMissing(job, "a chunk's index buffer or streams");
                return;
            }
            ++added;
        }
        if (added && job.secondary_execute)
        {
            // research/208 13.3: straight into this thread's secondary command buffer (the entry executes it).
            const VkCommandBuffer secondary = job.draws->recordSecondary(job.frame_slot, job.frame_begins);
            if (!secondary)
            {
                mSecondaryFailed.fetch_add(1, std::memory_order_relaxed);
                refusePass(view_index, pass, "no secondary command buffer", false);
                passMissing(job, "its secondary command buffer could not be recorded");
                return;
            }
            job.secondary->buffer.store(secondary, std::memory_order_release);
            mSecondaryRecorded.fetch_add(1, std::memory_order_relaxed);
        }
        else if (added)
        {
            job.draws->recordDraws();
            if (job.secondary)
            {
                // research/208 13.3, checking: the same into a secondary command buffer, thrown away.
                const U32 commands = job.draws->recordedCommands();
                const VkCommandBuffer secondary = job.draws->recordSecondary(job.frame_slot, job.frame_begins);
                if (!secondary)
                {
                    mSecondaryFailed.fetch_add(1, std::memory_order_relaxed);
                }
                else if (job.draws->recordedCommands() != commands)
                {
                    mSecondaryDiffered.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    mSecondaryChecked.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        mDrawnBuckets += added;
        pass_drew(mRefusals, view_index, pass);
        mPassWhy[view_index][pass] = nullptr;
        ++mPassesDrawn;
        ++mViewPassesDrawn[view_index];
        ++mS3Ran;
    }

    // research/208 2.4 (K2): a pass handed over that its runner could not
    // draw is missing from this frame (the main thread said it was drawn);
    // a problem, and the pass waits as a refused one (canDrawPass, from the
    // next take-back).
    void Scene::passMissing(PassJob& job, const char* why)
    {
        ++mS3Missing;
        ++mProblems;
        if (mS3MissingKept < MISSING_KEPT)
        {
            mS3MissingWhy[mS3MissingKept++] = why;
        }
        if (mS3MissingLogged < VERIFY_LOG)
        {
            ++mS3MissingLogged;
            LL_WARNS("VKScene") << "retained scene: pass " << job.pass << " of the " << VIEW_NAMES[job.view]
                                << " view, handed to the render thread, is missing from this frame: " << why
                                << " (research/208 S3; the pass waits as a refused one)" << LL_ENDL;
        }
    }

    // The tables back to the main thread: every pass handed over run (a join
    // when one is not), the events queued since applied in order. Main
    // thread, under the backend lock.
    void Scene::own(const char* why)
    {
        if (!mOwnedByThread || tRunningPass)
        {
            return;
        }
        if (std::this_thread::get_id() != mMainThread || LLVKRecorder::tStream)
        {
            ownerFault(why);    // not here: another thread, or inside a stream of the main thread's own (a join cannot record)
            return;
        }
        bool pending = mJobsRun.load(std::memory_order_acquire) != mJobsPosted;
        for (const std::unique_ptr<PassDigestJob>& check : mDigestJobs)
        {
            pending = pending || !check->idle();     // verify: the thread may still read the events
        }
        if (pending)
        {
            ++mS3Takebacks;
            mS3TakebackWhy = why;
            LLVKRender::join(false);
        }
        if (mJobsRun.load(std::memory_order_acquire) != mJobsPosted)
        {
            ownerFault("a pass not run at the join");
            return;
        }
        mOwnedByThread = false;
        const SceneEventView all = { mEvents.data(), mEventFloats.data(), mEventViews.data(), mEventBits.data(), mEventPlaces.data() };
        applyEvents(all, mEvents.size());
        mEvents.clear();
        mEventFloats.clear();
        mEventViews.clear();
        mEventBits.clear();
        mEventPlaces.clear();
        mEventsApplied = 0;
        mEventsPosted = 0;
        mCaptured.clear();
        if (mEventDropped)
        {
            // A place change could not be queued: every number is placed again from the pool.
            mEventDropped = false;
            for (const auto& kv : mNumberRecords)
            {
                mDirtyNumbers.push_back(kv.first);
            }
        }
        if (mThreadBroke.exchange(false, std::memory_order_relaxed) && !mBroken)
        {
            mBroken = true;
            LL_WARNS("VKScene") << "retained scene: a pass handed to the render thread threw part way; the scene stops "
                                << "(every batch is drawn the usual way from now on)" << LL_ENDL;
        }
    }

    void Scene::frameJoined()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("the frame's end");
    }

    // The holder's ("持ち主の札", research/208 S3): the tables touched by a
    // thread that does not hold them is counted (0 is the pass mark).
    void Scene::ownerCheck(const char* where) const
    {
        if (!mS3Used || tRunningPass)
        {
            return;
        }
        if (LLVKRender::onThread() || mOwnedByThread)
        {
            ownerFault(where);
        }
    }

    void Scene::ownerFault(const char* where) const
    {
        const U32 n = mOwnerFaults.fetch_add(1, std::memory_order_relaxed);
        if (n < OWNER_FAULTS_KEPT)
        {
            mOwnerFaultWhere[n].store(where, std::memory_order_relaxed);
        }
    }

    // The queue's arrays made large enough before the tables are handed over (main thread, holding them).
    void Scene::makeEventRoom()
    {
        const size_t grow = mEventsGrow ? 2 : 1;
        mEventsGrow = false;
        auto room = [grow](auto& v, size_t at_least)
        {
            const size_t want = llmax(at_least, v.capacity() * grow);
            if (v.capacity() < want)
            {
                v.reserve(want);
            }
        };
        room(mEvents, EVENTS_ROOM);
        room(mEventFloats, EVENTS_ROOM * 4);
        room(mEventViews, (size_t)LLRHI::SCENE_VIEWS * 4);
        room(mEventBits, EVENT_BITS_ROOM);
        room(mEventPlaces, EVENTS_ROOM);
    }

    // Room for one more event and its payload without moving an array the thread may read.
    bool Scene::eventRoom(size_t floats, size_t views, size_t bits, size_t places) const
    {
        return mEvents.size() < mEvents.capacity() && mEventFloats.size() + floats <= mEventFloats.capacity() &&
               mEventViews.size() + views <= mEventViews.capacity() && mEventBits.size() + bits <= mEventBits.capacity() &&
               mEventPlaces.size() + places <= mEventPlaces.capacity();
    }

    void Scene::queueEvent(U8 kind, U32 a, U32 at, U32 count)
    {
        SceneEvent e;
        e.kind = kind;
        e.a = a;
        e.at = at;
        e.count = count;
        e.frame = LLVKTracker::frame();
        mEvents.push_back(e);
        ++mS3Events;
    }

    // The tables' owner: the events up to `end` not applied yet, in order.
    void Scene::applyEvents(const SceneEventView& events, size_t end)
    {
        if (!events.events || end <= mEventsApplied)
        {
            return;
        }
        mApplyingEvents = true;
        for (size_t i = mEventsApplied; i < end; ++i)
        {
            applyEvent(events, events.events[i]);
        }
        mApplyingEvents = false;
        mEventsApplied = end;
    }

    // What the entry would have done then (setTransform, setView, numberChanged, ...).
    void Scene::applyEvent(const SceneEventView& events, const SceneEvent& e)
    {
        switch (e.kind)
        {
        case EVENT_TRANSFORM:
            mTable.setTransform(e.a, events.floats + e.at);
            break;
        case EVENT_TRANSFORM_GONE:
            mTable.removeTransform(e.a);
            break;
        case EVENT_UV:
            mTable.setUVTransform(e.a, events.floats + e.at);
            break;
        case EVENT_UV_GONE:
            mTable.removeUVTransform(e.a);
            break;
        case EVENT_VIEW:
        {
            ViewState& v = mViews[e.a];
            v.desc = events.views[e.at];
            v.desc_frame = e.frame;
            v.choice.valid = false;
            v.matrix_frame = U32_MAX;
            break;
        }
        case EVENT_BITS:
        {
            ViewState& v = mViews[e.a];
            v.bits.assign(events.bits + e.at, events.bits + e.at + e.count);
            v.bits_frame = e.frame;
            v.choice.valid = false;
            v.matrix_frame = U32_MAX;
            break;
        }
        case EVENT_PLACE:
            mCaptured[e.a] = events.places[e.at];
            placeChanged(e.a);
            break;
        default:
            break;
        }
    }

    // A buffer number's place: the pool's now (the main thread holding the
    // tables), or, on a pass's run and while events are applied, as the
    // number's last event had it (false without one).
    bool Scene::placeOf(U32 number, LLVKAttribPool::NumberPlace& out) const
    {
        if (tRunningPass || mApplyingEvents)
        {
            const auto it = mCaptured.find(number);
            if (it == mCaptured.end())
            {
                out = LLVKAttribPool::NumberPlace();
                return false;
            }
            out = it->second.place;
            return it->second.found;
        }
        return LLVKAttribPool::numberPlace(number, out);
    }

    // research/208 H19: the view's shares of the frame's palette and matrix
    // tables for its passes on the thread, taken at its first pass handed
    // over (main thread, holding the tables): the palette rows its passes
    // took the frame before and an eighth more, the matrix rows its view
    // makes (a row per transform row and per texture transform row now).
    void Scene::takeBudgets(U32 view_index, U32 frame)
    {
        SceneBudget& p = mPaletteBudget[view_index];
        if (p.frame != frame)
        {
            p = SceneBudget();
            p.frame = frame;
            const U32 last = mPaletteFrame == frame ? mPaletteWantedLast[view_index]
                                                    : (mPaletteFrame + 1 == frame ? mPaletteWanted[view_index] : 0);
            const U32 want = last ? llmin(last + last / 8 + 64, ROW_VEC4S_PER_FRAME / 2) : 0;
            if (want && LLVKBindless::paletteRows(want, p.base))
            {
                p.count = want;
            }
            mS3BudgetShort += want && !p.count ? 1 : 0;
        }
        SceneBudget& m = mMatrixBudget[view_index];
        if (m.frame != frame)
        {
            m = SceneBudget();
            m.frame = frame;
            const U32 rows = mKnownTransforms + mKnownUVs;
            m.rows = rows ? LLVKBindless::matrixRows(rows, m.base) : nullptr;
            m.count = m.rows ? rows : 0;
            mS3BudgetShort += rows && !m.rows ? 1 : 0;
        }
    }

    // expand's palette rows: from the view's share on a pass's run, else the table's.
    bool Scene::takePaletteRows(U32 view_index, U32 vec4s, U32& base)
    {
        const U32 frame = LLVKTracker::frame();
        if (mPaletteFrame != frame)
        {
            if (mPaletteFrame + 1 == frame)
            {
                memcpy(mPaletteWantedLast, mPaletteWanted, sizeof(mPaletteWantedLast));
            }
            else
            {
                memset(mPaletteWantedLast, 0, sizeof(mPaletteWantedLast));
            }
            memset(mPaletteWanted, 0, sizeof(mPaletteWanted));
            mPaletteFrame = frame;
        }
        mPaletteWanted[view_index] += vec4s;
        SceneBudget& p = mPaletteBudget[view_index];
        if (tRunningPass && p.frame == frame && p.count - p.used >= vec4s)
        {
            base = p.base + p.used;
            p.used += vec4s;
            return true;
        }
        return LLVKBindless::paletteRows(vec4s, base) != nullptr;
    }

    // matrixRows' run: the view's share on a pass's run (once a frame), else the table's.
    LLVKBindless::DrawMatrices* Scene::takeMatrixRows(U32 view_index, U32 count, U32& first)
    {
        SceneBudget& m = mMatrixBudget[view_index];
        if (tRunningPass && m.rows && m.frame == LLVKTracker::frame() && !m.used && count <= m.count)
        {
            m.used = count;
            first = m.base;
            return m.rows;
        }
        return LLVKBindless::matrixRows(count, first);
    }

    // The frame's upload commands: the job's on a pass's run (the main thread asked the mirror at the entry).
    VkCommandBuffer Scene::uploadCmd() const
    {
        return tRunningPass && mJob ? mJob->upload : LLVKMirror::instance().uploadCommands();
    }

    // research/208 H20: a frame descriptor set; on a pass's run from the
    // scene's own pools of the frame slot (the mirror's are the main
    // thread's), reset when the slot's frame comes round (frameSlot).
    VkDescriptorSet Scene::allocSet(VkDescriptorSetLayout layout)
    {
        if (!tRunningPass)
        {
            return LLVKMirror::instance().allocateSet(layout);
        }
        const U32 s = LLVKMirror::instance().getSlot() % LLVKMirror::FRAMES_IN_FLIGHT;
        std::vector<VkDescriptorPool>& pools = mThreadPools[s];
        size_t& index = mThreadPoolIndex[s];
        const VkDevice device = LLVKDevice::instance().getDevice();
        VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        while (index < THREAD_POOLS_MAX)
        {
            if (index == pools.size())
            {
                VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, THREAD_POOL_SETS * llmax(BINDINGS, EXPAND_BINDINGS) };
                VkDescriptorPoolCreateInfo create = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                create.maxSets = THREAD_POOL_SETS;
                create.poolSizeCount = 1;
                create.pPoolSizes = &size;
                VkDescriptorPool pool = VK_NULL_HANDLE;
                if (vkCreateDescriptorPool(device, &create, nullptr, &pool) != VK_SUCCESS)
                {
                    return VK_NULL_HANDLE;
                }
                pools.push_back(pool);
            }
            info.descriptorPool = pools[index];
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VkResult result = vkAllocateDescriptorSets(device, &info, &set);
            if (result == VK_SUCCESS)
            {
                return set;
            }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
            {
                return VK_NULL_HANDLE;
            }
            ++index;
        }
        return VK_NULL_HANDLE;
    }

    PassJob* Scene::freePassJob()
    {
        const size_t n = mPassJobs.size();
        for (size_t i = 0; i < n; ++i)
        {
            PassJob* job = mPassJobs[(mPassJobNext + i) % n].get();
            if (job->idle())
            {
                mPassJobNext = (mPassJobNext + i + 1) % n;
                return job;
            }
        }
        if (n >= MAX_RENDER_JOBS)
        {
            return nullptr;
        }
        mPassJobs.push_back(std::make_unique<PassJob>());
        return mPassJobs.back().get();
    }

    PassDigestJob* Scene::freeDigestJob()
    {
        const size_t n = mDigestJobs.size();
        for (size_t i = 0; i < n; ++i)
        {
            PassDigestJob* job = mDigestJobs[(mDigestJobNext + i) % n].get();
            if (job->idle())
            {
                mDigestJobNext = (mDigestJobNext + i + 1) % n;
                return job;
            }
        }
        if (n >= MAX_RENDER_JOBS)
        {
            return nullptr;
        }
        mDigestJobs.push_back(std::make_unique<PassDigestJob>());
        return mDigestJobs.back().get();
    }

    // The scene line's part (main thread, holding the tables; empty while no pass was handed over).
    std::string Scene::threadStats(U32 frames)
    {
        if (!mS3Used)
        {
            return std::string();
        }
        std::ostringstream out;
        out << "tables' owner (research/208 S3): passes handed over " << (F32)(mS3Posted + mS3Deferred) / frames << " a frame (to the thread "
            << mS3Posted << ", verify's to the join " << mS3Deferred << ", here without a stream " << mS3Here << "), run " << mS3Ran
            << ", back to the usual way at the entry " << mS3Fallback << ", events " << mS3Events << ", take-backs " << mS3Takebacks;
        if (mS3TakebackWhy)
        {
            out << " (the last: " << mS3TakebackWhy << ")";
        }
        if (mS3BudgetShort || mS3Dropped)
        {
            out << ", shares not taken " << mS3BudgetShort << ", places dropped " << mS3Dropped;
        }
        if (mS3Missing)
        {
            out << ", MISSING passes " << mS3Missing;
            for (U32 i = 0; i < mS3MissingKept; ++i)
            {
                out << (i ? "; " : " (") << mS3MissingWhy[i];
            }
            out << (mS3MissingKept ? ")" : "");
        }
        const U32 faults = mOwnerFaults.exchange(0, std::memory_order_relaxed);
        out << ", owner faults " << faults;
        for (U32 i = 0; i < faults && i < OWNER_FAULTS_KEPT; ++i)
        {
            const char* where = mOwnerFaultWhere[i].load(std::memory_order_relaxed);
            out << (i ? "; " : " (") << (where ? where : "?");
        }
        out << (faults ? ")" : "");
        mS3Posted = mS3Deferred = mS3Here = mS3Ran = mS3Fallback = mS3Takebacks = mS3Events = 0;
        mS3Missing = mS3BudgetShort = mS3Dropped = mS3MissingKept = 0;
        mS3TakebackWhy = nullptr;
        return out.str();
    }

    bool Scene::drawPass(U32 view_index, U32 pass, const LLRHI::SceneDrawContext& ctx)
    {
        const bool alpha_view = view_index == LLRHI::SCENE_VIEW_ALPHA;     // research/205 3a-3
        if ((view_index >= LLRHI::SCENE_VIEW_COUNT && !alpha_view) || pass >= LLRHI::SCENE_MAX_PASSES ||
            ctx.place_value >= LLRHI::SceneMaterialDesc::VALUES || (ctx.group_base && !alpha_view))
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        // research/208 S3: a drawn pass of the main or alpha view handed to the
        // render thread whole (its entry's half here). Else the tables come
        // back first, and the pass is drawn here as before.
        const LLVKRender::Use thread_use = threadPassUse(view_index, ctx);
        if (thread_use != LLVKRender::Use::Inline && postPass(view_index, pass, ctx, thread_use))
        {
            return true;
        }
        own("a pass drawn by the main thread");
        if (!ready())
        {
            return false;
        }
        syncFrame();
        ViewState& v = mViews[view_index];
        const U32 frame = LLVKTracker::frame();
        if (v.desc_frame != frame || v.bits_frame != frame)
        {
            ++mNotChosen;   // the viewer did not give this frame's view
            return false;
        }
        if (!v.choice.valid || v.choice.frame != frame)
        {
            const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_CHOOSE);     // research/208 S0
            applyPlaces(false);     // the rows have the places the groups were cut for
            if (!choose(view_index, ctx.verify_view))
            {
                return false;
            }
        }
        // Verify the rows and commands (and the records' places) with the
        // main view's verified choice, for a pass the viewer draws or would.
        // research/198 5.6: one pass a verified frame, each in turn (the
        // passes of the scope the frame before are counted to take turns).
        const FrameSlot& here = mSlots[LLVKMirror::instance().getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        bool verify = false;
        if (alpha_view)
        {
            // research/205 3a-3: the alpha view's verified frames compare its
            // passes' expands in turn (plain, fullbright, ...: the passes the
            // frame before had, counted as for the main view).
            if (ctx.scope && here.verify.pending && here.verify.frame == frame && here.verify.view_index == LLRHI::SCENE_VIEW_ALPHA &&
                mExpandReady && mBinder)
            {
                if (mAlphaVerifyFrame != frame)
                {
                    mAlphaVerifyFrame = frame;
                    ++mAlphaVerifyRound;
                    mAlphaVerifyPasses = 0;
                }
                verify = !here.expand.pending && (mAlphaVerifyRound + mAlphaVerifyPasses) % 2 == 0;
                ++mAlphaVerifyPasses;
            }
        }
        else if (ctx.scope && view_index == LLRHI::SCENE_VIEW_MAIN && here.verify.pending && here.verify.frame == frame &&
            here.verify.view_index == LLRHI::SCENE_VIEW_MAIN && mExpandReady && mBinder)
        {
            if (mVerifyTurnFrame != frame)
            {
                mVerifyTurnFrame = frame;
                mVerifyPassesLast = mVerifyPasses ? mVerifyPasses : mVerifyPassesLast;
                mVerifyPasses = 0;
                ++mVerifyRound;
            }
            verify = !here.expand.pending && mVerifyPasses == mVerifyRound % llmax(mVerifyPassesLast, 1u);
            ++mVerifyPasses;
        }
        else if (ctx.scope && ctx.verify_view && view_index != LLRHI::SCENE_VIEW_MAIN && here.verify.pending &&
                 here.verify.frame == frame && here.verify.view_index == view_index && here.verify.serial == v.choice.serial &&
                 mExpandReady && mBinder)
        {
            // research/209 S7: the face whose choice the turn copied, one of
            // its passes in turn (counted as for the main view).
            if (mOtherVerifySerial != v.choice.serial)
            {
                mOtherVerifySerial = v.choice.serial;
                mOtherVerifyPassesLast = mOtherVerifyPasses ? mOtherVerifyPasses : mOtherVerifyPassesLast;
                mOtherVerifyPasses = 0;
                ++mOtherVerifyRound;
            }
            verify = !here.expand.pending && mOtherVerifyPasses == mOtherVerifyRound % llmax(mOtherVerifyPassesLast, 1u);
            ++mOtherVerifyPasses;
            mViewVerified[view_index] += verify ? 1 : 0;
        }
        if (!ctx.draw && !verify)
        {
            return true;    // chosen
        }
        if (!mBinder || !mExpandReady || (ctx.draw && !drawing()))
        {
            ++mNoBinder;
            return false;
        }

        // The pass's words, as a draw of its program pushes them.
        LLVKScene::Binder::Words words;
        const char* why = nullptr;
        bool permanent = false;
        struct PassScope
        {
            LLVKScene::Binder* binder;
            ~PassScope() { binder->endPass(); }
        } scope{ mBinder };
        LLRHI::CensusTimer binder_census(LLRHI::CENSUS_SCENE_BINDER);    // research/208 S0: to the expand
        if (!mBinder->beginPass(ctx.attributes, ctx.attribute_count, ctx.materials, ctx.material_count, words, why, permanent))
        {
            refusePass(view_index, pass, why, permanent);
            return false;
        }
        if (words.matrix_word == U32_MAX || words.matrix_word >= words.count)
        {
            refusePass(view_index, pass, "its program reads no matrix row", true);
            return false;
        }
        // research/205 3a-3: an OIT session's draws take their priority
        // from the row; the viewer gives the groups' priorities for those.
        if (words.priority_word != U32_MAX && (words.priority_word < words.count || !ctx.group_priority))
        {
            refusePass(view_index, pass, "an OIT session's draws without the groups' priorities", false);
            return false;
        }
        // research/219: rigged records are drawn with a program that reads their palette rows, and only them.
        if (!LLVKSceneShader::main_pass_palette_ok(mPassSkinned[pass], mPassPlain[pass], words.palette_word != U32_MAX))
        {
            ++mPaletteRefused;
            refusePass(view_index, pass, "rigged records, and its program reads no palette (or the pass has others too)", false);
            return false;
        }
        // Per word, the material texture (research/205 H5: and the list's entry) the record's draw binds to its unit.
        WordTextures word_tex;
        if (!find_word_textures(words, ctx, word_tex))
        {
            refusePass(view_index, pass, "its program samples a material's unit outside the table (or not at all)", true);
            return false;
        }

        resolveTextures(ctx);
        if (!matrixRows(view_index, v, ctx))
        {
            refusePass(view_index, pass, "the frame's matrix table (or the view's share of it) is full", false);
            return false;
        }
        binder_census.stop();
        ExpandOut out;
        LLRHI::CensusTimer expand_census(LLRHI::CENSUS_SCENE_EXPAND);    // research/208 S0
        const bool expanded = expand(view_index, pass, words, word_tex, ctx, verify, out);
        expand_census.stop();
        if (!expanded)
        {
            if (out.no_room)
            {
                // fix-r8: this frame only (the next pass may fit; the next frame tries it again).
                noRoom(view_index, "pass", 0);
                return false;
            }
            refusePass(view_index, pass, "the frame's palette table (or the view's share of it) is full or missing", false);
            return false;
        }
        if (!ctx.draw)
        {
            return true;    // verified only
        }
        if (ctx.group_base)
        {
            // research/205 3a-5: drawn span by span (drawSorted), with what
            // its words were made from; the records' textures readable now.
            if (!out.buffer)
            {
                return true;    // no record of the pass in the view's choice: no span of it either
            }
            if (mSorted.size() < LLRHI::SCENE_MAX_PASSES)
            {
                mSorted.resize(LLRHI::SCENE_MAX_PASSES);
            }
            SortedPass& s = mSorted[pass];
            s.frame = frame;
            s.serial = v.choice.serial;
            s.commands = out.buffer;
            s.offset = out.offset;
            s.count = ctx.sorted_commands;
            s.words = words;
            s.word_textures = word_tex;
            s.attribute_count = llmin(ctx.attribute_count, (U32)LLRHI::DirectDraw::MAX_ATTRIBUTES);
            memcpy(s.attributes, ctx.attributes, s.attribute_count * sizeof(LLRHI::DirectDraw::Attribute));
            s.materials = ctx.materials;
            s.material_count = ctx.material_count;
            mBinder->settleTextures();
            pass_drew(mRefusals, view_index, pass);
            mPassWhy[view_index][pass] = nullptr;
            ++mSortedPrepared;
            return true;
        }

        // A draw per bucket of the pass, with the bucket's chunk bound.
        const Choice& c = v.choice;
        // research/208 S1: on the head's way everything a bucket's bind could
        // fail at is made or asked before any is drawn (the admission), so the
        // pass is drawn whole or handed back here, at its entry.
        const bool head_way = mBinder->headWay();
        if (ctx.once || head_way)
        {
            const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_BINDER);
            // research/205 3a-3: drawn twice the alpha would blend twice;
            // what a bucket's bind needs of its chunk is asked first.
            for (size_t k = 0; k + 3 < mExpandBuckets.size(); k += 4)
            {
                if (!mBinder->chunkBindable(mTable.bucketKey(mExpandBuckets[k]).chunk))
                {
                    ++mBindFailed;
                    refusePass(view_index, pass, "a chunk's index buffer or streams", false);
                    return false;   // nothing drawn: the caller draws the batches
                }
            }
            if (head_way && !mExpandBuckets.empty() && !mBinder->admit())
            {
                ++mBindFailed;
                refusePass(view_index, pass, "its pipeline or descriptor set (the pass head, research/208 S1)", false);
                return false;   // nothing drawn: the caller draws the batches
            }
        }
        const LLRHI::CensusTimer record_census(LLRHI::CENSUS_SCENE_RECORD);     // research/208 S0
        // research/208 S2: admitted on the head's way, the main and alpha
        // views' draws are values: the binder does its half here (the layouts,
        // the rendering) and the render thread records the binds and draws
        // into a stream spliced here (verify: here, and compared). A chunk
        // that cannot be bound now leaves nothing recorded: the pass is
        // refused at its entry as before.
        const LLVKRender::Use use = head_way && view_is_main(view_index) ? LLVKRender::use() : LLVKRender::Use::Inline;
        if (use != LLVKRender::Use::Inline && !mExpandBuckets.empty() && mBinder->beginDraws(use, pass))
        {
            U32 added = 0;
            for (size_t k = 0; k + 3 < mExpandBuckets.size(); k += 4)
            {
                const U32 b = mExpandBuckets[k];
                if (!mBinder->addDraw(mTable.bucketKey(b).chunk, out.buffer, out.offset + (VkDeviceSize)mExpandBuckets[k + 2] * COMMAND_BYTES,
                                      c.out, c.offset + (VkDeviceSize)(c.view.count_base + b) * 4, mExpandBuckets[k + 3], COMMAND_BYTES))
                {
                    mBinder->abortDraws();
                    ++mBindFailed;
                    refusePass(view_index, pass, "its pipeline or words could not be bound", false);
                    return false;   // nothing drawn: the caller draws the cut batches
                }
                ++added;
            }
            mDrawsByThread += mBinder->endDraws() ? 1 : 0;
            mDrawnBuckets += added;
            LLVKAttribPool::sceneDrew();
            pass_drew(mRefusals, view_index, pass);
            mPassWhy[view_index][pass] = nullptr;
            ++mPassesDrawn;
            ++mViewPassesDrawn[view_index];
            return true;
        }
        U32 drawn_buckets = 0;
        for (size_t k = 0; k + 3 < mExpandBuckets.size(); k += 4)
        {
            const U32 b = mExpandBuckets[k];
            VkCommandBuffer cmd = mBinder->bindChunk(mTable.bucketKey(b).chunk);
            if (!cmd && ctx.once && drawn_buckets)
            {
                // Some of the pass is drawn already: drawing it again would
                // blend it twice. This bucket's records are left out this
                // frame (a problem: the backend's checks count it).
                ++mBindFailed;
                ++mOnceMissed;
                ++mProblems;
                continue;
            }
            if (!cmd)
            {
                ++mBindFailed;
                refusePass(view_index, pass, "its pipeline or words could not be bound", false);
                return false;   // the caller draws the cut batches (the buckets drawn so far again: the same depth)
            }
            ++drawn_buckets;
            vkCmdDrawIndexedIndirectCount(cmd, out.buffer, out.offset + (VkDeviceSize)mExpandBuckets[k + 2] * COMMAND_BYTES, c.out,
                                          c.offset + (VkDeviceSize)(c.view.count_base + b) * 4, mExpandBuckets[k + 3], COMMAND_BYTES);
            ++mDrawnBuckets;
        }
        LLVKAttribPool::sceneDrew();
        pass_drew(mRefusals, view_index, pass);
        mPassWhy[view_index][pass] = nullptr;
        ++mPassesDrawn;
        ++mViewPassesDrawn[view_index];
        return true;
    }

    // research/205 3a-5: a span of a sorted pass, its records' commands in
    // the order of the array (the viewer's: its group's draw map), one
    // vkCmdDrawIndexedIndirect per run of records in one pool chunk (the
    // commands of a draw are processed in order). Nothing is drawn unless
    // every record of the span is placed (its command was written) and the
    // program's words are still those its rows were made from: the words a
    // record's row does not take from its own (the constants, the lights,
    // the material row's floats the pool does not set per batch). The 2D
    // samplers' words are the pass's as drawPass found them, which the
    // records' batches would bind (the viewer bound them so, as TexSetup
    // does for such a batch); a unit no record samples may differ.
    bool Scene::drawSorted(U32 view_index, U32 pass, U32 first, const U32* records, U32 count)
    {
        if (view_index != LLRHI::SCENE_VIEW_ALPHA || pass >= LLRHI::SCENE_MAX_PASSES || !records || !count)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a sorted span (research/205 3a-5)");
        const U32 frame = LLVKTracker::frame();
        if (!ready() || !mBinder || !mExpandReady || !drawing() || pass >= mSorted.size())
        {
            return false;
        }
        const SortedPass& s = mSorted[pass];
        if (s.frame != frame || !s.commands || first + count > s.count || first + count < first)
        {
            ++mSortedRefused;
            return false;
        }
        // Each record's chunk; a run per chunk.
        mSortedRuns.clear();
        for (U32 i = 0; i < count; ++i)
        {
            const auto it = mInfos.find(records[i]);
            if (it == mInfos.end() || it->second.state != PLACE_OK || it->second.row.chunk >= LLRHI::SCENE_UNPLACED)
            {
                ++mSortedRefused;
                return false;   // its command draws nothing (not chosen): the caller draws the span
            }
            const U32 chunk = it->second.row.chunk;
            if (mSortedRuns.empty() || mSortedRuns.back().chunk != chunk)
            {
                mSortedRuns.push_back({ chunk, i, 0 });
            }
            ++mSortedRuns.back().count;
        }

        LLVKScene::Binder::Words words;
        const char* why = nullptr;
        bool permanent = false;
        struct PassScope
        {
            LLVKScene::Binder* binder;
            ~PassScope() { binder->endPass(); }
        } scope{ mBinder };
        LLRHI::CensusTimer binder_census(LLRHI::CENSUS_SCENE_BINDER);    // research/208 S0: to the draws
        if (!mBinder->beginPass(s.attributes, s.attribute_count, s.materials, s.material_count, words, why, permanent))
        {
            ++mSortedRefused;
            if (permanent)
            {
                refusePass(view_index, pass, why, true);
            }
            return false;
        }
        bool same = words.count == s.words.count && words.matrix_word == s.words.matrix_word &&
                    words.priority_word == s.words.priority_word && words.material_word == s.words.material_word &&
                    words.material_vec4s == s.words.material_vec4s && words.palette_word == s.words.palette_word;
        for (U32 w = 0; same && w < words.count; ++w)
        {
            const bool record_word = s.word_textures.texture[w] >= 0 || s.word_textures.list[w] >= 0 || w == words.matrix_word ||
                                     w == words.material_word;
            same = record_word || s.words.units[w] >= 0 || words.words[w] == s.words.words[w];
        }
        if (same && words.material_word != U32_MAX)
        {
            for (U32 f = 0; same && f < words.material_vec4s * 4; ++f)
            {
                bool record_float = false;
                for (S16 at : words.material_at)
                {
                    record_float = record_float || at == (S16)f;
                }
                same = record_float || memcmp(&words.material[f], &s.words.material[f], sizeof(F32)) == 0;
            }
        }
        if (!same)
        {
            ++mSortedRefused;
            ++mSortedWordsChanged;
            return false;
        }
        for (const SortedRun& run : mSortedRuns)
        {
            if (!mBinder->chunkBindable(run.chunk))
            {
                ++mSortedRefused;
                return false;   // nothing drawn yet
            }
        }
        // research/208 S1: on the head's way, what the runs' binds could fail at, before any is drawn.
        if (mBinder->headWay() && !mBinder->admit())
        {
            ++mBindFailed;
            ++mSortedRefused;
            return false;   // nothing drawn yet
        }
        binder_census.stop();
        const LLRHI::CensusTimer record_census(LLRHI::CENSUS_SCENE_RECORD);     // research/208 S0
        U32 drawn = 0;
        for (const SortedRun& run : mSortedRuns)
        {
            VkCommandBuffer cmd = mBinder->bindChunk(run.chunk);
            if (!cmd)
            {
                ++mBindFailed;
                if (!drawn)
                {
                    ++mSortedRefused;
                    return false;   // nothing drawn: the caller draws the span
                }
                // Some of the span is drawn: drawing it again would blend it
                // twice. This run is left out this frame (a problem).
                ++mOnceMissed;
                ++mProblems;
                continue;
            }
            vkCmdDrawIndexedIndirect(cmd, s.commands, s.offset + (VkDeviceSize)(first + run.first) * COMMAND_BYTES, run.count,
                                     COMMAND_BYTES);
            ++drawn;
        }
        LLVKAttribPool::sceneDrew();
        ++mSortedSpans;
        mSortedDraws += drawn;
        mSortedRecords += count;
        return true;
    }

    // research/206 (stage 5): a shadow view's choice, and its records of
    // the passes made ready for the next native shadow pass: no expand, the
    // choice's own commands are drawn (their first instance is the record's
    // slot, which the shadow renderer's vertex shader reads the record and
    // its transform by). Any pending draws of an earlier call go first, so a
    // shadow pass never draws another cascade's.
    bool Scene::drawShadow(U32 view_index, const LLRHI::SceneShadowContext& ctx)
    {
        if (!LLRHI::sceneShadowView(view_index))   // research/206 5b: the sun cascades' and the spot lights'
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("a shadow view");
        mShadowPending.any = false;
        mShadowPending.masks = false;
        mShadowPending.skins = false;
        mShadowPending.skin_masks = false;
        mShadowPending.buckets.clear();
        mShadowPending.sampled.clear();
        if (!ready())
        {
            return false;
        }
        syncFrame();
        ViewState& v = mViews[view_index];
        const U32 frame = LLVKTracker::frame();
        if (v.desc_frame != frame || v.bits_frame != frame)
        {
            ++mNotChosen;
            return false;
        }
        // research/206 5c: passes drawn as masks (their records sample their diffuse texture).
        // research/219: and rigged passes (SCENE_SHADOW_SKINNED, skinned with their palette rows).
        bool masks = false;
        bool skins = false;
        for (U32 i = 0; ctx.modes && i < ctx.pass_count; ++i)
        {
            masks = masks || (ctx.modes[i] & LLRHI::SCENE_SHADOW_MODE_MASK) != 0;
            skins = skins || (ctx.modes[i] & LLRHI::SCENE_SHADOW_SKINNED) != 0;
        }
        const bool draw_masks = ctx.draw && masks;
        if (ctx.draw && skins && !shadowSkins())
        {
            ++mShadowRefused;
            ++mShadowSkinRefused;
            return false;
        }
        if (draw_masks)
        {
            // The masks' textures into the slot table before the draws read it: with the choice's
            // flush below, or flushed here when the view was chosen already this frame.
            if (!shadowMasks() || !shadowTextures(ctx, mShadowPending.sampled))
            {
                mShadowPending.sampled.clear();
                ++mShadowRefused;
                ++mShadowMaskRefused;
                return false;
            }
        }
        if (!v.choice.valid || v.choice.frame != frame)
        {
            applyPlaces(false);     // the rows have the places the groups were cut for
            if (!choose(view_index, false))
            {
                mShadowPending.sampled.clear();
                return false;
            }
            ++mShadowChoices;
        }
        else if (draw_masks && !mTexDirty.empty())
        {
            const VkCommandBuffer cmd = LLVKMirror::instance().uploadCommands();
            if (!cmd || !flush(cmd))
            {
                mShadowPending.sampled.clear();
                ++mShadowRefused;
                ++mShadowMaskRefused;
                return false;
            }
        }
        if (!ctx.draw)
        {
            return true;    // chosen (verify)
        }
        if (!ctx.view_proj || (!ctx.passes && ctx.pass_count) || LLVKTranslate::wanted() != 0 ||
            !mTableAddress[LLVKSceneTable::TABLE_RECORDS] || !mTableAddress[LLVKSceneTable::TABLE_TRANSFORMS])
        {
            mShadowPending.sampled.clear();
            ++mShadowRefused;
            return false;
        }
        // research/206 5b: every visible group still has all its records placed, as when the viewer cut it
        // (groupDrawable): a record whose place went would sit in no drawn bucket, and a cascade's cached
        // depth would keep that hole until it is drawn again. The caller draws them all instead.
        for (size_t w = 0; w < v.bits.size(); ++w)
        {
            for (U32 word = v.bits[w]; word; word &= word - 1)
            {
                const size_t group = w * 32 + (size_t)std::countr_zero(word) + 1;
                if (group >= mGroupRecords.size() || !mGroupRecords[group] || mGroupUndrawable[group])
                {
                    mShadowPending.sampled.clear();
                    ++mShadowRefused;
                    return false;
                }
            }
        }
        const Choice& c = v.choice;
        U32 mask_buckets = 0;
        U32 skin_buckets = 0;       // research/219
        U32 skin_mask_buckets = 0;
        // The cut groups' records of a chunk that cannot be drawn: the caller draws them all.
        auto refuse = [this](bool mask, bool skin)
        {
            mShadowPending.buckets.clear();
            mShadowPending.sampled.clear();
            ++mShadowRefused;
            mShadowMaskRefused += mask ? 1 : 0;
            mShadowSkinRefused += skin ? 1 : 0;
            return false;
        };
        for (U32 i = 0; i < ctx.pass_count; ++i)
        {
            const U32 pass = ctx.passes[i];
            const U8 mode = ctx.modes ? (U8)(ctx.modes[i] & LLRHI::SCENE_SHADOW_MODE_MASK) : 0;
            const bool skinned = ctx.modes && (ctx.modes[i] & LLRHI::SCENE_SHADOW_SKINNED) != 0;     // research/219
            if (pass >= LLRHI::SCENE_MAX_PASSES)
            {
                continue;
            }
            if (mode > LLRHI::ShadowPacket::MODE_ALPHA_TEST)
            {
                return refuse(true, skinned);   // not a mode the mask shaders know
            }
            // research/219: the skinned shaders read every record's palette from its flags, the others its
            // transform: a pass whose records are not all of the kind its mode draws is the caller's.
            if (!LLVKSceneShader::shadow_pass_palette_ok(mPassSkinned[pass], mPassPlain[pass], skinned))
            {
                return refuse(mode != 0, true);
            }
            for (U32 b : mTable.passBuckets(pass))
            {
                const U32 chunk = mTable.bucketKey(b).chunk;
                if (b >= c.capacity.size() || !c.capacity[b] || chunk >= LLRHI::SCENE_UNPLACED)
                {
                    continue;   // no pages in the choice, or records without a place (never drawn)
                }
                LLVKScene::ShadowDraws::Bucket bucket;
                bucket.positions = LLVKAttribPool::streamBuffer(chunk, 0, 16);     // LLVertexBuffer's LLVector4a positions
                bucket.indices = LLVKAttribPool::indexBuffer(chunk);
                if (!bucket.positions || !bucket.indices)
                {
                    return refuse(false, skinned);
                }
                bucket.mode = mode;
                if (mode)
                {
                    // research/206 5c: LLVertexBuffer's TYPE_TEXCOORD0 (vec2) and TYPE_COLOR (4 bytes), which the
                    // masks' programs read (so their records' places hold them); no colors: white, as a packet's.
                    bucket.texcoords = LLVKAttribPool::streamBuffer(chunk, 2, 8);
                    bucket.colors = LLVKAttribPool::streamBuffer(chunk, 6, 4);
                    if (!bucket.texcoords)
                    {
                        return refuse(true, skinned);
                    }
                    ++mask_buckets;
                }
                if (skinned)
                {
                    // research/219: LLVertexBuffer's TYPE_WEIGHT4 (vec4), which the rigged programs read.
                    bucket.skinned = true;
                    bucket.weights = LLVKAttribPool::streamBuffer(chunk, WEIGHT4_LOCATION, 16);
                    if (!bucket.weights)
                    {
                        return refuse(mode != 0, true);
                    }
                    ++(mode ? skin_mask_buckets : skin_buckets);
                }
                bucket.commands = c.offset + ((VkDeviceSize)c.view.command_base + (VkDeviceSize)c.first_command[b] * 5) * 4;
                bucket.count = c.offset + ((VkDeviceSize)c.view.count_base + b) * 4;
                bucket.capacity = c.capacity[b];
                mShadowPending.buckets.push_back(bucket);
            }
        }
        if (mShadowPending.buckets.empty())
        {
            // The viewer cut batches of these passes (their groups' records are all placed), yet the
            // choice has no bucket of them with a place: it draws them itself.
            return refuse(false, false);
        }
        mShadowPending.any = true;
        mShadowPending.masks = mask_buckets != 0;
        mShadowPending.skins = skin_buckets != 0;
        mShadowPending.skin_masks = skin_mask_buckets != 0;
        mShadowSkinBuckets += skin_buckets + skin_mask_buckets;
        if (!mShadowPending.masks)
        {
            mShadowPending.sampled.clear();     // no mask of them in the choice: nothing samples them
        }
        mShadowPending.buffer = c.out;
        mShadowPending.records = mTableAddress[LLVKSceneTable::TABLE_RECORDS];
        mShadowPending.transforms = mTableAddress[LLVKSceneTable::TABLE_TRANSFORMS];
        mShadowPending.uv_transforms = mTableAddress[LLVKSceneTable::TABLE_UV];
        mShadowPending.materials = mTableAddress[LLVKSceneTable::TABLE_MATERIALS];
        mShadowPending.texture_slots = mTexTableAddress;
        mShadowPending.texture_slot_count = TEX_SLOTS;
        mShadowPending.white_slot = LLVKBindless::WHITE_SLOT;
        LLRHITexAnim::splitTime(LLRHI::sTexAnimTime, mShadowPending.anim_time[0], mShadowPending.anim_time[1]);
        mShadowMaskBuckets += mask_buckets;
        memcpy(mShadowPending.view_proj, ctx.view_proj, sizeof(mShadowPending.view_proj));
        mShadowPendingFrame = frame;
        ++mShadowReady;
        LLVKAttribPool::sceneDrew();    // a later write this frame to a place it reads moves the place first
        mUsed = true;
        return true;
    }

    // research/206 5c: the masks' textures (the viewer's numbers and their GL
    // names) resolved to bindless slots as the shadow's packets resolve
    // theirs (LLVKBindless::slotFor), into the slot table (written at the
    // next flush); their names for the pass to ready them for sampling.
    // False: a number the table cannot hold (the caller draws them).
    bool Scene::shadowTextures(const LLRHI::SceneShadowContext& ctx, std::vector<U32>& sampled)
    {
        if (ctx.texture_count && (!ctx.textures || !ctx.texture_names))
        {
            return false;
        }
        LLVKBindless::slotFor(0);   // the stand-in (a record without a texture, a number past the table, samples it)
        for (U32 i = 0; i < ctx.texture_count; ++i)
        {
            const U32 id = ctx.textures[i];
            const U32 name = ctx.texture_names[i];
            if (!id || id >= TEX_SLOTS || !name)
            {
                return false;
            }
            const U32 slot = LLVKBindless::slotFor(name);
            ++mShadowTextures;
            if (mTexSlots[id] != slot)
            {
                mTexSlots[id] = slot;
                mTexDirty.push_back(id);
                ++mTexChanged;
            }
            sampled.push_back(name);
        }
        return true;
    }

    bool Scene::shadowMasks() const
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        return mShadowMasksReady && mReady && !mBroken && mExpandReady && mTexTableAddress != 0 &&
               mTableAddress[LLVKSceneTable::TABLE_UV] != 0 && mTableAddress[LLVKSceneTable::TABLE_MATERIALS] != 0 &&
               LLVKTranslate::wanted() == 0;
    }

    // research/219: the skinned shaders read the record's flags (its palette
    // base) and the palette table (set 0 binding 2, bound by every native
    // shadow pass): nothing of the texture tables.
    bool Scene::shadowSkins() const
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        return mShadowSkinsReady && mReady && !mBroken && mTableAddress[LLVKSceneTable::TABLE_RECORDS] != 0 &&
               LLVKTranslate::wanted() == 0;
    }

    bool Scene::takeShadowDraws(LLVKScene::ShadowDraws& out)
    {
        out.any = false;
        out.masks = false;
        out.skins = false;
        out.skin_masks = false;
        out.buckets.clear();
        out.sampled.clear();
        if (mShadowPending.any && mShadowPendingFrame == LLVKTracker::frame())
        {
            std::swap(out, mShadowPending);     // keeps the vectors' storage
        }
        mShadowPending.any = false;
        mShadowPending.masks = false;
        mShadowPending.skins = false;
        mShadowPending.skin_masks = false;
        mShadowPending.buckets.clear();
        mShadowPending.sampled.clear();
        return out.any;
    }

    void Scene::shadowDrawn(bool drawn, U32 buckets)
    {
        if (drawn)
        {
            ++mShadowDrawn;
            mShadowBuckets += buckets;
        }
        else
        {
            ++mShadowLost;
        }
    }

    void Scene::collect(U32 view_index, U32 pass, std::vector<U32>& drawn, std::vector<U32>& bounded_out)
    {
        drawn.clear();
        bounded_out.clear();
        if (view_index >= LLRHI::SCENE_VIEWS)
        {
            return;
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("the CPU's choice (verify)");
        const ViewState& v = mViews[view_index];
        mTable.collect(v.desc, v.bits.empty() ? nullptr : v.bits.data(), (U32)v.bits.size(), pass, drawn, bounded_out);
    }

    void Scene::shutdown()
    {
        if (!mTried)
        {
            return;     // nothing was made
        }
        VkDevice device = LLVKDevice::instance().getDevice();
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        auto destroy = [&](Buffer& b)
        {
            if (b.buffer)
            {
                vmaDestroyBuffer(allocator, b.buffer, b.memory);
            }
            b = Buffer();
        };
        for (Buffer& b : mTables)
        {
            destroy(b);
        }
        destroy(mTexTable);
        mTexTableAddress = 0;   // research/206 5c
        mQueuedChoices.clear();     // research/209 E4: their sets and room go with the device
        mQueuedExpands.clear();
        LLVKRender::destroySecondaryPools();    // research/208 13.3: the passes' secondary command buffers (the device is idle)
        for (std::vector<VkDescriptorPool>& pools : mThreadPools)     // research/208 S3
        {
            for (VkDescriptorPool pool : pools)
            {
                vkDestroyDescriptorPool(device, pool, nullptr);
            }
            pools.clear();
        }
        for (size_t& index : mThreadPoolIndex)
        {
            index = 0;
        }
        mShadowMasksReady = false;
        for (FrameSlot& slot : mSlots)
        {
            for (Buffer& b : slot.scratch)
            {
                destroy(b);
            }
            slot.scratch.clear();
            for (Buffer& b : slot.upload)
            {
                destroy(b);
            }
            slot.upload.clear();
            slot.upload_bytes = 0;
            destroy(slot.readback);
            destroy(slot.expand.readback);
            slot.verify.pending = false;
            slot.expand.pending = false;
        }
        if (mPipeline)
        {
            vkDestroyPipeline(device, mPipeline, nullptr);
            mPipeline = VK_NULL_HANDLE;
        }
        if (mLayout)
        {
            vkDestroyPipelineLayout(device, mLayout, nullptr);
            mLayout = VK_NULL_HANDLE;
        }
        if (mSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }
        if (mExpandPipeline)
        {
            vkDestroyPipeline(device, mExpandPipeline, nullptr);
            mExpandPipeline = VK_NULL_HANDLE;
        }
        if (mExpandLayout)
        {
            vkDestroyPipelineLayout(device, mExpandLayout, nullptr);
            mExpandLayout = VK_NULL_HANDLE;
        }
        if (mExpandSetLayout)
        {
            vkDestroyDescriptorSetLayout(device, mExpandSetLayout, nullptr);
            mExpandSetLayout = VK_NULL_HANDLE;
        }
        LLVKShader::destroy(mShader);
        LLVKShader::destroy(mExpandShader);
        mShadowPending.any = false;     // research/206
        mShadowPending.buckets.clear();
        for (VkDeviceAddress& address : mTableAddress)
        {
            address = 0;
        }
        mReady = false;
        mBroken = true;
        mDrawing = false;
        mExpandReady = false;
    }

    std::string Scene::takeStats(U32 frames)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        own("the perf line");
        if (!mUsed)
        {
            return std::string();
        }
        frames = llmax(frames, 1u);
        const LLVKSceneTable::Counters& n = mTable.counters();
        std::ostringstream out;
        out << "scene " << LLRHI::sceneModeName(LLRHI::sceneMode()) << (drawing() ? " drawing" : " choosing")
            << ": records " << mTable.liveRecords() << " (pages " << mTable.rowSpan(LLVKSceneTable::TABLE_PAGES) << ", buckets "
            << mTable.bucketSpan() << ", groups " << mTable.groupSpan() << "), added " << n.added << ", updated " << n.updated << " (moved "
            << n.moved << "), removed " << n.removed << ", refused " << n.refused + mRefusedRecords << ", stale " << n.stale
            << ", transforms written " << n.transform_writes << ", uploads " << (mUploadBytes / frames) << " B a frame, choices "
            << (F32)mChoices / frames << " a frame";
        // research/198 5.5: the records' places (the census of what keeps a group from being cut).
        U32 states[PLACE_STATES] = {};
        for (const auto& kv : mInfos)
        {
            ++states[kv.second.state < PLACE_STATES ? kv.second.state : PLACE_NONE];
        }
        out << "; places:";
        for (U32 s = 0; s < PLACE_STATES; ++s)
        {
            out << (s ? ", " : " ") << PLACE_NAMES[s] << " " << states[s];
        }
        out << " (place writes " << mPlaceUpdates << ", failed " << mPlaceUpdateFailed << ")";
        out << "; drawn passes " << (F32)mPassesDrawn / frames << " a frame, buckets " << (F32)mDrawnBuckets / frames << ", expands "
            << (F32)mExpands / frames << " (" << mExpandCommands / frames << " command rooms), matrix rows " << mMatrixRows / frames
            << ", texture slots " << mTexResolved / frames << " (white " << mTexWhite / frames << ", changed " << mTexChanged << ")";
        if (mPassesRefused || mNotChosen || mNoBinder || mBindFailed)
        {
            out << "; refused passes " << mPassesRefused << ", no view " << mNotChosen << ", not drawing " << mNoBinder << ", bind failed "
                << mBindFailed;
        }
        bool any = false;
        const U32 now = LLVKTracker::frame();
        for (U32 p = 0; p < LLRHI::SCENE_MAX_PASSES; ++p)
        {
            if (mRefusals.never[p])
            {
                const char* why = nullptr;
                for (U32 view = 0; view < LLRHI::SCENE_VIEWS && !why; ++view)
                {
                    why = mPassWhy[view][p];
                }
                out << (any ? ", " : "; usual way: ") << "pass " << p << " (its program) " << (why ? why : "?");
                any = true;
            }
            for (U32 view = 0; view < LLRHI::SCENE_VIEWS; ++view)
            {
                if (mRefusals.later[view][p] && (S32)(now - mRefusals.later_retry[view][p]) < 0)    // research/209: while it waits
                {
                    out << (any ? ", " : "; usual way: ") << "pass " << p << " (for now, " << VIEW_NAMES[view] << ") "
                        << (mPassWhy[view][p] ? mPassWhy[view][p] : "?");
                    any = true;
                }
            }
        }
        // research/209: the probe faces and the mirrors.
        for (U32 view = LLRHI::SCENE_VIEW_PROBE; view < LLRHI::SCENE_VIEW_COUNT; ++view)
        {
            if (mViewChoices[view] || mViewPassesRefused[view])
            {
                out << "; " << VIEW_NAMES[view] << ": choices " << (F32)mViewChoices[view] / frames << " a frame, passes drawn "
                    << (F32)mViewPassesDrawn[view] / frames << ", refused " << mViewPassesRefused[view] << ", matrix rows "
                    << mViewMatrixRows[view] / frames << ", expands verified " << mViewVerified[view];
            }
        }
        memset(mViewChoices, 0, sizeof(mViewChoices));
        memset(mViewPassesDrawn, 0, sizeof(mViewPassesDrawn));
        memset(mViewPassesRefused, 0, sizeof(mViewPassesRefused));
        memset(mViewVerified, 0, sizeof(mViewVerified));
        memset(mViewMatrixRows, 0, sizeof(mViewMatrixRows));
        if (mCompared || mExpandCompared || mPlaceChecked)
        {
            out << ", verify " << mCompared << " compared, " << mMismatches << " buckets differ; expands " << mExpandCompared << " compared, "
                << mExpandMismatches << " commands differ; places " << mPlaceChecked << " checked, " << mPlaceMismatches << " differ";
        }
        if (mPaletteRefused)
        {
            // research/219: passes whose rigged records their program cannot draw (drawn the usual way).
            out << ", PASSES REFUSED FOR THEIR RIGGED RECORDS " << mPaletteRefused;
            mPaletteRefused = 0;
        }
        if (mShadowChoices || mShadowReady || mShadowRefused || mShadowLost)
        {
            // research/206: the sun shadow views (stage 5).
            out << "; shadow (" << LLRHI::sceneModeName(LLRHI::sceneShadowMode()) << "): choices " << (F32)mShadowChoices / frames
                << " a frame, passes made ready " << (F32)mShadowReady / frames << ", drawn " << (F32)mShadowDrawn / frames << " (buckets "
                << (F32)mShadowBuckets / frames << ")";
            if (mShadowMaskBuckets || mShadowTextures || mShadowMaskRefused)
            {
                // research/206 5c: the masks.
                out << ", mask buckets " << (F32)mShadowMaskBuckets / frames << " (textures resolved " << (F32)mShadowTextures / frames
                    << ", refused " << mShadowMaskRefused << ")";
            }
            if (mShadowSkinBuckets || mShadowSkinRefused)
            {
                // research/219: the rigged records.
                out << ", skinned buckets " << (F32)mShadowSkinBuckets / frames << " (refused " << mShadowSkinRefused << ")";
            }
            if (mShadowRefused || mShadowLost)
            {
                out << ", refused " << mShadowRefused << ", not drawn by the pass " << mShadowLost;
            }
        }
        if (mPriorityRows || mOnceMissed)
        {
            // research/205 3a-3: the alpha view's rows with an OIT priority, and buckets left out rather than drawn twice.
            out << "; alpha rows with a priority " << mPriorityRows / frames << " a frame";
            if (mOnceMissed)
            {
                out << ", ALPHA BUCKETS LEFT OUT " << mOnceMissed;
            }
        }
        if (mSortedPrepared || mSortedSpans || mSortedRefused)
        {
            // research/205 3a-5: the sorted alpha (a hybrid session's world, no OIT).
            out << "; sorted alpha: passes expanded " << mSortedPrepared << ", spans drawn " << mSortedSpans << " (" << mSortedDraws
                << " indirect draws, " << mSortedRecords / frames << " records a frame), spans the pool drew " << mSortedRefused;
            if (mSortedWordsChanged)
            {
                out << " (" << mSortedWordsChanged << " as the program's words changed)";
            }
        }
        {
            // fix-r8 (bugloop round 9): the scene's uploads, its own room, and the views drawn the usual way for want of room.
            VkDeviceSize own = 0;
            for (const FrameSlot& slot : mSlots)
            {
                own = llmax(own, slot.upload_bytes);
            }
            out << "; upload room: the largest frame asked " << (mUploadPeak >> 10) << " KB (own room " << (own >> 10) << " of "
                << (mUploadCap >> 10) << " KB a frame slot)";
            if (mUploadShared)
            {
                out << ", to the frame ring " << mUploadShared;
            }
            bool short_any = false;
            for (U32 view = 0; view < LLRHI::SCENE_VIEWS; ++view)
            {
                if (mViewShorts[view] || mViewShortSkips[view])
                {
                    out << (short_any ? ", " : ", WITHOUT ROOM (that frame the usual way): ") << VIEW_NAMES[view] << " " << mViewShorts[view]
                        << " (waited " << mViewShortSkips[view] << ")";
                    short_any = true;
                }
            }
            if (mUploadPeak * 4 > (U64)mUploadCap * 3 && mUploadNearLogged < VERIFY_LOG)
            {
                ++mUploadNearLogged;
                LL_WARNS("VKScene") << "retained scene: its uploads came near its own room (the largest frame " << (mUploadPeak >> 10)
                                    << " KB of " << (mUploadCap >> 10)
                                    << " KB a frame slot); past it they share the frame ring, then a view is drawn the usual way that frame"
                                    << LL_ENDL;
            }
            mUploadPeak = 0;
            mUploadShared = 0;
            memset(mViewShorts, 0, sizeof(mViewShorts));
            memset(mViewShortSkips, 0, sizeof(mViewShortSkips));
        }
        if (mBinder)
        {
            // research/208 S1: the pass head's checks and admissions.
            const std::string head = mBinder->takeReport(frames);
            if (!head.empty())
            {
                out << "; " << head;
            }
        }
        {
            // research/208 S2: the render thread.
            const std::string rt = LLVKRender::takeReport(frames);
            if (!rt.empty())
            {
                out << "; " << rt << " (the scene's passes to the thread: expands " << (F32)mExpandsByThread / frames << ", draws "
                    << (F32)mDrawsByThread / frames << " a frame)";
            }
            mExpandsByThread = mDrawsByThread = 0;
        }
        {
            // research/209 E4 (vk-s4a4): the queued choices and expands; research/208 13.3: the secondary command buffers.
            const std::string batch = batchStats(frames);
            if (!batch.empty())
            {
                out << "; " << batch;
            }
            const std::string secondary = secondaryStats(frames);
            if (!secondary.empty())
            {
                out << "; " << secondary;
            }
        }
        {
            // research/208 S3: the passes handed over whole, and the tables' owner.
            const std::string owner = threadStats(frames);
            if (!owner.empty())
            {
                out << "; " << owner;
            }
        }
        out << (mBroken ? ", BROKEN" : "");
        mPriorityRows = mOnceMissed = 0;
        mSortedPrepared = mSortedSpans = mSortedDraws = mSortedRefused = mSortedWordsChanged = 0;
        mSortedRecords = 0;
        mShadowChoices = mShadowReady = mShadowRefused = mShadowDrawn = mShadowLost = 0;
        mShadowBuckets = 0;
        mShadowMaskBuckets = mShadowTextures = 0;
        mShadowMaskRefused = 0;
        mShadowSkinBuckets = mShadowSkinRefused = 0;
        mTable.resetCounters();
        mChoices = mDrawnBuckets = mNoBinder = mBindFailed = mNotChosen = 0;
        mUploadBytes = 0;
        mCompared = mMismatches = 0;
        mPassesDrawn = mPassesRefused = mExpands = mPlaceUpdates = mPlaceUpdateFailed = mRefusedRecords = 0;
        mMatrixRows = mExpandCommands = 0;
        mTexResolved = mTexWhite = mTexChanged = 0;
        mExpandCompared = mExpandMismatches = mPlaceChecked = mPlaceMismatches = 0;
        return out.str();
    }
}

void LLVKScene::shutdown()
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    LLVKAttribPool::setNumberListener(nullptr);
    LLVKAttribPool::sceneDrawing(false);
    LLVKMirror::sAfterRecorderFinish = nullptr;     // research/209 E4: nothing is submitted any more
    sScene.takeBack();  // research/208 S3: the tables back from the render thread first
    sScene.shutdown();
    // The viewer's events after this (draw infos and groups going at exit)
    // return at once.
    if (LLRHI::sScene == &sScene)
    {
        LLRHI::sScene = nullptr;
    }
}

void LLVKScene::install()
{
    const LLRHI::SceneMode mode = LLRHI::sceneMode();
    if (mode == LLRHI::SceneMode::Off)
    {
        return;     // LLRHI::sScene stays null: the viewer does nothing per frame
    }
    LL_INFOS("VKScene") << "retained scene: " << LLRHI::sceneModeName(mode) << " (LLRHI_SCENE, research/197 stage 2, research/198 5.5, 5.6); "
                        << (mode == LLRHI::SceneMode::Verify ? "verify (chosen, expanded and compared, never drawn)"
                                                             : mode == LLRHI::SceneMode::On
                                                                   ? "drawing the main view's opaque, masked, material, fullbright and glow batches"
                                                                   : "verify, then drawing")
                        << LL_ENDL;
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        LLVKAttribPool::setNumberListener(&on_number_changed);
    }
    // research/209 E4 (vk-s4a4): every submission records what the scene queued (nothing while the switch is off).
    LLVKMirror::sAfterRecorderFinish = &scene_submission_cut;
    LLRHI::sScene = &sScene;
}

bool LLVKScene::takeShadowDraws(ShadowDraws& out)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    return sScene.takeShadowDraws(out);
}

void LLVKScene::shadowDrawn(bool drawn, U32 buckets)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    sScene.shadowDrawn(drawn, buckets);
}

void LLVKScene::setShadowMasks(bool ready)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    sScene.setShadowMasks(ready);
}

void LLVKScene::setShadowSkins(bool ready)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    sScene.setShadowSkins(ready);
}

void LLVKScene::setBinder(Binder* binder)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    sScene.setBinder(binder);
}

bool LLVKScene::collectRayRecords(const U32 pass_mask[4], std::vector<RayRecord>& out, U64& serial)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    return sScene.collectRayRecords(pass_mask, out, serial);
}

U64 LLVKScene::writeSerial()
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    return sScene.tableWriteSerial();
}

std::string LLVKScene::takeStats(U32 frames)
{
    return LLRHI::sScene ? sScene.takeStats(frames) : std::string();
}

std::string LLVKScene::deviceGaps(const VkPhysicalDeviceFeatures& features, bool draw_indirect_count, const VkPhysicalDeviceLimits& limits)
{
    const LLVKSceneTable::Limits table;
    const U32 records = table.pages * LLVKSceneTable::PAGE;
    const VkDeviceSize record_bytes = (VkDeviceSize)records * sizeof(LLVKSceneTable::Record);
    std::string gaps;
    auto gap = [&gaps](const std::string& what)
    {
        gaps += (gaps.empty() ? "" : ", ");
        gaps += what;   // no ", " inside (plus/vkcheck splits the list at it)
    };
    if (!draw_indirect_count)
    {
        gap("drawIndirectCount (Vulkan 1.2; LLRHI_SCENE)");
    }
    if (!features.multiDrawIndirect)
    {
        gap("multiDrawIndirect (Vulkan 1.0; LLRHI_SCENE)");
    }
    if (!features.drawIndirectFirstInstance)
    {
        gap("drawIndirectFirstInstance (Vulkan 1.0; LLRHI_SCENE)");
    }
    if (limits.maxDrawIndirectCount < records)
    {
        gap(std::to_string(records) + " draws per indirect draw (maxDrawIndirectCount " + std::to_string(limits.maxDrawIndirectCount) +
            "; LLRHI_SCENE)");
    }
    if (limits.maxPerStageDescriptorStorageBuffers < BINDINGS || limits.maxDescriptorSetStorageBuffers < BINDINGS)
    {
        gap(std::to_string(BINDINGS) + " storage buffers per stage (this GPU allows " +
            std::to_string(llmin(limits.maxPerStageDescriptorStorageBuffers, limits.maxDescriptorSetStorageBuffers)) + "; LLRHI_SCENE)");
    }
    if (limits.maxStorageBufferRange < record_bytes)
    {
        gap(std::to_string(record_bytes >> 20) + " MB storage buffer range (maxStorageBufferRange " +
            std::to_string(limits.maxStorageBufferRange >> 20) + " MB; LLRHI_SCENE)");
    }
    if (limits.maxComputeWorkGroupCount[0] < llmax(table.pages, table.buckets))
    {
        gap(std::to_string(llmax(table.pages, table.buckets)) + " compute workgroups (maxComputeWorkGroupCount " +
            std::to_string(limits.maxComputeWorkGroupCount[0]) + "; LLRHI_SCENE)");
    }
    return gaps;
}
