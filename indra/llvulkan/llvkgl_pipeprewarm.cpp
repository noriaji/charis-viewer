/**
 * @file llvkgl_pipeprewarm.cpp
 * @brief Pipelines of the translated programs made before their first draw, from the keys of earlier sessions (research/111 B-PL).
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

// get_pipeline makes a program's pipeline on the first draw that needs it:
// after every shader reload (a new link is a new key), and for every blend
// state, target and vertex format the program meets for the first time. The
// driver's compile of one is milliseconds with the pipeline cache warm and
// tens of milliseconds without: hitches while walking into new content, and
// a burst of them after a preset (research/111 §3.4).
//
// Every key get_pipeline makes is kept on disk (pipeline_keys.bin beside the
// pipeline cache) with the program named by its content (the SPIR-V's hash)
// instead of this session's GL name. Once the programs have stopped changing
// (after login, after a reload), a low priority thread makes the pipelines of
// the kept keys whose program is linked now, and get_pipeline finds them
// made. A key stays while its program is linked in a session, and goes after
// KEEP_SESSIONS sessions without (other presets, older viewers).
//
// Pipelines of OIT variants are made once the variant exists (its first
// draw makes it, from the shader cache: research/111 B-PW).
//
// research/160 (LLRHI_PIPELINE_CARRY=1, off by default):
// - P1: a key also names its program by identity (program_identity: the same
//   under every preset). A program linked for the first time (a preset never
//   used, a setting changed) has no keys of its own content yet; it takes the
//   keys of the same identity, made in the background as soon as the reload
//   is done, and they are kept as its own from then on.
// - P2: a driver update keeps the list (the GPU must be the same); only the
//   pipeline cache starts over, and the kept keys are made again in the background.
// - P4: the pass runs on up to 3 low priority threads.
// The content word is the SPIR-V's hash (LLVKShader::Program::code_key, on
// with or without the switch): the same program on another vendor's GPU has
// the same keys (research/159 §7-4, the seed of P3).

#include "linden_common.h"

#include "llvkgl_drawpath.h"

#if LL_GL_FUNC_POINTER

#include "llfile.h"

#include <fstream>
#include <shared_mutex>
#include <unordered_set>

namespace llvkgl
{
    VkPipeline create_pipeline(const PipelineRecipe& recipe)
    {
        const LLVKDevice& device = LLVKDevice::instance();
        // Without fillModeNonSolid only FILL is valid (glPolygonMode's lines and points draw filled).
        const VkPolygonMode polygon_mode = device.usablePolygonMode(recipe.polygon_mode);
        // The translated vertex shaders do not write gl_PointSize: points need
        // maintenance5 (size 1 then, VUID-VkGraphicsPipelineCreateInfo-topology-08773).
        // Without it the pipeline is not made and the draw is left out (only
        // debug views and a few previews draw points).
        if (!device.hasMaintenance5() && recipe.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        {
            return VK_NULL_HANDLE;
        }
        const VkSpecializationMapEntry oit_entries[2] = { { OIT_BLEND_CONSTANT_ID, 0, sizeof(U32) },
                                                          { OIT_SPLIT_CONSTANT_ID, sizeof(U32), sizeof(U32) } };
        VkSpecializationInfo oit_specialization = {};
        oit_specialization.mapEntryCount = 2;
        oit_specialization.pMapEntries = oit_entries;
        oit_specialization.dataSize = sizeof(recipe.oit_constants);
        oit_specialization.pData = recipe.oit_constants;
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (const PipelineRecipe::Stage& s : recipe.stages)
        {
            VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stage.stage = s.stage;
            stage.module = s.module;
            stage.pName = "main";
            stage.pSpecializationInfo = s.oit_variant ? &oit_specialization : nullptr;
            stages.push_back(stage);
        }

        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (size_t i = 0; i < recipe.inputs.size(); ++i)
        {
            bindings.push_back({ (U32)i, recipe.inputs[i].stride, VK_VERTEX_INPUT_RATE_VERTEX });
            attributes.push_back({ recipe.inputs[i].location, (U32)i, recipe.inputs[i].format, 0 });
        }
        VkPipelineVertexInputStateCreateInfo vertex = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex.vertexBindingDescriptionCount = (U32)bindings.size();
        vertex.pVertexBindingDescriptions = bindings.data();
        vertex.vertexAttributeDescriptionCount = (U32)attributes.size();
        vertex.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = recipe.topology;

        VkPipelineViewportDepthClipControlCreateInfoEXT clip_control = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
        clip_control.negativeOneToOne = VK_TRUE;    // GL clip-space depth (required, research/124 V1)
        VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.pNext = &clip_control;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = polygon_mode;
        raster.lineWidth = 1.f;
        raster.depthClampEnable = recipe.depth_clamp;

        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

        VkPipelineColorBlendStateCreateInfo color = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        color.attachmentCount = recipe.color_count;
        color.pAttachments = recipe.blends;

        std::vector<VkDynamicState> dynamic_states =
        {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };
        if (recipe.generated)
        {
            // Vertex buffer tokens need it (VUID-VkGeneratedCommandsInfoEXT-indirectCommandsLayout-11079).
            dynamic_states.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
        }
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = (U32)dynamic_states.size();
        dynamic.pDynamicStates = dynamic_states.data();

        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = recipe.color_count;
        rendering.pColorAttachmentFormats = recipe.color_formats;
        rendering.depthAttachmentFormat = recipe.depth_format;
        rendering.stencilAttachmentFormat = recipe.stencil ? recipe.depth_format : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = (U32)stages.size();
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &color;
        info.pDynamicState = &dynamic;
        info.layout = recipe.layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device.getDevice(), LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
        }
        return pipeline;
    }

namespace
{
    using Clock = std::chrono::steady_clock;

    F64 seconds_since(Clock::time_point start)
    {
        return std::chrono::duration<F64>(Clock::now() - start).count();
    }

    // LLRHI_PIPELINE_PREWARM=0: no key list, no prewarm (pipelines as they are needed, as before).
    const bool sEnabled = LLRHI::envDefaultOn("LLRHI_PIPELINE_PREWARM");

    // LLRHI_PIPELINE_CARRY=1: research/160 P1, P2 and P4 (see the top).
    const bool sCarry = LLRHI::envFirstChar("LLRHI_PIPELINE_CARRY") == '1';

    // research/160 (research/159 §7-4): the program's content as the SPIR-V's
    // hash (LLVKShader::Program::code_key): the same whether the modules were
    // compiled, read from the cache or from the bundle, and on every GPU.
    U64 content_key(const Program& program)
    {
        return program.vk.code_key;
    }

    // ------------------------------------------------------------------
    // The kept keys: the program's content (content_key, two words) and its
    // identity (Program::identity, two words) in place of PK_PROGRAM..PK_LAYOUT,
    // then the rest of the key as get_pipeline made it. Each with the last
    // session it was used in.

    constexpr U32 KEYS_MAGIC = 0x4b504c4c;     // "LLPK"
    // Bump when get_pipeline's key words change (2: the trailer; 3: the
    // identity words and the GPU in the header; 4: the content is the SPIR-V's hash).
    // A file of another version is not read: the list starts over once.
    constexpr U32 KEYS_VERSION = 4;
    constexpr U32 KEEP_SESSIONS = 30;
    constexpr size_t KEYS_LIMIT = 8192;        // kept, written and read
    constexpr U32 KEY_WORDS_LIMIT = 256;
    constexpr F64 SAVE_SECONDS = 60.0;
    constexpr U32 PREFIX_WORDS = 4;            // content (2), identity (2)

    // The file's header: magic, version, session, then the GPU and driver.
    enum HeaderWord : U32
    {
        H_MAGIC, H_VERSION, H_SESSION, H_VENDOR, H_DEVICE, H_DRIVER, H_RESERVED,
        H_UUID,                                 // the pipeline cache UUID, VK_UUID_SIZE / 4 words
        HEADER_WORDS = H_UUID + VK_UUID_SIZE / 4,
    };

    std::mutex sKeysMutex;
    std::unordered_map<std::vector<U32>, U32, KeyHash> sKeys;
    U32 sSession = 1;
    // Atomic (research/154 W8): the present's prewarmPipelines() looks at it
    // without sKeysMutex every frame; set under the mutex.
    std::atomic<bool> sKeysLoaded{ false };
    bool sKeysDirty = false;
    Clock::time_point sKeysSaved = Clock::now();
    std::atomic<bool> sSaving{ false };

    // The file is for this GPU (formats, features: KeyCheck) and, without
    // research/160 P2, this driver too: its vendor, device and pipeline cache
    // UUID, as the pipeline cache's own header has. The rest of the header is 0.
    void device_header(U32 out[HEADER_WORDS])
    {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(LLVKDevice::instance().getPhysicalDevice(), &props);
        out[H_VENDOR] = props.vendorID;
        out[H_DEVICE] = props.deviceID;
        out[H_DRIVER] = props.driverVersion;
        memcpy(out + H_UUID, props.pipelineCacheUUID, VK_UUID_SIZE);
    }

    // A kept key decodes as get_pipeline's (make_pipeline_recipe reads it), and
    // what it hands vkCreateGraphicsPipelines is valid on this device: a key the
    // file's hash missed (or an older build's) must not crash every start.
    class KeyCheck
    {
    public:
        KeyCheck()
        {
            VkPhysicalDeviceProperties props = {};
            vkGetPhysicalDeviceProperties(LLVKDevice::instance().getPhysicalDevice(), &props);
            mMaxInputs = llmin(props.limits.maxVertexInputAttributes, props.limits.maxVertexInputBindings);   // one binding each
            mMaxLocations = props.limits.maxVertexInputAttributes;
            mMaxStride = props.limits.maxVertexInputBindingStride;
        }

        bool wellFormed(const std::vector<U32>& persistent)
        {
            // persistent[at(PK_x)] is the key's word PK_x
            constexpr auto at = [](size_t word) constexpr { return PREFIX_WORDS + word - PK_TOPOLOGY; };
            constexpr size_t AT_COLOR_FORMATS = at(PK_COLOR_FORMATS);
            if (persistent.size() < AT_COLOR_FORMATS || persistent[at(PK_COLOR_COUNT)] > 4 ||
                persistent.size() < AT_COLOR_FORMATS + persistent[at(PK_COLOR_COUNT)])
            {
                return false;
            }
            const size_t colors = persistent[at(PK_COLOR_COUNT)];
            const size_t rest = persistent.size() - AT_COLOR_FORMATS - colors;
            const size_t tail = rest % 3;
            switch (tail)
            {
            case 2:
                if (persistent.back() != PIPELINE_KEY_DGC || persistent[persistent.size() - 2] != PIPELINE_KEY_DGC) return false;
                break;
            case 1:
                if (persistent.back() != PIPELINE_KEY_OIT && persistent.back() != PIPELINE_KEY_OIT_SPLIT) return false;
                break;
            default:
                break;
            }

            if (persistent[at(PK_TOPOLOGY)] > 2 || persistent[at(PK_STENCIL)] > 1 || persistent[at(PK_DEPTH_CLAMP)] > 1)
            {
                return false;
            }
            for (size_t i = 0; i < colors; ++i)
            {
                const U32 format = persistent[AT_COLOR_FORMATS + i];
                if (format != VK_FORMAT_UNDEFINED && !(features(format).optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                {
                    return false;
                }
            }
            const U32 depth = persistent[at(PK_DEPTH_FORMAT)];
            if (depth != VK_FORMAT_UNDEFINED && !(features(depth).optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            {
                return false;
            }
            // the stencil attachment is the depth attachment (create_pipeline): a format with stencil
            if (persistent[at(PK_STENCIL)] && depth != VK_FORMAT_D16_UNORM_S8_UINT && depth != VK_FORMAT_D24_UNORM_S8_UINT &&
                depth != VK_FORMAT_D32_SFLOAT_S8_UINT && depth != VK_FORMAT_S8_UINT)
            {
                return false;
            }
            // vertex inputs: (location, format, stride), each its own binding
            const size_t inputs = rest / 3;
            if (inputs > mMaxInputs)
            {
                return false;
            }
            U64 locations = 0;
            for (size_t i = 0; i < inputs; ++i)
            {
                const size_t w = AT_COLOR_FORMATS + colors + 3 * i;
                const U32 location = persistent[w], format = persistent[w + 1], stride = persistent[w + 2];
                if (location >= mMaxLocations || location >= 64 || (locations & (1ull << location)) || stride > mMaxStride ||
                    !(features(format).bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
                {
                    return false;
                }
                locations |= 1ull << location;
            }
            return true;
        }

    private:
        // Core formats only (the viewer uses no others): an unknown value must not reach the driver.
        VkFormatProperties features(U32 format)
        {
            VkFormatProperties props = {};
            if (format == VK_FORMAT_UNDEFINED || format > VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
            {
                return props;
            }
            auto it = mFormats.find(format);
            if (it == mFormats.end())
            {
                vkGetPhysicalDeviceFormatProperties(LLVKDevice::instance().getPhysicalDevice(), (VkFormat)format, &props);
                it = mFormats.emplace(format, props).first;
            }
            return it->second;
        }

        std::unordered_map<U32, VkFormatProperties> mFormats;
        U32 mMaxInputs = 16;
        U32 mMaxLocations = 16;
        U32 mMaxStride = 2048;
    };

    std::string keys_path()
    {
        const std::string& dir = LLVKShader::cacheDir();
        return dir.empty() ? std::string() : dir + "/pipeline_keys.bin";
    }

    // Under sKeysMutex.
    void load_keys()
    {
        if (sKeysLoaded.load(std::memory_order_relaxed))
        {
            return;
        }
        sKeysLoaded.store(true, std::memory_order_release);
        const std::string path = keys_path();
        // The whole file (the path is UTF-8: LLVKShader::cacheDir), read only
        // when its trailer (size and hash, as the .vkp files) matches.
        std::string data;
        U32 header[HEADER_WORDS] = {};
        U32 device[HEADER_WORDS] = {};
        device_header(device);
        if (path.empty() || !LLVKShader::readCacheFile(path, data) || !LLVKShader::stripCacheTrailer(data) || data.size() < sizeof(header))
        {
            return;     // none yet, cut short or damaged, or an older layout: start over
        }
        memcpy(header, data.data(), sizeof(header));
        if (header[H_MAGIC] != KEYS_MAGIC || header[H_VERSION] != KEYS_VERSION || header[H_VENDOR] != device[H_VENDOR] ||
            header[H_DEVICE] != device[H_DEVICE])
        {
            return;     // an older layout, or another GPU: start over
        }
        const bool driver_changed = memcmp(header + H_UUID, device + H_UUID, VK_UUID_SIZE) != 0;
        if (driver_changed && !sCarry)
        {
            return;     // another driver: start over (research/160 P2 keeps them)
        }
        if (header[H_SESSION] == UINT32_MAX)
        {
            return;     // the count would go round to 0: start over
        }
        sSession = header[H_SESSION] + 1;
        KeyCheck check;
        size_t pos = sizeof(header);
        U32 entry[2];
        U32 refused = 0;
        while (pos + sizeof(entry) <= data.size() && sKeys.size() < KEYS_LIMIT)
        {
            memcpy(entry, data.data() + pos, sizeof(entry));
            pos += sizeof(entry);
            const U32 last_session = entry[0];
            const U32 words = entry[1];
            if (words > KEY_WORDS_LIMIT || pos + (size_t)words * sizeof(U32) > data.size())
            {
                break;      // not a key: the rest is not trusted either
            }
            std::vector<U32> key(words);
            memcpy(key.data(), data.data() + pos, (size_t)words * sizeof(U32));
            pos += (size_t)words * sizeof(U32);
            if (!check.wellFormed(key))
            {
                ++refused;  // a key this device cannot make: left out, and out of the next file
                continue;
            }
            // in 64 bits (a damaged count near 2^32 went round and kept the key); a session after this one is damage
            if (last_session < sSession && (U64)last_session + KEEP_SESSIONS >= sSession)
            {
                sKeys.emplace(std::move(key), last_session);
            }
        }
        if (refused)
        {
            LL_WARNS("VKShader") << "Pipeline keys: " << refused << " left out (not valid on this device)" << LL_ENDL;
            sKeysDirty = true;
        }
        LL_INFOS("VKShader") << "Pipeline keys: " << sKeys.size() << " from earlier sessions (session " << sSession << ")" << LL_ENDL;
        if (driver_changed)
        {
            // research/160 P2: the pipeline cache started over (LLVKDevice);
            // the keys stay, and the passes make their pipelines in the background.
            LL_INFOS("VKShader") << "Pipeline keys: the driver changed (version " << std::hex << header[H_DRIVER] << " -> "
                                 << device[H_DRIVER] << std::dec << "); " << sKeys.size()
                                 << " keys kept, their pipelines made again in the background" << LL_ENDL;
        }
    }

    // The file's contents when keys changed since the last save (and, with
    // due_only, the last save is SAVE_SECONDS old and not still being written).
    bool take_keys_data(std::string& data, bool due_only)
    {
        {
            std::lock_guard<std::mutex> lock(sKeysMutex);
            if (!sKeysDirty || (due_only && (seconds_since(sKeysSaved) < SAVE_SECONDS || sSaving.load())))
            {
                return false;
            }
            sKeysDirty = false;
            sKeysSaved = Clock::now();
            // The newest first when there are too many.
            std::vector<std::pair<U32, const std::vector<U32>*>> order;
            for (const auto& entry : sKeys)
            {
                order.push_back({ entry.second, &entry.first });
            }
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (order.size() > KEYS_LIMIT)
            {
                order.resize(KEYS_LIMIT);
            }
            U32 header[HEADER_WORDS] = {};
            device_header(header);
            header[H_MAGIC] = KEYS_MAGIC;
            header[H_VERSION] = KEYS_VERSION;
            header[H_SESSION] = sSession;
            data.append((const char*)header, sizeof(header));
            for (const auto& entry : order)
            {
                const U32 head[2] = { entry.first, (U32)entry.second->size() };
                data.append((const char*)head, sizeof(head));
                data.append((const char*)entry.second->data(), entry.second->size() * sizeof(U32));
            }
            LLVKShader::appendCacheTrailer(data);
        }
        return true;
    }

    // fix-round52 6: take_keys_data() marked the keys saved before they were written; a write
    // that failed (a full disk, the folder locked) puts the mark back, so the next save (after
    // SAVE_SECONDS, or at the end) tries again instead of the keys being lost.
    void keys_not_saved()
    {
        std::lock_guard<std::mutex> lock(sKeysMutex);
        sKeysDirty = true;
    }

    // Render thread. Written by a detached thread, as the pipeline cache is.
    void save_keys_if_due()
    {
        // research/154 W8: called every frame; looked into once a second (the
        // keys are saved every SAVE_SECONDS), so the frame neither takes
        // sKeysMutex nor builds the file's path otherwise.
        static Clock::time_point sNextLook;
        const Clock::time_point now = Clock::now();
        if (now < sNextLook)
        {
            return;
        }
        sNextLook = now + std::chrono::seconds(1);
        // The path first: taking the data marks the keys saved, so with no
        // cache folder yet they must stay unsaved for a later try.
        const std::string path = keys_path();
        if (path.empty())
        {
            return;
        }
        std::string data;
        if (!take_keys_data(data, true))
        {
            return;
        }
        sSaving = true;
        std::thread([path, data = std::move(data)]()
        {
            // A temporary file of this process and thread: a second viewer may be saving too.
            if (!LLVKShader::writeCacheFile(path, { { data.data(), data.size() } }))
            {
                keys_not_saved();   // before sSaving goes false: the save at the end sees it
            }
            sSaving = false;
        }).detach();
    }

    // ------------------------------------------------------------------
    // The prewarm

    // The programs are looked at every CHECK_FRAMES frames and must not have
    // changed for STABLE_CHECKS looks (some 1-2 s). research/160 P1: with
    // sCarry, 10 frames and 1 look: a reload links every program within one
    // frame, so the pass starts some 0.2 s after it.
    const U32 CHECK_FRAMES = sCarry ? 10 : 30;
    const U32 STABLE_CHECKS = sCarry ? 1 : 2;

    // research/160 P4: held shared while a prewarm thread creates a pipeline,
    // and exclusive by retire_pipeline_objects (the modules and layouts go).
    std::shared_mutex sObjectsMutex;
    std::atomic<U32> sObjectsEpoch{ 0 };        // retire_pipeline_objects() calls
    // Low priority threads (prewarm_threads); never destroyed (as the device), stopped at shutdown (LLVKGL::stopPrewarm).
    LLVKShader::Workers* sWorker = nullptr;

    // research/160 P4: 1, or with sCarry 1-3 by the logical cores (a cold
    // driver cache takes some 30 ms a pipeline; the frame keeps the rest).
    U32 prewarm_threads()
    {
        if (!sCarry)
        {
            return 1;
        }
        const U32 cores = std::thread::hardware_concurrency();
        return cores >= 12 ? 3 : cores >= 8 ? 2 : 1;
    }
    U32 sFrames = 0;
    U32 sSeenChanges = U32_MAX;
    U32 sStableChecks = 0;
    U32 sPrewarmedChanges = U32_MAX;

    struct Item
    {
        std::vector<U32> key;       // this session's
        PipelineRecipe recipe;
        GLuint program = 0;
        U32 link_serial = 0;
    };

    VkPrimitiveTopology topology_of_class(U32 topology_class)
    {
        // The pipeline's topology is dynamic within its class.
        return topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST :
               topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    bool is_oit_key(const std::vector<U32>& key)
    {
        const size_t colors = llmin(key[PK_COLOR_COUNT], 4u);
        return key.size() > PK_COLOR_FORMATS + colors && (key.size() - PK_COLOR_FORMATS - colors) % 3 == 1 &&
               (key.back() == PIPELINE_KEY_OIT || key.back() == PIPELINE_KEY_OIT_SPLIT);
    }

    // One pass: its items, taken in turn by the prewarm threads (research/160
    // P4); the last thread to finish logs it.
    struct Pass
    {
        std::vector<Item> items;
        U32 epoch = 0;
        Clock::time_point start;
        U32 skipped_made = 0;
        U32 skipped_oit = 0;
        U32 carried = 0;
        U32 unfit = 0;
        U32 threads = 1;
        std::atomic<size_t> next{ 0 };
        std::atomic<U32> made{ 0 };
        std::atomic<U32> lost{ 0 };
        std::atomic<U32> running{ 0 };
        std::atomic<bool> stopped{ false };
    };

    // On a prewarm thread.
    void make_pipelines(Pass& pass)
    {
        const VkDevice device = LLVKDevice::instance().getDevice();
        while (!pass.stopped.load())
        {
            const size_t index = pass.next.fetch_add(1);
            if (index >= pass.items.size())
            {
                break;
            }
            const Item& item = pass.items[index];
            VkPipeline pipeline = VK_NULL_HANDLE;
            {
                std::shared_lock<std::shared_mutex> lock(sObjectsMutex);
                if (sObjectsEpoch.load() != pass.epoch)
                {
                    pass.stopped = true;    // programs are going: the next pass makes the rest
                    break;
                }
                pipeline = create_pipeline(item.recipe);
            }
            if (!pipeline)
            {
                ++pass.lost;
                continue;
            }
            bool kept = false;
            {
                std::lock_guard<LLVKTrackerMutex> lock(sLock);
                auto program = sPrograms.find(item.program);
                if (program != sPrograms.end() && program->second.built && program->second.link_serial == item.link_serial &&
                    !sPipelines.count(item.key))
                {
                    // In the program's list, for destroy_program_objects (fix-pipeleak).
                    program->second.pipeline_keys.push_back(&sPipelines.emplace(item.key, pipeline).first->first);
                    kept = true;
                }
            }
            if (kept)
            {
                ++pass.made;
            }
            else
            {
                vkDestroyPipeline(device, pipeline, nullptr);   // never bound
                ++pass.lost;
            }
        }
        if (pass.running.fetch_sub(1) != 1)
        {
            return;
        }
        LL_INFOS("VKShader") << "Pipeline prewarm: " << pass.made.load() << " of " << pass.items.size() << " pipelines made in "
                             << seconds_since(pass.start) << " s on " << pass.threads << " low priority thread(s) (" << pass.lost.load()
                             << " not kept, " << pass.skipped_made << " made already, " << pass.skipped_oit
                             << " wait for their OIT variant, " << pass.carried << " carried over from the same program's other links, "
                             << pass.unfit << " not fitting the program's vertex inputs" << (pass.stopped.load() ? "; stopped: programs changed" : "")
                             << ")" << LL_ENDL;
    }

    // A vertex input's numeric type, as Vulkan matches a format to a shader
    // input: 0 float (and normalized, scaled), 1 signed integer, 2 unsigned.
    U32 numeric_type(U32 format)
    {
        switch (format)
        {
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            return 1;
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
            return 2;
        default:
            return 0;
        }
    }

    U32 numeric_type_of_input(U32 gl_type)
    {
        switch (gl_type)
        {
        case GL_INT: case GL_INT_VEC2: case GL_INT_VEC3: case GL_INT_VEC4:
            return 1;
        case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_VEC2: case GL_UNSIGNED_INT_VEC3: case GL_UNSIGNED_INT_VEC4:
            return 2;
        default:
            return 0;
        }
    }

    // A kept key's vertex inputs are the program's, as a draw lists them (one
    // per input, in the program's order: llvkgl_draw.cpp). True of every key
    // of the same SPIR-V; checked for all the same, so a content word two
    // programs met on never hands the driver a vertex state the shader does
    // not match. A carried key (research/160 P1: another link of the same
    // identity may read other inputs, or the same location as another type)
    // also has each format of its input's numeric type; a key of the same
    // SPIR-V has the formats a draw of it chose. Under sLock.
    bool inputs_fit(const Program& program, const std::vector<U32>& key, bool carried)
    {
        const size_t colors = llmin(key[PK_COLOR_COUNT], 4u);
        const size_t end = key.size() - (key.size() - PK_COLOR_FORMATS - colors) % 3;
        const size_t first = PK_COLOR_FORMATS + colors;
        const std::vector<LLVKShader::Input>& inputs = program.vk.inputs;
        if (end < first || (end - first) / 3 != inputs.size())
        {
            return false;
        }
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const size_t w = first + 3 * i;
            if (key[w] != inputs[i].location || (carried && numeric_type(key[w + 1]) != numeric_type_of_input(inputs[i].gl_type)))
            {
                return false;
            }
        }
        return true;
    }

    // Render thread: the kept keys of the programs linked now, as this
    // session's keys and recipes; their sessions are brought up to date.
    // research/160 P1 (sCarry): a program with no kept key of its content
    // takes the kept keys of its identity, and keeps them as its own.
    void start_pass()
    {
        const Clock::time_point start = Clock::now();
        std::vector<std::vector<U32>> kept;
        {
            std::lock_guard<std::mutex> lock(sKeysMutex);
            kept.reserve(sKeys.size());
            for (const auto& entry : sKeys)
            {
                kept.push_back(entry.first);
            }
        }
        // Read before the programs: a program's objects retired from here on stop the pass.
        const U32 epoch = sObjectsEpoch.load();
        std::vector<Item> items;
        std::vector<const std::vector<U32>*> linked;    // kept keys whose program is linked now
        std::vector<std::vector<U32>> carried;          // P1: kept keys made the new programs' own
        U32 skipped_made = 0, skipped_oit = 0, unfit = 0, pass_carried = 0;
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            std::unordered_map<U64, std::vector<GLuint>> by_content;
            for (const auto& entry : sPrograms)
            {
                const U64 content = content_key(entry.second);
                if (entry.second.built && content && !entry.second.vk.modules.empty())
                {
                    by_content[content].push_back(entry.first);
                }
            }
            // P1: the programs no kept key names by content, by identity.
            std::unordered_map<U64, std::vector<GLuint>> by_identity;
            if (sCarry)
            {
                std::unordered_set<U64> kept_contents;
                for (const std::vector<U32>& persistent : kept)
                {
                    kept_contents.insert((U64)persistent[0] | ((U64)persistent[1] << 32));
                }
                for (const auto& entry : by_content)
                {
                    if (kept_contents.count(entry.first))
                    {
                        continue;
                    }
                    for (GLuint id : entry.second)
                    {
                        const U64 identity = sPrograms[id].identity;
                        if (identity)
                        {
                            by_identity[identity].push_back(id);
                        }
                    }
                }
            }
            // The keys of other links of the same identity often meet: each once.
            std::unordered_set<std::vector<U32>, KeyHash> queued;
            auto add = [&](GLuint id, const std::vector<U32>& persistent, bool carry)
            {
                Program& program = sPrograms[id];
                std::vector<U32> key = { id, program.link_serial, (U32)(uintptr_t)program.layout };
                key.insert(key.end(), persistent.begin() + PREFIX_WORDS, persistent.end());
                if (sPipelines.count(key))
                {
                    ++skipped_made;
                    return;
                }
                if (!queued.insert(key).second)
                {
                    return;
                }
                if (!inputs_fit(program, key, carry))
                {
                    ++unfit;
                    return;
                }
                const OITVariant* oit = nullptr;
                if (is_oit_key(key))
                {
                    if (!program.oit || !program.oit->ok)
                    {
                        ++skipped_oit;
                        return;
                    }
                    oit = program.oit.get();
                }
                if (carry)
                {
                    const U64 content = content_key(program);
                    std::vector<U32> own = { (U32)content, (U32)(content >> 32), (U32)program.identity, (U32)(program.identity >> 32) };
                    own.insert(own.end(), persistent.begin() + PREFIX_WORDS, persistent.end());
                    carried.push_back(std::move(own));
                }
                Item item;
                item.key = std::move(key);
                item.program = id;
                item.link_serial = program.link_serial;
                make_pipeline_recipe(program, item.key, topology_of_class(item.key[PK_TOPOLOGY]), oit, item.recipe);
                items.push_back(std::move(item));
            };
            for (const std::vector<U32>& persistent : kept)
            {
                const U64 content = (U64)persistent[0] | ((U64)persistent[1] << 32);
                auto found = by_content.find(content);
                if (found != by_content.end())
                {
                    linked.push_back(&persistent);
                    for (GLuint id : found->second)
                    {
                        add(id, persistent, false);
                    }
                }
                const U64 identity = (U64)persistent[2] | ((U64)persistent[3] << 32);
                auto same = identity ? by_identity.find(identity) : by_identity.end();
                if (same != by_identity.end())
                {
                    for (GLuint id : same->second)
                    {
                        add(id, persistent, true);
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(sKeysMutex);
            for (const std::vector<U32>* key : linked)
            {
                auto it = sKeys.find(*key);
                if (it != sKeys.end() && it->second != sSession)
                {
                    it->second = sSession;
                    sKeysDirty = true;
                }
            }
            // Kept as the program's own (the prewarm made them, so get_pipeline
            // will not note them): the next session finds them by content.
            pass_carried = (U32)carried.size();
            for (std::vector<U32>& key : carried)
            {
                auto result = sKeys.emplace(std::move(key), sSession);
                if (result.second || result.first->second != sSession)
                {
                    result.first->second = sSession;
                    sKeysDirty = true;
                }
            }
        }
        if (items.empty())
        {
            return;
        }
        if (!sWorker)
        {
            sWorker = new LLVKShader::Workers("VKPipelinePrewarm", prewarm_threads(), true);
        }
        auto pass = std::make_shared<Pass>();
        pass->items = std::move(items);
        pass->epoch = epoch;
        pass->start = start;
        pass->skipped_made = skipped_made;
        pass->skipped_oit = skipped_oit;
        pass->carried = pass_carried;
        pass->unfit = unfit;
        pass->threads = (U32)llmin((size_t)sWorker->threads(), pass->items.size());
        pass->running = pass->threads;
        for (U32 i = 0; i < pass->threads; ++i)
        {
            sWorker->post([pass]() { make_pipelines(*pass); });
        }
    }
} // namespace

    void note_pipeline_key(const Program& program, const std::vector<U32>& key)
    {
        const U64 content = content_key(program);
        if (!sEnabled || !content || key.size() <= PK_TOPOLOGY)
        {
            return;
        }
        std::vector<U32> persistent = { (U32)content, (U32)(content >> 32), (U32)program.identity, (U32)(program.identity >> 32) };
        persistent.insert(persistent.end(), key.begin() + PK_TOPOLOGY, key.end());
        std::lock_guard<std::mutex> lock(sKeysMutex);
        load_keys();
        auto result = sKeys.emplace(std::move(persistent), sSession);
        if (result.second || result.first->second != sSession)
        {
            result.first->second = sSession;
            sKeysDirty = true;
        }
    }

    void retire_pipeline_objects()
    {
        // The epoch first, then the lock: a prewarm thread taking the lock from
        // here on sees it and stops, so the exclusive lock waits only for the
        // pipelines being created now (up to 3), never for threads taking the
        // shared lock in turn (the order std::shared_mutex grants it in is not given).
        sObjectsEpoch.fetch_add(1);
        std::unique_lock<std::shared_mutex> lock(sObjectsMutex);
    }
} // namespace llvkgl

void LLVKGL::prewarmPipelines()
{
    using namespace llvkgl;
    if (!sEnabled || LLVKShader::cacheDir().empty())
    {
        return;
    }
    if (!sKeysLoaded.load(std::memory_order_acquire))     // research/154 W8: no lock once loaded
    {
        std::lock_guard<std::mutex> lock(sKeysMutex);
        load_keys();
    }
    save_keys_if_due();
    if (++sFrames % CHECK_FRAMES || (sWorker && !sWorker->idle()) || !LLRHI::sBackendCounters)
    {
        return;
    }
    // Once the programs have stopped changing (a reload links some 250).
    U32 changes = 0, skips = 0, texture_versions = 0;
    LLRHI::sBackendCounters(changes, skips, texture_versions);
    if (changes != sSeenChanges)
    {
        sSeenChanges = changes;
        sStableChecks = 0;
        return;
    }
    if (++sStableChecks < STABLE_CHECKS || changes == sPrewarmedChanges)
    {
        return;
    }
    sPrewarmedChanges = changes;
    start_pass();
}

void LLVKGL::stopPrewarm()
{
    using namespace llvkgl;
    retire_pipeline_objects();  // a pass making pipelines stops at its next one
    if (sWorker)
    {
        sWorker->stop();        // the object stays (as the device); later posts are dropped
    }
    stop_shader_prewarm();
    LLVKTexCompress::stopBuilding();    // bc7-fast-start: the BC7 encoder's compile threads (research/88 section 13)
    // fix-round14 3: the keys since the last periodic save (up to SAVE_SECONDS
    // of them) are written now, on this thread. A periodic save still being
    // written is waited for first (up to 2 s), so its older list cannot land
    // after this one.
    if (sEnabled)
    {
        for (U32 tries = 0; sSaving.load() && tries < 200; ++tries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::string data;
        const std::string path = keys_path();
        if (!path.empty() && take_keys_data(data, false))
        {
            if (sSaving.load())
            {
                LL_WARNS("VKShader") << "Pipeline keys: a periodic save is still being written; saving over it" << LL_ENDL;
            }
            if (!LLVKShader::writeCacheFile(path, { { data.data(), data.size() } }))
            {
                keys_not_saved();
            }
        }
    }
}

#else

void LLVKGL::prewarmPipelines()
{
}

void LLVKGL::stopPrewarm()
{
}

#endif // LL_GL_FUNC_POINTER
