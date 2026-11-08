/**
 * @file llvkgl_oit.cpp
 * @brief Order-independent transparency sessions: per-pixel node lists and their resolve (research/41, research/91).
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
    // ------------------------------------------------------------------
    // Order-independent transparency sessions (research/41; the lists and
    // the shaders are described with OIT_FRAGMENT_CODE).
    //
    // oit_begin: on the bound draw framebuffer (one color attachment and a
    // depth attachment), clears the list heads and the node count and opens
    // the session. Draws into that target then write nodes (draw_one: the
    // program's OIT variant, oit_variant), others draw as before.
    // oit_resolve: copies the target's color, composites every pixel's nodes
    // over the copy into the target, and copies the node count for the size
    // check (oit_harvest, once the frame slot comes round again).
    //
    // Resources: the heads image (r32ui), the copy (the target's format) and
    // the rest image (r32ui x OIT_REST_LAYERS, made after a first overflow)
    // follow the target's size; the node buffer grows. They are used by the
    // GPU in submission order only (no CPU access): one set serves every
    // frame in flight, the barriers at each begin and resolve order a session
    // after the ones before it, also those of the previous frame. Per frame
    // slot there are the descriptor sets (one per session) and the count
    // readback, as the CPU writes or reads those.

    OITState sOIT;

namespace
{

    // LLRHI_OIT_LAYERS: nodes per pixel the node buffer starts with (4);
    // LLRHI_OIT_GROW_LAYERS: per pixel it grows to at most (8; fragments past
    // it are summed into the rest image); LLRHI_OIT_MAX_MB: its largest size
    // in any case (768); LLRHI_OIT_MAX_LAYERS: nodes of a pixel the resolve
    // keeps in order, the nearest (16, at most OIT_MAX_LAYERS; the farther
    // are summed behind them).
    // A whole number from low to high (above 0 but outside it: the nearest end); unset, 0 or not
    // a whole number (LLRHI::envNumber logs that): fallback.
    U32 env_count(const char* name, U32 low, U32 high, U32 fallback)
    {
        U64 value = 0;
        LLRHI::envNumber(name, 0, ~(U64)0, value);
        return value > 0 ? (U32)llclamp(value, (U64)low, (U64)high) : fallback;
    }
    const U32 sOITLayers = env_count("LLRHI_OIT_LAYERS", 1, 64, 4);
    const U32 sOITGrowLayers = llmax(env_count("LLRHI_OIT_GROW_LAYERS", 1, 64, 8), sOITLayers);
    const U32 sOITMaxLayers = env_count("LLRHI_OIT_MAX_LAYERS", 1, OIT_MAX_LAYERS, 16);
    const U32 sOITMaxMB = env_count("LLRHI_OIT_MAX_MB", 16, 4000, 768);
    constexpr U32 OIT_SET_BINDINGS = 6;
    constexpr VkShaderStageFlags OIT_RESOLVE_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr U32 OIT_HEADER_WORDS = OIT_HEADER_BYTES / 4;
    // Layers this close behind each other count as one surface and go by
    // their priority (clothing layers lie less than the depth precision
    // apart, and a per-pixel depth sort would speckle them):
    // LLRHI_OIT_TOLERANCE_MM (4 mm; 0: only equal depths) near the eye, and
    // from where that is less than LLRHI_OIT_TOLERANCE_REL of the distance
    // (0.0005: 8 m on), that. A surface is at most LLRHI_OIT_SPAN (4) such
    // gaps deep, so a long run of leaves is not reordered by priority.
    const F32 sOITTolerance = []()
    {
        const char* env = std::getenv("LLRHI_OIT_TOLERANCE_MM");
        return env && *env ? llclamp((F32)atof(env), 0.f, 1000.f) / 1000.f : 0.004f;
    }();
    const F32 sOITRelative = []()
    {
        const char* env = std::getenv("LLRHI_OIT_TOLERANCE_REL");
        return env && *env ? llclamp((F32)atof(env), 0.f, 0.1f) : 0.0005f;
    }();
    const F32 sOITSpan = []()
    {
        const char* env = std::getenv("LLRHI_OIT_SPAN");
        return env && *env ? llclamp((F32)atof(env), 1.f, 100.f) : 4.f;
    }();
    // The resolve pass's push constants (OIT_RESOLVE_FRAGMENT).
    struct OITResolveConstants
    {
        U32 clamp_colors = 0;
        F32 depth_a = 0.f;
        F32 depth_b = 0.f;
        F32 ndc_scale = 2.f;
        F32 ndc_bias = -1.f;
        F32 tolerance = 0.f;
        F32 relative = 0.f;
        F32 span = 4.f;
        F32 width = 1.f;                // the vertex stage's (OIT_RESOLVE_VERTEX)
        F32 height = 1.f;
    };
    // A readback entry: the header as the resolve leaves it, then the count
    // as the avatar half of a hybrid session left it (oit_world).
    constexpr U32 OIT_READBACK_BYTES = OIT_HEADER_BYTES + 16;
    constexpr U32 OIT_READBACK_WORDS = OIT_READBACK_BYTES / 4;
    // Hybrid: after this many sessions in a row whose avatar half appended no
    // node, the world draws as before while the avatar half draws no more
    // than then (oit_world).
    constexpr U32 OIT_EMPTY_STREAK = 3;

    void oit_destroy_image(OITImage& img)
    {
        if (img.image)
        {
            VkImage image = img.image;
            VmaAllocation memory = img.memory;
            VkImageView view = img.view;
            LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_RELEASED, (U64)image, memory, "OIT image");    // research/109
            LLVKMirror::instance().defer([image, memory, view]()
            {
                LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_FREED, (U64)image, memory, "OIT image");
                if (view)
                {
                    vkDestroyImageView(LLVKDevice::instance().getDevice(), view, nullptr);
                }
                vmaDestroyImage(LLVKDevice::instance().getAllocator(), image, memory);
            });
        }
        img = OITImage();
    }

    bool oit_make_image(OITImage& img, VkFormat format, U32 width, U32 height, U32 layers, VkImageUsageFlags usage)
    {
        img = OITImage();
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(LLVKDevice::instance().getAllocator(), &info, &gpu, &img.image, &img.memory, nullptr) != VK_SUCCESS)
        {
            img = OITImage();
            return false;
        }
        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = img.image;
        view.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
        if (vkCreateImageView(LLVKDevice::instance().getDevice(), &view, nullptr, &img.view) != VK_SUCCESS)
        {
            vmaDestroyImage(LLVKDevice::instance().getAllocator(), img.image, img.memory);
            img = OITImage();
            return false;
        }
        img.format = format;
        char what[56];
        snprintf(what, sizeof(what), "OIT image %ux%ux%u format %d", width, height, layers, (S32)format);
        LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_MADE, (U64)img.image, img.memory, what);    // research/109
        return true;
    }

    // The node buffer goes once the GPU is done with what was recorded so far.
    void oit_release_nodes()
    {
        if (sOIT.nodes)
        {
            VkBuffer old = sOIT.nodes;
            VmaAllocation old_memory = sOIT.nodes_memory;
            LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_RELEASED, (U64)old, old_memory, "OIT node buffer");   // research/109
            LLVKMirror::instance().defer([old, old_memory]()
            {
                LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_FREED, (U64)old, old_memory, "OIT node buffer");
                vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), old, old_memory);
            });
        }
        sOIT.nodes = VK_NULL_HANDLE;
        sOIT.nodes_memory = nullptr;
        sOIT.capacity = 0;
    }

    // The device has what OIT needs: fragment stores, r32ui image atomics.
    bool oit_capable()
    {
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        return LLVKDevice::instance().hasFragmentStores() && (format_features(VK_FORMAT_R32_UINT) & needed) == needed;
    }

    // The bounds pass (OIT_BOUNDS_COMPUTE); copy: the GLSL format qualifier
    // of the copy it writes too (null: without). Null: it did not build.
    VkPipeline oit_bounds_pipeline(const char* copy)
    {
        std::string source = "#version 450\n";
        if (copy)
        {
            source += std::string("#define COPY ") + copy + "\n";
        }
        source += OIT_BOUNDS_COMPUTE;
        LLVKShader::Program program;
        if (!LLVKShader::build({ { LLVKShader::Stage::Compute, source } }, program) || program.modules.empty())
        {
            LL_WARNS("VKGL") << "OIT: bounds pass " << (copy ? copy : "") << " failed: " << program.log << LL_ENDL;
            return VK_NULL_HANDLE;
        }
        VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = program.modules[0];
        info.stage.pName = "main";
        info.layout = sOIT.bounds_layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(LLVKDevice::instance().getDevice(), LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr,
                                     &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
        }
        sOIT.bounds_programs.push_back(program);   // never destroyed (as the resolve's)
        return pipeline;
    }

    // The GLSL storage format qualifier of a copy in this format (null: the
    // bounds pass does not write it; the resolve copies the whole target).
    const char* oit_copy_qualifier(VkFormat format)
    {
        if (!(format_features(format) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        {
            return nullptr;
        }
        switch (format)
        {
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "rgba16f";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "rgba32f";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "r11f_g11f_b10f";
        case VK_FORMAT_R8G8B8A8_UNORM: return "rgba8";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "rgb10_a2";
        case VK_FORMAT_R16G16B16A16_UNORM: return "rgba16";
        default: return nullptr;
        }
    }

    // Once: the device can do it, layouts, shaders, sets and readbacks.
    bool oit_init()
    {
        if (sOIT.tried)
        {
            return sOIT.ready;
        }
        sOIT.tried = true;
        LLVKDevice& device = LLVKDevice::instance();
        if (!oit_capable())
        {
            LL_WARNS("VKGL") << "OIT: no fragment stores or r32ui image atomics: alpha stays sorted" << LL_ENDL;
            return false;
        }
        VkDevice vk = device.getDevice();
        constexpr VkShaderStageFlags FRAGMENT = VK_SHADER_STAGE_FRAGMENT_BIT;
        constexpr VkShaderStageFlags COMPUTE = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorSetLayoutBinding bindings[OIT_SET_BINDINGS] =
        {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, FRAGMENT | COMPUTE, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FRAGMENT | COMPUTE | VK_SHADER_STAGE_VERTEX_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, FRAGMENT, nullptr },
            { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FRAGMENT, nullptr },
            { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, COMPUTE, nullptr },
            { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, COMPUTE, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = OIT_SET_BINDINGS;
        set_info.pBindings = bindings;
        VkDescriptorSetLayoutCreateInfo empty_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        if (vkCreateDescriptorSetLayout(vk, &set_info, nullptr, &sOIT.set_layout) != VK_SUCCESS ||
            vkCreateDescriptorSetLayout(vk, &empty_info, nullptr, &sOIT.empty_layout) != VK_SUCCESS)
        {
            LL_WARNS("VKGL") << "OIT: set layouts failed" << LL_ENDL;
            return false;
        }
        const VkPushConstantRange range = { OIT_RESOLVE_STAGES, 0, sizeof(OITResolveConstants) };
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &sOIT.set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        const VkPushConstantRange bounds_range = { COMPUTE, 0, 8 };
        VkPipelineLayoutCreateInfo bounds_info = layout_info;
        bounds_info.pPushConstantRanges = &bounds_range;
        if (vkCreatePipelineLayout(vk, &layout_info, nullptr, &sOIT.resolve_layout) != VK_SUCCESS ||
            vkCreatePipelineLayout(vk, &bounds_info, nullptr, &sOIT.bounds_layout) != VK_SUCCESS)
        {
            LL_WARNS("VKGL") << "OIT: resolve layouts failed" << LL_ENDL;
            return false;
        }
        if (!LLVKShader::build({ { LLVKShader::Stage::Vertex, OIT_RESOLVE_VERTEX } }, sOIT.resolve_vertex) ||
            !LLVKShader::build({ { LLVKShader::Stage::Fragment, OIT_RESOLVE_FRAGMENT } }, sOIT.resolve_fragment))
        {
            LL_WARNS("VKGL") << "OIT: resolve shaders failed: " << sOIT.resolve_vertex.log << sOIT.resolve_fragment.log << LL_ENDL;
            return false;
        }
        sOIT.bounds = oit_bounds_pipeline(nullptr);
        if (!sOIT.bounds)
        {
            return false;
        }
        VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(vk, &sampler, nullptr, &sOIT.sampler) != VK_SUCCESS)
        {
            return false;
        }
        constexpr U32 SETS = LLVKMirror::FRAMES_IN_FLIGHT * OIT_SESSIONS_PER_FRAME;
        const VkDescriptorPoolSize sizes[3] =
        {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS * 3 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS * 2 },
        };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = SETS;
        pool_info.poolSizeCount = 3;
        pool_info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(vk, &pool_info, nullptr, &sOIT.pool) != VK_SUCCESS)
        {
            return false;
        }
        for (U32 s = 0; s < LLVKMirror::FRAMES_IN_FLIGHT; ++s)
        {
            VkDescriptorSetLayout layouts[OIT_SESSIONS_PER_FRAME];
            std::fill(std::begin(layouts), std::end(layouts), sOIT.set_layout);
            VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            alloc.descriptorPool = sOIT.pool;
            alloc.descriptorSetCount = OIT_SESSIONS_PER_FRAME;
            alloc.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(vk, &alloc, sOIT.sets[s]) != VK_SUCCESS)
            {
                return false;
            }
            OITReadback& rb = sOIT.readback[s];
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = OIT_SESSIONS_PER_FRAME * OIT_READBACK_BYTES;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo host = {};
            host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped = {};
            if (vmaCreateBuffer(device.getAllocator(), &info, &host, &rb.buffer, &rb.memory, &mapped) != VK_SUCCESS || !mapped.pMappedData)
            {
                return false;
            }
            rb.words = (const U32*)mapped.pMappedData;
            memset(mapped.pMappedData, 0, (size_t)info.size);
            vmaFlushAllocation(device.getAllocator(), rb.memory, 0, VK_WHOLE_SIZE);
        }
        if (!oit_make_image(sOIT.dummy_rest, VK_FORMAT_R32_UINT, 1, 1, OIT_REST_LAYERS, VK_IMAGE_USAGE_STORAGE_BIT) ||
            !oit_make_image(sOIT.dummy_storage, VK_FORMAT_R32_UINT, 1, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT))
        {
            return false;
        }
        const U64 max_bytes = llmin((U64)sOITMaxMB << 20, (U64)device.getLimits().maxStorageBufferRange);
        sOIT.max_nodes = (U32)llmin((max_bytes - OIT_HEADER_BYTES) / (OIT_NODE_WORDS * 4), (U64)0x7fffffffu / OIT_NODE_WORDS);
        sOIT.ready = true;
        LL_INFOS("VKGL") << "OIT ready: " << sOITLayers << " nodes per pixel to start with, at most " << sOIT.max_nodes << " nodes" << LL_ENDL;
        return true;
    }

    // The counts a frame slot's last sessions wrote: how many nodes they
    // wanted. Only once the slot is recorded again (its fence was waited for).
    void oit_harvest(OITReadback& rb)
    {
        if (!rb.sessions || !rb.frame_begin)
        {
            rb.sessions = 0;
            std::fill(std::begin(rb.copied), std::end(rb.copied), false);
            std::fill(std::begin(rb.avatar_copied), std::end(rb.avatar_copied), false);
            return;
        }
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), rb.memory, 0, VK_WHOLE_SIZE);
        U32 need = 0;
        for (U32 i = 0; i < rb.sessions; ++i)
        {
            if (rb.avatar_copied[i])
            {
                // Hybrid: whether the avatar half appended anything (oit_world's rule).
                rb.avatar_copied[i] = false;
                if (rb.words[i * OIT_READBACK_WORDS + OIT_HEADER_WORDS])
                {
                    sOIT.empty_streak = 0;
                    sOIT.empty_draws = 0;
                }
                else
                {
                    ++sOIT.empty_streak;
                    sOIT.empty_draws = llmax(sOIT.empty_draws, rb.avatar_draws[i]);
                }
            }
            if (!rb.copied[i])
            {
                continue;   // dropped or empty: nothing was copied there
            }
            rb.copied[i] = false;
            const U32 count = rb.words[i * OIT_READBACK_WORDS];
            const U32 capacity = rb.words[i * OIT_READBACK_WORDS + 1];
            need = llmax(need, count);
            if (count > capacity)
            {
                ++sOIT.overflows;           // fragments were summed into the rest, or lost
                sOIT.overflow_need = llmax(sOIT.overflow_need, count);
            }
        }
        rb.sessions = 0;
        sOIT.peak = llmax(sOIT.peak, need);
        // Grow before it runs out: half again what was used, up to
        // LLRHI_OIT_GROW_LAYERS per pixel (more is summed into the rest).
        if (need > sOIT.capacity - sOIT.capacity / 8)
        {
            const U64 most = llmin((U64)sOIT.width * sOIT.height * sOITGrowLayers, (U64)sOIT.max_nodes);
            sOIT.wanted = llmax(sOIT.wanted, (U32)llmin((U64)need + need / 2, most));
        }
    }

    // The sized resources for a target of width x height with this color format.
    bool oit_resources(U32 width, U32 height, VkFormat color_format)
    {
        const U64 pixels = (U64)width * height;
        if (width != sOIT.width || height != sOIT.height || !sOIT.heads.image)
        {
            oit_destroy_image(sOIT.heads);
            oit_destroy_image(sOIT.copy);
            oit_destroy_image(sOIT.rest);
            sOIT.width = sOIT.height = 0;
            if (!oit_make_image(sOIT.heads, VK_FORMAT_R32_UINT, width, height, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            {
                LL_WARNS("VKGL") << "OIT: no heads image " << width << "x" << height << LL_ENDL;
                return false;
            }
            sOIT.width = width;
            sOIT.height = height;
            const U32 start = (U32)llmin(pixels * sOITLayers, (U64)sOIT.max_nodes);
            if (sOIT.nodes && sOIT.capacity / 2 > start)
            {
                // research/109: a buffer grown for a larger target (a 3840x2160
                // snapshot: 759 MB) goes back to this size's start; kept, it
                // held that VRAM for the rest of the session. It grows again
                // when the lists need more (oit_harvest).
                oit_release_nodes();
                sOIT.wanted = start;
            }
            else
            {
                sOIT.wanted = llmax(sOIT.wanted, start);
            }
        }
        if (sOIT.copy.format != color_format)
        {
            // The bounds pass writes the copy where it can (only the pixels with
            // a list), else the resolve copies the whole target into it.
            oit_destroy_image(sOIT.copy);
            const char* qualifier = oit_copy_qualifier(color_format);
            sOIT.copy_storage = qualifier && oit_make_image(sOIT.copy, color_format, width, height, 1,
                                                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                VK_IMAGE_USAGE_STORAGE_BIT);
            if (!sOIT.copy_storage &&
                !oit_make_image(sOIT.copy, color_format, width, height, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            {
                LL_WARNS("VKGL") << "OIT: no copy image " << width << "x" << height << LL_ENDL;
                return false;
            }
        }
        if (!sOIT.nodes || sOIT.capacity < sOIT.wanted)
        {
            // A new buffer; the old one goes once the GPU is done with it.
            U32 capacity = llmax(sOIT.wanted, 1024u);
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation memory = nullptr;
            while (true)
            {
                VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                info.size = OIT_HEADER_BYTES + (VkDeviceSize)capacity * OIT_NODE_WORDS * 4;
                info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocationCreateInfo gpu = {};
                gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &gpu, &buffer, &memory, nullptr) == VK_SUCCESS)
                {
                    break;
                }
                buffer = VK_NULL_HANDLE;
                if (capacity <= sOIT.capacity || capacity <= 1024u)
                {
                    break;
                }
                capacity = llmax(capacity / 2, llmax(sOIT.capacity, 1024u));    // try smaller
            }
            if (buffer && capacity > sOIT.capacity)
            {
                if (sOIT.nodes)
                {
                    oit_release_nodes();
                    ++sOIT.grows;
                }
                sOIT.nodes = buffer;
                sOIT.nodes_memory = memory;
                LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_MADE, (U64)buffer, memory, "OIT node buffer");    // research/109
                LL_INFOS("VKGL") << "OIT: node buffer " << capacity << " nodes (" << ((U64)capacity * OIT_NODE_WORDS * 4 >> 20) << " MB) for "
                                 << width << "x" << height << LL_ENDL;
                sOIT.capacity = capacity;
            }
            else if (buffer)
            {
                vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), buffer, memory);
            }
            sOIT.wanted = llmin(sOIT.wanted, sOIT.capacity);    // as large as it got
            if (!sOIT.nodes)
            {
                LL_WARNS("VKGL") << "OIT: no node buffer" << LL_ENDL;
                return false;
            }
        }
        // Running out once only makes the buffer grow (the next frames fit);
        // when it cannot grow to that, fragments past the end are summed into
        // the rest image from then on instead of being lost.
        if (sOIT.overflow_need > sOIT.capacity)
        {
            sOIT.rest_wanted = true;
        }
        sOIT.overflow_need = 0;
        if (sOIT.rest_wanted && !sOIT.rest.image)
        {
            if (oit_make_image(sOIT.rest, VK_FORMAT_R32_UINT, width, height, OIT_REST_LAYERS,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            {
                LL_INFOS("VKGL") << "OIT: the node buffer cannot hold every fragment, the rest are summed" << LL_ENDL;
            }
            else
            {
                sOIT.rest_wanted = false;   // fragments past the end stay lost
                LL_WARNS("VKGL") << "OIT: no rest image" << LL_ENDL;
            }
        }
        return true;
    }

    // The resolve pass's pipeline for the target's attachments.
    VkPipeline oit_resolve_pipeline(const LLVKMirror::Target& target)
    {
        std::vector<U32> key = { (U32)target.color_formats[0], (U32)target.depth_format, target.stencil ? 1u : 0u };
        auto it = sOIT.resolve_pipelines.find(key);
        if (it != sOIT.resolve_pipelines.end())
        {
            return it->second;
        }
        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = sOIT.resolve_vertex.modules[0];
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = sOIT.resolve_fragment.modules[0];
        stages[1].pName = "main";
        const VkSpecializationMapEntry layers_entry = { 0, 0, sizeof(U32) };     // MAX_LAYERS
        VkSpecializationInfo layers = {};
        layers.mapEntryCount = 1;
        layers.pMapEntries = &layers_entry;
        layers.dataSize = sizeof(U32);
        layers.pData = &sOITMaxLayers;
        stages[1].pSpecializationInfo = &layers;
        VkPipelineVertexInputStateCreateInfo vertex = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };   // no tests, no writes
        VkPipelineColorBlendAttachmentState blend = {};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo color = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        color.attachmentCount = 1;
        color.pAttachments = &blend;
        const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = target.color_formats;
        rendering.depthAttachmentFormat = target.depth_format;
        rendering.stencilAttachmentFormat = target.stencil ? target.depth_format : VK_FORMAT_UNDEFINED;
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
        info.layout = sOIT.resolve_layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(LLVKDevice::instance().getDevice(), LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
        }
        sOIT.resolve_pipelines[key] = pipeline;
        return pipeline;
    }

    // The interface the program's pipelines and sets are made for, unchanged
    // in its OIT variant: vertex inputs, uniform blocks, uniforms (set 1 and
    // the default block) and descriptors, besides the lists in set 2 and the
    // draw's priority in the push constants (the push constant block, binding
    // -1, grows to hold it; the range is the same for every layout).
    bool oit_same_interface(const Program& program, const LLVKShader::Program& v)
    {
        auto oit_name = [](const std::string& name)
        {
            static const std::string SEQUENCE = ".oit_priority";
            return name.compare(0, 9, "llrhi_oit") == 0 ||
                   (name.size() > SEQUENCE.size() && name.compare(name.size() - SEQUENCE.size(), SEQUENCE.size(), SEQUENCE) == 0);
        };
        if (v.inputs.size() != program.vk.inputs.size())
        {
            return false;
        }
        for (size_t i = 0; i < v.inputs.size(); ++i)
        {
            const LLVKShader::Input& a = program.vk.inputs[i];
            const LLVKShader::Input& b = v.inputs[i];
            if (a.name != b.name || a.location != b.location || a.gl_type != b.gl_type)
            {
                return false;
            }
        }
        std::vector<const LLVKShader::Block*> blocks;
        for (const LLVKShader::Block& b : v.blocks)
        {
            if (!oit_name(b.name))
            {
                blocks.push_back(&b);
            }
        }
        if (blocks.size() != program.vk.blocks.size())
        {
            return false;
        }
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const LLVKShader::Block& a = program.vk.blocks[i];
            if (a.name != blocks[i]->name || a.binding != blocks[i]->binding || (a.binding >= 0 && a.size != blocks[i]->size))
            {
                return false;
            }
        }
        auto block_name = [](const LLVKShader::Program& p, S32 index) { return index >= 0 && (size_t)index < p.blocks.size() ? p.blocks[index].name : std::string(); };
        size_t matched = 0;
        for (const LLVKShader::Uniform& b : v.uniforms)
        {
            if (oit_name(b.name))
            {
                continue;
            }
            bool found = false;
            for (const LLVKShader::Uniform& a : program.vk.uniforms)
            {
                if (a.name == b.name)
                {
                    found = a.gl_type == b.gl_type && a.offset == b.offset && a.binding == b.binding && a.size == b.size &&
                            a.array_stride == b.array_stride && block_name(program.vk, a.block) == block_name(v, b.block);
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
            ++matched;
        }
        if (matched != program.vk.uniforms.size())
        {
            return false;
        }
        for (const LLVKShader::Descriptor& d : v.descriptors)
        {
            if (d.set == 2 && oit_name(d.name) && d.binding <= 2)
            {
                continue;
            }
            bool known = false;
            for (const LLVKShader::Descriptor& e : program.vk.descriptors)
            {
                known |= e.set == d.set && e.binding == d.binding && e.name == d.name;
            }
            if (!known)
            {
                return false;
            }
        }
        return true;
    }

    // Of the variant's modules only the fragment one is used (get_pipeline
    // takes the program's own other stages): the others go. False: the
    // variant or the program has no fragment stage.
    bool oit_keep_fragment(const Program& program, OITVariant& v)
    {
        const bool program_fragment =
            std::find(program.vk.stages.begin(), program.vk.stages.end(), VK_SHADER_STAGE_FRAGMENT_BIT) != program.vk.stages.end();
        VkShaderModule fragment = VK_NULL_HANDLE;
        for (size_t i = 0; i < v.vk.modules.size(); ++i)
        {
            if (v.vk.stages[i] == VK_SHADER_STAGE_FRAGMENT_BIT)
            {
                fragment = v.vk.modules[i];
            }
            else
            {
                vkDestroyShaderModule(LLVKDevice::instance().getDevice(), v.vk.modules[i], nullptr);
            }
        }
        v.vk.modules.assign(fragment ? 1 : 0, fragment);
        v.vk.stages.assign(fragment ? 1 : 0, VK_SHADER_STAGE_FRAGMENT_BIT);
        v.fragment = fragment;
        return fragment && program_fragment;
    }

} // namespace

    // LLRHI::sOITSupported (the preferences): capable, and the setup has
    // not failed (it runs at the first session).
    bool oit_supported()
    {
        static const bool capable = oit_capable();
        return capable && (!sOIT.tried || sOIT.ready);
    }
    // LLRHI::sOITBegin. hybrid: a hybrid session (oit_world); the mode is
    // the session's to its resolve, whatever LLRHI::sOITHybrid does meanwhile.
    bool oit_begin(F32 depth_a, F32 depth_b, bool hybrid)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        LLVKMirror& mirror = LLVKMirror::instance();
        if (sOIT.active)
        {
            sOIT.active = false;    // never resolved: its nodes are dropped
            ++sOIT.dropped;
        }
        sOIT.hybrid = sOIT.world = sOIT.classic_world = false;
        if (!LLRHI::sOIT || !LLRHI::sOITSources || !mirror.isRecording() || !oit_init())
        {
            return false;
        }
        ResolvedTarget target;
        if (!resolve_draw_target(target) || target.is_default || target.target.color_count != 1 || target.color_textures != 1 ||
            !target.colors[0] || !target.colors[0]->image || !target.depth || !target.target.width || !target.target.height)
        {
            ++sOIT.refused;
            return false;
        }
        Texture& color = *target.colors[0];
        const U32 slot = mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT;
        OITReadback& rb = sOIT.readback[slot];
        if (rb.frame_begin != mirror.frameBegins())
        {
            oit_harvest(rb);    // the slot's last frame is done
            rb.frame_begin = 0;
        }
        if (rb.sessions >= OIT_SESSIONS_PER_FRAME || !oit_resources(target.target.width, target.target.height, color.format.format))
        {
            ++sOIT.refused;
            return false;
        }
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize ring_offset = 0;
        void* header_data = nullptr;
        if (!mirror.allocate(OIT_HEADER_BYTES, 16, ring, ring_offset, header_data))
        {
            ++sOIT.refused;
            return false;
        }
        const OITImage& rest = sOIT.rest.image ? sOIT.rest : sOIT.dummy_rest;
        // The bounds start empty (min past max).
        const U32 header[OIT_HEADER_WORDS] = { 0, sOIT.capacity, sOIT.rest.image ? OIT_FLAG_REST : 0u, 0, U32_MAX, U32_MAX, 0, 0 };
        memcpy(header_data, header, sizeof(header));

        // The session's set (the frame that used it last is done).
        const U32 entry = rb.sessions;
        const VkDescriptorSet set = sOIT.sets[slot][entry];
        const VkDescriptorImageInfo heads_info = { VK_NULL_HANDLE, sOIT.heads.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorBufferInfo nodes_info = { sOIT.nodes, 0, VK_WHOLE_SIZE };
        const VkDescriptorImageInfo rest_info = { VK_NULL_HANDLE, rest.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo copy_info = { sOIT.sampler, sOIT.copy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        // The bounds pass's: the target sampled (in GENERAL then) and the copy as storage.
        const bool copy_storage = sOIT.copy_storage && color.attach_view;
        const VkDescriptorImageInfo target_info = { sOIT.sampler, copy_storage ? color.attach_view : sOIT.copy.view,
                                                    copy_storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo storage_info = { VK_NULL_HANDLE, copy_storage ? sOIT.copy.view : sOIT.dummy_storage.view,
                                                     VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet writes[OIT_SET_BINDINGS] = {};
        for (U32 i = 0; i < OIT_SET_BINDINGS; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &heads_info;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &nodes_info;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &rest_info;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &copy_info;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo = &target_info;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[5].pImageInfo = &storage_info;
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), OIT_SET_BINDINGS, writes, 0, nullptr);

        // Outside the rendering scope: every earlier use of the lists (the last
        // session's draws and resolve, its count copy, earlier frames) before
        // the clears, and the clears before this session's draws.
        VkCommandBuffer cmd = mirror.transferCommands();
        VkMemoryBarrier2 before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkImageMemoryBarrier2 to_general[4] = {};
        U32 transitions = 0;
        OITImage* storage[4] = { &sOIT.heads, sOIT.rest.image ? &sOIT.rest : nullptr, &sOIT.dummy_rest, &sOIT.dummy_storage };
        for (OITImage* img : storage)
        {
            if (!img || img->layout == VK_IMAGE_LAYOUT_GENERAL)
            {
                continue;
            }
            VkImageMemoryBarrier2& b = to_general[transitions++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = img->layout;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img->image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
            img->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &before;
        dependency.imageMemoryBarrierCount = transitions;
        dependency.pImageMemoryBarriers = transitions ? to_general : nullptr;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkClearColorValue empty = {};
        empty.uint32[0] = empty.uint32[1] = empty.uint32[2] = empty.uint32[3] = 0xffffffffu;
        const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
        vkCmdClearColorImage(cmd, sOIT.heads.image, VK_IMAGE_LAYOUT_GENERAL, &empty, 1, &range);
        if (sOIT.rest.image)
        {
            const VkClearColorValue zero = {};
            vkCmdClearColorImage(cmd, sOIT.rest.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        const VkBufferCopy header_copy = { ring_offset, 0, OIT_HEADER_BYTES };
        vkCmdCopyBuffer(cmd, ring, sOIT.nodes, 1, &header_copy);

        VkMemoryBarrier2 after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        after.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo after_dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        after_dependency.memoryBarrierCount = 1;
        after_dependency.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(cmd, &after_dependency);

        rb.sessions = entry + 1;
        rb.copied[entry] = false;       // until its resolve copies the count
        rb.avatar_copied[entry] = false;
        rb.avatar_draws[entry] = 0;
        rb.frame_begin = mirror.frameBegins();
        sOIT.active = true;
        sOIT.hybrid = hybrid;
        if (sOIT.logged_hybrid != (hybrid ? 1 : 0))
        {
            sOIT.logged_hybrid = hybrid ? 1 : 0;
            LL_INFOS("VKGL") << (hybrid ? "OIT: hybrid (avatar alpha in the lists, the rest only over avatar alpha)" : "OIT: full (all alpha in the lists)")
                             << LL_ENDL;
        }
        sOIT.hybrid_sessions += hybrid ? 1 : 0;
        sOIT.frame = sFrame;
        sOIT.color = &color;
        sOIT.depth = target.depth;
        sOIT.color_image = color.image;
        sOIT.color_view = copy_storage ? color.attach_view : VK_NULL_HANDLE;
        sOIT.set = set;
        sOIT.slot = slot;
        sOIT.entry = entry;
        sOIT.node_draws = 0;
        sOIT.depth_a = depth_a;
        sOIT.depth_b = depth_b;
        ++sOIT.sessions;
        return true;
    }

    // LLRHI::sOITWorld: a hybrid session's avatar draws are all made; the
    // draws from here on are the world's (split, oit_for_draw). Their head
    // reads come after every avatar node write: a barrier outside the
    // rendering scope (the next draw opens a new one). The count the avatar
    // half left is read back (oit_harvest) for this rule: the world draws
    // as before, with the programs' own pipelines, when no avatar draw had a
    // variant, or when the last OIT_EMPTY_STREAK sessions read back appended
    // no avatar node and this avatar half draws no more than those did (an
    // avatar that comes into view with no new draw is ordered over the
    // world alpha for the frames until the count comes back). The resolve
    // still runs when there were avatar draws (its bounds pass finds the
    // nodes, if any).
    void oit_world()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!sOIT.active || !sOIT.hybrid || sOIT.world || sOIT.frame != sFrame)
        {
            return;
        }
        sOIT.world = true;
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!sOIT.node_draws || !mirror.isRecording())
        {
            sOIT.classic_world = true;
            ++sOIT.avatarless;
            return;
        }
        VkCommandBuffer cmd = mirror.transferCommands();
        VkMemoryBarrier2 written = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        written.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        written.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        written.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        written.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &written;
        vkCmdPipelineBarrier2(cmd, &dependency);
        // The avatar half's count; the world draws' appends wait for the read.
        OITReadback& rb = sOIT.readback[sOIT.slot];
        const VkBufferCopy count_copy = { 0, (VkDeviceSize)sOIT.entry * OIT_READBACK_BYTES + OIT_HEADER_BYTES, 4 };
        vkCmdCopyBuffer(cmd, sOIT.nodes, rb.buffer, 1, &count_copy);
        rb.avatar_copied[sOIT.entry] = true;
        rb.avatar_draws[sOIT.entry] = sOIT.node_draws;
        VkMemoryBarrier2 read = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        read.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        read.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        dependency.pMemoryBarriers = &read;
        vkCmdPipelineBarrier2(cmd, &dependency);
        if (sOIT.empty_streak >= OIT_EMPTY_STREAK && sOIT.node_draws <= sOIT.empty_draws)
        {
            sOIT.classic_world = true;
            ++sOIT.avatarless;
        }
    }
    // LLRHI::sOITResolve.
    void oit_resolve()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!sOIT.active)
        {
            return;
        }
        sOIT.active = false;
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!sOIT.node_draws && sOIT.frame == sFrame)
        {
            // Nothing in the lists: no resolve, and the session's readback entry
            // (the last one) stays unwritten, so it is not read.
            OITReadback& rb = sOIT.readback[sOIT.slot];
            if (rb.sessions == sOIT.entry + 1)
            {
                rb.sessions = sOIT.entry;
            }
            return;
        }
        ResolvedTarget target;
        if (sOIT.frame != sFrame || !mirror.isRecording() || !resolve_draw_target(target) || target.colors[0] != sOIT.color ||
            target.depth != sOIT.depth || target.target.color_count != 1 || sOIT.color->image != sOIT.color_image ||
            target.target.width != sOIT.width || target.target.height != sOIT.height)
        {
            // The target is not the session's any more (never seen): its nodes are dropped.
            LL_WARNS_ONCE("VKGL") << "OIT: the target changed during the session, its alpha is dropped" << LL_ENDL;
            ++sOIT.dropped;
            return;
        }
        const VkPipeline pipeline = oit_resolve_pipeline(target.target);
        if (!pipeline)
        {
            LL_WARNS_ONCE("VKGL") << "OIT: no resolve pipeline, alpha is dropped" << LL_ENDL;
            ++sOIT.dropped;
            return;
        }
        Texture& color = *sOIT.color;
        // The bounds pass's copy samples the view the session's set has.
        const char* qualifier = sOIT.color_view && color.attach_view == sOIT.color_view ? oit_copy_qualifier(color.format.format) : nullptr;
        VkPipeline bounds = sOIT.bounds;
        if (qualifier)
        {
            auto it = sOIT.bounds_copy.find((U32)color.format.format);
            if (it == sOIT.bounds_copy.end())
            {
                it = sOIT.bounds_copy.emplace((U32)color.format.format, oit_bounds_pipeline(qualifier)).first;
            }
            bounds = it->second ? it->second : bounds;
            qualifier = it->second ? qualifier : nullptr;
        }
        VkCommandBuffer cmd = mirror.transferCommands();
        // The draws' nodes before the bounds pass and the resolve read them.
        VkMemoryBarrier2 written = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        written.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        written.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        written.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        written.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &written;
        vkCmdPipelineBarrier2(cmd, &dependency);

        // What is behind the nodes: the target as the session leaves it, at the
        // pixels with a list (the bounds pass), else all of it.
        if (qualifier)
        {
            transition(cmd, color, VK_IMAGE_LAYOUT_GENERAL);
            LLVKMirror::imageBarrier(cmd, sOIT.copy.image, VK_IMAGE_ASPECT_COLOR_BIT, sOIT.copy.layout, VK_IMAGE_LAYOUT_GENERAL, 1, 1);
            sOIT.copy.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        else
        {
            transition(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            LLVKMirror::imageBarrier(cmd, sOIT.copy.image, VK_IMAGE_ASPECT_COLOR_BIT, sOIT.copy.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1);
            sOIT.copy.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkImageCopy region = {};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { sOIT.width, sOIT.height, 1 };
            vkCmdCopyImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sOIT.copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        // The bounds pass: the bounds of the pixels with a list into the
        // header (and the copy there).
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bounds);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sOIT.bounds_layout, 0, 1, &sOIT.set, 0, nullptr);
        const U32 size[2] = { sOIT.width, sOIT.height };
        vkCmdPushConstants(cmd, sOIT.bounds_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
        vkCmdDispatch(cmd, (sOIT.width + 15) / 16, (sOIT.height + 15) / 16, 1);
        VkMemoryBarrier2 bounded = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        bounded.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bounded.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        bounded.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        bounded.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
        dependency.pMemoryBarriers = &bounded;
        vkCmdPipelineBarrier2(cmd, &dependency);
        // The count, for the size check when the slot comes round again.
        const VkBufferCopy count_copy = { 0, (VkDeviceSize)sOIT.entry * OIT_READBACK_BYTES, OIT_HEADER_BYTES };
        vkCmdCopyBuffer(cmd, sOIT.nodes, sOIT.readback[sOIT.slot].buffer, 1, &count_copy);
        sOIT.readback[sOIT.slot].copied[sOIT.entry] = true;
        // The copy (written by the transfer or the bounds pass) for sampling.
        LLVKMirror::imageBarrier(cmd, sOIT.copy.image, VK_IMAGE_ASPECT_COLOR_BIT, sOIT.copy.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);
        sOIT.copy.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transition(cmd, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // The depth stays as the session's draws had it.
        target.target.depth_read_only = target.depth->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        if (needs_attachment_layouts(target))
        {
            to_attachment_layouts(cmd, target);
        }

        VkCommandBuffer draw = mirror.beginRendering(target.target);
        vkCmdBindPipeline(draw, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(draw, VK_PIPELINE_BIND_POINT_GRAPHICS, sOIT.resolve_layout, 0, 1, &sOIT.set, 0, nullptr);
        const U32 clamp_colors = (format_features(color.format.format) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) &&
                                 !(color.format.format == VK_FORMAT_R16G16B16A16_SFLOAT || color.format.format == VK_FORMAT_R32G32B32A32_SFLOAT ||
                                   color.format.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || color.format.format == VK_FORMAT_R16G16B16_SFLOAT ||
                                   color.format.format == VK_FORMAT_R16G16_SFLOAT || color.format.format == VK_FORMAT_R16_SFLOAT ||
                                   color.format.format == VK_FORMAT_R32_SFLOAT || color.format.format == VK_FORMAT_R32G32_SFLOAT) ? 1u : 0u;
        OITResolveConstants constants;
        constants.clamp_colors = clamp_colors;
        constants.depth_a = sOIT.depth_a;
        constants.depth_b = sOIT.depth_b;
        // Window depth to clip depth: [0, 1] -> [-1, 1], GL's clip depth
        // (depth clip control, required: research/124 V1).
        constants.ndc_scale = 2.f;
        constants.ndc_bias = -1.f;
        constants.tolerance = sOITTolerance;
        constants.relative = sOITRelative;
        constants.span = sOITSpan;
        constants.width = (F32)sOIT.width;
        constants.height = (F32)sOIT.height;
        vkCmdPushConstants(draw, sOIT.resolve_layout, OIT_RESOLVE_STAGES, 0, sizeof(constants), &constants);
        const VkViewport viewport = { 0.f, 0.f, (F32)sOIT.width, (F32)sOIT.height, 0.f, 1.f };
        const VkRect2D scissor = { { 0, 0 }, { sOIT.width, sOIT.height } };
        vkCmdSetViewport(draw, 0, 1, &viewport);
        vkCmdSetScissor(draw, 0, 1, &scissor);
        vkCmdDraw(draw, 4, 1, 0, 0);   // a strip over the bounds (OIT_RESOLVE_VERTEX)

        // The generic path's bookkeeping: another layout's pipeline, set 0 and
        // push constants, and no dynamic state of its own.
        Recorded& r = sRecorded;
        r.pipeline = pipeline;
        r.set = VK_NULL_HANDLE;
        r.table_only = false;
        r.slot_count = 0;
        r.dynamic = false;
        pass_invalidate();
    }

    // LLRHI::sOITSnapshotCheck (research/41): after a snapshot's render (the
    // frame goes on unpresented), submit what is recorded, wait for it and
    // read the counts of its sessions now instead of when the slot comes round.
    // The GPU is done with the sessions' entries, so the next render (another
    // tile, or this one again) starts from the first entry. True: a session
    // ran out of nodes and the buffer has grown for it (up to LLRHI_OIT_MAX_MB,
    // not just LLRHI_OIT_GROW_LAYERS per pixel), or, at the most it can hold,
    // the rest image has just been made: the caller renders it again.
    bool oit_snapshot_check()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!sOIT.ready || sOIT.active || !mirror.isRecording())
        {
            return false;
        }
        OITReadback& rb = sOIT.readback[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        if (rb.frame_begin != mirror.frameBegins() || !rb.sessions)
        {
            return false;   // no session in this frame
        }
        // The count copies before the host reads them.
        VkCommandBuffer cmd = mirror.transferCommands();
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        const bool done = mirror.submitAndWait();
        // The command buffers started over: nothing is bound in them.
        sRecorded = Recorded();
        sRecorded.frame = sFrame;
        pass_invalidate();
        if (!done)
        {
            return false;
        }
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), rb.memory, 0, VK_WHOLE_SIZE);
        U32 need = 0;
        bool overflow = false;
        for (U32 i = 0; i < rb.sessions; ++i)
        {
            if (!rb.copied[i])
            {
                continue;   // dropped or empty
            }
            rb.copied[i] = false;
            // research/109: entries are OIT_READBACK_WORDS apart (as oit_harvest reads them).
            const U32 count = rb.words[i * OIT_READBACK_WORDS];
            const U32 capacity = rb.words[i * OIT_READBACK_WORDS + 1];
            need = llmax(need, count);
            overflow |= count > capacity;
        }
        rb.sessions = 0;
        sOIT.peak = llmax(sOIT.peak, need);
        if (!overflow)
        {
            return false;
        }
        ++sOIT.overflows;
        const U32 before = sOIT.capacity;
        if (before < sOIT.max_nodes)
        {
            sOIT.wanted = llmax(sOIT.wanted, (U32)llmin((U64)need + need / 16, (U64)sOIT.max_nodes));
            oit_resources(sOIT.width, sOIT.height, sOIT.copy.format);
            if (sOIT.capacity > before)
            {
                LL_INFOS("VKGL") << "OIT: snapshot needed " << need << " nodes, rendering it again" << LL_ENDL;
                return true;
            }
        }
        // As large as it gets: the fragments past the end go to the rest image.
        if (!sOIT.rest.image)
        {
            sOIT.overflow_need = llmax(sOIT.overflow_need, need);
            oit_resources(sOIT.width, sOIT.height, sOIT.copy.format);
            return sOIT.rest.image != VK_NULL_HANDLE;
        }
        return false;
    }
    // The program's OIT variant: made on its first draw in a session (a
    // compile, then the SPIR-V cache). Null: it has none and draws as before.
    OITVariant* oit_variant(GLuint id, Program& program)
    {
        if (program.oit)
        {
            return program.oit->ok ? program.oit.get() : nullptr;
        }
        program.oit = std::make_shared<OITVariant>();
        OITVariant& v = *program.oit;
        std::vector<LLVKShader::Source> sources = program.oit_sources;
        const char* why = nullptr;
        // research/205 3a-2: the priority from the draw's row (while LLRHI_SCENE_ALPHA is not off).
        const U32 row_word = oit_row_word(program.draw_rows, program.draw_row_words);
        bool row_priority = false;
        if (sources.empty())
        {
            why = "no sources";
        }
        else if (!oit_rewrite(sources, row_word, &row_priority))
        {
            why = "no single fragment main";
        }
        else if (!LLVKShader::build(sources, v.vk, 1))
        {
            why = "did not build";
        }
        else if (!oit_same_interface(program, v.vk))
        {
            why = "its interface changed";
        }
        else if (!oit_keep_fragment(program, v))
        {
            why = "no fragment stage";
        }
        else
        {
            const VkDescriptorSetLayout set_layouts[3] = { LLVKBindless::layout(), program.set_layout ? program.set_layout : sOIT.empty_layout,
                                                           sOIT.set_layout };
            const VkPushConstantRange push_range = LLVKBindless::pushConstantRange();
            VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            info.setLayoutCount = 3;
            info.pSetLayouts = set_layouts;
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &push_range;
            if (vkCreatePipelineLayout(LLVKDevice::instance().getDevice(), &info, nullptr, &v.layout) != VK_SUCCESS)
            {
                v.layout = VK_NULL_HANDLE;
                why = "no pipeline layout";
            }
        }
        if (why)
        {
            // Programs that are no alpha program land here too (no frag_color).
            LL_INFOS("VKGL") << "program " << id << ": no OIT variant (" << why << "), draws as before" << LL_ENDL;
            if (!v.vk.modules.empty())
            {
                LLVKShader::destroy(v.vk);
            }
            v.vk = LLVKShader::Program();
            ++sOIT.variants_failed;
        }
        // Tried once per link: the sources are not needed again.
        std::vector<LLVKShader::Source>().swap(program.oit_sources);
        if (why)
        {
            return nullptr;
        }
        v.ok = true;
        v.row_priority_word = row_priority ? row_word : U32_MAX;
        ++sOIT.variants;
        note_oit_variant(program.main_fragment);   // research/111 B-PW: prewarmed from now on
        return &v;
    }

    // Set 2 for the variant's draw, after bind_program_set, and the draw's
    // priority (LLRHI::sOITPriority, which the alpha pool sets per batch):
    // the last push constant word, which no variant's own words reach. Both
    // only when they are not what is bound: set 2 stays while the only
    // descriptor binds since were bind_program_set's with the same program's
    // layout (compatible with the variant's for sets 0 and 1: binding them
    // again does not disturb set 2); the word stays while no push reached it
    // (slot_count stays below it; whatever pushes the last word raises it).
    void oit_bind_set(VkCommandBuffer cmd, const Program& program, const OITVariant& v)
    {
        Recorded& r = sRecorded;
        const U32 binds = LLVKDispatch::sCounts[LLVKDispatch::C_CmdBindDescriptorSets];
        const bool fresh = r.oit_cmd != cmd || r.oit_epoch != sRecordEpoch;   // nothing known of what is bound
        if (fresh || r.oit_layout != v.layout || r.oit_set != sOIT.set || r.oit_program_layout != program.layout ||
            binds - r.oit_binds != r.oit_compatible)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.layout, 2, 1, &sOIT.set, 0, nullptr);
            r.oit_layout = v.layout;
            r.oit_program_layout = program.layout;
            r.oit_set = sOIT.set;
            r.oit_cmd = cmd;
            r.oit_epoch = sRecordEpoch;
            r.oit_binds = LLVKDispatch::sCounts[LLVKDispatch::C_CmdBindDescriptorSets];
            r.oit_compatible = 0;
            ++sOIT.set_binds;
            // fix-r6: a variant of a program on the shared layout has the empty
            // set layout at set 1, so this bind disturbs the set 1 another
            // program left bound (its layout is not compatible for set 1). The
            // table at set 0 stays (compatible for set 0); the next program
            // with a set of its own must bind it again (bind_program_set).
            if (!program.set_layout && r.set)
            {
                r.set = VK_NULL_HANDLE;
                r.table_only = true;
                r.dynamic_offset = 0;
            }
        }
        const U32 priority = oit_priority();
        if (fresh || !r.oit_priority_pushed || r.oit_priority != priority || r.slot_count > OIT_PRIORITY_OFFSET / 4)
        {
            vkCmdPushConstants(cmd, v.layout, LLVKBindless::pushConstantRange().stageFlags, OIT_PRIORITY_OFFSET, 4, &priority);
            r.oit_priority = priority;
            r.oit_priority_pushed = true;
            // A program pushing all the words must push the last one again.
            r.slot_count = llmin(r.slot_count, OIT_PRIORITY_OFFSET / 4);
        }
    }

    // LLRHI::sOITVariant: whether draws of the GL program make nodes in the
    // open session (its variant, made now if not yet).
    bool oit_program_variant(U32 gl_program)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!sOIT.active)
        {
            return false;
        }
        auto it = sPrograms.find(gl_program);
        return it != sPrograms.end() && it->second.built && oit_variant(gl_program, it->second) != nullptr;
    }

    std::string oit_report(U32 frames)
    {
        std::ostringstream out;
        out << "OIT sessions " << sOIT.sessions / frames << " (refused " << sOIT.refused << ", dropped " << sOIT.dropped << ", hybrid "
            << sOIT.hybrid_sessions / frames << ", without avatar alpha " << sOIT.avatarless / frames << "), node draws "
            << sOIT.draws / frames << " (split " << sOIT.split_draws / frames << ", set binds " << sOIT.set_binds / frames
            << "), drawn as before " << sOIT.classic_draws / frames
            << ", nodes peak " << sOIT.peak << " of "
            << sOIT.capacity << ", overflows " << sOIT.overflows << ", grows " << sOIT.grows << ", rest " << (sOIT.rest.image ? "on" : "off")
            << ", variants " << sOIT.variants << " (none " << sOIT.variants_failed << ")";
        sOIT.sessions = sOIT.refused = sOIT.dropped = sOIT.draws = sOIT.classic_draws = 0;
        sOIT.hybrid_sessions = sOIT.avatarless = sOIT.split_draws = sOIT.set_binds = 0;
        sOIT.peak = sOIT.overflows = sOIT.grows = 0;
        return out.str();
    }

    bool bake_oit_shaders()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return oit_init();
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
