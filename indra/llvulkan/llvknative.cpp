/**
 * @file llvknative.cpp
 * @brief Draws recorded directly in Vulkan from viewer draw packets.
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

#include "llvknative.h"

#include "llglheaders.h"
#include "llrhihash.h"
#include "llvkdevice.h"
#include "llvkframetable.h"
#include "llvkmirror.h"
#include "llvkshader.h"
#include "llvktracker.h"
#include "llvkbindless.h"
#include "llvkgpudriven.h"
#include "llvkgpulist.h"
#include "llvkscene.h"        // research/197 stage 2
#include "llvkrtas.h"         // research/230 R2
#include "llvkglare.h"
#include "llvkgtao.h"
#include "llvklook.h"
#include "llvkupscale.h"
#include "llvktaa.h"
#include "llvkatmosphere.h"
#include "llvkrain.h"
#include "llvkskinfill.h"
#include "llvkskinsss.h"
#include "llvkssgi.h"
#include "llvkssr.h"
#include "llvkwetlook.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using Packet = LLRHI::ShadowPacket;

    // ------------------------------------------------------------------
    // Shadow casters: one depth-only pipeline and one alpha-tested pipeline
    // per render target format, with the same math as the GL shaders.

    struct ShadowPush
    {
        F32 mvp[16];
        F32 tex_row0[4];
        F32 tex_row1[4];
        U32 textures[4];
        F32 params[4];      // alpha cutoff, shadow target width, mode, unused
    };
    static_assert(sizeof(ShadowPush) == 128, "push constant budget is 128 bytes");

    const char* const PUSH_BLOCK =
        "layout(push_constant) uniform ShadowPush { mat4 mvp; vec4 tex_row0; vec4 tex_row1; uvec4 textures; vec4 params; } pc;\n";

    // research/206 (stage 5): the retained scene's casters of a sun shadow
    // pass (LLVKScene::ShadowDraws). The push constants' first 80 bytes: the
    // pass's projection x view, then the record table's and the transform
    // table's device addresses (each as two 32-bit halves, low first).
    // 5c: the masks' shaders read the rest (the depth only shader reads the
    // first 80 bytes): the texture transform and material tables' and the
    // texture slot table's addresses, the texture animations' time, the
    // dither's target width, the bucket's mode (pushed again per bucket),
    // the stand-in's slot and the numbers the slot table holds.
    struct ScenePush
    {
        F32 view_proj[16];
        U32 tables[4];          // records, transforms
        U32 tables2[4];         // 5c: texture transforms, materials
        U32 slot_table[2];      // 5c
        F32 anim_time[2];       // 5c: LLRHI::sTexAnimTime (high, low)
        F32 target_width;       // 5c: as the packets' params.y
        U32 mode;               // 5c: ShadowPacket::Mode of the bucket
        U32 white_slot;         // 5c
        U32 slot_count;         // 5c
    };
    static_assert(sizeof(ScenePush) == sizeof(ShadowPush), "the whole push constant range");
    static_assert(offsetof(ScenePush, mode) == 116, "SCENE_PUSH_BLOCK's mode");
    const char* const SCENE_PUSH_BLOCK =
        "layout(push_constant) uniform ScenePush { mat4 view_proj; uvec4 tables; uvec4 tables2; uvec2 slot_table; vec2 anim_time;"
        " float target_width; uint mode; uint white_slot; uint slot_count; } pc;\n";
    // shadowV over a record: its slot is the draw's first instance; its
    // transform (scene_cull.comp's Record, word 7) names the transform row.
    const char* const SCENE_SHADOW_VS =
        "#version 450\n"
        "#extension GL_EXT_buffer_reference : require\n"
        "#extension GL_EXT_buffer_reference_uvec2 : require\n"
        "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer llrhi_scene_records { uvec4 w[]; };\n"
        "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer llrhi_scene_transforms { mat4 m[]; };\n"
        "layout(push_constant) uniform ScenePush { mat4 view_proj; uvec4 tables; } pc;\n"
        "layout(location = 0) in vec3 position;\n"
        "void main()\n"
        "{\n"
        "    llrhi_scene_records records = llrhi_scene_records(pc.tables.xy);\n"
        "    llrhi_scene_transforms transforms = llrhi_scene_transforms(pc.tables.zw);\n"
        "    uint transform = records.w[uint(gl_InstanceIndex) * 4u + 1u].w;\n"
        "    gl_Position = pc.view_proj * (transforms.m[transform] * vec4(position, 1.0));\n"
        "}\n";

    // research/206 5c: shadowAlphaMaskV / treeShadowV over a record (the
    // packets' alpha_vs with the record's rows): its transform, its texture
    // transform (scene_cull.comp's Record word 12; a texture animation's
    // description turned into its matrix at the frame's animation time, as
    // LLRHITexAnim::matrixAt in floats), its material (word 13: its diffuse
    // texture's number, LLRHI::SceneMaterialDesc word 0, and its cutoff,
    // word 11 = value 5) and the number's slot from the scene's slot table.
    // SCENE_KIND_BITS: LLRHITexAnim::KIND_SCROLL's bits (put in when built).
    const char* const SCENE_MASK_VS_BODY =
        "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer llrhi_scene_records { uvec4 w[]; };\n"
        "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer llrhi_scene_transforms { mat4 m[]; };\n"
        "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer llrhi_scene_materials { uvec4 w[]; };\n"
        "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer llrhi_scene_slots { uint s[]; };\n"
        "layout(location = 0) in vec3 position;\n"
        "layout(location = 1) in vec2 texcoord0;\n"
        "layout(location = 2) in vec4 diffuse_color;\n"
        "layout(location = 0) out vec2 vary_texcoord0;\n"
        "layout(location = 1) out float vertex_alpha;\n"
        "layout(location = 2) flat out uint texture_slot;\n"
        "layout(location = 3) out float target_pos_x;\n"
        "layout(location = 4) out float post_pos_w;\n"
        "layout(location = 5) flat out float alpha_cutoff;\n"
        "float scene_fmod(float x, float y) { return x - y * trunc(x / y); }\n"     // C's fmodf: the sign of x
        "vec2 scene_uv(uint row, vec2 tc)\n"
        "{\n"
        "    if (row == 0u) return tc;\n"
        "    mat4 m = llrhi_scene_transforms(pc.tables2.xy).m[row];\n"
        "    if (m[3][3] == -1.0 && floatBitsToUint(m[3][2]) == SCENE_KIND_BITS)\n"
        "    {\n"
        "        float counter = ((pc.anim_time.x - m[0][1]) + (pc.anim_time.y - m[0][2])) * m[0][0] + m[0][3];\n"
        "        counter = scene_fmod(counter, m[1][0]) + m[1][1];\n"
        "        float size_x = m[1][2], size_y = m[1][3];\n"
        "        float scale_s, scale_t, off_s, off_t;\n"
        "        if (size_x != 0.0 && size_y != 0.0)\n"
        "        {\n"
        "            scale_s = 1.0 / size_x;\n"
        "            scale_t = 1.0 / size_y;\n"
        "            float x_frame = scene_fmod(counter, size_x);\n"
        "            int y_frame = int(counter / size_x);\n"
        "            off_s = (-0.5 + 0.5 * scale_s) + x_frame * scale_s;\n"
        "            off_t = (0.5 - 0.5 * scale_t) - float(y_frame) * scale_t;\n"
        "        }\n"
        "        else\n"
        "        {\n"
        "            off_s = counter;\n"
        "            off_t = 0.0;\n"
        "            scale_s = m[3][0];\n"
        "            scale_t = m[3][1];\n"
        "        }\n"
        "        vec4 r = m[2];\n"      // R00 R01 R10 R11
        "        float row3_0 = -0.5 * r.x + -0.5 * r.z;\n"
        "        float row3_1 = -0.5 * r.y + -0.5 * r.w;\n"
        "        return vec2(r.x * scale_s * tc.x + r.z * scale_s * tc.y + (row3_0 * scale_s + (off_s + 0.5)),\n"
        "                    r.y * scale_t * tc.x + r.w * scale_t * tc.y + (row3_1 * scale_t + (off_t + 0.5)));\n"
        "    }\n"
        "    return (m * vec4(tc, 0.0, 1.0)).xy;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    uint i = uint(gl_InstanceIndex) * 4u;\n"
        "    llrhi_scene_records records = llrhi_scene_records(pc.tables.xy);\n"
        "    uvec4 r1 = records.w[i + 1u];\n"
        "    uvec4 r3 = records.w[i + 3u];\n"
        "    vec4 pos = pc.view_proj * (llrhi_scene_transforms(pc.tables.zw).m[r1.w] * vec4(position, 1.0));\n"
        "    gl_Position = pos;\n"
        "    vary_texcoord0 = scene_uv(r3.x, texcoord0);\n"
        "    vertex_alpha = diffuse_color.a;\n"
        "    llrhi_scene_materials materials = llrhi_scene_materials(pc.tables2.zw);\n"
        "    uint id = materials.w[r3.y * 6u].x;\n"
        "    texture_slot = id != 0u && id < pc.slot_count ? llrhi_scene_slots(pc.slot_table).s[id] : pc.white_slot;\n"
        "    alpha_cutoff = uintBitsToFloat(materials.w[r3.y * 6u + 2u].w);\n"
        "    target_pos_x = 0.5 * (pc.target_width - 1.0) * pos.x;\n"
        "    post_pos_w = pos.w;\n"
        "}\n";
    // shadowAlphaMaskF (mode 1, mode 2 = IS_FULLBRIGHT) and treeShadowF (mode 3), as the packets' alpha_fs.
    const char* const SCENE_MASK_FS_BODY =
        "layout(set = 0, binding = 0) uniform sampler2D textures[];\n"
        "layout(location = 0) in vec2 vary_texcoord0;\n"
        "layout(location = 1) in float vertex_alpha;\n"
        "layout(location = 2) flat in uint texture_slot;\n"
        "layout(location = 3) in float target_pos_x;\n"
        "layout(location = 4) in float post_pos_w;\n"
        "layout(location = 5) flat in float alpha_cutoff;\n"
        "void main()\n"
        "{\n"
        "    float alpha = texture(textures[nonuniformEXT(texture_slot)], vary_texcoord0).a;\n"
        "    if (alpha < alpha_cutoff) discard;\n"
        "    if (pc.mode == 3u) return;\n"
        "    if (pc.mode == 1u) alpha *= vertex_alpha;\n"
        "    if (alpha < 0.05) discard;\n"
        "    if (alpha < 0.88 && fract(0.5 * floor(target_pos_x / post_pos_w)) < 0.25) discard;\n"
        "}\n";

    // ==== scene skin shaders: begin ====
    // (tools/selftest/sim/gen_riggedtable.py copies this section into sim_riggedtable.)
    // research/219: the retained scene's rigged casters, the depth only and
    // the masks' vertex shaders above with each record skinned instead of
    // its transform (a rigged record's is the identity). The skinning is
    // skin_table's (getObjectSkinnedTransform over the frame's palette
    // table, set 0 binding 2: objectSkinV.glsl's formula and order) with
    // the palette row's first vec4 from the record's flags (scene_cull.comp's
    // Record word 14, bits 8 on: SCENE_PALETTE_SHIFT) instead of the
    // packet's params.w, and without the hair sway (a swaying palette keeps
    // no record, research/219 5.2). Every text replaced must be found once:
    // else nothing is made and the rigged records stay the packets'.
    inline bool scene_replace_once(std::string& text, const std::string& from, const std::string& to)
    {
        const size_t at = text.find(from);
        if (at == std::string::npos || text.find(from, at + 1) != std::string::npos)
        {
            return false;
        }
        text.replace(at, from.size(), to);
        return true;
    }

    inline bool scene_skin_sources(const std::string& skin_table, const std::string& shadow_vs, const std::string& mask_vs,
                                   std::string& skin_shadow_vs, std::string& skin_mask_vs)
    {
        std::string skin = skin_table;
        if (!scene_replace_once(skin, "floatBitsToUint(pc.params.w)", "scene_palette_base") ||
            !scene_replace_once(skin, "HAIR_SWAY", "") ||
            !scene_replace_once(skin, "    m[3].xyz += hair_sway((m * vec4(position, 1.0)).xyz);\n", ""))
        {
            return false;
        }
        const std::string position = "layout(location = 0) in vec3 position;\n";
        const std::string code = position + "uint scene_palette_base = 0u;\n" + skin +
                                 "mat4 scene_skinned(uint flags)\n"
                                 "{\n"
                                 "    scene_palette_base = flags >> 8u;\n"
                                 "    return getObjectSkinnedTransform();\n"
                                 "}\n";
        skin_shadow_vs = shadow_vs;
        skin_mask_vs = mask_vs;
        return scene_replace_once(skin_shadow_vs, position, code) &&
               scene_replace_once(skin_shadow_vs, "transforms.m[transform] * vec4(position, 1.0)",
                                  "scene_skinned(records.w[uint(gl_InstanceIndex) * 4u + 3u].z) * vec4(position, 1.0)") &&
               scene_replace_once(skin_mask_vs, position, code) &&
               scene_replace_once(skin_mask_vs, "llrhi_scene_transforms(pc.tables.zw).m[r1.w] * vec4(position, 1.0)",
                                  "scene_skinned(r3.z) * vec4(position, 1.0)");
    }
    // ==== scene skin shaders: end ====

    // Joint matrices of rigged casters: a dynamic uniform buffer in the frame's
    // upload ring, one descriptor set per frame in flight.
    constexpr U32 MAX_JOINTS = 110;                 // LL_MAX_JOINTS_PER_MESH_OBJECT
    constexpr U32 PALETTE_BYTES = MAX_JOINTS * 48;  // mat3x4 = 3 vec4 columns
    // LLVKShader shifts uniform block bindings by 64 (GL block bindings start at 0).
    constexpr U32 PALETTE_BINDING = 64;

    // Hair sway (newview/plus/hairsway/hairsway.h): the agent's hair palettes
    // end with two more entries (hairline plane, head centre, depths, offsets) whose first has this
    // mark in column 1's w (float 7 of the entry: LLMatrix4a row 3, y), never a
    // joint's translation. The skinned shaders move the hair vertices by them;
    // which entry is in params.z / 8 (params.z % 8 stays the mode).
    constexpr F32 HAIR_SWAY_MARK = -98765.f;
    constexpr U32 HAIR_SWAY_ENTRIES = 2;
    constexpr U32 HAIR_SWAY_MODE_BITS = 8;

    // Where the packet palette's sway entries start (its joint count), 0: none.
    U32 hair_sway_index(const F32* palette, U32 joints)
    {
        if (!palette || joints < 1 + HAIR_SWAY_ENTRIES || joints > MAX_JOINTS)
        {
            return 0;
        }
        const U32 index = joints - HAIR_SWAY_ENTRIES;
        return palette[index * 12 + 7] == HAIR_SWAY_MARK ? index : 0;
    }

    class PaletteBuffer
    {
    public:
        bool init(VkDevice device);
        VkDescriptorSetLayout layout() const { return mLayout; }
        // Copies the palette into this frame's ring (reused for the same palette
        // within a pass) and returns the set and dynamic offset to bind.
        bool place(const F32* palette, U32 joints, VkDescriptorSet& set, U32& offset);
        void newPass() { mLastPalette = nullptr; }

        U32 mCopies = 0;    // palettes copied into the ring (research/56 counters)
        U32 mReused = 0;    // the same palette as the draw before

    private:
        VkDescriptorSetLayout mLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[2] = {};
        VkBuffer mSetBuffer[2] = {};    // ring buffer each set points at
        const F32* mLastPalette = nullptr;
        U32 mLastOffset = 0;
        U32 mLastFrameSlot = U32_MAX;
    };
    // mSets and mSetBuffer: one per frame in flight, picked with getSlot() & 1.
    static_assert(LLVKMirror::FRAMES_IN_FLIGHT == 2, "PaletteBuffer keeps two sets, picked with getSlot() & 1");

    bool PaletteBuffer::init(VkDevice device)
    {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = PALETTE_BINDING;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.bindingCount = 1;
        layout_info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &mLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &pool_info, nullptr, &mPool) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorSetLayout layouts[2] = { mLayout, mLayout };
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = mPool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts;
        return vkAllocateDescriptorSets(device, &alloc, mSets) == VK_SUCCESS;
    }

    bool PaletteBuffer::place(const F32* palette, U32 joints, VkDescriptorSet& set, U32& offset)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        const U32 frame_slot = mirror.getSlot() & 1;
        if (palette == mLastPalette && frame_slot == mLastFrameSlot)
        {
            set = mSets[frame_slot];
            offset = mLastOffset;
            ++mReused;
            return true;
        }
        const VkDeviceSize align = LLVKDevice::instance().getLimits().minUniformBufferOffsetAlignment;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize ring_offset = 0;
        void* dst = nullptr;
        if (!mirror.allocate(PALETTE_BYTES, align, ring, ring_offset, dst))
        {
            return false;
        }
        memcpy(dst, palette, (size_t)llmin(joints, MAX_JOINTS) * 48);
        ++mCopies;
        if (mSetBuffer[frame_slot] != ring)
        {
            // The frame's ring is fixed for the slot; point the set at it once.
            VkDescriptorBufferInfo info = { ring, 0, PALETTE_BYTES };
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = mSets[frame_slot];
            write.dstBinding = PALETTE_BINDING;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &info;
            vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), 1, &write, 0, nullptr);
            mSetBuffer[frame_slot] = ring;
        }
        set = mSets[frame_slot];
        offset = (U32)ring_offset;
        mLastPalette = palette;
        mLastOffset = offset;
        mLastFrameSlot = frame_slot;
        return true;
    }

    // One native shadow pass's draw commands, with everything they need
    // resolved on the main thread (research/66 step 2): ShadowRenderer::record
    // reads only this, the packets' values and the model and texture matrices
    // they point at, so a cull thread can record it into a spliced stream.
    struct PassRecord
    {
        // Per packet.
        struct Resolved
        {
            VkBuffer vertex;
            VkBuffer index;
            U32 slots[4];
            bool pooled;
            // Skinned casters that copy their palette: placed by the main
            // thread (PaletteBuffer::place updates descriptor sets).
            bool palette_ok;
            VkDescriptorSet palette_set;
            U32 palette_offset;
        };
        std::vector<Resolved> resolved;
        const Packet* packets = nullptr;
        size_t count = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;           // the bindless set
        VkPipeline pipelines[2][2] = {};                // [skinned][alpha]
        VkPipeline table_pipelines[2] = {};             // skinned with a palette table row, [alpha]
        VkPipeline pooled_pipeline = VK_NULL_HANDLE;
        U32 frame = 0;                                  // LLVKTracker::frame()
        // The cull mode beginPass set (the GL state's): casters with
        // Packet::CULL_INHERIT draw with it, the others with their own, and
        // the record leaves it set again (research/137 N7).
        VkCullModeFlags cull = VK_CULL_MODE_NONE;
        F32 view_proj[16] = {};
        F32 target_width = 0.f;
        // Pooled casters: their commands in the ring, how many per pool buffer
        // and index size (in that order), the pool buffers.
        VkBuffer indirect = VK_NULL_HANDLE;
        VkDeviceSize indirect_offset = 0;
        U32 pooled = 0;
        U32 indirect_counts[LLVKGPUDriven::MAX_CHUNKS][2] = {};
        VkBuffer chunks[LLVKGPUDriven::MAX_CHUNKS] = {};
        U32 max_draws = 1;                              // maxDrawIndirectCount
        LLVKGPUList::Draws list_draws;
        // research/206: the retained scene's casters (LLVKScene::takeShadowDraws).
        LLVKScene::ShadowDraws scene;
        VkPipeline scene_pipeline = VK_NULL_HANDLE;
        VkPipeline scene_mask_pipeline = VK_NULL_HANDLE;    // research/206 5c: the buckets with a mask mode
        VkPipeline scene_skin_pipeline = VK_NULL_HANDLE;    // research/219: the rigged buckets, depth only
        VkPipeline scene_skin_mask_pipeline = VK_NULL_HANDLE;   // research/219: the rigged buckets with a mask mode
        // Draw checkpoints (LLRHI_VK_CHECKPOINTS, which keeps everything on the
        // main thread): null when recorded on a cull thread.
        const LLVKDevice* checkpoints = nullptr;
        U32 checkpoint_base = 0;
    };

    struct PassCounts
    {
        U32 draws = 0;
        U32 pooled_draws = 0;
        U32 indirect_calls = 0;
        U32 list_calls = 0;
        U32 palette_rows = 0;
        U32 scene_calls = 0;        // research/206
    };

    // A pass handed to a cull thread. IDLE (free; only the main thread
    // touches it) -> POSTED (its record is complete; release) -> RUNNING
    // (claimed by a cull thread, or by the main thread at the join) -> DONE
    // (its stream is complete, counts and ms written; release) -> IDLE (the
    // join added the counts). A claim is a compare-exchange from POSTED, so a
    // task left over in the cull threads' queue (the main thread took its job
    // at a join) never runs a job twice: it finds it IDLE, or POSTED again
    // for a later pass, which it may then record as well as any.
    enum { JOB_IDLE = 0, JOB_POSTED = 1, JOB_RUNNING = 2, JOB_DONE = 3 };
    constexpr U32 MAX_JOBS = 8;                 // a window's passes: 4 cascades x (cached + moving)
    constexpr U32 RECORD_BINS = 8;              // sun0..3, probe0..3 (LLRHI::sShadowRecordBin)
    struct RecordJob
    {
        PassRecord rec;
        PassCounts counts;
        LLVKRecorder::Stream* stream = nullptr;
        U32 bin = 0;
        F64 ms = 0.0;                           // recording (on whichever thread ran it)
        bool on_main = false;                   // the main thread took it at the join
        std::atomic<int> state{ JOB_IDLE };
    };

    class ShadowRenderer
    {
    public:
        bool ready();
        bool draw(const Packet* packets, size_t count, const F32* view_proj, F32 target_width, const LLRHI::ShadowListPass* list);
        bool pooledReady() const { return mPooledReady; }
        // The pass's commands, from the descriptor set bind to its last draw.
        // Any thread (reads only rec, the packets and what they point at,
        // and members fixed at init); counts into counts.
        void record(const PassRecord& rec, PassCounts& counts) const;
        // A claimed job: records it into its stream, then marks it DONE.
        void fill(RecordJob& job);
        // Main thread: every posted job recorded, its counts added.
        void join();

        U32 mDraws = 0;
        U32 mPooledDraws = 0;       // opaque casters drawn from the geometry pool (research/46 G1)
        U32 mIndirectCalls = 0;
        U32 mListCalls = 0;         // indirect count draws of the GPU shadow list (research/46 G2)
        U32 mSceneCalls = 0;        // indirect count draws of the retained scene's records (research/206)
        U32 mPaletteRows = 0;       // skinned casters that read their palette's table row (research/56)
        U64 mNanoseconds = 0;       // the main thread's time in draw()
        U32 mSplicedPasses = 0;     // passes recorded through a spliced stream (LLRHI::sSpliceTest)
        PaletteBuffer mPalettes;

        // Research/66 step 2 counters (main thread; jobs' added at the join).
        U32 mThreadPasses[RECORD_BINS] = {};    // recorded on a cull thread, per bin
        F64 mThreadMs[RECORD_BINS] = {};        // their recording time
        U32 mMainPasses = 0;                    // posted, taken by the main thread at the join
        F64 mMainMs = 0.0;
        F64 mJoinWaitMs = 0.0;                  // the join waiting for cull threads
        U32 mInlinePasses = 0;                  // could have gone to a thread, recorded inline (no stream or job)

    private:
        RecordJob mJobs[MAX_JOBS];
        U32 mJobsUsed = 0;                      // mJobs[0, mJobsUsed) posted since the last join
        std::mutex mJobMutex;
        std::condition_variable mJobDone;
        PassRecord mInline;                     // a pass recorded on the main thread
        std::vector<U32> mSkinned;              // per pass scratch: packets with a palette

        U32 mSpliceTestFrame = U32_MAX;     // the tracker frame mSpliceTestCount counts
        U32 mSpliceTestCount = 0;           // passes spliced in it

        bool init();
        // pooled: shadowV over the geometry pool (alpha and skinned false).
        // table (skinned): the palette from the frame's palette table (research/56).
        // scene: shadowV over the retained scene's records (research/206; 5c: alpha, a mask's;
        // research/219: skinned, the rigged records' with their palette rows).
        VkPipeline pipeline(bool alpha, bool skinned, const LLVKMirror::Target& target, bool pooled = false, bool table = false,
                            bool scene = false);

        bool mTried = false;
        bool mReady = false;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        LLVKShader::Program mOpaqueVS, mAlphaVS, mAlphaFS, mSkinnedOpaqueVS, mSkinnedAlphaVS, mPooledVS;
        bool mPooledReady = false;
        // Skinned casters whose packet names its palette's row in the frame's
        // palette table (LLRHI::sPalettePrebuild, research/56): the row's first
        // vec4 goes in the push constants' params.w (no fragment shader reads it).
        LLVKShader::Program mSkinnedOpaqueTableVS, mSkinnedAlphaTableVS;
        bool mTableReady = false;
        LLVKShader::Program mSceneVS;           // research/206
        bool mSceneReady = false;
        LLVKShader::Program mSceneMaskVS, mSceneMaskFS;     // research/206 5c
        bool mSceneMasksReady = false;
        LLVKShader::Program mSceneSkinVS, mSceneSkinMaskVS;     // research/219 (the masks' fragment shader is mSceneMaskFS)
        bool mSceneSkinsReady = false;          // both, or the depth only one while the masks' are not made
        LLVKScene::ShadowDraws mSceneTaken;     // per pass scratch: what the pass took (swapped into its record)
        VkBuffer mDefaults = VK_NULL_HANDLE;        // constant inputs for missing vertex arrays
        VmaAllocation mDefaultsMemory = nullptr;
        std::unordered_map<U64, VkPipeline> mPipelines;
        // Per pass scratch.
        std::vector<LLVKTracker::GLName> mSampled;
        // Pooled casters' draw commands by pool buffer and index size (16, 32 bits),
        // and the matrix table row of each model matrix in the pass.
        std::vector<VkDrawIndexedIndirectCommand> mIndirect[LLVKGPUDriven::MAX_CHUNKS][2];
        LLVKFrameTable mModelRows;      // the matrix's address -> row; emptied each pass without freeing (fix-pipeleak)
    };

    ShadowRenderer sShadows;

    bool ShadowRenderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = init();
            if (!mReady)
            {
                LL_WARNS("VKNative") << "native shadow renderer unavailable; shadows stay on the GL path" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool ShadowRenderer::init()
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        if (!LLVKBindless::init() || !mPalettes.init(device))
        {
            return false;
        }
        // Set 0 and the push constant range match every other bindless layout (llvkbindless.h).
        VkDescriptorSetLayout set_layouts[2] = { LLVKBindless::layout(), mPalettes.layout() };
        VkPushConstantRange range = LLVKBindless::pushConstantRange();
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 2;
        layout_info.pSetLayouts = set_layouts;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &mLayout) != VK_SUCCESS)
        {
            return false;
        }

        // shadowV.glsl
        std::string opaque_vs = std::string("#version 450\n") + PUSH_BLOCK +
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = pc.mvp * vec4(position, 1.0); }\n";
        // shadowAlphaMaskV.glsl / treeShadowV.glsl
        std::string alpha_vs = std::string("#version 450\n") + PUSH_BLOCK +
            "layout(location = 0) in vec3 position;\n"
            "layout(location = 1) in vec2 texcoord0;\n"
            "layout(location = 2) in vec4 diffuse_color;\n"
            "layout(location = 3) in uint texture_index;\n"
            "layout(location = 0) out vec2 vary_texcoord0;\n"
            "layout(location = 1) out float vertex_alpha;\n"
            "layout(location = 2) flat out uint texture_slot;\n"
            "layout(location = 3) out float target_pos_x;\n"
            "layout(location = 4) out float post_pos_w;\n"
            "void main()\n"
            "{\n"
            "    vec4 pos = pc.mvp * vec4(position, 1.0);\n"
            "    gl_Position = pos;\n"
            "    vec4 tc = vec4(texcoord0, 0.0, 1.0);\n"
            "    vary_texcoord0 = vec2(dot(pc.tex_row0, tc), dot(pc.tex_row1, tc));\n"
            "    vertex_alpha = diffuse_color.a;\n"
            "    texture_slot = pc.textures[min(texture_index, 3u)];\n"
            "    target_pos_x = 0.5 * (pc.params.y - 1.0) * pos.x;\n"
            "    post_pos_w = pos.w;\n"
            "}\n";
        // objectSkinV.glsl getObjectSkinnedTransform (shadowSkinnedV / shadowAlphaMaskV with HAS_SKIN)
        // Hair sway (hair_sway_index), after the variant's palette_at(i).
        static const char* hair_sway =
            "vec3 hair_sway(vec3 p)\n"
            "{\n"
            "    int s = int(pc.params.z) >> 3;\n"
            "    if (s < 1 || s > 108) return vec3(0.0);\n"
            "    mat3x4 a = palette_at(s), b = palette_at(s + 1);\n"
            "    vec4 shape = b[1];\n"
            "    if (a[1].w != -98765.0 || !(shape.x < shape.y && shape.y < shape.z)) return vec3(0.0);\n"
            "    float d = a[0].w - dot(a[0].xyz, p);\n"
            "    vec3 o = a[2].xyz * smoothstep(shape.x, shape.y, d) + (b[0].xyz - a[2].xyz) * smoothstep(shape.y, shape.z, d);\n"
            "    vec3 outward = p - a[1].xyz;\n"
            "    float len = length(outward);\n"
            "    if (len > 0.0001) { outward /= len; o -= outward * (min(dot(o, outward), 0.0) * (1.0 - shape.w)); }\n"
            "    return o;\n"
            "}\n";
        static const char* skin =
            "layout(set = 1, binding = 0) uniform Palette { mat3x4 matrixPalette[110]; };\n"
            "layout(location = 4) in vec4 weight4;\n"
            "mat3x4 palette_at(int i) { return matrixPalette[i]; }\n"
            "HAIR_SWAY"
            "mat4 getObjectSkinnedTransform()\n"
            "{\n"
            "    vec4 w = fract(weight4);\n"
            "    vec4 index = clamp(floor(weight4), vec4(0.0), vec4(109.0));\n"
            "    index = mix(index, vec4(0.0), isnan(index));  // a NaN weight (a broken mesh; clamp may pass it on): joint 0\n"
            "    w *= 1.0 / (w.x + w.y + w.z + w.w);\n"
            "    int i1 = int(index.x), i2 = int(index.y), i3 = int(index.z), i4 = int(index.w);\n"
            "    mat3 mat = mat3(matrixPalette[i1]) * w.x + mat3(matrixPalette[i2]) * w.y + mat3(matrixPalette[i3]) * w.z + mat3(matrixPalette[i4]) * w.w;\n"
            "    vec3 trans = vec3(matrixPalette[i1][0].w, matrixPalette[i1][1].w, matrixPalette[i1][2].w) * w.x\n"
            "               + vec3(matrixPalette[i2][0].w, matrixPalette[i2][1].w, matrixPalette[i2][2].w) * w.y\n"
            "               + vec3(matrixPalette[i3][0].w, matrixPalette[i3][1].w, matrixPalette[i3][2].w) * w.z\n"
            "               + vec3(matrixPalette[i4][0].w, matrixPalette[i4][1].w, matrixPalette[i4][2].w) * w.w;\n"
            "    mat4 m = mat4(vec4(mat[0], 0.0), vec4(mat[1], 0.0), vec4(mat[2], 0.0), vec4(trans, 1.0));\n"
            "    m[3].xyz += hair_sway((m * vec4(position, 1.0)).xyz);\n"
            "    return m;\n"
            "}\n";
        auto skinned = [&](const std::string& source)
        {
            // Insert the skinning code after the position input (the hair sway
            // reads it) and transform through it.
            std::string out = source;
            const std::string position = "layout(location = 0) in vec3 position;\n";
            size_t at = out.find(position) + position.size();
            std::string code = skin;
            code.replace(code.find("HAIR_SWAY"), strlen("HAIR_SWAY"), hair_sway);
            out.insert(at, code);
            size_t mvp = out.find("pc.mvp * vec4(position, 1.0)");
            out.replace(mvp, std::string("pc.mvp * vec4(position, 1.0)").size(), "pc.mvp * (getObjectSkinnedTransform() * vec4(position, 1.0))");
            return out;
        };
        const std::string skinned_opaque_vs = skinned(opaque_vs);
        // The same skinning with the palette read from the frame's palette table
        // (set 0 binding 2, LLVKBindless::paletteRows) from the vec4 whose index
        // is in params.w: the same values in the same order as the uniform block.
        static const char* skin_table =
            "layout(std430, set = 0, binding = 2) readonly buffer llrhi_palette_table { vec4 v[]; } llrhi_pal;\n"
            "layout(location = 4) in vec4 weight4;\n"
            "mat3x4 palette_at(int i)\n"
            "{\n"
            "    uint b = floatBitsToUint(pc.params.w) + uint(i) * 3u;\n"
            "    return mat3x4(llrhi_pal.v[b], llrhi_pal.v[b + 1u], llrhi_pal.v[b + 2u]);\n"
            "}\n"
            "HAIR_SWAY"
            "mat4 getObjectSkinnedTransform()\n"
            "{\n"
            "    vec4 w = fract(weight4);\n"
            "    vec4 index = clamp(floor(weight4), vec4(0.0), vec4(109.0));\n"
            "    index = mix(index, vec4(0.0), isnan(index));  // a NaN weight (a broken mesh; clamp may pass it on): joint 0\n"
            "    w *= 1.0 / (w.x + w.y + w.z + w.w);\n"
            "    mat3x4 m1 = palette_at(int(index.x)), m2 = palette_at(int(index.y)), m3 = palette_at(int(index.z)), m4 = palette_at(int(index.w));\n"
            "    mat3 mat = mat3(m1) * w.x + mat3(m2) * w.y + mat3(m3) * w.z + mat3(m4) * w.w;\n"
            "    vec3 trans = vec3(m1[0].w, m1[1].w, m1[2].w) * w.x\n"
            "               + vec3(m2[0].w, m2[1].w, m2[2].w) * w.y\n"
            "               + vec3(m3[0].w, m3[1].w, m3[2].w) * w.z\n"
            "               + vec3(m4[0].w, m4[1].w, m4[2].w) * w.w;\n"
            "    mat4 m = mat4(vec4(mat[0], 0.0), vec4(mat[1], 0.0), vec4(mat[2], 0.0), vec4(trans, 1.0));\n"
            "    m[3].xyz += hair_sway((m * vec4(position, 1.0)).xyz);\n"
            "    return m;\n"
            "}\n";
        auto skinned_table = [&](const std::string& source)
        {
            std::string out = source;
            const std::string position = "layout(location = 0) in vec3 position;\n";
            size_t at = out.find(position) + position.size();
            std::string code = skin_table;
            code.replace(code.find("HAIR_SWAY"), strlen("HAIR_SWAY"), hair_sway);
            out.insert(at, code);
            size_t mvp = out.find("pc.mvp * vec4(position, 1.0)");
            out.replace(mvp, std::string("pc.mvp * vec4(position, 1.0)").size(), "pc.mvp * (getObjectSkinnedTransform() * vec4(position, 1.0))");
            return out;
        };
        // shadowV for casters drawn from the geometry pool (research/46 G1): the
        // MVP comes from the frame's draw matrix table (set 0 binding 1, the rows
        // of LLVKBindless::DrawMatrices), its row is the draw's first instance.
        const std::string pooled_vs = std::string("#version 450\n") +
            "struct llrhi_draw_matrices { mat4 mv; mat4 mvp; mat4 tex0; mat3 nrm; };\n"
            "layout(std430, set = 0, binding = 1) readonly buffer llrhi_matrix_table { llrhi_draw_matrices m[]; } llrhi_mt;\n"
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = llrhi_mt.m[gl_InstanceIndex].mvp * vec4(position, 1.0); }\n";
        const std::string skinned_alpha_vs = skinned(alpha_vs);
        // shadowAlphaMaskF.glsl (mode 1, mode 2 = IS_FULLBRIGHT) and treeShadowF.glsl (mode 3)
        std::string alpha_fs = std::string("#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n") + PUSH_BLOCK +
            "layout(set = 0, binding = 0) uniform sampler2D textures[];\n"
            "layout(location = 0) in vec2 vary_texcoord0;\n"
            "layout(location = 1) in float vertex_alpha;\n"
            "layout(location = 2) flat in uint texture_slot;\n"
            "layout(location = 3) in float target_pos_x;\n"
            "layout(location = 4) in float post_pos_w;\n"
            "void main()\n"
            "{\n"
            "    float alpha = texture(textures[nonuniformEXT(texture_slot)], vary_texcoord0).a;\n"
            "    if (alpha < pc.params.x) discard;\n"
            "    int mode = int(pc.params.z) & 7;\n"     // above the mode bits: the hair sway entry (hair_sway_index)
            "    if (mode == 3) return;\n"
            "    if (mode == 1) alpha *= vertex_alpha;\n"
            "    if (alpha < 0.05) discard;\n"
            "    if (alpha < 0.88 && fract(0.5 * floor(target_pos_x / post_pos_w)) < 0.25) discard;\n"
            "}\n";
        if (!LLVKShader::build({ { LLVKShader::Stage::Vertex, opaque_vs } }, mOpaqueVS) ||
            !LLVKShader::build({ { LLVKShader::Stage::Vertex, alpha_vs } }, mAlphaVS) ||
            !LLVKShader::build({ { LLVKShader::Stage::Fragment, alpha_fs } }, mAlphaFS) ||
            !LLVKShader::build({ { LLVKShader::Stage::Vertex, skinned_opaque_vs } }, mSkinnedOpaqueVS) ||
            !LLVKShader::build({ { LLVKShader::Stage::Vertex, skinned_alpha_vs } }, mSkinnedAlphaVS))
        {
            LL_WARNS("VKNative") << "shadow shaders failed: " << mOpaqueVS.log << mAlphaVS.log << mAlphaFS.log
                                 << mSkinnedOpaqueVS.log << mSkinnedAlphaVS.log << LL_ENDL;
            return false;
        }
        // Optional: without them skinned casters copy their palette, as before.
        mTableReady = LLVKShader::build({ { LLVKShader::Stage::Vertex, skinned_table(opaque_vs) } }, mSkinnedOpaqueTableVS) &&
                      LLVKShader::build({ { LLVKShader::Stage::Vertex, skinned_table(alpha_vs) } }, mSkinnedAlphaTableVS);
        if (!mTableReady)
        {
            LL_WARNS("VKNative") << "palette table shadow shaders failed: " << mSkinnedOpaqueTableVS.log << mSkinnedAlphaTableVS.log << LL_ENDL;
        }
        // Optional (research/206): without it the scene's shadow draws are not drawn (the viewer draws its casters).
        mSceneReady = LLVKShader::build({ { LLVKShader::Stage::Vertex, SCENE_SHADOW_VS } }, mSceneVS);
        if (!mSceneReady)
        {
            LL_WARNS("VKNative") << "retained scene shadow shader failed: " << mSceneVS.log << LL_ENDL;
        }
        // Optional (research/206 5c): without them the viewer leaves the masks' records to its packets.
        if (mSceneReady)
        {
            const F32 kind = LLRHITexAnim::KIND_SCROLL;
            U32 kind_bits = 0;
            memcpy(&kind_bits, &kind, sizeof(kind_bits));
            char kind_line[64];
            snprintf(kind_line, sizeof(kind_line), "const uint SCENE_KIND_BITS = 0x%08xu;\n", kind_bits);
            const std::string mask_vs = std::string("#version 450\n#extension GL_EXT_buffer_reference : require\n"
                                                    "#extension GL_EXT_buffer_reference_uvec2 : require\n") +
                                        SCENE_PUSH_BLOCK + kind_line + SCENE_MASK_VS_BODY;
            const std::string mask_fs = std::string("#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n") + SCENE_PUSH_BLOCK +
                                        SCENE_MASK_FS_BODY;
            mSceneMasksReady = LLVKShader::build({ { LLVKShader::Stage::Vertex, mask_vs } }, mSceneMaskVS) &&
                               LLVKShader::build({ { LLVKShader::Stage::Fragment, mask_fs } }, mSceneMaskFS);
            if (!mSceneMasksReady)
            {
                LL_WARNS("VKNative") << "retained scene mask shadow shaders failed: " << mSceneMaskVS.log << mSceneMaskFS.log << LL_ENDL;
            }
            // Optional (research/219): without them the viewer leaves the rigged records to its packets.
            // With the masks' shaders made, the rigged masks' must be made too (the viewer asks for both).
            std::string skin_vs, skin_mask_vs;
            if (scene_skin_sources(skin_table, SCENE_SHADOW_VS, mask_vs, skin_vs, skin_mask_vs))
            {
                mSceneSkinsReady = LLVKShader::build({ { LLVKShader::Stage::Vertex, skin_vs } }, mSceneSkinVS) &&
                                   (!mSceneMasksReady ||
                                    LLVKShader::build({ { LLVKShader::Stage::Vertex, skin_mask_vs } }, mSceneSkinMaskVS));
            }
            if (!mSceneSkinsReady)
            {
                LL_WARNS("VKNative") << "retained scene skinned shadow shaders failed (research/219): " << mSceneSkinVS.log
                                     << mSceneSkinMaskVS.log << LL_ENDL;
            }
        }
        LLVKScene::setShadowMasks(mSceneMasksReady);
        LLVKScene::setShadowSkins(mSceneSkinsReady);
        // Optional: without it every caster is drawn on its own, as before.
        mPooledReady = LLVKShader::build({ { LLVKShader::Stage::Vertex, pooled_vs } }, mPooledVS);
        if (!mPooledReady)
        {
            LL_WARNS("VKNative") << "pooled shadow shader failed: " << mPooledVS.log << LL_ENDL;
        }

        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = 64;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo mem = {};
        mem.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        mem.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped = {};
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &mem, &mDefaults, &mDefaultsMemory, &mapped) != VK_SUCCESS)
        {
            return false;
        }
        U8* data = (U8*)mapped.pMappedData;
        memset(data, 0, 64);            // texcoord (0, 0) at 0
        memset(data + 16, 0xff, 4);     // opaque white at 16
        return true;
    }

    VkPipeline ShadowRenderer::pipeline(bool alpha, bool skinned, const LLVKMirror::Target& target, bool pooled, bool table, bool scene)
    {
        const LLVKTracker::PipelineState state = LLVKTracker::pipelineState();
        U64 key = LLRHI::FNV1A_BASIS_SHORT;
        auto mix = [&key](U64 v) { key = LLRHI::fnv1aMix(key, v); };
        mix(alpha);
        mix(skinned);
        mix(pooled);
        mix(table);
        mix(scene);
        mix(state.depth_clamp);
        mix(state.polygon_mode);
        mix(target.color_count);
        mix(target.depth_format);
        mix(target.stencil);
        for (U32 i = 0; i < target.color_count; ++i)
        {
            mix(target.color_formats[i]);
        }
        auto it = mPipelines.find(key);
        if (it != mPipelines.end())
        {
            return it->second;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = scene && skinned ? (alpha ? mSceneSkinMaskVS.modules[0] : mSceneSkinVS.modules[0])    // research/219
                         : scene  ? (alpha ? mSceneMaskVS.modules[0] : mSceneVS.modules[0])     // research/206 5c: a mask's
                         : pooled ? mPooledVS.modules[0]
                         : skinned && table ? (alpha ? mSkinnedAlphaTableVS.modules[0] : mSkinnedOpaqueTableVS.modules[0])
                         : skinned ? (alpha ? mSkinnedAlphaVS.modules[0] : mSkinnedOpaqueVS.modules[0])
                                   : (alpha ? mAlphaVS.modules[0] : mOpaqueVS.modules[0]);
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = scene && alpha ? mSceneMaskFS.modules[0] : mAlphaFS.modules[0];
        stages[1].pName = "main";

        // Binding 0: positions (16 byte stride, texture index in w), 1: texcoord0, 2: color.
        // Strides are dynamic (set with the buffers); these are the usual ones.
        // Binding 3 (skinned): weight4 (joint index + weight per component).
        VkVertexInputBindingDescription bindings[4] = {
            { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 8, VK_VERTEX_INPUT_RATE_VERTEX }, { 2, 4, VK_VERTEX_INPUT_RATE_VERTEX },
            { 3, 16, VK_VERTEX_INPUT_RATE_VERTEX } };
        VkVertexInputAttributeDescription attributes[5] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 1, VK_FORMAT_R32G32_SFLOAT, 0 },
            { 2, 2, VK_FORMAT_R8G8B8A8_UNORM, 0 }, { 3, 0, VK_FORMAT_R32_UINT, 12 },
            { 4, 3, VK_FORMAT_R32G32B32A32_SFLOAT, 0 } };
        U32 binding_count = alpha ? 3 : 1;
        U32 attribute_count = alpha ? (scene ? 3 : 4) : 1;     // research/206 5c: a record's mask reads no texture index
        if (skinned)
        {
            // The weight binding follows the ones in use.
            bindings[binding_count] = { binding_count, 16, VK_VERTEX_INPUT_RATE_VERTEX };
            attributes[attribute_count] = { 4, binding_count, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
            ++binding_count;
            ++attribute_count;
        }
        VkPipelineVertexInputStateCreateInfo vertex = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex.vertexBindingDescriptionCount = binding_count;
        vertex.pVertexBindingDescriptions = bindings;
        vertex.vertexAttributeDescriptionCount = attribute_count;
        vertex.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportDepthClipControlCreateInfoEXT clip_control = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
        clip_control.negativeOneToOne = VK_TRUE;    // GL clip-space depth (required, research/124 V1)
        VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.pNext = &clip_control;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = LLVKDevice::instance().usablePolygonMode(
            state.polygon_mode == GL_LINE ? VK_POLYGON_MODE_LINE : state.polygon_mode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
        raster.lineWidth = 1.f;
        raster.depthClampEnable = state.depth_clamp;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState blends[4] = {};     // shadow passes write depth only
        VkPipelineColorBlendStateCreateInfo color = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        color.attachmentCount = target.color_count;
        color.pAttachments = blends;
        // The same dynamic state as the translated pipelines (LLVKTracker::beginPass sets it).
        const VkDynamicState dynamic_states[] =
        {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            // Missing vertex arrays read one constant element (stride 0).
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = (U32)(sizeof(dynamic_states) / sizeof(dynamic_states[0]));
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = target.color_count;
        rendering.pColorAttachmentFormats = target.color_formats;
        rendering.depthAttachmentFormat = target.depth_format;
        rendering.stencilAttachmentFormat = target.stencil ? target.depth_format : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = alpha ? 2 : 1;
        info.pStages = stages;
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &color;
        info.pDynamicState = &dynamic;
        info.layout = mLayout;
        VkPipeline result = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(LLVKDevice::instance().getDevice(), LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &result) != VK_SUCCESS)
        {
            result = VK_NULL_HANDLE;
        }
        mPipelines[key] = result;
        return result;
    }

    // out = a * b, column-major 4x4.
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

    // A cull thread's task (LLRHI::sPostWork): the job, unless someone claimed it first.
    void run_record_job(void* arg);

    bool ShadowRenderer::draw(const Packet* packets, size_t count, const F32* view_proj, F32 target_width,
                              const LLRHI::ShadowListPass* list)
    {
        // research/206: the retained scene's casters made ready for this pass
        // (LLRHI::Scene::drawShadow) are taken here, drawn or not: a later
        // pass never draws them. Not drawn (any return false below, or the
        // pass recorded without them): the viewer draws what it cut.
        if (LLRHI::sScene)
        {
            LLVKScene::takeShadowDraws(mSceneTaken);
        }
        struct SceneTaken
        {
            LLVKScene::ShadowDraws& taken;
            ~SceneTaken()
            {
                if (taken.any)  // still here: the pass did not keep them (its record takes them)
                {
                    LLVKScene::shadowDrawn(false, 0);
                    taken.any = false;
                    taken.buckets.clear();
                }
            }
        } scene_taken{ mSceneTaken };
        if (!ready())
        {
            return false;
        }
        if (mJobsUsed && LLRHI::sShadowRecordBin < 0)
        {
            join();     // the caller's window closed without a join: nothing may still read its packets
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            return false;
        }
        const auto start = std::chrono::steady_clock::now();

        // Research/66 step 2: the draw commands may be recorded by a cull
        // thread when the caller keeps the packets until its join, a job is
        // free and nothing has to stay on this thread (draw checkpoints are
        // recorded directly; the recorder is off with them anyway).
        LLVKDevice& device = LLVKDevice::instance();
        const bool threads = LLRHI::sShadowRecordThreads && LLRHI::sShadowRecordBin >= 0 && LLRHI::sPostWork &&
                             LLRHI::sJoinShadowRecording && LLRHI::sCullThreads && LLRHI::sRecordSplice &&
                             !device.checkpointsEnabled();
        RecordJob* job = nullptr;
        if (threads)
        {
            if (mJobsUsed < MAX_JOBS)
            {
                job = &mJobs[mJobsUsed];    // IDLE: only this thread writes it until it is posted
            }
            else
            {
                ++mInlinePasses;
            }
        }
        PassRecord& rec = job ? job->rec : mInline;

        // Buffers and texture slots first: uploads and layout changes cannot
        // happen inside the rendering scope.
        std::vector<PassRecord::Resolved>& resolved = rec.resolved;
        resolved.resize(count);
        mSampled.clear();
        mSkinned.clear();
        // Textures repeat from caster to caster: a texture resolves once per pass.
        // slotFor again for the same name, with no table slot moved since (nothing
        // else here changes textures), gives the same slot and changes nothing,
        // and the name is in mSampled already.
        struct SlotEntry
        {
            LLVKTracker::GLName name = 0;
            U32 slot = 0;
            U32 moves = 0;
            bool valid = false;
        };
        constexpr U32 SLOT_ENTRIES = 64;
        SlotEntry slot_cache[SLOT_ENTRIES];
        auto slot_for = [&](LLVKTracker::GLName name)
        {
            SlotEntry& e = slot_cache[name & (SLOT_ENTRIES - 1)];
            if (e.valid && e.name == name && e.moves == LLVKBindless::moves())
            {
                return e.slot;
            }
            const U32 slot = LLVKBindless::slotFor(name);
            if (name)
            {
                mSampled.push_back(name);
            }
            e.name = name;
            e.slot = slot;
            e.moves = LLVKBindless::moves();
            e.valid = true;
            return slot;
        };
        // GPU-driven (research/46 G1): opaque casters whose buffers are in the
        // geometry pool become indirect draw commands, all drawn first in a few
        // calls. Depth only, with the same test: the order does not change the result.
        const bool pool = LLRHI::sGPUDriven && mPooledReady && LLVKGPUDriven::supported();
        U32 pooled = 0;
        if (pool)
        {
            for (auto& lists : mIndirect)
            {
                lists[0].clear();
                lists[1].clear();
            }
            mModelRows.clear();
        }
        const F32* last_model = nullptr;
        U32 last_row = U32_MAX;
        // The draw matrix table row holding view_proj * model; U32_MAX when the table is full.
        auto model_row = [&](const F32* model) -> U32
        {
            if (last_row != U32_MAX && model == last_model)
            {
                return last_row;
            }
            if (const U32* found = mModelRows.find((U64)(uintptr_t)model))
            {
                last_model = model;
                last_row = *found;
                return last_row;
            }
            U32 row = 0;
            LLVKBindless::DrawMatrices* m = LLVKBindless::matrixRow(row);
            if (row >= LLVKBindless::MATRIX_ROWS - 1)
            {
                return U32_MAX;     // full: the last row is shared by later draws, so it is not written here
            }
            if (model)
            {
                mul4(view_proj, model, m->modelview_projection);
            }
            else
            {
                memcpy(m->modelview_projection, view_proj, sizeof(m->modelview_projection));
            }
            mModelRows.set((U64)(uintptr_t)model, row);
            last_model = model;
            last_row = row;
            return row;
        };
        for (size_t i = 0; i < count; ++i)
        {
            const Packet& p = packets[i];
            PassRecord::Resolved& r = resolved[i];
            r.pooled = false;
            r.palette_ok = false;
            LLVKGPUDriven::Placement placed;
            if (pool && p.mode == Packet::MODE_OPAQUE && !p.palette && p.index_count && p.cull == Packet::CULL_INHERIT &&
                LLVKGPUDriven::place(p.vertex_buffer, p.index_buffer, p.index_type, p.position_offset, p.vertex_count, p.index_total, placed) &&
                (U64)p.first_index + p.index_count <= placed.index_count)
            {
                const U32 row = model_row(p.model);
                if (row != U32_MAX)
                {
                    VkDrawIndexedIndirectCommand command;
                    command.indexCount = p.index_count;
                    command.instanceCount = 1;
                    command.firstIndex = placed.first_index + p.first_index;
                    command.vertexOffset = placed.vertex_offset;
                    command.firstInstance = row;
                    mIndirect[placed.chunk][placed.index32 ? 1 : 0].push_back(command);
                    r.pooled = true;
                    r.vertex = VK_NULL_HANDLE;
                    r.index = VK_NULL_HANDLE;
                    ++pooled;
                    continue;
                }
            }
            r.vertex = LLVKTracker::drawBuffer(p.vertex_buffer);
            r.index = LLVKTracker::drawBuffer(p.index_buffer);
            if (p.palette)
            {
                mSkinned.push_back((U32)i);     // its palette is placed below, in this order
            }
            if (p.mode == Packet::MODE_OPAQUE)
            {
                continue;
            }
            for (int t = 0; t < 4; ++t)
            {
                r.slots[t] = t == 0 || p.textures[t] ? slot_for(p.textures[t]) : r.slots[0];
            }
        }
        // The commands go into the frame's upload ring; without room there the
        // pooled casters are drawn one by one like the rest.
        VkBuffer indirect = VK_NULL_HANDLE;
        VkDeviceSize indirect_offset = 0;
        if (pooled)
        {
            void* dst = nullptr;
            if (mirror.allocate((VkDeviceSize)pooled * sizeof(VkDrawIndexedIndirectCommand), 4, indirect, indirect_offset, dst))
            {
                U8* out = (U8*)dst;
                for (auto& lists : mIndirect)
                {
                    for (const auto& list : lists)
                    {
                        if (!list.empty())
                        {
                            memcpy(out, list.data(), list.size() * sizeof(VkDrawIndexedIndirectCommand));
                            out += list.size() * sizeof(VkDrawIndexedIndirectCommand);
                        }
                    }
                }
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    PassRecord::Resolved& r = resolved[i];
                    if (r.pooled)
                    {
                        r.pooled = false;
                        r.vertex = LLVKTracker::drawBuffer(packets[i].vertex_buffer);
                        r.index = LLVKTracker::drawBuffer(packets[i].index_buffer);
                    }
                }
                pooled = 0;
            }
        }
        // GPU shadow list (research/46 G2, G3): the pass's compute dispatch goes
        // into the frame's upload commands, its draws into the pass below. If it
        // fails the list stops (logged); the rows the CPU left to it are missing
        // from this one pass.
        LLVKGPUList::Draws list_draws;
        if (list && mPooledReady && !LLVKGPUList::prepare(*list, view_proj, list_draws))
        {
            list_draws.any = false;
        }
        // research/206 5c: the scene's masks sample their textures in this pass too (readied with the packets').
        if (mSceneTaken.any && mSceneTaken.masks)
        {
            mSampled.insert(mSampled.end(), mSceneTaken.sampled.begin(), mSceneTaken.sampled.end());
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }
        LLVKBindless::flush();
        VkPipeline pipelines[2][2] = { { pipeline(false, false, target), pipeline(true, false, target) },
                                       { pipeline(false, true, target), pipeline(true, true, target) } };
        if (!pipelines[0][0] || !pipelines[0][1] || !pipelines[1][0] || !pipelines[1][1])
        {
            return false;
        }
        // Skinned casters with a palette table row (research/56); none: they copy their palette.
        VkPipeline table_pipelines[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        if (mTableReady)
        {
            table_pipelines[0] = pipeline(false, true, target, false, true);
            table_pipelines[1] = pipeline(true, true, target, false, true);
        }
        const U32 frame = LLVKTracker::frame();
        VkPipeline pooled_pipeline = VK_NULL_HANDLE;
        if (pooled || list_draws.any)
        {
            pooled_pipeline = pipeline(false, false, target, true);
            if (!pooled_pipeline)
            {
                return false;
            }
        }
        // research/206: the retained scene's casters, with their own vertex shader (5c: the masks' with their own shaders).
        VkPipeline scene_pipeline = VK_NULL_HANDLE;
        VkPipeline scene_mask_pipeline = VK_NULL_HANDLE;
        VkPipeline scene_skin_pipeline = VK_NULL_HANDLE;
        VkPipeline scene_skin_mask_pipeline = VK_NULL_HANDLE;
        if (mSceneTaken.any)
        {
            scene_pipeline = mSceneReady ? pipeline(false, false, target, false, false, true) : VK_NULL_HANDLE;
            if (!scene_pipeline)
            {
                return false;   // the viewer draws the pass's casters itself (and cuts no more)
            }
            if (mSceneTaken.masks)
            {
                scene_mask_pipeline = mSceneMasksReady ? pipeline(true, false, target, false, false, true) : VK_NULL_HANDLE;
                if (!scene_mask_pipeline)
                {
                    return false;
                }
            }
            // research/219: the rigged buckets' (their weights at the binding after the others).
            if (mSceneTaken.skins)
            {
                scene_skin_pipeline = mSceneSkinsReady ? pipeline(false, true, target, false, false, true) : VK_NULL_HANDLE;
                if (!scene_skin_pipeline)
                {
                    return false;
                }
            }
            if (mSceneTaken.skin_masks)
            {
                scene_skin_mask_pipeline =
                    mSceneSkinsReady && mSceneMasksReady ? pipeline(true, true, target, false, false, true) : VK_NULL_HANDLE;
                if (!scene_skin_mask_pipeline)
                {
                    return false;
                }
            }
        }

        // Skinned casters that copy their palette: placed here, on this thread
        // (ring memory and the palette set's descriptor update), in the order
        // and for the casters the draw loop would place them, so the ring
        // offsets and the commands are the ones it would record.
        mPalettes.newPass();
        for (U32 i : mSkinned)
        {
            const Packet& p = packets[i];
            PassRecord::Resolved& r = resolved[i];
            if (r.pooled || !r.vertex || !r.index || !p.index_count)
            {
                // LLRHI::sShadowSkinned (research/107 section 27): not drawn this pass.
                LLRHI::sShadowSkinned.unresolved += (!r.pooled && p.index_count) ? 1 : 0;
                continue;
            }
            const bool is_alpha = p.mode != Packet::MODE_OPAQUE;
            const bool is_skinned = p.palette && p.joint_count && p.weight_offset != U32_MAX;
            const bool use_row = is_skinned && table_pipelines[is_alpha] && p.palette_row.frame == frame &&
                                 p.palette_row.room >= MAX_JOINTS * 3;
            if (is_skinned && !use_row)
            {
                r.palette_ok = mPalettes.place(p.palette, p.joint_count, r.palette_set, r.palette_offset);
                LLRHI::sShadowSkinned.no_palette += r.palette_ok ? 0 : 1;
            }
            LLRHI::sShadowSkinned.no_weights += (p.palette && !is_skinned) ? 1 : 0;
            ++LLRHI::sShadowSkinned.casters;
        }

        rec.packets = packets;
        rec.count = count;
        rec.cmd = cmd;
        rec.set = LLVKBindless::set();
        memcpy(rec.pipelines, pipelines, sizeof(pipelines));
        memcpy(rec.table_pipelines, table_pipelines, sizeof(table_pipelines));
        rec.pooled_pipeline = pooled_pipeline;
        rec.frame = frame;
        rec.cull = LLVKTracker::passCullMode();
        memcpy(rec.view_proj, view_proj, sizeof(rec.view_proj));
        rec.target_width = target_width;
        rec.indirect = indirect;
        rec.indirect_offset = indirect_offset;
        rec.pooled = pooled;
        for (U32 c = 0; c < LLVKGPUDriven::MAX_CHUNKS; ++c)
        {
            for (U32 t = 0; t < 2; ++t)
            {
                rec.indirect_counts[c][t] = pooled ? (U32)mIndirect[c][t].size() : 0;
            }
            rec.chunks[c] = pooled || list_draws.any ? LLVKGPUDriven::chunkBuffer(c) : VK_NULL_HANDLE;
        }
        rec.max_draws = llmax(device.getLimits().maxDrawIndirectCount, 1u);
        rec.list_draws = list_draws;
        // research/206: the record keeps the scene's draws (both vectors keep their storage).
        const bool scene_any = mSceneTaken.any;
        const U32 scene_buckets = (U32)mSceneTaken.buckets.size();
        std::swap(rec.scene, mSceneTaken);
        mSceneTaken.any = false;
        mSceneTaken.buckets.clear();
        rec.scene_pipeline = scene_pipeline;
        rec.scene_mask_pipeline = scene_mask_pipeline;
        rec.scene_skin_pipeline = scene_skin_pipeline;
        rec.scene_skin_mask_pipeline = scene_skin_mask_pipeline;
        if (scene_any)
        {
            LLVKScene::shadowDrawn(true, scene_buckets);
        }
        rec.checkpoints = &device;
        rec.checkpoint_base = mDraws;

        if (job)
        {
            // The commands go into a stream spliced in right here; a cull
            // thread fills it while this thread goes on. beginPass above has
            // already dropped what the translation path knew of the bindings.
            LLVKRecorder::Stream* stream = LLVKRecorder::acquire();
            if (stream)
            {
                LLVKRecorder::splice(stream);
                rec.checkpoints = nullptr;
                job->stream = stream;
                job->bin = (U32)llmin(LLRHI::sShadowRecordBin, (S32)RECORD_BINS - 1);
                job->counts = PassCounts();
                job->ms = 0.0;
                job->on_main = false;
                ++mJobsUsed;
                job->state.store(JOB_POSTED, std::memory_order_release);
                if (!LLRHI::sPostWork(&run_record_job, job))
                {
                    run_record_job(job);    // no cull threads: record it now
                }
                mNanoseconds += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                return true;
            }
            ++mInlinePasses;    // no stream (recorder off, or splicing off): record inline below
        }

        // Splice self-test (research/66 step 1): from here on the pass's
        // commands go into a private stream (filled right here, on this
        // thread) and the recording worker replays it where the splice packet
        // stands, which is where they would have gone: the same commands in
        // the same order. beginPass above has already dropped what the
        // translation path knew of the bindings.
        LLVKRecorder::Stream* splice_stream = nullptr;
        if (LLRHI::sSpliceTest)
        {
            if (mSpliceTestFrame != frame)
            {
                mSpliceTestFrame = frame;
                mSpliceTestCount = 0;
            }
            if (mSpliceTestCount < LLRHI::sSpliceTestPasses && (splice_stream = LLVKRecorder::acquire()) != nullptr)
            {
                LLVKRecorder::splice(splice_stream);
                ++mSpliceTestCount;
                ++mSplicedPasses;
            }
        }
        {
            LLVKRecorder::Fill splice_fill(splice_stream);  // until the record (null: records as usual)
            PassCounts counts;
            record(rec, counts);
            mDraws += counts.draws;
            mPooledDraws += counts.pooled_draws;
            mIndirectCalls += counts.indirect_calls;
            mListCalls += counts.list_calls;
            mSceneCalls += counts.scene_calls;
            mPaletteRows += counts.palette_rows;
        }
        mNanoseconds += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        return true;
    }

    void ShadowRenderer::record(const PassRecord& rec, PassCounts& counts) const
    {
        const VkCommandBuffer cmd = rec.cmd;
        const Packet* const packets = rec.packets;
        const F32* const view_proj = rec.view_proj;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &rec.set, 0, nullptr);

        if (rec.pooled)
        {
            // One call per pool buffer and index size (split at the device's limit).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rec.pooled_pipeline);
            const U32 max_draws = rec.max_draws;
            const VkDeviceSize zero = 0;
            const VkDeviceSize stride = LLVKGPUDriven::POSITION_STRIDE;
            VkDeviceSize at = rec.indirect_offset;
            for (U32 c = 0; c < LLVKGPUDriven::MAX_CHUNKS; ++c)
            {
                for (U32 t = 0; t < 2; ++t)
                {
                    const U32 size = rec.indirect_counts[c][t];
                    if (!size)
                    {
                        continue;
                    }
                    const VkBuffer chunk = rec.chunks[c];
                    vkCmdBindVertexBuffers2(cmd, 0, 1, &chunk, &zero, nullptr, &stride);
                    vkCmdBindIndexBuffer(cmd, chunk, 0, t ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                    for (U32 done = 0; done < size; )
                    {
                        const U32 n = llmin(size - done, max_draws);
                        vkCmdDrawIndexedIndirect(cmd, rec.indirect, at, n, (U32)sizeof(VkDrawIndexedIndirectCommand));
                        at += (VkDeviceSize)n * sizeof(VkDrawIndexedIndirectCommand);
                        done += n;
                        ++counts.indirect_calls;
                    }
                    counts.pooled_draws += size;
                }
            }
        }
        const LLVKGPUList::Draws& list_draws = rec.list_draws;
        if (list_draws.any)
        {
            // The list's rows: per pool buffer and index size, the commands and
            // count the compute dispatch wrote. Same shader, same matrix rows
            // (view_proj x region matrix) as the pooled casters above.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rec.pooled_pipeline);
            const VkDeviceSize zero = 0;
            const VkDeviceSize stride = LLVKGPUDriven::POSITION_STRIDE;
            for (U32 b = 0; b < LLVKGPUList::BUCKETS; ++b)
            {
                const VkBuffer chunk = rec.chunks[b / 2];
                if (!list_draws.capacity[b] || !chunk)
                {
                    continue;
                }
                vkCmdBindVertexBuffers2(cmd, 0, 1, &chunk, &zero, nullptr, &stride);
                vkCmdBindIndexBuffer(cmd, chunk, 0, (b & 1) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexedIndirectCount(cmd, list_draws.buffer,
                                              list_draws.commands + (VkDeviceSize)list_draws.first[b] * sizeof(VkDrawIndexedIndirectCommand),
                                              list_draws.buffer, list_draws.counts + (VkDeviceSize)b * 4, list_draws.capacity[b],
                                              (U32)sizeof(VkDrawIndexedIndirectCommand));
                ++counts.list_calls;
            }
        }
        const LLVKScene::ShadowDraws& scene = rec.scene;
        VkPipeline scene_bound = VK_NULL_HANDLE;
        if (scene.any && rec.scene_pipeline && (!scene.masks || rec.scene_mask_pipeline) && (!scene.skins || rec.scene_skin_pipeline) &&
            (!scene.skin_masks || rec.scene_skin_mask_pipeline))
        {
            // research/206: the retained scene's records the viewer cut from
            // this pass, per bucket the commands and count its choice wrote.
            // Same pass state (depth clamp, culling, bias) as every caster here.
            // 5c: a mask's bucket with the mask shaders, its texture
            // coordinates and colors (none: the white constant, as a packet's).
            auto halves = [](VkDeviceAddress address, U32* out)
            {
                out[0] = (U32)(address & 0xffffffffull);
                out[1] = (U32)(address >> 32);
            };
            ScenePush scene_push = {};
            memcpy(scene_push.view_proj, scene.view_proj, sizeof(scene_push.view_proj));
            halves(scene.records, scene_push.tables);
            halves(scene.transforms, scene_push.tables + 2);
            halves(scene.uv_transforms, scene_push.tables2);
            halves(scene.materials, scene_push.tables2 + 2);
            halves(scene.texture_slots, scene_push.slot_table);
            scene_push.anim_time[0] = scene.anim_time[0];
            scene_push.anim_time[1] = scene.anim_time[1];
            scene_push.target_width = rec.target_width;
            scene_push.white_slot = scene.white_slot;
            scene_push.slot_count = scene.texture_slot_count;
            U32 pushed_mode = U32_MAX;
            for (const LLVKScene::ShadowDraws::Bucket& b : scene.buckets)
            {
                const VkPipeline wanted = b.skinned ? (b.mode ? rec.scene_skin_mask_pipeline : rec.scene_skin_pipeline)     // research/219
                                                    : (b.mode ? rec.scene_mask_pipeline : rec.scene_pipeline);
                if (wanted != scene_bound)
                {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
                    scene_bound = wanted;
                }
                if (pushed_mode == U32_MAX)
                {
                    scene_push.mode = b.mode;
                    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(scene_push),
                                       &scene_push);
                    pushed_mode = b.mode;
                }
                else if (b.mode != pushed_mode)
                {
                    const U32 mode = b.mode;
                    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       (U32)offsetof(ScenePush, mode), sizeof(mode), &mode);
                    pushed_mode = b.mode;
                }
                // research/219: a rigged bucket's weights (weight4, stride 16) at the binding after the others.
                if (b.mode)
                {
                    const VkBuffer buffers[4] = { b.positions, b.texcoords, b.colors ? b.colors : mDefaults, b.weights };
                    const VkDeviceSize offsets[4] = { 0, 0, b.colors ? 0u : 16u, 0 };
                    const VkDeviceSize strides[4] = { 16, 8, b.colors ? 4u : 0u, 16 };
                    vkCmdBindVertexBuffers2(cmd, 0, b.skinned ? 4 : 3, buffers, offsets, nullptr, strides);
                }
                else
                {
                    const VkBuffer buffers[2] = { b.positions, b.weights };
                    const VkDeviceSize offsets[2] = { 0, 0 };
                    const VkDeviceSize strides[2] = { 16, 16 };     // LLVertexBuffer's LLVector4a positions; weight4
                    vkCmdBindVertexBuffers2(cmd, 0, b.skinned ? 2 : 1, buffers, offsets, nullptr, strides);
                }
                vkCmdBindIndexBuffer(cmd, b.indices, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexedIndirectCount(cmd, scene.buffer, b.commands, scene.buffer, b.count, b.capacity,
                                              (U32)sizeof(VkDrawIndexedIndirectCommand));
                ++counts.scene_calls;
            }
        }

        ShadowPush push = {};
        // After the pooled draws: the loop below binds its own pipeline and
        // buffers (a pool buffer is never a caster's GL buffer, so it rebinds).
        VkPipeline bound = scene_bound ? scene_bound : rec.pooled_pipeline;
        VkBuffer bound_vb = VK_NULL_HANDLE, bound_ib = VK_NULL_HANDLE;
        U32 bound_offsets[4] = { U32_MAX, U32_MAX, U32_MAX, U32_MAX };
        U32 bound_palette = U32_MAX;
        U32 bound_index_type = 0;
        VkCullModeFlags bound_cull = rec.cull;
        for (size_t i = 0; i < rec.count; ++i)
        {
            const Packet& p = packets[i];
            const PassRecord::Resolved& r = rec.resolved[i];
            if (r.pooled || !r.vertex || !r.index || !p.index_count)
            {
                continue;
            }
            const bool is_alpha = p.mode != Packet::MODE_OPAQUE;
            const bool is_skinned = p.palette && p.joint_count && p.weight_offset != U32_MAX;
            if (p.palette && !is_skinned)
            {
                continue;   // rigged without skin data: GL skips these too
            }
            // The palette's row in the frame's table, when the packet names one of
            // this frame with room for every joint the shader may index.
            const bool use_row = is_skinned && rec.table_pipelines[is_alpha] && p.palette_row.frame == rec.frame &&
                                 p.palette_row.room >= MAX_JOINTS * 3;
            VkPipeline wanted = use_row ? rec.table_pipelines[is_alpha] : rec.pipelines[is_skinned][is_alpha];
            if (wanted != bound)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
                bound = wanted;
                bound_vb = VK_NULL_HANDLE;
            }
            if (is_skinned && !use_row)
            {
                // Placed by draw() (PaletteBuffer::place); not placed: no draw.
                if (!r.palette_ok)
                {
                    continue;
                }
                if (r.palette_offset != bound_palette)
                {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 1, 1, &r.palette_set, 1, &r.palette_offset);
                    bound_palette = r.palette_offset;
                }
            }
            const U32 tc = is_alpha ? p.texcoord_offset : U32_MAX;
            const U32 col = is_alpha ? p.color_offset : U32_MAX;
            const U32 weights = is_skinned ? p.weight_offset : U32_MAX;
            if (r.vertex != bound_vb || bound_offsets[0] != p.position_offset || bound_offsets[1] != tc || bound_offsets[2] != col ||
                bound_offsets[3] != weights)
            {
                // Bindings in use: position [, texcoord, color] [, weights].
                VkBuffer buffers[4] = { r.vertex, tc != U32_MAX ? r.vertex : mDefaults, col != U32_MAX ? r.vertex : mDefaults, r.vertex };
                VkDeviceSize offsets[4] = { p.position_offset, tc != U32_MAX ? tc : 0u, col != U32_MAX ? col : 16u, 0 };
                VkDeviceSize strides[4] = { 16, tc != U32_MAX ? 8u : 0u, col != U32_MAX ? 4u : 0u, 16 };
                U32 n = is_alpha ? 3 : 1;
                if (is_skinned)
                {
                    buffers[n] = r.vertex;
                    offsets[n] = weights;
                    strides[n] = 16;
                    ++n;
                }
                vkCmdBindVertexBuffers2(cmd, 0, n, buffers, offsets, nullptr, strides);
                bound_vb = r.vertex;
                bound_offsets[0] = p.position_offset;
                bound_offsets[1] = tc;
                bound_offsets[2] = col;
                bound_offsets[3] = weights;
            }
            if (r.index != bound_ib || bound_index_type != p.index_type)
            {
                vkCmdBindIndexBuffer(cmd, r.index, 0, p.index_type == GL_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                bound_ib = r.index;
                bound_index_type = p.index_type;
            }

            if (p.model)
            {
                mul4(view_proj, p.model, push.mvp);
            }
            else
            {
                memcpy(push.mvp, view_proj, sizeof(push.mvp));
            }
            U32 push_size = sizeof(push.mvp);
            if (is_alpha)
            {
                // Texture matrix rows, applied to (u, v, 0, 1).
                F32 tex_anim[16];   // research/194 11: a texture scroll's description as its matrix
                const F32* m = LLRHI::texAnimMatrix(p.texture_matrix, tex_anim);
                const F32 row0[4] = { m ? m[0] : 1.f, m ? m[4] : 0.f, m ? m[8] : 0.f, m ? m[12] : 0.f };
                const F32 row1[4] = { m ? m[1] : 0.f, m ? m[5] : 1.f, m ? m[9] : 0.f, m ? m[13] : 0.f };
                memcpy(push.tex_row0, row0, sizeof(row0));
                memcpy(push.tex_row1, row1, sizeof(row1));
                memcpy(push.textures, r.slots, sizeof(push.textures));
                push.params[0] = p.alpha_cutoff;
                push.params[1] = rec.target_width;
                push.params[2] = (F32)p.mode;
                push_size = sizeof(push);
            }
            if (is_skinned)
            {
                // The mode and where the palette's hair sway entries are (0: none):
                // every skinned shader reads params.z.
                push.params[2] = (F32)((is_alpha ? p.mode : 0u) + HAIR_SWAY_MODE_BITS * hair_sway_index(p.palette, p.joint_count));
                push_size = sizeof(push);
            }
            if (use_row)
            {
                // The row's first vec4, as bits (read with floatBitsToUint).
                memcpy(&push.params[3], &p.palette_row.base, 4);
                push_size = sizeof(push);
                ++counts.palette_rows;
            }
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, &push);
            // research/137 N7: a caster's own culling (cull mode is dynamic state).
            const VkCullModeFlags cull = p.cull == Packet::CULL_INHERIT ? rec.cull :
                                         p.cull == 0 ? VK_CULL_MODE_NONE : p.cull == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
            if (cull != bound_cull)
            {
                vkCmdSetCullMode(cmd, cull);
                bound_cull = cull;
            }
            if (rec.checkpoints)
            {
                rec.checkpoints->checkpoint(cmd, (0xffffffffull << 32) | (rec.checkpoint_base + counts.draws));     // named in a fault report
            }
            vkCmdDrawIndexed(cmd, p.index_count, 1, p.first_index, 0, 0);
            ++counts.draws;
        }
        if (bound_cull != rec.cull)
        {
            vkCmdSetCullMode(cmd, rec.cull);    // the generic path's record of the dynamic state (sRecorded) says the pass's
        }
    }

    void ShadowRenderer::fill(RecordJob& job)
    {
        const auto start = std::chrono::steady_clock::now();
        try
        {
            // This thread's commands go into the job's stream until the end of
            // this scope, which marks it complete for the recording worker.
            LLVKRecorder::Fill stream_fill(job.stream);
            try
            {
                record(job.rec, job.counts);
            }
            catch (...)
            {
                // The pass's cull mode, as record() leaves it at its end (the
                // generic path's sRecorded says the pass's).
                vkCmdSetCullMode(job.rec.cmd, job.rec.cull);
                throw;
            }
        }
        catch (...)
        {
            // fix-threads4: a pass that threw (bad_alloc, ...) on a cull
            // thread: logged (the first few), its casters partly missing this
            // frame; the job still ends, so join() does not wait forever.
            static std::atomic<U32> sFailures{ 0 };
            if (sFailures.fetch_add(1, std::memory_order_relaxed) < 5)
            {
                try
                {
                    LL_WARNS("VKNative") << "a shadow pass recorded on a cull thread failed: some casters are missing this frame" << LL_ENDL;
                }
                catch (...)
                {
                }
            }
        }
        job.ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count();
        {
            std::lock_guard<std::mutex> lock(mJobMutex);
            job.state.store(JOB_DONE, std::memory_order_release);
        }
        mJobDone.notify_all();
    }

    void ShadowRenderer::join()
    {
        if (!mJobsUsed)
        {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        F64 main_ms = 0.0;
        for (U32 j = 0; j < mJobsUsed; ++j)
        {
            RecordJob& job = mJobs[j];
            int expected = JOB_POSTED;
            if (job.state.compare_exchange_strong(expected, JOB_RUNNING, std::memory_order_acquire, std::memory_order_relaxed))
            {
                // No cull thread has started it: record it here (its stream is
                // spliced already; the recording worker waits for it).
                job.on_main = true;
                fill(job);
                main_ms += job.ms;
                continue;
            }
            std::unique_lock<std::mutex> lock(mJobMutex);
            mJobDone.wait(lock, [&job] { return job.state.load(std::memory_order_acquire) == JOB_DONE; });
        }
        const F64 total_ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count();
        mJoinWaitMs += llmax(total_ms - main_ms, 0.0);

        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        for (U32 j = 0; j < mJobsUsed; ++j)
        {
            RecordJob& job = mJobs[j];
            mDraws += job.counts.draws;
            mPooledDraws += job.counts.pooled_draws;
            mIndirectCalls += job.counts.indirect_calls;
            mListCalls += job.counts.list_calls;
            mSceneCalls += job.counts.scene_calls;
            mPaletteRows += job.counts.palette_rows;
            if (job.on_main)
            {
                ++mMainPasses;
                mMainMs += job.ms;
            }
            else
            {
                ++mThreadPasses[job.bin];
                mThreadMs[job.bin] += job.ms;
            }
            job.stream = nullptr;
            job.rec.packets = nullptr;
            job.state.store(JOB_IDLE, std::memory_order_relaxed);
        }
        mJobsUsed = 0;
    }

    void run_record_job(void* arg)
    {
        RecordJob* job = (RecordJob*)arg;
        int expected = JOB_POSTED;
        if (job->state.compare_exchange_strong(expected, JOB_RUNNING, std::memory_order_acquire, std::memory_order_relaxed))
        {
            sShadows.fill(*job);
        }
    }

    void join_shadow_recording()
    {
        sShadows.join();
    }

    void on_texture_deleted(LLVKTracker::GLName texture)
    {
        LLVKBindless::release(texture);
    }
}

void LLVKNative::install()
{
    LLVKGtao::install();        // research/92: GTAO and contact shadows (fullscreen passes, no shadow casters needed)
    LLVKLook::install();        // research/102: looks, the colour grading LUT (a fullscreen pass)
    LLVKUpscale::install();     // research/165: the main view drawn smaller and upscaled, FSR 1 (fullscreen passes)
    LLVKTaa::install();         // research/185: temporal anti-aliasing (a fullscreen pass)
    LLVKGlare::install();       // research/108: glare (HDR bloom) and the sun's lens flare (fullscreen passes)
    LLVKSkinSss::install();     // research/100: skin subsurface scattering (fullscreen passes on the skin mark)
    LLVKSkinFill::install();    // research/110: softer shadows on skin (fullscreen passes on the skin mark)
    LLVKWetLook::install();     // research/120: wet look, the G-buffer made wet before the lighting (fullscreen passes)
    LLVKRain::install();        // research/120 7-8: rain streaks and splashes before tone mapping (a fullscreen pass)
    LLVKAtmosphere::install();  // research/148: ground fog and fireflies (fullscreen passes)
    LLVKSsr::install();         // research/129: screen space reflections in passes of their own (GCP's Hi-Z tracing)
    LLVKSsgi::install();        // research/164: screen space indirect light, GTAO widened into a visibility bitmask (fullscreen passes)
    // The bindless slots of deleted textures go back whether or not the
    // shadow casters below are ready: the translated draws take slots too
    // (fix-round14 4: a failed shadow setup kept every slot until the table filled).
    LLVKTracker::setTextureDeletedCallback(&on_texture_deleted);
    LLVKScene::install();       // research/197 stage 2: the retained scene (LLRHI_SCENE; its tables are made when first used)
    // Built up front so a shader or layout problem shows at startup, not mid-session.
    if (!sShadows.ready())
    {
        return;
    }
    LLRHI::sDrawShadowPackets = &LLVKNative::drawShadowPackets;
    LLRHI::sJoinShadowRecording = &join_shadow_recording;     // research/66 step 2
    LL_INFOS("VKNative") << "native shadow casters ready" << LL_ENDL;
    if (sShadows.pooledReady())
    {
        LLVKGPUList::install();     // its rows draw with the pooled casters' shader
    }
    LLVKGPUList::installProbe();    // research/46 G5: the probe faces' G-buffer list (made when first used)
}

bool LLVKNative::drawShadowPackets(const LLRHI::ShadowPacket* packets, size_t count, const F32* view_proj, F32 target_width,
                                   const LLRHI::ShadowListPass* list)
{
    return sShadows.draw(packets, count, view_proj, target_width, list);
}

std::string LLVKNative::takeStats(U32 frames)
{
    frames = llmax(frames, 1u);
    std::ostringstream out;
    out << "native shadow draws " << sShadows.mDraws / frames << " in " << (F64)sShadows.mNanoseconds / frames / 1e6 << " ms";
    const std::string pool = LLVKGPUDriven::takeStats(frames);
    if (sShadows.mPooledDraws || !pool.empty())
    {
        out << " (also pooled " << sShadows.mPooledDraws / frames << " in " << sShadows.mIndirectCalls / frames << " indirect calls; "
            << pool << ")";
    }
    const std::string list = LLVKGPUList::takeStats(frames);
    if (!list.empty())
    {
        out << " (GPU " << list << ", " << sShadows.mListCalls / frames << " indirect count calls a frame)";
    }
    const std::string probe = LLVKGPUList::takeProbeStats(frames);
    if (!probe.empty())
    {
        out << " (GPU " << probe << ")";
    }
    const std::string scene = LLVKScene::takeStats(frames);     // research/197 stage 2
    if (!scene.empty())
    {
        out << " (" << scene << ")";
    }
    const std::string rt = LLVKRtAs::takeStats(frames);        // research/230 R2 (empty while ray tracing was never on)
    if (!rt.empty())
    {
        out << " (" << rt << ")";
    }
    if (sShadows.mSceneCalls)
    {
        out << " (scene shadow indirect count calls " << (F64)sShadows.mSceneCalls / frames << " a frame)";   // research/206
    }
    if (sShadows.mSplicedPasses || LLRHI::sSpliceTest)
    {
        out << " (splice test: " << (F64)sShadows.mSplicedPasses / frames << " passes a frame through a spliced stream)";
    }
    // Research/66 step 2: passes recorded on the cull threads (per bin: sun
    // cascade, probe map), their recording time, and what the main thread
    // still spent on them (passes no thread had started at the join, waiting
    // at the join). Saved on the main thread: about the threads' ms minus the wait.
    U32 thread_passes = 0;
    F64 thread_ms = 0.0;
    for (U32 b = 0; b < RECORD_BINS; ++b)
    {
        thread_passes += sShadows.mThreadPasses[b];
        thread_ms += sShadows.mThreadMs[b];
    }
    if (thread_passes || sShadows.mMainPasses || sShadows.mInlinePasses || LLRHI::sShadowRecordThreads)
    {
        static const char* const bins[RECORD_BINS] = { "sun0", "sun1", "sun2", "sun3", "probe0", "probe1", "probe2", "probe3" };
        out << " (record threads " << (LLRHI::sShadowRecordThreads ? "on" : "off") << ": " << (F64)thread_passes / frames
            << " passes a frame on cull threads in " << thread_ms / frames << " ms [";
        bool first = true;
        for (U32 b = 0; b < RECORD_BINS; ++b)
        {
            if (sShadows.mThreadPasses[b])
            {
                out << (first ? "" : ", ") << bins[b] << " " << (F64)sShadows.mThreadPasses[b] / frames << " in "
                    << sShadows.mThreadMs[b] / frames << " ms";
                first = false;
            }
        }
        out << "], taken by the main thread " << (F64)sShadows.mMainPasses / frames << " in " << sShadows.mMainMs / frames
            << " ms, join wait " << sShadows.mJoinWaitMs / frames << " ms, main saved ~"
            << (thread_ms - sShadows.mJoinWaitMs) / frames << " ms, inline " << (F64)sShadows.mInlinePasses / frames << ")";
    }
    for (U32 b = 0; b < RECORD_BINS; ++b)
    {
        sShadows.mThreadPasses[b] = 0;
        sShadows.mThreadMs[b] = 0.0;
    }
    sShadows.mMainPasses = 0;
    sShadows.mMainMs = 0.0;
    sShadows.mJoinWaitMs = 0.0;
    sShadows.mInlinePasses = 0;
    sShadows.mSplicedPasses = 0;
    sShadows.mDraws = 0;
    sShadows.mPooledDraws = 0;
    sShadows.mIndirectCalls = 0;
    sShadows.mListCalls = 0;
    sShadows.mSceneCalls = 0;
    sShadows.mNanoseconds = 0;
    return out.str();
}

void LLVKNative::takePaletteStats(U32& copies, U32& reused, U32& rows)
{
    std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
    copies = sShadows.mPalettes.mCopies;
    reused = sShadows.mPalettes.mReused;
    rows = sShadows.mPaletteRows;
    sShadows.mPalettes.mCopies = 0;
    sShadows.mPalettes.mReused = 0;
    sShadows.mPaletteRows = 0;
}
