/**
 * @file llvkgl_drawpath.h
 * @brief Types and small inline functions of the per-draw path, shared by the llvkgl*.cpp files (research/91).
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

#ifndef LL_LLVKGL_DRAWPATH_H
#define LL_LLVKGL_DRAWPATH_H

#include "llvkgl_internal.h"
#include "llrhihash.h"

#if LL_GL_FUNC_POINTER

// The draw path's shared types and state. Functions called on every draw from
// another file are inline here: without LTO a call across files is never
// inlined (research/91 2.4).
namespace llvkgl
{
    // What the frame's command buffer already has bound/set, so unchanged
    // state is not re-recorded draw after draw. Reset with every new frame.
    struct Recorded
    {
        U32 frame = U32_MAX;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        U32 dynamic_offset = 0;
        // The bindless table is bound at set 0 (every translated layout declares the
        // same set 0 and push range, so it stays valid while only set 1 changes).
        bool table_bound = false;
        // With set null: a draw on the shared layout (research/50 step 3) bound
        // the table at set 0 and nothing at set 1. Whoever nulls `set` because
        // set 0 may have changed clears this too.
        bool table_only = false;
        std::vector<VkBuffer> vertex_buffers;
        std::vector<VkDeviceSize> vertex_offsets;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkDeviceSize index_offset = 0;
        VkIndexType index_type = VK_INDEX_TYPE_MAX_ENUM;
        bool dynamic = false;
        VkViewport viewport = {};
        VkRect2D scissor = {};
        VkCullModeFlags cull = 0;
        VkFrontFace front = VK_FRONT_FACE_MAX_ENUM;
        VkPrimitiveTopology prim = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        VkBool32 depth_test = 0, depth_write = 0, bias_enable = 0, stencil_test = 0;
        VkCompareOp depth_op = VK_COMPARE_OP_MAX_ENUM;
        F32 bias_units = 0.f, bias_factor = 0.f, bias_clamp = 0.f;
        U32 stencil[2][7] = {};
        U32 slots[LLVKBindless::MAX_PUSHED_SLOTS] = {};     // push constants last recorded
        U32 slot_count = 0;
        // OIT (research/41, oit_bind_set): set 2 as bound with oit_layout (null:
        // not known to be bound), the descriptor set binds recorded until then
        // (LLVKDispatch's count), and those bind_program_set recorded since
        // with oit_program_layout (compatible for sets 0 and 1: set 2 stays).
        VkPipelineLayout oit_layout = VK_NULL_HANDLE;
        VkPipelineLayout oit_program_layout = VK_NULL_HANDLE;
        VkDescriptorSet oit_set = VK_NULL_HANDLE;
        VkCommandBuffer oit_cmd = VK_NULL_HANDLE;
        U32 oit_epoch = 0;
        U32 oit_binds = 0;
        U32 oit_compatible = 0;
        U32 oit_priority = 0;
        bool oit_priority_pushed = false;   // oit_priority is in the last word (while slot_count stays below it)
    };
    extern Recorded sRecorded;
    extern U32 sRecordEpoch;

    // Every program's entry (record_epoch): the command buffer's bindings or
    // push constants were changed outside sRecorded's bookkeeping.
    inline void pass_invalidate()
    {
        ++sRecordEpoch;
    }

    // Whether the bindless table is what set 0 has bound.
    inline bool table_at_set0(const Recorded& r)
    {
        return r.table_bound && (r.set || r.table_only);
    }

    // The versions of the program's own state in the key (textures: sampled_units_key).
    struct PassVersions
    {
        U32 unit_version, unit_bindings, uniform_blocks, buffer_deletes;
    };

    inline PassVersions pass_versions(const Program& program)
    {
        PassVersions v = { program.unit_version, 0, 0, 0 };
        if (!program.sampler_slots.empty())
        {
            // A sum of the units' binding counters only grows when one of them
            // changes (unit_values are fixed while unit_version is).
            for (GLint unit : program.unit_values)
            {
                if (unit >= 0 && unit < 32)
                {
                    v.unit_bindings += sUnitBindings[unit];
                }
            }
        }
        if (program.buffer_blocks)
        {
            v.uniform_blocks = sUniformBlockState;
            v.buffer_deletes = sBufferDeletes;
        }
        return v;
    }

    void set_dynamic_state(VkCommandBuffer cmd, const LLVKMirror::Target& target, VkPrimitiveTopology prim);
    // research/137 N11: set_dynamic_state in two halves. dynamic_values: what
    // a draw into `target` sets as dynamic state, from the GL state (sState,
    // tOITDraw, shadow_depth_bias()) on the thread that resolves the draw.
    // record_dynamic_values: what of it differs from r, recorded (all of it
    // while r.dynamic is false), and r brought up to date. It reads nothing
    // else, so a draw list's segment recorded on a cull thread
    // (llvkdrawlist.cpp) records with a Recorded of its own.
    struct DynamicValues
    {
        VkViewport viewport = {};
        VkRect2D scissor = {};
        VkCullModeFlags cull = 0;
        VkFrontFace front = VK_FRONT_FACE_MAX_ENUM;
        VkPrimitiveTopology prim = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        VkBool32 depth_test = 0, depth_write = 0, bias_enable = 0, stencil_test = 0;
        VkCompareOp depth_op = VK_COMPARE_OP_MAX_ENUM;
        F32 bias_units = 0.f, bias_factor = 0.f, bias_clamp = 0.f;
        // Per face (front, back): GL's fail, pass, depth fail, function,
        // compare mask, write mask and reference (Recorded::stencil's order).
        U32 stencil[2][7] = {};
    };
    DynamicValues dynamic_values(const LLVKMirror::Target& target, VkPrimitiveTopology prim);
    void record_dynamic_values(VkCommandBuffer cmd, Recorded& r, const DynamicValues& v);

    inline VkStencilOp stencil_op(GLenum op)
    {
        switch (op)
        {
        case GL_ZERO: return VK_STENCIL_OP_ZERO;
        case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
        case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case GL_INVERT: return VK_STENCIL_OP_INVERT;
        case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
        }
    }

    // ------------------------------------------------------------------
    // Render targets: the attachments' layouts before recording into them.

    inline VkImageLayout depth_layout(const ResolvedTarget& t)
    {
        return t.target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    inline bool needs_attachment_layouts(const ResolvedTarget& t)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (t.is_default)
        {
            return mirror.mColorLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                   mirror.mDepthLayout != depth_layout(t);
        }
        for (U32 i = 0; i < t.color_textures; ++i)
        {
            if (t.colors[i]->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            {
                return true;
            }
        }
        return t.depth && t.depth->layout != depth_layout(t);
    }

    inline void to_attachment_layouts(VkCommandBuffer cmd, const ResolvedTarget& t)
    {
        LLVKMirror::BarrierBatch batch(cmd);    // the attachments' barriers in one call (research/111 A-BB)
        LLVKMirror& mirror = LLVKMirror::instance();
        if (t.is_default)
        {
            if (mirror.mColorLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            {
                LLVKMirror::imageBarrier(cmd, mirror.getColorImage(), VK_IMAGE_ASPECT_COLOR_BIT, mirror.mColorLayout,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
                mirror.mColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ++sLayoutState;
            }
            if (mirror.mDepthLayout != depth_layout(t))
            {
                LLVKMirror::imageBarrier(cmd, mirror.getDepthImage(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                         mirror.mDepthLayout, depth_layout(t), 1, 1);
                mirror.mDepthLayout = depth_layout(t);
                ++sLayoutState;
            }
            return;
        }
        for (U32 i = 0; i < t.color_textures; ++i)
        {
            transition(cmd, *t.colors[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (t.depth)
        {
            transition(cmd, *t.depth, depth_layout(t));
        }
    }

    // ------------------------------------------------------------------
    // Pipelines (llvkgl_pipeline.cpp)

    struct VertexInput
    {
        U32 location;
        VkFormat format;
        U32 stride;
    };

    // FNV-1a over the words (selftest keys_fuzz copies it, with llrhihash.h).
    struct KeyHash
    {
        size_t operator()(const std::vector<U32>& key) const
        {
            return (size_t)LLRHI::fnv1aWords(LLRHI::FNV1A_BASIS_SHORT, key.data(), key.size());
        }
    };
    extern std::vector<U32> tPipelineKey;

    inline VkPrimitiveTopology topology(GLenum mode)
    {
        switch (mode)
        {
        case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    }

    VkPipeline get_pipeline(GLuint program_id, Program& program, const std::vector<VertexInput>& inputs, VkPrimitiveTopology prim,
                            const LLVKMirror::Target& target, bool generated = false, const OITVariant* oit = nullptr, bool split = false);
    // research/208 S1: the same with the GL state given (a pass head's, read
    // at the pass's entry) instead of the thread's sState.
    VkPipeline get_pipeline_with(const State& state, GLuint program_id, Program& program, const std::vector<VertexInput>& inputs,
                                 VkPrimitiveTopology prim, const LLVKMirror::Target& target, bool generated, const OITVariant* oit,
                                 bool split);

    inline U32 topology_class(VkPrimitiveTopology t)
    {
        return t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 0 :
               (t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) ? 1 : 2;
    }

    // A translated program's pipelines, by key (get_pipeline). Under sLock.
    // The key's words: these, then the color formats, 3 words a vertex input
    // (location, format, stride), and PIPELINE_KEY_DGC twice for device
    // generated commands or one PIPELINE_KEY_OIT* for an OIT variant.
    enum PipelineKeyWord : U32
    {
        PK_PROGRAM, PK_LINK, PK_LAYOUT,         // this link of this program (this session only)
        PK_TOPOLOGY,                            // topology_class
        PK_POLYGON, PK_BLEND, PK_SRC_RGB, PK_DST_RGB, PK_SRC_A, PK_DST_A, PK_EQ_RGB, PK_EQ_A,     // GL state
        PK_COLOR_MASK,                          // R, G, B, A in bits 0-3
        PK_COLOR_COUNT, PK_DEPTH_FORMAT, PK_STENCIL, PK_DEPTH_CLAMP,
        PK_COLOR_FORMATS,
    };
    constexpr U32 PIPELINE_KEY_DGC = 0x44474331u;           // 'DGC1'
    constexpr U32 PIPELINE_KEY_OIT = 0x4f495431u;           // 'OIT1'
    constexpr U32 PIPELINE_KEY_OIT_SPLIT = 0x4f495432u;     // 'OIT2': a hybrid session's world draws
    extern std::unordered_map<std::vector<U32>, VkPipeline, KeyHash> sPipelines;

    // research/111 B-PL: what vkCreateGraphicsPipelines takes for one, as
    // plain values. Made on the render thread (make_pipeline_recipe, from a
    // key), created on any thread (create_pipeline, llvkgl_pipeprewarm.cpp).
    struct PipelineRecipe
    {
        struct Stage
        {
            VkShaderStageFlagBits stage;
            VkShaderModule module;
            bool oit_variant;                   // takes oit_constants
        };
        std::vector<Stage> stages;
        U32 oit_constants[2] = { 0, 0 };        // OIT_BLEND_CONSTANT_ID, OIT_SPLIT_CONSTANT_ID
        std::vector<VertexInput> inputs;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
        bool depth_clamp = false;
        U32 color_count = 0;
        VkFormat color_formats[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
        VkPipelineColorBlendAttachmentState blends[4] = {};
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        bool stencil = false;
        bool generated = false;                 // dynamic vertex strides (device generated commands)
        VkPipelineLayout layout = VK_NULL_HANDLE;
    };
    void make_pipeline_recipe(const Program& program, const std::vector<U32>& key, VkPrimitiveTopology prim, const OITVariant* oit,
                              PipelineRecipe& recipe);
    VkPipeline create_pipeline(const PipelineRecipe& recipe);
    // A pipeline get_pipeline made: its key is kept for the next session.
    void note_pipeline_key(const Program& program, const std::vector<U32>& key);
    // A program's modules and layouts are about to be destroyed (the deferred
    // half of destroy_program_objects): a prewarm making a pipeline from
    // them finishes first, and one about to start does not.
    void retire_pipeline_objects();

    // ------------------------------------------------------------------
    // Draws (llvkgl_draw.cpp)

    struct IndexInfo
    {
        bool indexed = false;
        GLenum type = GL_UNSIGNED_SHORT;
        size_t offset = 0;
    };
    extern const LLRHI::DirectDraw* tDirect;    // the direct draw being recorded, null for GL draws
    // first index + count of a draw within its index buffer (the CPU copy's
    // length; null buffer: false). type: GL_UNSIGNED_SHORT / GL_UNSIGNED_INT.
    bool index_range_fits(const Buffer* buffer, GLenum type, size_t offset, GLsizei count);

    // A direct draw's vertex inputs (research/39 N3a). The format part (per
    // program input: location, VkFormat, stride, and which packet attribute or
    // constant feeds it) depends on the attribute layout only, not on the
    // buffer or its offsets (LLVertexBuffer layouts are planar: the offsets
    // change with the vertex count), so it is kept while the layout repeats;
    // per draw there is one buffer lookup and the offsets.
    struct DirectInputs
    {
        const void* program = nullptr;
        U32 program_changes = U32_MAX;
        U32 constants = U32_MAX;
        U32 frame = U32_MAX;            // the constants live in the frame's ring
        U64 format = 0;
        std::vector<VertexInput> inputs;
        std::vector<S32> source;        // packet attribute index, -1: constant
        std::vector<VkBuffer> constant_buffers;
        std::vector<VkDeviceSize> constant_offsets;
        U64 signature = 0;
        bool needs_buffer = false;      // some input comes from the vertex buffer
    };
    extern thread_local DirectInputs tDirectInputs;
    extern std::vector<VkBuffer> tVertexBuffers;
    extern std::vector<VkDeviceSize> tVertexOffsets;
    extern thread_local VkBuffer tFanIndexBuffer;
    // tDirect for a scope: what was there before comes back on every way out
    // (early returns and exceptions included).
    struct DirectScope
    {
        explicit DirectScope(const LLRHI::DirectDraw* draw) : saved(tDirect) { tDirect = draw; }
        ~DirectScope() { tDirect = saved; }
        DirectScope(const DirectScope&) = delete;
        DirectScope& operator=(const DirectScope&) = delete;
        const LLRHI::DirectDraw* const saved;
    };
    // A fan / line loop draw's index buffer (tFanIndexBuffer) is cleared on every way out.
    struct FanIndexScope
    {
        FanIndexScope() = default;
        ~FanIndexScope() { tFanIndexBuffer = VK_NULL_HANDLE; }
        FanIndexScope(const FanIndexScope&) = delete;
        FanIndexScope& operator=(const FanIndexScope&) = delete;
    };
    extern thread_local U8 tOITDraw;            // the draw's OITDrawMode
    void draw(GLenum mode, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index_in);
    // llvkgl_draw.cpp: LLRHI's direct draws, the light table, clears, and the
    // fault report's draws; their counters (perfReport).
    bool direct_draw(const LLRHI::DirectDraw& d);
    // Pieces of the draw path, published for the draw lists (llvkdrawlist.cpp,
    // research/137 N7) and their recording elsewhere later (research/140 7-4).
    // direct_inputs: tVertexBuffers / tVertexOffsets for tDirect (false: the
    // draw cannot be recorded). bind_program_set: set 1 with its default block
    // at dynamic_offset (and the table at set 0) unless bound. record_draw_commands:
    // a bound draw's push constants (tSlots), vertex and index buffers and the draw.
    bool direct_inputs(Program& program, U64& signature);
    void bind_program_set(VkCommandBuffer cmd, const Program& program, VkDescriptorSet set, bool has_dynamic, U32 dynamic_offset);
    // research/137 N11: bind_program_set with what it reads given: the record
    // of the command buffer's bindings (sRecorded, or a cull thread's own for
    // a draw list's segment), the program's layout and this frame's bindless
    // table (LLVKBindless::set()). Touches nothing else.
    void bind_program_set(VkCommandBuffer cmd, Recorded& r, VkPipelineLayout layout, VkDescriptorSet table, VkDescriptorSet set,
                          bool has_dynamic, U32 dynamic_offset);
    void record_draw_commands(VkCommandBuffer cmd, const Program& program, GLint first, GLsizei count, GLsizei instances,
                              const IndexInfo& index, VkBuffer index_buffer, VkIndexType index_type);

    // A draw list (research/137 N7): what holds for all its draws, resolved
    // once by list_begin (draw_one's checks before a draw's own values, the
    // program, the draw target). Under sLock, on the thread that records.
    struct ListHead
    {
        Program* program = nullptr;
        ResolvedTarget target;
        U32 target_state = 0;           // sTargetState and the framebuffer the target was resolved for
        GLuint framebuffer = 0;
        // Whether `target` resolved for target_state/framebuffer: while they stay
        // the same, a failed resolve (a partly filled target) keeps the draws out.
        bool target_ok = false;
        bool depth_only_program = false;
    };
    // False: the list's draws must go through direct_draw one by one (a probe
    // capture, an occlusion query, a depth-only pass, the census, no frame,
    // no program, an incomplete target, draws that write nothing): its own
    // branches handle those exactly as for the old path's draws.
    bool list_begin(ListHead& head);
    // direct_draw for one of the list's draws (the same counts and result).
    bool list_draw(ListHead& head, const LLRHI::DirectDraw& d);
    // Why direct_draw refuses `d` with `program` current (outside the probe
    // capture, which takes every draw); DIRECT_REFUSAL_COUNT: it takes it.
    inline U32 direct_refusal(const Program* program, const LLRHI::DirectDraw& d)
    {
        const bool indexed = d.index_count > 0;
        if (!program)
        {
            return DIRECT_NO_PROGRAM;
        }
        if (!program->built)
        {
            return DIRECT_UNBUILT;
        }
        if ((!indexed && !d.vertex_count) || !d.attributes ||
            (indexed && d.index_type != GL_UNSIGNED_SHORT && d.index_type != GL_UNSIGNED_INT))
        {
            return DIRECT_SHAPE;
        }
        // Units sampled through descriptor images (not the bindless table) take
        // their texture from the GL binding only.
        for (GLint unit : program->unit_values)
        {
            if (unit >= 0 && (U32)unit < d.texture_count && d.textures[unit] != LLRHI::DirectDraw::KEEP_UNIT)
            {
                return DIRECT_UNIT;
            }
        }
        return DIRECT_REFUSAL_COUNT;
    }
    // The room of a row sPlacePalette places (llvkgl_hooks.cpp): what every
    // rigged mesh program declares (mat3x4 matrixPalette[LL_MAX_JOINTS_PER_MESH_OBJECT])
    // and the native shadow casters' shaders may index. Shared with the draw
    // lists (llvkdrawlist.cpp), which name such rows (research/137 N7).
    constexpr U32 PREBUILT_PALETTE_ROOM = 110 * 3;
    // Whether LLRHI::sUsePaletteRow takes `row` for `program` (its draws then
    // read their palette from the frame's table). Reads only what a link sets
    // and the frame number (the present moves it).
    inline bool palette_row_takes(const Program* program, const LLRHI::PaletteRow& row)
    {
        return program && program->built && program->palette_columns == 3 && row.frame == sFrame && row.room &&
               (U64)program->palette_elements * program->palette_columns <= row.room;
    }
    bool native_light_table_program(U32 gl_program);
    F32* native_table_rows(U32 vec4s, U32& base);
    void clear(GLbitfield mask);
    void report_lost_draws(const std::vector<U64>& markers);
    extern U32 sFansConverted;
    extern U32 sLoopsConverted;
    extern U32 sVertexInputsReused;
    extern U32 sDirectInputsReused;

    // ------------------------------------------------------------------
    // Descriptors and push constants (llvkgl_descriptors.cpp): the draw's
    // descriptor set, texture slots, matrix, palette, material and constants rows.

    extern std::vector<VkWriteDescriptorSet> tWrites;
    extern const std::vector<Texture*>* tSampledUsed;
    extern std::vector<U64> tKey;
    extern std::vector<Texture*> tBindlessSampled;
    extern U32 tSlots[LLVKBindless::MAX_PUSHED_SLOTS];
    extern GLuint tSlotNames[LLVKBindless::MAX_PUSHED_SLOTS];
    extern VkImageView tSlotViews[LLVKBindless::MAX_PUSHED_SLOTS];
    extern U32 tSlotCount;
    extern U32 tDynamicOffset;
    extern bool tHasDynamic;
    extern bool tPaletteFull;
    extern U32 sWhiteFrame;
    extern U32 sLightBatches, sLightInstances;
    extern U32 sSlotsUnitCached, sSlotsFrameCached, sSlotsResolved;

    bool texture_slot_cached(const Texture& tex, const ResolvedTarget& target, bool slot_cache);
    void gather_slots(Program& program, const ResolvedTarget& target);
    bool gather_descriptors(Program& program, ResolvedTarget& target);
    U32 matrix_row(Program& program);
    U64 consts_hash(const Program& program);
    bool direct_writes_block(Program& program);
    bool default_block_copy(Program& program, VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset);

    // The size of the default block's descriptor (0: the program binds none).
    inline VkDeviceSize default_block_size(const Program& program)
    {
        if (program.default_block < 0 || (size_t)program.default_block >= program.vk.blocks.size() ||
            program.vk.blocks[program.default_block].binding < 0)
        {
            return 0;
        }
        return (VkDeviceSize)llmax(program.vk.blocks[program.default_block].size, 16);
    }

    // ------------------------------------------------------------------
    // Occlusion queries (llvkgl_occlusion.cpp): a box marks its query's word
    // in the frame's visibility buffer.

    constexpr U32 VISIBILITY_WORDS = 32768;
    // research/179 A: the marks go to host memory (LLRHI_OCCLUSION_STORE=0,
    // `buffer`, cleared after each harvest) or to device-local memory (1,
    // `gpu`: a box stores the recording's stamp, so nothing is cleared; the
    // used words are copied to `readback` at the frame's end). 2 does both.
    struct VisibilityBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;   // host marks: 1 where seen
        VmaAllocation memory = VK_NULL_HANDLE;
        U32* words = nullptr;
        VkBuffer gpu = VK_NULL_HANDLE;      // device marks: the stamp where seen
        VmaAllocation gpu_memory = VK_NULL_HANDLE;
        VkBuffer readback = VK_NULL_HANDLE; // the device marks copied for the CPU
        VmaAllocation readback_memory = VK_NULL_HANDLE;
        const U32* readback_words = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;       // binding 0 the host marks, 1 the device marks (store 0)
        VkDescriptorSet gpu_set = VK_NULL_HANDLE;   // binding 0 the device marks, 1 the host marks (store 1, 2)
        U32 store = 0;                  // LLRHI::Occlusion::sStoreMode of the recording whose marks it holds
        U32 stamp = 0;                  // what that recording's device marks hold where seen (never 0)
        U64 copied = 0;                 // begins of the recording whose device marks were copied to readback
        U32 frame = U32_MAX;            // frame whose marks it holds
        U64 begins = 0;                 // LLVKMirror::frameBegins() of that frame (visibility_fenced)
        U32 used = 0;
        std::vector<U32> results;       // marks of results_frame
        U32 results_frame = U32_MAX;

        VkDescriptorSet markSet() const { return store ? gpu_set : set; }
    };
    struct GLQuery
    {
        U32 frame = U32_MAX;    // frame it was begun in
        U32 slot = 0;
        U32 word = U32_MAX;     // its word in the slot's buffer (none until a box draws)
        bool unmarked = false;  // a draw inside it could not mark: reads visible
    };
    extern const U32 sOcclusionMode;
    extern VisibilityBuffer sVisibility[LLVKMirror::FRAMES_IN_FLIGHT];
    extern GLuint sActiveQuery;
    extern U32 sQueriesIssued;
    extern U32 sQueriesUnmarked;
    GLQuery* gl_query(GLuint name);
    VisibilityBuffer* frame_visibility();
    void read_query(GLuint name, GLenum pname, GLuint* params);

    // The active query's word, given on its first box. U32_MAX: none to mark.
    inline U32 visibility_word()
    {
        GLQuery* q = gl_query(sActiveQuery);
        if (!q || q->frame != sFrame)
        {
            return U32_MAX;
        }
        if (q->word == U32_MAX)
        {
            VisibilityBuffer* v = frame_visibility();
            if (!v || v->used >= VISIBILITY_WORDS)
            {
                return U32_MAX;
            }
            q->slot = (U32)(v - sVisibility);
            q->word = v->used++;
            ++sQueriesIssued;
        }
        return q->word;
    }

    // ------------------------------------------------------------------
    // The depth-only path of the sun shadow casters (llvkgl_occlusion.cpp)

    extern U32 sDepthOnlyDraws;
    extern std::unordered_map<const char*, U32> sMarkFailures;
    extern std::unordered_map<const char*, U32> sDepthOnlyFailures;
    extern std::unordered_set<GLuint> sDepthOnlyPrograms;
    // glDeleteProgram: the path's lists by program name forget it (fix-pipeleak:
    // GL names are never used again, so every shader reload added to them).
    void forget_depth_only_program(GLuint id);
    bool is_depth_only_program(Program& program);
    bool draw_depth_only(Program& program, GLenum mode, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index,
                         U32 word = U32_MAX);

    // ------------------------------------------------------------------
    // Order-independent transparency sessions (llvkgl_oit.cpp): the open
    // session, and the draws into its target

    constexpr U32 OIT_SESSIONS_PER_FRAME = 8;

    struct OITImage
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };

    struct OITReadback
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        const U32* words = nullptr;     // OIT_SESSIONS_PER_FRAME entries of OIT_READBACK_BYTES
        U64 frame_begin = 0;            // LLVKMirror::frameBegins() of the frame that wrote them (0: none)
        U32 sessions = 0;               // entries that frame took
        bool copied[OIT_SESSIONS_PER_FRAME] = {};   // the entry's count copy was recorded (a dropped session has none)
        bool avatar_copied[OIT_SESSIONS_PER_FRAME] = {};    // hybrid: the avatar half's count was copied
        U32 avatar_draws[OIT_SESSIONS_PER_FRAME] = {};      // hybrid: the avatar half's node draws
    };

    struct OITState
    {
        bool tried = false;
        bool ready = false;
        // heads, nodes, rest, background (the copy, sampled), the target
        // (sampled), the copy (storage: the bounds pass's copy)
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout empty_layout = VK_NULL_HANDLE;    // set 1 of variants of shared-layout programs
        VkPipelineLayout resolve_layout = VK_NULL_HANDLE;
        LLVKShader::Program resolve_vertex;
        LLVKShader::Program resolve_fragment;
        VkPipelineLayout bounds_layout = VK_NULL_HANDLE;
        VkPipeline bounds = VK_NULL_HANDLE;     // the bounds pass without the copy
        std::unordered_map<U32, VkPipeline> bounds_copy;    // with it, by the copy's format (null: none)
        std::vector<LLVKShader::Program> bounds_programs;   // kept for their modules
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[LLVKMirror::FRAMES_IN_FLIGHT][OIT_SESSIONS_PER_FRAME] = {};
        OITReadback readback[LLVKMirror::FRAMES_IN_FLIGHT];
        std::unordered_map<std::vector<U32>, VkPipeline, KeyHash> resolve_pipelines;    // by the target's formats
        U32 max_nodes = 0;
        // Sized resources.
        U32 width = 0;
        U32 height = 0;
        OITImage heads;
        OITImage copy;
        OITImage rest;
        OITImage dummy_rest;            // 1x1, bound while there is no rest image (never read then)
        OITImage dummy_storage;         // 1x1, bound as the copy's storage when it has none (never written then)
        bool copy_storage = false;      // the copy can be written by the bounds pass
        bool rest_wanted = false;       // sessions ran out of nodes the buffer cannot grow to: make the rest image
        U32 overflow_need = 0;          // nodes the last session that ran out wanted
        VkBuffer nodes = VK_NULL_HANDLE;
        VmaAllocation nodes_memory = nullptr;
        U32 capacity = 0;               // nodes
        U32 wanted = 0;                 // nodes the last counts asked for
        // The open session.
        bool active = false;
        bool hybrid = false;            // LLRHI::sOITBegin's hybrid, for the whole session
        bool world = false;             // hybrid: oit_world came (the draws now are the world's)
        bool classic_world = false;     // hybrid: and no avatar draw made nodes: the world draws as before
        S32 logged_hybrid = -1;         // the mode last logged
        U32 frame = U32_MAX;
        Texture* color = nullptr;
        Texture* depth = nullptr;
        VkImage color_image = VK_NULL_HANDLE;
        VkImageView color_view = VK_NULL_HANDLE;   // the bounds pass samples it (the set's binding 4); null: it does not copy
        VkDescriptorSet set = VK_NULL_HANDLE;
        U32 slot = 0;
        U32 entry = 0;
        U32 node_draws = 0;             // the session's draws with an OIT variant
        F32 depth_a = 0.f;              // the session's projection (LLRHI::sOITBegin)
        F32 depth_b = 0.f;
        // Since the last perf report.
        U32 sessions = 0, refused = 0, dropped = 0, draws = 0, classic_draws = 0;
        U32 hybrid_sessions = 0, avatarless = 0, split_draws = 0;  // hybrid: sessions, those without avatar nodes, world draws split
        U32 set_binds = 0;              // set 2 binds (oit_bind_set)
        U32 empty_streak = 0;           // hybrid sessions in a row (as read back) whose avatar half appended nothing
        U32 empty_draws = 0;            // the most avatar node draws of those
        U32 peak = 0;                   // largest node count read back
        U32 overflows = 0;              // sessions whose count passed the capacity
        U32 grows = 0;
        // Programs whose variant was made / could not be.
        U32 variants = 0, variants_failed = 0;
    };
    extern OITState sOIT;
    OITVariant* oit_variant(GLuint id, Program& program);
    void oit_bind_set(VkCommandBuffer cmd, const Program& program, const OITVariant& v);

    // Whether a draw into the target now goes to the open session (oit_for_draw's test).
    inline bool oit_session_takes(const ResolvedTarget& target)
    {
        const State& s = sState;
        return sOIT.active && !sOIT.classic_world && sOIT.frame == sFrame && target.colors[0] == sOIT.color && target.depth == sOIT.depth &&
               target.target.color_count == 1 && !depth_only_pass() && !sActiveQuery &&
               (s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3]);
    }

    // A draw into the open session's target writes nodes (with the program's
    // variant; mode: OIT_DRAW_NODES, or OIT_DRAW_SPLIT for a hybrid session's
    // world draws). Null: it draws as before (no session, another target, a
    // draw writing no color, no variant, or a hybrid session's world draws
    // without avatar nodes).
    inline OITVariant* oit_for_draw(Program& program, const ResolvedTarget& target, U8& mode)
    {
        mode = OIT_DRAW_NONE;
        if (!oit_session_takes(target))
        {
            return nullptr;
        }
        OITVariant* v = oit_variant(sCurrentProgram, program);
        ++(v ? sOIT.draws : sOIT.classic_draws);
        sOIT.node_draws += v ? 1 : 0;
        if (v)
        {
            mode = sOIT.world ? OIT_DRAW_SPLIT : OIT_DRAW_NODES;
            sOIT.split_draws += sOIT.world ? 1 : 0;
        }
        return v;
    }

    // ------------------------------------------------------------------
    // The pass cache (research/39 N3, llvkgl_descriptors.cpp)

    struct PassCache
    {
        bool valid = false;
        // Key: what the full path's results were made from.
        const Program* program = nullptr;
        GLuint program_name = 0;
        U32 program_changes = 0;
        U32 frame = 0;
        U32 pipeline_state = 0;         // sPipelineState / sFixedState, with the state itself
        U32 fixed_state = 0;            // for when a change was undone (LLGLDisable around a draw)
        State state;
        GLuint framebuffer = 0;
        U32 target_state = 0;
        U32 target_stamp = 0;
        LLVKMirror::Target target;      // as resolved (the default target has no stamp)
        VkPrimitiveTopology prim = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        U64 inputs = 0;                 // vertex format (input signature)
        U32 uniform_version = 0;        // the default uniform block (differs: a uniform hit)
        U32 unit_version = 0;           // sampler units
        U32 unit_bindings = 0;          // bindings of the units sampled through descriptors
        // Textures, only with descriptor samplers: those the samplers read
        // (sampled_units_key), confirmed again only when some texture changed
        // or was deleted since (texture_state, texture_deletes).
        U64 units = 0;
        U32 texture_state = 0;
        U32 texture_deletes = 0;
        U32 uniform_blocks = 0;         // buffer-backed blocks and buffer deletions: only with such blocks
        U32 buffer_deletes = 0;
        U32 attrib_constants = 0;
        U32 layouts = 0;
        U32 scope_epoch = 0;            // LLVKMirror rendering scopes
        U32 record_epoch = 0;
        U32 bindless_moves = 0;
        U8 oit = 0;                     // the draw's OITDrawMode (research/41): its pipeline is the variant's
        // Results: what the draw had bound.
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool has_dynamic = false;
        VkBuffer dynamic_buffer = VK_NULL_HANDLE;  // the buffer the set's default block descriptor reads
        U32 dynamic_offset = 0;         // the default block's copy (moved by uniform hits)
        bool depth_read_only = false;   // the target as the draw's images needed it
    };
    enum PassHit { PASS_MISS, PASS_HIT, PASS_HIT_UNIFORMS };
    // research/137 N7: draw_one's pass cache hit in two halves (research/140
    // 7-4). pass_hit_resolve (after pass_hit gave a hit, tDirect's inputs
    // resolved): the draw's slots and its default block copy; out gets what
    // the record binds. pass_hit_record: the commands, from out and the slots
    // and buffers the resolve left in tSlots / tVertexBuffers; takes no lock
    // (the caller holds sLock today), but still reads sRecorded, sState
    // (dynamic state), the open run (run_draw) and sOIT: a recording on
    // another thread needs those per stream (research/137 N11).
    enum PassHitResult
    {
        PASS_HIT_RECORD,        // record it with pass_hit_record
        PASS_HIT_FULL_PATH,     // not usable as it is: the full path draws it
        PASS_HIT_DROP,          // not drawn (the palette table is full; skipped and counted)
    };
    struct PassHitDraw
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool has_dynamic = false;
        U32 dynamic_offset = 0;
        bool depth_read_only = false;
    };
    PassHitResult pass_hit_resolve(Program& program, ResolvedTarget& target, PassHit hit, PassHitDraw& out);
    void pass_hit_record(Program& program, ResolvedTarget& target, const PassHitDraw& h, const OITVariant* oit, U8 oit_mode,
                         VkPrimitiveTopology prim, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index,
                         VkBuffer index_buffer, VkIndexType index_type);
    extern const bool sPassCacheEnabled;
    extern U32 sPassHits, sPassMisses;
    extern U32 sPassUniformHits;
    extern U32 sPassRebinds;
    extern U32 sPassUnitsKept;
    // Why pass_hit missed (research/58; counted for every miss since
    // research/155 T0b): its first failing test, which pass_hit hands back.
    enum PassMissWhy
    {
        MISS_NO_ENTRY, MISS_PROGRAM, MISS_FRAME, MISS_SHAPE, MISS_TARGET, MISS_ATTRIB_CONSTANTS, MISS_LAYOUTS, MISS_SCOPE,
        MISS_RECORD, MISS_BINDLESS_MOVES, MISS_NOT_RENDERING, MISS_OIT, MISS_UNITS, MISS_BLOCKS, MISS_TEXTURES, MISS_STATE,
        MISS_UNUSABLE, MISS_WHY_COUNT
    };
    PassHit pass_hit(Program& program, const ResolvedTarget& target, VkPrimitiveTopology prim, U64 inputs, PassMissWhy* why = nullptr);
    void pass_store(Program& program, const ResolvedTarget& target, VkPrimitiveTopology prim, U64 inputs, VkPipeline pipeline,
                    VkDescriptorSet set);
    extern U32 sPassMissWhy[MISS_WHY_COUNT];
    // research/156 A-DS (llvkgl_descriptors.cpp): descriptor sets kept across frames.
    extern const bool sSetPersist;
    extern U32 sPersistGeneration;
    extern U32 sPersistHits, sPersistNew, sPersistRetires;
    VkDescriptorSet persistent_set(Program& program);
    std::string pass_miss_report(U32 frames);
    // research/155 T0b: every reason with its count per frame, zeros too, in
    // PassMissWhy order (the VKPerf line's counters keep their columns).
    std::string pass_miss_columns(U32 frames);

    // ------------------------------------------------------------------
    // Draw runs, draw templates and the probe list's capture (llvkgl_runs.cpp)

    // Capture (research/50 step 4): the GPU probe list registers a group's
    // batches of a render type by having the viewer draw them the way their
    // pool does while this is on. Nothing is recorded; each direct draw leaves
    // what it would have drawn from and pushed (capture_draw), any other draw
    // an entry that is not ok.
    struct CaptureState
    {
        bool on = false;
        std::vector<LLVKGLProbe::Capture> draws;
    };
    extern CaptureState sCapture;

    struct DrawRun
    {
        bool open = false;
        U32 frame = U32_MAX;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        const Program* program = nullptr;
        U32 program_changes = 0;
        U32 scope_epoch = 0;
        U32 record_epoch = 0;
        U32 resets = 0;
        U64 commands = 0;           // run_commands() after the run's last draw
        U32 chunk = 0;
        bool rows = false;          // the draws' words are rows (else the pushed words below)
        U32 slots[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 slot_count = 0;
        VkDrawIndexedIndirectCommand* args = nullptr;   // in the frame's ring
        U32* count = nullptr;
        U32 capacity = 0;
        U32 used = 0;
        // What the run's last draw was recorded with (run_continue): its target
        // and the change counters of everything its recording depended on.
        bool fast = false;
        // tOITDraw of the run's last draw. A split draw (OIT_DRAW_SPLIT) takes
        // its OIT priority from oit_bind_set, which only the usual way calls:
        // run_unchanged lets no draw continue such a run (run_continue and
        // draw_template run with tOITDraw 0).
        U8 oit_mode = OIT_DRAW_NONE;
        ResolvedTarget target;
        GLuint program_name = 0;
        U32 pipeline_state = 0;
        U32 fixed_state = 0;
        U32 target_state = 0;
        GLuint framebuffer = 0;
        U32 attrib_constants = 0;
        U32 layouts = 0;
        U32 bindless_moves = 0;
        U32 uniform_version = 0;
        U32 texture_state = 0;
        U32 texture_deletes = 0;
        PassVersions versions = {};
        U64 format = 0;
        U64 inputs = 0;             // the input signature (direct_inputs) the pass cache keys on
        // run_pass_hit found the pass cache hit binding nothing since the
        // run's last draw through the usual way (research/46 G4c): while the
        // state above stays, the next draws need not ask again.
        bool pass_ok = false;
        // Research/50 step 4 (kept only while device generated commands may
        // draw after the run): the run's vertex inputs as its pipeline has
        // them, the buffers it bound per input, and per input the pool stream
        // it reads (location -1: a constant).
        bool generated = false;
        std::vector<VertexInput> vertex_inputs;
        std::vector<VkBuffer> buffers;
        std::vector<VkDeviceSize> offsets;
        std::vector<S32> stream_location;
        std::vector<U32> stream_stride;
    };
    extern DrawRun sRun;
    // The indirect commands a new run reserves: Program::run_capacity follows
    // how long the program's runs get, within these (run_draw, and a draw
    // list's segments recorded on a cull thread, llvkdrawlist.cpp).
    constexpr U32 RUN_MIN_CAPACITY = 16;
    constexpr U32 RUN_MAX_CAPACITY = 4096;
    extern U32 sDispatchResets;
    extern U32 sRunsOpened, sRunDraws, sRunRows, sRunRowsFull, sRunFast;
    extern U32 sGeneratedExecutes;

    // Why run_draw sent a direct draw the usual way (perfReport; a full table
    // is sRunRowsFull). Not counted: draws while runs are off, and the
    // join-only tries (run_continue, draw_template), which fall back to a
    // full run_draw that counts.
    enum RunRefusal
    {
        RUN_UNMARKED,       // the program has no draw runs mark
        RUN_NOT_DIRECT,     // a marked program drawn without a DirectDraw
        RUN_SHAPE,          // not one indexed triangle list instance (or a light table draw)
        RUN_UNSIZED,        // the DirectDraw does not give its whole buffers (buffer_vertices)
        RUN_SLOTS,          // push constants other than the rows rewrite expects
        RUN_POOL,           // the attribute pool did not place the buffers (yet)
        RUN_INPUTS,         // vertex inputs or a pool stream missing
        RUN_RING,           // no index buffer, indirect count or ring space
        RUN_REFUSAL_COUNT
    };
    extern U32 sRunRefused[RUN_REFUSAL_COUNT];
    std::string run_refusals(U32 frames);

    struct DrawTemplate
    {
        U32* owner = nullptr;           // the slot that names it; null: free
        const Program* program = nullptr;
        U32 program_changes = 0;
        U64 format = 0;                 // run_format of the draw it was made from
        LLVKAttribPool::Ticket ticket;
        GLuint vertex_buffer = 0;
        GLuint index_buffer = 0;
        U32 index_type = 0;
        U32 buffer_vertices = 0;
        U32 buffer_indices = 0;
    };
    extern std::vector<DrawTemplate>& sTemplates;
    extern std::vector<U32>& sTemplateFree;
    extern U32 sTemplateDraws, sTemplateRefused, sTemplatesMade, sTemplatesFull;

    // The FNV-1a of a direct draw's attributes without their offsets (direct_inputs' key; run_format's with the count).
    U64 direct_attributes_hash(const LLRHI::DirectDraw& d);
    U64 run_format(const LLRHI::DirectDraw& d);
    bool run_draw(VkCommandBuffer cmd, Program& program, const ResolvedTarget& target, VkPrimitiveTopology prim, GLsizei count,
                  GLsizei instances, const IndexInfo& index, bool join_only);
    bool run_continue(Program& program, const LLRHI::DirectDraw& d);
    bool draw_template(const LLRHI::DirectDraw& d);
    void draw_template_released(U32 id, const U32* slot);
    // The pieces of a run's next draw (llvkgl_runs.cpp), shared with the draw
    // packets (llvkgl_packet.cpp): the commands recorded so far, whether the
    // open run may take a next draw of `program` (state, then pass cache),
    // the draw's row of tSlots (false: the table's share is spent) and its
    // indirect command (room checked by the caller).
    U64 run_commands();
    bool run_unchanged(Program& program);
    bool run_pass_hit(Program& program);
    bool run_row(U32& first_instance);
    void run_push(U32 index_count, U32 first_index, S32 vertex_offset, U32 first_instance);

    // ------------------------------------------------------------------
    // Draw packets (research/180 stage 4 P1, llvkgl_packet.cpp)

    // A batch's record beside its draw template (same id, sTemplates[id - 1]):
    // what the usual way resolved when it last drew the batch with a
    // LLRHI::PacketNote. Its textures by unit (the packet's units, in order):
    // the name, the bindless slot the draw took (PACKET_NO_SLOT: no sampler
    // reads the unit) and the publication's generation of the name then; its
    // uniform values as a patch (sPatches, interned by content, permanent).
    // ---- research/180 stage 4: the packet part (tools/selftest sim_packet extracts it) ----
    constexpr U32 PACKET_NO_SLOT = U32_MAX;
    struct PacketPart
    {
        bool live = false;
        const Program* program = nullptr;   // the program the record was made under (its link)
        U32 program_changes = 0;
        U8 unit_count = 0;
        U8 units[LLRHI::MAX_PACKET_UNITS] = {};
        GLuint names[LLRHI::MAX_PACKET_UNITS] = {};
        U32 slots[LLRHI::MAX_PACKET_UNITS] = {};
        U32 gens[LLRHI::MAX_PACKET_UNITS] = {};
        U32 patch = U32_MAX;    // U32_MAX: none (the program sets none of the uniforms)
    };
    extern std::vector<PacketPart>& sPacketParts;   // grows with sTemplates, never shrinks
    // The template `id` is made again for another batch, or released: its record goes.
    inline void packet_part_reset(U32 id)
    {
        if (id && id <= sPacketParts.size())
        {
            sPacketParts[id - 1] = PacketPart();
        }
    }
    // ---- end of the packet part ----
    // LLRHI::tPacketNote is set: the draw `d` of `program` just joined the
    // open run (draw_template or run_draw, tSlots its words): when it is the
    // note's batch, verify compares its record with what the draw resolved,
    // then the record is written.
    void packet_note(const Program& program, const LLRHI::DirectDraw& d);
    // LLRHI::sPacketDraw.
    U32 packet_draw(LLRHI::Packet& packet);
    // ", packets [...]" for the VKPerf counters line (empty while packets never ran), and the reset.
    std::string packet_report(U32 frames);
    void packet_reset_counters();
    // A material row of these contents (llvkgl_descriptors.cpp; U32_MAX: the table is full).
    U32 material_row_content(const F32* data, U32 vec4s);
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER

#endif // LL_LLVKGL_DRAWPATH_H
