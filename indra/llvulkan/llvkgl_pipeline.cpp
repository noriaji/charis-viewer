/**
 * @file llvkgl_pipeline.cpp
 * @brief The draw's render target (GL framebuffer to Vulkan attachments) and its graphics pipeline: key, recipe and creation (research/91, research/111 B-PL).
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
namespace
{
    VkBlendFactor blend_factor(GLenum f)
    {
        switch (f)
        {
        case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
        case GL_ONE: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
        }
    }

    VkBlendOp blend_op(GLenum eq)
    {
        switch (eq)
        {
        case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case GL_MIN: return VK_BLEND_OP_MIN;
        case GL_MAX: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
        }
    }

    // ------------------------------------------------------------------
    // Render targets

    // The draw framebuffer as Vulkan attachments. False when incomplete.
    bool resolve_target_uncached(ResolvedTarget& out);

    // Whether two resolutions give the same attachments in every respect the
    // users of the stamp read (Target::operator== leaves out the formats).
    bool same_resolution(const ResolvedTarget& a, bool a_ok, const ResolvedTarget& b, bool b_ok)
    {
        const LLVKMirror::Target& x = a.target;
        const LLVKMirror::Target& y = b.target;
        if (a_ok != b_ok || a.is_default != b.is_default || a.color_textures != b.color_textures || a.depth != b.depth ||
            x.color_count != y.color_count || x.depth != y.depth || x.depth_format != y.depth_format || x.stencil != y.stencil ||
            x.depth_read_only != y.depth_read_only || x.width != y.width || x.height != y.height)
        {
            return false;
        }
        for (U32 i = 0; i < 4; ++i)
        {
            if (x.colors[i] != y.colors[i] || x.color_formats[i] != y.color_formats[i] || a.colors[i] != b.colors[i])
            {
                return false;
            }
        }
        return true;
    }

    U32 sStamp = 0;     // the last stamp handed out (render thread)

} // namespace
    // The draw framebuffer as Vulkan attachments, re-resolved only when the
    // bound framebuffer or anything attached to framebuffers changed.
    // Research/154 W1, W2: the stamp stays the same for the same resolution,
    // so caches keyed on it (the program's last pipeline, its image list, the
    // texture slots, the pass cache) keep their entries when a target is
    // bound again, when another target came in between, and from frame to
    // frame; the frame's own target has one too.
    bool resolve_draw_target(ResolvedTarget& out)
    {
        if (sState.draw_framebuffer == 0)
        {
            // Its views and size change only when the mirror makes its target again.
            struct DefaultCached
            {
                U32 serial = 0;     // LLVKMirror::defaultTargetSerial() starts at 1
                bool ok = false;
                ResolvedTarget value;
            };
            static DefaultCached sDefault;
            const U32 serial = LLVKMirror::instance().defaultTargetSerial();
            if (sDefault.serial != serial)
            {
                sDefault.value = ResolvedTarget();
                sDefault.ok = resolve_target_uncached(sDefault.value);
                sDefault.value.stamp = ++sStamp;
                sDefault.serial = serial;
            }
            out = sDefault.value;
            return sDefault.ok;
        }
        // The last framebuffer asked for, without looking it up.
        struct Cached
        {
            GLuint framebuffer = U32_MAX;
            U32 target_state = U32_MAX;
            U32 frame = U32_MAX;
            bool ok = false;
            ResolvedTarget value;
        };
        static Cached sCached;
        const U32 target_state = sTargetState.load(std::memory_order_relaxed);
        if (sCached.framebuffer != sState.draw_framebuffer || sCached.target_state != target_state || sCached.frame != sFrame)
        {
            auto fb_it = sFramebuffers.find(sState.draw_framebuffer);
            if (fb_it == sFramebuffers.end())
            {
                sCached.value = ResolvedTarget();
                sCached.ok = resolve_target_uncached(sCached.value);    // false: no such framebuffer
                sCached.value.stamp = ++sStamp;
            }
            else
            {
                Framebuffer& fb = fb_it->second;
                if (fb.resolved_state != target_state || fb.resolved_frame != sFrame)
                {
                    ResolvedTarget value;
                    const bool ok = resolve_target_uncached(value);
                    value.stamp = fb.resolved.stamp && same_resolution(value, ok, fb.resolved, fb.resolved_ok) ? fb.resolved.stamp : ++sStamp;
                    fb.resolved = value;
                    fb.resolved_ok = ok;
                    fb.resolved_state = target_state;   // the value checked: a bump during the resolve is seen next time
                    fb.resolved_frame = sFrame;
                }
                sCached.value = fb.resolved;
                sCached.ok = fb.resolved_ok;
            }
            sCached.framebuffer = sState.draw_framebuffer;
            sCached.target_state = target_state;
            sCached.frame = sFrame;
        }
        out = sCached.value;
        return sCached.ok;
    }
namespace
{

    bool resolve_target_uncached(ResolvedTarget& out)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (sState.draw_framebuffer == 0)
        {
            out.target = mirror.defaultTarget();
            out.is_default = true;
            return out.target.width > 0;
        }
        auto fb_it = sFramebuffers.find(sState.draw_framebuffer);
        if (fb_it == sFramebuffers.end())
        {
            return false;
        }
        const Framebuffer& fb = fb_it->second;
        U32 width = U32_MAX, height = U32_MAX;
        for (U32 i = 0; i < fb.draw_count && i < 4; ++i)
        {
            GLenum buffer = fb.draw_buffers[i];
            out.target.color_count = i + 1;
            if (buffer == GL_NONE)
            {
                continue;
            }
            U32 index = buffer - GL_COLOR_ATTACHMENT0;
            if (index >= 4 || !fb.color[index])
            {
                continue;
            }
            Texture* found = sTextures.find(fb.color[index]);
            if (!found || !found->attach_view)
            {
                return false;
            }
            Texture& tex = *found;
            tex.rendered_to = true;
            out.target.colors[i] = tex.attach_view;
            out.target.color_formats[i] = tex.format.format;
            out.colors[out.color_textures++] = &tex;
            width = llmin(width, tex.width);
            height = llmin(height, tex.height);
        }
        if (fb.depth)
        {
            Texture* found = sTextures.find(fb.depth);
            if (!found || !found->attach_view || !found->format.depth)
            {
                return false;
            }
            Texture& tex = *found;
            tex.rendered_to = true;
            out.target.depth = tex.attach_view;
            out.target.depth_format = tex.format.format;
            out.target.stencil = tex.format.stencil;
            out.depth = &tex;
            width = llmin(width, tex.width);
            height = llmin(height, tex.height);
        }
        if (width == U32_MAX)
        {
            return false;
        }
        out.target.width = width;
        out.target.height = height;
        return true;
    }

} // namespace

    // ------------------------------------------------------------------
    // Pipelines: keyed on everything that is not dynamic state (PipelineKeyWord).

    std::unordered_map<std::vector<U32>, VkPipeline, KeyHash> sPipelines;
    std::vector<U32> tPipelineKey;

namespace
{

    // The GL blend state as an OIT node keeps it (research/41, OIT_FACTOR_*),
    // from a pipeline's key.
    U32 oit_blend_word(const std::vector<U32>& key)
    {
        auto factor = [](GLenum f) -> U32
        {
            switch (f)
            {
            case GL_ZERO: return OIT_FACTOR_ZERO;
            case GL_SRC_COLOR: return OIT_FACTOR_SRC_COLOR;
            case GL_ONE_MINUS_SRC_COLOR: return OIT_FACTOR_ONE_MINUS_SRC_COLOR;
            case GL_DST_COLOR: return OIT_FACTOR_DST_COLOR;
            case GL_ONE_MINUS_DST_COLOR: return OIT_FACTOR_ONE_MINUS_DST_COLOR;
            case GL_SRC_ALPHA: return OIT_FACTOR_SRC_ALPHA;
            case GL_ONE_MINUS_SRC_ALPHA: return OIT_FACTOR_ONE_MINUS_SRC_ALPHA;
            case GL_DST_ALPHA: return OIT_FACTOR_DST_ALPHA;
            case GL_ONE_MINUS_DST_ALPHA: return OIT_FACTOR_ONE_MINUS_DST_ALPHA;
            case GL_SRC_ALPHA_SATURATE: return OIT_FACTOR_SRC_ALPHA_SATURATE;
            case GL_CONSTANT_COLOR: case GL_CONSTANT_ALPHA: return OIT_FACTOR_CONSTANT;
            case GL_ONE_MINUS_CONSTANT_COLOR: case GL_ONE_MINUS_CONSTANT_ALPHA: return OIT_FACTOR_ONE_MINUS_CONSTANT;
            default: return OIT_FACTOR_ONE;     // GL_ONE, and whatever blend_factor() reads as ONE
            }
        };
        auto equation = [](GLenum e) -> U32
        {
            return e == GL_FUNC_SUBTRACT ? OIT_EQ_SUBTRACT : e == GL_FUNC_REVERSE_SUBTRACT ? OIT_EQ_REVERSE_SUBTRACT :
                   e == GL_MIN ? OIT_EQ_MIN : e == GL_MAX ? OIT_EQ_MAX : OIT_EQ_ADD;
        };
        // Blending off: the color replaces what is behind it (ONE, ZERO).
        const bool blend = key[PK_BLEND] != 0;
        const U32 src_rgb = blend ? factor(key[PK_SRC_RGB]) : OIT_FACTOR_ONE;
        const U32 dst_rgb = blend ? factor(key[PK_DST_RGB]) : OIT_FACTOR_ZERO;
        const U32 src_a = blend ? factor(key[PK_SRC_A]) : OIT_FACTOR_ONE;
        const U32 dst_a = blend ? factor(key[PK_DST_A]) : OIT_FACTOR_ZERO;
        const U32 eq_rgb = blend ? equation(key[PK_EQ_RGB]) : OIT_EQ_ADD;
        const U32 eq_a = blend ? equation(key[PK_EQ_A]) : OIT_EQ_ADD;
        const U32 mask = key[PK_COLOR_MASK];
        return src_rgb | (dst_rgb << 4) | (src_a << 8) | (dst_a << 12) | (eq_rgb << 16) | (eq_a << 19) | (mask << 22);
    }
} // namespace

    // Everything get_pipeline creates a pipeline from is in its key, but the
    // program's objects (research/111 B-PL: the prewarm makes a pipeline from
    // a key of an earlier session). Render thread, under sLock.
    void make_pipeline_recipe(const Program& program, const std::vector<U32>& key, VkPrimitiveTopology prim, const OITVariant* oit,
                              PipelineRecipe& recipe)
    {
        const U32 color_count = llmin(key[PK_COLOR_COUNT], 4u);
        // After the color formats, 3 words a vertex input, then 2 for device
        // generated commands or 1 for OIT (their lengths never meet).
        const size_t rest = key.size() - PK_COLOR_FORMATS - color_count;
        const size_t tail = rest % 3;
        const bool split = oit && tail == 1 && key.back() == PIPELINE_KEY_OIT_SPLIT;

        // The variant's fragment module with the program's own other stages
        // (the same positions, depth and coverage as the program's pipelines).
        recipe.stages.clear();
        for (size_t i = 0; i < program.vk.modules.size(); ++i)
        {
            const bool variant = oit && program.vk.stages[i] == VK_SHADER_STAGE_FRAGMENT_BIT;
            recipe.stages.push_back({ program.vk.stages[i], variant ? oit->fragment : program.vk.modules[i], variant });
        }
        recipe.oit_constants[0] = oit ? oit_blend_word(key) : 0u;
        recipe.oit_constants[1] = oit && split ? 1u : 0u;

        recipe.inputs.clear();
        for (size_t w = PK_COLOR_FORMATS + color_count; w + 3 <= key.size() - tail; w += 3)
        {
            recipe.inputs.push_back({ key[w], (VkFormat)key[w + 1], key[w + 2] });
        }
        recipe.topology = prim;
        recipe.polygon_mode = key[PK_POLYGON] == GL_LINE ? VK_POLYGON_MODE_LINE : key[PK_POLYGON] == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
        recipe.depth_clamp = key[PK_DEPTH_CLAMP] != 0;
        recipe.color_count = color_count;
        recipe.depth_format = (VkFormat)key[PK_DEPTH_FORMAT];
        recipe.stencil = key[PK_STENCIL] != 0;

        // GL applies one blend state to every draw buffer; formats that cannot blend just write.
        const U32 mask = key[PK_COLOR_MASK];
        for (U32 i = 0; i < color_count; ++i)
        {
            const VkFormat format = (VkFormat)key[PK_COLOR_FORMATS + i];
            recipe.color_formats[i] = format;
            VkPipelineColorBlendAttachmentState& blend = recipe.blends[i];
            blend = {};
            const bool can_blend = format != VK_FORMAT_UNDEFINED && (format_features(format) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
            blend.blendEnable = key[PK_BLEND] && can_blend;
            blend.srcColorBlendFactor = blend_factor(key[PK_SRC_RGB]);
            blend.dstColorBlendFactor = blend_factor(key[PK_DST_RGB]);
            blend.colorBlendOp = blend_op(key[PK_EQ_RGB]);
            blend.srcAlphaBlendFactor = blend_factor(key[PK_SRC_A]);
            blend.dstAlphaBlendFactor = blend_factor(key[PK_DST_A]);
            blend.alphaBlendOp = blend_op(key[PK_EQ_A]);
            blend.colorWriteMask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                   ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
            if (oit && !split)
            {
                blend.blendEnable = VK_FALSE;   // the node keeps the blend state; the resolve blends
                blend.colorWriteMask = 0;
            }
        }
        recipe.generated = tail == 2;
        recipe.layout = oit ? oit->layout : program.layout;
    }

    // generated: the variant device generated commands bind vertex buffers
    // with (research/50 step 4): the same pipeline with dynamic vertex strides.
    // oit: the program's OIT variant (research/41): the blend state as the
    // fragment stage's specialization constant, its own layout, and no color
    // writes, or with split (a hybrid session's world draws) the draw's own
    // blend state and color mask.
    VkPipeline get_pipeline(GLuint program_id, Program& program, const std::vector<VertexInput>& inputs,
                            VkPrimitiveTopology prim, const LLVKMirror::Target& target, bool generated,
                            const OITVariant* oit, bool split)
    {
        return get_pipeline_with(sState, program_id, program, inputs, prim, target, generated, oit, split);
    }

    VkPipeline get_pipeline_with(const State& s, GLuint program_id, Program& program, const std::vector<VertexInput>& inputs,
                                 VkPrimitiveTopology prim, const LLVKMirror::Target& target, bool generated,
                                 const OITVariant* oit, bool split)
    {
        if (generated && oit)
        {
            return VK_NULL_HANDLE;  // never both (their key words could meet)
        }
        std::vector<U32>& key = tPipelineKey;
        // The words in PipelineKeyWord order.
        key.assign({ program_id, program.link_serial, (U32)(uintptr_t)program.layout, topology_class(prim), s.polygon_mode,
                     s.blend, s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_a, s.blend_dst_a,
                     s.blend_eq_rgb, s.blend_eq_a,
                     (U32)s.color_mask[0] | ((U32)s.color_mask[1] << 1) | ((U32)s.color_mask[2] << 2) | ((U32)s.color_mask[3] << 3),
                     target.color_count, (U32)target.depth_format, target.stencil ? 1u : 0u, s.depth_clamp ? 1u : 0u });
        for (U32 i = 0; i < target.color_count; ++i)
        {
            key.push_back(target.color_formats[i]);
        }
        for (const VertexInput& in : inputs)
        {
            key.push_back(in.location);
            key.push_back(in.format);
            key.push_back(in.stride);
        }
        if (generated)
        {
            // Two more words: a plain key with the same color count (key[13]) has
            // 17 + colors + 3 per input words, never this many.
            key.push_back(PIPELINE_KEY_DGC);
            key.push_back(PIPELINE_KEY_DGC);
        }
        if (oit)
        {
            // One more word (two for DGC): never the length of a plain or DGC key
            // with the same color count. The blend state is in the key already.
            key.push_back(split ? PIPELINE_KEY_OIT_SPLIT : PIPELINE_KEY_OIT);
        }
        auto it = sPipelines.find(key);
        if (it != sPipelines.end())
        {
            return it->second;
        }
        PipelineRecipe recipe;
        make_pipeline_recipe(program, key, prim, oit, recipe);
        const VkPipeline pipeline = create_pipeline(recipe);
        // Kept also when it failed (null: not tried again with this link).
        // The program's list lets destroy_program_objects find it (fix-pipeleak).
        program.pipeline_keys.push_back(&sPipelines.emplace(key, pipeline).first->first);
        if (pipeline)
        {
            note_pipeline_key(program, key);   // research/111 B-PL: made before it is needed next session
        }
        return pipeline;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
