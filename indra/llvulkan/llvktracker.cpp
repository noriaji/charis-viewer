/**
 * @file llvktracker.cpp
 * @brief LLVKTracker: what native renderers may ask the GL-on-Vulkan tracker (research/91).
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

#include "llvktracker.h"

#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"
#include "llvkdispatch.h"   // after the Vulkan headers: commands go to the frame's recorder in order

#include <sstream>

// ----------------------------------------------------------------------------
// LLVKTrackerMutex (research/156 T14)

LLVKTrackerMutex::LLVKTrackerMutex()
{
#if LL_WINDOWS
    const char* env = std::getenv("LLRHI_TRACKER_SRW");
    mSRW = env && *env == '1';
#endif
}

void LLVKTrackerMutex::noteForeignUnlock()
{
    static std::atomic<bool> noted{ false };
    if (!noted.exchange(true))
    {
        LL_WARNS("VKGL") << "tracker lock released on another thread than the one that took it (LLRHI_TRACKER_SRW=1)" << LL_ENDL;
    }
}

#if LL_GL_FUNC_POINTER

using namespace llvkgl;

// ----------------------------------------------------------------------------
// LLVKTracker: what native renderers may ask the tracker.

LLVKTrackerMutex& LLVKTracker::lock()
{
    return sLock;
}

U32 LLVKTracker::frame()
{
    return sFrame;
}

VkBuffer LLVKTracker::drawBuffer(GLName buffer)
{
    return gpu_buffer_named(buffer);    // counted there (research/293 branch 0)
}

const U8* LLVKTracker::bufferContents(GLName buffer, size_t& size)
{
    Buffer* b = buffer ? sBuffers.find(buffer) : nullptr;
    if (LLRHIVBCensus::sOn && b && b->owner_number)     // research/293 branch 0: counting only
    {
        LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_CONTENTS, b->owner_number);
    }
    if (!b || b->data.empty())
    {
        size = 0;
        return nullptr;
    }
    size = b->data.size();
    return b->data.data();
}

void LLVKTracker::setBufferChangedCallback(void (*callback)(GLName buffer))
{
    sBufferChanged = callback;
}

void LLVKTracker::addBufferChangedCallback(void (*callback)(GLName buffer))
{
    sBufferChangedMore = callback;
}

LLVKTracker::Sampled LLVKTracker::sampledTexture(GLName texture)
{
    Sampled out;
    Texture* tex = nullptr;
    if (texture)
    {
        tex = sTextures.find(texture);
    }
    VkImageView view = tex ? texture_view(*tex) : VK_NULL_HANDLE;
    if (!view)
    {
        tex = dummy_texture(VK_IMAGE_VIEW_TYPE_2D, false);
        view = tex ? texture_view(*tex) : VK_NULL_HANDLE;
    }
    if (view)
    {
        out.view = view;
        out.sampler = sampler_for(tex, false);
        out.serial = tex->view_serial;
    }
    return out;
}

LLVKTracker::PipelineState LLVKTracker::pipelineState()
{
    PipelineState out;
    out.depth_clamp = sState.depth_clamp;
    out.polygon_mode = sState.polygon_mode;
    return out;
}

namespace
{
    // A native pass binds its own pipelines, sets and buffers and pushes its
    // own constants: what the generic path last recorded no longer holds.
    void forget_recorded_bindings()
    {
        sRecorded.pipeline = VK_NULL_HANDLE;
        sRecorded.set = VK_NULL_HANDLE;
        sRecorded.table_only = false;
        sRecorded.vertex_buffers.clear();
        sRecorded.vertex_offsets.clear();
        sRecorded.index_buffer = VK_NULL_HANDLE;
        sRecorded.slot_count = 0;       // its push constants overwrite the slot numbers
    }
}

bool LLVKTracker::prepareSampled(const std::vector<GLName>& sampled)
{
    LLVKMirror& mirror = LLVKMirror::instance();
    if (!mirror.isRecording())
    {
        return false;
    }
    pass_invalidate();
    forget_recorded_bindings();
    for (GLName name : sampled)
    {
        Texture* tex = sTextures.find(name);
        if (tex && tex->image && tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            transition(mirror.transferCommands(), *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    return true;
}

VkCommandBuffer LLVKTracker::beginPass(const std::vector<GLName>& sampled, LLVKMirror::Target& target_out)
{
    LLVKMirror& mirror = LLVKMirror::instance();
    if (!mirror.isRecording())
    {
        return VK_NULL_HANDLE;
    }
    if (sRecorded.frame != sFrame)
    {
        sRecorded = Recorded();
        sRecorded.frame = sFrame;
    }
    pass_invalidate();      // the native pass binds and pushes its own
    ResolvedTarget target;
    if (!resolve_draw_target(target))
    {
        return VK_NULL_HANDLE;
    }
    static std::vector<Texture*> textures;
    textures.clear();
    for (GLName name : sampled)
    {
        Texture* tex = sTextures.find(name);
        if (!tex || !tex->image)
        {
            continue;
        }
        if (tex == target.depth)
        {
            // Sampled while attached: both read-only (the pass writes no depth).
            target.target.depth_read_only = true;
            continue;
        }
        if (is_attachment_of(target, tex))
        {
            return VK_NULL_HANDLE;  // a color attachment cannot be sampled at the same time
        }
        if (tex->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            textures.push_back(tex);
        }
    }
    const bool barriers = !textures.empty() || needs_attachment_layouts(target);
    if (barriers)
    {
        VkCommandBuffer cmd = mirror.transferCommands();
        LLVKMirror::BarrierBatch batch(cmd);    // research/111 A-BB
        for (Texture* tex : textures)
        {
            transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        to_attachment_layouts(cmd, target);
    }
    VkCommandBuffer cmd = mirror.beginRendering(target.target);
    set_dynamic_state(cmd, target.target, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    forget_recorded_bindings();
    target_out = target.target;
    return cmd;
}

VkCullModeFlags LLVKTracker::passCullMode()
{
    return sRecorded.cull;  // set_dynamic_state in beginPass
}

void LLVKTracker::endPass()
{
    // Its pipelines may have left dynamic state undefined (bound with it
    // static): the next generic draw sets all of it again.
    sRecorded.dynamic = false;
    sRecorded.pipeline = VK_NULL_HANDLE;
    pass_invalidate();
}

std::string LLVKTracker::describeDrawTarget()
{
    std::ostringstream text;
    const State& s = sState;
    text << "GL framebuffer " << s.draw_framebuffer;
    ResolvedTarget target;
    if (!resolve_draw_target(target))
    {
        text << " (does not resolve)";
    }
    else
    {
        text << (target.is_default ? " (the frame's)" : "") << ": " << target.target.width << "x" << target.target.height
             << ", colors " << target.target.color_count;
        for (U32 i = 0; i < target.target.color_count; ++i)
        {
            const Texture* tex = target.colors[i];
            text << " [" << i << ": view " << (void*)target.target.colors[i] << ", image " << (void*)(tex ? tex->image : VK_NULL_HANDLE)
                 << ", format " << (S32)target.target.color_formats[i] << "]";
        }
    }
    text << "; color mask " << s.color_mask[0] << s.color_mask[1] << s.color_mask[2] << s.color_mask[3] << ", scissor "
         << (s.scissor ? "on " : "off ") << s.scissor_rect[0] << "," << s.scissor_rect[1] << " " << s.scissor_rect[2] << "x"
         << s.scissor_rect[3] << ", clear colour " << s.clear_color[0] << "," << s.clear_color[1] << "," << s.clear_color[2] << ","
         << s.clear_color[3] << ", blend " << (s.blend ? "on" : "off");
    return text.str();
}

std::string LLVKTracker::describeTexture(GLName texture)
{
    std::ostringstream text;
    const Texture* tex = texture ? sTextures.find(texture) : nullptr;
    text << "GL texture " << texture;
    if (!tex)
    {
        text << " (unknown)";
        return text.str();
    }
    text << ": image " << (void*)tex->image << ", view " << (void*)tex->view << ", attach view " << (void*)tex->attach_view
         << ", format " << (S32)tex->format.format << ", " << tex->width << "x" << tex->height << ", levels " << tex->levels
         << " (defined 0x" << std::hex << tex->defined_levels << std::dec << "), layout " << (S32)tex->layout << ", attachable "
         << tex->attachable << ", attach later " << tex->attach_later << ", rendered to " << tex->rendered_to << ", version "
         << tex->version << ", min filter 0x" << std::hex << tex->min_filter << std::dec;
    return text.str();
}

bool LLVKTracker::copyTexels(GLName texture, const std::vector<std::pair<S32, S32>>& points, VkBuffer buffer, VkDeviceSize offset)
{
    LLVKMirror& mirror = LLVKMirror::instance();
    Texture* tex = texture ? sTextures.find(texture) : nullptr;
    if (!mirror.isRecording() || !tex || !tex->image || tex->format.texel_size != 4 || tex->format.compressed ||
        tex->format.depth || points.empty())
    {
        return false;
    }
    std::vector<VkBufferImageCopy> regions;
    for (const std::pair<S32, S32>& point : points)
    {
        VkBufferImageCopy region = {};
        region.bufferOffset = offset + regions.size() * 4;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageOffset = { llclamp(point.first, 0, (S32)tex->width - 1), llclamp(point.second, 0, (S32)tex->height - 1), 0 };
        region.imageExtent = { 1, 1, 1 };
        regions.push_back(region);
    }
    VkCommandBuffer cmd = mirror.transferCommands();
    transition(cmd, *tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, (U32)regions.size(), regions.data());
    transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

void LLVKTracker::setTextureDeletedCallback(void (*callback)(GLName texture))
{
    sTextureDeleted = callback;
}

void LLVKTracker::addTextureDeletedCallback(void (*callback)(GLName texture))
{
    sTextureDeletedMore = callback;
}

#endif // LL_GL_FUNC_POINTER
