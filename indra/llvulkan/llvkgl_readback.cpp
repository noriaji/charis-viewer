/**
 * @file llvkgl_readback.cpp
 * @brief Reads from the Vulkan images: glReadPixels, glGetTexImage, glGetTexLevelParameteriv, glCopyTexSubImage (research/42 G4, research/91).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
    // glReadPixels (research/42 G4). GL renders nothing, so reads come from
    // the Vulkan image behind the read framebuffer: the frame
    // recorded so far is submitted and waited for (LLVKMirror::submitAndWait),
    // the image is copied out whole and converted as GL would convert it. Rows
    // stay bottom first: the images are stored like GL's (only the mirror's
    // capture flips, for a top-first PNG). The copy is kept while nothing else
    // is recorded, so row-by-row reads (rawSnapshot) wait once. sRGB images are
    // returned as stored. Rare (snapshots, bakes, debug), so synchronous.

    ReadbackCache sReadback;
    U32 sReadbacks = 0, sReadbackWaits = 0;

namespace
{
    // Which framebuffer component (0-3: R, G, B, A) each channel of an image of
    // this format takes in a glCopyTexSubImage. GL fills the internal format's
    // components (luminance and intensity from R, alpha from A), so a channel
    // takes the first view component its swizzle shows it in. False when every
    // channel takes its own component (what a blit or copy does).
    bool copy_channel_sources(const TexFormat& format, U32 sources[4])
    {
        const VkComponentSwizzle view[4] = { format.swizzle.r, format.swizzle.g, format.swizzle.b, format.swizzle.a };
        bool remap = false;
        for (U32 channel = 0; channel < 4; ++channel)
        {
            sources[channel] = channel;
            for (U32 c = 0; c < 4; ++c)
            {
                const VkComponentSwizzle s = view[c];
                const U32 reads = s == VK_COMPONENT_SWIZZLE_IDENTITY ? c :
                                  s >= VK_COMPONENT_SWIZZLE_R && s <= VK_COMPONENT_SWIZZLE_A ? (U32)(s - VK_COMPONENT_SWIZZLE_R) : 4;
                if (reads == channel)
                {
                    sources[channel] = c;
                    break;
                }
            }
            remap |= sources[channel] != channel;
        }
        return remap;
    }

    bool copy_remapped(Texture& dst, const U32 sources[4], U32 level, S32 xoff, S32 yoff, U32 layer,
                       VkImage src_image, VkFormat src_format, U32 src_width, U32 src_height, Texture* src_tex,
                       S32 x, S32 y, U32 width, U32 height);

    // sReadback still holds this level and layer of the image, as it is now.
    bool readback_cached(VkImage image, VkImageAspectFlags aspect, VkFormat format, U32 level, U32 layer, U32 width, U32 height)
    {
        const ReadbackCache& c = sReadback;
        return !c.data.empty() && c.image == image && c.aspect == aspect && c.format == format && c.level == level &&
               c.layer == layer && c.width == width && c.height == height &&
               c.serial == LLVKMirror::instance().recordSerial() && c.texture_deletes == sTextureDeletes;
    }

    // Bytes per texel of the aspect copied to a buffer; 0: not read back.
    U32 readback_texel_size(VkFormat format, VkImageAspectFlags aspect)
    {
        if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
        {
            switch (format)
            {
            case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT:
                return 4;
            case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT:
                return 2;
            default:
                return 0;
            }
        }
        switch (format)
        {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
            return 4;
        case VK_FORMAT_R8_UNORM:
            return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT:
            return 2;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
            return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 0;
        }
    }

    // (half_to_float: llvkgl_texconvert.cpp, shared with the uploads.)

    // Unsigned 11- or 10-bit float (5-bit exponent, bias 15).
    F32 small_float(U32 bits, U32 mantissa_bits)
    {
        const U32 exponent = bits >> mantissa_bits;
        const U32 mantissa = bits & ((1u << mantissa_bits) - 1);
        if (exponent == 0)
        {
            return std::ldexp((F32)mantissa, -14 - (S32)mantissa_bits);
        }
        if (exponent == 31)
        {
            return mantissa ? std::numeric_limits<F32>::quiet_NaN() : std::numeric_limits<F32>::infinity();
        }
        return std::ldexp(1.f + (F32)mantissa / (F32)(1u << mantissa_bits), (S32)exponent - 15);
    }

    // One texel as GL reads it: missing channels are G = B = 0, A = 1.
    void decode_texel(VkFormat format, const U8* p, F32 out[4])
    {
        out[0] = out[1] = out[2] = 0.f;
        out[3] = 1.f;
        U16 h[4];
        F32 f[4];
        U32 packed;
        switch (format)
        {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
            for (int c = 0; c < 4; ++c) out[c] = p[c] / 255.f;
            break;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
            out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = p[3] / 255.f;
            break;
        case VK_FORMAT_R8_UNORM:
            out[0] = p[0] / 255.f;
            break;
        case VK_FORMAT_R8G8_UNORM:
            out[0] = p[0] / 255.f; out[1] = p[1] / 255.f;
            break;
        case VK_FORMAT_R16_SFLOAT:
            memcpy(h, p, 2);
            out[0] = half_to_float(h[0]);
            break;
        case VK_FORMAT_R16G16_SFLOAT:
            memcpy(h, p, 4);
            out[0] = half_to_float(h[0]); out[1] = half_to_float(h[1]);
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            memcpy(h, p, 8);
            for (int c = 0; c < 4; ++c) out[c] = half_to_float(h[c]);
            break;
        case VK_FORMAT_R16G16B16A16_UNORM:
            memcpy(h, p, 8);
            for (int c = 0; c < 4; ++c) out[c] = h[c] / 65535.f;
            break;
        case VK_FORMAT_R32_SFLOAT:
            memcpy(f, p, 4);
            out[0] = f[0];
            break;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            memcpy(out, p, 16);
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            memcpy(&packed, p, 4);
            out[0] = (packed & 0x3ff) / 1023.f;
            out[1] = ((packed >> 10) & 0x3ff) / 1023.f;
            out[2] = ((packed >> 20) & 0x3ff) / 1023.f;
            out[3] = (packed >> 30) / 3.f;
            break;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            memcpy(&packed, p, 4);
            out[0] = small_float(packed & 0x7ff, 6);
            out[1] = small_float((packed >> 11) & 0x7ff, 6);
            out[2] = small_float(packed >> 22, 5);
            break;
        default:
            break;
        }
    }

    F32 decode_depth(VkFormat format, const U8* p)
    {
        U32 bits = 0;
        F32 value = 0.f;
        switch (format)
        {
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            memcpy(&value, p, 4);
            return value;
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT:
            memcpy(&bits, p, 4);
            return (bits & 0xffffff) / 16777215.f;
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT:
            memcpy(&bits, p, 2);
            return bits / 65535.f;
        default:
            return 0.f;
        }
    }

    // One component in the client type, as glReadPixels converts it.
    void put_component(GLenum type, F32 v, U8* dst)
    {
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            *dst = (U8)(llclamp(v, 0.f, 1.f) * 255.f + 0.5f);
            break;
        case GL_UNSIGNED_SHORT:
        {
            const U16 value = (U16)(llclamp(v, 0.f, 1.f) * 65535.f + 0.5f);
            memcpy(dst, &value, 2);
            break;
        }
        case GL_UNSIGNED_INT:
        {
            const U32 value = (U32)(llclamp((F64)v, 0.0, 1.0) * 4294967295.0 + 0.5);
            memcpy(dst, &value, 4);
            break;
        }
        case GL_HALF_FLOAT:
        {
            const U16 value = float_to_half(v);
            memcpy(dst, &value, 2);
            break;
        }
        default:    // GL_FLOAT
            memcpy(dst, &v, 4);
            break;
        }
    }

    // Copies one level of one layer of the image (one aspect, width x height:
    // the level's size) into sReadback: bytes in all, texel each (0 for block
    // compressed images, copied as their blocks). Textures sampled before go
    // back to shader reading after the copy.
    bool read_back_image(VkImage image, VkFormat format, VkImageAspectFlags aspect, U32 level, U32 layer, U32 width, U32 height,
                         U32 texel, VkDeviceSize bytes, Texture* tex, VkImageLayout* default_layout, VkImageAspectFlags default_aspects)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo host = {};
        host.usage = VMA_MEMORY_USAGE_AUTO;
        host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VmaAllocationInfo mapped = {};
        if (vmaCreateBuffer(allocator, &info, &host, &buffer, &memory, &mapped) != VK_SUCCESS || !mapped.pMappedData)
        {
            if (buffer)
            {
                vmaDestroyBuffer(allocator, buffer, memory);
            }
            return false;
        }

        const VkImageLayout sampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto record_copy = [&](VkCommandBuffer cmd)
        {
            const bool was_sampled = tex && tex->layout == sampled;
            if (tex)
            {
                transition(cmd, *tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            }
            else
            {
                LLVKMirror::imageBarrier(cmd, image, default_aspects, *default_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
                *default_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                ++sLayoutState;
            }
            VkBufferImageCopy region = {};
            region.imageSubresource = { aspect, level, layer, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            if (was_sampled)
            {
                transition(cmd, *tex, sampled);     // later draws and uploads find it as before
            }

            VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        };

        bool done = true;
        if (mirror.isRecording())
        {
            record_copy(mirror.transferCommands());
            done = mirror.submitAndWait();
            // The command buffers started over: nothing is bound in them.
            sRecorded = Recorded();
            sRecorded.frame = sFrame;
            pass_invalidate();
            ++sReadbackWaits;
        }
        else
        {
            mirror.record(record_copy);     // a one-off submission, waited for
            // fix-round92: a failed (or skipped, after an earlier failure)
            // submission wrote nothing: the buffer holds no image, so nothing
            // is read back, as when submitAndWait fails above.
            done = mirror.stuckSubmitResult() == VK_SUCCESS;
        }

        ReadbackCache& c = sReadback;
        c.data.clear();
        if (done)
        {
            vmaInvalidateAllocation(allocator, memory, 0, VK_WHOLE_SIZE);
            c.data.assign((const U8*)mapped.pMappedData, (const U8*)mapped.pMappedData + info.size);
        }
        vmaDestroyBuffer(allocator, buffer, memory);
        c.image = image;
        c.aspect = aspect;
        c.format = format;
        c.level = level;
        c.layer = layer;
        c.width = width;
        c.height = height;
        c.texel = texel;
        c.serial = mirror.recordSerial();
        c.texture_deletes = sTextureDeletes;
        return done;
    }

    // research/88: one level of a texture stored BC7, blitted (decoded) into
    // a plain RGBA8 image of the level's size and read back from that.
    bool read_back_encoded(Texture& tex, U32 level, U32 width, U32 height)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VkImage plain = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        if (vmaCreateImage(allocator, &info, &gpu, &plain, &memory, nullptr) != VK_SUCCESS)
        {
            return false;
        }
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        auto record_blit = [&](VkCommandBuffer cmd)
        {
            const bool was_sampled = tex.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            transition(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            LLVKMirror::imageBarrier(cmd, plain, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1);
            VkImageBlit blit = {};
            blit.srcSubresource = { aspect, level, 0, 1 };
            blit.srcOffsets[1] = { (S32)width, (S32)height, 1 };
            blit.dstSubresource = { aspect, 0, 0, 1 };
            blit.dstOffsets[1] = { (S32)width, (S32)height, 1 };
            vkCmdBlitImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, plain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
            LLVKMirror::imageBarrier(cmd, plain, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
            if (was_sampled)
            {
                transition(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        };
        if (mirror.isRecording())
        {
            record_blit(mirror.transferCommands());
        }
        else
        {
            mirror.record(record_blit);
        }
        VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const bool done = read_back_image(plain, VK_FORMAT_R8G8B8A8_UNORM, aspect, 0, 0, width, height, 4,
                                          (VkDeviceSize)width * height * 4, nullptr, &layout, aspect);
        mirror.defer([plain, memory]() { vmaDestroyImage(LLVKDevice::instance().getAllocator(), plain, memory); });
        return done;
    }

    // copy_from_framebuffer into an 8-bit image whose channels take other
    // framebuffer components (copy_channel_sources): the source is read back
    // (the frame so far is waited for, as for glReadPixels), remapped and
    // uploaded in frame order. Only LLImageGL::scaleDown of luminance-alpha
    // and alpha textures (under memory pressure) comes here, so waiting is
    // acceptable. False when not handled: the caller blits instead.
    bool copy_remapped(Texture& dst, const U32 sources[4], U32 level, S32 xoff, S32 yoff, U32 layer,
                       VkImage src_image, VkFormat src_format, U32 src_width, U32 src_height, Texture* src_tex,
                       S32 x, S32 y, U32 width, U32 height)
    {
        const U32 channels = dst.format.format == VK_FORMAT_R8_UNORM ? 1 : dst.format.format == VK_FORMAT_R8G8_UNORM ? 2 : 0;
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        const U32 texel = readback_texel_size(src_format, aspect);
        if (!channels || !texel || x < 0 || y < 0 || (U64)x + width > src_width || (U64)y + height > src_height)
        {
            return false;
        }

        LLVKMirror& mirror = LLVKMirror::instance();
        ReadbackCache& c = sReadback;
        if (!readback_cached(src_image, aspect, src_format, 0, 0, src_width, src_height) &&
            !read_back_image(src_image, src_format, aspect, 0, 0, src_width, src_height, texel, (VkDeviceSize)src_width * src_height * texel,
                             src_tex, src_tex ? nullptr : &mirror.mColorLayout, VK_IMAGE_ASPECT_COLOR_BIT))
        {
            return false;
        }
        ++sReadbacks;

        std::vector<U8> pixels((size_t)width * height * channels);
        for (U32 row = 0; row < height; ++row)
        {
            const U8* src = c.data.data() + ((size_t)(y + row) * src_width + x) * texel;
            U8* out = pixels.data() + (size_t)row * width * channels;
            for (U32 col = 0; col < width; ++col, src += texel, out += channels)
            {
                F32 rgba[4];
                decode_texel(src_format, src, rgba);
                for (U32 ch = 0; ch < channels; ++ch)
                {
                    out[ch] = (U8)(llclamp(rgba[sources[ch]], 0.f, 1.f) * 255.f + 0.5f);
                }
            }
        }
        // False when the upload could not be recorded (texture-leak 2026-10-27): the caller blits instead.
        return upload(dst, level, layer, xoff, yoff, width, height, pixels.data(), pixels.size());
    }

    // ------------------------------------------------------------------
    // Texture readback (LLImageGL::readBackRaw: texture callbacks, sculpts,
    // GLTF; lltexlayer's Intel path). GL holds no texture data (a 1x1
    // placeholder at most), so the level parameters and images of the textures llvkgl knows come from their Vulkan images. Levels a
    // texture does not have answer 0, as GL does: readBackRaw gives up
    // without anything waited for. An image read costs one wait for the frame
    // recorded so far (read_back_image), like glReadPixels.

    // (compressed_block_bytes: llvkgl_texture.cpp, shared with the uploads.)

    // The texture bound to a target glGet*TexImage / glGetTexLevelParameteriv
    // can name (2D, cube faces, arrays), when llvkgl holds it.
    Texture* readback_texture(GLenum target)
    {
        const bool known = target == GL_TEXTURE_2D || is_cube_face(target) ||
                           target == GL_TEXTURE_CUBE_MAP_ARRAY || target == GL_TEXTURE_2D_ARRAY;
        return known ? bound_texture(target) : nullptr;
    }

    bool level_defined(const Texture& tex, GLint level)
    {
        return tex.image && level >= 0 && level < 32 && (U32)level < tex.levels && (tex.defined_levels & (1u << level));
    }

    // Compressed bytes of one level of one layer (0: not block compressed).
    U32 compressed_level_size(const Texture& tex, U32 level)
    {
        const U32 width = llmax(1u, tex.width >> level), height = llmax(1u, tex.height >> level);
        return ((width + 3) / 4) * ((height + 3) / 4) * compressed_block_bytes(tex.format.format);
    }

    // Reads one level of a 2D texture or cube face into sReadback. Arrays are
    // left to GL: GL returns every layer, and the one caller (the
    // LLCubeMapArray copy constructor) has room for a single face.
    // False when there is nothing to read here (no waiting then).
    bool read_back_level(GLenum target, GLint level, bool compressed, Texture*& tex_out, U32& width, U32& height, U32& texel)
    {
        if (target != GL_TEXTURE_2D && !is_cube_face(target))
        {
            return false;
        }
        Texture* tex = readback_texture(target);
        const U32 layer = is_cube_face(target) ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
        if (!tex || !level_defined(*tex, level) || layer >= tex->layers || tex->format.depth)
        {
            return false;
        }
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (tex->bcn_encoded)
        {
            // research/88: GL holds it plain (it never reports it compressed); decoded first.
            if (compressed)
            {
                unhandled("glGetCompressedTexImage of a texture stored BC7 by the backend");
                return false;
            }
            width = llmax(1u, tex->width >> level);
            height = llmax(1u, tex->height >> level);
            texel = 4;
            tex_out = tex;
            ++sReadbacks;
            return read_back_encoded(*tex, (U32)level, width, height);
        }
        const U32 block = compressed_block_bytes(tex->format.format);
        texel = readback_texel_size(tex->format.format, aspect);
        if (compressed ? !(tex->format.compressed && block) : !texel)
        {
            unhandled(std::string(compressed ? "glGetCompressedTexImage" : "glGetTexImage") + " of internal format " + hex(tex->gl_internal));
            return false;
        }
        width = llmax(1u, tex->width >> level);
        height = llmax(1u, tex->height >> level);
        const VkDeviceSize bytes = compressed ? (VkDeviceSize)compressed_level_size(*tex, level) : (VkDeviceSize)width * height * texel;
        tex_out = tex;
        ++sReadbacks;
        return readback_cached(tex->image, aspect, tex->format.format, level, layer, width, height) ||
               read_back_image(tex->image, tex->format.format, aspect, level, layer, width, height, texel, bytes, tex, nullptr, 0);
    }

    // Where each component a sampler sees comes from: 0-3 the decoded texel's
    // R, G, B, A, 4 zero, 5 one. The stored format's swizzle (alpha and
    // luminance kept as R8 / RG8) first, then GL_TEXTURE_SWIZZLE_*.
    void sampled_sources(const Texture& tex, U32 sources[4])
    {
        const VkComponentSwizzle view[4] = { tex.format.swizzle.r, tex.format.swizzle.g, tex.format.swizzle.b, tex.format.swizzle.a };
        U32 base[4];
        for (U32 c = 0; c < 4; ++c)
        {
            switch (view[c])
            {
            case VK_COMPONENT_SWIZZLE_R: base[c] = 0; break;
            case VK_COMPONENT_SWIZZLE_G: base[c] = 1; break;
            case VK_COMPONENT_SWIZZLE_B: base[c] = 2; break;
            case VK_COMPONENT_SWIZZLE_A: base[c] = 3; break;
            case VK_COMPONENT_SWIZZLE_ZERO: base[c] = 4; break;
            case VK_COMPONENT_SWIZZLE_ONE: base[c] = 5; break;
            default: base[c] = c; break;
            }
        }
        for (U32 c = 0; c < 4; ++c)
        {
            switch (tex.swizzle[c])
            {
            case GL_RED: sources[c] = base[0]; break;
            case GL_GREEN: sources[c] = base[1]; break;
            case GL_BLUE: sources[c] = base[2]; break;
            case GL_ALPHA: sources[c] = base[3]; break;
            case GL_ZERO: sources[c] = 4; break;
            case GL_ONE: sources[c] = 5; break;
            default: sources[c] = base[c]; break;
            }
        }
    }
} // namespace

namespace
{
    // The read framebuffer's colour image a copy reads (copy_from_framebuffer,
    // tex_scale_down). False, with the reason skip() logs, when there is none.
    struct CopySource
    {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        U32 width = 0, height = 0;
        Texture* tex = nullptr;     // null: the default framebuffer
    };

    bool copy_source(CopySource& src, const char*& why)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (sState.read_framebuffer == 0)
        {
            src.image = mirror.getColorImage();
            src.format = LLVKMirror::COLOR_FORMAT;
            src.width = mirror.getWidth();
            src.height = mirror.getHeight();
            src.tex = nullptr;
            return true;
        }
        auto fb = sFramebuffers.find(sState.read_framebuffer);
        U32 index = fb != sFramebuffers.end() ? fb->second.read_buffer - GL_COLOR_ATTACHMENT0 : 4;
        if (index >= 4 || !fb->second.color[index] || !sTextures.find(fb->second.color[index]))
        {
            why = "copy from unsupported read buffer";
            return false;
        }
        src.tex = sTextures.find(fb->second.color[index]);
        if (!src.tex->image || src.tex->format.depth)
        {
            why = "copy from depth or missing image";
            return false;
        }
        src.image = src.tex->image;
        src.format = src.tex->format.format;
        src.width = src.tex->width;
        src.height = src.tex->height;
        return true;
    }
} // namespace

    // glCopyTexSubImage2D/3D: read framebuffer region -> level/layer of dst (the
    // bound texture for the wrappers, a named one for the native copies).
    // True when the copy was recorded: the level is then defined (and written).
    bool copy_from_framebuffer(Texture* dst, GLint level, GLint xoff, GLint yoff, U32 layer, GLint x, GLint y, GLsizei width, GLsizei height)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            return false;
        }
        if (dst)
        {
            restore_plain(*dst);    // research/88
        }
        if (!dst || !dst->image || (U32)level >= dst->levels || width <= 0 || height <= 0 || dst->format.depth)
        {
            return false;
        }
        dst->late_write_frame = sFrame;

        // Source image and its tracked layout.
        CopySource src;
        const char* why = nullptr;
        if (!copy_source(src, why))
        {
            skip(why);
            return false;
        }
        const VkImage src_image = src.image;
        const VkFormat src_format = src.format;
        const U32 src_width = src.width, src_height = src.height;
        Texture* src_tex = src.tex;
        if (src_tex == dst)
        {
            skip("copy within the same texture");
            return false;
        }

        U32 dst_w = llmax(1u, dst->width >> level), dst_h = llmax(1u, dst->height >> level);
        S32 w = llmin((S32)width, llmin((S32)src_width - x, (S32)dst_w - xoff));
        S32 h = llmin((S32)height, llmin((S32)src_height - y, (S32)dst_h - yoff));
        if (w <= 0 || h <= 0 || x < 0 || y < 0 || xoff < 0 || yoff < 0 || layer >= dst->layers)
        {
            return false;
        }
        if (level == 0)
        {
            note_level0_write(*dst);    // research/88: a queued BC7 entry is stale
        }
        const U32 bit = 1u << level;
        if (!(dst->defined_levels & bit) && !dst->attach_wanted && !dst->rendered_to)
        {
            ++sTexDefined.filled;   // texture-leak (2026-10-27): storage made earlier, written now
        }

        // Luminance-alpha (R8G8, read as RRRG) and alpha (R8, read as 000R)
        // images take the framebuffer's alpha in G or R, where a blit would put
        // its G or R: those are remapped on the CPU (luminance R8 blits as is).
        U32 sources[4];
        if (copy_channel_sources(dst->format, sources) &&
            copy_remapped(*dst, sources, (U32)level, xoff, yoff, layer, src_image, src_format, src_width, src_height, src_tex,
                          x, y, (U32)w, (U32)h))
        {
            dst->defined_levels |= bit;
            dst->written_levels |= bit;
            return true;
        }

        VkCommandBuffer cmd = mirror.transferCommands();
        if (src_tex)
        {
            transition(cmd, *src_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        else
        {
            LLVKMirror::imageBarrier(cmd, src_image, VK_IMAGE_ASPECT_COLOR_BIT, mirror.mColorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
            mirror.mColorLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            ++sLayoutState;
        }
        transition(cmd, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        if (src_format == dst->format.format)
        {
            VkImageCopy region = {};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffset = { x, y, 0 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (U32)level, layer, 1 };
            region.dstOffset = { xoff, yoff, 0 };
            region.extent = { (U32)w, (U32)h, 1 };
            vkCmdCopyImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        else
        {
            VkImageBlit blit = {};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0] = { x, y, 0 };
            blit.srcOffsets[1] = { x + w, y + h, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (U32)level, layer, 1 };
            blit.dstOffsets[0] = { xoff, yoff, 0 };
            blit.dstOffsets[1] = { xoff + w, yoff + h, 1 };
            vkCmdBlitImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        }
        transition(cmd, *dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dst->defined_levels |= bit;
        dst->written_levels |= bit;
        return true;
    }

    // texture-leak (2026-10-27): LLImageGL::scaleDown (the FBO way: the
    // texture was just drawn smaller into the read framebuffer). Its old
    // image goes only when the new one can be filled: the conditions under
    // which copy_from_framebuffer would leave the new level without texels
    // are checked first, and then the old image stays (the texture keeps its
    // larger picture; scaleDown is asked again later). Before, the storage
    // call made the new image defined ahead of the copy, and a copy that did
    // not happen left the texture showing the pool's old memory: textures
    // freed before (the login screen's snapshot, purple and green bands from
    // their mips), on and off as the texture went down and up.
    bool tex_scale_down(Texture* tex, GLint internal, GLsizei width, GLsizei height)
    {
        const char* why = nullptr;
        CopySource src;
        if (!LLVKMirror::instance().isRecording())
        {
            why = "no frame being recorded";
        }
        else if (!tex || !tex->image || tex->format.depth || tex->target != GL_TEXTURE_2D || tex->layers != 1)
        {
            why = "not a 2D colour texture with an image";
        }
        else if (width <= 0 || height <= 0 || (U32)width > tex->width || (U32)height > tex->height)
        {
            why = "not smaller";
        }
        else if ((GLenum)internal != tex->gl_internal)
        {
            why = "another internal format";    // the texture's own format is one the image was made with
        }
        else if (!copy_source(src, why))
        {
            // why: the read framebuffer's reason
        }
        else if (src.tex == tex)
        {
            why = "copy within the same texture";
        }
        else if (src.width < (U32)width || src.height < (U32)height)
        {
            why = "read framebuffer smaller than the new image";   // a partial copy would leave the rest without texels
        }
        if (why)
        {
            if (++sTexDefined.scale_kept <= 20)
            {
                LL_INFOS("Vulkan") << "texture scale-down kept the old image (texture " << (tex ? tex->gl_name : 0) << ", to " << width
                                   << "x" << height << "): " << why << LL_ENDL;
            }
            return false;
        }
        touch_texture(tex);
        tex_image(tex, GL_TEXTURE_2D, 0, internal, width, height, 0, 0, nullptr, false, 0);
        if (tex->image && tex->width == (U32)width && tex->height == (U32)height)
        {
            copy_from_framebuffer(tex, 0, 0, 0, 0, 0, 0, width, height);
        }
        // The old image is gone either way: a new one that could not be made
        // or filled samples white (never defined without texels).
        ++sTexDefined.scaled;
        return true;
    }

    // glReadPixels from the Vulkan image of the read framebuffer. False when it
    // cannot (a format or source not handled): the caller asks GL.
    bool read_pixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
    {
        if (width <= 0 || height <= 0)
        {
            return true;
        }
        if (!pixels)
        {
            return false;
        }
        const bool depth = format == GL_DEPTH_COMPONENT;
        int order[4] = { 0, 1, 2, 3 };
        U32 channels = 0;
        switch (format)
        {
        case GL_RGBA: channels = 4; break;
        case GL_BGRA: channels = 4; order[0] = 2; order[2] = 0; break;
        case GL_RGB: channels = 3; break;
        case GL_BGR: channels = 3; order[0] = 2; order[2] = 0; break;
        case GL_RG: channels = 2; break;
        case GL_RED: channels = 1; break;
        case GL_GREEN: channels = 1; order[0] = 1; break;
        case GL_BLUE: channels = 1; order[0] = 2; break;
        case GL_ALPHA: channels = 1; order[0] = 3; break;
        case GL_DEPTH_COMPONENT: channels = 1; break;
        default: return false;
        }
        const U32 component = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT || type == GL_HALF_FLOAT ? 2 :
                              type == GL_FLOAT || type == GL_UNSIGNED_INT ? 4 : 0;
        if (!component || (depth && type == GL_HALF_FLOAT))
        {
            return false;
        }

        // The source: an attachment of the read framebuffer, or the default one.
        LLVKMirror& mirror = LLVKMirror::instance();
        VkImage image = VK_NULL_HANDLE;
        VkFormat vk_format = VK_FORMAT_UNDEFINED;
        U32 image_width = 0, image_height = 0;
        Texture* tex = nullptr;
        VkImageLayout* default_layout = nullptr;
        VkImageAspectFlags default_aspects = 0;
        const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (sState.read_framebuffer == 0)
        {
            image = depth ? mirror.getDepthImage() : mirror.getColorImage();
            vk_format = depth ? LLVKMirror::DEPTH_FORMAT : LLVKMirror::COLOR_FORMAT;
            default_layout = depth ? &mirror.mDepthLayout : &mirror.mColorLayout;
            default_aspects = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            image_width = mirror.getWidth();
            image_height = mirror.getHeight();
        }
        else
        {
            auto fb = sFramebuffers.find(sState.read_framebuffer);
            if (fb == sFramebuffers.end())
            {
                return false;
            }
            GLuint name = 0;
            if (depth)
            {
                name = fb->second.depth;
            }
            else
            {
                const U32 index = fb->second.read_buffer - GL_COLOR_ATTACHMENT0;
                name = index < 4 ? fb->second.color[index] : 0;
            }
            tex = name ? sTextures.find(name) : nullptr;
            if (!tex || !tex->image || tex->format.depth != depth || tex->layers != 1)
            {
                return false;
            }
            image = tex->image;
            vk_format = tex->format.format;
            image_width = tex->width;
            image_height = tex->height;
        }
        const U32 texel = readback_texel_size(vk_format, aspect);
        if (!image || !image_width || !image_height || !texel)
        {
            return false;
        }

        ++sReadbacks;
        ReadbackCache& c = sReadback;
        if (!readback_cached(image, aspect, vk_format, 0, 0, image_width, image_height) &&
            !read_back_image(image, vk_format, aspect, 0, 0, image_width, image_height, texel, (VkDeviceSize)image_width * image_height * texel,
                             tex, default_layout, default_aspects))
        {
            return false;
        }

        // Into the client's layout (pack row length and alignment); pixels
        // outside the image are left alone (GL leaves them undefined).
        const size_t pixel_bytes = (size_t)channels * component;
        size_t stride = (size_t)(sPackRowLength > 0 ? sPackRowLength : width) * pixel_bytes;
        if ((GLint)component < sPackAlignment)
        {
            stride = (stride + sPackAlignment - 1) / sPackAlignment * sPackAlignment;
        }
        U8* out = (U8*)pixels;
        for (GLsizei row = 0; row < height; ++row)
        {
            const S64 sy = (S64)y + row;
            if (sy < 0 || sy >= (S64)image_height)
            {
                continue;
            }
            U8* dst_row = out + (size_t)row * stride;
            const U8* src_row = c.data.data() + (size_t)sy * image_width * texel;
            for (GLsizei col = 0; col < width; ++col)
            {
                const S64 sx = (S64)x + col;
                if (sx < 0 || sx >= (S64)image_width)
                {
                    continue;
                }
                const U8* src = src_row + (size_t)sx * texel;
                U8* dst = dst_row + (size_t)col * pixel_bytes;
                if (depth)
                {
                    put_component(type, decode_depth(vk_format, src), dst);
                }
                else
                {
                    F32 rgba[4];
                    decode_texel(vk_format, src, rgba);
                    for (U32 i = 0; i < channels; ++i)
                    {
                        put_component(type, rgba[order[i]], dst + i * component);
                    }
                }
            }
        }
        return true;
    }

    // False: not a texture or parameter answered here, GL is asked.
    bool tex_level_parameter(GLenum target, GLint level, GLenum pname, GLint* params)
    {
        if (!params || (pname != GL_TEXTURE_WIDTH && pname != GL_TEXTURE_HEIGHT && pname != GL_TEXTURE_DEPTH &&
                        pname != GL_TEXTURE_COMPRESSED && pname != GL_TEXTURE_COMPRESSED_IMAGE_SIZE))
        {
            return false;
        }
        const Texture* tex = readback_texture(target);
        if (!tex)
        {
            return false;
        }
        const bool layered = target == GL_TEXTURE_CUBE_MAP_ARRAY || target == GL_TEXTURE_2D_ARRAY;
        GLint value = 0;
        if (level_defined(*tex, level))
        {
            // Compressed only when the Vulkan image keeps GL's blocks: generic
            // compressed formats (GL_COMPRESSED_RGBA, ...) are stored plain and
            // read back plain.
            const bool compressed = tex->format.compressed && compressed_block_bytes(tex->format.format);
            switch (pname)
            {
            case GL_TEXTURE_WIDTH: value = (GLint)llmax(1u, tex->width >> level); break;
            case GL_TEXTURE_HEIGHT: value = (GLint)llmax(1u, tex->height >> level); break;
            case GL_TEXTURE_DEPTH: value = layered ? (GLint)tex->layers : 1; break;
            case GL_TEXTURE_COMPRESSED: value = compressed ? GL_TRUE : GL_FALSE; break;
            default:    // GL_TEXTURE_COMPRESSED_IMAGE_SIZE: the whole level of an array, one face of a cube map
                value = compressed ? (GLint)(compressed_level_size(*tex, level) * (layered ? tex->layers : 1)) : 0;
                break;
            }
        }
        *params = value;
        return true;
    }

    // glGetTexImage from the Vulkan image, converted to format/type as GL
    // converts it, into the client's pack layout. Rows stay as stored (bottom
    // first, as uploaded). Components are what a sampler sees, so alpha and
    // luminance textures come back as they were uploaded; luminance requests
    // take R, as GL's do. False when not handled: GL is asked.
    bool get_tex_image(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
    {
        int order[4] = { 0, 1, 2, 3 };
        U32 channels = 0;
        switch (format)
        {
        case GL_RGBA: channels = 4; break;
        case GL_BGRA: channels = 4; order[0] = 2; order[2] = 0; break;
        case GL_RGB: channels = 3; break;
        case GL_BGR: channels = 3; order[0] = 2; order[2] = 0; break;
        case GL_RG: channels = 2; break;
        case GL_RED: case GL_LUMINANCE: channels = 1; break;
        case GL_GREEN: channels = 1; order[0] = 1; break;
        case GL_BLUE: channels = 1; order[0] = 2; break;
        case GL_ALPHA: channels = 1; order[0] = 3; break;
        case GL_LUMINANCE_ALPHA: channels = 2; order[1] = 3; break;
        default: return false;
        }
        if (type == GL_UNSIGNED_INT_8_8_8_8_REV && channels == 4)
        {
            type = GL_UNSIGNED_BYTE;    // the same bytes on a little-endian CPU (LLImageGL's BGRA textures)
        }
        const U32 component = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT || type == GL_HALF_FLOAT ? 2 :
                              type == GL_FLOAT || type == GL_UNSIGNED_INT ? 4 : 0;
        Texture* tex = nullptr;
        U32 width = 0, height = 0, texel = 0;
        if (!pixels || !component || !read_back_level(target, level, false, tex, width, height, texel))
        {
            return false;
        }

        U32 sampled[4];
        sampled_sources(*tex, sampled);
        U32 sources[4] = {};
        for (U32 i = 0; i < channels; ++i)
        {
            sources[i] = sampled[order[i]];
        }
        // (A texture stored BC7, research/88, was read back decoded to RGBA8.)
        const VkFormat vk_format = tex->bcn_encoded ? VK_FORMAT_R8G8B8A8_UNORM : tex->format.format;
        // 8-bit textures into bytes (nearly every readback): the bytes are
        // picked directly. Missing stored components read as G = B = 0, A = 1.
        const U32 stored = vk_format == VK_FORMAT_R8_UNORM ? 1 : vk_format == VK_FORMAT_R8G8_UNORM ? 2 :
                           vk_format == VK_FORMAT_R8G8B8A8_UNORM || vk_format == VK_FORMAT_R8G8B8A8_SRGB ? 4 : 0;
        const bool bytes = type == GL_UNSIGNED_BYTE && stored;

        const size_t pixel_bytes = (size_t)channels * component;
        size_t stride = (size_t)(sPackRowLength > 0 ? sPackRowLength : width) * pixel_bytes;
        if ((GLint)component < sPackAlignment)
        {
            stride = (stride + sPackAlignment - 1) / sPackAlignment * sPackAlignment;
        }
        const ReadbackCache& c = sReadback;
        for (U32 row = 0; row < height; ++row)
        {
            const U8* src = c.data.data() + (size_t)row * width * texel;
            U8* dst = (U8*)pixels + (size_t)row * stride;
            for (U32 col = 0; col < width; ++col, src += texel, dst += pixel_bytes)
            {
                if (bytes)
                {
                    for (U32 i = 0; i < channels; ++i)
                    {
                        const U32 s = sources[i];
                        dst[i] = s < stored ? src[s] : s == 3 || s == 5 ? 255 : 0;
                    }
                    continue;
                }
                F32 rgba[4];
                decode_texel(vk_format, src, rgba);
                for (U32 i = 0; i < channels; ++i)
                {
                    const U32 s = sources[i];
                    put_component(type, s < 4 ? rgba[s] : s == 5 ? 1.f : 0.f, dst + i * component);
                }
            }
        }
        return true;
    }

    // glGetCompressedTexImage: the blocks of a texture uploaded compressed.
    bool get_compressed_tex_image(GLenum target, GLint level, void* pixels)
    {
        Texture* tex = nullptr;
        U32 width = 0, height = 0, texel = 0;
        if (!pixels || !read_back_level(target, level, true, tex, width, height, texel))
        {
            return false;
        }
        memcpy(pixels, sReadback.data.data(), sReadback.data.size());
        return true;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
