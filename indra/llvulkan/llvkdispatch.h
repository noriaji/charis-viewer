/**
 * @file llvkdispatch.h
 * @brief Device-level entry points for the per-draw Vulkan calls (no loader trampoline).
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

#ifndef LL_LLVKDISPATCH_H
#define LL_LLVKDISPATCH_H

#include <vulkan/vulkan.h>

#include "llvkrecorder.h"

#include <atomic>
#include <cstring>
#include <new>
#include <tuple>

// Every vkCmd* call made through the loader goes through a trampoline that
// looks up the device's dispatch table. With ~100k draws a frame that adds
// up, so the per-draw calls are fetched once with vkGetDeviceProcAddr (which
// still returns validation layer entry points when layers are on). Include
// this after the Vulkan headers in files that record draws; the macros at the
// end route the usual names through the table.
namespace LLVKDispatch
{
    struct Functions
    {
        PFN_vkCmdBindPipeline CmdBindPipeline = ::vkCmdBindPipeline;
        PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = ::vkCmdBindDescriptorSets;
        PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = ::vkCmdBindVertexBuffers;
        PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2 = ::vkCmdBindVertexBuffers2;
        PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = ::vkCmdBindIndexBuffer;
        PFN_vkCmdDraw CmdDraw = ::vkCmdDraw;
        PFN_vkCmdDrawIndexed CmdDrawIndexed = ::vkCmdDrawIndexed;
        PFN_vkCmdPushConstants CmdPushConstants = ::vkCmdPushConstants;
        PFN_vkCmdSetViewport CmdSetViewport = ::vkCmdSetViewport;
        PFN_vkCmdSetScissor CmdSetScissor = ::vkCmdSetScissor;
        PFN_vkCmdSetCullMode CmdSetCullMode = ::vkCmdSetCullMode;
        PFN_vkCmdSetFrontFace CmdSetFrontFace = ::vkCmdSetFrontFace;
        PFN_vkCmdSetPrimitiveTopology CmdSetPrimitiveTopology = ::vkCmdSetPrimitiveTopology;
        PFN_vkCmdSetDepthTestEnable CmdSetDepthTestEnable = ::vkCmdSetDepthTestEnable;
        PFN_vkCmdSetDepthWriteEnable CmdSetDepthWriteEnable = ::vkCmdSetDepthWriteEnable;
        PFN_vkCmdSetDepthCompareOp CmdSetDepthCompareOp = ::vkCmdSetDepthCompareOp;
        PFN_vkCmdSetDepthBiasEnable CmdSetDepthBiasEnable = ::vkCmdSetDepthBiasEnable;
        PFN_vkCmdSetDepthBias CmdSetDepthBias = ::vkCmdSetDepthBias;
        PFN_vkCmdSetStencilTestEnable CmdSetStencilTestEnable = ::vkCmdSetStencilTestEnable;
        PFN_vkCmdSetStencilOp CmdSetStencilOp = ::vkCmdSetStencilOp;
        PFN_vkCmdSetStencilCompareMask CmdSetStencilCompareMask = ::vkCmdSetStencilCompareMask;
        PFN_vkCmdSetStencilWriteMask CmdSetStencilWriteMask = ::vkCmdSetStencilWriteMask;
        PFN_vkCmdSetStencilReference CmdSetStencilReference = ::vkCmdSetStencilReference;
        PFN_vkUpdateDescriptorSets UpdateDescriptorSets = ::vkUpdateDescriptorSets;
        PFN_vkAllocateDescriptorSets AllocateDescriptorSets = ::vkAllocateDescriptorSets;
        PFN_vkCmdBeginRendering CmdBeginRendering = ::vkCmdBeginRendering;
        PFN_vkCmdEndRendering CmdEndRendering = ::vkCmdEndRendering;
        PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2 = ::vkCmdPipelineBarrier2;
        PFN_vkCmdCopyBuffer CmdCopyBuffer = ::vkCmdCopyBuffer;
        PFN_vkCmdBlitImage CmdBlitImage = ::vkCmdBlitImage;
        PFN_vkCmdCopyImage CmdCopyImage = ::vkCmdCopyImage;
        PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = ::vkCmdCopyBufferToImage;
        PFN_vkCmdClearColorImage CmdClearColorImage = ::vkCmdClearColorImage;
        PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage = ::vkCmdClearDepthStencilImage;
        PFN_vkCmdClearAttachments CmdClearAttachments = ::vkCmdClearAttachments;
        PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = ::vkCmdCopyImageToBuffer;
        PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect = ::vkCmdDrawIndexedIndirect;
        PFN_vkCmdDrawIndexedIndirectCount CmdDrawIndexedIndirectCount = ::vkCmdDrawIndexedIndirectCount;
        PFN_vkCmdDispatch CmdDispatch = ::vkCmdDispatch;
        PFN_vkCmdResetQueryPool CmdResetQueryPool = ::vkCmdResetQueryPool;
        PFN_vkCmdWriteTimestamp2 CmdWriteTimestamp2 = ::vkCmdWriteTimestamp2;
        PFN_vkCmdBeginQuery CmdBeginQuery = ::vkCmdBeginQuery;
        PFN_vkCmdEndQuery CmdEndQuery = ::vkCmdEndQuery;
        PFN_vkCmdFillBuffer CmdFillBuffer = ::vkCmdFillBuffer;
        PFN_vkCmdExecuteCommands CmdExecuteCommands = ::vkCmdExecuteCommands;     // research/208 13.3 (vk-s4a4)
    };

    inline Functions sFns;

    // Calls through the table, counted (LLVKGL::perfReport shows them per frame).
    enum Counter
    {
        C_CmdBindPipeline,
        C_CmdBindDescriptorSets,
        C_CmdBindVertexBuffers,
        C_CmdBindVertexBuffers2,
        C_CmdBindIndexBuffer,
        C_CmdDraw,
        C_CmdDrawIndexed,
        C_CmdPushConstants,
        C_CmdSetViewport,
        C_CmdSetScissor,
        C_CmdSetCullMode,
        C_CmdSetFrontFace,
        C_CmdSetPrimitiveTopology,
        C_CmdSetDepthTestEnable,
        C_CmdSetDepthWriteEnable,
        C_CmdSetDepthCompareOp,
        C_CmdSetDepthBiasEnable,
        C_CmdSetDepthBias,
        C_CmdSetStencilTestEnable,
        C_CmdSetStencilOp,
        C_CmdSetStencilCompareMask,
        C_CmdSetStencilWriteMask,
        C_CmdSetStencilReference,
        C_UpdateDescriptorSets,
        C_AllocateDescriptorSets,
        C_CmdBeginRendering,
        C_CmdEndRendering,
        C_CmdPipelineBarrier2,
        C_CmdCopyBuffer,
        C_CmdBlitImage,
        C_CmdCopyImage,
        C_CmdCopyBufferToImage,
        C_CmdClearColorImage,
        C_CmdClearDepthStencilImage,
        C_CmdClearAttachments,
        C_CmdCopyImageToBuffer,
        C_CmdDrawIndexedIndirect,
        C_CmdDrawIndexedIndirectCount,
        C_CmdDispatch,
        C_CmdResetQueryPool,
        C_CmdWriteTimestamp2,
        C_CmdBeginQuery,
        C_CmdEndQuery,
        C_CmdFillBuffer,
        C_CmdExecuteCommands,
        COUNTER_COUNT
    };
    inline const char* const sCounterNames[COUNTER_COUNT] = { "vkCmdBindPipeline", "vkCmdBindDescriptorSets", "vkCmdBindVertexBuffers", "vkCmdBindVertexBuffers2", "vkCmdBindIndexBuffer", "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdPushConstants", "vkCmdSetViewport", "vkCmdSetScissor", "vkCmdSetCullMode", "vkCmdSetFrontFace", "vkCmdSetPrimitiveTopology", "vkCmdSetDepthTestEnable", "vkCmdSetDepthWriteEnable", "vkCmdSetDepthCompareOp", "vkCmdSetDepthBiasEnable", "vkCmdSetDepthBias", "vkCmdSetStencilTestEnable", "vkCmdSetStencilOp", "vkCmdSetStencilCompareMask", "vkCmdSetStencilWriteMask", "vkCmdSetStencilReference", "vkUpdateDescriptorSets", "vkAllocateDescriptorSets", "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdPipelineBarrier2", "vkCmdCopyBuffer", "vkCmdBlitImage", "vkCmdCopyImage", "vkCmdCopyBufferToImage", "vkCmdClearColorImage", "vkCmdClearDepthStencilImage", "vkCmdClearAttachments", "vkCmdCopyImageToBuffer", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndexedIndirectCount", "vkCmdDispatch", "vkCmdResetQueryPool", "vkCmdWriteTimestamp2", "vkCmdBeginQuery", "vkCmdEndQuery", "vkCmdFillBuffer", "vkCmdExecuteCommands" };
    inline unsigned int sCounts[COUNTER_COUNT] = {};
    static_assert(COUNTER_COUNT <= LLVKRecorder::MAX_COUNTERS, "a stream counts its calls in an array of MAX_COUNTERS");
    // This thread's counters: the shared ones, or those of the stream it fills
    // (LLVKRecorder::Fill; added to these once the stream has been replayed).
    inline unsigned int* counts()
    {
        unsigned int* own = LLVKRecorder::tCounts;
        return own ? own : sCounts;
    }

    inline void load(VkDevice device)
    {
        if (auto fn = (PFN_vkCmdBindPipeline)vkGetDeviceProcAddr(device, "vkCmdBindPipeline")) sFns.CmdBindPipeline = fn;
        if (auto fn = (PFN_vkCmdBindDescriptorSets)vkGetDeviceProcAddr(device, "vkCmdBindDescriptorSets")) sFns.CmdBindDescriptorSets = fn;
        if (auto fn = (PFN_vkCmdBindVertexBuffers)vkGetDeviceProcAddr(device, "vkCmdBindVertexBuffers")) sFns.CmdBindVertexBuffers = fn;
        if (auto fn = (PFN_vkCmdBindVertexBuffers2)vkGetDeviceProcAddr(device, "vkCmdBindVertexBuffers2")) sFns.CmdBindVertexBuffers2 = fn;
        if (auto fn = (PFN_vkCmdBindIndexBuffer)vkGetDeviceProcAddr(device, "vkCmdBindIndexBuffer")) sFns.CmdBindIndexBuffer = fn;
        if (auto fn = (PFN_vkCmdDraw)vkGetDeviceProcAddr(device, "vkCmdDraw")) sFns.CmdDraw = fn;
        if (auto fn = (PFN_vkCmdDrawIndexed)vkGetDeviceProcAddr(device, "vkCmdDrawIndexed")) sFns.CmdDrawIndexed = fn;
        if (auto fn = (PFN_vkCmdPushConstants)vkGetDeviceProcAddr(device, "vkCmdPushConstants")) sFns.CmdPushConstants = fn;
        if (auto fn = (PFN_vkCmdSetViewport)vkGetDeviceProcAddr(device, "vkCmdSetViewport")) sFns.CmdSetViewport = fn;
        if (auto fn = (PFN_vkCmdSetScissor)vkGetDeviceProcAddr(device, "vkCmdSetScissor")) sFns.CmdSetScissor = fn;
        if (auto fn = (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(device, "vkCmdSetCullMode")) sFns.CmdSetCullMode = fn;
        if (auto fn = (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(device, "vkCmdSetFrontFace")) sFns.CmdSetFrontFace = fn;
        if (auto fn = (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(device, "vkCmdSetPrimitiveTopology")) sFns.CmdSetPrimitiveTopology = fn;
        if (auto fn = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(device, "vkCmdSetDepthTestEnable")) sFns.CmdSetDepthTestEnable = fn;
        if (auto fn = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(device, "vkCmdSetDepthWriteEnable")) sFns.CmdSetDepthWriteEnable = fn;
        if (auto fn = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(device, "vkCmdSetDepthCompareOp")) sFns.CmdSetDepthCompareOp = fn;
        if (auto fn = (PFN_vkCmdSetDepthBiasEnable)vkGetDeviceProcAddr(device, "vkCmdSetDepthBiasEnable")) sFns.CmdSetDepthBiasEnable = fn;
        if (auto fn = (PFN_vkCmdSetDepthBias)vkGetDeviceProcAddr(device, "vkCmdSetDepthBias")) sFns.CmdSetDepthBias = fn;
        if (auto fn = (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(device, "vkCmdSetStencilTestEnable")) sFns.CmdSetStencilTestEnable = fn;
        if (auto fn = (PFN_vkCmdSetStencilOp)vkGetDeviceProcAddr(device, "vkCmdSetStencilOp")) sFns.CmdSetStencilOp = fn;
        if (auto fn = (PFN_vkCmdSetStencilCompareMask)vkGetDeviceProcAddr(device, "vkCmdSetStencilCompareMask")) sFns.CmdSetStencilCompareMask = fn;
        if (auto fn = (PFN_vkCmdSetStencilWriteMask)vkGetDeviceProcAddr(device, "vkCmdSetStencilWriteMask")) sFns.CmdSetStencilWriteMask = fn;
        if (auto fn = (PFN_vkCmdSetStencilReference)vkGetDeviceProcAddr(device, "vkCmdSetStencilReference")) sFns.CmdSetStencilReference = fn;
        if (auto fn = (PFN_vkUpdateDescriptorSets)vkGetDeviceProcAddr(device, "vkUpdateDescriptorSets")) sFns.UpdateDescriptorSets = fn;
        if (auto fn = (PFN_vkAllocateDescriptorSets)vkGetDeviceProcAddr(device, "vkAllocateDescriptorSets")) sFns.AllocateDescriptorSets = fn;
        if (auto fn = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(device, "vkCmdBeginRendering")) sFns.CmdBeginRendering = fn;
        if (auto fn = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(device, "vkCmdEndRendering")) sFns.CmdEndRendering = fn;
        if (auto fn = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2")) sFns.CmdPipelineBarrier2 = fn;
        if (auto fn = (PFN_vkCmdCopyBuffer)vkGetDeviceProcAddr(device, "vkCmdCopyBuffer")) sFns.CmdCopyBuffer = fn;
        if (auto fn = (PFN_vkCmdBlitImage)vkGetDeviceProcAddr(device, "vkCmdBlitImage")) sFns.CmdBlitImage = fn;
        if (auto fn = (PFN_vkCmdCopyImage)vkGetDeviceProcAddr(device, "vkCmdCopyImage")) sFns.CmdCopyImage = fn;
        if (auto fn = (PFN_vkCmdCopyBufferToImage)vkGetDeviceProcAddr(device, "vkCmdCopyBufferToImage")) sFns.CmdCopyBufferToImage = fn;
        if (auto fn = (PFN_vkCmdClearColorImage)vkGetDeviceProcAddr(device, "vkCmdClearColorImage")) sFns.CmdClearColorImage = fn;
        if (auto fn = (PFN_vkCmdClearDepthStencilImage)vkGetDeviceProcAddr(device, "vkCmdClearDepthStencilImage")) sFns.CmdClearDepthStencilImage = fn;
        if (auto fn = (PFN_vkCmdClearAttachments)vkGetDeviceProcAddr(device, "vkCmdClearAttachments")) sFns.CmdClearAttachments = fn;
        if (auto fn = (PFN_vkCmdCopyImageToBuffer)vkGetDeviceProcAddr(device, "vkCmdCopyImageToBuffer")) sFns.CmdCopyImageToBuffer = fn;
        if (auto fn = (PFN_vkCmdDrawIndexedIndirect)vkGetDeviceProcAddr(device, "vkCmdDrawIndexedIndirect")) sFns.CmdDrawIndexedIndirect = fn;
        if (auto fn = (PFN_vkCmdDrawIndexedIndirectCount)vkGetDeviceProcAddr(device, "vkCmdDrawIndexedIndirectCount")) sFns.CmdDrawIndexedIndirectCount = fn;
        if (auto fn = (PFN_vkCmdDispatch)vkGetDeviceProcAddr(device, "vkCmdDispatch")) sFns.CmdDispatch = fn;
        if (auto fn = (PFN_vkCmdResetQueryPool)vkGetDeviceProcAddr(device, "vkCmdResetQueryPool")) sFns.CmdResetQueryPool = fn;
        if (auto fn = (PFN_vkCmdWriteTimestamp2)vkGetDeviceProcAddr(device, "vkCmdWriteTimestamp2")) sFns.CmdWriteTimestamp2 = fn;
        if (auto fn = (PFN_vkCmdBeginQuery)vkGetDeviceProcAddr(device, "vkCmdBeginQuery")) sFns.CmdBeginQuery = fn;
        if (auto fn = (PFN_vkCmdEndQuery)vkGetDeviceProcAddr(device, "vkCmdEndQuery")) sFns.CmdEndQuery = fn;
        if (auto fn = (PFN_vkCmdFillBuffer)vkGetDeviceProcAddr(device, "vkCmdFillBuffer")) sFns.CmdFillBuffer = fn;
        if (auto fn = (PFN_vkCmdExecuteCommands)vkGetDeviceProcAddr(device, "vkCmdExecuteCommands")) sFns.CmdExecuteCommands = fn;
    }
}

// The command calls, deferred to the recording worker while LLVKRecorder
// defers the command buffer (llvkrecorder.h), direct otherwise. Pointed-to
// arguments are copied into the packet after its fixed part.
namespace LLVKRecorder
{
    // Fixed-argument commands: the arguments are kept by value.
    template <typename Fn, typename... A>
    struct ValuePacket
    {
        Header header;
        Fn fn;
        VkCommandBuffer cmd;
        std::tuple<A...> args;
        static void run(void* self)
        {
            ValuePacket* p = (ValuePacket*)self;
            std::apply([p](A... a) { p->fn(p->cmd, a...); }, p->args);
        }
    };

    template <typename Fn, typename... A>
    inline void value(Fn fn, VkCommandBuffer cmd, A... a)
    {
        if (!deferred(cmd))
        {
            fn(cmd, a...);
            return;
        }
        using P = ValuePacket<Fn, A...>;
        P* p = new (reserve(align8(sizeof(P)))) P{ { &P::run, (uint32_t)align8(sizeof(P)) }, fn, cmd, std::tuple<A...>(a...) };
        (void)p;
    }

    // A packet with room for `extra` bytes of copied arrays after its fixed part.
    template <typename P>
    inline P* packet(void (*run)(void*), size_t extra)
    {
        const size_t bytes = align8(sizeof(P) + extra);
        P* p = (P*)reserve(bytes);
        p->header = { run, (uint32_t)bytes };
        return p;
    }

    // Copies count elements after offset into the packet; returns the new offset.
    template <typename T>
    inline size_t put(void* p, size_t offset, const T* items, uint32_t count)
    {
        offset = (offset + alignof(T) - 1) & ~(alignof(T) - 1);
        if (items && count)
        {
            memcpy((unsigned char*)p + offset, items, sizeof(T) * count);
        }
        return offset + sizeof(T) * count;
    }
    template <typename T>
    inline T* at(void* p, size_t& offset, uint32_t count)
    {
        offset = (offset + alignof(T) - 1) & ~(alignof(T) - 1);
        T* items = (T*)((unsigned char*)p + offset);
        offset += sizeof(T) * count;
        return items;
    }
    template <typename T>
    constexpr size_t room(uint32_t count) { return sizeof(T) * count + alignof(T); }

    inline void CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint a, VkPipeline b) { value(LLVKDispatch::sFns.CmdBindPipeline, c, a, b); }
    inline void CmdBindIndexBuffer(VkCommandBuffer c, VkBuffer a, VkDeviceSize b, VkIndexType d) { value(LLVKDispatch::sFns.CmdBindIndexBuffer, c, a, b, d); }
    inline void CmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { value(LLVKDispatch::sFns.CmdDraw, c, a, b, d, e); }
    inline void CmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f) { value(LLVKDispatch::sFns.CmdDrawIndexed, c, a, b, d, e, f); }
    inline void CmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer a, VkDeviceSize b, uint32_t d, uint32_t e) { value(LLVKDispatch::sFns.CmdDrawIndexedIndirect, c, a, b, d, e); }
    inline void CmdDrawIndexedIndirectCount(VkCommandBuffer c, VkBuffer a, VkDeviceSize b, VkBuffer d, VkDeviceSize e, uint32_t f, uint32_t g) { value(LLVKDispatch::sFns.CmdDrawIndexedIndirectCount, c, a, b, d, e, f, g); }
    inline void CmdDispatch(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d) { value(LLVKDispatch::sFns.CmdDispatch, c, a, b, d); }
    inline void CmdResetQueryPool(VkCommandBuffer c, VkQueryPool a, uint32_t b, uint32_t d) { value(LLVKDispatch::sFns.CmdResetQueryPool, c, a, b, d); }
    inline void CmdWriteTimestamp2(VkCommandBuffer c, VkPipelineStageFlags2 a, VkQueryPool b, uint32_t d) { value(LLVKDispatch::sFns.CmdWriteTimestamp2, c, a, b, d); }
    inline void CmdBeginQuery(VkCommandBuffer c, VkQueryPool a, uint32_t b, VkQueryControlFlags d) { value(LLVKDispatch::sFns.CmdBeginQuery, c, a, b, d); }
    inline void CmdEndQuery(VkCommandBuffer c, VkQueryPool a, uint32_t b) { value(LLVKDispatch::sFns.CmdEndQuery, c, a, b); }
    // bugloop r7: a direct call recorded into the frame's upload buffer on the main thread while
    // the recording worker replayed into the same pool (THREADING ERROR with LLRHI_SCENE=on).
    inline void CmdFillBuffer(VkCommandBuffer c, VkBuffer a, VkDeviceSize b, VkDeviceSize d, uint32_t e) { value(LLVKDispatch::sFns.CmdFillBuffer, c, a, b, d, e); }
    // research/208 13.3 (vk-s4a4): secondary command buffers. CmdExecuteLater
    // reads the buffer when it replays (a thread records it after the main
    // thread recorded the call: it is complete once the stream spliced before
    // the call is, and replayed is set by that stream's last packet,
    // CmdMarkReplayed). Null, or the stream skipped (its splice timed out):
    // nothing is executed (an empty rendering scope).
    inline void CmdExecuteCommands(VkCommandBuffer c, uint32_t n, const VkCommandBuffer* buffers)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdExecuteCommands(c, n, buffers);
            return;
        }
        struct P { Header header; VkCommandBuffer c; uint32_t count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdExecuteCommands(p->c, p->count, at<VkCommandBuffer>(p, o, p->count));
        }, room<VkCommandBuffer>(n));
        *p = { p->header, c, n };
        put(p, sizeof(P), buffers, n);
    }
    inline void execute_later(VkCommandBuffer c, const std::atomic<VkCommandBuffer>* buffer, const std::atomic<bool>* replayed)
    {
        const VkCommandBuffer secondary = buffer->load(std::memory_order_acquire);
        if (secondary && replayed->load(std::memory_order_acquire))
        {
            LLVKDispatch::sFns.CmdExecuteCommands(c, 1, &secondary);
        }
    }
    inline void mark_replayed(VkCommandBuffer, std::atomic<bool>* replayed)
    {
        replayed->store(true, std::memory_order_release);
    }
    inline void CmdExecuteLater(VkCommandBuffer c, const std::atomic<VkCommandBuffer>* buffer, const std::atomic<bool>* replayed)
    {
        value(&execute_later, c, buffer, replayed);
    }
    inline void CmdMarkReplayed(VkCommandBuffer c, std::atomic<bool>* replayed)
    {
        value(&mark_replayed, c, replayed);
    }
    inline void CmdSetCullMode(VkCommandBuffer c, VkCullModeFlags a) { value(LLVKDispatch::sFns.CmdSetCullMode, c, a); }
    inline void CmdSetFrontFace(VkCommandBuffer c, VkFrontFace a) { value(LLVKDispatch::sFns.CmdSetFrontFace, c, a); }
    inline void CmdSetPrimitiveTopology(VkCommandBuffer c, VkPrimitiveTopology a) { value(LLVKDispatch::sFns.CmdSetPrimitiveTopology, c, a); }
    inline void CmdSetDepthTestEnable(VkCommandBuffer c, VkBool32 a) { value(LLVKDispatch::sFns.CmdSetDepthTestEnable, c, a); }
    inline void CmdSetDepthWriteEnable(VkCommandBuffer c, VkBool32 a) { value(LLVKDispatch::sFns.CmdSetDepthWriteEnable, c, a); }
    inline void CmdSetDepthCompareOp(VkCommandBuffer c, VkCompareOp a) { value(LLVKDispatch::sFns.CmdSetDepthCompareOp, c, a); }
    inline void CmdSetDepthBiasEnable(VkCommandBuffer c, VkBool32 a) { value(LLVKDispatch::sFns.CmdSetDepthBiasEnable, c, a); }
    inline void CmdSetDepthBias(VkCommandBuffer c, float a, float b, float d) { value(LLVKDispatch::sFns.CmdSetDepthBias, c, a, b, d); }
    inline void CmdSetStencilTestEnable(VkCommandBuffer c, VkBool32 a) { value(LLVKDispatch::sFns.CmdSetStencilTestEnable, c, a); }
    inline void CmdSetStencilOp(VkCommandBuffer c, VkStencilFaceFlags a, VkStencilOp b, VkStencilOp d, VkStencilOp e, VkCompareOp f) { value(LLVKDispatch::sFns.CmdSetStencilOp, c, a, b, d, e, f); }
    inline void CmdSetStencilCompareMask(VkCommandBuffer c, VkStencilFaceFlags a, uint32_t b) { value(LLVKDispatch::sFns.CmdSetStencilCompareMask, c, a, b); }
    inline void CmdSetStencilWriteMask(VkCommandBuffer c, VkStencilFaceFlags a, uint32_t b) { value(LLVKDispatch::sFns.CmdSetStencilWriteMask, c, a, b); }
    inline void CmdSetStencilReference(VkCommandBuffer c, VkStencilFaceFlags a, uint32_t b) { value(LLVKDispatch::sFns.CmdSetStencilReference, c, a, b); }
    inline void CmdEndRendering(VkCommandBuffer c) { value(LLVKDispatch::sFns.CmdEndRendering, c); }

    inline void CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bind, VkPipelineLayout layout, uint32_t first, uint32_t count,
                                      const VkDescriptorSet* sets, uint32_t dynamic_count, const uint32_t* dynamic)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdBindDescriptorSets(c, bind, layout, first, count, sets, dynamic_count, dynamic);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkPipelineBindPoint bind; VkPipelineLayout layout; uint32_t first, count, dynamic_count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            const VkDescriptorSet* sets = at<VkDescriptorSet>(p, o, p->count);
            const uint32_t* dynamic = at<uint32_t>(p, o, p->dynamic_count);
            LLVKDispatch::sFns.CmdBindDescriptorSets(p->c, p->bind, p->layout, p->first, p->count, sets, p->dynamic_count,
                                                     p->dynamic_count ? dynamic : nullptr);
        }, room<VkDescriptorSet>(count) + room<uint32_t>(dynamic_count));
        *p = { p->header, c, bind, layout, first, count, dynamic_count };
        size_t o = put(p, sizeof(P), sets, count);
        put(p, o, dynamic, dynamic_count);
    }

    inline void CmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdBindVertexBuffers(c, first, count, buffers, offsets);
            return;
        }
        struct P { Header header; VkCommandBuffer c; uint32_t first, count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            const VkBuffer* buffers = at<VkBuffer>(p, o, p->count);
            const VkDeviceSize* offsets = at<VkDeviceSize>(p, o, p->count);
            LLVKDispatch::sFns.CmdBindVertexBuffers(p->c, p->first, p->count, buffers, offsets);
        }, room<VkBuffer>(count) + room<VkDeviceSize>(count));
        *p = { p->header, c, first, count };
        size_t o = put(p, sizeof(P), buffers, count);
        put(p, o, offsets, count);
    }

    inline void CmdBindVertexBuffers2(VkCommandBuffer c, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets,
                                      const VkDeviceSize* sizes, const VkDeviceSize* strides)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdBindVertexBuffers2(c, first, count, buffers, offsets, sizes, strides);
            return;
        }
        struct P { Header header; VkCommandBuffer c; uint32_t first, count; bool sizes, strides; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            const VkBuffer* buffers = at<VkBuffer>(p, o, p->count);
            const VkDeviceSize* offsets = at<VkDeviceSize>(p, o, p->count);
            const VkDeviceSize* sizes = at<VkDeviceSize>(p, o, p->sizes ? p->count : 0);
            const VkDeviceSize* strides = at<VkDeviceSize>(p, o, p->strides ? p->count : 0);
            LLVKDispatch::sFns.CmdBindVertexBuffers2(p->c, p->first, p->count, buffers, offsets, p->sizes ? sizes : nullptr,
                                                     p->strides ? strides : nullptr);
        }, room<VkBuffer>(count) + 3 * room<VkDeviceSize>(count));
        *p = { p->header, c, first, count, sizes != nullptr, strides != nullptr };
        size_t o = put(p, sizeof(P), buffers, count);
        o = put(p, o, offsets, count);
        o = put(p, o, sizes, sizes ? count : 0);
        put(p, o, strides, strides ? count : 0);
    }

    inline void CmdPushConstants(VkCommandBuffer c, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                 const void* values)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdPushConstants(c, layout, stages, offset, size, values);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t offset, size; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            const unsigned char* values = at<unsigned char>(p, o, p->size);
            LLVKDispatch::sFns.CmdPushConstants(p->c, p->layout, p->stages, p->offset, p->size, values);
        }, size);
        *p = { p->header, c, layout, stages, offset, size };
        put(p, sizeof(P), (const unsigned char*)values, size);
    }

    inline void CmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t count, const VkViewport* viewports)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdSetViewport(c, first, count, viewports);
            return;
        }
        struct P { Header header; VkCommandBuffer c; uint32_t first, count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdSetViewport(p->c, p->first, p->count, at<VkViewport>(p, o, p->count));
        }, room<VkViewport>(count));
        *p = { p->header, c, first, count };
        put(p, sizeof(P), viewports, count);
    }

    inline void CmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t count, const VkRect2D* rects)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdSetScissor(c, first, count, rects);
            return;
        }
        struct P { Header header; VkCommandBuffer c; uint32_t first, count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdSetScissor(p->c, p->first, p->count, at<VkRect2D>(p, o, p->count));
        }, room<VkRect2D>(count));
        *p = { p->header, c, first, count };
        put(p, sizeof(P), rects, count);
    }

    inline void CmdCopyBuffer(VkCommandBuffer c, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy* regions)
    {
        if (count == 0)
        {
            return;     // nothing to copy (and regionCount 0 is invalid usage)
        }
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdCopyBuffer(c, src, dst, count, regions);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkBuffer src, dst; uint32_t count; };
        // A packet must fit a recorder chunk: a long region list (the GPU
        // lists' table writes after a teleport, research/96) goes as several
        // copies. The regions are independent, so the result is the same.
        constexpr uint32_t MAX_REGIONS = (uint32_t)((MAX_PACKET_BYTES - align8(sizeof(P) + alignof(VkBufferCopy))) / sizeof(VkBufferCopy));
        static_assert(MAX_REGIONS > 0 && align8(sizeof(P) + room<VkBufferCopy>(MAX_REGIONS)) <= MAX_PACKET_BYTES,
                      "a copy packet with MAX_REGIONS regions fits a recorder chunk");
        do
        {
            const uint32_t n = count < MAX_REGIONS ? count : MAX_REGIONS;
            P* p = packet<P>([](void* s)
            {
                P* p = (P*)s;
                size_t o = sizeof(P);
                LLVKDispatch::sFns.CmdCopyBuffer(p->c, p->src, p->dst, p->count, at<VkBufferCopy>(p, o, p->count));
            }, room<VkBufferCopy>(n));
            *p = { p->header, c, src, dst, n };
            put(p, sizeof(P), regions, n);
            regions += n;
            count -= n;
        } while (count > 0);
    }

    inline void CmdBlitImage(VkCommandBuffer c, VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout, uint32_t count,
                             const VkImageBlit* regions, VkFilter filter)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdBlitImage(c, src, src_layout, dst, dst_layout, count, regions, filter);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkImage src; VkImageLayout src_layout; VkImage dst; VkImageLayout dst_layout; uint32_t count; VkFilter filter; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdBlitImage(p->c, p->src, p->src_layout, p->dst, p->dst_layout, p->count, at<VkImageBlit>(p, o, p->count), p->filter);
        }, room<VkImageBlit>(count));
        *p = { p->header, c, src, src_layout, dst, dst_layout, count, filter };
        put(p, sizeof(P), regions, count);
    }

    inline void CmdCopyImage(VkCommandBuffer c, VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout, uint32_t count,
                             const VkImageCopy* regions)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdCopyImage(c, src, src_layout, dst, dst_layout, count, regions);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkImage src; VkImageLayout src_layout; VkImage dst; VkImageLayout dst_layout; uint32_t count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdCopyImage(p->c, p->src, p->src_layout, p->dst, p->dst_layout, p->count, at<VkImageCopy>(p, o, p->count));
        }, room<VkImageCopy>(count));
        *p = { p->header, c, src, src_layout, dst, dst_layout, count };
        put(p, sizeof(P), regions, count);
    }

    inline void CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count, const VkBufferImageCopy* regions)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdCopyBufferToImage(c, src, dst, layout, count, regions);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkBuffer src; VkImage dst; VkImageLayout layout; uint32_t count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdCopyBufferToImage(p->c, p->src, p->dst, p->layout, p->count, at<VkBufferImageCopy>(p, o, p->count));
        }, room<VkBufferImageCopy>(count));
        *p = { p->header, c, src, dst, layout, count };
        put(p, sizeof(P), regions, count);
    }

    inline void CmdCopyImageToBuffer(VkCommandBuffer c, VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t count, const VkBufferImageCopy* regions)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdCopyImageToBuffer(c, src, layout, dst, count, regions);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkImage src; VkImageLayout layout; VkBuffer dst; uint32_t count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdCopyImageToBuffer(p->c, p->src, p->layout, p->dst, p->count, at<VkBufferImageCopy>(p, o, p->count));
        }, room<VkBufferImageCopy>(count));
        *p = { p->header, c, src, layout, dst, count };
        put(p, sizeof(P), regions, count);
    }

    inline void CmdClearColorImage(VkCommandBuffer c, VkImage image, VkImageLayout layout, const VkClearColorValue* color, uint32_t count,
                                   const VkImageSubresourceRange* ranges)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdClearColorImage(c, image, layout, color, count, ranges);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkImage image; VkImageLayout layout; VkClearColorValue color; uint32_t count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdClearColorImage(p->c, p->image, p->layout, &p->color, p->count, at<VkImageSubresourceRange>(p, o, p->count));
        }, room<VkImageSubresourceRange>(count));
        *p = { p->header, c, image, layout, *color, count };
        put(p, sizeof(P), ranges, count);
    }

    inline void CmdClearDepthStencilImage(VkCommandBuffer c, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue* value,
                                          uint32_t count, const VkImageSubresourceRange* ranges)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdClearDepthStencilImage(c, image, layout, value, count, ranges);
            return;
        }
        struct P { Header header; VkCommandBuffer c; VkImage image; VkImageLayout layout; VkClearDepthStencilValue value; uint32_t count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            LLVKDispatch::sFns.CmdClearDepthStencilImage(p->c, p->image, p->layout, &p->value, p->count, at<VkImageSubresourceRange>(p, o, p->count));
        }, room<VkImageSubresourceRange>(count));
        *p = { p->header, c, image, layout, *value, count };
        put(p, sizeof(P), ranges, count);
    }

    inline void CmdClearAttachments(VkCommandBuffer c, uint32_t count, const VkClearAttachment* attachments, uint32_t rect_count,
                                    const VkClearRect* rects)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdClearAttachments(c, count, attachments, rect_count, rects);
            return;
        }
        struct P { Header header; VkCommandBuffer c; uint32_t count, rect_count; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            const VkClearAttachment* attachments = at<VkClearAttachment>(p, o, p->count);
            const VkClearRect* rects = at<VkClearRect>(p, o, p->rect_count);
            LLVKDispatch::sFns.CmdClearAttachments(p->c, p->count, attachments, p->rect_count, rects);
        }, room<VkClearAttachment>(count) + room<VkClearRect>(rect_count));
        *p = { p->header, c, count, rect_count };
        size_t o = put(p, sizeof(P), attachments, count);
        put(p, o, rects, rect_count);
    }

    // Copies the rendering info and its attachments (no pNext chains are used:
    // a chain is not copied, and recording it directly while deferred would
    // put it before the packets queued ahead of it).
    inline void CmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo* info)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdBeginRendering(c, info);
            return;
        }
        llassert(!info->pNext);
        struct P { Header header; VkCommandBuffer c; VkRenderingInfo info; bool depth, stencil; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            p->info.pColorAttachments = at<VkRenderingAttachmentInfo>(p, o, p->info.colorAttachmentCount);
            const VkRenderingAttachmentInfo* depth = at<VkRenderingAttachmentInfo>(p, o, p->depth ? 1 : 0);
            const VkRenderingAttachmentInfo* stencil = at<VkRenderingAttachmentInfo>(p, o, p->stencil ? 1 : 0);
            p->info.pDepthAttachment = p->depth ? depth : nullptr;
            p->info.pStencilAttachment = p->stencil ? stencil : nullptr;
            LLVKDispatch::sFns.CmdBeginRendering(p->c, &p->info);
        }, room<VkRenderingAttachmentInfo>(info->colorAttachmentCount + 2));
        *p = { p->header, c, *info, info->pDepthAttachment != nullptr, info->pStencilAttachment != nullptr };
        p->info.pNext = nullptr;    // not copied: never read after the caller's chain is gone
        size_t o = put(p, sizeof(P), info->pColorAttachments, info->colorAttachmentCount);
        o = put(p, o, info->pDepthAttachment, info->pDepthAttachment ? 1 : 0);
        put(p, o, info->pStencilAttachment, info->pStencilAttachment ? 1 : 0);
    }

    // Copies the dependency info and its barrier arrays (no pNext chain, as CmdBeginRendering).
    inline void CmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* info)
    {
        if (!deferred(c))
        {
            LLVKDispatch::sFns.CmdPipelineBarrier2(c, info);
            return;
        }
        llassert(!info->pNext);
        struct P { Header header; VkCommandBuffer c; VkDependencyInfo info; };
        P* p = packet<P>([](void* s)
        {
            P* p = (P*)s;
            size_t o = sizeof(P);
            p->info.pMemoryBarriers = at<VkMemoryBarrier2>(p, o, p->info.memoryBarrierCount);
            p->info.pBufferMemoryBarriers = at<VkBufferMemoryBarrier2>(p, o, p->info.bufferMemoryBarrierCount);
            p->info.pImageMemoryBarriers = at<VkImageMemoryBarrier2>(p, o, p->info.imageMemoryBarrierCount);
            LLVKDispatch::sFns.CmdPipelineBarrier2(p->c, &p->info);
        }, room<VkMemoryBarrier2>(info->memoryBarrierCount) + room<VkBufferMemoryBarrier2>(info->bufferMemoryBarrierCount) +
           room<VkImageMemoryBarrier2>(info->imageMemoryBarrierCount));
        *p = { p->header, c, *info };
        p->info.pNext = nullptr;    // not copied (CmdBeginRendering)
        size_t o = put(p, sizeof(P), info->pMemoryBarriers, info->memoryBarrierCount);
        o = put(p, o, info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount);
        put(p, o, info->pImageMemoryBarriers, info->imageMemoryBarrierCount);
    }
}

#define vkCmdBindPipeline (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBindPipeline], LLVKRecorder::CmdBindPipeline)
#define vkCmdBindDescriptorSets (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBindDescriptorSets], LLVKRecorder::CmdBindDescriptorSets)
#define vkCmdBindVertexBuffers (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBindVertexBuffers], LLVKRecorder::CmdBindVertexBuffers)
#define vkCmdBindVertexBuffers2 (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBindVertexBuffers2], LLVKRecorder::CmdBindVertexBuffers2)
#define vkCmdBindIndexBuffer (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBindIndexBuffer], LLVKRecorder::CmdBindIndexBuffer)
#define vkCmdDraw (++LLVKDispatch::counts()[LLVKDispatch::C_CmdDraw], LLVKRecorder::CmdDraw)
#define vkCmdDrawIndexed (++LLVKDispatch::counts()[LLVKDispatch::C_CmdDrawIndexed], LLVKRecorder::CmdDrawIndexed)
#define vkCmdPushConstants (++LLVKDispatch::counts()[LLVKDispatch::C_CmdPushConstants], LLVKRecorder::CmdPushConstants)
#define vkCmdSetViewport (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetViewport], LLVKRecorder::CmdSetViewport)
#define vkCmdSetScissor (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetScissor], LLVKRecorder::CmdSetScissor)
#define vkCmdSetCullMode (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetCullMode], LLVKRecorder::CmdSetCullMode)
#define vkCmdSetFrontFace (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetFrontFace], LLVKRecorder::CmdSetFrontFace)
#define vkCmdSetPrimitiveTopology (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetPrimitiveTopology], LLVKRecorder::CmdSetPrimitiveTopology)
#define vkCmdSetDepthTestEnable (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetDepthTestEnable], LLVKRecorder::CmdSetDepthTestEnable)
#define vkCmdSetDepthWriteEnable (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetDepthWriteEnable], LLVKRecorder::CmdSetDepthWriteEnable)
#define vkCmdSetDepthCompareOp (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetDepthCompareOp], LLVKRecorder::CmdSetDepthCompareOp)
#define vkCmdSetDepthBiasEnable (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetDepthBiasEnable], LLVKRecorder::CmdSetDepthBiasEnable)
#define vkCmdSetDepthBias (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetDepthBias], LLVKRecorder::CmdSetDepthBias)
#define vkCmdSetStencilTestEnable (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetStencilTestEnable], LLVKRecorder::CmdSetStencilTestEnable)
#define vkCmdSetStencilOp (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetStencilOp], LLVKRecorder::CmdSetStencilOp)
#define vkCmdSetStencilCompareMask (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetStencilCompareMask], LLVKRecorder::CmdSetStencilCompareMask)
#define vkCmdSetStencilWriteMask (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetStencilWriteMask], LLVKRecorder::CmdSetStencilWriteMask)
#define vkCmdSetStencilReference (++LLVKDispatch::counts()[LLVKDispatch::C_CmdSetStencilReference], LLVKRecorder::CmdSetStencilReference)
#define vkUpdateDescriptorSets (++LLVKDispatch::counts()[LLVKDispatch::C_UpdateDescriptorSets], LLVKDispatch::sFns.UpdateDescriptorSets)
#define vkAllocateDescriptorSets (++LLVKDispatch::counts()[LLVKDispatch::C_AllocateDescriptorSets], LLVKDispatch::sFns.AllocateDescriptorSets)
#define vkCmdBeginRendering (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBeginRendering], LLVKRecorder::CmdBeginRendering)
#define vkCmdEndRendering (++LLVKDispatch::counts()[LLVKDispatch::C_CmdEndRendering], LLVKRecorder::CmdEndRendering)
#define vkCmdPipelineBarrier2 (++LLVKDispatch::counts()[LLVKDispatch::C_CmdPipelineBarrier2], LLVKRecorder::CmdPipelineBarrier2)
#define vkCmdCopyBuffer (++LLVKDispatch::counts()[LLVKDispatch::C_CmdCopyBuffer], LLVKRecorder::CmdCopyBuffer)
#define vkCmdBlitImage (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBlitImage], LLVKRecorder::CmdBlitImage)
#define vkCmdCopyImage (++LLVKDispatch::counts()[LLVKDispatch::C_CmdCopyImage], LLVKRecorder::CmdCopyImage)
#define vkCmdCopyBufferToImage (++LLVKDispatch::counts()[LLVKDispatch::C_CmdCopyBufferToImage], LLVKRecorder::CmdCopyBufferToImage)
#define vkCmdClearColorImage (++LLVKDispatch::counts()[LLVKDispatch::C_CmdClearColorImage], LLVKRecorder::CmdClearColorImage)
#define vkCmdClearDepthStencilImage (++LLVKDispatch::counts()[LLVKDispatch::C_CmdClearDepthStencilImage], LLVKRecorder::CmdClearDepthStencilImage)
#define vkCmdClearAttachments (++LLVKDispatch::counts()[LLVKDispatch::C_CmdClearAttachments], LLVKRecorder::CmdClearAttachments)
#define vkCmdCopyImageToBuffer (++LLVKDispatch::counts()[LLVKDispatch::C_CmdCopyImageToBuffer], LLVKRecorder::CmdCopyImageToBuffer)
#define vkCmdDrawIndexedIndirect (++LLVKDispatch::counts()[LLVKDispatch::C_CmdDrawIndexedIndirect], LLVKRecorder::CmdDrawIndexedIndirect)
#define vkCmdDrawIndexedIndirectCount (++LLVKDispatch::counts()[LLVKDispatch::C_CmdDrawIndexedIndirectCount], LLVKRecorder::CmdDrawIndexedIndirectCount)
#define vkCmdDispatch (++LLVKDispatch::counts()[LLVKDispatch::C_CmdDispatch], LLVKRecorder::CmdDispatch)
#define vkCmdResetQueryPool (++LLVKDispatch::counts()[LLVKDispatch::C_CmdResetQueryPool], LLVKRecorder::CmdResetQueryPool)
#define vkCmdWriteTimestamp2 (++LLVKDispatch::counts()[LLVKDispatch::C_CmdWriteTimestamp2], LLVKRecorder::CmdWriteTimestamp2)
#define vkCmdBeginQuery (++LLVKDispatch::counts()[LLVKDispatch::C_CmdBeginQuery], LLVKRecorder::CmdBeginQuery)
#define vkCmdEndQuery (++LLVKDispatch::counts()[LLVKDispatch::C_CmdEndQuery], LLVKRecorder::CmdEndQuery)
#define vkCmdFillBuffer (++LLVKDispatch::counts()[LLVKDispatch::C_CmdFillBuffer], LLVKRecorder::CmdFillBuffer)
#define vkCmdExecuteCommands (++LLVKDispatch::counts()[LLVKDispatch::C_CmdExecuteCommands], LLVKRecorder::CmdExecuteCommands)

#endif // LL_LLVKDISPATCH_H
