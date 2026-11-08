/**
 * @file llvkgl_occlusion.cpp
 * @brief Occlusion queries as visibility marks, and the depth-only path of the sun shadow casters (research/37, research/91).
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
#include "llrhihash.h"
#include "llvkocclusion.h"

#include "llrhiocclusion.h"

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // ------------------------------------------------------------------
    // Occlusion culling. The viewer culls what its groups'
    // boxes show hidden (LLOcclusionCullingGroup): it draws each box between
    // glBeginQuery and glEndQuery and reads the query a few frames later.
    // Vulkan occlusion queries lost the device at a heavy place even with valid
    // use (research/37), so a box draws with a fragment shader that marks its
    // query's word in a host-visible buffer (early fragment tests: only
    // fragments passing the depth test mark it). The marks are read once the
    // frame slot's fence was waited for. Without culling every group counted as
    // visible and Vulkan drew 2-3x what GL draws.

    // LLRHI_OCCLUSION: 2 full (default), 0 off (boxes skipped, every query
    // visible), 1 boxes drawn and marked but every query still reads visible,
    // 3 boxes drawn the generic way without marks (both for finding faults).
    const U32 sOcclusionMode = []()
    {
        U64 mode = 2;
        LLRHI::envNumber("LLRHI_OCCLUSION", 0, ~(U64)0, mode);
        return (U32)llmin(mode, (U64)3);
    }();
    VisibilityBuffer sVisibility[LLVKMirror::FRAMES_IN_FLIGHT];
    GLuint sActiveQuery = 0;
    U32 sQueriesIssued = 0;             // queries given a word
    U32 sQueriesUnmarked = 0;           // draws inside a query that could not mark

namespace
{
    VkDescriptorSetLayout sVisibilitySetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sVisibilityPool = VK_NULL_HANDLE;
    VkPipelineLayout sVisibilityLayout = VK_NULL_HANDLE;
    LLVKShader::Program sVisibilityShader;     // fragment stage: host marks (store 0)
    LLVKShader::Program sVisibilityDeviceShader;   // device marks (store 1)
    LLVKShader::Program sVisibilityVerifyShader;   // both (store 2)
    LLVKShader::Program sVisibilityVertex;     // box vertex stage
    bool sVisibilityFailed = false;

    // The fragment stages a marking pipeline takes (depth_only_pipeline's key).
    enum : U32 { MARKS_NONE = 0, MARKS_HOST, MARKS_DEVICE, MARKS_VERIFY };
    U32 mark_kind(const VisibilityBuffer& v)
    {
        return v.store == LLRHI::Occlusion::STORE_HOST ? MARKS_HOST : v.store == LLRHI::Occlusion::STORE_DEVICE ? MARKS_DEVICE : MARKS_VERIFY;
    }

    // research/179 A, since the last report (under sLock).
    struct StoreCounts
    {
        U32 copies = 0;             // frames whose device marks were copied
        U64 copied_words = 0;
        U32 lost = 0;               // harvests of device marks never copied (read visible)
        U64 compared = 0;           // store 2: queries whose host and device marks were both read
        U64 hidden_by_device = 0;   // store 2: seen by the host marks, not by the device marks
        U64 seen_by_device = 0;     // store 2: seen by the device marks, not by the host marks
    };
    StoreCounts sStoreCounts;
    std::vector<GLQuery> sGLQueries;    // by GL query name (small)
    // LLRHI_OCCLUSION_SUBGROUP=0: every fragment of a box stores its mark, as
    // before (research/111 A-OC). On by default where fragment shaders have
    // subgroup elections.
    const bool sOcclusionSubgroup = LLRHI::envFirstChar("LLRHI_OCCLUSION_SUBGROUP") != '0';

    bool init_visibility()
    {
        if (sVisibilityLayout || sVisibilityFailed)
        {
            return !sVisibilityFailed;
        }
        sVisibilityFailed = true;
        if (!LLVKDevice::instance().hasFragmentStores())
        {
            LL_WARNS("VKGL") << "no fragmentStoresAndAtomics: occlusion culling stays off" << LL_ENDL;
            return false;
        }
        // The viewer's box shaders (occlusionCubeV, shadowCubeV) scale and move a
        // unit cube; plain position programs pass a zero center and unit size.
        static const char* vertex =
            "#version 450\n"
            "layout(push_constant) uniform Box { mat4 modelview_projection; vec4 center; vec4 size; } pc;\n"
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = pc.modelview_projection * vec4(position * pc.size.xyz + pc.center.xyz, 1.0); }\n";
        // The fragment stages. Each box stores into its query's word only
        // where it passes the depth test (early fragment tests). With
        // subgroups, one fragment of each subgroup stores (all of a box's
        // fragments mark the same word): the stores drop to about 1/32
        // (research/98 H2). Helper invocations store nothing, so the election
        // is among the real fragments only.
        //  host:   1 into the host marks (binding 0), as before research/179.
        //  device: the recording's stamp into the device-local marks (binding
        //          0), read first so that only the box's first subgroup there
        //          writes (a read of device memory is cheap; the stores of
        //          every subgroup to one word in host memory are thought to be
        //          what made it slow, research/179 s6.3, not confirmed).
        //  verify: both (binding 0 the device marks, 1 the host marks).
        const bool subgroup = sOcclusionSubgroup && LLVKDevice::instance().hasFragmentSubgroupBasic();
        auto fragment = [subgroup](const char* buffers, const char* body)
        {
            std::string source = "#version 450\n";
            if (subgroup)
            {
                source += "#extension GL_KHR_shader_subgroup_basic : require\n";
            }
            source += "layout(early_fragment_tests) in;\n"
                      "layout(push_constant) uniform Visibility { layout(offset = 96) uint word; uint stamp; } pc;\n";
            source += buffers;
            source += "void main()\n{\n";
            source += subgroup ? "    if (!gl_HelperInvocation && subgroupElect())\n" : "    if (!gl_HelperInvocation)\n";
            source += "    {\n";
            source += body;
            source += "    }\n}\n";
            return source;
        };
        static const char* device_store = "        if (visible[pc.word] != pc.stamp)\n        {\n            visible[pc.word] = pc.stamp;\n        }\n";
        const std::string host_source = fragment("layout(set = 0, binding = 0, std430) buffer Visible { uint visible[]; };\n",
                                                 "        visible[pc.word] = 1u;\n");
        const std::string device_source = fragment("layout(set = 0, binding = 0, std430) buffer Visible { uint visible[]; };\n", device_store);
        const std::string verify_source =
            fragment("layout(set = 0, binding = 0, std430) buffer Visible { uint visible[]; };\n"
                     "layout(set = 0, binding = 1, std430) buffer HostVisible { uint host_visible[]; };\n",
                     (std::string(device_store) + "        host_visible[pc.word] = 1u;\n").c_str());
        LL_INFOS("VKGL") << "occlusion marks: one store per " << (subgroup ? "subgroup" : "fragment")
                         << (sOcclusionSubgroup ? "" : " (LLRHI_OCCLUSION_SUBGROUP=0)") << ", into "
                         << (LLRHI::Occlusion::sStoreMode.load(std::memory_order_relaxed) == LLRHI::Occlusion::STORE_HOST ? "host memory"
                             : LLRHI::Occlusion::sStoreMode.load(std::memory_order_relaxed) == LLRHI::Occlusion::STORE_DEVICE
                                 ? "device memory"
                                 : "device and host memory (LLRHI_OCCLUSION_STORE=2, compared)")
                         << LL_ENDL;
        if (!LLVKShader::build({ { LLVKShader::Stage::Vertex, vertex } }, sVisibilityVertex) ||
            !LLVKShader::build({ { LLVKShader::Stage::Fragment, host_source.c_str() } }, sVisibilityShader) ||
            !LLVKShader::build({ { LLVKShader::Stage::Fragment, device_source.c_str() } }, sVisibilityDeviceShader) ||
            !LLVKShader::build({ { LLVKShader::Stage::Fragment, verify_source.c_str() } }, sVisibilityVerifyShader))
        {
            LL_WARNS("VKGL") << "visibility shaders failed: " << sVisibilityVertex.log << sVisibilityShader.log << sVisibilityDeviceShader.log
                             << sVisibilityVerifyShader.log << LL_ENDL;
            return false;
        }
        VkDevice device = LLVKDevice::instance().getDevice();
        const VkDescriptorSetLayoutBinding bindings[2] =
        {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = 2;
        set_info.pBindings = bindings;
        // Two sets per slot (set, gpu_set), two buffers each.
        VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * LLVKMirror::FRAMES_IN_FLIGHT };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 2 * LLVKMirror::FRAMES_IN_FLIGHT;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        // The vertex stage reads the MVP, center and size (0-95), the fragment stage the word (96) and the stamp (100).
        VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 104 };
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &sVisibilitySetLayout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &sVisibilitySetLayout) != VK_SUCCESS ||
            vkCreateDescriptorPool(device, &pool_info, nullptr, &sVisibilityPool) != VK_SUCCESS ||
            vkCreatePipelineLayout(device, &layout_info, nullptr, &sVisibilityLayout) != VK_SUCCESS)
        {
            LL_WARNS("VKGL") << "visibility layout failed" << LL_ENDL;
            return false;
        }
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        const VkDeviceSize bytes = VISIBILITY_WORDS * sizeof(U32);
        for (VisibilityBuffer& v : sVisibility)
        {
            VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo host = {};
            host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped = {};
            if (vmaCreateBuffer(allocator, &info, &host, &v.buffer, &v.memory, &mapped) != VK_SUCCESS || !mapped.pMappedData)
            {
                LL_WARNS("VKGL") << "visibility buffer failed" << LL_ENDL;
                return false;
            }
            v.words = (U32*)mapped.pMappedData;
            memset(v.words, 0, (size_t)bytes);
            vmaFlushAllocation(allocator, v.memory, 0, VK_WHOLE_SIZE);

            // research/179 A: the device marks, and the host copy the CPU reads
            // them from (also the zeros the device marks start from).
            VkBufferCreateInfo gpu_info = info;
            gpu_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo device_local = {};
            device_local.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VkBufferCreateInfo readback_info = info;
            readback_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationInfo readback_mapped = {};
            if (vmaCreateBuffer(allocator, &gpu_info, &device_local, &v.gpu, &v.gpu_memory, nullptr) != VK_SUCCESS ||
                vmaCreateBuffer(allocator, &readback_info, &host, &v.readback, &v.readback_memory, &readback_mapped) != VK_SUCCESS ||
                !readback_mapped.pMappedData)
            {
                LL_WARNS("VKGL") << "device visibility buffer failed" << LL_ENDL;
                return false;
            }
            memset(readback_mapped.pMappedData, 0, (size_t)bytes);
            vmaFlushAllocation(allocator, v.readback_memory, 0, VK_WHOLE_SIZE);
            v.readback_words = (const U32*)readback_mapped.pMappedData;

            VkDescriptorSetLayout layouts[2] = { sVisibilitySetLayout, sVisibilitySetLayout };
            VkDescriptorSet sets[2] = {};
            VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            alloc.descriptorPool = sVisibilityPool;
            alloc.descriptorSetCount = 2;
            alloc.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS)
            {
                return false;
            }
            v.set = sets[0];
            v.gpu_set = sets[1];
            const VkDescriptorBufferInfo host_info = { v.buffer, 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo device_info = { v.gpu, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet writes[4] = {};
            const VkDescriptorSet targets[4] = { v.set, v.set, v.gpu_set, v.gpu_set };
            const VkDescriptorBufferInfo* infos[4] = { &host_info, &device_info, &device_info, &host_info };
            for (U32 i = 0; i < 4; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = targets[i];
                writes[i].dstBinding = i % 2;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = infos[i];
            }
            vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
        }
        // The device marks start as zeros (copied from the zeroed readback
        // buffers): a word never marked then never holds a stamp by chance.
        // Into the frame when one records (ordered before its boxes), else at once.
        LLVKMirror::instance().record([](VkCommandBuffer cmd)
        {
            const VkBufferCopy region = { 0, 0, VISIBILITY_WORDS * sizeof(U32) };
            for (const VisibilityBuffer& v : sVisibility)
            {
                vkCmdCopyBuffer(cmd, v.readback, v.gpu, 1, &region);
            }
            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        });
        sVisibilityFailed = false;
        return true;
    }

    // Keeps the marks of the frame the buffer holds and clears them. Only once
    // the GPU is done with that frame: its slot's fence was waited for.
    void harvest_visibility(VisibilityBuffer& v)
    {
        if (v.used && v.frame != U32_MAX)
        {
            VmaAllocator allocator = LLVKDevice::instance().getAllocator();
            const VkDeviceSize used_bytes = v.used * sizeof(U32);
            if (v.store == LLRHI::Occlusion::STORE_HOST)
            {
                vmaInvalidateAllocation(allocator, v.memory, 0, used_bytes);
                v.results.assign(v.words, v.words + v.used);
            }
            else
            {
                StoreCounts& counts = sStoreCounts;
                if (v.copied == v.begins)
                {
                    vmaInvalidateAllocation(allocator, v.readback_memory, 0, used_bytes);
                    v.results.resize(v.used);
                    for (U32 i = 0; i < v.used; ++i)
                    {
                        v.results[i] = v.readback_words[i] == v.stamp ? 1u : 0u;
                    }
                }
                else
                {
                    // Not copied (the recording never reached endFrame): every query reads visible.
                    v.results.assign(v.used, 1u);
                    ++counts.lost;
                }
                if (v.store == LLRHI::Occlusion::STORE_VERIFY)
                {
                    vmaInvalidateAllocation(allocator, v.memory, 0, used_bytes);
                    if (v.copied == v.begins)
                    {
                        for (U32 i = 0; i < v.used; ++i)
                        {
                            const bool host_seen = v.words[i] != 0;
                            const bool device_seen = v.results[i] != 0;
                            counts.hidden_by_device += host_seen && !device_seen ? 1 : 0;
                            counts.seen_by_device += !host_seen && device_seen ? 1 : 0;
                        }
                        counts.compared += v.used;
                    }
                }
            }
            v.results_frame = v.frame;
            if (v.store != LLRHI::Occlusion::STORE_DEVICE)
            {
                memset(v.words, 0, (size_t)used_bytes);
                vmaFlushAllocation(allocator, v.memory, 0, used_bytes);
            }
        }
        v.frame = U32_MAX;
        v.used = 0;
    }

    // LLVKMirror::sBeforeFrameEnd: the device marks of the frame's slot, as
    // far as its boxes used them, copied for the CPU (read at the harvest,
    // once the slot's fence was waited for; endCommandBuffers makes them
    // visible to the host).
    void copy_device_marks(VkCommandBuffer cmd)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!sVisibilityLayout)
        {
            return;
        }
        VisibilityBuffer& v = sVisibility[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        if (v.store == LLRHI::Occlusion::STORE_HOST || !v.used || v.frame == U32_MAX || v.begins != mirror.frameBegins())
        {
            return;
        }
        VkMemoryBarrier2 before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        before.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        before.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &before;
        vkCmdPipelineBarrier2(cmd, &dependency);
        const VkBufferCopy region = { 0, 0, v.used * sizeof(U32) };
        vkCmdCopyBuffer(cmd, v.gpu, v.readback, 1, &region);
        // The next recording on this slot marks the same words: after the copy read them.
        VkMemoryBarrier2 after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        after.srcAccessMask = 0;
        after.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        dependency.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(cmd, &dependency);
        v.copied = v.begins;
        ++sStoreCounts.copies;
        sStoreCounts.copied_words += v.used;
    }
} // namespace

    GLQuery* gl_query(GLuint name)
    {
        if (!name || name > (1u << 20))
        {
            return nullptr;
        }
        if (name >= sGLQueries.size())
        {
            sGLQueries.resize(llmax((size_t)name + 1, sGLQueries.size() * 2));
        }
        return &sGLQueries[name];
    }

    // The current frame's buffer (beginFrame waited for the slot's last frame).
    VisibilityBuffer* frame_visibility()
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording() || !init_visibility())
        {
            return nullptr;
        }
        VisibilityBuffer& v = sVisibility[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        if (v.frame != sFrame)
        {
            // sFrame also moves on when beginFrame starts no frame (this one still records): the marks
            // are then this recording's, not written by the GPU yet. They stay (the boxes before read
            // as visible, their frame is not v.frame any more); a harvest would read zeros as "hidden"
            // and give their words out again.
            if (v.frame == U32_MAX || v.begins != mirror.frameBegins())
            {
                harvest_visibility(v);
                v.begins = mirror.frameBegins();
                // A new recording on this slot: the store it marks with
                // (research/179 A), and its stamp, which differs from the
                // slot's earlier recordings' (frameBegins only grows).
                v.store = LLRHI::Occlusion::sStoreMode.load(std::memory_order_relaxed);
                v.stamp = (U32)v.begins ? (U32)v.begins : 1u;
            }
            v.frame = sFrame;
        }
        return &v;
    }

namespace
{
    // Whether the fence of the frame whose marks v holds was waited for:
    // beginFrame waits for a slot's fence before it counts the frame that
    // reuses the slot (or waits for all, when its target is made again or a
    // frame was never submitted). sFrame alone does not say so: it moves on
    // before beginFrame waits (resetFrameStats), and also when beginFrame
    // starts no frame (no fence waited).
    bool visibility_fenced(const VisibilityBuffer& v)
    {
        return LLVKMirror::instance().frameBegins() >= v.begins + LLVKMirror::FRAMES_IN_FLIGHT;
    }

    // read_query's two answers: whether the result is there, and whether the
    // box was seen (taking the slot's marks in first when its fence was
    // waited for). One call answers both glGetQueryObjectuiv questions
    // (research/137 N8, sOcclusionResult).
    void read_answers(GLuint name, bool& available, bool& visible)
    {
        GLQuery* q = gl_query(name);
        available = true;
        visible = true;
        if (q && q->frame != U32_MAX && q->word != U32_MAX && !q->unmarked)
        {
            VisibilityBuffer& v = sVisibility[q->slot];
            if (v.frame == q->frame && visibility_fenced(v))
            {
                harvest_visibility(v);  // the slot's fence was waited for since
            }
            if (v.results_frame == q->frame && q->word < v.results.size())
            {
                visible = v.results[q->word] != 0;
            }
            else if (v.frame == q->frame)
            {
                available = false;
            }
        }
    }
} // namespace

    // glGetQueryObjectuiv. Unknown or lost results read as
    // visible (never hide something by mistake); marks not back yet read as
    // unavailable (the viewer asks again, then reads a blocking result: visible).
    void read_query(GLuint name, GLenum pname, GLuint* params)
    {
        bool available = true;
        bool visible = true;
        read_answers(name, available, visible);
        if (pname == GL_QUERY_RESULT_AVAILABLE)
        {
            *params = available ? 1 : 0;
        }
        else
        {
            *params = !available || visible ? 1 : 0;
        }
    }

    // ------------------------------------------------------------------
    // Depth-only fast path (LLRHI::sDepthOnlyPass): the opaque sun shadow
    // casters all use one shader that only transforms the position by
    // modelview_projection_matrix. Inside that pass such draws skip the
    // generic translation (descriptors, generic pipeline key) and record a
    // dedicated pipeline with the MVP as a push constant.

    U32 sDepthOnlyDraws = 0;
    // Why draws missed a path, by reason (string literals: keyed by address, no
    // allocation per draw).
    std::unordered_map<const char*, U32> sMarkFailures;        // occlusion boxes that could not mark
    std::unordered_map<const char*, U32> sDepthOnlyFailures;   // depth-only-capable draws that went the generic way
    std::unordered_set<GLuint> sDepthOnlyPrograms;     // (with checkpoints) for naming the path in a fault report

namespace
{
    std::unordered_set<GLuint> sMarkFailedPrograms;    // logged once each
    VkPipelineLayout sDepthOnlyLayout = VK_NULL_HANDLE;
    LLVKShader::Program sDepthOnlyShader;
    bool sDepthOnlyFailed = false;
    std::unordered_map<std::vector<U32>, VkPipeline, KeyHash> sDepthOnlyPipelines;

    bool init_depth_only()
    {
        if (sDepthOnlyLayout || sDepthOnlyFailed)
        {
            return !sDepthOnlyFailed;
        }
        static const char* source =
            "#version 450\n"
            "layout(push_constant) uniform DepthOnly { mat4 modelview_projection; } pc;\n"
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = pc.modelview_projection * vec4(position, 1.0); }\n";
        if (!LLVKShader::build({ { LLVKShader::Stage::Vertex, source } }, sDepthOnlyShader))
        {
            LL_WARNS("VKGL") << "depth-only shader failed: " << sDepthOnlyShader.log << LL_ENDL;
            sDepthOnlyFailed = true;
            return false;
        }
        VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
        VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(LLVKDevice::instance().getDevice(), &info, nullptr, &sDepthOnlyLayout) != VK_SUCCESS)
        {
            sDepthOnlyFailed = true;
            return false;
        }
        return true;
    }

    // kind: MARKS_NONE, or which occlusion fragment stage (sVisibilityLayout)
    // the boxes mark with. depth_clamp: the state's (sState.depth_clamp), or a batched box's.
    VkPipeline depth_only_pipeline(const VertexInput& input, VkPrimitiveTopology prim, const LLVKMirror::Target& target, U32 kind,
                                   bool depth_clamp)
    {
        const State& s = sState;
        const bool marks = kind != MARKS_NONE;
        std::vector<U32>& key = tPipelineKey;
        key.assign({ (U32)input.format, input.stride, topology_class(prim), depth_clamp ? 1u : 0u, (U32)s.polygon_mode,
                     target.color_count, (U32)target.depth_format, target.stencil ? 1u : 0u, kind });
        for (U32 i = 0; i < target.color_count; ++i)
        {
            key.push_back(target.color_formats[i]);
        }
        auto it = sDepthOnlyPipelines.find(key);
        if (it != sDepthOnlyPipelines.end())
        {
            return it->second;
        }
        if (prim == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && !LLVKDevice::instance().hasMaintenance5())
        {
            // Points need gl_PointSize, which these vertex stages do not write (as create_pipeline).
            sDepthOnlyPipelines[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }

        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = marks ? sVisibilityVertex.modules[0] : sDepthOnlyShader.modules[0];
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = kind == MARKS_HOST     ? sVisibilityShader.modules[0] :
                           kind == MARKS_DEVICE   ? sVisibilityDeviceShader.modules[0] :
                           kind == MARKS_VERIFY   ? sVisibilityVerifyShader.modules[0] : VK_NULL_HANDLE;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = { 0, input.stride, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attribute = { 0, 0, input.format, 0 };
        VkPipelineVertexInputStateCreateInfo vertex = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex.vertexBindingDescriptionCount = 1;
        vertex.pVertexBindingDescriptions = &binding;
        vertex.vertexAttributeDescriptionCount = 1;
        vertex.pVertexAttributeDescriptions = &attribute;

        VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = prim;

        VkPipelineViewportDepthClipControlCreateInfoEXT clip_control = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
        clip_control.negativeOneToOne = VK_TRUE;    // GL clip-space depth (required, research/124 V1)
        VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.pNext = &clip_control;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = LLVKDevice::instance().usablePolygonMode(
            s.polygon_mode == GL_LINE ? VK_POLYGON_MODE_LINE : s.polygon_mode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
        raster.lineWidth = 1.f;
        raster.depthClampEnable = depth_clamp;

        VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

        // Color stays untouched (the pass masks all color writes).
        VkPipelineColorBlendAttachmentState blends[4] = {};
        VkPipelineColorBlendStateCreateInfo color = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        color.attachmentCount = target.color_count;
        color.pAttachments = blends;

        const VkDynamicState dynamic_states[] =
        {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
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
        info.stageCount = marks ? 2 : 1;
        info.pStages = stages;
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &color;
        info.pDynamicState = &dynamic;
        info.layout = marks ? sVisibilityLayout : sDepthOnlyLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(LLVKDevice::instance().getDevice(), LLVKDevice::instance().usePipelineCache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
        }
        sDepthOnlyPipelines[key] = pipeline;
        return pipeline;
    }

    // Whether the program is the plain "position x modelview_projection_matrix" shader.
    constexpr S32 DEPTH_ONLY_MVP_FROM_TABLE = -3;
} // namespace

    void forget_depth_only_program(GLuint id)
    {
        sDepthOnlyPrograms.erase(id);
        sMarkFailedPrograms.erase(id);
    }


    bool is_depth_only_program(Program& program)
    {
        if (program.depth_only_mvp == -2)
        {
            program.depth_only_mvp = -1;
            program.depth_only_box_center = program.depth_only_box_size = program.depth_only_mirror_flag = -1;
            // Material uniforms and constants left the default block: a program
            // with any is not known to be plain (the check below sees only the block).
            if (program.vk.inputs.size() == 1 && program.vk.inputs[0].name == "position" && program.sampler_slots.empty() &&
                !program.palette_columns && !program.material_vec4s && !program.consts_vec4s)
            {
                // Only the uniforms the path applies: any other may move the vertices.
                S32 mvp = -1, center = -1, size = -1, mirror = -1;
                bool other = false;
                for (const auto& uniform : program.vk.uniforms)
                {
                    if (uniform.block != program.default_block)
                    {
                        continue;
                    }
                    const bool is_mvp = uniform.name == "modelview_projection_matrix";
                    const size_t bytes = is_mvp ? 64 : uniform.name == "box_center" || uniform.name == "box_size" ? 12 : 4;
                    if (uniform.offset < 0 || (size_t)uniform.offset + bytes > program.uniform_data.size())
                    {
                        other = true;
                    }
                    else if (is_mvp)
                    {
                        mvp = uniform.offset;
                    }
                    else if (uniform.name == "box_center")
                    {
                        center = uniform.offset;
                    }
                    else if (uniform.name == "box_size")
                    {
                        size = uniform.offset;
                    }
                    else if (uniform.name == "mirror_flag")
                    {
                        mirror = uniform.offset;
                    }
                    else if (uniform.name == "clipPlane" || uniform.name == "clipSign")
                    {
                        // used only with mirror_flag set
                    }
                    else
                    {
                        other = true;
                    }
                }
                if (mvp < 0 && (program.matrix_mask & (1u << MATRIX_MVP)))
                {
                    mvp = DEPTH_ONLY_MVP_FROM_TABLE;
                }
                if (!other && mvp != -1 && (center >= 0) == (size >= 0))
                {
                    program.depth_only_mvp = mvp;
                    program.depth_only_box_center = center;
                    program.depth_only_box_size = size;
                    program.depth_only_mirror_flag = mirror;
                }
            }
        }
        return program.depth_only_mvp >= 0 || program.depth_only_mvp == DEPTH_ONLY_MVP_FROM_TABLE;
    }

    // Records the draw through the depth-only path. False: not applicable, use the generic path.
    // True: recorded, or (research/285) not drawn this frame because a buffer's remake was left
    // to a later frame (the generic path would skip it too).
    // word: an occlusion box marking that word of the frame's visibility buffer.
    bool draw_depth_only(Program& program, GLenum mode, GLint first, GLsizei count, GLsizei instances, const IndexInfo& index,
                         U32 word)
    {
        const State& s = sState;
        const bool marks = word != U32_MAX;
        // Why an occlusion box could not mark (perf report), and the program once.
        auto fail = [&](const char* reason)
        {
            if (!marks)
            {
                ++sDepthOnlyFailures[reason];
            }
            if (marks)
            {
                ++sMarkFailures[reason];
                if (!strcmp(reason, "program") && sMarkFailedPrograms.insert(sCurrentProgram).second)
                {
                    std::ostringstream out;
                    out << "occlusion box program " << sCurrentProgram << " not usable: inputs";
                    for (const auto& input : program.vk.inputs)
                    {
                        out << " " << input.name;
                    }
                    out << "; samplers " << program.sampler_slots.size() << "; default block uniforms";
                    for (const auto& uniform : program.vk.uniforms)
                    {
                        if (uniform.block == program.default_block)
                        {
                            out << " " << uniform.name << "@" << uniform.offset;
                        }
                    }
                    out << " (block size " << program.uniform_data.size() << ")";
                    LL_INFOS("VKGL") << out.str() << LL_ENDL;
                }
            }
            return false;
        };
        if (instances != 1)
        {
            return fail("instances");
        }
        if (s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3])
        {
            return fail("color mask");
        }
        if (!is_depth_only_program(program))
        {
            return fail("program");
        }
        if (program.depth_only_mirror_flag >= 0 && *(const F32*)(program.uniform_data.data() + program.depth_only_mirror_flag) > 0.f)
        {
            return fail("mirror clip");
        }
        if (!init_depth_only() || (marks && !init_visibility()) ||
            (!marks && program.depth_only_box_center >= 0))     // boxes only through the marking stage
        {
            return fail("init");
        }
        S32 attrib_index = program.input_attribs[0];
        Attrib* a = attrib_index >= 0 && attrib_index < 32 ? &sAttribs[attrib_index] : nullptr;
        Attrib direct_attrib;
        const F32* mvp = program.depth_only_mvp == DEPTH_ONLY_MVP_FROM_TABLE ? program.matrices.modelview_projection :
                         program.uniform_data.data() ? (const F32*)(program.uniform_data.data() + program.depth_only_mvp) : nullptr;
        if (tDirect)
        {
            // The direct draw's position attribute and MVP.
            a = nullptr;
            for (U32 k = 0; k < tDirect->attribute_count; ++k)
            {
                const LLRHI::DirectDraw::Attribute& source = tDirect->attributes[k];
                if (attrib_index >= 0 && source.location == (U32)attrib_index)
                {
                    direct_attrib.enabled = true;
                    direct_attrib.size = (GLint)source.size;
                    direct_attrib.type = source.type;
                    direct_attrib.normalized = source.normalized;
                    direct_attrib.integer = source.integer;
                    direct_attrib.stride = (GLsizei)source.stride;
                    direct_attrib.offset = source.offset;
                    direct_attrib.buffer = tDirect->vertex_buffer;
                    a = &direct_attrib;
                    break;
                }
            }
            if (tDirect->modelview_projection)
            {
                mvp = tDirect->modelview_projection;
            }
        }
        if (!a || !a->enabled || !a->buffer || a->type != GL_FLOAT || a->size < 3 || a->integer || !mvp)
        {
            return fail(!a ? "no position" : !a->enabled ? "position disabled" : !a->buffer ? "position not in a buffer" :
                        a->type != GL_FLOAT || a->size < 3 || a->integer ? "position format" : "no mvp");
        }
        Buffer* vertex_source = attrib_buffer(*a);
        if (!vertex_source)
        {
            return fail("vertex buffer");
        }
        Buffer* index_source = nullptr;
        if (index.indexed && !tFanIndexBuffer)
        {
            index_source = tDirect ? sBuffers.find(tDirect->index_buffer) : element_buffer();
            if (LLRHIVBCensus::sOn && index_source && index_source->owner_number)   // research/293 branch 0: counting only
            {
                LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_FIND_SIZE, index_source->owner_number);
            }
            if (!index_source || index.type == GL_UNSIGNED_BYTE)
            {
                return fail("index buffer");
            }
        }

        LLVKMirror& mirror = LLVKMirror::instance();
        ResolvedTarget target;
        if (!resolve_draw_target(target))
        {
            return fail("target");
        }
        VkBuffer vertex_buffer = gpu_buffer(*vertex_source);
        VkBuffer index_buffer = tFanIndexBuffer ? tFanIndexBuffer : index_source ? gpu_buffer(*index_source) : VK_NULL_HANDLE;
        if (!vertex_buffer || (index.indexed && !index_buffer))
        {
            // research/285: a remake left to a later frame. A depth-only draw is done
            // (nothing drawn this frame, as the generic way would skip it); an
            // occlusion box goes the generic way, which skips it, its query read visible.
            return !marks && (held_this_frame(vertex_source) || held_this_frame(index_source));
        }
        if (needs_attachment_layouts(target))
        {
            to_attachment_layouts(mirror.transferCommands(), target);
        }

        VertexInput input = { 0, attrib_format(*a), a->stride ? (U32)a->stride : (U32)a->size * 4 };
        VkPrimitiveTopology prim = topology(mode);
        // The word is the current slot's (visibility_word): its buffer says how it marks.
        const VisibilityBuffer& v = sVisibility[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        VkPipeline pipeline = depth_only_pipeline(input, prim, target.target, marks ? mark_kind(v) : MARKS_NONE, s.depth_clamp);
        if (!pipeline)
        {
            return false;
        }

        Recorded& r = sRecorded;
        VkCommandBuffer cmd = mirror.beginRendering(target.target);
        if (pipeline != r.pipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            r.pipeline = pipeline;
        }
        r.set = VK_NULL_HANDLE;     // the next generic draw rebinds its set with its own layout
        r.table_only = false;
        VkDeviceSize vertex_offset = a->offset;
        if (r.vertex_buffers.size() != 1 || r.vertex_buffers[0] != vertex_buffer || r.vertex_offsets[0] != vertex_offset)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &vertex_offset);
            r.vertex_buffers.assign(1, vertex_buffer);
            r.vertex_offsets.assign(1, vertex_offset);
        }
        set_dynamic_state(cmd, target.target, prim);
        if (marks)
        {
            struct { F32 mvp[16]; F32 center[4]; F32 size[4]; U32 word; U32 stamp; } push =
                { {}, { 0.f, 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f, 0.f }, word, v.stamp };
            memcpy(push.mvp, mvp, sizeof(push.mvp));
            if (program.depth_only_box_center >= 0)
            {
                memcpy(push.center, program.uniform_data.data() + program.depth_only_box_center, 12);
                memcpy(push.size, program.uniform_data.data() + program.depth_only_box_size, 12);
            }
            const VkDescriptorSet set = v.markSet();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sVisibilityLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, sVisibilityLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        }
        else
        {
            vkCmdPushConstants(cmd, sDepthOnlyLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
        }
        sRecorded.slot_count = 0;   // the generic path's slot numbers were overwritten
        pass_invalidate();          // its own pipeline, layout, push constants and topology
        if (index.indexed)
        {
            VkIndexType index_type = index.type == GL_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            if (index_buffer != r.index_buffer || index_type != r.index_type || r.index_offset != 0)
            {
                vkCmdBindIndexBuffer(cmd, index_buffer, 0, index_type);
                r.index_buffer = index_buffer;
                r.index_type = index_type;
                r.index_offset = 0;
            }
            U32 first_index = (U32)(index.offset / (index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2));
            LLVKDevice::instance().checkpoint(cmd, ((U64)sCurrentProgram << 32) | sDrawsReplayed);
            vkCmdDrawIndexed(cmd, (U32)count, 1, first_index, 0, 0);
        }
        else
        {
            LLVKDevice::instance().checkpoint(cmd, ((U64)sCurrentProgram << 32) | sDrawsReplayed);
            vkCmdDraw(cmd, (U32)count, 1, (U32)first, 0);
        }
        ++sDrawsReplayed;
        if (!marks)
        {
            ++sDepthOnlyDraws;
        }
        if (LLVKDevice::instance().checkpointsEnabled())
        {
            sDepthOnlyPrograms.insert(sCurrentProgram);
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Occlusion boxes in one batch (research/137 N8, llrhiocclusion.h).
    // LLPipeline::doOcclusion's boxes come as values in one call: the state
    // (program, target, pipeline, buffers, the frame's visibility set, the
    // MVP) is resolved once, and per box the query is begun as begin_query
    // does, the box marks its word as draw_depth_only's marking stage does,
    // and the query is ended. The same queries, words and frames as boxes
    // drawn one by one: the results read the same (read_query), one to two
    // frames later.

namespace
{
    namespace OC = LLRHI::Occlusion;

    struct BatchCounts
    {
        U32 lists = 0;
        U32 boxes = 0;              // queries begun
        U32 fans = 0;               // draws
        U32 unmarked = 0;           // boxes that got no word (the frame's buffer full, no pipeline): read visible
        U32 skipped = 0;            // boxes begun nowhere: a query target or name begin_query ignores too
        U32 bad_fans = 0;           // boxes with a fan outside the cube: begun, not drawn, read visible
        U32 refused = 0;            // lists refused (nothing recorded; the viewer drew them the old way)
        U32 leaks = 0;              // lists whose state changed past the batch after sOcclusionBoxesReady
        U32 by_scope[3] = {};       // boxes by LLRHI::sOcclusionScope: main view, sun shadow cameras, reflection probes
        U64 ticks = 0;              // recording, __rdtsc
    };
    BatchCounts sBatchCounts;
    // Why the backend did not take boxes (sOcclusionBoxesReady false, a list
    // refused), by reason (string literals).
    std::unordered_map<const char*, U32> sBatchRefusals;

    // LLRHI_OCCLUSION_SKIP and LLRHI_OCCLUSION_MAX (draw_one, for finding
    // faults) leave boxes out one by one: with either set, boxes go the old way.
    const bool sOcclusionDebugEnv = std::getenv("LLRHI_OCCLUSION_SKIP") || std::getenv("LLRHI_OCCLUSION_MAX");

    // The cube the boxes draw from, as the viewer's (OC::Cube): its corners,
    // then each fan as a triangle list (as fan_to_list makes the old draws'
    // fans), in a buffer of its own, written once.
    struct CubeBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = VK_NULL_HANDLE;
        std::vector<F32> positions;     // what it was made from
        std::vector<U16> indices;
        U32 fan_size = 0;
        U32 list_size = 0;              // indices of a fan as a list: 3 x (fan_size - 2)
        VkDeviceSize index_offset = 0;  // bytes: the lists after the corners
        bool failed = false;
    };
    CubeBuffer sCube;

    bool same_cube(const OC::Cube& c)
    {
        return c.positions && c.indices && c.fan_size == sCube.fan_size && (size_t)c.vertex_count * 3 == sCube.positions.size() &&
               c.index_count == sCube.indices.size() &&
               !memcmp(c.positions, sCube.positions.data(), sCube.positions.size() * sizeof(F32)) &&
               !memcmp(c.indices, sCube.indices.data(), sCube.indices.size() * sizeof(U16));
    }

    // The cube's buffer, made from the first batch's cube. Another cube later
    // is refused (the buffer may be in flight; the viewer's cube is fixed).
    bool cube_buffer(const OC::Cube& c)
    {
        if (sCube.buffer)
        {
            return same_cube(c);
        }
        if (sCube.failed)
        {
            return false;
        }
        sCube.failed = true;
        if (!c.positions || !c.indices || c.vertex_count < 3 || c.vertex_count > 0xffff || c.fan_size < 3 || c.index_count < c.fan_size ||
            c.index_count % c.fan_size)
        {
            LL_WARNS("VKGL") << "occlusion batch: the viewer's cube is not a set of fans (" << c.vertex_count << " corners, "
                             << c.index_count << " indices, fans of " << c.fan_size << ")" << LL_ENDL;
            return false;
        }
        for (U32 i = 0; i < c.index_count; ++i)
        {
            if (c.indices[i] >= c.vertex_count)
            {
                LL_WARNS("VKGL") << "occlusion batch: a cube index past its corners" << LL_ENDL;
                return false;
            }
        }
        const U32 fans = c.index_count / c.fan_size;
        const U32 list_size = 3 * (c.fan_size - 2);
        const VkDeviceSize position_bytes = (VkDeviceSize)c.vertex_count * 3 * sizeof(F32);
        const VkDeviceSize index_offset = (position_bytes + 3) & ~(VkDeviceSize)3;
        const VkDeviceSize bytes = index_offset + (VkDeviceSize)fans * list_size * sizeof(U16);

        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VmaAllocationCreateInfo where = {};
        where.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        where.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped = {};
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        if (vmaCreateBuffer(allocator, &info, &where, &sCube.buffer, &sCube.memory, &mapped) != VK_SUCCESS || !mapped.pMappedData)
        {
            if (sCube.buffer)
            {
                vmaDestroyBuffer(allocator, sCube.buffer, sCube.memory);
            }
            sCube.buffer = VK_NULL_HANDLE;
            sCube.memory = VK_NULL_HANDLE;
            LL_WARNS("VKGL") << "occlusion batch: the cube buffer failed" << LL_ENDL;
            return false;
        }
        U8* data = (U8*)mapped.pMappedData;
        memcpy(data, c.positions, (size_t)position_bytes);
        U16* lists = (U16*)(data + index_offset);
        for (U32 f = 0; f < fans; ++f)
        {
            const U16* fan = c.indices + (size_t)f * c.fan_size;
            U16* list = lists + (size_t)f * list_size;
            for (U32 i = 1; i + 1 < c.fan_size; ++i)
            {
                *list++ = fan[0];
                *list++ = fan[i];
                *list++ = fan[i + 1];
            }
        }
        vmaFlushAllocation(allocator, sCube.memory, 0, VK_WHOLE_SIZE);
        sCube.positions.assign(c.positions, c.positions + (size_t)c.vertex_count * 3);
        sCube.indices.assign(c.indices, c.indices + c.index_count);
        sCube.fan_size = c.fan_size;
        sCube.list_size = list_size;
        sCube.index_offset = index_offset;
        sCube.failed = false;
        return true;
    }

    // What the boxes read of the state now (values, not counters: a box drawn
    // the old way between two batches moves counters and puts the values back).
    // Not depth clamping: each box brings its own (Box::DEPTH_CLAMP), and a
    // water box asks for readiness inside its LLGLEnable(GL_DEPTH_CLAMP).
    U64 batch_stamp()
    {
        const State& s = sState;
        U64 h = LLRHI::FNV1A_BASIS_SHORT;
        auto mix = [&h](U64 v) { h = LLRHI::fnv1aMix(h, v); };
        mix(sCurrentProgram);
        mix((U32)s.color_mask[0] | ((U32)s.color_mask[1] << 1) | ((U32)s.color_mask[2] << 2) | ((U32)s.color_mask[3] << 3));
        mix((U32)s.depth_test | ((U32)s.depth_write << 1) | ((U32)s.cull << 3) | ((U32)s.stencil << 4) |
            ((U32)s.scissor << 5) | ((U32)s.polygon_offset << 6));
        mix(s.depth_func);
        mix(s.polygon_mode);
        mix(s.draw_framebuffer);
        for (U32 i = 0; i < 4; ++i)
        {
            mix((U32)s.viewport[i]);
            mix((U32)s.scissor_rect[i]);
        }
        mix((U64)depth_only_pass() | ((U64)suppress_translation() << 1) | ((U64)sCapture.on << 2) | ((U64)(sActiveQuery != 0) << 3) |
            ((U64)sOIT.active << 4));
        mix(census_tag());
        return h ? h : 1;
    }

    // The program the boxes draw with when the state takes them now (each box
    // would go down draw_one's marking path: draw_depth_only with a word).
    // Null: the old way (`why` says why). Under sLock.
    Program* batch_program(const char*& why)
    {
        why = nullptr;
        if (sOcclusionMode != 2 || sOcclusionDebugEnv)
        {
            why = "occlusion mode";
            return nullptr;
        }
        if (!LLVKMirror::instance().isRecording())
        {
            why = "no frame";
            return nullptr;
        }
        if (sCapture.on || depth_only_pass() || suppress_translation() || census_tag() || sActiveQuery || tDirect)
        {
            why = "state";
            return nullptr;
        }
        const State& s = sState;
        if (s.color_mask[0] || s.color_mask[1] || s.color_mask[2] || s.color_mask[3])
        {
            why = "color mask";
            return nullptr;
        }
        if (!init_visibility())
        {
            why = "init";
            return nullptr;
        }
        Program* program = current_program();
        if (!program || !program->built || !is_depth_only_program(*program) || program->depth_only_box_center < 0)
        {
            why = "program";
            return nullptr;
        }
        if (program->depth_only_mirror_flag >= 0 && *(const F32*)(program->uniform_data.data() + program->depth_only_mirror_flag) > 0.f)
        {
            why = "mirror clip";
            return nullptr;
        }
        return program;
    }

    // LLRHI::sOcclusionBoxesReady.
    bool occlusion_boxes_ready(OC::Context& context)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const char* why = nullptr;
        if (!batch_program(why))
        {
            ++sBatchRefusals[why];
            return false;
        }
        context = OC::Context();
        context.stamp = batch_stamp();
        return true;
    }

    // LLRHI::sOcclusionBoxes.
    bool occlusion_boxes(const OC::List& l)
    {
        const U64 start = __rdtsc();
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        BatchCounts& counts = sBatchCounts;
        auto refuse = [&counts](const char* why)
        {
            ++counts.refused;
            ++sBatchRefusals[why];
            return false;
        };
        if (l.stamp != batch_stamp())
        {
            // Something changed the state past the batch since it was said
            // ready (a hook called between the boxes): the boxes draw with
            // the state as it is now.
            ++counts.leaks;
            LL_WARNS_ONCE("VKGL") << "occlusion boxes '" << l.name << "': the state changed while the batch was open" << LL_ENDL;
        }
        const char* why = nullptr;
        Program* const current = batch_program(why);
        if (!current)
        {
            return refuse(why);
        }
        if (!cube_buffer(l.cube))
        {
            return refuse("cube");
        }
        Program& program = *current;
        const F32* mvp = program.depth_only_mvp == DEPTH_ONLY_MVP_FROM_TABLE ? program.matrices.modelview_projection :
                         program.uniform_data.data() ? (const F32*)(program.uniform_data.data() + program.depth_only_mvp) : nullptr;
        if (!mvp)
        {
            return refuse("no mvp");
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        ResolvedTarget target;
        if (!resolve_draw_target(target))
        {
            return refuse("target");
        }
        // The frame's buffer first: it says how the boxes mark (nothing is recorded yet).
        VisibilityBuffer* const v = frame_visibility();
        if (!v)
        {
            return refuse("visibility");
        }
        const U32 kind = mark_kind(*v);
        const VertexInput input = { 0, VK_FORMAT_R32G32B32_SFLOAT, (U32)(3 * sizeof(F32)) };
        const VkPrimitiveTopology prim = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // [0]: without depth clamping, [1]: with (made when a box asks).
        VkPipeline pipelines[2] = { depth_only_pipeline(input, prim, target.target, kind, false), VK_NULL_HANDLE };
        if (!pipelines[0])
        {
            return refuse("pipeline");
        }

        // From here every box is handled (promise 5).
        if (sRecorded.frame != sFrame)
        {
            sRecorded = Recorded();
            sRecorded.frame = sFrame;
            pass_invalidate();
        }
        if (needs_attachment_layouts(target))
        {
            to_attachment_layouts(mirror.transferCommands(), target);
        }
        Recorded& r = sRecorded;
        VkCommandBuffer cmd = mirror.beginRendering(target.target);
        // As draw_depth_only: the pipeline, the buffers, then the dynamic
        // state (the clamping variant has the same dynamic states).
        VkPipeline bound = pipelines[0];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        r.pipeline = bound;
        const VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &sCube.buffer, &vertex_offset);
        r.vertex_buffers.assign(1, sCube.buffer);
        r.vertex_offsets.assign(1, vertex_offset);
        vkCmdBindIndexBuffer(cmd, sCube.buffer, sCube.index_offset, VK_INDEX_TYPE_UINT16);
        r.index_buffer = sCube.buffer;
        r.index_type = VK_INDEX_TYPE_UINT16;
        r.index_offset = sCube.index_offset;
        set_dynamic_state(cmd, target.target, prim);
        // Set 0 holds the frame's visibility buffer now (as draw_depth_only's marks).
        const VkDescriptorSet set = v->markSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sVisibilityLayout, 0, 1, &set, 0, nullptr);
        r.set = VK_NULL_HANDLE;     // the next generic draw rebinds its set with its own layout
        r.table_only = false;
        // The MVP once; per box its center, size, word and the stamp (the push
        // layout of the marking stage: MVP 0-63, center 64, size 80, word 96, stamp 100).
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        vkCmdPushConstants(cmd, sVisibilityLayout, stages, 0, 16 * sizeof(F32), mvp);
        const U32 fans_in_cube = (U32)(sCube.indices.size() / sCube.fan_size);
        const U32 scope = llmin(l.scope, 2u);
        for (U32 i = 0; i < l.box_count; ++i)
        {
            const OC::Box& box = l.boxes[i];
            // begin_query: only samples-passed targets, only names the table takes.
            GLQuery* q = box.target == GL_ANY_SAMPLES_PASSED || box.target == GL_SAMPLES_PASSED ? gl_query(box.query) : nullptr;
            if (!q)
            {
                ++counts.skipped;
                continue;
            }
            *q = GLQuery();
            q->frame = sFrame;
            ++counts.boxes;
            ++counts.by_scope[scope];
            U32 fans[2];
            U32 fan_count = 0;
            bool bad = false;
            for (U32 k = 0; k < 2; ++k)
            {
                const U32 first = box.first_index[k];
                if (first == OC::NONE)
                {
                    continue;
                }
                if (first % sCube.fan_size || first / sCube.fan_size >= fans_in_cube)
                {
                    bad = true;
                    continue;
                }
                fans[fan_count++] = first / sCube.fan_size;
            }
            if (bad || !fan_count)
            {
                // Not a box of the cube (the viewer's bug): read visible.
                q->unmarked = true;
                ++counts.bad_fans;
                continue;
            }
            // As visibility_word for this query.
            if (v->used >= VISIBILITY_WORDS)
            {
                q->unmarked = true;
                ++sQueriesUnmarked;
                ++counts.unmarked;
                continue;
            }
            q->slot = (U32)(v - sVisibility);
            q->word = v->used++;
            ++sQueriesIssued;

            const U32 clamp = (box.flags & OC::Box::DEPTH_CLAMP) ? 1 : 0;
            if (!pipelines[clamp])
            {
                pipelines[clamp] = depth_only_pipeline(input, prim, target.target, kind, clamp != 0);
            }
            if (!pipelines[clamp])
            {
                q->unmarked = true;     // reads visible
                ++sQueriesUnmarked;
                ++counts.unmarked;
                continue;
            }
            if (pipelines[clamp] != bound)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[clamp]);
                bound = pipelines[clamp];
            }
            struct { F32 center[4]; F32 size[4]; U32 word; U32 stamp; } push =
            {
                { box.center[0], box.center[1], box.center[2], 0.f }, { box.size[0], box.size[1], box.size[2], 0.f }, q->word, v->stamp
            };
            vkCmdPushConstants(cmd, sVisibilityLayout, stages, 16 * sizeof(F32), sizeof(push), &push);
            for (U32 k = 0; k < fan_count; ++k)
            {
                LLVKDevice::instance().checkpoint(cmd, ((U64)sCurrentProgram << 32) | sDrawsReplayed);
                vkCmdDrawIndexed(cmd, sCube.list_size, 1, fans[k] * sCube.list_size, 0, 0);
                ++sDrawsReplayed;
                ++counts.fans;
            }
        }
        r.pipeline = bound;
        sRecorded.slot_count = 0;   // the generic path's push constants were overwritten
        pass_invalidate();          // its own pipeline, layout, push constants and topology
        if (LLVKDevice::instance().checkpointsEnabled())
        {
            sDepthOnlyPrograms.insert(sCurrentProgram);
        }
        ++counts.lists;
        counts.ticks += __rdtsc() - start;
        return true;
    }

    // LLRHI::sOcclusionResult: get_query_object's two answers in one hold of the lock.
    void occlusion_result(U32 name, U32* available, U32* result)
    {
        bool is_available = true;
        bool visible = true;
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            read_answers(name, is_available, visible);
        }
        if (sOcclusionMode != 2)
        {
            is_available = visible = true;  // boxes marked but unused (mode 1) or skipped (0); never hidden
        }
        if (available)
        {
            *available = is_available ? 1 : 0;
        }
        if (result)
        {
            *result = !is_available || visible ? 1 : 0;
        }
    }
} // namespace

    bool bake_occlusion_shaders()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const bool depth_only = init_depth_only();
        return init_visibility() && depth_only;
    }
} // namespace llvkgl

void LLVKOcclusion::install()
{
    // research/179 A: the device marks are copied for the CPU at each frame's end.
    LLVKMirror::sBeforeFrameEnd = &llvkgl::copy_device_marks;
    LLRHI::sOcclusionBoxesReady = &llvkgl::occlusion_boxes_ready;
    LLRHI::sOcclusionBoxes = &llvkgl::occlusion_boxes;
    LLRHI::sOcclusionResult = &llvkgl::occlusion_result;
    LL_INFOS("VKGL") << "occlusion batch available: LLRHI_OCCLUSION_BATCH=" << LLRHI::Occlusion::sMode << LL_ENDL;
}

std::string LLVKOcclusion::report(U32 frames)
{
    LLRHI::Occlusion::Stats viewer;
    LLRHI::Occlusion::takeStats(viewer);
    llvkgl::BatchCounts c;
    llvkgl::StoreCounts store;
    std::unordered_map<const char*, U32> refusals;
    {
        std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
        c = llvkgl::sBatchCounts;
        llvkgl::sBatchCounts = llvkgl::BatchCounts();
        store = llvkgl::sStoreCounts;
        llvkgl::sStoreCounts = llvkgl::StoreCounts();
        refusals.swap(llvkgl::sBatchRefusals);
    }
    const U32 store_mode = LLRHI::Occlusion::sStoreMode.load(std::memory_order_relaxed);
    if (!LLRHI::Occlusion::sMode && !c.lists && !c.refused && !viewer.batches && !viewer.results && !store.copies && !store.lost &&
        !store.compared)
    {
        return std::string();
    }
    frames = llmax(frames, 1u);
    const F64 per = 1.0 / frames;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    // research/179 A: where the marks went, and with LLRHI_OCCLUSION_STORE=2 the
    // queries whose device marks answer otherwise than the host marks (must be 0).
    out << "marks in " << (store_mode == LLRHI::Occlusion::STORE_HOST ? "host" : store_mode == LLRHI::Occlusion::STORE_DEVICE ? "device" : "device+host (verify)")
        << " memory (copies " << store.copies * per << ", words " << store.copied_words * per << ", never copied " << store.lost << ")";
    if (store.compared)
    {
        out << ", verify: compared " << store.compared << ", differ " << store.hidden_by_device + store.seen_by_device
            << " (hidden only by the device marks " << store.hidden_by_device << ", seen only by them " << store.seen_by_device << ")";
    }
    out << "; ";
    out << "lists " << c.lists * per << ", boxes " << c.boxes * per << " (main " << c.by_scope[0] * per << ", sun shadow "
        << c.by_scope[1] * per << ", probes " << c.by_scope[2] * per << "), draws " << c.fans * per << ", unmarked " << c.unmarked * per
        << ", skipped " << c.skipped * per << ", bad fans " << c.bad_fans * per << ", lists refused " << c.refused * per
        << ", state changed while open " << c.leaks * per << ", record " << c.ticks / (llvkgl::perf_ticks_per_ns() * 1e6) * per
        << " ms; viewer: batches " << viewer.batches * per << ", submissions " << viewer.submissions * per << " (cuts "
        << viewer.cuts * per << "), boxes " << viewer.boxes * per << ", not ready " << viewer.not_ready * per
        << ", boxes drawn the old way after a refusal " << viewer.refused * per << ", results " << viewer.results * per << " (kept "
        << viewer.results_kept * per << ")";
    if (!refusals.empty())
    {
        out << "; not taken:";
        for (const auto& refusal : refusals)
        {
            out << " " << refusal.first << " " << refusal.second * per;
        }
    }
    return out.str();
}

#else // !LL_GL_FUNC_POINTER

void LLVKOcclusion::install()
{
}

std::string LLVKOcclusion::report(U32)
{
    return std::string();
}

#endif // LL_GL_FUNC_POINTER
