/**
 * @file llvkgl_texture.cpp
 * @brief GL textures on Vulkan: units and pixel storage, formats, images and views, uploads, mips, BC7 storage, samplers, stand-ins (research/91).
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
#include "llvkqueues.h"

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // Texture::version values come from one counter, so a texture made after
    // another was deleted (even at the same address) never repeats its version.
    std::atomic<U32> sTextureVersions{ 0 };
namespace
{
    U32 next_texture_version() { return sTextureVersions.fetch_add(1, std::memory_order_relaxed) + 1; }

    // A new version for tex: what a sampler of it sees may change. Its slot
    // publication (research/180) no longer holds.
    void new_texture_version(Texture& tex)
    {
        tex.version = next_texture_version();
        unpublish(tex);
    }
} // namespace

    // ------------------------------------------------------------------
    // Textures

    NameTable<Texture> sTextures;
    thread_local GLuint sActiveUnit = 0;
    thread_local UnitBindings sUnits[32];
    // Bumped when a unit's binding changes, so caches of what a program samples
    // through some units are not thrown away by binds to the others.
    thread_local U32 sUnitBindings[32] = {};

    thread_local GLint sUnpackAlignment = 4;
    thread_local GLint sUnpackRowLength = 0;
    // glReadPixels' pack state (read back from Vulkan).
    thread_local GLint sPackAlignment = 4;
    thread_local GLint sPackRowLength = 0;
namespace
{

    TexFormat tex_format(GLenum internal)
    {
        TexFormat f;
        const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G,
                                 ONE = VK_COMPONENT_SWIZZLE_ONE, ZERO = VK_COMPONENT_SWIZZLE_ZERO;
        // The generic GL_COMPRESSED_* formats (LLImageGL with RenderCompressTextures,
        // on by default for small VRAM) leave the compression to the driver: they
        // are kept uncompressed here, like their plain formats.
        switch (internal)
        {
        case GL_RGBA8: case GL_RGBA: case GL_RGB8: case GL_RGB: case GL_COMPRESSED_RGBA: case GL_COMPRESSED_RGB:
            f.format = VK_FORMAT_R8G8B8A8_UNORM; break;
        case GL_SRGB8_ALPHA8: case GL_SRGB8: case GL_SRGB_ALPHA: case GL_SRGB: case GL_COMPRESSED_SRGB_ALPHA: case GL_COMPRESSED_SRGB:
            f.format = VK_FORMAT_R8G8B8A8_SRGB; break;
        case GL_R8: case GL_RED: case GL_COMPRESSED_RED:
            f.format = VK_FORMAT_R8_UNORM; f.texel_size = 1; break;
        case GL_RG8: case GL_RG: case GL_COMPRESSED_RG:
            f.format = VK_FORMAT_R8G8_UNORM; f.texel_size = 2; break;
        case GL_ALPHA8: case GL_ALPHA: case GL_COMPRESSED_ALPHA:
            f.format = VK_FORMAT_R8_UNORM; f.texel_size = 1; f.swizzle = { ZERO, ZERO, ZERO, R }; break;
        case GL_LUMINANCE8: case GL_LUMINANCE: case GL_COMPRESSED_LUMINANCE:
            f.format = VK_FORMAT_R8_UNORM; f.texel_size = 1; f.swizzle = { R, R, R, ONE }; break;
        case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE_ALPHA: case GL_COMPRESSED_LUMINANCE_ALPHA:
            f.format = VK_FORMAT_R8G8_UNORM; f.texel_size = 2; f.swizzle = { R, R, R, G }; break;
        case GL_RGBA16: case GL_RGB16:
            f.format = VK_FORMAT_R16G16B16A16_UNORM; f.texel_size = 8; break;
        case GL_RGBA16F: case GL_RGB16F:
            f.format = VK_FORMAT_R16G16B16A16_SFLOAT; f.texel_size = 8; break;
        case GL_RGBA32F: case GL_RGB32F:
            f.format = VK_FORMAT_R32G32B32A32_SFLOAT; f.texel_size = 16; break;
        case GL_R16F:
            f.format = VK_FORMAT_R16_SFLOAT; f.texel_size = 2; break;
        case GL_R32F:
            f.format = VK_FORMAT_R32_SFLOAT; f.texel_size = 4; break;
        case GL_RG16F:
            f.format = VK_FORMAT_R16G16_SFLOAT; f.texel_size = 4; break;
        case GL_R11F_G11F_B10F:
            f.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32; f.texel_size = 4; break;
        case GL_RGB10_A2:
            f.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32; f.texel_size = 4; break;
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            f.format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            f.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            f.format = VK_FORMAT_BC2_UNORM_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            f.format = VK_FORMAT_BC3_UNORM_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
            f.format = VK_FORMAT_BC1_RGB_SRGB_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
            f.format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
            f.format = VK_FORMAT_BC2_SRGB_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
            f.format = VK_FORMAT_BC3_SRGB_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_COMPRESSED_RED_RGTC1:   // research/288: the constellation pictures, as their file has them
            f.format = VK_FORMAT_BC4_UNORM_BLOCK; f.compressed = true; f.texel_size = 0; break;
        case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16:
            // Same 24-bit fixed-point depth as GL where possible: polygon offset
            // units (shadow bias) depend on the depth format.
            f.format = (format_features(VK_FORMAT_X8_D24_UNORM_PACK32) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                       ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT;
            f.depth = true; break;
        case GL_DEPTH_COMPONENT32F: case GL_DEPTH_COMPONENT32:
            f.format = VK_FORMAT_D32_SFLOAT; f.depth = true; break;
        case GL_DEPTH24_STENCIL8: case GL_DEPTH_STENCIL:
            f.format = (format_features(VK_FORMAT_D24_UNORM_S8_UINT) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                       ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
            f.depth = true; f.stencil = true; f.texel_size = 8; break;
        case GL_DEPTH32F_STENCIL8:
            f.format = VK_FORMAT_D32_SFLOAT_S8_UINT; f.depth = true; f.stencil = true; f.texel_size = 8; break;
        default:
            break;
        }
        return f;
    }

    VkImageAspectFlags full_aspect(const Texture& tex)
    {
        if (tex.format.depth)
        {
            return VK_IMAGE_ASPECT_DEPTH_BIT | (tex.format.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        }
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

} // namespace
    // Bytes of one 4x4 block of the block-compressed formats tex_format makes (0: not one).
    U32 compressed_block_bytes(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return 8;
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
            return 16;
        default:
            return 0;
        }
    }

namespace
{
    void bcn_note_write(VkImage image, bool wait);  // research/200 (below, with the BC7 queue)
} // namespace

    // Record a layout change for the whole image (no-op when already there).
    void transition(VkCommandBuffer cmd, Texture& tex, VkImageLayout to)
    {
        if (to != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            unpublish(tex);     // research/180 I4: published slots are of readable textures only
            tex.write_frame = sFrame;   // research/285 X2: written (or read back) lately
        }
        if (tex.layout == to || !tex.image)
        {
            return;
        }
        if (tex.bcn_shared && to != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            bcn_note_write(tex.image, true);    // research/200 H3: the compute queue may be reading it for BC7
        }
        if (cmd != LLVKMirror::instance().uploadCommands())
        {
            tex.late_write_frame = sFrame;      // its layout now changes in the main commands
            // Draws recorded with the old layout cannot be repeated as they are.
            // (The upload commands run before the frame and end where they began:
            // early uploads go from and back to shader reading.)
            ++sLayoutState;
        }
        LLVKMirror::imageBarrier(cmd, tex.image, full_aspect(tex), tex.layout, to, tex.levels, tex.layers);
        tex.layout = to;
    }

    // LLRHI_VK_CHECKPOINTS diagnostics: views queued for destruction, and the last
    // frames' generic draws, so a GPU fault can be traced to the textures it read.
    std::unordered_map<VkImageView, U32> sDestroyedViews;     // view -> frame it was released in
    std::vector<DrawRecord> sDrawLog;
namespace
{

    void note_destroyed_view(VkImageView view)
    {
        if (view && LLVKDevice::instance().checkpointsEnabled())
        {
            // Kept as long as the draw log keeps its draws (record_draw()): older ones
            // cannot be in a report, and a released handle may be reused anyway.
            static U32 sPurgedFrame = 0;
            if (sPurgedFrame != sFrame)
            {
                sPurgedFrame = sFrame;
                for (auto it = sDestroyedViews.begin(); it != sDestroyedViews.end();)
                {
                    it = it->second + 3 < sFrame ? sDestroyedViews.erase(it) : std::next(it);
                }
            }
            sDestroyedViews[view] = sFrame;
        }
    }

    // The texture's view is going: what the bindless sets hold of it must not
    // be trusted by handle any more. texture-leak (2026-10-27): the slot the
    // table really gives its GL name, never bindless_slot (it can be a slot
    // the texture left, after a reclaim or slotFor's moves, which do not write
    // it back: forget() there would drop another texture's entry). A texture
    // without a name (the stand-ins) never holds a slot by name.
    void forget_bindless(const Texture& tex)
    {
        if (tex.gl_name)
        {
            LLVKBindless::forgetTexture(tex.gl_name);
        }
    }

} // namespace
    void destroy_texture_objects(Texture& tex)
    {
        if (!tex.image)
        {
            return;
        }
        ++sTextureState;
        new_texture_version(tex);
        ++sTargetState;
        ++sLayoutState;
        VkImage image = tex.image;
        VmaAllocation memory = tex.memory;
        VkImageView view = tex.view;
        VkImageView attach_view = tex.attach_view;
        note_destroyed_view(view);
        forget_bindless(tex);
        LLVKDevice::instance().forgetImage(memory);    // research/78: texture bytes for the budget
        if (tex.bcn_encoded)
        {
            LLVKTexCompress::noteReleased(tex.bcn_plain_bytes, tex.bcn_bytes);
            tex.bcn_encoded = false;
        }
        tex.bcn_queued = false;     // a queued entry names this image: it is skipped
        if (tex.bcn_shared)
        {
            // research/200: an encoding of it on the compute queue is thrown away
            // (no wait: the image goes after the frame's fence, research/200 H4).
            bcn_note_write(image, false);
        }
        tex.bcn_shared = false;     // of this image (the next one decides again)

        // research/109: released now, freed when the GPU is done (the fault report's history).
        char what[56];
        snprintf(what, sizeof(what), "texture %ux%ux%u format %d, %u levels%s%s", tex.width, tex.height, tex.layers,
                 (S32)tex.format.format, tex.levels, tex.rendered_to ? ", rendered to" : "", tex.attach_view ? ", attachable" : "");
        LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_RELEASED, (U64)image, memory, what);

        const U32 width = tex.width, height = tex.height;
        LLVKMirror::instance().defer([image, memory, view, attach_view, width, height]()
        {
            char freed[56];
            snprintf(freed, sizeof(freed), "texture %ux%u", width, height);
            LLVKDevice::instance().noteResource(LLVKDevice::RESOURCE_FREED, (U64)image, memory, freed);
            VkDevice device = LLVKDevice::instance().getDevice();
            if (view)
            {
                vkDestroyImageView(device, view, nullptr);
            }
            if (attach_view)
            {
                vkDestroyImageView(device, attach_view, nullptr);
            }
            vmaDestroyImage(LLVKDevice::instance().getAllocator(), image, memory);
        });
        tex.image = VK_NULL_HANDLE;
        tex.memory = nullptr;
        tex.view = VK_NULL_HANDLE;
        tex.attach_view = VK_NULL_HANDLE;
        tex.view_levels = 0;
        tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        tex.defined_levels = 0;
        tex.written_levels = 0;
    }
namespace
{

    U32 full_mip_count(U32 width, U32 height)
    {
        U32 levels = 1;
        U32 size = llmax(width, height);
        while (size > 1)
        {
            size >>= 1;
            ++levels;
        }
        return levels;
    }

} // namespace
    bool is_cube_face(GLenum target)
    {
        return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
    }
namespace
{

    // The image tex's format, size, levels and layers call for. attach: with
    // attachment usage where the format allows it; attachable tells whether an
    // attachment view can be made (one layer).
    VkImageCreateInfo image_info(const Texture& tex, bool attach, bool& attachable, bool& could_attach)
    {
        const TexFormat& format = tex.format;
        VkFormatFeatureFlags features = format_features(format.format);
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format.format;
        info.extent = { tex.width, tex.height, 1 };
        info.mipLevels = tex.levels;
        info.arrayLayers = tex.layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        // GL decides later whether a texture is a render target; allow it wherever the format can.
        attachable = false;
        could_attach = false;
        VkImageUsageFlags attachment = 0;
        if (format.depth && (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            attachment = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        else if (!format.depth && !format.compressed && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        {
            attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (attachment)
        {
            could_attach = tex.layers == 1;
            if (attach)
            {
                info.usage |= attachment;
                attachable = tex.layers == 1;
            }
        }
        if (tex.layers % 6 == 0 && (tex.target == GL_TEXTURE_CUBE_MAP || tex.target == GL_TEXTURE_CUBE_MAP_ARRAY))
        {
            info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        return info;
    }

    VkImageView create_attach_view(const Texture& tex)
    {
        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = tex.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = tex.format.format;
        view.subresourceRange = { full_aspect(tex), 0, 1, 0, 1 };
        VkImageView out = VK_NULL_HANDLE;
        if (vkCreateImageView(LLVKDevice::instance().getDevice(), &view, nullptr, &out) != VK_SUCCESS)
        {
            // Memory full (fix-round95 C): not attachable, as the callers of
            // attach_view take a null one; the output is not defined on failure.
            LL_WARNS_ONCE("VKGL") << "Could not create a texture's attachment view" << LL_ENDL;
            out = VK_NULL_HANDLE;
        }
        return out;
    }

    // A new image of tex in the shape bcn_candidate asks for, with no
    // attachment usage.
    bool bcn_shape(const Texture& tex, const VkImageCreateInfo& info)
    {
        return LLRHI::sTexBCN && tex.bcn_wanted && tex.target == GL_TEXTURE_2D && tex.layers == 1 &&
               tex.format.format == VK_FORMAT_R8G8B8A8_UNORM && (U64)tex.width * tex.height >= LLVKTexCompress::minTexels() &&
               !(info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    }

    // research/200: such an image that the BC7 encoder may read on the compute
    // queue (the compute queue in use for it, LLVKTexCompress::asyncWanted).
    bool bcn_shareable(const Texture& tex, const VkImageCreateInfo& info)
    {
        return bcn_shape(tex, info) && LLVKTexCompress::asyncWanted();
    }

    // research/285 X3: such an image that the encoder will likely store as BC7
    // in an image of its own a few frames on (research/88): its plain image is
    // interim (no memory of its own for it). Not when the encoder is off or the
    // texture is past what this GPU ever encodes (LLVKTexCompress::mayEncode).
    bool bcn_interim(const Texture& tex, const VkImageCreateInfo& info)
    {
        return bcn_shape(tex, info) && LLVKTexCompress::mayEncode(tex.width, tex.height, tex.levels);
    }

    // (Re)create the image when level 0 is specified with a new size/format.
    // attach false (research/70, LLRHI::sTexLazyAttach): a one-layer color
    // image is made without attachment usage unless it was ever attached; the
    // driver then need not give it memory of its own (NVIDIA prefers a
    // dedicated allocation for attachable images of 1024x1024 and up, one
    // kernel allocation each). make_attachable remakes it if it is attached.
    bool ensure_image(Texture& tex, GLenum internal, U32 width, U32 height, U32 layers, bool attach = true)
    {
        // (A texture stored BC7, research/88, gets a plain image again.)
        if (tex.image && tex.gl_internal == internal && tex.width == width && tex.height == height && tex.layers == layers &&
            !tex.bcn_encoded)
        {
            return true;
        }
        {
            // Security audit B 5: past the device's limits vkCreateImage is
            // invalid. GL_INVALID_VALUE, as glTexImage2D: the texture stays as it was.
            const VkPhysicalDeviceLimits& limits = LLVKDevice::instance().getLimits();
            const bool cube = tex.target == GL_TEXTURE_CUBE_MAP || tex.target == GL_TEXTURE_CUBE_MAP_ARRAY;
            const U32 max_side = cube ? llmin(limits.maxImageDimension2D, limits.maxImageDimensionCube) : limits.maxImageDimension2D;
            if (!width || !height || !layers)
            {
                // Review retro-vulkan 4: a size of 0 (a render target sized
                // while minimized, ...) is not past any limit; counted
                // apart (VKPerf's skips, no string made), as no image.
                skip("texture of size 0 (no image made)");
                raise_gl_error(GL_INVALID_VALUE);
                return false;
            }
            if (width > max_side || height > max_side || layers > limits.maxImageArrayLayers)
            {
                unhandled("texture size past the device's limits");
                raise_gl_error(GL_INVALID_VALUE);
                return false;
            }
        }
        {
            // research/288: a block-compressed format the GPU cannot sample (no
            // textureCompressionBC: it is enabled only where there): GL_INVALID_ENUM,
            // as GL refuses a format it does not have; the texture stays as it was.
            const TexFormat wanted = tex_format(internal);
            if (wanted.compressed && !(format_features(wanted.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            {
                unhandled("block-compressed texture format the GPU cannot sample " + hex(internal));
                raise_gl_error(GL_INVALID_ENUM);
                return false;
            }
        }
        destroy_texture_objects(tex);
        ++sTextureState;
        new_texture_version(tex);
        ++sTargetState;

        TexFormat format = tex_format(internal);
        if (format.format == VK_FORMAT_UNDEFINED)
        {
            unhandled("texture internal format " + hex(internal));
            return false;
        }
        tex.format = format;
        tex.gl_internal = internal;
        tex.width = width;
        tex.height = height;
        tex.layers = layers;
        tex.levels = format.depth ? 1 : full_mip_count(width, height);

        // Only one-layer color images wait: depth and layered images (probes)
        // keep their attachment usage as before.
        const bool lazy = !attach && !tex.attach_wanted && layers == 1 && !format.depth;
        bool could_attach = false;
        VkImageCreateInfo info = image_info(tex, !lazy, tex.attachable, could_attach);
        tex.attach_later = lazy && could_attach;
        // research/200: shared with the compute queue when the encoder may read it there.
        const bool shared = bcn_shareable(tex, info) && LLVKQueues::instance().shareWithCompute(info);
        // In the texture pool of its size unless the driver wants it alone (research/77).
        const LLVKDevice::ImageMemory where = bcn_interim(tex, info) ? LLVKDevice::ImageMemory::Interim : LLVKDevice::ImageMemory::Any;
        if (LLVKDevice::instance().createImage(info, tex.image, tex.memory, where) != VK_SUCCESS)
        {
            unhandled("vmaCreateImage failure");
            // research/289 (materialize integration check L10): counted only; a texture given its pixels (lazy:
            // made without attachment usage) apart from the others (render targets, the 1 x 1 stand-ins, ...)
            (lazy ? LLRHI::sTextureImageFailures : LLRHI::sTargetImageFailures).fetch_add(1, std::memory_order_relaxed);
            tex.image = VK_NULL_HANDLE;
            raise_gl_error(GL_OUT_OF_MEMORY);   // as glTexImage2D would: LLRenderTarget::allocate fails
            return false;
        }
        tex.bcn_shared = shared;
        ++sTextureData.new_images;  // research/137 N14
        if (sNameObjects)
        {
            std::ostringstream name;
            name << "texture image " << width << "x" << height << "x" << layers << ", target 0x" << std::hex << tex.target
                 << ", internal 0x" << internal << std::dec << ", " << info.mipLevels << " levels" << (tex.attachable ? ", attachable" : "");
            name_object((U64)tex.image, name.str());
        }
        tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        ++sLayoutState;

        if (tex.attachable)
        {
            tex.attach_view = create_attach_view(tex);
        }

        // Valid to sample (and to transition from) before any data arrives.
        LLVKMirror::instance().record([&](VkCommandBuffer cmd)
        {
            transition(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        return true;
    }

} // namespace
    // research/88: tex, stored BC7, is about to be written (a sub-image, a
    // copy, an attachment): it gets a plain image of its GL format again, its
    // BC7 texels decoded into every level in frame order, as though it had
    // never been compressed. The write then goes into the plain image.
    void restore_plain(Texture& tex)
    {
        if (!tex.bcn_encoded || !tex.image)
        {
            return;
        }
        const TexFormat encoded_format = tex.format;
        // The encoded image has only the levels that were sampled then; the plain
        // one gets every level again, as ensure_image made it, so a level written
        // later (glTexImage2D/glTexSubImage2D below them) is not dropped.
        const U32 encoded_levels = tex.levels;
        const U32 plain_levels = full_mip_count(tex.width, tex.height);
        tex.format = tex_format(tex.gl_internal);
        tex.levels = plain_levels;
        bool attachable = false, could_attach = false;
        const VkImageCreateInfo info = image_info(tex, tex.attach_wanted, attachable, could_attach);
        tex.levels = encoded_levels;    // still the encoded image's until it is replaced below
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        if (LLVKDevice::instance().createImage(info, image, memory) != VK_SUCCESS)
        {
            tex.format = encoded_format;
            unhandled("vmaCreateImage failure (block-compressed texture made plain)");
            return;
        }
        LLVKMirror::instance().record([&](VkCommandBuffer cmd)
        {
            transition(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            LLVKTexCompress::decode(cmd, tex.image, image, tex.width, tex.height, encoded_levels);
            if (plain_levels > encoded_levels)
            {
                // The levels below the decoded ones (undefined until uploaded) in the
                // layout the texture reports, recorded at once (not batched: the
                // decoded levels are left alone).
                VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, encoded_levels, plain_levels - encoded_levels, 0, tex.layers };
                VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            }
        });
        // The encoded image goes once the GPU is done with it (and stops counting as compressed).
        const U32 defined_levels = tex.defined_levels;
        const U32 written_levels = tex.written_levels;
        destroy_texture_objects(tex);
        tex.image = image;
        tex.memory = memory;
        tex.levels = plain_levels;
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex.defined_levels = defined_levels;
        tex.written_levels = written_levels;
        tex.attachable = attachable;
        tex.attach_later = !attachable && could_attach;
        if (attachable)
        {
            tex.attach_view = create_attach_view(tex);
        }
        // Writes later this frame must follow the decoding.
        tex.late_write_frame = sFrame;
        ++sLayoutState;
    }
namespace
{

    U32 sAttachRemakes = 0;     // make_attachable copies (research/70)

    // The regions of a copy of tex's levels in `levels` (a mask) into an image
    // of its format whose level 0 is tex's level first_level (research/285
    // X2), one a level, at most 32 (defined_levels is a 32-bit mask).
    U32 level_regions(const Texture& tex, U32 first_level, U32 levels, VkImageCopy (&regions)[32])
    {
        const VkImageAspectFlags aspect = full_aspect(tex);
        const U32 last = llmin(tex.levels, 32u);
        U32 count = 0;
        for (U32 level = first_level; level < last; ++level)
        {
            if (!(levels & (1u << level)))
            {
                continue;
            }
            VkImageCopy& region = regions[count++];
            region = {};
            region.srcSubresource = { aspect, level, 0, tex.layers };
            region.dstSubresource = { aspect, level - first_level, 0, tex.layers };
            region.extent = { llmax(1u, tex.width >> level), llmax(1u, tex.height >> level), 1 };
        }
        return count;
    }

} // namespace
    // research/70: tex is being attached to a framebuffer but its image was
    // made without attachment usage. Make it again with that usage and copy
    // every level and layer over in the frame's commands, in order with what
    // was recorded before (draws reading the old image keep it until the GPU
    // is done with them): the same texels, now attachable.
    void make_attachable(Texture& tex)
    {
        unpublish(tex);     // research/180 I5: never published again (attach_wanted)
        tex.attach_wanted = true;
        // texture-leak (2026-10-27): a render target's level 0 counts as
        // defined from its attachment on (it is drawn into), as before the
        // "contents only once written" rule (tex_image).
        if (tex.image && !(tex.defined_levels & 1))
        {
            tex.defined_levels |= 1;
            ++sTexDefined.attached;
        }
        restore_plain(tex);     // research/88: BC7 cannot be rendered into (made attachable here)
        if (!tex.attach_later || !tex.image)
        {
            return;
        }
        tex.attach_later = false;
        bool attachable = false, could_attach = false;
        VkImageCreateInfo info = image_info(tex, true, attachable, could_attach);
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        if (!attachable || LLVKDevice::instance().createImage(info, image, memory) != VK_SUCCESS)
        {
            unhandled("vmaCreateImage failure (attachable remake)");
            return;
        }
        if (++sAttachRemakes <= 20)
        {
            LL_INFOS("Vulkan") << "Texture made attachable when attached (" << tex.width << "x" << tex.height
                               << ", remake " << sAttachRemakes << ", research/70)" << LL_ENDL;
        }
        if (sNameObjects)
        {
            std::ostringstream name;
            name << "texture image " << tex.width << "x" << tex.height << ", internal 0x" << std::hex << tex.gl_internal << std::dec
                 << ", " << tex.levels << " levels, attachable (remade)";
            name_object((U64)image, name.str());
        }
        const VkImageAspectFlags aspect = full_aspect(tex);
        LLVKMirror::instance().record([&](VkCommandBuffer cmd)
        {
            transition(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            LLVKMirror::imageBarrier(cmd, image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, tex.levels, tex.layers);
            // Every level, one region each.
            VkImageCopy regions[32];
            const U32 level_count = level_regions(tex, 0, ~0u, regions);
            vkCmdCopyImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           level_count, regions);
            LLVKMirror::imageBarrier(cmd, image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     tex.levels, tex.layers);
        });

        // The old image and its views go once the GPU is done with them; the
        // texture now names the new one (a new version: nothing cached of the
        // old one is used again).
        const U32 defined_levels = tex.defined_levels;
        const U32 written_levels = tex.written_levels;
        destroy_texture_objects(tex);
        tex.image = image;
        tex.memory = memory;
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex.defined_levels = defined_levels;
        tex.written_levels = written_levels;
        tex.attachable = true;
        tex.attach_view = create_attach_view(tex);
        // Uploads later this frame must follow the copy (not run before it with the frame's uploads).
        tex.late_write_frame = sFrame;
        ++sLayoutState;
    }

    // View over the contiguous run of defined mip levels (GL completeness).
    VkImageView texture_view(Texture& tex)
    {
        if (!tex.image || !(tex.defined_levels & 1))
        {
            return VK_NULL_HANDLE;
        }
        // GL completeness only looks at base..max level.
        U32 last = llmin(tex.levels, tex.max_level + 1);
        U32 levels = 0;
        while (levels < last && (tex.defined_levels & (1u << levels)))
        {
            ++levels;
        }
        bool wants_mips = tex.min_filter != GL_NEAREST && tex.min_filter != GL_LINEAR;
        if (!wants_mips)
        {
            levels = 1;
        }
        if (tex.base_level != 0)
        {
            unhandled("GL_TEXTURE_BASE_LEVEL other than 0");
        }
        U32 swizzle_key = (tex.swizzle[0] & 0xff) | ((tex.swizzle[1] & 0xff) << 8) | ((tex.swizzle[2] & 0xff) << 16) | ((tex.swizzle[3] & 0xff) << 24);
        if (tex.view && tex.view_levels == levels && tex.view_swizzle == swizzle_key)
        {
            return tex.view;
        }
        if (tex.view)
        {
            unpublish(tex);     // research/180 I2 (a version change came first; this is the backstop)
            VkImageView old = tex.view;
            note_destroyed_view(old);
            forget_bindless(tex);
            LLVKMirror::instance().defer([old]() { vkDestroyImageView(LLVKDevice::instance().getDevice(), old, nullptr); });
        }
        VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = tex.image;
        info.viewType = tex.target == GL_TEXTURE_CUBE_MAP ? VK_IMAGE_VIEW_TYPE_CUBE :
                        tex.target == GL_TEXTURE_CUBE_MAP_ARRAY ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
                        tex.target == GL_TEXTURE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        info.format = tex.format.format;
        info.components = tex.format.swizzle;
        if (!tex.format.depth)
        {
            VkComponentSwizzle* out[4] = { &info.components.r, &info.components.g, &info.components.b, &info.components.a };
            for (int c = 0; c < 4; ++c)
            {
                switch (tex.swizzle[c])
                {
                case GL_RED: if (c != 0) *out[c] = VK_COMPONENT_SWIZZLE_R; break;
                case GL_GREEN: if (c != 1) *out[c] = VK_COMPONENT_SWIZZLE_G; break;
                case GL_BLUE: if (c != 2) *out[c] = VK_COMPONENT_SWIZZLE_B; break;
                case GL_ALPHA: if (c != 3) *out[c] = VK_COMPONENT_SWIZZLE_A; break;
                case GL_ZERO: *out[c] = VK_COMPONENT_SWIZZLE_ZERO; break;
                case GL_ONE: *out[c] = VK_COMPONENT_SWIZZLE_ONE; break;
                default: break;
                }
            }
        }
        // Depth textures are sampled through their depth aspect only.
        info.subresourceRange = { tex.format.depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                                  0, levels, 0, tex.layers };
        if (vkCreateImageView(LLVKDevice::instance().getDevice(), &info, nullptr, &tex.view) != VK_SUCCESS)
        {
            // Memory full (fix-round95 C): no view, as for an incomplete
            // texture above; the next use tries again.
            LL_WARNS_ONCE("VKGL") << "Could not create a texture's view" << LL_ENDL;
            tex.view = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        tex.view_serial = next_object_serial();     // research/156 A-DS
        tex.view_levels = levels;
        tex.view_swizzle = swizzle_key;
        if (tex_defined_check())
        {
            // texture-leak (2026-10-27): every level the view shows was written
            // (render targets are drawn into: not checked).
            const U32 shown = levels >= 32 ? ~0u : (1u << levels) - 1;
            const U32 unwritten = tex.defined_levels & shown & ~tex.written_levels;
            if (unwritten && !tex.attach_wanted && !tex.rendered_to)
            {
                if (++sTexDefined.unwritten <= 20)
                {
                    LL_WARNS("Vulkan") << "texture defined check: texture " << tex.gl_name << " (" << tex.width << "x" << tex.height
                                       << ", internal 0x" << std::hex << tex.gl_internal << ") shows levels 0x" << unwritten
                                       << " never written (defined 0x" << tex.defined_levels << ", written 0x" << tex.written_levels
                                       << std::dec << ")" << LL_ENDL;
                }
            }
        }
        return tex.view;
    }
namespace
{

    // Upload a region of one level/layer through a staging buffer, in frame order.
    // Texture uploads and mip generation for plain textures (not render targets,
    // readable or new) go to the frame's upload commands, which run before any
    // draw of the frame: they no longer split the rendering scope draws are in.
    // A draw earlier in the frame may then see the newer texels, which is fine.
    VkCommandBuffer early_texture_commands(const Texture& tex)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording() || tex.rendered_to || tex.late_write_frame == sFrame ||
            (tex.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && tex.layout != VK_IMAGE_LAYOUT_UNDEFINED))
        {
            return VK_NULL_HANDLE;
        }
        return mirror.uploadCommands();
    }

    // Texture staging taken from the frame's upload ring this frame (kept well
    // below the ring size so draws never run out).
    constexpr VkDeviceSize TEXTURE_STAGING_BUDGET = 64 * 1024 * 1024;
    constexpr VkDeviceSize TEXTURE_STAGING_MAX = 8 * 1024 * 1024;
    VkDeviceSize sTextureStaging = 0;
    U32 sTextureStagingFrame = U32_MAX;
} // namespace
    U32 sTextureUploads = 0, sTextureUploadsEarly = 0;
    U32 sInvisibleDraws = 0;
namespace
{

    // write(dst, frame) fills the size bytes of staging memory at dst. frame:
    // the copy from it goes into the frame's commands (submitted at the end of
    // the frame or by submitAndWait), so the bytes may still be written later,
    // before that; otherwise the copy is submitted before upload_with returns.
    // False: nothing recorded (no staging memory). clear_first (texture-leak
    // 2026-10-27): the level's layer is cleared to zero before the copy, for a
    // first write that does not cover a level without contents. layer_count
    // (research/288): that many layers from layer, one after another in the
    // bytes, in one copy (a 2D array's level at once).
    template <typename Write>
    bool upload_with(Texture& tex, U32 level, U32 layer, S32 x, S32 y, U32 width, U32 height, size_t size, const Write& write,
                     bool clear_first = false, U32 layer_count = 1)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (sTextureStagingFrame != sFrame)
        {
            sTextureStagingFrame = sFrame;
            sTextureStaging = 0;
        }
        ++sTextureUploads;
        if (level == 0)
        {
            note_level0_write(tex);     // research/88: a queued BC7 entry is stale
        }
        VkCommandBuffer early = early_texture_commands(tex);
        auto record_copy = [&](VkCommandBuffer cmd, VkBuffer staging, VkDeviceSize staging_offset)
        {
            transition(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (clear_first)
            {
                const VkClearColorValue zero = {};
                const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, layer_count };
                vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
                // The copy below writes over part of what the clear wrote.
                VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            }
            VkBufferImageCopy region = {};
            region.bufferOffset = staging_offset;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, layer_count };
            region.imageOffset = { x, y, 0 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        };

        // Small uploads stage through the frame ring: no allocation per upload.
        if (mirror.isRecording() && size <= TEXTURE_STAGING_MAX && sTextureStaging + size <= TEXTURE_STAGING_BUDGET)
        {
            VkBuffer ring = VK_NULL_HANDLE;
            VkDeviceSize ring_offset = 0;
            void* dst = nullptr;
            if (mirror.allocate(size, 16, ring, ring_offset, dst))
            {
                sTextureStaging += size;
                write((U8*)dst, true);
                if (early)
                {
                    ++sTextureUploadsEarly;
                    record_copy(early, ring, ring_offset);
                }
                else
                {
                    tex.late_write_frame = sFrame;
                    mirror.record([&](VkCommandBuffer cmd) { record_copy(cmd, ring, ring_offset); });
                }
                return true;
            }
        }

        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo host = {};
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation staging_memory = nullptr;
        VmaAllocationInfo mapped = {};
        if (vmaCreateBuffer(allocator, &info, &host, &staging, &staging_memory, &mapped) != VK_SUCCESS)
        {
            return false;
        }
        write((U8*)mapped.pMappedData, mirror.isRecording());     // record() submits at once outside a frame

        if (early)
        {
            ++sTextureUploadsEarly;
            record_copy(early, staging, 0);
        }
        else
        {
            tex.late_write_frame = sFrame;
            mirror.record([&](VkCommandBuffer cmd) { record_copy(cmd, staging, 0); });
        }
        mirror.defer([staging, staging_memory]()
        {
            vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), staging, staging_memory);
        });
        return true;
    }

} // namespace
    bool upload(Texture& tex, U32 level, U32 layer, S32 x, S32 y, U32 width, U32 height, const void* data, size_t size, bool clear_first)
    {
        return upload_with(tex, level, layer, x, y, width, height, size, [data, size](U8* dst, bool) { memcpy(dst, data, size); },
                           clear_first);
    }

    Texture* bound_texture(GLenum target)
    {
        GLenum bind_target = is_cube_face(target) ? GL_TEXTURE_CUBE_MAP : target;
        Texture* tex = unit_texture(sActiveUnit, bind_target);
        if (!tex)
        {
            // A name deleted while bound stays on the unit (glDeleteTextures). Given out again, the
            // viewer believes it bound (LLTexUnit skips binding the name it bound last) and the
            // glBindTexture that would make it never comes: an image, its storage or a parameter for
            // it makes it here, as bind_texture would have, instead of being lost.
            UnitBindings& u = sUnits[sActiveUnit];
            const int slot = target_slot(bind_target);
            const GLuint name = u.names[slot];
            if (name && texture_name_live(name))
            {
                Texture& t = sTextures.get(name);
                if (!t.target) t.target = bind_target;
                u.textures[slot] = &t;
                u.resolved_at[slot] = sTextureDeletes;
                ++sUnitBindings[sActiveUnit];
                tex = &t;
            }
        }
        return tex;
    }

    // The texture's image, contents or parameters are about to change (tex may
    // be null: nothing bound, only the global state changes).
    void touch_texture(Texture* tex)
    {
        ++sTextureState;
        if (tex)
        {
            new_texture_version(*tex);
        }
    }

    // The bound texture's image, contents or parameters are about to change.
    void touch_bound(GLenum target)
    {
        touch_texture(bound_texture(target));
    }
namespace
{

    // False: nothing recorded (a format not handled, no staging memory).
    bool upload_converted(Texture& tex, U32 level, U32 layer, S32 x, S32 y, U32 width, U32 height,
                          GLenum format, GLenum type, const void* pixels, bool clear_first = false)
    {
        ConvertJob job;
        if (!plan_convert(tex, format, type, pixels, width, height, job))
        {
            unhandled("pixel upload format " + hex(format) + " type " + hex(type) + " into " + hex(tex.gl_internal));
            return false;
        }
        const size_t size = job.size();
        LLRHI::sTexConvertBytes.fetch_add(size, std::memory_order_relaxed);
        if (!LLRHI::sTexDirect)
        {
            // As before research/70: a buffer of its own, then a copy into staging.
            std::vector<U8> converted(size);
            job.dst = converted.data();
            run_convert(job, 0, height);
            return upload(tex, level, layer, x, y, width, height, converted.data(), converted.size(), clear_first);
        }
        // Straight into the staging memory, now or (LLRHI::tTexConvertDefer,
        // research/70) in the caller's join before the frame is submitted.
        const bool defer = LLRHI::tTexConvertDefer && LLRHI::sTexConvertJoin;
        return upload_with(tex, level, layer, x, y, width, height, size, [&job, defer](U8* dst, bool frame)
        {
            job.dst = dst;
            if (defer && frame)
            {
                defer_convert(job);
            }
            else
            {
                run_convert(job, 0, job.height);
            }
        }, clear_first);
    }

    // research/137 N14: a texture data call's time on the calling thread, into
    // sTextureData (LLRHI_VK_PERF only).
    struct TextureDataTime
    {
        const U64 start = sPerf ? __rdtsc() : 0;
        ~TextureDataTime()
        {
            if (start)
            {
                sTextureData.ticks += __rdtsc() - start;
            }
        }
    };

} // namespace
    TextureDataCounts sTextureData;
    TexDefinedCounts sTexDefined;

    bool tex_defined_check()
    {
        static const bool on = []()
        {
            U64 check = 0;
            LLRHI::envNumber("LLRHI_TEX_DEFINED_CHECK", 0, ~(U64)0, check);
            const bool value = check != 0;
            if (value)
            {
                LL_INFOS("Vulkan") << "texture defined check on (LLRHI_TEX_DEFINED_CHECK=1, texture-leak 2026-10-27): every texture"
                                   << " view made is checked for levels defined but never written" << LL_ENDL;
            }
            return value;
        }();
        return on;
    }
namespace
{

    // LLRHI_TEX_DEFINED_CHECK: what the rule did, every 30 seconds while it
    // changes (once a frame, from compress_queued_textures). Compare the times
    // with "texture pressure" lines and with when a surface flickered.
    void report_tex_defined()
    {
        if (!tex_defined_check())
        {
            return;
        }
        using Clock = std::chrono::steady_clock;
        static Clock::time_point last;
        static TexDefinedCounts reported;
        const Clock::time_point now = Clock::now();
        if (now - last < std::chrono::seconds(30))
        {
            return;
        }
        last = now;
        const TexDefinedCounts& d = sTexDefined;
        if (!memcmp(&d, &reported, sizeof(d)))
        {
            return;
        }
        reported = d;
        LL_INFOS("Vulkan") << "texture defined check (since start): storage without texels " << d.withheld << " (render targets "
                           << d.attached << "), written later " << d.filled << " (cleared first " << d.cleared << "), uploads not recorded " << d.upload_failed
                           << ", mips of nothing skipped " << d.mips_skipped << ", scale-downs " << d.scaled << ", declined (old image kept) "
                           << d.scale_kept << ", from unpack buffers " << d.pbo_images << ", views over levels never written "
                           << d.unwritten << LL_ENDL;
    }

} // namespace

    // glTexImage2D / glCompressedTexImage2D into tex (the texture bound to
    // target, or the one a native upload names).
    void tex_image(Texture* tex, GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
                   GLenum format, GLenum type, const void* pixels, bool compressed, size_t compressed_size)
    {
        ++sTextureData.images;  // research/137 N14
        TextureDataTime time;
        if (!tex || width <= 0 || height <= 0)
        {
            return;
        }
        bool cube = is_cube_face(target);
        if (target != GL_TEXTURE_2D && !cube)
        {
            unhandled("glTexImage2D target " + hex(target));
            return;
        }
        U32 layer = cube ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
        // research/88: whether the viewer lets this image be stored BC7.
        if (level == 0 && pixels)
        {
            tex->bcn_wanted = LLRHI::tTexCompress;
        }
        else if (level != 0)
        {
            restore_plain(*tex);
        }
        // research/70: an image given pixels is attachable only once attached (make_attachable).
        const bool attach = !pixels || !LLRHI::sTexLazyAttach;
        if (level == 0 && !ensure_image(*tex, (GLenum)internal, width, height, cube ? 6 : 1, attach))
        {
            return;
        }
        if (!tex->image || (U32)level >= tex->levels)
        {
            return;
        }
        // The image keeps the size level 0 gave it: a level of another size would
        // be copied outside it. Dropped without a GL error: GL takes such a level
        // (the texture is then incomplete), and RenderDebugGL stops on any error.
        if ((U32)width != llmax(1u, tex->width >> level) || (U32)height != llmax(1u, tex->height >> level))
        {
            skip("glTexImage2D level of another size than level 0 gives it");
            return;
        }
        if (compressed && pixels)
        {
            // GL rejects data smaller than the level's blocks without reading it.
            const U32 block = compressed_block_bytes(tex->format.format);
            const size_t needed = (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * block;
            if (!tex->format.compressed || !block)
            {
                unhandled("glCompressedTexImage2D into internal format " + hex(tex->gl_internal));
                return;
            }
            if (compressed_size < needed)
            {
                skip("glCompressedTexImage2D data smaller than the level");
                raise_gl_error(GL_INVALID_VALUE);
                return;
            }
            compressed_size = needed;   // the staging copy reads only the level's blocks
        }
        const U32 bit = 1u << level;

        if (!pixels)
        {
            if (level == 0)
            {
                note_level0_write(*tex);    // contents now undefined: a queued BC7 entry is stale
            }
            // texture-leak (2026-10-27): storage only. A render target (it is
            // drawn into) counts as defined at once, as before. Any other
            // texture's level is defined only once texels are written into it
            // (a sub-image, a copy, mips): until then a draw samples white
            // (gather_slots) instead of the image's memory, which the texture
            // pool hands on from whatever image it held last (LLImageGL::
            // scaleDown's new image showed other textures' pictures). The same
            // image kept (same size and format) keeps what it had.
            if (tex->attach_wanted || tex->rendered_to)
            {
                tex->defined_levels |= bit;
            }
            else if (!(tex->defined_levels & bit))
            {
                ++sTexDefined.withheld;
            }
            return;
        }
        // Defined once the upload is recorded (not when it could not be).
        const bool uploaded = compressed ? upload(*tex, level, layer, 0, 0, width, height, pixels, compressed_size)
                                         : upload_converted(*tex, level, layer, 0, 0, width, height, format, type, pixels);
        if (uploaded)
        {
            tex->defined_levels |= bit;
            tex->written_levels |= bit;
        }
        else
        {
            ++sTexDefined.upload_failed;
        }
    }
namespace
{

    // GL rejects a sub-image outside the level (GL_INVALID_VALUE) without reading
    // the pixels. Do the same: callers may pass a source that only covers the
    // rectangle GL would have accepted (a media texture mid-resize).
    bool sub_image_fits(const Texture& tex, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth)
    {
        const S64 level_width = llmax(1u, tex.width >> level);
        const S64 level_height = llmax(1u, tex.height >> level);
        if (x < 0 || y < 0 || z < 0 || (S64)x + width > level_width || (S64)y + height > level_height || (S64)z + depth > (S64)tex.layers)
        {
            skip("sub-image outside the texture");
            return false;
        }
        return true;
    }

} // namespace
    // glTexSubImage2D into tex (the texture bound to target, or the one a
    // native upload names).
    void tex_sub_image(Texture* tex, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels)
    {
        ++sTextureData.sub_images;  // research/137 N14
        TextureDataTime time;
        if (!tex || !tex->image || !pixels || width <= 0 || height <= 0)
        {
            return;
        }
        restore_plain(*tex);    // research/88
        if ((U32)level >= tex->levels)
        {
            return;
        }
        if (tex->format.compressed || tex->format.depth)
        {
            unhandled("glTexSubImage2D on compressed/depth texture");
            return;
        }
        U32 layer = is_cube_face(target) ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
        if (!sub_image_fits(*tex, level, x, y, (GLint)layer, width, height, 1))
        {
            return;
        }
        // texture-leak (2026-10-27): the first write into a level without
        // contents defines it; one that covers only part of it clears the
        // level first, so the rest is zero, not the pool's old memory.
        const U32 bit = 1u << level;
        const bool was_defined = (tex->defined_levels & bit) != 0;
        const bool whole = x == 0 && y == 0 && (U32)width == llmax(1u, tex->width >> level) && (U32)height == llmax(1u, tex->height >> level);
        const bool clear_first = !was_defined && !whole;
        if (!upload_converted(*tex, level, layer, x, y, width, height, format, type, pixels, clear_first))
        {
            ++sTexDefined.upload_failed;
            return;
        }
        if (!was_defined)
        {
            tex->defined_levels |= bit;
            ++sTexDefined.filled;
            sTexDefined.cleared += clear_first ? 1 : 0;
        }
        tex->written_levels |= bit;
    }
namespace
{

    size_t client_layer_size(GLenum format, GLenum type, GLsizei width, GLsizei height)
    {
        size_t channels = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 :
                          format == GL_RG ? 2 : 1;
        size_t component = type == GL_FLOAT ? 4 : type == GL_HALF_FLOAT ? 2 : 1;
        size_t row = (size_t)(sUnpackRowLength > 0 ? sUnpackRowLength : width) * channels * component;
        row = (row + sUnpackAlignment - 1) / sUnpackAlignment * sUnpackAlignment;
        return row * height;
    }

} // namespace
    // Cube map arrays (reflection probes) and 2D arrays; each mip level is specified separately.
    void tex_image_3d(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLsizei depth,
                      GLenum format, GLenum type, const void* pixels)
    {
        Texture* tex = bound_texture(target);
        if (!tex || width <= 0 || height <= 0 || depth <= 0)
        {
            return;
        }
        if (target != GL_TEXTURE_CUBE_MAP_ARRAY && target != GL_TEXTURE_2D_ARRAY)
        {
            unhandled("glTexImage3D target " + hex(target));
            return;
        }
        if (level == 0 && !ensure_image(*tex, (GLenum)internal, width, height, depth))
        {
            return;
        }
        // Security audit B 6: a later level has to fit the image made at level 0.
        if (!tex->image || (U32)level >= tex->levels || !sub_image_fits(*tex, level, 0, 0, 0, width, height, depth))
        {
            return;
        }
        // Probe arrays: render targets, defined at once (texture-leak 2026-10-27 leaves them as they were).
        tex->defined_levels |= 1u << level;
        if (pixels)
        {
            size_t layer_size = client_layer_size(format, type, width, height);
            for (GLsizei layer = 0; layer < depth; ++layer)
            {
                upload_converted(*tex, level, layer, 0, 0, width, height, format, type, (const U8*)pixels + layer * layer_size);
            }
            tex->written_levels |= 1u << level;
        }
    }

    void tex_sub_image_3d(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth,
                          GLenum format, GLenum type, const void* pixels)
    {
        Texture* tex = bound_texture(target);
        if (!tex || !tex->image || !pixels || (U32)level >= tex->levels || width <= 0 || height <= 0 || depth <= 0 ||
            !sub_image_fits(*tex, level, x, y, z, width, height, depth))
        {
            return;
        }
        size_t layer_size = client_layer_size(format, type, width, height);
        for (GLsizei layer = 0; layer < depth; ++layer)
        {
            upload_converted(*tex, level, z + layer, x, y, width, height, format, type, (const U8*)pixels + layer * layer_size);
        }
    }

    // research/288: glCompressedTexSubImage3D, blocks into layers of a 2D array
    // (the constellation pictures, BC4): each layer's blocks one after another,
    // the layers in one copy (as the uploads of glCompressedTexImage2D: the
    // blocks are copied as they are, nothing is converted). GL's checks, each
    // with its error and nothing read: a 2D array (GL_INVALID_ENUM), an image
    // to write into in that very compressed format (GL_INVALID_OPERATION), the
    // rectangle inside the level on whole blocks and data as large as the
    // blocks (GL_INVALID_VALUE). A copy that cannot be recorded (no staging
    // memory) is GL_OUT_OF_MEMORY, so the caller can tell.
    void tex_compressed_sub_image_3d(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height,
                                     GLsizei depth, GLenum format, GLsizei size, const void* data)
    {
        ++sTextureData.sub_images;  // research/137 N14
        TextureDataTime time;
        if (target != GL_TEXTURE_2D_ARRAY)
        {
            unhandled("glCompressedTexSubImage3D target " + hex(target));
            raise_gl_error(GL_INVALID_ENUM);
            return;
        }
        Texture* tex = bound_texture(target);
        if (!tex || !tex->image)
        {
            raise_gl_error(GL_INVALID_OPERATION);   // no storage (made by glTexImage3D, which failed or did not run)
            return;
        }
        const U32 block = compressed_block_bytes(tex->format.format);
        if (!tex->format.compressed || !block || format != tex->gl_internal)
        {
            unhandled("glCompressedTexSubImage3D into internal format " + hex(tex->gl_internal));
            raise_gl_error(GL_INVALID_OPERATION);
            return;
        }
        if (!data || (U32)level >= tex->levels || width <= 0 || height <= 0 || depth <= 0 || size < 0)
        {
            raise_gl_error(GL_INVALID_VALUE);
            return;
        }
        // inside the level first (in 64 bits: no sum overflows), then whole blocks only: the
        // rectangle starts on a block and covers whole ones (or runs to the level's edge)
        if (!sub_image_fits(*tex, level, x, y, z, width, height, depth))
        {
            raise_gl_error(GL_INVALID_VALUE);
            return;
        }
        const S64 level_width = llmax(1u, tex->width >> level);
        const S64 level_height = llmax(1u, tex->height >> level);
        if (x % 4 || y % 4 || ((S64)x + width != level_width && width % 4) || ((S64)y + height != level_height && height % 4))
        {
            raise_gl_error(GL_INVALID_VALUE);
            return;
        }
        const size_t layer_bytes = (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4) * block;
        if ((size_t)size < layer_bytes * (size_t)depth)
        {
            skip("glCompressedTexSubImage3D data smaller than the blocks");
            raise_gl_error(GL_INVALID_VALUE);
            return;
        }
        const U32 bit = 1u << level;
        const size_t bytes = layer_bytes * (size_t)depth;
        if (!upload_with(*tex, (U32)level, (U32)z, x, y, (U32)width, (U32)height, bytes,
                         [data, bytes](U8* dst, bool) { memcpy(dst, data, bytes); }, false, (U32)depth))
        {
            ++sTexDefined.upload_failed;
            raise_gl_error(GL_OUT_OF_MEMORY);
            return;
        }
        tex->defined_levels |= bit;
        tex->written_levels |= bit;
    }

    // glGenerateMipmap on tex (the texture bound to the target, or the one a
    // native call names).
    void generate_mipmaps(Texture* tex)
    {
        ++sTextureData.mip_chains;  // research/137 N14
        TextureDataTime time;
        if (!tex || !tex->image || tex->format.compressed || tex->format.depth || tex->levels < 2)
        {
            return;
        }
        const VkFormatFeatureFlags features = format_features(tex->format.format);
        // Each level is read from the one above and written in the same image,
        // so the format needs both blit directions (logged once, left as is).
        if (!(features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(features & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        {
            unhandled("glGenerateMipmap on format without blit support");
            return;
        }
        // A linear blit needs linear filtering of the format (e.g. 32-bit float
        // on some GPUs); otherwise each level takes the nearest texel.
        const VkFilter filter = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        // glGenerateMipmap fills base+1 .. max level only.
        const U32 last = llmin(tex->levels, tex->max_level + 1);
        if (last < 2)
        {
            return;
        }
        // texture-leak (2026-10-27): mips of a level 0 without contents would
        // be made from the pool's old memory (smooth bands of other textures'
        // colours): none are made, the texture samples white until written.
        if (!(tex->defined_levels & 1))
        {
            ++sTexDefined.mips_skipped;
            return;
        }
        auto record_mips = [&](VkCommandBuffer cmd)
        {
            transition(cmd, *tex, VK_IMAGE_LAYOUT_GENERAL);
            for (U32 level = 1; level < last; ++level)
            {
                VkImageBlit blit = {};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, tex->layers };
                blit.srcOffsets[1] = { (S32)llmax(1u, tex->width >> (level - 1)), (S32)llmax(1u, tex->height >> (level - 1)), 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, tex->layers };
                blit.dstOffsets[1] = { (S32)llmax(1u, tex->width >> level), (S32)llmax(1u, tex->height >> level), 1 };
                vkCmdBlitImage(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, filter);

                VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            }
            transition(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        };
        if (VkCommandBuffer early = early_texture_commands(*tex))
        {
            record_mips(early);     // right after this texture's uploads, before the frame's draws
        }
        else
        {
            tex->late_write_frame = sFrame;
            LLVKMirror::instance().record(record_mips);
        }
        const U32 made = (1u << last) - 1;
        tex->defined_levels |= made;
        if (tex->written_levels & 1)
        {
            tex->written_levels |= made;    // from written texels (a render target's are drawn: not tracked)
        }
    }
namespace
{

    // ------------------------------------------------------------------
    // research/88: textures stored BC7. A texture the viewer allows is made
    // plain as before; once its mips are made it is queued, and at the start
    // of a later frame (LLVKGL::compressTextures, a few a frame) its image is
    // encoded and replaced (llvktexcompress). research/200: a texture whose
    // image is shared with the compute queue (bcn_shared) is encoded there
    // instead (LLRHI_ASYNC_QUEUES on, or auto once verified), and replaced at
    // the start of a frame after the compute queue has finished it.

    struct BCNPending
    {
        GLuint name;
        VkImage image;      // the plain image queued (a texture remade since is queued again)
        U64 bytes;          // what that image takes
        U32 level0_writes;  // Texture::level0_writes when queued (written since: skipped)
    };
    std::vector<BCNPending> sBCNQueue;
    size_t sBCNQueueHead = 0;
    U64 sBCNQueuedBytes = 0;
    // bc7-fault: the ones a frame put off for being larger than its share
    // (back to the end of the queue after it), and how many it may put off.
    std::vector<BCNPending> sBCNPutOff;
    constexpr size_t BCN_PUT_OFF_MAX = 64;

    // research/200: an encoding on the compute queue, until it is done (finish_async_bcn).
    struct BCNInFlight
    {
        GLuint name = 0;
        VkImage plain = VK_NULL_HANDLE;     // the image encoded (the texture's at the time)
        U64 bytes = 0;                      // what it takes (queued again after a failed submission)
        U32 level0_writes = 0;              // Texture::level0_writes at the time
        U32 levels = 0;                     // sampled_levels at the time
        LLVKTexCompress::Encoded encoded;
        U64 value = 0;                      // its compute batch's value (0: this frame's, not submitted yet)
        bool written = false;               // the plain image was written or released meanwhile (bcn_note_write): thrown away
        bool wait = false;                  // written: the graphics submissions wait for its batch (H3)
    };
    std::vector<BCNInFlight> sBCNInFlight;

    // research/285 X2: an encoding on the compute queue still reads `image`
    // (or will take its texture's place from it).
    bool bcn_in_flight(VkImage image)
    {
        for (const BCNInFlight& f : sBCNInFlight)
        {
            if (f.plain == image)
            {
                return true;
            }
        }
        return false;
    }

    // research/285 X2: nothing submitted to the compute queue is still
    // running. VERIFY's second encodings (research/200 3.5) read a texture's
    // shared image there with no entry in sBCNInFlight; a frame's encodings
    // are all in its one batch. One query a frame, again after a submission.
    bool compute_queue_idle()
    {
        static U32 checked_frame = U32_MAX;
        static U64 checked_value = 0;
        static bool idle = true;
        LLVKQueues& queues = LLVKQueues::instance();
        const U64 submitted = queues.computeSubmitted();
        if (!submitted)
        {
            return true;    // the compute queue never used
        }
        if (checked_frame != sFrame || checked_value != submitted)
        {
            checked_frame = sFrame;
            checked_value = submitted;
            idle = queues.computeCompleted() >= submitted;
        }
        return idle;
    }

    U64 allocation_bytes(VmaAllocation memory)
    {
        VmaAllocationInfo info = {};
        vmaGetAllocationInfo(LLVKDevice::instance().getAllocator(), memory, &info);
        return info.size;
    }

    // The levels from 0 on that are defined and that sampling can reach.
    U32 sampled_levels(const Texture& tex)
    {
        const U32 last = llmin(tex.levels, tex.max_level + 1);
        U32 levels = 0;
        while (levels < last && (tex.defined_levels & (1u << levels)))
        {
            ++levels;
        }
        return levels;
    }

    // tex may be stored BC7: the viewer allowed it, and it is a plain RGBA8
    // 2D image of pixels, never rendered into, large enough.
    bool bcn_candidate(const Texture& tex)
    {
        return LLRHI::sTexBCN && tex.bcn_wanted && !tex.bcn_encoded && tex.image &&
               tex.target == GL_TEXTURE_2D && tex.layers == 1 && tex.format.format == VK_FORMAT_R8G8B8A8_UNORM &&
               !tex.rendered_to && !tex.attach_wanted && (tex.defined_levels & 1) &&
               (U64)tex.width * tex.height >= LLVKTexCompress::minTexels();
    }

} // namespace
    void queue_for_bcn(GLuint name, Texture& tex)
    {
        if (tex.bcn_queued || !bcn_candidate(tex))
        {
            return;
        }
        const U64 bytes = allocation_bytes(tex.memory);
        sBCNQueue.push_back({ name, tex.image, bytes, tex.level0_writes });
        sBCNQueuedBytes += bytes;
        tex.bcn_queued = true;
    }
namespace
{

    // research/200: tex's image is about to be written (a layout change away
    // from shader reading, wait) or released, while the compute queue may
    // still read it: the encoding's result is thrown away when it is done, and
    // for a write the graphics submissions from now on wait for that compute
    // batch (H3). (A released image's handle may name another image later.)
    void bcn_note_write(VkImage image, bool wait)
    {
        for (BCNInFlight& f : sBCNInFlight)
        {
            if (f.plain == image && !f.written)
            {
                f.written = true;
                f.wait = wait;
                // (0: this frame's batch, not submitted yet: submit_async_bcn makes the wait.)
                if (wait)
                {
                    LLVKQueues::instance().graphicsWaitsFor(f.value);
                }
            }
        }
    }

    // An encoded image no texture took: freed once the GPU is done with what
    // was recorded so far (and, through the frame's fence, with the compute
    // batches submitted before, research/200 3.2).
    void discard_encoded(const LLVKTexCompress::Encoded& encoded)
    {
        if (!encoded.image)
        {
            return;
        }
        const VkImage image = encoded.image;
        const VmaAllocation memory = encoded.memory;
        LLVKDevice::instance().forgetImage(memory);
        LLVKMirror::instance().defer([image, memory]() { vmaDestroyImage(LLVKDevice::instance().getAllocator(), image, memory); });
    }

    // tex takes encoded (levels 0..levels-1 in SHADER_READ_ONLY_OPTIMAL) in
    // place of its plain image, which goes once the GPU is done with it.
    void take_encoded(Texture& tex, const LLVKTexCompress::Encoded& encoded, U32 levels)
    {
        const U64 plain_bytes = allocation_bytes(tex.memory);
        const U64 encoded_bytes = allocation_bytes(encoded.memory);
        destroy_texture_objects(tex);
        TexFormat format;
        format.format = LLVKTexCompress::ENCODED_FORMAT;
        format.compressed = true;
        format.texel_size = 0;
        tex.format = format;
        tex.image = encoded.image;
        tex.memory = encoded.memory;
        tex.levels = levels;
        tex.defined_levels = (1u << levels) - 1;
        tex.written_levels = tex.defined_levels;
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex.attachable = false;
        tex.attach_later = false;
        tex.bcn_encoded = true;
        tex.bcn_plain_bytes = plain_bytes;
        tex.bcn_bytes = encoded_bytes;
        ++sLayoutState;
        LLVKTexCompress::noteCompressed(plain_bytes, encoded_bytes);
        if (sNameObjects)
        {
            std::ostringstream label;
            label << "texture image " << tex.width << "x" << tex.height << ", BC7 (research/88), " << levels << " levels";
            name_object((U64)tex.image, label.str());
        }
    }

    // Records the encoding of tex into cmd (the frame's upload commands) and
    // gives it the encoded image; the plain one goes once the GPU is done.
    // verify: a VERIFY pair's number (research/200), 0 for none.
    bool compress_texture(Texture& tex, GLuint name, VkCommandBuffer cmd, U64 verify)
    {
        const U32 levels = sampled_levels(tex);
        LLVKTexCompress::Encoded encoded;
        if (!levels || !LLVKTexCompress::encode(cmd, tex.image, tex.width, tex.height, levels, name, encoded, verify))
        {
            return false;
        }
        take_encoded(tex, encoded, levels);
        return true;
    }

    // research/200: the encodings the compute queue has finished take their
    // texture's place if it is still as it was (the same image, not written
    // since, still a candidate with the same levels); the others are thrown
    // away. Start of a frame, before anything of it samples the textures.
    void finish_async_bcn()
    {
        if (sBCNInFlight.empty())
        {
            return;
        }
        const U64 done = LLVKQueues::instance().computeCompleted();
        size_t kept = 0;
        for (size_t i = 0; i < sBCNInFlight.size(); ++i)
        {
            const BCNInFlight f = sBCNInFlight[i];
            if (!f.value || f.value > done)
            {
                sBCNInFlight[kept++] = f;
                continue;
            }
            Texture* tex = sTextures.find(f.name);
            const bool current = tex && !f.written && tex->image == f.plain && tex->level0_writes == f.level0_writes &&
                                 tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && bcn_candidate(*tex) &&
                                 sampled_levels(*tex) == f.levels;
            if (current)
            {
                // H6: what samples it from now on waits for the batch (done already: at once).
                LLVKQueues::instance().graphicsWaitsFor(f.value);
                take_encoded(*tex, f.encoded, f.levels);
            }
            else
            {
                discard_encoded(f.encoded);
            }
            LLVKTexCompress::noteAsyncResult(current);
        }
        sBCNInFlight.resize(kept);
    }

    // research/200: submits this frame's compute batch; the encodings from
    // first on wait in sBCNInFlight for it. A failed submission ran nothing:
    // their images go and their textures are queued again (encoded in the
    // frame from then on: the compute queue is not used again).
    void submit_async_bcn(size_t first)
    {
        const U64 value = LLVKTexCompress::submitAsync();
        if (value)
        {
            for (size_t i = first; i < sBCNInFlight.size(); ++i)
            {
                BCNInFlight& f = sBCNInFlight[i];
                f.value = value;
                if (f.wait)
                {
                    LLVKQueues::instance().graphicsWaitsFor(value);     // H3, written before the batch was submitted
                }
            }
            return;
        }
        for (size_t i = first; i < sBCNInFlight.size(); ++i)
        {
            const BCNInFlight& f = sBCNInFlight[i];
            discard_encoded(f.encoded);
            Texture* tex = sTextures.find(f.name);
            if (tex && !f.written && tex->image == f.plain && tex->level0_writes == f.level0_writes && !tex->bcn_queued)
            {
                sBCNQueue.push_back({ f.name, f.plain, f.bytes, f.level0_writes });
                sBCNQueuedBytes += f.bytes;
                tex->bcn_queued = true;
            }
        }
        sBCNInFlight.resize(first);
    }

} // namespace
    // The queue, oldest first, while the frame's budget lasts. A texture
    // deleted, remade or written since it was queued is skipped.
    // research/200: first, what the compute queue has finished goes in its
    // texture's place; textures shared with it are encoded there (ON), or in
    // the frame and a few also there, compared (VERIFY).
    void compress_queued_textures()
    {
        report_tex_defined();
        // bc7-fast-start (research/88 section 13): the encoder is made on
        // threads of its own from the first frame on; until it can encode,
        // the textures waiting stay queued (plain), none is dropped.
        const bool encoder_coming = LLRHI::sTexBCN && LLVKTexCompress::prepare();
        LLVKTexCompress::beginFrame();
        finish_async_bcn();
        VkCommandBuffer cmd = LLVKMirror::instance().uploadCommands();
        if (encoder_coming)
        {
            LLVKTexCompress::noteQueue((U32)(sBCNQueue.size() - sBCNQueueHead), sBCNQueuedBytes);
            return;
        }
        if (sBCNQueueHead == sBCNQueue.size() || !cmd)
        {
            return;
        }
        const bool on = LLRHI::sTexBCN && LLVKTexCompress::ready();
        const LLVKTexCompress::Async async = on ? LLVKTexCompress::asyncMode() : LLVKTexCompress::Async::OFF;
        // bc7-fault: the frame's share, by level-0 texels and by blocks of
        // every level (held to the GPU time earlier frames took; none in the
        // hold after the first texture, the textures then stay queued).
        const LLVKTexCompress::Budget budget = on ? LLVKTexCompress::frameBudget(sBCNQueuedBytes) : LLVKTexCompress::Budget();
        const size_t first_in_flight = sBCNInFlight.size();
        U64 texels = 0;
        U64 blocks = 0;
        sBCNPutOff.clear();
        while (sBCNQueueHead < sBCNQueue.size() &&
               (!on || (texels < budget.texels && blocks < budget.blocks && LLVKTexCompress::hasRoom())))
        {
            const BCNPending pending = sBCNQueue[sBCNQueueHead];
            Texture* tex = sTextures.find(pending.name);
            const bool still = tex && tex->image == pending.image && tex->level0_writes == pending.level0_writes;
            const bool candidate = still && on && bcn_candidate(*tex) && tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const bool to_compute = candidate && async == LLVKTexCompress::Async::ON && tex->bcn_shared;
            if (to_compute && !LLVKTexCompress::asyncRoom())
            {
                break;      // no compute batch free this frame (none waited for): it stays queued
            }
            // bc7-fault: one larger than what is left of the share goes to the
            // back of the queue (the frame's first may take up to budget.single).
            // One larger than budget.never (the GPU's speed sure) would wait
            // for good (a slow GPU): it leaves the queue and stays plain.
            const U64 need = candidate ? LLVKTexCompress::blocksFor(tex->width, tex->height, sampled_levels(*tex)) : 0;
            if (candidate && blocks + need > budget.blocks && !(blocks == 0 && need <= budget.single))
            {
                if (budget.never && need > budget.never)
                {
                    ++sBCNQueueHead;
                    sBCNQueuedBytes -= llmin(sBCNQueuedBytes, pending.bytes);
                    tex->bcn_queued = false;
                    LLVKTexCompress::noteLeftPlain();
                    continue;
                }
                if (sBCNPutOff.size() >= BCN_PUT_OFF_MAX)
                {
                    break;
                }
                sBCNPutOff.push_back(pending);      // still queued: its bytes and bcn_queued stay
                ++sBCNQueueHead;
                continue;
            }
            ++sBCNQueueHead;
            sBCNQueuedBytes -= llmin(sBCNQueuedBytes, pending.bytes);
            if (!still)
            {
                continue;   // remade or written since: note_level0_write let it be queued again
            }
            tex->bcn_queued = false;
            if (!candidate)
            {
                continue;
            }
            if (to_compute)
            {
                BCNInFlight f;
                f.name = pending.name;
                f.plain = tex->image;
                f.bytes = pending.bytes;
                f.level0_writes = tex->level0_writes;
                f.levels = sampled_levels(*tex);
                if (f.levels && LLVKTexCompress::encodeAsync(tex->image, tex->width, tex->height, f.levels, pending.name, f.encoded))
                {
                    sBCNInFlight.push_back(f);
                    texels += (U64)tex->width * tex->height;
                    blocks += need;
                }
                continue;
            }
            // VERIFY (research/200 3.5): a few are also encoded on the compute
            // queue and the two compared; the frame's encoding is the one used.
            U64 verify = 0;
            if (async == LLVKTexCompress::Async::VERIFY && tex->bcn_shared)
            {
                verify = LLVKTexCompress::verifySerial(pending.name);
                const U32 levels = sampled_levels(*tex);
                LLVKTexCompress::Encoded second;
                if (verify && (!levels || !LLVKTexCompress::asyncRoom() ||
                               !LLVKTexCompress::encodeAsync(tex->image, tex->width, tex->height, levels, pending.name, second, verify)))
                {
                    LLVKTexCompress::dropVerify(verify);
                    verify = 0;
                }
                discard_encoded(second);    // freed after this frame's fence, which follows the batch (submitted below)
            }
            if (compress_texture(*tex, pending.name, cmd, verify))
            {
                texels += (U64)tex->width * tex->height;
                blocks += need;
            }
            else if (verify)
            {
                LLVKTexCompress::dropVerify(verify);
            }
        }
        submit_async_bcn(first_in_flight);
        if (on)
        {
            LLVKTexCompress::endFrameEncodes(cmd, !sBCNPutOff.empty());
        }
        sBCNQueue.insert(sBCNQueue.end(), sBCNPutOff.begin(), sBCNPutOff.end());
        if (sBCNQueueHead == sBCNQueue.size())
        {
            sBCNQueue.clear();
            sBCNQueueHead = 0;
        }
        else if (sBCNQueueHead * 2 > sBCNQueue.size())
        {
            sBCNQueue.erase(sBCNQueue.begin(), sBCNQueue.begin() + (std::ptrdiff_t)sBCNQueueHead);
            sBCNQueueHead = 0;
        }
        LLVKTexCompress::noteQueue((U32)(sBCNQueue.size() - sBCNQueueHead), sBCNQueuedBytes);
    }

    // LLRHI::sTextureCompressed (research/88).
    void native_texture_compressed(const U32* names, U32 count, bool* out)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        for (U32 i = 0; i < count; ++i)
        {
            const Texture* tex = names[i] ? sTextures.find(names[i]) : nullptr;
            out[i] = tex && tex->bcn_encoded;
        }
    }

    // LLRHI::sTextureMoveHold (research/285 compact-hysteresis).
    void native_texture_move_hold(U32 name, bool hold)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (Texture* tex = name ? sTextures.find(name) : nullptr)
        {
            tex->move_hold = hold;
        }
    }

    // ------------------------------------------------------------------
    // research/285 X2: a texture's texels moved into a new image of its own
    // format on the GPU, in place of the image it has (tex-compact; tex-mipdrop
    // is to take it with first_level for a smaller image). In three steps, so
    // that many textures move with one barrier call before their copies and
    // one after: make_moved_image, record_image_moves, take_moved_image.

    bool image_movable(const Texture& tex)
    {
        // Left where they are: render targets (drawn into; attachment usage and
        // views), images the BC7 encoder waits for (bcn_queued: its queue entry
        // names the image) or may read on the compute queue now (shared with
        // it, while an entry of sBCNInFlight names it or a compute batch still
        // runs; once neither, it moves and its new image is not shared),
        // images not in shader reading right now, images without contents,
        // cube maps, arrays and depth.
        return tex.image && tex.memory && tex.target == GL_TEXTURE_2D && tex.layers == 1 && !tex.format.depth &&
               !tex.attach_wanted && !tex.rendered_to && !tex.attachable && !tex.attach_view && !tex.bcn_queued &&
               (!tex.bcn_shared || (!bcn_in_flight(tex.image) && compute_queue_idle())) &&
               tex.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && (tex.defined_levels & 1);
    }

    bool make_moved_image(Texture& tex, U32 first_level, LLVKDevice::ImageMemory where, ImageMove& move)
    {
        // first_level > 0 (a smaller image, for tex-mipdrop) is not used yet:
        // checked here until that path has been verified (the levels, GL's
        // max level, the BC7 byte counts in take_moved_image).
        llassert(first_level == 0);
        move = ImageMove();
        if (!image_movable(tex) || first_level >= tex.levels)
        {
            return false;
        }
        // The image as the texture's is now: no attachment usage (attachable
        // is false), its format (BC7 for one stored BC7) and its levels.
        bool attachable = false, could_attach = false;
        VkImageCreateInfo info = image_info(tex, false, attachable, could_attach);
        info.extent = { llmax(1u, tex.width >> first_level), llmax(1u, tex.height >> first_level), 1 };
        info.mipLevels = tex.levels - first_level;
        if (LLVKDevice::instance().createImage(info, move.image, move.memory, where) != VK_SUCCESS)
        {
            move = ImageMove();
            return false;
        }
        move.tex = &tex;
        move.first_level = first_level;
        return true;
    }

    void drop_moved_image(ImageMove& move)
    {
        if (move.image)
        {
            // Nothing was recorded with it: freed at once.
            LLVKDevice& device = LLVKDevice::instance();
            device.forgetImage(move.memory);
            device.noteResource(LLVKDevice::RESOURCE_RELEASED, (U64)move.image, move.memory, "texture image not moved into");
            device.noteResource(LLVKDevice::RESOURCE_FREED, (U64)move.image, move.memory, "texture image not moved into");
            vmaDestroyImage(device.getAllocator(), move.image, move.memory);
        }
        move = ImageMove();
    }
namespace
{
    std::vector<VkImageMemoryBarrier2> sMoveBarriers;   // record_image_moves' (kept: no allocation once grown)

    void move_barrier(VkImage image, U32 levels, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                      VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
    {
        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        sMoveBarriers.push_back(barrier);
    }

    void record_move_barriers(VkCommandBuffer cmd)
    {
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = (U32)sMoveBarriers.size();
        dependency.pImageMemoryBarriers = sMoveBarriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
        sMoveBarriers.clear();
    }

    // What samples a texture: the shaders of draws, and compute (the BC7 encoder reads plain images).
    constexpr VkPipelineStageFlags2 SAMPLING_STAGES =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
} // namespace

    bool record_image_moves(VkCommandBuffer cmd, const ImageMove* moves, U32 count)
    {
        if (!cmd || !count)
        {
            return false;
        }
        // Before the copies: each old image (in shader reading) after all that
        // came before it on the queue, earlier frames' draws and its uploads
        // included; each new image from nothing (new memory).
        for (U32 i = 0; i < count; ++i)
        {
            const ImageMove& move = moves[i];
            const Texture& tex = *move.tex;
            move_barrier(tex.image, tex.levels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            move_barrier(move.image, tex.levels - move.first_level, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        record_move_barriers(cmd);
        // The levels with contents (defined_levels), level first_level into level 0.
        for (U32 i = 0; i < count; ++i)
        {
            const ImageMove& move = moves[i];
            const Texture& tex = *move.tex;
            VkImageCopy regions[32];
            const U32 region_count = level_regions(tex, move.first_level, tex.defined_levels, regions);
            if (region_count)
            {
                vkCmdCopyImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, move.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               region_count, regions);
            }
        }
        // After them: both in shader reading. The old image as it was, for
        // anything still naming it until it goes; the new one for what samples
        // the texture from now on (its levels without contents too: the view
        // shows only the defined ones).
        for (U32 i = 0; i < count; ++i)
        {
            const ImageMove& move = moves[i];
            const Texture& tex = *move.tex;
            move_barrier(move.image, tex.levels - move.first_level, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         SAMPLING_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            move_barrier(tex.image, tex.levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, SAMPLING_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        record_move_barriers(cmd);
        if (cmd != LLVKMirror::instance().uploadCommands())
        {
            // In the frame's main commands: later writes this frame follow the
            // copy there, and draws recorded before cannot be repeated as they are
            // (as transition() does).
            for (U32 i = 0; i < count; ++i)
            {
                moves[i].tex->late_write_frame = sFrame;
            }
            ++sLayoutState;
        }
        return true;
    }

    void take_moved_image(const ImageMove& move)
    {
        Texture& tex = *move.tex;
        const U32 shift = move.first_level;
        const U32 levels = tex.levels - shift;
        const U32 kept = levels >= 32 ? ~0u : (1u << levels) - 1;
        const U32 defined_levels = (tex.defined_levels >> shift) & kept;
        const U32 written_levels = (tex.written_levels >> shift) & kept;
        const bool attach_later = tex.attach_later;
        const bool encoded = tex.bcn_encoded;
        const U64 plain_bytes = tex.bcn_plain_bytes >> (2 * shift);     // a plain image of the new size, about
        // The old image and its view go once the GPU is done with what was
        // recorded so far (the copy included); a new version, and its bindless
        // slot and publications are forgotten, as take_encoded's plain image.
        destroy_texture_objects(tex);
        tex.image = move.image;
        tex.memory = move.memory;
        tex.width = llmax(1u, tex.width >> shift);
        tex.height = llmax(1u, tex.height >> shift);
        tex.levels = levels;
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex.defined_levels = defined_levels;
        tex.written_levels = written_levels;
        tex.attach_later = attach_later;
        if (encoded)
        {
            // Still BC7 (destroy_texture_objects counted the old image out).
            tex.bcn_encoded = true;
            tex.bcn_plain_bytes = plain_bytes;
            tex.bcn_bytes = allocation_bytes(move.memory);
            LLVKTexCompress::noteCompressed(tex.bcn_plain_bytes, tex.bcn_bytes);
        }
        ++sLayoutState;
        if (sNameObjects)
        {
            std::ostringstream label;
            label << "texture image " << tex.width << "x" << tex.height << (encoded ? ", BC7" : "") << ", " << levels
                  << " levels (moved, research/285)";
            name_object((U64)tex.image, label.str());
        }
    }

    // Sets one sampling parameter of the bound texture; true when it changed
    // (only then do the caches keyed by the texture's version need to drop it:
    // LLGLSLShader::bindTexture sets the same filter on every bind).
    bool tex_parameter(GLenum target, GLenum pname, F32 value)
    {
        Texture* tex = bound_texture(target);
        if (!tex)
        {
            return false;
        }
        auto set = [](auto& field, auto v)
        {
            if (field == v)
            {
                return false;
            }
            field = v;
            return true;
        };
        switch (pname)
        {
        case GL_TEXTURE_MIN_FILTER: return set(tex->min_filter, (GLenum)value);
        case GL_TEXTURE_MAG_FILTER: return set(tex->mag_filter, (GLenum)value);
        case GL_TEXTURE_WRAP_S: return set(tex->wrap_s, (GLenum)value);
        case GL_TEXTURE_WRAP_T: return set(tex->wrap_t, (GLenum)value);
        case GL_TEXTURE_WRAP_R: return set(tex->wrap_r, (GLenum)value);
        case GL_TEXTURE_MAX_ANISOTROPY: return set(tex->anisotropy, value);
        case GL_TEXTURE_COMPARE_MODE: return set(tex->compare_mode, (GLenum)value);
        case GL_TEXTURE_COMPARE_FUNC: return set(tex->compare_func, (GLenum)value);
        case GL_TEXTURE_BASE_LEVEL: return set(tex->base_level, (U32)llmax(0.f, value));
        case GL_TEXTURE_MAX_LEVEL: return set(tex->max_level, (U32)llmax(0.f, value));
        case GL_TEXTURE_SWIZZLE_R: return set(tex->swizzle[0], (GLenum)value);
        case GL_TEXTURE_SWIZZLE_G: return set(tex->swizzle[1], (GLenum)value);
        case GL_TEXTURE_SWIZZLE_B: return set(tex->swizzle[2], (GLenum)value);
        case GL_TEXTURE_SWIZZLE_A: return set(tex->swizzle[3], (GLenum)value);
        default: return false;
        }
    }
namespace
{

    // ------------------------------------------------------------------
    // Samplers

    VkSamplerAddressMode address_mode(GLenum wrap)
    {
        switch (wrap)
        {
        case GL_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

} // namespace
    VkCompareOp compare_op(GLenum func)
    {
        switch (func)
        {
        case GL_NEVER: return VK_COMPARE_OP_NEVER;
        case GL_LESS: return VK_COMPARE_OP_LESS;
        case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
        case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case GL_GREATER: return VK_COMPARE_OP_GREATER;
        case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
        }
    }
namespace
{

    std::map<std::vector<U32>, VkSampler> sSamplers;

    VkSampler create_sampler(const Texture* tex, bool shadow);

} // namespace
    VkSampler sampler_for(const Texture* tex, bool shadow)
    {
        if (!tex)
        {
            return create_sampler(tex, shadow);
        }
        const U32 params[8] = { tex->min_filter, tex->mag_filter, tex->wrap_s, tex->wrap_t, tex->wrap_r,
                                (U32)(tex->anisotropy * 16), tex->compare_func, (U32)tex->format.format };
        if (tex->sampler_cached[shadow] && same_words(params, tex->sampler_params[shadow], 8))
        {
            return tex->sampler_cached[shadow];
        }
        VkSampler sampler = create_sampler(tex, shadow);
        tex->sampler_cached[shadow] = sampler;
        memcpy(tex->sampler_params[shadow], params, sizeof(params));
        return sampler;
    }
namespace
{

    VkSampler create_sampler(const Texture* tex, bool shadow)
    {
        Texture defaults;
        defaults.min_filter = GL_LINEAR;
        const Texture& t = tex ? *tex : defaults;
        // Formats without linear filtering support (some depth formats) fall back to nearest.
        bool linear_ok = !tex || (format_features(t.format.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        std::vector<U32> key = { t.min_filter, t.mag_filter, t.wrap_s, t.wrap_t, t.wrap_r, (U32)(t.anisotropy * 16),
                                 shadow ? 1u : 0u, t.compare_func, linear_ok ? 1u : 0u };
        auto it = sSamplers.find(key);
        if (it != sSamplers.end())
        {
            return it->second;
        }
        VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter = t.mag_filter == GL_NEAREST || !linear_ok ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter = (t.min_filter == GL_NEAREST || t.min_filter == GL_NEAREST_MIPMAP_NEAREST || t.min_filter == GL_NEAREST_MIPMAP_LINEAR || !linear_ok)
                         ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        // Linear mip blending also needs FILTER_LINEAR support (VUID-vkCmdDraw-mipmapMode-04770).
        info.mipmapMode = linear_ok && (t.min_filter == GL_NEAREST_MIPMAP_LINEAR || t.min_filter == GL_LINEAR_MIPMAP_LINEAR)
                          ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = address_mode(t.wrap_s);
        info.addressModeV = address_mode(t.wrap_t);
        info.addressModeW = address_mode(t.wrap_r);
        info.anisotropyEnable = t.anisotropy > 1.f;
        info.maxAnisotropy = llmin(t.anisotropy, LLVKDevice::instance().getLimits().maxSamplerAnisotropy);
        info.compareEnable = shadow;
        info.compareOp = compare_op(t.compare_func);
        info.minLod = 0.f;
        info.maxLod = (t.min_filter == GL_NEAREST || t.min_filter == GL_LINEAR) ? 0.25f : VK_LOD_CLAMP_NONE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler sampler = VK_NULL_HANDLE;
        vkCreateSampler(LLVKDevice::instance().getDevice(), &info, nullptr, &sampler);
        sSamplers[key] = sampler;
        return sampler;
    }

    // 1x1 stand-ins for unbound or incomplete textures, per view type
    // (white color, far-plane depth for shadow samplers).
    std::map<std::pair<VkImageViewType, bool>, Texture> sDummies;

} // namespace
    Texture* dummy_texture(VkImageViewType type, bool depth)
    {
        auto key = std::make_pair(type, depth);
        auto it = sDummies.find(key);
        if (it != sDummies.end())
        {
            return it->second.image ? &it->second : nullptr;
        }
        Texture& tex = sDummies[key];
        tex.target = type == VK_IMAGE_VIEW_TYPE_CUBE ? GL_TEXTURE_CUBE_MAP :
                     type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? GL_TEXTURE_CUBE_MAP_ARRAY :
                     type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        U32 layers = (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ? 6 : 1;
        tex.min_filter = GL_LINEAR;
        if (!ensure_image(tex, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, 1, 1, layers))
        {
            return nullptr;
        }
        LLVKMirror::instance().record([&](VkCommandBuffer cmd)
        {
            transition(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageSubresourceRange range = { full_aspect(tex), 0, tex.levels, 0, layers };
            if (depth)
            {
                VkClearDepthStencilValue far_plane = { 1.f, 0 };
                vkCmdClearDepthStencilImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_plane, 1, &range);
            }
            else
            {
                VkClearColorValue white = { { 1.f, 1.f, 1.f, 1.f } };
                vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
            }
            transition(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        tex.defined_levels = 1;
        tex.written_levels = 1;
        return &tex;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
