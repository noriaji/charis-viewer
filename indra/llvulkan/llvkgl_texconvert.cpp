/**
 * @file llvkgl_texconvert.cpp
 * @brief GL client pixels into a texture's Vulkan texel layout, deferred to the cull threads (research/70, research/91).
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

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // IEEE half with round-to-nearest-even and denormals, as GL drivers convert.
    U16 float_to_half(F32 value)
    {
        U32 bits;
        memcpy(&bits, &value, 4);
        U32 sign = (bits >> 16) & 0x8000;
        U32 abs = bits & 0x7fffffff;
        if (abs >= 0x7f800000)
        {
            return (U16)(sign | 0x7c00 | (abs > 0x7f800000 ? 0x200 : 0));   // inf / nan
        }
        if (abs >= 0x477ff000)
        {
            return (U16)(sign | 0x7c00);    // overflow
        }
        if (abs < 0x38800000)
        {
            // Denormal half: shift the implicit-1 mantissa into place, rounding.
            if (abs < 0x33000000)
            {
                return (U16)sign;
            }
            U32 exponent = abs >> 23;
            U32 mantissa = (abs & 0x7fffff) | 0x800000;
            U32 shift = 126 - exponent;
            U32 half = mantissa >> shift;
            U32 rest = mantissa & ((1u << shift) - 1);
            U32 midpoint = 1u << (shift - 1);
            if (rest > midpoint || (rest == midpoint && (half & 1)))
            {
                ++half;
            }
            return (U16)(sign | half);
        }
        U32 half = ((abs - 0x38000000) >> 13);
        U32 rest = abs & 0x1fff;
        if (rest > 0x1000 || (rest == 0x1000 && (half & 1)))
        {
            ++half;
        }
        return (U16)(sign | half);
    }

    F32 half_to_float(U16 half)
    {
        const U32 sign = (U32)(half & 0x8000) << 16;
        U32 exponent = (half >> 10) & 0x1f;
        U32 mantissa = half & 0x3ff;
        U32 bits;
        if (exponent == 0)
        {
            if (!mantissa)
            {
                bits = sign;
            }
            else
            {
                exponent = 127 - 15 + 1;    // denormal: normalize the mantissa
                while (!(mantissa & 0x400))
                {
                    mantissa <<= 1;
                    --exponent;
                }
                bits = sign | (exponent << 23) | ((mantissa & 0x3ff) << 13);
            }
        }
        else if (exponent == 31)
        {
            bits = sign | 0x7f800000 | (mantissa << 13);
        }
        else
        {
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        }
        F32 value;
        memcpy(&value, &bits, 4);
        return value;
    }

    // Plan the conversion of one GL client image (format/type) into tex's
    // texel layout. Returns false for combinations not handled yet.
    bool plan_convert(const Texture& tex, GLenum format, GLenum type, const void* pixels, U32 width, U32 height, ConvertJob& job)
    {
        const U32 src_channels = format == GL_RGBA || format == GL_BGRA ? 4 :
                                 format == GL_RGB || format == GL_BGR ? 3 :
                                 format == GL_RG || format == GL_LUMINANCE_ALPHA ? 2 : 1;
        U32 src_component = type == GL_UNSIGNED_BYTE ? 1 : type == GL_FLOAT ? 4 : type == GL_HALF_FLOAT ? 2 : 0;
        if (!src_component || !tex.format.texel_size || tex.format.depth)
        {
            return false;
        }
        // Packed formats (11/11/10, 10/10/10/2) are render targets, not uploads.
        // Their 4-byte texel would otherwise pass the per-component check below
        // as 4 x 8 bits and be filled with RGBA8 bytes.
        if (tex.format.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || tex.format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        {
            return false;
        }
        U32 row_pixels = sUnpackRowLength > 0 ? (U32)sUnpackRowLength : width;
        size_t src_row = (size_t)row_pixels * src_channels * src_component;
        src_row = (src_row + sUnpackAlignment - 1) / sUnpackAlignment * sUnpackAlignment;

        const U32 dst_channels = tex.format.format == VK_FORMAT_R8_UNORM || tex.format.format == VK_FORMAT_R16_SFLOAT ||
                                 tex.format.format == VK_FORMAT_R32_SFLOAT ? 1 :
                                 tex.format.format == VK_FORMAT_R8G8_UNORM || tex.format.format == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
        const U32 dst_component = tex.format.texel_size / dst_channels;
        if (dst_component != 1 && dst_component != 2 && dst_component != 4)
        {
            return false;   // packed formats (11/11/10, 10/10/10/2) are render targets, not uploads
        }
        if (dst_component == 2 && tex.format.format == VK_FORMAT_R16G16B16A16_UNORM)
        {
            return false;   // 16-bit unorm uploads: not used by the viewer so far
        }
        job.src = (const U8*)pixels;
        job.src_row = src_row;
        job.dst = nullptr;
        job.width = width;
        job.height = height;
        job.format = format;
        job.type = type;
        job.src_channels = src_channels;
        job.src_component = src_component;
        job.dst_channels = dst_channels;
        job.dst_component = dst_component;
        job.texel_size = tex.format.texel_size;
        return true;
    }

    // Rows first_row .. end_row - 1 of a planned conversion into job.dst. Reads
    // only the job (no GL or tracker state): safe on any thread while the
    // source pixels and the destination stay valid.
    void run_convert(const ConvertJob& job, U32 first_row, U32 end_row)
    {
        const U32 width = job.width;
        const U32 src_channels = job.src_channels;
        const U32 src_component = job.src_component;
        const U32 dst_channels = job.dst_channels;
        const U32 dst_component = job.dst_component;
        const GLenum format = job.format;
        const GLenum type = job.type;

        // 8-bit to 8-bit (nearly every texture): the same bytes as the generic
        // loop below, which converted every component through a float. That
        // loop took milliseconds per texture on the main thread and held back
        // texture creation at heavy places.
        if (type == GL_UNSIGNED_BYTE && dst_component == 1)
        {
            const bool swap = format == GL_BGRA || format == GL_BGR;
            const U32 channels = llmin(src_channels, 4u);
            for (U32 y = first_row; y < end_row; ++y)
            {
                const U8* src = job.src + y * job.src_row;
                U8* dst = job.dst + (size_t)y * width * dst_channels;
                if (channels == dst_channels && !swap)
                {
                    memcpy(dst, src, (size_t)width * dst_channels);
                }
                else if (channels == 3 && dst_channels == 4)
                {
                    // One 4-byte store per texel (the staging memory is
                    // write-combined): the same bytes as four byte stores.
                    const U32 r = swap ? 2 : 0, b = swap ? 0 : 2;
                    for (U32 x = 0; x < width; ++x, src += 3, dst += 4)
                    {
                        const U8 texel[4] = { src[r], src[1], src[b], 255 };
                        memcpy(dst, texel, 4);
                    }
                }
                else
                {
                    for (U32 x = 0; x < width; ++x, src += src_channels, dst += dst_channels)
                    {
                        U8 bytes[4] = { 0, 0, 0, 255 };
                        for (U32 c = 0; c < channels; ++c)
                        {
                            bytes[c] = src[c];
                        }
                        if (swap)
                        {
                            std::swap(bytes[0], bytes[2]);
                        }
                        for (U32 c = 0; c < dst_channels; ++c)
                        {
                            dst[c] = bytes[c];
                        }
                    }
                }
            }
            return;
        }

        for (U32 y = first_row; y < end_row; ++y)
        {
            const U8* src = job.src + y * job.src_row;
            U8* dst = job.dst + (size_t)y * width * job.texel_size;
            for (U32 x = 0; x < width; ++x)
            {
                F32 value[4] = { 0.f, 0.f, 0.f, 1.f };
                U8 bytes[4] = { 0, 0, 0, 255 };
                for (U32 c = 0; c < src_channels && c < 4; ++c)
                {
                    const U8* p = src + (x * src_channels + c) * src_component;
                    if (type == GL_UNSIGNED_BYTE)
                    {
                        bytes[c] = *p;
                        value[c] = *p / 255.f;
                    }
                    else
                    {
                        if (type == GL_FLOAT)
                        {
                            memcpy(&value[c], p, 4);
                        }
                        else    // GL_HALF_FLOAT (plan_convert takes no other type)
                        {
                            U16 half;
                            memcpy(&half, p, 2);
                            value[c] = half_to_float(half);
                        }
                        if (dst_component == 1)
                        {
                            // Into 8-bit unorm as GL converts: clamped, rounded (NaN as 0).
                            const F32 unorm = value[c] > 0.f ? llmin(value[c], 1.f) : 0.f;
                            bytes[c] = (U8)(unorm * 255.f + 0.5f);
                        }
                    }
                }
                if (format == GL_BGRA || format == GL_BGR)
                {
                    std::swap(bytes[0], bytes[2]);
                    std::swap(value[0], value[2]);
                }

                if (dst_component == 1)
                {
                    for (U32 c = 0; c < dst_channels; ++c)
                    {
                        dst[x * dst_channels + c] = bytes[c];
                    }
                }
                else if (dst_component == 2)
                {
                    U16* d = (U16*)dst + x * dst_channels;
                    for (U32 c = 0; c < dst_channels; ++c)
                    {
                        if (type == GL_HALF_FLOAT && c < src_channels)
                        {
                            memcpy(&d[c], src + (x * src_channels + c) * 2, 2);
                        }
                        else
                        {
                            d[c] = float_to_half(value[c]);
                        }
                    }
                }
                else
                {
                    F32* d = (F32*)dst + x * dst_channels;
                    for (U32 c = 0; c < dst_channels; ++c)
                    {
                        d[c] = value[c];
                    }
                }
            }
        }
    }

namespace
{
    // Conversions left for LLRHI::sTexConvertJoin (research/70): written in
    // pieces of about 256 KB on the cull threads. The copies that read them are
    // already recorded. Main thread, under sLock.
    struct ConvertPiece
    {
        U32 job;
        U32 first_row;
        U32 end_row;
    };
    std::vector<ConvertJob> sPendingConverts;
    std::vector<ConvertPiece> sPendingPieces;
    constexpr size_t CONVERT_PIECE_BYTES = 256 * 1024;
} // namespace

    void defer_convert(const ConvertJob& job)
    {
        const U32 index = (U32)sPendingConverts.size();
        sPendingConverts.push_back(job);
        const size_t row_bytes = llmax((size_t)job.width * job.texel_size, (size_t)1);
        const U32 rows = (U32)llmax(CONVERT_PIECE_BYTES / row_bytes, (size_t)1);
        for (U32 y = 0; y < job.height; y += rows)
        {
            sPendingPieces.push_back({ index, y, llmin(y + rows, job.height) });
        }
        LLRHI::sTexConvertDeferredBytes.fetch_add(job.size(), std::memory_order_relaxed);
        LLRHI::sTexConvertPendingBytes.fetch_add(job.size(), std::memory_order_relaxed);
    }

    // LLRHI::sTexConvertJoin: the pieces first (the larger work), then the
    // caller's jobs, in one fork and join. Holds sLock throughout, so a
    // submission from another thread (which joins first, below) waits for it.
    void join_tex_converts(LLRHI::RunJobs run, U32 extra_count, const std::function<void(U32)>& extra)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const U32 pieces = (U32)sPendingPieces.size();
        const U32 count = pieces + extra_count;
        if (count)
        {
            auto job = [pieces, &extra](U32 index)
            {
                if (index < pieces)
                {
                    const ConvertPiece& piece = sPendingPieces[index];
                    run_convert(sPendingConverts[piece.job], piece.first_row, piece.end_row);
                }
                else
                {
                    extra(index - pieces);
                }
            };
            if (run && count > 1)
            {
                run(count, job);
            }
            else
            {
                for (U32 i = 0; i < count; ++i)
                {
                    job(i);
                }
            }
        }
        sPendingConverts.clear();
        sPendingPieces.clear();
        LLRHI::sTexConvertPendingBytes.store(0, std::memory_order_relaxed);
    }

    // LLVKMirror::sBeforeSubmit: the GPU must never copy staging memory whose
    // pixels are still to be written. The batches join before anything can
    // submit; this only catches a submission inside one (on this thread, or on
    // another: then it waits for sLock), writing what is left on this thread.
    void join_before_submit()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (sPendingPieces.empty())
        {
            return;
        }
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            LL_WARNS("Vulkan") << "Texture conversions still pending at a submission: written now (research/70)" << LL_ENDL;
        }
        join_tex_converts(nullptr, 0, std::function<void(U32)>());
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
