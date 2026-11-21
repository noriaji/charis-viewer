/**
 * @file llvkresource.cpp
 * @brief The resource counts of the VKPerf line (buffer writes, texture data).
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

#include "llvkresource.h"

// Before the #if: llvkgl_internal.h brings llglheaders.h, which defines
// LL_GL_FUNC_POINTER (as in the llvkgl*.cpp files).
#include "llvkgl_internal.h"

#if LL_GL_FUNC_POINTER

using namespace llvkgl;

std::string LLVKResource::report(U32 frames)
{
    frames = llmax(frames, 1u);
    const F64 per = 1.0 / frames;
    const F64 ticks_per_ms = perf_ticks_per_ns() * 1e6;
    const BufferWriteCounts b = sBufferWrites;
    const TextureDataCounts t = sTextureData;
    sBufferWrites = BufferWriteCounts();
    sTextureData = TextureDataCounts();
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "resources: buffer writes " << b.calls * per << " (ranges " << b.ranges * per << ") " << b.bytes * per / 1024.0 << " KB";
    if (sPerf)
    {
        out << " in " << std::setprecision(3) << b.ticks / ticks_per_ms * per << " ms" << std::setprecision(2);
    }
    out << ", texture data: images " << t.images * per << " (new " << t.new_images * per << "), sub-images " << t.sub_images * per
        << ", mip chains " << t.mip_chains * per;
    if (sPerf)
    {
        out << " in " << std::setprecision(3) << t.ticks / ticks_per_ms * per << " ms";
    }
    // texture-leak (2026-10-27): since the start.
    const TexDefinedCounts& d = sTexDefined;
    out << "; contents only once written: storage without texels " << d.withheld << " (render targets " << d.attached
        << ", written later " << d.filled << ", cleared first "
        << d.cleared << "), uploads not recorded " << d.upload_failed << ", mips of nothing skipped " << d.mips_skipped
        << ", scale-downs " << d.scaled << " (declined, old image kept " << d.scale_kept << "), from unpack buffers " << d.pbo_images;
    if (tex_defined_check())
    {
        out << ", defined check: never written " << d.unwritten;
    }
    return out.str();
}

#else // !LL_GL_FUNC_POINTER

std::string LLVKResource::report(U32)
{
    return std::string();
}

#endif // LL_GL_FUNC_POINTER
