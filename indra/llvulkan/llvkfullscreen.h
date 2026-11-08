/**
 * @file llvkfullscreen.h
 * @brief Fullscreen fragment passes recorded natively on the GL targets the
 *        viewer binds (the building block of the plus graphics passes).
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

#ifndef LL_LLVKFULLSCREEN_H
#define LL_LLVKFULLSCREEN_H

#include "llvktracker.h"

#include <map>

// A pass is one fragment shader drawn as a single triangle over the whole
// target that LLVKTracker::beginPass opened. Its descriptor set 0 holds the
// uniform block the shader declares at binding 0 (in the module and the set
// layout at LLVKShader::UNIFORM_BLOCK_BINDING_SHIFT, as LLVKShader::build
// shifts uniform blocks) and combined image samplers at bindings 1..N, all
// for the fragment stage. Pipelines are made per target format on first
// use and kept. Used under LLVKTracker::lock(), between beginPass and endPass.
namespace LLVKFullscreen
{
    // How the fragment's output meets the target's.
    enum class Blend
    {
        Replace,            // written as it is
        Add,                // added to the target's RGB (alpha kept)
        Min,                // the smaller of the two, per channel
        PremultipliedOver,  // RGB laid over the target's: src + dst * (1 - src alpha) (alpha kept)
    };

    // A stencil test the pass's pipelines bake in: the pass draws only where
    // (stencil & compare_mask) == (reference & compare_mask). The stencil is
    // never written, so the target's depth may be attached read-only. A pass
    // with the test draws nothing on a target without stencil.
    struct StencilTest
    {
        bool enabled = false;
        U32 reference = 0;
        U32 compare_mask = 0;
    };

    constexpr VkColorComponentFlags WRITE_RGBA = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    constexpr U32 MAX_IMAGES = 8;
    // Colour attachments a pass may write at once (a G-buffer; LLVKMirror::Target's colors).
    constexpr U32 MAX_COLORS = 4;

    // A GL texture the pass samples, in the layout it is in during the pass.
    // Or, with view set, an image of the backend's own (not a GL texture:
    // texture is then ignored), sampled through view and sampler in layout;
    // its owner moves it to that layout before the pass.
    struct Image
    {
        LLVKTracker::GLName texture = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    class Pass
    {
    public:
        // Compiles the fragment shader and makes the layouts; false (logged
        // with name, a string literal) when that fails. image_count: at most
        // MAX_IMAGES. write_mask: the target channels the pass writes (with
        // Blend::Add and Blend::PremultipliedOver only RGB are ever written).
        bool init(const char* name, const char* fragment_glsl, U32 image_count, Blend blend = Blend::Replace,
                  VkColorComponentFlags write_mask = WRITE_RGBA, const StencilTest& stencil = StencilTest());

        // The same for a pass that writes several colour attachments at once
        // (a G-buffer): output location i goes to attachment i as it is
        // (Blend::Replace) through write_masks[i]. color_count: 1..MAX_COLORS;
        // the pass draws only on targets with exactly that many attachments.
        bool initColors(const char* name, const char* fragment_glsl, U32 image_count, U32 color_count,
                        const VkColorComponentFlags* write_masks, const StencilTest& stencil = StencilTest());

        // Records the pass on target (as beginPass gave it): the uniforms go
        // to the frame's ring, the images are bound at bindings 1..count.
        // False when nothing was recorded (no pipeline for the target's
        // format, a stencil test on a target without stencil, or the frame's
        // ring or descriptor pool is full).
        bool draw(VkCommandBuffer cmd, const LLVKMirror::Target& target, const void* uniforms, size_t uniform_size,
                  const Image* images, U32 image_count);

        // The name given to init (for log lines).
        const char* name() const { return mName; }

    private:
        struct TargetKey
        {
            VkFormat colors[MAX_COLORS] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
            VkFormat depth = VK_FORMAT_UNDEFINED;
            bool stencil = false;
            bool operator<(const TargetKey& other) const;
        };

        // init's and initColors' common part, once the outputs are set.
        bool build(const char* name, const char* fragment_glsl, U32 image_count, const StencilTest& stencil);
        VkPipeline pipeline(const LLVKMirror::Target& target);
        VkDescriptorSet uniformSet(const void* uniforms, size_t size);

        const char* mName = "";
        VkShaderModule mFragment = VK_NULL_HANDLE;
        U32 mImageCount = 0;
        Blend mBlend = Blend::Replace;      // attachment 0's (the others are written as they are)
        U32 mColorCount = 1;
        VkColorComponentFlags mWriteMasks[MAX_COLORS] = { WRITE_RGBA, WRITE_RGBA, WRITE_RGBA, WRITE_RGBA };
        StencilTest mStencil;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        std::map<TargetKey, VkPipeline> mPipelines;     // null where making it failed (not retried)
    };

    // Window depth (as GL's depth range stores it) to NDC z: ndc = depth * out[0] + out[1].
    // Depth clip control (required, research/124 V1) keeps GL's -1..1 range, stored as [0, 1].
    void depthToNDC(F32 out[2]);
}

#endif // LL_LLVKFULLSCREEN_H
