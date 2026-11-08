/**
 * @file llvkfullscreen.cpp
 * @brief Fullscreen fragment passes recorded natively on the GL targets the
 *        viewer binds.
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

#include "llvkfullscreen.h"

#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvkshader.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <cstring>
#include <tuple>

namespace
{
    // Where the uniform block a pass declares at binding 0 is in its module:
    // LLVKShader::build shifts uniform block bindings (samplers keep theirs).
    constexpr U32 UNIFORM_BINDING = LLVKShader::UNIFORM_BLOCK_BINDING_SHIFT;

    // One triangle over the whole target (vertices 0..2).
    const char* const FULLSCREEN_VERTEX = R"GLSL(#version 450
void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    // Shared by every pass; null when it failed to build.
    VkShaderModule fullscreenVertex()
    {
        static VkShaderModule module = []()
        {
            LLVKShader::Program program;
            if (!LLVKShader::build({ { LLVKShader::Stage::Vertex, FULLSCREEN_VERTEX } }, program))
            {
                LL_WARNS("VKFullscreen") << "Fullscreen vertex shader failed: " << program.log << LL_ENDL;
                return (VkShaderModule)VK_NULL_HANDLE;
            }
            return program.modules[0];
        }();
        return module;
    }

    VkPipelineColorBlendAttachmentState blendState(LLVKFullscreen::Blend blend, VkColorComponentFlags write_mask)
    {
        VkPipelineColorBlendAttachmentState state = {};
        state.colorWriteMask = write_mask;
        switch (blend)
        {
        case LLVKFullscreen::Blend::Replace:
            break;
        case LLVKFullscreen::Blend::Add:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.colorBlendOp = VK_BLEND_OP_ADD;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.alphaBlendOp = VK_BLEND_OP_ADD;
            state.colorWriteMask &= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            break;
        case LLVKFullscreen::Blend::Min:
            // The factors are ignored by MIN; they are set to valid values all the same.
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.colorBlendOp = VK_BLEND_OP_MIN;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.alphaBlendOp = VK_BLEND_OP_MIN;
            break;
        case LLVKFullscreen::Blend::PremultipliedOver:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.colorBlendOp = VK_BLEND_OP_ADD;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.alphaBlendOp = VK_BLEND_OP_ADD;
            state.colorWriteMask &= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            break;
        }
        return state;
    }

    // Tests the stencil without writing it (the ops keep it, the write mask is
    // 0): valid on a read-only depth-stencil attachment too.
    VkPipelineDepthStencilStateCreateInfo depthStencilState(const LLVKFullscreen::StencilTest& stencil)
    {
        VkPipelineDepthStencilStateCreateInfo state = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        if (stencil.enabled)
        {
            VkStencilOpState face = {};
            face.failOp = VK_STENCIL_OP_KEEP;
            face.passOp = VK_STENCIL_OP_KEEP;
            face.depthFailOp = VK_STENCIL_OP_KEEP;
            face.compareOp = VK_COMPARE_OP_EQUAL;
            face.compareMask = stencil.compare_mask;
            face.writeMask = 0;
            face.reference = stencil.reference;
            state.stencilTestEnable = VK_TRUE;
            state.front = face;
            state.back = face;
        }
        return state;
    }
}

bool LLVKFullscreen::Pass::TargetKey::operator<(const TargetKey& other) const
{
    static_assert(MAX_COLORS == 4, "TargetKey compares four colour formats");
    return std::tie(colors[0], colors[1], colors[2], colors[3], depth, stencil) <
           std::tie(other.colors[0], other.colors[1], other.colors[2], other.colors[3], other.depth, other.stencil);
}

bool LLVKFullscreen::Pass::init(const char* name, const char* fragment_glsl, U32 image_count, Blend blend,
                                VkColorComponentFlags write_mask, const StencilTest& stencil)
{
    mBlend = blend;
    mColorCount = 1;
    mWriteMasks[0] = write_mask;
    return build(name, fragment_glsl, image_count, stencil);
}

bool LLVKFullscreen::Pass::initColors(const char* name, const char* fragment_glsl, U32 image_count, U32 color_count,
                                      const VkColorComponentFlags* write_masks, const StencilTest& stencil)
{
    llassert(color_count >= 1 && color_count <= MAX_COLORS);
    mBlend = Blend::Replace;
    mColorCount = llclamp(color_count, 1u, MAX_COLORS);
    for (U32 i = 0; i < mColorCount; ++i)
    {
        mWriteMasks[i] = write_masks[i];
    }
    return build(name, fragment_glsl, image_count, stencil);
}

bool LLVKFullscreen::Pass::build(const char* name, const char* fragment_glsl, U32 image_count, const StencilTest& stencil)
{
    llassert(image_count <= MAX_IMAGES);
    mName = name;
    mImageCount = llmin(image_count, MAX_IMAGES);
    mStencil = stencil;

    LLVKShader::Program fragment;
    if (!fullscreenVertex() || !LLVKShader::build({ { LLVKShader::Stage::Fragment, fragment_glsl } }, fragment))
    {
        LL_WARNS("VKFullscreen") << mName << ": shader failed: " << fragment.log << LL_ENDL;
        return false;
    }
    mFragment = fragment.modules[0];

    // The block the shader declares at binding 0 is at UNIFORM_BINDING in the module (LLVKShader's shift).
    VkDescriptorSetLayoutBinding bindings[MAX_IMAGES + 1] = {};
    for (U32 i = 0; i <= mImageCount; ++i)
    {
        bindings[i].binding = i == 0 ? UNIFORM_BINDING : i;
        bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDevice device = LLVKDevice::instance().getDevice();
    VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_info.bindingCount = mImageCount + 1;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &mSetLayout) != VK_SUCCESS)
    {
        return false;
    }
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &mSetLayout;
    return vkCreatePipelineLayout(device, &layout_info, nullptr, &mLayout) == VK_SUCCESS;
}

// No depth test, no depth or stencil writes, no culling; the pass's stencil
// test if it has one; viewport and scissor dynamic.
VkPipeline LLVKFullscreen::Pass::pipeline(const LLVKMirror::Target& target)
{
    TargetKey key;
    for (U32 i = 0; i < mColorCount; ++i)
    {
        key.colors[i] = target.color_formats[i];
    }
    key.depth = target.depth_format;
    key.stencil = target.stencil;
    auto it = mPipelines.find(key);
    if (it != mPipelines.end())
    {
        return it->second;
    }
    if (mStencil.enabled && !key.stencil)
    {
        // Without the test the pass would draw everywhere.
        LL_WARNS("VKFullscreen") << mName << ": needs a stencil attachment; not drawn on depth format " << (U32)key.depth << LL_ENDL;
        mPipelines[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = fullscreenVertex();
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mFragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineDepthStencilStateCreateInfo depth = depthStencilState(mStencil);
    VkPipelineColorBlendAttachmentState blends[MAX_COLORS];
    for (U32 i = 0; i < mColorCount; ++i)
    {
        blends[i] = blendState(i == 0 ? mBlend : Blend::Replace, mWriteMasks[i]);
    }
    VkPipelineColorBlendStateCreateInfo color = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    color.attachmentCount = mColorCount;
    color.pAttachments = blends;
    const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = mColorCount;
    rendering.pColorAttachmentFormats = key.colors;
    rendering.depthAttachmentFormat = key.depth;
    rendering.stencilAttachmentFormat = key.stencil ? key.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.pNext = &rendering;
    info.stageCount = 2;
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
    if (vkCreateGraphicsPipelines(LLVKDevice::instance().getDevice(), LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr,
                                  &result) != VK_SUCCESS)
    {
        result = VK_NULL_HANDLE;
        LL_WARNS("VKFullscreen") << mName << ": no pipeline for color format " << (U32)key.colors[0] << " (" << mColorCount
                                 << " attachments), depth format " << (U32)key.depth << LL_ENDL;
    }
    mPipelines[key] = result;
    return result;
}

VkDescriptorSet LLVKFullscreen::Pass::uniformSet(const void* uniforms, size_t size)
{
    LLVKMirror& mirror = LLVKMirror::instance();
    const VkDeviceSize alignment = LLVKDevice::instance().getLimits().minUniformBufferOffsetAlignment;
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* data = nullptr;
    VkDescriptorSet set = mirror.allocateSet(mSetLayout);
    if (!set || !mirror.allocate(size, alignment, ring, offset, data))
    {
        return VK_NULL_HANDLE;
    }
    memcpy(data, uniforms, size);
    const VkDescriptorBufferInfo buffer = { ring, offset, size };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = UNIFORM_BINDING;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), 1, &write, 0, nullptr);
    return set;
}

bool LLVKFullscreen::Pass::draw(VkCommandBuffer cmd, const LLVKMirror::Target& target, const void* uniforms, size_t uniform_size,
                                const Image* images, U32 image_count)
{
    llassert(image_count == mImageCount);
    if (target.color_count != mColorCount || image_count != mImageCount)
    {
        return false;
    }
    VkPipeline pipeline = this->pipeline(target);
    VkDescriptorSet set = pipeline ? uniformSet(uniforms, uniform_size) : VK_NULL_HANDLE;
    if (!set)
    {
        return false;
    }

    VkDescriptorImageInfo infos[MAX_IMAGES];
    VkWriteDescriptorSet writes[MAX_IMAGES];
    for (U32 i = 0; i < image_count; ++i)
    {
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        if (images[i].view)
        {
            infos[i] = { images[i].sampler, images[i].view, images[i].layout };    // the backend's own image
        }
        else
        {
            const LLVKTracker::Sampled sampled = LLVKTracker::sampledTexture(images[i].texture);
            infos[i] = { sampled.sampler, sampled.view, images[i].layout };
        }
        writes[i].dstSet = set;
        writes[i].dstBinding = i + 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    if (image_count)
    {
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), image_count, writes, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &set, 0, nullptr);
    const VkViewport viewport = { 0.f, 0.f, (F32)target.width, (F32)target.height, 0.f, 1.f };
    const VkRect2D scissor = { { 0, 0 }, { target.width, target.height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    return true;
}

void LLVKFullscreen::depthToNDC(F32 out[2])
{
    out[0] = 2.f;
    out[1] = -1.f;
}
