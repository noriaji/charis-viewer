/**
 * @file llvkgl_scene.cpp
 * @brief The retained scene's binder (research/198 5.5): the bound program's pass, bound for the scene's indirect draws.
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
#include "llrhiframe.h"     // research/208 S1: LLRHI_FRAME_SNAPSHOT
#include "llvkattribpool.h"
#include "llvkscene.h"
#include "llvktranslate.h"
#include "llrhipassstate.h"  // research/204 10 (vk-s4c2): a declared pass's head
#include "llvkpassstate.h"

#include <memory>
#include <sstream>
#include <unordered_map>

#if LL_GL_FUNC_POINTER

// LLVKScene::Binder for the translation layer. The scene draws a pass's
// records with the program the draw pool bound, in the target and state the
// pool set, exactly as a draw run of that program (research/46 G4b): the
// program's draw-rows build reads every word a draw pushes from the row its
// first instance names, so one indirect draw per pool chunk draws records
// of any texture and transform. What a draw of the pass would push when it
// names no texture of its own (gather_slots with a draw of the layout and no
// textures or matrices: each sampler as its unit is bound, the program's
// matrix, material and constants rows) is the row's template; the scene puts
// each record's texture slots and matrix row in place (llvkscene.cpp, the
// expand; research/198 5.6: and its own material row when the pool sets
// material uniforms per batch, programTable). Binding a chunk is draw_into_target's half before the draw, with
// the pool's streams as run_draw binds them, the rows word pushed as 0 ("read
// the rows"), and sRecorded kept as those keep it, so the draws after the
// scene's bind what they need again.
// research/205 3a-3: inside an OIT session (the alpha pool's "all" session)
// a pass draws with its program's OIT variant as a node draw of
// draw_into_target would (its pipeline, set 2, no depth writes, counted as
// the session's node draws), when the variant reads each draw's priority from
// the draw's row (llvkgl_oitshaders.cpp 3a-2): the expand writes it there.
// research/205 3a-5: a hybrid session's world draws too (split draws), and
// a sorted pass's textures are settled when it is expanded (settleTextures),
// its spans being drawn later, each under a beginPass of its own.
// research/208 S1 (stage 4a): the pass's head as values. beginPass reads
// what the binder needs of the main thread's GL context once, at the pass's
// entry (the program, the state its pipeline and dynamic state come from, the
// draw target, the units' words and descriptors, the default block's place,
// the vertex inputs, the OIT mode) into a PassHead. On the head's way
// (LLRHI_FRAME_SNAPSHOT on, or auto once 600 frames of its checks agreed)
// admit makes what a chunk's bind could fail at before anything of the pass
// is drawn (the pipeline, one descriptor set for the pass's chunks), and
// bindChunk reads only the head and the pool's chunk, never the context: a
// render thread could bind what the main thread entered. verify (and auto
// before it is on) makes the head and compares it, once a pass, with what the
// context gives then, the context's way drawing. A texture of the pass's
// words that textureSlot moves to another slot makes the head stale (the
// expand's rows hold the old words; the context's way refuses such a pass too).
namespace llvkgl
{
namespace
{
    // tOITDraw for a chunk's binding (research/205 3a-3), as OITDrawScope for a draw.
    struct OITModeScope
    {
        explicit OITModeScope(U8 mode) { tOITDraw = mode; }
        ~OITModeScope() { tOITDraw = 0; }
        OITModeScope(const OITModeScope&) = delete;
        OITModeScope& operator=(const OITModeScope&) = delete;
    };

    // tDirectVertexBuffer for a scope: direct_inputs wants a buffer for the
    // layout's attributes (the scene binds the pool's streams in their place).
    struct DirectBufferScope
    {
        explicit DirectBufferScope(VkBuffer buffer) : saved(tDirectVertexBuffer) { tDirectVertexBuffer = buffer; }
        ~DirectBufferScope() { tDirectVertexBuffer = saved; }
        DirectBufferScope(const DirectBufferScope&) = delete;
        DirectBufferScope& operator=(const DirectBufferScope&) = delete;
        const VkBuffer saved;
    };

    // research/198 5.6 (stage 2d): what the scene needs of a program to give
    // each record its own words, kept per program (a pass's, or the alpha
    // pool's several, research/205 6) until it is linked again: per word the
    // texture unit of its 2D sampler, and where the material uniforms the
    // viewer names for its draws are in the program's material row.
    struct ProgramTable
    {
        U32 link_serial = U32_MAX;
        const LLRHI::SceneMaterialUniform* materials = nullptr;     // what it was made for (the viewer's static lists)
        U32 material_count = 0;
        const char* why = nullptr;          // not null: a record's draw cannot be made of the program's rows (for good)
        U32 material_word = U32_MAX;        // the material row's word, when a named uniform is in the row
        S16 material_at[LLRHI::SceneMaterialDesc::VALUES];
        S32 units[LLVKScene::Binder::Words::MAX];
    };

    // research/208 S1: what a chunk's bind takes of the main thread's GL
    // context, read at the pass's entry (beginPass). Pointers only to the
    // backend's own objects (the program, textures, the OIT variant), which
    // live while the frame is recorded; the descriptor writes point into the
    // head's own copies of their infos. The vectors keep their storage from
    // pass to pass.
    struct PassHead
    {
        bool valid = false;
        Program* program = nullptr;
        GLuint program_name = 0;
        U32 program_changes = 0;
        U32 link_serial = 0;
        State state;                    // the pipeline's key takes its blend, masks, polygon mode, depth clamp
        ResolvedTarget target;          // as gather_descriptors left it (a sampled depth attached read-only)
        DynamicValues dynamic;          // dynamic_values for the target, triangles, with the pass's OIT mode
        const OITVariant* oit = nullptr;
        U8 oit_mode = OIT_DRAW_NONE;
        U32 words[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 word_count = 0;             // the rows word last
        U32 sampler_words = 0;          // the rewritten 2D samplers' words (the first ones)
        std::vector<VkDescriptorBufferInfo> buffers;
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<Texture*> sampled;              // through descriptors
        std::vector<Texture*> bindless_sampled;     // through the bindless table (the units' words)
        bool has_dynamic = false;
        U32 dynamic_offset = 0;
        std::vector<VertexInput> inputs;
        std::vector<S32> source;                    // the layout's attribute, -1: a constant
        std::vector<VkBuffer> constant_buffers;
        std::vector<VkDeviceSize> constant_offsets;
        U64 input_signature = 0;
        // admit's.
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;       // null with the shared layout
        bool admitted = false;
        bool stale = false;             // textureSlot moved a texture of the units' words
    };

    // research/208 S2: a pass's indirect draws as values (bindHead's binds
    // after its first half, and drawPass's draws), recorded on the render
    // thread (or here) from a Recorded of its own: nothing is known of what is
    // bound where its stream is spliced, so its first bucket binds it all, as
    // a draw list's segment does (llvkdrawlist.cpp). bind_program_set (the
    // values' overload) and record_dynamic_values read only their arguments.
    // research/208 S3: also the render thread's draws of a pass it runs whole
    // (LLVKScene::ThreadDraws): beginThreadDraws makes the values and, for
    // every chunk of the pool, its index buffer and the layout's streams as
    // bindHead would find them at the entry; the thread adds the buckets its
    // expand made and records. Held (mHeld) until its taker releases it.
    class SceneDrawJob final : public LLVKRender::Job, public LLVKScene::ThreadDraws
    {
    public:
        struct Bucket
        {
            VkBuffer index_buffer = VK_NULL_HANDLE;
            U32 first_input = 0;                    // its vertex inputs in buffers / offsets
            VkBuffer commands = VK_NULL_HANDLE;
            VkDeviceSize commands_offset = 0;
            VkBuffer counts = VK_NULL_HANDLE;
            VkDeviceSize counts_offset = 0;
            U32 max_count = 0;
            U32 stride = 0;
        };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet table = VK_NULL_HANDLE;     // the bindless table (set 0)
        VkDescriptorSet set = VK_NULL_HANDLE;       // the program's (null: the shared layout)
        bool has_dynamic = false;
        U32 dynamic_offset = 0;
        DynamicValues dynamic;
        VkShaderStageFlags push_stages = 0;
        U32 words[LLVKBindless::MAX_PUSHED_SLOTS] = {};     // the rows word last (pushed as 0: "read the rows")
        U32 word_count = 0;
        U32 input_count = 0;
        std::vector<VkBuffer> buffers;              // input_count per bucket (kept from pass to pass)
        std::vector<VkDeviceSize> offsets;
        std::vector<Bucket> buckets;
        // research/208 S3 (beginThreadDraws): per chunk its index buffer, and
        // its input_count buffers (a stream, or the head's constant); the
        // inputs' offsets (a stream's streamShift, or the constant's). Kept from pass to pass.
        U32 chunk_count = 0;
        std::vector<VkBuffer> chunk_index;
        std::vector<VkBuffer> chunk_inputs;
        std::vector<VkDeviceSize> input_offsets;
        std::atomic<bool> mHeld{ false };
        LLVKRender::SecondaryTarget formats;        // research/208 13.3: the entry's rendering scope's
        U32 mRecorded = 0;

        void record() override { mRecorded = recordInto(cmd); }
        // The binds and the draws into `into`; the commands recorded.
        U32 recordInto(VkCommandBuffer into);
        U64 digest() const override;
        const char* what() const override { return "draws"; }
        // research/208 S3: LLVKScene::ThreadDraws (values only).
        bool addBucket(U32 chunk, VkBuffer commands, VkDeviceSize offset, VkBuffer counts, VkDeviceSize count_offset, U32 max_count,
                       U32 stride) override;
        void recordDraws() override { mRecorded = recordInto(cmd); }
        VkCommandBuffer recordSecondary(U32 frame_slot, U64 frame_begins) override;
        U32 recordedCommands() const override { return mRecorded; }
        U64 entryDigest() const override;
        void release() override { mHeld.store(false, std::memory_order_release); }
    };

    bool SceneDrawJob::addBucket(U32 chunk, VkBuffer commands, VkDeviceSize offset, VkBuffer counts, VkDeviceSize count_offset,
                                 U32 max_count, U32 stride)
    {
        if (chunk >= chunk_count || !chunk_index[chunk])
        {
            return false;
        }
        const VkBuffer* inputs = chunk_inputs.data() + (size_t)chunk * input_count;
        for (U32 i = 0; i < input_count; ++i)
        {
            if (!inputs[i])
            {
                return false;
            }
        }
        if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only (a bucket that was taken)
        {
            LLRHIVBCensus::count(LLRHIVBCensus::C_SCENE_BUCKETS);
        }
        Bucket b;
        b.index_buffer = chunk_index[chunk];
        b.first_input = (U32)buffers.size();
        buffers.insert(buffers.end(), inputs, inputs + input_count);
        offsets.insert(offsets.end(), input_offsets.begin(), input_offsets.end());
        b.commands = commands;
        b.commands_offset = offset;
        b.counts = counts;
        b.counts_offset = count_offset;
        b.max_count = max_count;
        b.stride = stride;
        buckets.push_back(b);
        return true;
    }

    U64 SceneDrawJob::entryDigest() const
    {
        LLVKRender::Digest d;
        d << digest() << chunk_count;
        d.bytes(chunk_index.data(), chunk_index.size() * sizeof(VkBuffer));
        d.bytes(chunk_inputs.data(), chunk_inputs.size() * sizeof(VkBuffer));
        d.bytes(input_offsets.data(), input_offsets.size() * sizeof(VkDeviceSize));
        return d.h;
    }

    // The commands counted as they are recorded (research/208 13.3: verify compares both ways').
    U32 SceneDrawJob::recordInto(VkCommandBuffer into)
    {
        U32 commands = 0;
        Recorded r;     // its vectors stay empty: the inputs are compared with the bucket before
        vkCmdBindPipeline(into, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bind_program_set(into, r, layout, table, set, has_dynamic, dynamic_offset);
        record_dynamic_values(into, r, dynamic);
        U32 pushed[LLVKBindless::MAX_PUSHED_SLOTS];
        memcpy(pushed, words, word_count * 4);
        pushed[word_count - 1] = 0;
        vkCmdPushConstants(into, layout, push_stages, 0, word_count * 4, pushed);
        commands += 2;
        const Bucket* before = nullptr;
        for (const Bucket& b : buckets)
        {
            const VkBuffer* inputs = buffers.data() + b.first_input;
            const VkDeviceSize* at = offsets.data() + b.first_input;
            if (input_count && (!before || memcmp(inputs, buffers.data() + before->first_input, input_count * sizeof(VkBuffer)) ||
                                memcmp(at, offsets.data() + before->first_input, input_count * sizeof(VkDeviceSize))))
            {
                vkCmdBindVertexBuffers(into, 0, input_count, inputs, at);
                ++commands;
            }
            if (!before || b.index_buffer != before->index_buffer)
            {
                vkCmdBindIndexBuffer(into, b.index_buffer, 0, VK_INDEX_TYPE_UINT32);
                ++commands;
            }
            vkCmdDrawIndexedIndirectCount(into, b.commands, b.commands_offset, b.counts, b.counts_offset, b.max_count, b.stride);
            ++commands;
            before = &b;
        }
        return commands;
    }

    // research/208 13.3 (vk-s4a4): this thread's secondary command buffer for
    // the entry's scope, the same commands recorded straight into it.
    VkCommandBuffer SceneDrawJob::recordSecondary(U32 frame_slot, U64 frame_begins)
    {
        const VkCommandBuffer secondary = LLVKRender::beginSecondary(formats, frame_slot, frame_begins);
        if (!secondary)
        {
            return VK_NULL_HANDLE;
        }
        mRecorded = recordInto(secondary);
        return LLVKRender::endSecondary(secondary) ? secondary : VK_NULL_HANDLE;
    }

    // research/208 13.3: what a secondary command buffer inherits of a scope
    // LLVKMirror::beginRendering opens on target (its stencil attachment only
    // with target.stencil).
    LLVKRender::SecondaryTarget secondary_formats(const LLVKMirror::Target& target)
    {
        LLVKRender::SecondaryTarget out;
        out.color_count = llmin(target.color_count, 4u);
        for (U32 i = 0; i < out.color_count; ++i)
        {
            out.colors[i] = target.colors[i] ? target.color_formats[i] : VK_FORMAT_UNDEFINED;
        }
        out.depth = target.depth ? target.depth_format : VK_FORMAT_UNDEFINED;
        out.stencil = target.depth && target.stencil ? target.depth_format : VK_FORMAT_UNDEFINED;
        return out;
    }

    U64 SceneDrawJob::digest() const
    {
        LLVKRender::Digest d;
        d << cmd << pipeline << layout << table << set << (U8)has_dynamic << dynamic_offset;
        const DynamicValues& v = dynamic;
        d << v.viewport.x << v.viewport.y << v.viewport.width << v.viewport.height << v.viewport.minDepth << v.viewport.maxDepth;
        d << v.scissor.offset.x << v.scissor.offset.y << v.scissor.extent.width << v.scissor.extent.height;
        d << v.cull << v.front << v.prim << v.depth_test << v.depth_write << v.bias_enable << v.stencil_test << v.depth_op;
        d << v.bias_units << v.bias_factor << v.bias_clamp;
        d.bytes(v.stencil, sizeof(v.stencil));
        d << push_stages << word_count;
        d.bytes(words, word_count * 4);
        d << input_count << (U64)buckets.size();
        for (const Bucket& b : buckets)
        {
            d << b.index_buffer << b.first_input << b.commands << b.commands_offset << b.counts << b.counts_offset << b.max_count << b.stride;
        }
        d.bytes(buffers.data(), buffers.size() * sizeof(VkBuffer));
        d.bytes(offsets.data(), offsets.size() * sizeof(VkDeviceSize));
        return d.h;
    }
    constexpr size_t MAX_DRAW_JOBS = 256;   // a frame's (past them: bound chunk by chunk as before)

    // research/208 S1: the head's counters since the last report.
    struct HeadCounts
    {
        U32 heads = 0;          // made
        U32 not_made = 0;       // could not be made (its vertex inputs)
        U32 compared = 0;       // passes compared with the context (verify)
        U32 differ = 0;
        U32 stale = 0;          // heads made stale by a moved texture
        U32 admitted = 0;
        U32 refused = 0;        // admit said no (the pass drawn the usual way at its entry)
        U32 bound = 0;          // chunks bound from a head
        U32 rt_failures = 0;    // a bind after the admission failed
    };
    constexpr U32 HEAD_AUTO_FRAMES = 600;   // auto: frames of compared passes without a difference
    constexpr U32 HEAD_DIFFS_KEPT = 8;

    // The writes and the infos they point at, as the head's own.
    void copy_writes(const std::vector<VkWriteDescriptorSet>& from, PassHead& h)
    {
        h.buffers.clear();
        h.images.clear();
        h.writes.clear();
        for (const VkWriteDescriptorSet& w : from)
        {
            if (w.pBufferInfo)
            {
                h.buffers.insert(h.buffers.end(), w.pBufferInfo, w.pBufferInfo + w.descriptorCount);
            }
            if (w.pImageInfo)
            {
                h.images.insert(h.images.end(), w.pImageInfo, w.pImageInfo + w.descriptorCount);
            }
            h.writes.push_back(w);
        }
        size_t b = 0;
        size_t i = 0;
        for (VkWriteDescriptorSet& w : h.writes)
        {
            if (w.pBufferInfo)
            {
                w.pBufferInfo = h.buffers.data() + b;
                b += w.descriptorCount;
            }
            if (w.pImageInfo)
            {
                w.pImageInfo = h.images.data() + i;
                i += w.descriptorCount;
            }
        }
    }

    // The same descriptor contents (bindings, types, and what each names).
    bool same_writes(const std::vector<VkWriteDescriptorSet>& a, const std::vector<VkWriteDescriptorSet>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t k = 0; k < a.size(); ++k)
        {
            const VkWriteDescriptorSet& x = a[k];
            const VkWriteDescriptorSet& y = b[k];
            if (x.dstBinding != y.dstBinding || x.descriptorCount != y.descriptorCount || x.descriptorType != y.descriptorType ||
                !x.pBufferInfo != !y.pBufferInfo || !x.pImageInfo != !y.pImageInfo)
            {
                return false;
            }
            for (U32 e = 0; e < x.descriptorCount; ++e)
            {
                if (x.pBufferInfo && (x.pBufferInfo[e].buffer != y.pBufferInfo[e].buffer || x.pBufferInfo[e].offset != y.pBufferInfo[e].offset ||
                                      x.pBufferInfo[e].range != y.pBufferInfo[e].range))
                {
                    return false;
                }
                if (x.pImageInfo && (x.pImageInfo[e].sampler != y.pImageInfo[e].sampler ||
                                     x.pImageInfo[e].imageView != y.pImageInfo[e].imageView ||
                                     x.pImageInfo[e].imageLayout != y.pImageInfo[e].imageLayout))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // What of the GL state get_pipeline's key takes (the rest is dynamic state).
    bool same_pipeline_state(const State& a, const State& b)
    {
        return a.polygon_mode == b.polygon_mode && a.blend == b.blend && a.blend_src_rgb == b.blend_src_rgb &&
               a.blend_dst_rgb == b.blend_dst_rgb && a.blend_src_a == b.blend_src_a && a.blend_dst_a == b.blend_dst_a &&
               a.blend_eq_rgb == b.blend_eq_rgb && a.blend_eq_a == b.blend_eq_a && a.color_mask[0] == b.color_mask[0] &&
               a.color_mask[1] == b.color_mask[1] && a.color_mask[2] == b.color_mask[2] && a.color_mask[3] == b.color_mask[3] &&
               a.depth_clamp == b.depth_clamp;
    }

    bool same_target(const ResolvedTarget& a, const ResolvedTarget& b)
    {
        if (a.target != b.target || a.stamp != b.stamp || a.color_textures != b.color_textures || a.depth != b.depth ||
            a.is_default != b.is_default)
        {
            return false;
        }
        for (U32 i = 0; i < a.color_textures && i < 4; ++i)
        {
            if (a.colors[i] != b.colors[i])
            {
                return false;
            }
        }
        return true;
    }

    class SceneBinder final : public LLVKScene::Binder
    {
    public:
        bool beginPass(const LLRHI::DirectDraw::Attribute* attributes, U32 attribute_count,
                       const LLRHI::SceneMaterialUniform* materials, U32 material_count, Words& out, const char*& why,
                       bool& permanent) override;
        U32 textureSlot(U32 ref) override;
        void texturesDone() override;
        VkCommandBuffer bindChunk(U32 chunk) override;
        bool chunkBindable(U32 chunk) override;
        void settleTextures() override;
        void endPass() override;
        bool headWay() const override { return mOpen && mHeadWay; }
        bool admit() override;
        // research/208 S2.
        bool beginDraws(LLVKRender::Use use, U32 pass) override;
        bool addDraw(U32 chunk, VkBuffer commands, VkDeviceSize offset, VkBuffer counts, VkDeviceSize count_offset, U32 max_count,
                     U32 stride) override;
        bool endDraws() override;
        void abortDraws() override;
        LLVKScene::ThreadDraws* beginThreadDraws(U32 pass, bool secondary) override;    // research/208 S3 (13.3: secondary)
        std::string takeReport(U32 frames) override;

    private:
        bool gather(Program& program);
        const ProgramTable& programTable(const Program& program, const LLRHI::SceneMaterialUniform* materials, U32 material_count);
        // research/208 S1.
        bool makeHead(Program& program);
        VkCommandBuffer bindHead(U32 chunk);
        void compareHead();
        void headDiffers(const char* what);
        void movedSlot(U32 slot);
        void rtFailure();
        bool headBindable() const;
        void headBarriers(const PassHead& h);
        SceneDrawJob* freeDrawJob();

        std::unordered_map<const Program*, ProgramTable> mTables;

        bool mOpen = false;
        bool mOIT = false;              // research/205 3a-3: the pass draws into an OIT session
        bool mOITSplit = false;         // research/205 3a-5: ... as a hybrid session's world (split draws)
        Program* mProgram = nullptr;
        GLuint mProgramName = 0;
        U32 mProgramChanges = 0;
        ResolvedTarget mTarget;
        LLRHI::DirectDraw mLayout;
        LLRHI::DirectDraw::Attribute mAttributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        U32 mWords[LLVKBindless::MAX_PUSHED_SLOTS] = {};
        U32 mWordCount = 0;
        bool mSlotsUsed = false;
        std::vector<Texture*> mTransitions;     // the records' textures not in shader read layout
        std::vector<VkBuffer> mBuffers;
        std::vector<VkDeviceSize> mOffsets;

        // research/208 S1: the pass head.
        PassHead mHead;
        const OITVariant* mOITVariant = nullptr;    // beginPass's variant for an OIT session's pass
        bool mHeadWay = false;                  // this pass binds from its head (latched at beginPass)
        bool mHeadVerify = false;               // this pass's head is compared with the context
        bool mHeadCompared = false;             // ... and was
        bool mAutoOn = false;                   // auto: the checks agreed long enough
        U32 mCompareFrame = U32_MAX;            // the frame of the last compared pass
        bool mFrameDiffered = false;
        U32 mCleanFrames = 0;
        bool mDiffLogged = false;
        HeadCounts mCounts;
        const char* mDiffWhat[HEAD_DIFFS_KEPT] = {};
        GLuint mDiffProgram[HEAD_DIFFS_KEPT] = {};
        U32 mDiffKept = 0;

        // research/208 S2: the draws jobs (each free again after the render thread's join) and the open one.
        std::vector<std::unique_ptr<SceneDrawJob>> mDrawJobs;
        size_t mDrawJobNext = 0;
        SceneDrawJob* mDraws = nullptr;
        LLVKRender::Use mDrawsUse = LLVKRender::Use::Inline;
    };

    SceneBinder sSceneBinder;

    // The pass's descriptors and words for a draw of the layout (tDirect),
    // as draw_into_target gathers them.
    bool SceneBinder::gather(Program& program)
    {
        DirectScope scope(&mLayout);
        if (!gather_descriptors(program, mTarget))
        {
            return false;
        }
        return !tPaletteFull;
    }

    // The program's table (made again after a link, or for another list of
    // material uniforms). A word binds a record's texture only if its sampler
    // is a 2D one (research/205 6, research/195 2.1: a sampler GL never gave
    // a unit stays at unit 0, the diffuse's; the bindless words are the
    // rewritten sampler2D, and a sampler of the same name bound another way
    // keeps the pass's binding). A material uniform the viewer names is
    // found in the program's material row by name; one the program reads
    // from anywhere else (its default block, the constants row: material
    // table off, or its rewrite failed) cannot differ per record.
    const ProgramTable& SceneBinder::programTable(const Program& program, const LLRHI::SceneMaterialUniform* materials,
                                                  U32 material_count)
    {
        ProgramTable& t = mTables[&program];
        if (t.link_serial == program.link_serial && t.materials == materials && t.material_count == material_count)
        {
            return t;
        }
        t = ProgramTable();
        t.link_serial = program.link_serial;
        t.materials = materials;
        t.material_count = material_count;
        for (S16& at : t.material_at)
        {
            at = -1;
        }
        for (U32 w = 0; w < LLVKScene::Binder::Words::MAX; ++w)
        {
            t.units[w] = -1;
            if (w >= program.bindless.size())
            {
                continue;
            }
            const Program::BindlessSampler& sampler = program.bindless[w];
            bool two_d = true;
            for (const SamplerSlot& slot : program.sampler_slots)
            {
                two_d = two_d && !(slot.name == sampler.name && target_slot(slot.target) != target_slot(GL_TEXTURE_2D));
            }
            t.units[w] = two_d ? (S32)sampler.unit : -1;
        }
        bool in_row = false;
        for (U32 i = 0; i < material_count; ++i)
        {
            const LLRHI::SceneMaterialUniform& u = materials[i];
            if (!u.name || u.value >= LLRHI::SceneMaterialDesc::VALUES)
            {
                continue;
            }
            const MaterialMember* member = nullptr;
            for (const MaterialMember& m : program.material_members)
            {
                if (m.name == u.name)
                {
                    member = &m;
                    break;
                }
            }
            if (!member)
            {
                bool elsewhere = false;
                for (const LLVKShader::Uniform& v : program.vk.uniforms)
                {
                    elsewhere = elsewhere || (v.stages && v.name == u.name);
                }
                for (const ConstMember& c : program.consts_members)
                {
                    elsewhere = elsewhere || c.name == u.name;
                }
                if (elsewhere)
                {
                    t.why = "its program keeps a material uniform the pool sets per batch outside its material row";
                    return t;
                }
                continue;   // the program does not read it (the pool's set is dropped too)
            }
            // apply_material: a call's components go to the start of the
            // element's vec4 (element 0 here: no list sets an array).
            const U32 components = llmin((U32)u.components, 4u);
            for (U32 c = 0; c < components && u.value + c < LLRHI::SceneMaterialDesc::VALUES; ++c)
            {
                t.material_at[u.value + c] = (S16)(member->first * 4 + c);
            }
            in_row = true;
        }
        if (in_row)
        {
            if (!program.material_vec4s || program.material_vec4s > LLVKScene::Binder::Words::MATERIAL_VEC4S)
            {
                t.why = "its program's material row is larger than a record's copy";
                return t;
            }
            // The words: the samplers, the matrix, palette and light rows, then the material row (gather_slots).
            t.material_word = (U32)program.bindless.size() + (program.matrix_mask ? 1u : 0u) + (program.palette_columns ? 1u : 0u) +
                              (program.light_table ? 1u : 0u);
        }
        return t;
    }

    bool SceneBinder::beginPass(const LLRHI::DirectDraw::Attribute* attributes, U32 attribute_count,
                                const LLRHI::SceneMaterialUniform* materials, U32 material_count, Words& out, const char*& why,
                                bool& permanent)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        mOpen = false;
        mOIT = false;
        mOITSplit = false;
        mProgram = nullptr;
        mTransitions.clear();
        mSlotsUsed = false;
        mOITVariant = nullptr;
        mHead.valid = false;
        mHeadWay = false;
        mHeadVerify = false;
        mHeadCompared = false;
        out = Words();
        permanent = false;
        LLVKMirror& mirror = LLVKMirror::instance();
        if (LLVKTranslate::redirect() || tQueuedDraw)
        {
            why = "the translation thread records";
            return false;
        }
        if (sCapture.on || suppress_translation() || !mirror.isRecording())
        {
            why = "no frame being recorded (or a probe capture)";
            return false;
        }
        if (sActiveQuery || depth_only_pass() || census_tag() || LLVKDevice::instance().checkpointsEnabled())
        {
            why = "an occlusion query, a depth-only pass, the census or checkpoints";
            return false;
        }
        {
            // Draws that write nothing are dropped by the usual way (draw_one).
            const State& s = sState;
            const bool writes_color = s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3];
            const bool writes_depth = s.depth_test && s.depth_write;
            const bool writes_stencil = s.stencil && (s.stencil_front.write_mask || s.stencil_back.write_mask);
            if (!writes_color && !writes_depth && !writes_stencil)
            {
                why = "the pass writes nothing";
                return false;
            }
        }
        Program* program = current_program();
        if (!program || !program->built)
        {
            why = "no program built";
            return false;
        }
        if (!program->draw_runs || !program->draw_rows)
        {
            why = "its program reads no draw rows (LLRHI_DRAW_ROWS, research/46 G4b)";
            permanent = true;
            return false;
        }
        if (direct_writes_block(*program))
        {
            // A matrix the table does not carry (inv_modelview, or one left in
            // its default block) would be the pass's for every record.
            why = "its program reads a draw's matrix from its default block";
            permanent = true;
            return false;
        }
        if (!attributes || !attribute_count || attribute_count > LLRHI::DirectDraw::MAX_ATTRIBUTES)
        {
            why = "no vertex layout";
            return false;
        }
        if (sRecorded.frame != sFrame)
        {
            sRecorded = Recorded();
            sRecorded.frame = sFrame;
            pass_invalidate();
        }
        if (!resolve_draw_target(mTarget))
        {
            why = "an incomplete render target";
            return false;
        }
        if (sOIT.active)
        {
            // research/205 3a-3: node draws with a variant that reads the
            // priority from the row. 3a-5: a hybrid session's world draws
            // too, as draw_into_target draws them: split draws of that
            // variant (nodes where an avatar's are, else blended into the
            // target in the order they come), or, when the session has no
            // avatar nodes (oit_session_takes false), as without OIT.
            if (oit_session_takes(mTarget))
            {
                const OITVariant* v = oit_variant(sCurrentProgram, *program);
                if (!v || v->row_priority_word != program->draw_row_words)
                {
                    why = "its OIT variant does not read the draw's priority from its row (LLRHI_SCENE_ALPHA, research/205 3a-2)";
                    permanent = true;
                    return false;
                }
                mOIT = true;
                mOITSplit = sOIT.world;
                mOITVariant = v;
            }
            else if (!sOIT.world)
            {
                why = "an OIT session's draw that makes no nodes (another target)";
                return false;
            }
        }
        // The layout as an indexed draw of 32-bit indices: no buffers, no
        // textures of its own (every unit keeps its binding), no matrices
        // (the program's row: the records' rows name their own).
        memcpy(mAttributes, attributes, attribute_count * sizeof(LLRHI::DirectDraw::Attribute));
        mLayout = LLRHI::DirectDraw();
        mLayout.attributes = mAttributes;
        mLayout.attribute_count = attribute_count;
        mLayout.index_type = GL_UNSIGNED_INT;
        mLayout.index_count = 3;
        if (!gather(*program))
        {
            why = "its descriptors or the frame's palette table";
            return false;
        }
        if (!tSlotCount || tSlotCount != program->draw_row_words + 1 || tSlotCount > LLVKBindless::MAX_PUSHED_SLOTS)
        {
            why = "its words";
            permanent = true;
            return false;
        }
        const ProgramTable& table = programTable(*program, materials, material_count);
        if (table.why)
        {
            why = table.why;
            permanent = true;
            return false;
        }
        mWordCount = tSlotCount;
        memcpy(mWords, tSlots, tSlotCount * 4);
        out.count = tSlotCount - 1;
        memcpy(out.words, tSlots, out.count * 4);
        for (U32 w = 0; w < out.count; ++w)
        {
            out.units[w] = table.units[w];
        }
        out.matrix_word = program->matrix_mask ? (U32)program->bindless.size() : U32_MAX;
        out.priority_word = mOIT ? program->draw_row_words : U32_MAX;
        // research/219: the palette row's word (gather_slots: after the
        // samplers and the matrix row), when the program's palette is what a
        // resident row holds (3 vec4s for each of up to PREBUILT_PALETTE_ROOM
        // / 3 joints): a rigged record's row names its resident row there.
        // Else none (the scene draws no rigged record with the program).
        const U32 palette_word = (U32)program->bindless.size() + (program->matrix_mask ? 1u : 0u);
        if (program->palette_columns == 3 && (U64)program->palette_elements * 3 <= PREBUILT_PALETTE_ROOM && palette_word < out.count)
        {
            out.palette_word = palette_word;
        }
        // research/198 5.6: the material row each record gets, as the program holds it now.
        out.material_word = U32_MAX;
        out.material_vec4s = 0;
        if (table.material_word != U32_MAX)
        {
            if (table.material_word >= out.count || program->material.size() < (size_t)program->material_vec4s * 4)
            {
                why = "its words";
                permanent = true;
                return false;
            }
            out.material_word = table.material_word;
            out.material_vec4s = program->material_vec4s;
            memcpy(out.material, program->material.data(), (size_t)program->material_vec4s * 4 * sizeof(F32));
            memcpy(out.material_at, table.material_at, sizeof(out.material_at));
        }
        // research/208 S1: the pass's head, read here, at its entry.
        const LLRHI::PassHeadMode mode = LLRHI::passHeadMode();
        if (mode != LLRHI::PassHeadMode::Off)
        {
            const bool head_way = mode == LLRHI::PassHeadMode::On || (mode == LLRHI::PassHeadMode::Auto && mAutoOn);
            if (!makeHead(*program))
            {
                ++mCounts.not_made;
                if (head_way)
                {
                    why = "its pass head (its vertex inputs, research/208 S1)";
                    return false;
                }
            }
            mHeadWay = head_way;
            mHeadVerify = !head_way;
        }
        mProgram = program;
        mProgramName = sCurrentProgram;
        mProgramChanges = sProgramChanges;
        mOpen = true;
        return true;
    }

    // A draw naming the number (gather_slots, from_draw with a handle): the
    // number's published slot, else its image resolved as a name's would be
    // (and published by the same rule).
    U32 SceneBinder::textureSlot(U32 ref)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!mOpen)
        {
            return LLVKBindless::WHITE_SLOT;
        }
        TexRefEntry* e = texref_entry(ref);
        if (!e)
        {
            return LLVKBindless::WHITE_SLOT;
        }
        if (sTexRefTake && e->pub.slot1)
        {
            const U32 slot = e->pub.slot1 - 1;
            LLVKBindless::markUsed(slot, sFrame);
            ++sTexRefCounts.hits;
            return slot;
        }
        const GLuint name = e->image;
        Texture* tex = name ? sTextures.find(name) : nullptr;
        // Sampling a texture that is also being written: GL leaves this undefined (the stand-in, as gather_slots).
        if (!tex || !tex->image || !tex->defined_levels || is_attachment_of(mTarget, tex))
        {
            return LLVKBindless::WHITE_SLOT;
        }
        const VkImageView view = texture_view(*tex);
        if (!view)
        {
            return LLVKBindless::WHITE_SLOT;
        }
        if (tex->bindless_slot == U32_MAX)
        {
            tex->bindless_slot = LLVKBindless::acquire(name);
        }
        if (tex->bindless_slot == LLVKBindless::WHITE_SLOT)
        {
            tex->bindless_slot = U32_MAX;   // table full: try again next time
            return LLVKBindless::WHITE_SLOT;
        }
        // May move the texture to a new slot (its view changed mid-frame).
        const U32 before = tex->bindless_slot;
        tex->bindless_slot = LLVKBindless::use(tex->bindless_slot, name, view, sampler_for(tex, false), tex->view_serial);
        const U32 slot = tex->bindless_slot;
        if (slot != before)
        {
            movedSlot(before);  // research/208 S1: a unit's word may name the old slot
        }
        if (slot == LLVKBindless::WHITE_SLOT)
        {
            tex->bindless_slot = U32_MAX;
            return LLVKBindless::WHITE_SLOT;
        }
        mSlotsUsed = true;
        if (tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            mTransitions.push_back(tex);    // moved before the pass's draws (bindChunk)
        }
        texref_resolved(ref, tex, slot);
        return slot;
    }

    void SceneBinder::texturesDone()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (sWhiteFrame != sFrame)
        {
            LLVKBindless::slotFor(0);   // the stand-in, once per frame (records without a texture sample it)
            sWhiteFrame = sFrame;
        }
        if (mSlotsUsed)
        {
            LLVKBindless::flush();
            mSlotsUsed = false;
        }
    }

    VkCommandBuffer SceneBinder::bindChunk(U32 chunk)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (mOpen && mHeadWay)
        {
            return bindHead(chunk);     // research/208 S1: from the head only
        }
        if (mOpen && mHeadVerify && !mHeadCompared)
        {
            compareHead();  // research/208 S1: once a pass, before the context's way binds
        }
        if (!mOpen || !mProgram || current_program() != mProgram || sCurrentProgram != mProgramName || sProgramChanges != mProgramChanges)
        {
            return VK_NULL_HANDLE;
        }
        Program& program = *mProgram;
        LLVKMirror& mirror = LLVKMirror::instance();
        const VkBuffer index_buffer = LLVKAttribPool::indexBuffer(chunk);
        if (!index_buffer)
        {
            return VK_NULL_HANDLE;
        }

        // The vertex inputs of the layout: each attribute from its stream in
        // the chunk (bound at their streamShift, as run_draw binds them), the
        // constants as direct_inputs finds them.
        U64 signature = 0;
        {
            DirectScope scope(&mLayout);
            DirectBufferScope buffer(index_buffer);     // a stand-in: those inputs are bound to the streams below
            if (!direct_inputs(program, signature))
            {
                return VK_NULL_HANDLE;
            }
        }
        const DirectInputs& f = tDirectInputs;
        const size_t n = f.source.size();
        if (tVertexBuffers.size() != n)
        {
            return VK_NULL_HANDLE;
        }
        mBuffers.resize(n);
        mOffsets.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const S32 k = f.source[i];
            if (k >= 0)
            {
                const LLRHI::DirectDraw::Attribute& a = mAttributes[k];
                mBuffers[i] = LLVKAttribPool::streamBuffer(chunk, a.streamKey(), a.stride);
                mOffsets[i] = a.streamShift();      // research/293 branch 1
                if (!mBuffers[i])
                {
                    return VK_NULL_HANDLE;  // no record of the chunk has this attribute's array (never so for a record drawn)
                }
            }
            else
            {
                mBuffers[i] = tVertexBuffers[i];
                mOffsets[i] = tVertexOffsets[i];
            }
        }

        // The descriptors and words again: the expand wrote the rows from beginPass's.
        if (!gather(program) || tSlotCount != mWordCount || !same_words(tSlots, mWords, mWordCount))
        {
            return VK_NULL_HANDLE;
        }

        // Layouts: sampled textures readable, attachments writable (as draw_into_target).
        {
            auto sampled_layout = [&](const Texture* tex)
            {
                return tex == mTarget.depth && mTarget.target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            };
            bool barriers = needs_attachment_layouts(mTarget);
            for (Texture* tex : *tSampledUsed)
            {
                barriers |= tex->layout != sampled_layout(tex);
            }
            for (Texture* tex : tBindlessSampled)
            {
                barriers |= tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            for (Texture* tex : mTransitions)
            {
                barriers |= tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            if (barriers)
            {
                VkCommandBuffer cmd = mirror.transferCommands();
                LLVKMirror::BarrierBatch batch(cmd);
                for (Texture* tex : *tSampledUsed)
                {
                    transition(cmd, *tex, sampled_layout(tex));
                }
                for (Texture* tex : tBindlessSampled)
                {
                    transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                for (Texture* tex : mTransitions)
                {
                    transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                to_attachment_layouts(cmd, mTarget);
            }
        }

        // research/205 3a-3: the session's variant, as draw_into_target's
        // node draws (and counted as theirs: the resolve runs for them);
        // 3a-5: a hybrid session's world draws as its split draws.
        OITVariant* oit = nullptr;
        U8 oit_mode = OIT_DRAW_NONE;
        if (mOIT)
        {
            oit = oit_for_draw(program, mTarget, oit_mode);
            if (!oit || oit_mode != (mOITSplit ? OIT_DRAW_SPLIT : OIT_DRAW_NODES) ||
                oit->row_priority_word != program.draw_row_words)
            {
                return VK_NULL_HANDLE;
            }
        }
        else if (sOIT.active && oit_session_takes(mTarget))
        {
            return VK_NULL_HANDLE;  // the session takes draws now that beginPass found it would not
        }
        const OITModeScope oit_scope(oit_mode);
        const VkPipeline pipeline = get_pipeline(sCurrentProgram, program, f.inputs, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mTarget.target,
                                                 false, oit, oit_mode == OIT_DRAW_SPLIT);
        if (!pipeline)
        {
            return VK_NULL_HANDLE;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!program.shared_layout)
        {
            set = mirror.allocateSet(program.set_layout);
            if (!set)
            {
                return VK_NULL_HANDLE;
            }
            for (VkWriteDescriptorSet& write : tWrites)
            {
                write.dstSet = set;
            }
            vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), (U32)tWrites.size(), tWrites.data(), 0, nullptr);
        }

        Recorded& r = sRecorded;
        VkCommandBuffer cmd = mirror.beginRendering(mTarget.target);
        if (pipeline != r.pipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            r.pipeline = pipeline;
        }
        bind_program_set(cmd, program, set, tHasDynamic, tDynamicOffset);
        if (oit)
        {
            oit_bind_set(cmd, program, *oit);
        }
        set_dynamic_state(cmd, mTarget.target, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        // The words, the rows word saying "read the rows" (a draw run's push).
        U32 pushed[LLVKBindless::MAX_PUSHED_SLOTS];
        memcpy(pushed, tSlots, tSlotCount * 4);
        pushed[tSlotCount - 1] = 0;
        if (tSlotCount > r.slot_count || !same_words(pushed, r.slots, tSlotCount))
        {
            const VkPushConstantRange range = LLVKBindless::pushConstantRange();
            vkCmdPushConstants(cmd, program.layout, range.stageFlags, 0, tSlotCount * 4, pushed);
            memcpy(r.slots, pushed, tSlotCount * 4);
            r.slot_count = llmax(r.slot_count, tSlotCount);
        }
        if (!same_elements(mBuffers, r.vertex_buffers) || !same_elements(mOffsets, r.vertex_offsets))
        {
            vkCmdBindVertexBuffers(cmd, 0, (U32)n, mBuffers.data(), mOffsets.data());
            r.vertex_buffers = mBuffers;
            r.vertex_offsets = mOffsets;
        }
        if (index_buffer != r.index_buffer || r.index_type != VK_INDEX_TYPE_UINT32 || r.index_offset != 0)
        {
            vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
            r.index_buffer = index_buffer;
            r.index_type = VK_INDEX_TYPE_UINT32;
            r.index_offset = 0;
        }
        return cmd;
    }

    bool SceneBinder::chunkBindable(U32 chunk)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!mOpen || !LLVKAttribPool::indexBuffer(chunk))
        {
            return false;
        }
        for (U32 i = 0; i < mLayout.attribute_count; ++i)
        {
            if (!LLVKAttribPool::streamBuffer(chunk, mAttributes[i].streamKey(), mAttributes[i].stride))
            {
                return false;
            }
        }
        return true;
    }

    // research/205 3a-5: as bindChunk's barriers, for the records' textures only.
    void SceneBinder::settleTextures()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!mOpen)
        {
            return;
        }
        bool barriers = false;
        for (const Texture* tex : mTransitions)
        {
            barriers |= tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (barriers)
        {
            VkCommandBuffer cmd = LLVKMirror::instance().transferCommands();
            LLVKMirror::BarrierBatch batch(cmd);
            for (Texture* tex : mTransitions)
            {
                transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
        mTransitions.clear();
    }

    void SceneBinder::endPass()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (mOpen && mHeadVerify && !mHeadCompared)
        {
            compareHead();  // research/208 S1: a pass only chosen (LLRHI_SCENE verify), or refused before its binds
        }
        mHeadWay = false;
        mHeadVerify = false;
        mDraws = nullptr;   // research/208 S2: a job begun and never ended was never posted (free again)
        if (mOpen)
        {
            // What was bound for the scene's draws is in sRecorded; the open
            // draw run and the pass cache's runs ask again (run_commands moved).
            pass_invalidate();
        }
        mOpen = false;
        mOIT = false;
        mOITSplit = false;
        mProgram = nullptr;
        mTransitions.clear();
    }

    // research/208 S1: the head, from what beginPass resolved and gathered
    // (the thread's tWrites, tSlots, tSampledUsed, ... are its gather's).
    // False: the vertex inputs could not be made (the context's way fails
    // there too).
    bool SceneBinder::makeHead(Program& program)
    {
        PassHead& h = mHead;
        h.valid = false;
        h.admitted = false;
        h.stale = false;
        h.pipeline = VK_NULL_HANDLE;
        h.set = VK_NULL_HANDLE;
        h.program = &program;
        h.program_name = sCurrentProgram;
        h.program_changes = sProgramChanges;
        h.link_serial = program.link_serial;
        h.state = sState;
        // research/204 10 (vk-s4c2, 6 4): a declared world pass (LLRHI_PASS_STATE on, inside its
        // write-through, its copy holding the whole declaration) picks its buckets' pipeline from the
        // declaration, not from what the copy's calls left (the same values on the main thread).
        if (const LLRHI::PassState* declared = LLRHI::openPassState())
        {
            LLVKPassState::apply(*declared, h.state);
        }
        h.target = mTarget;
        h.oit = mOIT ? mOITVariant : nullptr;
        h.oit_mode = mOIT ? (mOITSplit ? OIT_DRAW_SPLIT : OIT_DRAW_NODES) : OIT_DRAW_NONE;
        {
            const OITModeScope oit_scope(h.oit_mode);
            h.dynamic = dynamic_values(h.target.target, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        }
        memcpy(h.words, tSlots, tSlotCount * 4);
        h.word_count = tSlotCount;
        h.sampler_words = (U32)llmin(program.bindless.size(), (size_t)tSlotCount);
        copy_writes(tWrites, h);
        h.sampled.assign(tSampledUsed->begin(), tSampledUsed->end());
        h.bindless_sampled.assign(tBindlessSampled.begin(), tBindlessSampled.end());
        h.has_dynamic = tHasDynamic;
        h.dynamic_offset = tDynamicOffset;

        // The layout's vertex inputs and the constants as direct_inputs finds
        // them now; the stand-in buffer is never bound (the chunk's streams are).
        const VkBuffer stand_in = LLVKBindless::paletteBuffer();
        if (!stand_in)
        {
            return false;
        }
        {
            DirectScope scope(&mLayout);
            DirectBufferScope buffer(stand_in);
            U64 signature = 0;
            if (!direct_inputs(program, signature))
            {
                return false;
            }
        }
        const DirectInputs& f = tDirectInputs;
        h.inputs.assign(f.inputs.begin(), f.inputs.end());
        h.source.assign(f.source.begin(), f.source.end());
        h.constant_buffers.assign(f.constant_buffers.begin(), f.constant_buffers.end());
        h.constant_offsets.assign(f.constant_offsets.begin(), f.constant_offsets.end());
        h.input_signature = f.signature;
        h.valid = true;
        ++mCounts.heads;
        return true;
    }

    // textureSlot moved a texture from `slot`: a head whose units' words name
    // it holds words the context no longer gives (nor do the expand's rows).
    void SceneBinder::movedSlot(U32 slot)
    {
        PassHead& h = mHead;
        if (!h.valid || h.stale)
        {
            return;
        }
        for (U32 w = 0; w < h.sampler_words; ++w)
        {
            if (h.words[w] == slot)
            {
                h.stale = true;
                ++mCounts.stale;
                return;
            }
        }
    }

    // What bindHead could fail at, made before any of the pass is drawn.
    bool SceneBinder::admit()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!mOpen || !mHeadWay)
        {
            return true;    // the context's way: bindChunk decides per chunk, as before
        }
        PassHead& h = mHead;
        if (h.admitted)
        {
            return true;
        }
        auto refuse = [this]()
        {
            ++mCounts.refused;
            return false;
        };
        if (!h.valid || h.stale || !h.program || h.program->link_serial != h.link_serial)
        {
            return refuse();
        }
        Program& program = *h.program;
        h.pipeline = get_pipeline_with(h.state, h.program_name, program, h.inputs, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, h.target.target,
                                       false, h.oit, h.oit_mode == OIT_DRAW_SPLIT);
        if (!h.pipeline)
        {
            return refuse();
        }
        h.set = VK_NULL_HANDLE;
        if (!program.shared_layout)
        {
            // One set for the pass's chunks: what bindChunk's way writes into a set per chunk is the same for each.
            h.set = LLVKMirror::instance().allocateSet(program.set_layout);
            if (!h.set)
            {
                return refuse();
            }
            for (VkWriteDescriptorSet& write : h.writes)
            {
                write.dstSet = h.set;
            }
            vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), (U32)h.writes.size(), h.writes.data(), 0, nullptr);
        }
        h.admitted = true;
        ++mCounts.admitted;
        return true;
    }

    // bindChunk's other half from the head: nothing of the GL context is read
    // (no current program, sState, units, framebuffers or attribute constants).
    VkCommandBuffer SceneBinder::bindHead(U32 chunk)
    {
        PassHead& h = mHead;
        if (!h.valid || !h.admitted || h.stale || !h.program || h.program->link_serial != h.link_serial)
        {
            rtFailure();
            return VK_NULL_HANDLE;
        }
        Program& program = *h.program;
        LLVKMirror& mirror = LLVKMirror::instance();
        const VkBuffer index_buffer = LLVKAttribPool::indexBuffer(chunk);
        if (!index_buffer)
        {
            rtFailure();
            return VK_NULL_HANDLE;
        }
        const size_t n = h.source.size();
        mBuffers.resize(n);
        mOffsets.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const S32 k = h.source[i];
            if (k >= 0)
            {
                const LLRHI::DirectDraw::Attribute& a = mAttributes[k];
                mBuffers[i] = LLVKAttribPool::streamBuffer(chunk, a.streamKey(), a.stride);
                mOffsets[i] = a.streamShift();      // research/293 branch 1
                if (!mBuffers[i])
                {
                    rtFailure();
                    return VK_NULL_HANDLE;
                }
            }
            else
            {
                mBuffers[i] = h.constant_buffers[i];
                mOffsets[i] = h.constant_offsets[i];
            }
        }

        // Layouts: sampled textures readable, attachments writable (as bindChunk's other way).
        headBarriers(h);

        // research/205 3a-3: counted as the session's node draws (oit_for_draw's counts: the resolve runs for them).
        if (h.oit)
        {
            ++sOIT.draws;
            ++sOIT.node_draws;
            sOIT.split_draws += h.oit_mode == OIT_DRAW_SPLIT ? 1 : 0;
        }

        Recorded& r = sRecorded;
        VkCommandBuffer cmd = mirror.beginRendering(h.target.target);
        if (h.pipeline != r.pipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h.pipeline);
            r.pipeline = h.pipeline;
        }
        bind_program_set(cmd, program, h.set, h.has_dynamic, h.dynamic_offset);
        if (h.oit)
        {
            oit_bind_set(cmd, program, *h.oit);
        }
        record_dynamic_values(cmd, r, h.dynamic);
        // The words, the rows word saying "read the rows" (a draw run's push).
        const U32 count = h.word_count;
        U32 pushed[LLVKBindless::MAX_PUSHED_SLOTS];
        memcpy(pushed, h.words, count * 4);
        pushed[count - 1] = 0;
        if (count > r.slot_count || !same_words(pushed, r.slots, count))
        {
            const VkPushConstantRange range = LLVKBindless::pushConstantRange();
            vkCmdPushConstants(cmd, program.layout, range.stageFlags, 0, count * 4, pushed);
            memcpy(r.slots, pushed, count * 4);
            r.slot_count = llmax(r.slot_count, count);
        }
        if (!same_elements(mBuffers, r.vertex_buffers) || !same_elements(mOffsets, r.vertex_offsets))
        {
            vkCmdBindVertexBuffers(cmd, 0, (U32)n, mBuffers.data(), mOffsets.data());
            r.vertex_buffers = mBuffers;
            r.vertex_offsets = mOffsets;
        }
        if (index_buffer != r.index_buffer || r.index_type != VK_INDEX_TYPE_UINT32 || r.index_offset != 0)
        {
            vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
            r.index_buffer = index_buffer;
            r.index_type = VK_INDEX_TYPE_UINT32;
            r.index_offset = 0;
        }
        ++mCounts.bound;
        return cmd;
    }

    // The head is what its admission made and still holds (bindHead, beginDraws).
    bool SceneBinder::headBindable() const
    {
        const PassHead& h = mHead;
        return h.valid && h.admitted && !h.stale && h.program && h.program->link_serial == h.link_serial;
    }

    // bindHead's first half: the head's sampled textures readable, the
    // records' textures (textureSlot's) too, the target's attachments
    // writable; nothing when they are so already (after a pass's first chunk).
    void SceneBinder::headBarriers(const PassHead& h)
    {
        auto sampled_layout = [&](const Texture* tex)
        {
            return tex == h.target.depth && h.target.target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        };
        bool barriers = needs_attachment_layouts(h.target);
        for (Texture* tex : h.sampled)
        {
            barriers |= tex->layout != sampled_layout(tex);
        }
        for (Texture* tex : h.bindless_sampled)
        {
            barriers |= tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        for (Texture* tex : mTransitions)
        {
            barriers |= tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (barriers)
        {
            VkCommandBuffer cmd = LLVKMirror::instance().transferCommands();
            LLVKMirror::BarrierBatch batch(cmd);
            for (Texture* tex : h.sampled)
            {
                transition(cmd, *tex, sampled_layout(tex));
            }
            for (Texture* tex : h.bindless_sampled)
            {
                transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            for (Texture* tex : mTransitions)
            {
                transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            to_attachment_layouts(cmd, h.target);
        }
    }

    // research/208 S2: a job no one holds (each is free again after the render thread's join).
    SceneDrawJob* SceneBinder::freeDrawJob()
    {
        const size_t n = mDrawJobs.size();
        for (size_t i = 0; i < n; ++i)
        {
            SceneDrawJob* job = mDrawJobs[(mDrawJobNext + i) % n].get();
            if (job->idle() && !job->mHeld.load(std::memory_order_acquire))     // research/208 S3: not a pass's the render thread holds
            {
                mDrawJobNext = (mDrawJobNext + i + 1) % n;
                return job;
            }
        }
        if (n >= MAX_DRAW_JOBS)
        {
            return nullptr;
        }
        mDrawJobs.push_back(std::make_unique<SceneDrawJob>());
        return mDrawJobs.back().get();
    }

    // research/208 S2: bindHead's first half once for the pass (the main
    // thread's: the tracked layouts and the open rendering are its state),
    // and the values its second half binds, into a job.
    bool SceneBinder::beginDraws(LLVKRender::Use use, U32 pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        mDraws = nullptr;
        const PassHead& h = mHead;
        if (use == LLVKRender::Use::Inline || !mOpen || !mHeadWay || !headBindable() || h.oit || !h.word_count)
        {
            return false;   // the chunks are bound one by one (bindHead counts what fails there)
        }
        SceneDrawJob* job = freeDrawJob();
        if (!job)
        {
            return false;
        }
        Program& program = *h.program;
        headBarriers(h);
        const VkCommandBuffer cmd = LLVKMirror::instance().beginRendering(h.target.target);
        if (!cmd)
        {
            return false;
        }
        job->cmd = cmd;
        job->pipeline = h.pipeline;
        job->layout = program.layout;
        job->table = LLVKBindless::set();
        job->set = h.set;
        job->has_dynamic = h.has_dynamic;
        job->dynamic_offset = h.dynamic_offset;
        job->dynamic = h.dynamic;
        job->push_stages = LLVKBindless::pushConstantRange().stageFlags;
        memcpy(job->words, h.words, h.word_count * 4);
        job->word_count = h.word_count;
        job->input_count = (U32)h.source.size();
        job->buffers.clear();
        job->offsets.clear();
        job->buckets.clear();
        job->mPass = pass;
        mDraws = job;
        mDrawsUse = use;
        return true;
    }

    // research/208 S2: a bucket's chunk as bindHead resolves it (the pool's
    // index buffer and the layout's streams, the constants as the head has
    // them), and its indirect draw.
    bool SceneBinder::addDraw(U32 chunk, VkBuffer commands, VkDeviceSize offset, VkBuffer counts, VkDeviceSize count_offset, U32 max_count,
                              U32 stride)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        SceneDrawJob* job = mDraws;
        if (!job)
        {
            return false;
        }
        const PassHead& h = mHead;
        SceneDrawJob::Bucket b;
        b.index_buffer = LLVKAttribPool::indexBuffer(chunk);
        if (!b.index_buffer)
        {
            rtFailure();
            return false;
        }
        b.first_input = (U32)job->buffers.size();
        for (size_t i = 0; i < h.source.size(); ++i)
        {
            const S32 k = h.source[i];
            if (k >= 0)
            {
                const LLRHI::DirectDraw::Attribute& a = mAttributes[k];
                const VkBuffer stream = LLVKAttribPool::streamBuffer(chunk, a.streamKey(), a.stride);
                if (!stream)
                {
                    rtFailure();
                    return false;
                }
                job->buffers.push_back(stream);
                job->offsets.push_back(a.streamShift());    // research/293 branch 1
            }
            else
            {
                job->buffers.push_back(h.constant_buffers[i]);
                job->offsets.push_back(h.constant_offsets[i]);
            }
        }
        b.commands = commands;
        b.commands_offset = offset;
        b.counts = counts;
        b.counts_offset = count_offset;
        b.max_count = max_count;
        b.stride = stride;
        job->buckets.push_back(b);
        ++mCounts.bound;
        return true;
    }

    // research/208 S2: the draws recorded by the render thread into a stream
    // spliced here, or here (verify: and handed to the thread to compare).
    // True: the thread records them.
    bool SceneBinder::endDraws()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        SceneDrawJob* job = mDraws;
        mDraws = nullptr;
        if (!job)
        {
            return false;
        }
        const bool posted = mDrawsUse == LLVKRender::Use::Thread && LLVKRender::post(*job);
        if (!posted)
        {
            job->record();
            if (mDrawsUse == LLVKRender::Use::Verify)
            {
                job->mMainDigest = job->digest();
                LLVKRender::postVerify(*job);
            }
        }
        // What the draws bound is not in sRecorded (they record with a
        // Recorded of their own): the draws after them bind what they need
        // again, as after a draw list's segments. The vectors keep their room.
        std::vector<VkBuffer> buffers = std::move(sRecorded.vertex_buffers);
        std::vector<VkDeviceSize> offsets = std::move(sRecorded.vertex_offsets);
        buffers.clear();
        offsets.clear();
        sRecorded = Recorded();
        sRecorded.frame = sFrame;
        sRecorded.vertex_buffers = std::move(buffers);
        sRecorded.vertex_offsets = std::move(offsets);
        sRun.open = false;      // its indirect draw was recorded before them
        pass_invalidate();
        return posted;
    }

    void SceneBinder::abortDraws()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        mDraws = nullptr;   // never posted: free again
    }

    // research/208 S3: beginDraws' half of the pass's binds (the layouts, the
    // rendering), and what addDraw would resolve of each chunk, for every
    // chunk of the pool now: the render thread adds the buckets its expand
    // makes from these values only.
    LLVKScene::ThreadDraws* SceneBinder::beginThreadDraws(U32 pass, bool secondary)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const PassHead& h = mHead;
        if (!mOpen || !mHeadWay || !headBindable() || h.oit || !h.word_count)
        {
            return nullptr;
        }
        SceneDrawJob* job = freeDrawJob();
        if (!job)
        {
            return nullptr;
        }
        const U32 inputs = (U32)h.source.size();
        const U32 chunks = LLVKAttribPool::chunkCount();
        job->chunk_count = chunks;
        job->input_count = inputs;
        job->chunk_index.resize(chunks);
        job->chunk_inputs.resize((size_t)chunks * inputs);
        job->input_offsets.resize(inputs);
        for (U32 i = 0; i < inputs; ++i)
        {
            job->input_offsets[i] = h.source[i] >= 0 ? mAttributes[h.source[i]].streamShift() : h.constant_offsets[i];   // research/293 branch 1
        }
        for (U32 c = 0; c < chunks; ++c)
        {
            job->chunk_index[c] = LLVKAttribPool::indexBuffer(c);
            VkBuffer* row = job->chunk_inputs.data() + (size_t)c * inputs;
            for (U32 i = 0; i < inputs; ++i)
            {
                const S32 k = h.source[i];
                row[i] = k >= 0 ? LLVKAttribPool::streamBuffer(c, mAttributes[k].streamKey(), mAttributes[k].stride) : h.constant_buffers[i];
            }
        }
        Program& program = *h.program;
        headBarriers(h);
        // research/208 13.3: secondary, a scope of its own for the thread's secondary command buffer only
        // (the barriers above stay before it).
        const LLVKMirror::Target& target = h.target.target;
        const VkCommandBuffer cmd = secondary ? LLVKMirror::instance().beginSecondaryRendering(target)
                                              : LLVKMirror::instance().beginRendering(target);
        if (!cmd)
        {
            return nullptr;
        }
        job->cmd = cmd;
        job->formats = secondary_formats(target);
        job->pipeline = h.pipeline;
        job->layout = program.layout;
        job->table = LLVKBindless::set();
        job->set = h.set;
        job->has_dynamic = h.has_dynamic;
        job->dynamic_offset = h.dynamic_offset;
        job->dynamic = h.dynamic;
        job->push_stages = LLVKBindless::pushConstantRange().stageFlags;
        memcpy(job->words, h.words, h.word_count * 4);
        job->word_count = h.word_count;
        job->buffers.clear();
        job->offsets.clear();
        job->buckets.clear();
        job->mPass = pass;
        job->mRecorded = 0;
        job->mHeld.store(true, std::memory_order_release);
        // The draws are recorded where the pass's stream is spliced, after
        // this: what they bind is not known here afterwards (as endDraws).
        std::vector<VkBuffer> buffers = std::move(sRecorded.vertex_buffers);
        std::vector<VkDeviceSize> offsets = std::move(sRecorded.vertex_offsets);
        buffers.clear();
        offsets.clear();
        sRecorded = Recorded();
        sRecorded.frame = sFrame;
        sRecorded.vertex_buffers = std::move(buffers);
        sRecorded.vertex_offsets = std::move(offsets);
        sRun.open = false;
        pass_invalidate();
        return job;
    }

    // verify: the head against what the context gives now (the program, the
    // target, the state, the units' words and descriptors, the default
    // block, the vertex inputs, the OIT mode), as bindChunk's other way finds
    // them. auto turns the head's way on after HEAD_AUTO_FRAMES frames of
    // compared passes without a difference.
    void SceneBinder::compareHead()
    {
        mHeadCompared = true;
        if (mCompareFrame != sFrame)
        {
            if (mCompareFrame != U32_MAX)
            {
                mCleanFrames = mFrameDiffered ? 0 : mCleanFrames + 1;
            }
            mCompareFrame = sFrame;
            mFrameDiffered = false;
            if (LLRHI::passHeadMode() == LLRHI::PassHeadMode::Auto && !mAutoOn && mCleanFrames >= HEAD_AUTO_FRAMES)
            {
                mAutoOn = true;     // from the next pass (this one keeps its way)
                LL_INFOS("VKScene") << "pass head (research/208 S1): " << mCleanFrames
                                    << " frames of compared passes without a difference, the scene's passes bind from their heads" << LL_ENDL;
            }
        }
        ++mCounts.compared;
        const PassHead& h = mHead;
        if (!h.valid)
        {
            headDiffers("no head (its vertex inputs)");
            return;
        }
        Program* program = current_program();
        if (program != h.program || !program || sCurrentProgram != h.program_name || sProgramChanges != h.program_changes ||
            program->link_serial != h.link_serial)
        {
            headDiffers("program");
            return;
        }
        if (!same_pipeline_state(sState, h.state))
        {
            headDiffers("state (the pipeline's)");
            return;
        }
        ResolvedTarget target;
        if (!resolve_draw_target(target))
        {
            headDiffers("target (not resolved)");
            return;
        }
        bool gathered = false;
        {
            DirectScope scope(&mLayout);
            gathered = gather_descriptors(*program, target) && !tPaletteFull;
        }
        if (!gathered)
        {
            headDiffers("units (not gathered)");
            return;
        }
        if (!same_target(target, h.target))
        {
            headDiffers("target");
            return;
        }
        // The OIT mode the context gives (oit_for_draw's test, without its counts).
        const OITVariant* oit = nullptr;
        U8 oit_mode = OIT_DRAW_NONE;
        if (oit_session_takes(target))
        {
            oit = oit_variant(sCurrentProgram, *program);
            oit_mode = oit ? (sOIT.world ? OIT_DRAW_SPLIT : OIT_DRAW_NODES) : OIT_DRAW_NONE;
        }
        if (oit != h.oit || oit_mode != h.oit_mode)
        {
            headDiffers("OIT mode");
            return;
        }
        {
            const OITModeScope oit_scope(oit_mode);
            if (!same_bytes(dynamic_values(target.target, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST), h.dynamic))
            {
                headDiffers("state (dynamic)");
                return;
            }
        }
        if (h.stale)
        {
            // The words moved by design (the pass is refused on both ways); the rest is compared.
        }
        else if (tSlotCount != h.word_count || !same_words(tSlots, h.words, tSlotCount))
        {
            headDiffers("units (words)");
            return;
        }
        if (!same_writes(tWrites, h.writes))
        {
            headDiffers("units (descriptors)");
            return;
        }
        if (!same_elements(*tSampledUsed, h.sampled) || !same_elements(tBindlessSampled, h.bindless_sampled))
        {
            headDiffers("units (sampled textures)");
            return;
        }
        if (tHasDynamic != h.has_dynamic || (tHasDynamic && tDynamicOffset != h.dynamic_offset))
        {
            headDiffers("uniforms (the default block)");
            return;
        }
        const VkBuffer stand_in = LLVKBindless::paletteBuffer();
        bool inputs = false;
        if (stand_in)
        {
            DirectScope scope(&mLayout);
            DirectBufferScope buffer(stand_in);
            U64 signature = 0;
            inputs = direct_inputs(*program, signature);
        }
        const DirectInputs& f = tDirectInputs;
        if (!inputs || f.signature != h.input_signature || !same_elements(f.source, h.source) ||
            !same_elements(f.constant_buffers, h.constant_buffers) || !same_elements(f.constant_offsets, h.constant_offsets))
        {
            headDiffers("vertex inputs");
            return;
        }
    }

    void SceneBinder::headDiffers(const char* what)
    {
        ++mCounts.differ;
        mFrameDiffered = true;
        mCleanFrames = 0;
        if (mDiffKept < HEAD_DIFFS_KEPT)
        {
            mDiffWhat[mDiffKept] = what;
            mDiffProgram[mDiffKept] = sCurrentProgram;
            ++mDiffKept;
        }
        if (!mDiffLogged)
        {
            mDiffLogged = true;
            LL_WARNS("VKScene") << "pass head (research/208 S1): the head differs from the GL context at a chunk's bind: " << what
                                << " (program " << sCurrentProgram << "); the VKPerf scene line counts them" << LL_ENDL;
        }
    }

    // A bind after the admission failed: never so by design (admit made or
    // asked all of it). auto goes back to comparing.
    void SceneBinder::rtFailure()
    {
        ++mCounts.rt_failures;
        if (LLRHI::passHeadMode() == LLRHI::PassHeadMode::Auto && mAutoOn)
        {
            mAutoOn = false;
            mCleanFrames = 0;
            LL_WARNS("VKScene") << "pass head (research/208 S1): a chunk's bind failed after the pass's admission; "
                                   "the passes are compared again before they bind from their heads" << LL_ENDL;
        }
    }

    std::string SceneBinder::takeReport(U32 frames)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const LLRHI::PassHeadMode mode = LLRHI::passHeadMode();
        const HeadCounts& c = mCounts;
        if (mode == LLRHI::PassHeadMode::Off || (!c.heads && !c.not_made && !c.compared))
        {
            return std::string();
        }
        frames = llmax(frames, 1u);
        std::ostringstream out;
        out << "pass head (research/208 S1, " << LLRHI::passHeadModeName(mode)
            << (mode == LLRHI::PassHeadMode::Auto ? (mAutoOn ? ", bound from the heads" : ", comparing") : "") << "): made "
            << (F32)c.heads / frames << " a frame";
        if (c.not_made)
        {
            out << ", not made " << c.not_made;
        }
        if (c.compared)
        {
            out << ", compared " << c.compared << " passes, differ " << c.differ;
            for (U32 i = 0; i < mDiffKept; ++i)
            {
                out << (i ? "; " : " (") << mDiffWhat[i] << " in program " << mDiffProgram[i];
            }
            out << (mDiffKept ? ")" : "");
            if (mode == LLRHI::PassHeadMode::Auto && !mAutoOn)
            {
                out << ", frames without a difference " << mCleanFrames << " of " << HEAD_AUTO_FRAMES;
            }
        }
        if (c.stale)
        {
            out << ", stale (a word's texture moved) " << c.stale;
        }
        if (c.admitted || c.refused || c.bound)
        {
            out << ", admitted " << c.admitted << ", refused at the admission " << c.refused << ", chunks bound from heads "
                << (F32)c.bound / frames << " a frame";
        }
        if (c.rt_failures)
        {
            out << ", RT FAILURES " << c.rt_failures << " (a bind after the admission failed: a render thread could not hand it back)";
        }
        mCounts = HeadCounts();
        mDiffKept = 0;
        return out.str();
    }
} // namespace

    void install_scene_binder()
    {
        LLVKScene::setBinder(&sSceneBinder);
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
