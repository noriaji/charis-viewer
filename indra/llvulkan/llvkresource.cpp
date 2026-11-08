/**
 * @file llvkresource.cpp
 * @brief The Vulkan backend's side of resources by handle (research/137 N14).
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

#include "llrhiresource.h"
// Before the #if: llvkgl_internal.h brings llglheaders.h, which defines
// LL_GL_FUNC_POINTER (as in the llvkgl*.cpp files).
#include "llvkgl_internal.h"

#if LL_GL_FUNC_POINTER

// A handle (LLRHI::BufferHandle) is the Buffer in sBuffers behind the GL
// name: a heap object of the name table, which stays where it is until the
// name is deleted (glDeleteBuffers erases it). LLVertexBuffer takes it once
// it has its name (the name has storage by then: LLVBOPool gave it
// sBufferData) and drops it before the name goes back to the pool, so it
// never holds one past the name's deletion.
namespace
{
    using namespace llvkgl;

    bool sInstalled = false;
    U32 sStaleHandles = 0;      // write_ranges' handles that were not the name's (under sLock)

    // LLRHI::sBufferHandle.
    LLRHI::BufferHandle* buffer_handle(U32 name)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return name ? reinterpret_cast<LLRHI::BufferHandle*>(sBuffers.find(name)) : nullptr;
    }

    // LLRHI::sBufferWriteRanges.
    void write_ranges(LLRHI::BufferHandle* handle, U32 name, const U8* data, const LLRHI::BufferRange* ranges, U32 count)
    {
        const U64 start = sPerf ? __rdtsc() : 0;
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Buffer* buffer = reinterpret_cast<Buffer*>(handle);
        if (!buffer || !name || !data || !ranges || !count)
        {
            return;
        }
        // The handle must still be the name's (see above). Checked once a
        // buffer (one lookup, where the old way looked the name up once a
        // range) until the A/B runs show it never fails: if it does, the
        // write goes to the name's buffer, as the old way's, and the VKPerf
        // line counts it ("stale handles", 0 expected).
        Buffer* const named = sBuffers.find(name);
        if (named != buffer)
        {
            ++sStaleHandles;
            LL_WARNS_ONCE("VKResource") << "a vertex buffer's handle is not its name's any more (name " << name
                                        << "): written by name (research/137 N14)" << LL_ENDL;
            buffer = named;
            if (!buffer)
            {
                return;
            }
        }
        U64 bytes = 0;
        for (U32 i = 0; i < count; ++i)
        {
            bytes += ranges[i].size;
        }
        buffer_write_ranges(name, *buffer, data, ranges, count);
        BufferWriteCounts& c = sBufferWrites;
        ++c.calls;
        ++c.by_handle;
        c.ranges += count;
        c.bytes += bytes;
        if (start)
        {
            c.ticks += __rdtsc() - start;
        }
    }
}

void LLVKResource::install()
{
    if (!LLRHI::sNativeResources)
    {
        return;
    }
    if (!LLRHI::sBufferData || !LLRHI::sBufferSubData)
    {
        LL_INFOS("VKResource") << "native resources off: the native buffer data hooks are not in (research/137 N14)" << LL_ENDL;
        return;
    }
    LLRHI::sBufferHandle = &buffer_handle;
    LLRHI::sBufferWriteRanges = &write_ranges;
    sInstalled = true;
    LL_INFOS("VKResource") << "native resources on (LLRHI_NATIVE_RESOURCES=1, research/137 N14): vertex buffers keep the backend's"
                           << " handle and write each unmap in one call; the immediate ring draws from its Vulkan buffer" << LL_ENDL;
}

bool LLVKResource::installed()
{
    return sInstalled;
}

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
    out << "resources (LLRHI_NATIVE_RESOURCES " << (sInstalled ? "on" : LLRHI::sNativeResources ? "asked, not in" : "off")
        << "): buffer writes " << b.calls * per << " (ranges " << b.ranges * per << ", by handle " << b.by_handle * per
        << ", stale handles since start " << sStaleHandles << ") " << b.bytes * per / 1024.0 << " KB";
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

void LLVKResource::install()
{
}

bool LLVKResource::installed()
{
    return false;
}

std::string LLVKResource::report(U32)
{
    return std::string();
}

#endif // LL_GL_FUNC_POINTER
