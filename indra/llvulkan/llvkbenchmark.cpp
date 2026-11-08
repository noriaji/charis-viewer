/**
 * @file llvkbenchmark.cpp
 * @brief The GPU benchmark the viewer's GPU class comes from, on Vulkan (research/117 9.3).
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

#include "llvkbenchmark.h"

#include "llvkdevice.h"
#include "llvkshader.h"

#include "vk_mem_alloc.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // As gpu_benchmark() (llglsandbox.cpp): 32 textures and a target of
    // 1024x1024 RGBA8, 64 rounds of one full-screen triangle per texture.
    constexpr U32 RES = 1024;
    constexpr U32 TEXTURES = 32;
    constexpr U32 ROUNDS = 64;
    constexpr VkFormat FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

    // benchmarkV.glsl and benchmarkF.glsl: one triangle over the target
    // (vertices 0..2), the texture read where it lands.
    const char* const VERTEX = R"GLSL(#version 450
layout(location = 0) out vec2 tc0;
void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    tc0 = corner;
}
)GLSL";

    const char* const FRAGMENT = R"GLSL(#version 450
layout(binding = 0) uniform sampler2D diffuseMap;
layout(location = 0) in vec2 tc0;
layout(location = 0) out vec4 frag_color;
void main()
{
    frag_color = texture(diffuseMap, tc0);
}
)GLSL";

    // Everything the benchmark makes, destroyed in reverse order (nothing of
    // it is in use then: every submission is waited for).
    struct Resources
    {
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = nullptr;
        VkImage images[TEXTURES] = {};
        VmaAllocation image_memory[TEXTURES] = {};
        VkImageView views[TEXTURES] = {};
        VkImage target = VK_NULL_HANDLE;
        VmaAllocation target_memory = nullptr;
        VkImageView target_view = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation staging_memory = nullptr;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[TEXTURES] = {};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        LLVKShader::Program program;

        ~Resources()
        {
            if (!device)
            {
                return;
            }
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (queries) vkDestroyQueryPool(device, queries, nullptr);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);   // frees `commands`
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);     // frees `sets`
            if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            if (staging) vmaDestroyBuffer(allocator, staging, staging_memory);
            if (target_view) vkDestroyImageView(device, target_view, nullptr);
            if (target) vmaDestroyImage(allocator, target, target_memory);
            for (U32 i = 0; i < TEXTURES; ++i)
            {
                if (views[i]) vkDestroyImageView(device, views[i], nullptr);
                if (images[i]) vmaDestroyImage(allocator, images[i], image_memory[i]);
            }
            LLVKShader::destroy(program);
        }
    };

    bool make_image(Resources& r, VkImageUsageFlags usage, VkImage& image, VmaAllocation& memory, VkImageView& view)
    {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = FORMAT;
        info.extent = { RES, RES, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(r.allocator, &info, &gpu, &image, &memory, nullptr) != VK_SUCCESS)
        {
            image = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = FORMAT;
        view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return vkCreateImageView(r.device, &view_info, nullptr, &view) == VK_SUCCESS;
    }

    void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
    {
        VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // Records into r.commands with `record`, submits and waits.
    template <typename F>
    bool submit_and_wait(Resources& r, F&& record)
    {
        if (vkResetCommandPool(r.device, r.command_pool, 0) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(r.commands, &begin) != VK_SUCCESS)
        {
            return false;
        }
        record(r.commands);
        if (vkEndCommandBuffer(r.commands) != VK_SUCCESS)
        {
            return false;
        }
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &r.commands;
        if (vkResetFences(r.device, 1, &r.fence) != VK_SUCCESS || LLVKDevice::instance().submit(submit, r.fence) != VK_SUCCESS)
        {
            return false;
        }
        return vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }

    bool setup(Resources& r)
    {
        LLVKDevice& device = LLVKDevice::instance();
        // Textures (sampled, filled from the staging buffer) and the target.
        for (U32 i = 0; i < TEXTURES; ++i)
        {
            if (!make_image(r, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, r.images[i], r.image_memory[i], r.views[i]))
            {
                return false;
            }
        }
        if (!make_image(r, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, r.target, r.target_memory, r.target_view))
        {
            return false;
        }
        // Random texels, as gpu_benchmark's (compression must not help).
        VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size = (VkDeviceSize)RES * RES * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo host = {};
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped = {};
        if (vmaCreateBuffer(r.allocator, &buffer_info, &host, &r.staging, &r.staging_memory, &mapped) != VK_SUCCESS || !mapped.pMappedData)
        {
            return false;
        }
        {
            std::mt19937 random(12345);
            U32* texels = (U32*)mapped.pMappedData;
            for (U32 i = 0; i < RES * RES; ++i)
            {
                texels[i] = random();
            }
            vmaFlushAllocation(r.allocator, r.staging_memory, 0, VK_WHOLE_SIZE);
        }

        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = 0.f;
        if (vkCreateSampler(r.device, &sampler_info, nullptr, &r.sampler) != VK_SUCCESS)
        {
            return false;
        }

        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 1;
        set_info.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(r.device, &set_info, nullptr, &r.set_layout) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURES };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = TEXTURES;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(r.device, &pool_info, nullptr, &r.descriptor_pool) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorSetLayout layouts[TEXTURES];
        for (U32 i = 0; i < TEXTURES; ++i)
        {
            layouts[i] = r.set_layout;
        }
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = r.descriptor_pool;
        alloc.descriptorSetCount = TEXTURES;
        alloc.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(r.device, &alloc, r.sets) != VK_SUCCESS)
        {
            return false;
        }
        for (U32 i = 0; i < TEXTURES; ++i)
        {
            VkDescriptorImageInfo image = { r.sampler, r.views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = r.sets[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            vkUpdateDescriptorSets(r.device, 1, &write, 0, nullptr);
        }

        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &r.set_layout;
        if (vkCreatePipelineLayout(r.device, &layout_info, nullptr, &r.layout) != VK_SUCCESS)
        {
            return false;
        }
        if (!LLVKShader::build({ { LLVKShader::Stage::Vertex, VERTEX }, { LLVKShader::Stage::Fragment, FRAGMENT } }, r.program) ||
            r.program.modules.size() != 2)
        {
            LL_WARNS("Benchmark") << "Vulkan benchmark shaders failed: " << r.program.log << LL_ENDL;
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {};
        for (U32 i = 0; i < 2; ++i)
        {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = r.program.stages[i];
            stages[i].module = r.program.modules[i];
            stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport = { 0.f, 0.f, (F32)RES, (F32)RES, 0.f, 1.f };
        VkRect2D scissor = { { 0, 0 }, { RES, RES } };
        VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = &viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment = {};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &FORMAT;
        VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline_info.pNext = &rendering;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.layout = r.layout;
        if (vkCreateGraphicsPipelines(r.device, device.usePipelineCache(), 1, &pipeline_info, nullptr, &r.pipeline) != VK_SUCCESS)
        {
            return false;
        }

        VkCommandPoolCreateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        command_info.queueFamilyIndex = device.getGraphicsQueueFamily();
        if (vkCreateCommandPool(r.device, &command_info, nullptr, &r.command_pool) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferAllocateInfo command_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_alloc.commandPool = r.command_pool;
        command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_alloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(r.device, &command_alloc, &r.commands) != VK_SUCCESS)
        {
            return false;
        }
        VkQueryPoolCreateInfo query_info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        if (vkCreateQueryPool(r.device, &query_info, nullptr, &r.queries) != VK_SUCCESS)
        {
            return false;
        }
        VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(r.device, &fence_info, nullptr, &r.fence) != VK_SUCCESS)
        {
            return false;
        }

        // The textures' contents and every image's first layout.
        return submit_and_wait(r, [&r](VkCommandBuffer cmd)
        {
            for (U32 i = 0; i < TEXTURES; ++i)
            {
                barrier(cmd, r.images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy copy = {};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.imageExtent = { RES, RES, 1 };
                vkCmdCopyBufferToImage(cmd, r.staging, r.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier(cmd, r.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            barrier(cmd, r.target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        });
    }

    // One timed run: seconds, or a negative value when it failed.
    F64 timed_run(Resources& r, F64 timestamp_period_ns, U64 timestamp_mask)
    {
        const bool recorded = submit_and_wait(r, [&r](VkCommandBuffer cmd)
        {
            vkCmdResetQueryPool(cmd, r.queries, 0, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r.queries, 0);
            VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            color.imageView = r.target_view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            rendering.renderArea = { { 0, 0 }, { RES, RES } };
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
            for (U32 round = 0; round < ROUNDS; ++round)
            {
                for (U32 i = 0; i < TEXTURES; ++i)
                {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 1, &r.sets[i], 0, nullptr);
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                }
            }
            vkCmdEndRendering(cmd);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r.queries, 1);
        });
        if (!recorded)
        {
            return -1.0;
        }
        U64 ticks[2] = {};
        if (vkGetQueryPoolResults(r.device, r.queries, 0, 2, sizeof(ticks), ticks, sizeof(U64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        {
            return -1.0;
        }
        const U64 elapsed = (ticks[1] - ticks[0]) & timestamp_mask;     // the counter may wrap at its valid bits
        return elapsed ? (F64)elapsed * timestamp_period_ns / 1e9 : -1.0;
    }
}

F32 LLVKBenchmark::memoryBandwidth(std::string (*sensors)())
{
    LLVKDevice& device = LLVKDevice::instance();
    if (!device.isReady())
    {
        return -1.f;
    }
    U32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.getPhysicalDevice(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.getPhysicalDevice(), &family_count, families.data());
    const F64 period_ns = device.getLimits().timestampPeriod;
    const U32 valid_bits = device.getGraphicsQueueFamily() < family_count ? families[device.getGraphicsQueueFamily()].timestampValidBits : 0;
    if (!valid_bits || period_ns <= 0.0)
    {
        LL_WARNS("Benchmark") << "Vulkan benchmark: the graphics queue has no timestamps" << LL_ENDL;
        return -1.f;
    }
    const U64 mask = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
    auto sensor_text = [sensors]() { return sensors ? sensors() : std::string(); };

    Resources r;
    r.device = device.getDevice();
    r.allocator = device.getAllocator();
    if (!setup(r))
    {
        LL_WARNS("Benchmark") << "Vulkan benchmark could not be set up" << LL_ENDL;
        return -1.f;
    }
    const F64 samples = (F64)ROUNDS * TEXTURES * RES * RES;
    auto gbps_of = [samples](F64 seconds) { return (F32)(samples / seconds / 1e9 * 4.0); };     // 4 bytes per sample
    const std::string idle_sensors = sensor_text();

    // research/117 9.5: gpu_benchmark's two back-to-back runs (about 43 ms
    // each on an RTX 2080) can still be on the GPU's way up from idle clocks;
    // its one GL measurement (201.0 GB/s) and the first Vulkan one (189.5,
    // the second of two runs, as gpu_benchmark) were 6 % apart, right across
    // the Class3/4 line (192). The same runs are made until the GPU has been
    // busy for WARM_SECONDS, then MEASURED (odd) runs give the median.
    constexpr F64 WARM_SECONDS = 0.5;       // GPU time
    constexpr F64 WARM_WALL_SECONDS = 3.0;  // a slow GPU stops warming here
    constexpr U32 WARM_RUNS_MAX = 100;
    const auto warm_start = std::chrono::steady_clock::now();
    F64 warm_gpu = 0.0;
    U32 warm_runs = 0;
    F32 first = 0.f, last = 0.f;
    F64 last_seconds = 0.0;
    while (warm_runs < WARM_RUNS_MAX && warm_gpu < WARM_SECONDS)
    {
        const F64 seconds = timed_run(r, period_ns, mask);
        if (seconds <= 0.0)
        {
            LL_WARNS("Benchmark") << "Vulkan benchmark warm-up run " << warm_runs + 1 << " failed" << LL_ENDL;
            return -1.f;
        }
        last = gbps_of(seconds);
        last_seconds = seconds;
        first = warm_runs ? first : last;
        warm_gpu += seconds;
        ++warm_runs;
        if (std::chrono::duration<F64>(std::chrono::steady_clock::now() - warm_start).count() > WARM_WALL_SECONDS)
        {
            break;
        }
    }
    const std::string warm_sensors = sensor_text();

    // Each run is gpu_benchmark's: one timestamp before the first of its
    // 2,048 draws and one after the last (as GL_TIME_ELAPSED from the program
    // bind to the unbind), samples = draws x 1024^2 (GL_SAMPLES_PASSED of
    // full-screen draws without depth or stencil). A slow GPU gets 3 runs.
    const U32 measured = last_seconds > 0.25 ? 3 : 7;
    std::vector<F32> results;
    for (U32 run = 0; run < measured; ++run)
    {
        const F64 seconds = timed_run(r, period_ns, mask);
        if (seconds <= 0.0)
        {
            LL_WARNS("Benchmark") << "Vulkan benchmark run " << run + 1 << " failed" << LL_ENDL;
            return -1.f;
        }
        results.push_back(gbps_of(seconds));
    }
    std::vector<F32> sorted = results;
    std::sort(sorted.begin(), sorted.end());
    const F32 median = sorted[sorted.size() / 2];

    std::ostringstream runs;
    for (size_t i = 0; i < results.size(); ++i)
    {
        runs << (i ? ", " : "") << llformat("%.1f", results[i]);
    }
    LL_INFOS("Benchmark") << "Vulkan benchmark warm-up: " << warm_runs << " runs, " << llformat("%.3f", warm_gpu) << " s on the GPU, "
                          << llformat("%.1f", first) << " -> " << llformat("%.1f", last) << " GB/sec; before" << idle_sensors
                          << "; after" << warm_sensors << LL_ENDL;
    LL_INFOS("Benchmark") << "Vulkan benchmark runs: " << runs.str() << " GB/sec (min " << llformat("%.1f", sorted.front())
                          << ", max " << llformat("%.1f", sorted.back()) << ")" << sensor_text() << LL_ENDL;
    LL_INFOS("Benchmark") << "Memory bandwidth, final run is " << llformat("%.3f", median)
                          << " GB/sec according to Vulkan timestamps (research/117: median of " << results.size()
                          << " runs after warm-up)" << LL_ENDL;
    return median;
}
