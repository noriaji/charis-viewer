/**
 * @file llvkgl_internal.h
 * @brief What the llvkgl*.cpp files share: types, object tables, state (research/91).
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

#ifndef LL_LLVKGL_INTERNAL_H
#define LL_LLVKGL_INTERNAL_H

#include "llvkgl.h"

#include "llfile.h"
#include "llglheaders.h"
#include "llvkdevice.h"
#include "llvkframetable.h"
#include "llvkmirror.h"
#include "llvkshader.h"
#include "llrhi.h"
#include "llrhiresource.h"
#include "llrhivbcensus.h"    // research/293 branch 0
#include "llvknative.h"
#include "llvktracker.h"
#include "llvkbindless.h"
#include "llvkattribpool.h"
#include "llvkgpudriven.h"
#include "llvkgpulist.h"
#include "llvktexcompress.h"
#include "llrhidrawlist.h"     // LLVKGL_REDIRECT cuts an open draw list (research/137 N7)
#include "llvkvolumetric.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <intrin.h>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// At the top of a GL entry point the viewer calls through a GL pointer.
// LLVKGL_REDIRECT submits a draw list the calling thread has open first
// (research/137 N7: the guard's cut at the GL entry points, which it cannot
// wrap; nothing on a thread without an open list). LLVKGL_REDIRECT_QUERY,
// for the entry points that only read state or objects the list's draws do
// not change (glGetIntegerv, glIsEnabled, glGetUniformLocation, ...), does
// nothing. The names are the retired translation thread's (research/207 T1:
// it sent the call there); `invoke` is not used any more. They stay until
// the GL entry points shrink (research/207 T3, T10).
#define LLVKGL_REDIRECT_QUERY(name, invoke)
#define LLVKGL_REDIRECT(name, invoke) LLRHI::DrawList::Builder::cut(name);

#if LL_GL_FUNC_POINTER

// The parts of the GL-on-Vulkan tracker are split into files by feature
// (research/91). What more than one file uses is declared here, in namespace
// llvkgl (the per-draw path's types and inline functions: llvkgl_drawpath.h);
// the rest stays in each file's anonymous namespace.
//   llvkgl.cpp              LLVKGL:: (install, the reports), the lock, skips, change counters, statistics
//   llvkgl_entry.cpp        the GL entry points (wrap_gl*), the GL context they track, install_gl_wraps
//   llvkgl_hooks.cpp        the native API LLRHI hands the viewer instead of GL, install_native_hooks
//   llvkgl_buffer.cpp       buffers: table, bindings, Vulkan copies, writes
//   llvkgl_texture.cpp      textures: units, images, views, uploads, mips, BC7, samplers
//   llvkgl_texconvert.cpp   pixel conversion of uploads (research/70)
//   llvkgl_readback.cpp     glReadPixels, glGetTexImage, glCopyTexSubImage
//   llvkgl_program.cpp      programs: link, shared layout, uniform writes
//   llvkgl_glsl*.cpp        the GLSL rewrites; llvkgl_oitshaders.cpp the OIT shaders and variant rewrite
//   llvkgl_linkbuild.cpp    the compile of a link, parallel reloads, prewarm (research/111)
//   llvkgl_pipeline.cpp     render targets and pipelines; llvkgl_pipeprewarm.cpp pipelines made before their first draw
//   llvkgl_descriptors.cpp  a draw's descriptors, push constants, dynamic state, the pass cache
//   llvkgl_draw.cpp         vertex inputs, draw, draw_one, direct draws, clear
//   llvkgl_runs.cpp         draw runs, draw templates, LLVKGLProbe
//   llvkgl_occlusion.cpp    occlusion queries, the depth-only path; llvkgl_oit.cpp the OIT sessions
//   llvktracker.cpp         LLVKTracker
namespace llvkgl
{
    extern LLVKTrackerMutex sLock;      // every wrapper body runs under it (llvkgl.cpp)

    // LLVKGL::DrawGlobals: the values a draw list brought (llvkdrawlist.cpp,
    // QueuedGlobals: taken when it was submitted, the OIT priority per draw)
    // while it records; null otherwise: the draw path reads LLRHI's. Read
    // through these.
    inline thread_local const LLVKGL::DrawGlobals* tQueuedDraw = nullptr;
    inline bool depth_only_pass() { return tQueuedDraw ? tQueuedDraw->depth_only : LLRHI::sDepthOnlyPass; }
    inline bool suppress_translation() { return tQueuedDraw ? tQueuedDraw->suppress_translation : LLRHI::sSuppressTranslation; }
    inline U32 occlusion_scope() { return tQueuedDraw ? tQueuedDraw->occlusion_scope : LLRHI::sOcclusionScope; }
    inline U32 census_tag() { return tQueuedDraw ? tQueuedDraw->census_tag : LLRHI::sCensusTag; }
    inline U32 oit_priority() { return tQueuedDraw ? tQueuedDraw->oit_priority : LLRHI::sOITPriority; }
    inline const LLRHI::ShadowDepthBias& shadow_depth_bias() { return tQueuedDraw ? tQueuedDraw->shadow_bias : LLRHI::sShadowDepthBias; }

    // Frame statistics (llvkgl.cpp): what was not replayed, and why.
    extern U32 sDrawsReplayed;
    void skip(const char* reason);
    void unhandled(const std::string& what);
    // Logged once per what, with the details of the first time (fix-pipeleak: a
    // GL program name in what made an entry per program and shader reload).
    void unhandled(const std::string& what, const std::string& first_detail);
    std::string hex(U32 value);
    // The fault report's address log (LLRHI_VK_ADDRESS_LOG, llvkgl.cpp): what a
    // Vulkan image or buffer was made for.
    extern const bool sNameObjects;
    void name_object(U64 handle, const std::string& name);

    // Per-frame cost accounting (llvkgl.cpp, LLRHI_VK_PERF): where the CPU time goes.
    // Parts of a draw (PERF_DRAW), the descriptor and record steps broken down
    // further, and every other GL call through the wrappers (PERF_GL_CALLS).
    enum PerfSlot { PERF_DRAW, PERF_VERTEX, PERF_DESCRIPTORS, PERF_SLOTS, PERF_UBO, PERF_IMAGES, PERF_SETS, PERF_BARRIERS,
                    PERF_PIPELINE, PERF_RECORD, PERF_BEGIN, PERF_DYNAMIC, PERF_GL_CALLS, PERF_COUNT };
    extern U64 sPerfTicks[PERF_COUNT];
    extern const bool sPerf;
    F64 perf_ticks_per_ns();        // the time stamp counter's ticks per ns since startup (the reports)
    // The GL calls through the wrappers (llvkgl_entry.cpp), and the costliest of them since the last report.
    extern U32 sGLCalls;
    std::string gl_call_report(U32 frames);
    extern U32 sNativeStateCalls;   // calls of the native state API (llvkgl_hooks.cpp)
    // Direct draws, and the rigged ones among them (research/56, LLRHI::sAvatarStats).
    extern U32 sDirectDraws;
    // Direct draws refused (LLRHI::sDirectDraw returned
    // false: the caller draws through the GL entry points), by reason.
    enum DirectRefusal
    {
        DIRECT_NO_PROGRAM,      // no program bound
        DIRECT_UNBUILT,         // the program did not build
        DIRECT_SHAPE,           // no vertices, no attributes, an index type other than 16/32 bits
        DIRECT_UNIT,            // a texture for a unit the program samples through descriptor images
        DIRECT_REFUSAL_COUNT
    };
    extern U32 sDirectRefused[DIRECT_REFUSAL_COUNT];
    std::string direct_refusals(U32 frames);    // "no program N, unbuilt N, shape N, descriptor unit N" per frame
    extern U32 sInvisibleDraws;     // draws writing nothing, skipped
    extern U32 sAvRiggedDraws, sAvRiggedRun, sAvRiggedStraight, sAvRiggedTemplate;
    // Descriptor sets and uniform copies; palette, material and constants rows
    // (copied, reused, shared); research/56's palette writes and rows.
    extern U32 sSetsAllocated, sSetsReused, sSetsCached, sUboCopies, sUboReused, sImagesReused;
    extern U32 sPaletteCopies, sPaletteReused, sPaletteShared, sPaletteSkips;
    extern U32 sPalettePlaced, sPaletteNamed;
    extern U32 sAvPaletteWrites, sAvPaletteRows, sAvPaletteNamed;
    extern U64 sAvPaletteWriteTicks, sAvPaletteRowTicks;
    extern U32 sMaterialCopies, sMaterialReused, sMaterialShared;
    extern U32 sConstsCopies, sConstsReused, sConstsShared;

    struct PerfScope
    {
        PerfSlot slot;
        U64 start = 0;
        explicit PerfScope(PerfSlot s) : slot(s) { if (sPerf) start = __rdtsc(); }
        ~PerfScope() { stop(); }
        void stop()
        {
            if (start)
            {
                sPerfTicks[slot] += __rdtsc() - start;
                start = 0;
            }
        }
    };

    // Inline equality for the per-draw comparisons: memcmp is a call each time
    // (about 8 a draw, 2% of the main thread at a heavy place). Bitwise, as
    // memcmp compares.
    inline bool same_words(const U32* a, const U32* b, U32 count)
    {
        for (U32 i = 0; i < count; ++i)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }
        return true;
    }

    // The same for `words` 4-byte words at any alignment (uniform data, palettes).
    inline bool same_words_at(const void* a, const void* b, U32 words)
    {
        const U8* x = (const U8*)a;
        const U8* y = (const U8*)b;
        for (U32 i = 0; i < words; ++i)
        {
            U32 u, v;
            memcpy(&u, x + (size_t)i * 4, 4);
            memcpy(&v, y + (size_t)i * 4, 4);
            if (u != v)
            {
                return false;
            }
        }
        return true;
    }

    template <typename T>
    inline bool same_bytes(const T& a, const T& b)
    {
        static_assert(std::is_trivially_copyable<T>::value && sizeof(T) % sizeof(U32) == 0, "whole words of plain data");
        U32 x[sizeof(T) / sizeof(U32)];
        U32 y[sizeof(T) / sizeof(U32)];
        memcpy(x, &a, sizeof(T));
        memcpy(y, &b, sizeof(T));
        return same_words(x, y, (U32)(sizeof(T) / sizeof(U32)));
    }

    // vector == vector, without the memcmp call (scalar elements).
    template <typename T>
    inline bool same_elements(const std::vector<T>& a, const std::vector<T>& b)
    {
        static_assert(std::is_scalar<T>::value, "scalar elements");
        const size_t n = a.size();
        if (n != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < n; ++i)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // GL object tables, change counters and the tracker's callbacks (llvkgl.cpp)

    // Change counters, so draws can tell "nothing changed since last time" cheaply.
    extern U32 sTextureDeletes;
    extern U32 sBufferDeletes;
    extern U32 sFrame;
    extern U32 sLayoutState;
    extern U32 sUniformBlockState;
    extern U32 sTextureState;
    extern std::atomic<U32> sTargetState;
    // research/155 T0b: draw buffer calls that named what the framebuffer
    // already had (render thread, like sNativeStateCalls; LLVKGL::counterReport).
    extern U32 sDrawBuffersKept;
    extern U32 sBindTargets;    // research/156 N12c (llvkgl_hooks.cpp)
    // research/156 A-DS: never the same twice (Buffer::gpu_serial, Texture::view_serial).
    U64 next_object_serial();
    extern std::atomic<U32> sPipelineState;
    extern U32 sVertexState;
    extern U32 sAttribConstants;
    extern thread_local U32 sFixedState;

    // LLVKTracker's callbacks (llvktracker.cpp sets them).
    extern void (*sTextureDeleted)(GLuint);
    extern void (*sTextureDeletedMore)(GLuint);
    extern void (*sBufferChanged)(GLuint);
    extern void (*sBufferChangedMore)(GLuint);

    struct Buffer;
    struct Texture;

    // GL buffer and texture names are small and dense: index them directly. A
    // draw looked buffers up by name several times (hash lookups were ~14% of the
    // main thread at a heavy place). Entries are heap objects, so pointers stay
    // valid until the name is deleted; unusually large names go to a map.
    template <typename T>
    class NameTable
    {
    public:
        T* find(GLuint name)
        {
            if (name < mDense.size())
            {
                return mDense[name].get();
            }
            if (name < DENSE_LIMIT)
            {
                return nullptr;
            }
            auto it = mSparse.find(name);
            return it != mSparse.end() ? &it->second : nullptr;
        }
        T& get(GLuint name)
        {
            if (name >= DENSE_LIMIT)
            {
                return named(mSparse[name], name);
            }
            if (name >= mDense.size())
            {
                mDense.resize(llmax((size_t)name + 1, mDense.size() * 2));
            }
            if (!mDense[name])
            {
                mDense[name] = std::make_unique<T>();
            }
            return named(*mDense[name], name);
        }
        void erase(GLuint name)
        {
            if (name < mDense.size())
            {
                mDense[name].reset();
            }
            else if (name >= DENSE_LIMIT)
            {
                mSparse.erase(name);
            }
        }
        // Every entry (LLRHI_VK_LIVE's counts).
        template <typename F>
        void forEach(F&& f) const
        {
            for (const std::unique_ptr<T>& entry : mDense)
            {
                if (entry)
                {
                    f(*entry);
                }
            }
            for (const auto& entry : mSparse)
            {
                f(entry.second);
            }
        }
        // Every entry with its name, to change it (research/245: under sLock).
        template <typename F>
        void forEachNamed(F&& f)
        {
            for (size_t name = 0; name < mDense.size(); ++name)
            {
                if (mDense[name])
                {
                    f((GLuint)name, *mDense[name]);
                }
            }
            for (auto& entry : mSparse)
            {
                f(entry.first, entry.second);
            }
        }
    private:
        // texture-leak (2026-10-27): a texture knows its GL name
        // (Texture::gl_name), so its own events can find its bindless slot.
        static T& named(T& entry, GLuint name)
        {
            if constexpr (std::is_same_v<T, Texture>)
            {
                entry.gl_name = name;
            }
            return entry;
        }
        static constexpr GLuint DENSE_LIMIT = 1u << 22;
        std::vector<std::unique_ptr<T>> mDense;
        std::unordered_map<GLuint, T> mSparse;
    };

    // ------------------------------------------------------------------
    // Buffers (llvkgl_buffer.cpp)

    // A GL buffer's Vulkan and CPU copies.
    struct Buffer
    {
        std::vector<U8> data;
        VkBuffer gpu = VK_NULL_HANDLE;
        VmaAllocation gpu_memory = nullptr;
        size_t gpu_size = 0;        // bytes of the Vulkan buffer actually allocated
        size_t dirty_begin = SIZE_MAX;
        size_t dirty_end = 0;
        U32 used_frame = U32_MAX;
        // research/156 A-DS: a new next_object_serial() whenever `gpu` is made
        // (a handle value may come back for another buffer; the serial never does).
        U64 gpu_serial = 0;
        bool uniform_source = false;    // read as a uniform block: writes bump sUniformBlockState
        // research/285: compact_buffers gave its Vulkan copy back; gpu_buffer makes
        // it again within a cap of bytes a frame (a turn of the camera does not
        // make hundreds of MB again in one frame and run the upload ring dry).
        bool compacted = false;
        U32 held_frame = U32_MAX;       // the frame its remake was last left to a later one (held_this_frame)
        // A new value from sBufferVersions at every write of its data (0: never
        // written), for LLRHI::sObjectVersions.
        U32 version = 0;
        // research/293 branch 0 (LLRHI_VB_CENSUS): the number of the LLVertexBuffer that owns this
        // GL name (vertex or index buffer), 0: none or not counting. Set and cleared by the
        // census only (census_set_owner); read by the counting calls beside the reads by name.
        U32 owner_number = 0;

        void markDirty(size_t begin, size_t end)
        {
            dirty_begin = llmin(dirty_begin, begin);
            dirty_end = llmax(dirty_end, end);
        }
    };

    extern NameTable<Buffer> sBuffers;
    VkBuffer gpu_buffer(Buffer& buffer);
    VkBuffer gpu_buffer_named(GLuint name);
    // research/285: gpu_buffer left the buffer's remake (its Vulkan copy given
    // back by compact_buffers) to a later frame this frame: a draw that needs
    // it draws nothing this frame (null: false).
    bool held_this_frame(const Buffer* buffer);
    // research/285: a VKBuffers line about those, now and then (render thread, no lock).
    void log_held_remakes();

    // gpu_buffer's results of this frame by GL name, for the draws that name
    // their buffers (direct draws, native shadow packets: ~140k lookups a frame
    // at a heavy place). A hit skips the name table and the Buffer itself (a
    // cache miss each): gpu_buffer would return the same handle and change
    // nothing, since it already ran for the buffer this frame (used_frame set,
    // nothing dirty, capacity still enough) and nothing wrote or deleted it since
    // (buffer_data / buffer_sub_data / glDeleteBuffers forget its entry).
    // Direct-mapped by name; a collision only costs the full lookup.
    struct ResolvedBuffer
    {
        GLuint name;    // 0: empty
        U32 frame;
        VkBuffer gpu;
    };
    constexpr U32 RESOLVED_BUFFERS = 1u << 15;
    extern ResolvedBuffer sResolvedBuffers[RESOLVED_BUFFERS];

    inline void forget_resolved_buffer(GLuint name)
    {
        ResolvedBuffer& e = sResolvedBuffers[name & (RESOLVED_BUFFERS - 1)];
        if (e.name == name)
        {
            e.name = 0;
        }
    }

    // The element array binding (per context) and its buffer, resolved again
    // after a buffer deletion.
    extern thread_local GLuint sElementBuffer;
    extern thread_local Buffer* sElementResolved;
    extern thread_local GLuint sElementResolvedName;
    extern thread_local U32 sElementResolvedAt;
    // Uniform buffer binding points (per context).
    struct UniformBinding
    {
        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr size = 0;
        Buffer* resolved = nullptr;     // valid while resolved_at == sBufferDeletes
        U32 resolved_at = U32_MAX;
    };
    constexpr U32 MAX_UNIFORM_BINDINGS = 128;
    extern thread_local UniformBinding sUniformBindings[MAX_UNIFORM_BINDINGS];    // binding point -> buffer

    // What the GL entry points and install use of the buffers (llvkgl_buffer.cpp).
    extern thread_local GLuint sArrayBuffer;        // the array and uniform buffer bindings (per context)
    extern thread_local GLuint sUniformBuffer;
    GLuint* binding_for(GLenum target);
    GLuint bound_name(GLenum target);
    Buffer* bound_buffer(GLenum target);
    void retire_gpu(Buffer& buffer);
    // research/245, 285: the steady clock in seconds, and the tracker frame at
    // about each second over the last 64 s, to tell a time as a number of
    // frames (the frame rate varies a lot while VRAM is short). The GL buffer
    // and texture pool compactions keep one history each (frame thread).
    inline F64 steady_seconds()
    {
        using namespace std::chrono;
        return duration<F64>(steady_clock::now().time_since_epoch()).count();
    }
    class FrameHistory
    {
    public:
        static constexpr U32 SIZE = 64;
        // `frame` at `now`, kept when a second or more after the last one kept.
        void note(F64 now, U32 frame)
        {
            if (!mCount || now - mAt[(mNext + SIZE - 1) % SIZE].seconds >= 1.0)
            {
                mAt[mNext] = { now, frame };
                mNext = (mNext + 1) % SIZE;
                mCount = llmin(mCount + 1, SIZE);
            }
        }
        // The newest frame kept at least `ago` seconds before `now` (false:
        // the history does not reach back that far).
        bool frameBefore(F64 now, F64 ago, U32& frame) const
        {
            for (U32 i = 0; i < mCount; ++i)
            {
                const FrameAt& at = mAt[(mNext + SIZE - 1 - i) % SIZE];
                if (at.seconds <= now - ago)
                {
                    frame = at.frame;
                    return true;
                }
            }
            return false;
        }
    private:
        struct FrameAt
        {
            F64 seconds = 0.0;
            U32 frame = 0;
        };
        FrameAt mAt[SIZE];
        U32 mCount = 0;
        U32 mNext = 0;
    };
    // research/245 (LLRHI::sBufferCompact): at the start of a frame, under
    // sLock, while VRAM is short, the Vulkan copies of the buffers in the
    // emptiest GL buffer pool blocks go (llvkgl_buffer.cpp).
    void compact_buffers();
    void buffer_compact_counts(LLRHI::BufferCompactCounts& out);
    void buffer_data(GLuint name, Buffer* b, size_t size, const void* data);
    void buffer_sub_data(GLuint name, Buffer* b, size_t offset, size_t size, const void* data);
    void bind_uniform_buffer(GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void bind_buffer_base(GLenum target, GLuint index, GLuint buffer);
    // The native buffer API (research/39 N4, research/42).
    void native_buffer_data(U32 name, size_t size, const void* data);
    void native_buffer_sub_data(U32 name, size_t offset, size_t size, const void* data);
    void native_bind_buffer_base(U32 target, U32 index, U32 buffer);
    // research/137 N14 (LLRHI::sBufferWriteRanges, llvkresource.cpp): several
    // ranges of b (GL name `name`) from data + offset, in one call (under sLock).
    void buffer_write_ranges(GLuint name, Buffer& b, const U8* data, const LLRHI::BufferRange* ranges, U32 count);
    // Buffer writes through the native hooks, both ways (one range by name,
    // LLRHI::sBufferSubData; ranges by handle, sBufferWriteRanges), for the
    // VKPerf line (LLVKResource::report). Under sLock.
    struct BufferWriteCounts
    {
        U32 calls = 0;
        U32 ranges = 0;
        U32 by_handle = 0;      // of the calls
        U64 bytes = 0;
        U64 ticks = 0;          // __rdtsc, LLRHI_VK_PERF only (the lock included)
    };
    extern BufferWriteCounts sBufferWrites;
    // research/137 N14: while llvkimmediate.cpp draws from the frame's ring,
    // the ring's Vulkan buffer: direct_inputs takes the draw's vertices from
    // it instead of from the GL name's buffer. Null at any other time.
    extern thread_local VkBuffer tDirectVertexBuffer;

    // research/293 branch 0 (LLRHI_VB_CENSUS): counting only. The owner mark of GL name `name` is
    // set or cleared (cleared only if it is `number`'s); census_owner is the mark (0: none).
    static_assert(DIRECT_NO_PROGRAM == LLRHIVBCensus::REFUSE_NO_PROGRAM && DIRECT_UNBUILT == LLRHIVBCensus::REFUSE_UNBUILT &&
                  DIRECT_SHAPE == LLRHIVBCensus::REFUSE_SHAPE && DIRECT_UNIT == LLRHIVBCensus::REFUSE_UNIT &&
                  DIRECT_REFUSAL_COUNT == LLRHIVBCensus::REFUSE_NOT_DIRECT,
                  "the census counts a direct draw's refusal by DirectRefusal's numbers");
    void census_set_owner(U32 name, U32 number);
    void census_clear_owner(U32 name, U32 number);
    U32 census_owner(GLuint name);
    void census_changed(GLuint name);

    // GL wrote a buffer's contents or deleted it: tell the copies made from it.
    inline void buffer_changed(GLuint name)
    {
        if (LLRHIVBCensus::sOn) census_changed(name);   // research/293 branch 0
        if (sBufferChanged) sBufferChanged(name);
        if (sBufferChangedMore) sBufferChangedMore(name);
    }

    // ------------------------------------------------------------------
    // Textures (llvkgl_texture.cpp)

    struct TexFormat
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkComponentMapping swizzle = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        U32 texel_size = 4;     // bytes per texel in the Vulkan image (0 for block compressed)
        bool compressed = false;
        bool depth = false;
        bool stencil = false;
    };

    // The fields every draw's texture slot lookup reads (gather_slots: version,
    // the frame's resolution, the slot, the layout its callers check) come
    // first, in one cache line (research/58).
    struct alignas(64) Texture
    {
        // Bumped whenever what a sampler of it sees may change (image, levels,
        // contents, view, sampling parameters): caches compare it. A new value
        // from next_texture_version (0: never given an image).
        U32 version = 0;
        // Its bindless resolution for this frame (direct_*), reused while frame
        // and version stay the same (and, with LLRHI_SLOT_CACHE=0, only for
        // direct draws into the same render target resolution).
        U32 direct_frame = U32_MAX;
        U32 direct_version = 0;
        U32 direct_target = 0;
        U32 bindless_slot = U32_MAX;    // its entry in the bindless table, once sampled through it
        U32 defined_levels = 0;     // bitmask of specified mip levels
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // whole image, in recording order
        VkImageView direct_view = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;          // for sampling
        U64 view_serial = 0;                        // research/156 A-DS: as Buffer::gpu_serial, for `view`
        U32 view_levels = 0;
        U32 view_swizzle = 0;
        GLenum target = 0;
        TexFormat format;
        GLenum gl_internal = 0;
        U32 width = 0, height = 0, layers = 1, levels = 1;
        VmaAllocation memory = nullptr;
        VkImageView attach_view = VK_NULL_HANDLE;   // level 0, layer 0, for rendering
        bool attachable = false;
        // research/70: the format could be rendered to, but the image was made
        // without attachment usage (it was given pixels and never attached:
        // such images get no dedicated allocation). make_attachable remakes it.
        bool attach_later = false;
        // Has been attached to a framebuffer: its images are made attachable.
        bool attach_wanted = false;
        // Sampler parameters (GL defaults).
        GLenum min_filter = GL_NEAREST_MIPMAP_LINEAR;
        GLenum mag_filter = GL_LINEAR;
        GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT, wrap_r = GL_REPEAT;
        F32 anisotropy = 1.f;
        GLenum compare_mode = GL_NONE;
        GLenum compare_func = GL_LEQUAL;
        // GL_TEXTURE_BASE_LEVEL / GL_TEXTURE_MAX_LEVEL: LLImageGL limits the chain to the levels it uploads.
        U32 base_level = 0;
        U32 max_level = 1000;
        // GL_TEXTURE_SWIZZLE_*: LLImageGL stores 1-channel fonts as R8 read as (1,1,1,R).
        GLenum swizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
        // Last sampler handed out (plain, shadow) and the parameters it was made from.
        mutable VkSampler sampler_cached[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        // Frame in which something wrote this texture in the frame's main commands:
        // later writes that frame must follow it there (not jump ahead to the uploads).
        U32 late_write_frame = U32_MAX;
        // Has been a framebuffer attachment (uploads into it stay in order with rendering).
        bool rendered_to = false;
        mutable U32 sampler_params[2][8] = {};
        // research/88 (BC7): the viewer let it be stored compressed when its
        // level 0 was last given pixels (LLRHI::tTexCompress); it waits in
        // sBCNQueue; its image is BC7 now, and what the plain one took.
        bool bcn_wanted = false;
        bool bcn_queued = false;
        bool bcn_encoded = false;
        bool bcn_shared = false;    // research/200: the image is shared with the compute queue (the encoder may read it there)
        U64 bcn_plain_bytes = 0;
        U64 bcn_bytes = 0;
        // Bumped by every write into level 0 (note_level0_write; under sLock like
        // the rest of the tracker). A queued BC7 entry remembers it and is
        // skipped when it changed: the image handle alone misses a same-size
        // glTexImage2D/glTexSubImage2D into the same image.
        U32 level0_writes = 0;
        // research/180 stage 1: the GL name it is published under (0: none),
        // for clearing the publication on the texture's own events (a texture
        // does not know its name otherwise). Not read per draw.
        GLuint published_name = 0;
        // research/197 stage 1a: the texture number (TexRef id) whose entry
        // publishes this texture's slot (0: none; one number at a time), for
        // clearing it on the texture's own events. Not read per draw.
        U32 published_ref = 0;
        // texture-leak (2026-10-27): its GL name (NameTable::get), for its
        // bindless slot on its own events (LLVKBindless::forgetTexture: the
        // table's slot, which bindless_slot may not be any more).
        GLuint gl_name = 0;
        // texture-leak: the levels texels were really written into (an upload
        // recorded, a copy, mips from a written level 0, BC7). defined_levels
        // never gets a level this does not have, except for a render target
        // (attach_wanted, rendered_to): what LLRHI_TEX_DEFINED_CHECK verifies.
        U32 written_levels = 0;
        // research/285 X2: the tracker frame it last left shader reading in
        // (transition(): an upload, mips, a copy into it, a read back; U32_MAX:
        // never). The pool compaction leaves textures written lately where
        // they are (media and other textures written every frame).
        U32 write_frame = U32_MAX;
        // research/285 (compact-hysteresis): queued for a smaller image by the
        // viewer (LLRHI::sTextureMoveHold): the pool compaction leaves it.
        bool move_hold = false;
    };

    // texture-leak (2026-10-27): what the "contents only once written" rule
    // did (tex_image, tex_sub_image, generate_mipmaps, tex_scale_down), since
    // the start; LLVKResource::report's VKPerf line. Under sLock.
    struct TexDefinedCounts
    {
        U32 withheld = 0;       // glTexImage2D without pixels on a texture (not a render target): not defined until written
        U32 attached = 0;       // of those, attached to a framebuffer (a render target: defined from then on)
        U32 filled = 0;         // of those, a level later written (sub-image, copy)
        U32 cleared = 0;        // first sub-image into an undefined level not covering it: the level cleared first
        U32 upload_failed = 0;  // pixels given, nothing recorded (staging or conversion failed): not defined
        U32 mips_skipped = 0;   // glGenerateMipmap on an undefined level 0: nothing made
        U32 scaled = 0;         // LLImageGL::scaleDown done by tex_scale_down
        U32 scale_kept = 0;     // tex_scale_down declined (the copy could not be made): the old image kept
        U32 unwritten = 0;      // LLRHI_TEX_DEFINED_CHECK: a view made over a defined level never written
        U32 pbo_images = 0;     // glTexImage2D / glTexSubImage2D from a pixel unpack buffer (not answered: not defined)
    };
    extern TexDefinedCounts sTexDefined;
    // LLRHI_TEX_DEFINED_CHECK=1 (default off): texture_view checks each view
    // it makes against written_levels and logs what it finds.
    bool tex_defined_check();

    // ---- research/180 stage 1: the slot publication (published_slot) ----
    // GL texture name -> bindless slot + 1 while the slot is what resolving
    // the texture again would give (llvkgl_descriptors.cpp; 0: not
    // published). Under sLock. Dense by name, names past 1 << 22 never in it.
    // gen (research/180 stage 4): advanced by every clear of the name's
    // publication, so a record of {slot, gen} (the draw packets' parts) holds
    // only for the publication it was made from, never for a later one of
    // the same name (a name used again for another texture, or published
    // again after a clear).
    struct PublishedSlot
    {
        U32 slot1 = 0;      // slot + 1, 0: not published
        U32 gen = 0;
    };
    extern std::vector<PublishedSlot> sPublishedSlots;
    inline U32 published_slot(GLuint name) { return name < sPublishedSlots.size() ? sPublishedSlots[name].slot1 : 0; }
    inline PublishedSlot published_entry(GLuint name) { return name < sPublishedSlots.size() ? sPublishedSlots[name] : PublishedSlot(); }
    // ---- end of published_slot ----

    // ---- research/197 stage 1a: texture numbers (llvkgl_texref.cpp; tools/selftest sim_texhandles extracts it) ----
    // LLRHI::Textures' table: a number's image (the GL name the viewer's
    // texture has now) and the slot published for it. The publication holds
    // what a name's does (I1..I6, for the image's texture) and also
    //  R1 the entry's image is the texture it was published for (a new image
    //     or the number's retirement clears it),
    //  R2 that texture's published_ref is the number (one at a time).
    // gen is advanced by every clear, never reset (also not when a retired
    // number is given out again), so a record of {slot, gen} (a packet part)
    // holds only for the publication it was made from. Under sLock.
    struct TexRefEntry
    {
        GLuint image = 0;           // 0: none
        PublishedSlot pub;          // slot1 0: not published
        U32 version = 0;            // the texture's version at publication (verify's I6)
        U32 free_frame = 0;         // retired: the frame from which the number may be given out again
        bool live = false;
    };
    extern std::vector<TexRefEntry> sTexRefs;   // by number (0 never given out)
    inline TexRefEntry* texref_entry(U32 id) { return id && id < sTexRefs.size() && sTexRefs[id].live ? &sTexRefs[id] : nullptr; }
    extern bool sTexRefTake;    // this frame: draws take a number's published slot (on, not verify)
    // The publication a draw handle names (DirectDraw::textures, PacketEntry::
    // names): a number's entry while numbers' publications are taken (none
    // while they are only verified), else the GL name's (published_entry).
    inline PublishedSlot published_handle_entry(GLuint handle)
    {
        if (LLRHI::isTexHandle(handle))
        {
            const TexRefEntry* e = sTexRefTake ? texref_entry(LLRHI::texHandleRef(handle).id) : nullptr;
            return e ? e->pub : PublishedSlot();
        }
        return published_entry(handle);
    }
    // The texture lost what its number's publication stands on (the same
    // events as unpublish): the entry's publication goes.
    void unpublish_ref(Texture& tex);
    // ---- end of the texture numbers ----
    // research/198 5.5 (llvkgl_scene.cpp): the scene resolved the number's
    // image to `slot` right now; published as a draw's resolution would be.
    void texref_resolved(U32 id, Texture* tex, U32 slot);
    // research/198 5.5: LLVKScene's binder (llvkgl_scene.cpp; LLVKGL::install).
    void install_scene_binder();
    // research/197 stage 1a: the image a handle names now (a number's image, or the GL name itself).
    inline GLuint handle_image(GLuint handle)
    {
        if (LLRHI::isTexHandle(handle))
        {
            const TexRefEntry* e = texref_entry(LLRHI::texHandleRef(handle).id);
            return e ? e->image : 0;
        }
        return handle;
    }
    // research/197 stage 1a: counters (VKPerf "numbers [...]") and the switch.
    struct TexRefCounts
    {
        U64 created = 0, replaced = 0, retired = 0;
        U64 hits = 0, publishes = 0, clears = 0;            // the publication
        U64 checked = 0, mismatches = 0, breaks = 0;        // verify: the slot the number published vs the usual way, its invariants
        U64 viewer_checked = 0, image_mismatches = 0, unnumbered = 0, sampler_diffs = 0;   // verify: the viewer's numbers
    };
    extern TexRefCounts sTexRefCounts;      // since the last report
    extern TexRefCounts sTexRefTotals;      // since the switch was last turned on (the auto mode reads it)
    extern bool sTexRefVerify;              // this frame: compare instead of taking (sTexRefTake above)
    // Once a frame (slot_publish_frame): the switch (and its auto mode) for
    // this frame. True: numbers are published this frame (on or verify).
    bool texref_frame();
    // Every publication of a number goes (the publication starts over).
    void texref_clear();
    // Verify: every number's publication's invariants (I1..I6, R1, R2).
    void texref_check_all();
    void install_texref();                  // LLRHI::Textures' backend
    std::string texref_report(U32 frames);
    void texref_reset_counters();
    // The publication this frame (set at the frame's first gather_slots):
    // 0 off (or not set up yet this frame), 1 names published but draws do
    // not take them (verify), 2 draws take published slots (on).
    U32 slot_publish_mode();
    // Every event that may make the publication untrue clears it, whether or
    // not the switch is on: the texture's version, view, image, layout (away
    // from shader reading), attachment, deletion, and its slot leaving it.
    void unpublish(Texture& tex);
    void unpublish_name(GLuint name);
    // The VKPerf "texture slots" line's "published [...]" (empty while it never ran).
    std::string slot_publish_report(U32 frames);
    void slot_publish_reset_counters();
    // research/218 vk-s7f: gather_slots' old paths (lookups by GL name, direct
    // draws by GL name) since the last call, totals; starts them over. Read
    // before perfReport (it starts the publication's hits over).
    std::string old_texture_path_report();

    // Level 0 of tex is being written: a BC7 queue entry made before is stale,
    // and the texture may be queued again once its mips are made.
    inline void note_level0_write(Texture& tex)
    {
        ++tex.level0_writes;
        tex.bcn_queued = false;
    }

    extern NameTable<Texture> sTextures;
    extern thread_local U32 sUnitBindings[32];      // bumped when a unit's binding changes

    // Bindings of one texture unit, per target kind. The Texture* is resolved at
    // bind time and re-resolved after any texture deletion.
    struct UnitBindings
    {
        GLuint names[6] = {};
        Texture* textures[6] = {};
        U32 resolved_at[6] = {};    // sTextureDeletes when textures[] was looked up
    };
    extern thread_local UnitBindings sUnits[32];

    inline int target_slot(GLenum target)
    {
        switch (target)
        {
        case GL_TEXTURE_2D: return 0;
        case GL_TEXTURE_CUBE_MAP: return 1;
        case GL_TEXTURE_CUBE_MAP_ARRAY: return 2;
        case GL_TEXTURE_2D_ARRAY: return 3;
        case GL_TEXTURE_3D: return 4;
        default: return 5;
        }
    }

    inline Texture* unit_texture(U32 unit, GLenum target)
    {
        UnitBindings& u = sUnits[unit];
        int slot = target_slot(target);
        if (!u.names[slot])
        {
            return nullptr;
        }
        // Also while nothing was found: a deleted name left on the unit
        // (glDeleteTextures) finds its next texture once that is made.
        if (u.resolved_at[slot] != sTextureDeletes || !u.textures[slot])
        {
            u.textures[slot] = sTextures.find(u.names[slot]);
            u.resolved_at[slot] = sTextureDeletes;
        }
        return u.textures[slot];
    }

    extern thread_local GLint sUnpackAlignment;     // the GL unpack state (glPixelStorei, LLRHI::sPixelStorei)
    extern thread_local GLint sUnpackRowLength;
    extern thread_local GLint sPackAlignment;       // glReadPixels' pack state
    extern thread_local GLint sPackRowLength;

    VkFormatFeatureFlags format_features(VkFormat format);
    void transition(VkCommandBuffer cmd, Texture& tex, VkImageLayout to);
    // LLRHI_VK_CHECKPOINTS diagnostics (llvkgl_texture.cpp): views queued for
    // destruction, and the last frames' generic draws (llvkgl_draw.cpp records them).
    extern std::unordered_map<VkImageView, U32> sDestroyedViews;
    struct DrawRecord
    {
        U32 frame;
        U32 draw;
        GLuint program;
        U32 count;
        U32 slots[4];
        GLuint names[4];
        VkImageView views[4];
    };
    extern std::vector<DrawRecord> sDrawLog;
    bool is_cube_face(GLenum target);
    void restore_plain(Texture& tex);
    // False: nothing recorded (no staging memory). clear_first: the level
    // and layer are cleared to zero before the copy (texture-leak 2026-10-27).
    bool upload(Texture& tex, U32 level, U32 layer, S32 x, S32 y, U32 width, U32 height, const void* data, size_t size,
                bool clear_first = false);
    Texture* bound_texture(GLenum target);
    VkImageView texture_view(Texture& tex);
    VkSampler sampler_for(const Texture* tex, bool shadow);
    VkCompareOp compare_op(GLenum func);
    Texture* dummy_texture(VkImageViewType type, bool depth);

    // What the GL entry points and the native texture API do to textures (llvkgl_texture.cpp).
    extern std::atomic<U32> sTextureVersions;      // Texture::version values (LLRHI::sBackendCounters)
    extern thread_local GLuint sActiveUnit;
    extern U32 sTextureUploads, sTextureUploadsEarly;
    // research/137 N14: texture data calls (tex_image, tex_sub_image,
    // generate_mipmaps, from the GL wrappers and the native hooks alike) and
    // their time on the calling thread, for the VKPerf line
    // (LLVKResource::report). Pixel conversion handed to the texture threads
    // (research/70) is not in it. Under sLock.
    struct TextureDataCounts
    {
        U32 images = 0;         // tex_image calls
        U32 new_images = 0;     // Vulkan images made for a texture (ensure_image)
        U32 sub_images = 0;     // tex_sub_image calls
        U32 mip_chains = 0;     // generate_mipmaps calls
        U64 ticks = 0;          // __rdtsc, LLRHI_VK_PERF only
    };
    extern TextureDataCounts sTextureData;
    void destroy_texture_objects(Texture& tex);
    void make_attachable(Texture& tex);
    void touch_texture(Texture* tex);
    void touch_bound(GLenum target);
    void tex_image(Texture* tex, GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLenum format, GLenum type,
                   const void* pixels, bool compressed, size_t compressed_size);
    void tex_sub_image(Texture* tex, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                       GLenum type, const void* pixels);
    void tex_image_3d(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                      GLenum type, const void* pixels);
    // research/288: blocks into layers of a compressed 2D array (llvkgl_texture.cpp).
    void tex_compressed_sub_image_3d(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height,
                                     GLsizei depth, GLenum format, GLsizei size, const void* data);
    void tex_sub_image_3d(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth,
                          GLenum format, GLenum type, const void* pixels);
    void generate_mipmaps(Texture* tex);
    bool tex_parameter(GLenum target, GLenum pname, F32 value);
    // research/88: textures stored BC7.
    void queue_for_bcn(GLuint name, Texture& tex);
    void compress_queued_textures();
    void native_texture_compressed(const U32* names, U32 count, bool* out);
    void native_texture_move_hold(U32 name, bool hold);    // LLRHI::sTextureMoveHold (Texture::move_hold)
    // research/285 X2 (llvkgl_texture.cpp): a texture's texels moved into a
    // new image of its format on the GPU, in place of the one it has. Under
    // sLock, in this order for each texture (several may share each step):
    //   make_moved_image   the new image, for tex's levels from first_level on
    //                      (that level becomes level 0: 0 keeps the size);
    //                      where: as LLVKDevice::createImage's (ExistingPoolBlocks:
    //                      only in a block the texture pool of its size has).
    //                      False: none made (image_movable says no, or no memory).
    //   drop_moved_image   one made and not wanted after all: freed at once
    //                      (nothing may have been recorded with it).
    //   record_image_moves the copies (the defined levels) into cmd, outside a
    //                      rendering scope: the frame's upload commands for a
    //                      texture not written yet this frame, else the frame's
    //                      main commands (LLVKMirror::transferCommands; later
    //                      writes then follow it there). Its barriers are
    //                      recorded at once: not inside an LLVKMirror::
    //                      BarrierBatch open on cmd. False: nothing recorded
    //                      (no command buffer): drop the images, take none.
    //   take_moved_image   the texture names the new image; the old one goes
    //                      once the GPU is done with what was recorded so far (a
    //                      new version: views, bindless slots, publications of
    //                      it go). A texture stored BC7 stays BC7.
    struct ImageMove
    {
        Texture* tex = nullptr;
        VkImage image = VK_NULL_HANDLE;     // the new image
        VmaAllocation memory = nullptr;
        U32 first_level = 0;
    };
    // A plain 2D texture with contents in shader reading, not a render target,
    // not waiting for a BC7 encoding nor read by one on the compute queue.
    bool image_movable(const Texture& tex);
    bool make_moved_image(Texture& tex, U32 first_level, LLVKDevice::ImageMemory where, ImageMove& move);
    void drop_moved_image(ImageMove& move);
    bool record_image_moves(VkCommandBuffer cmd, const ImageMove* moves, U32 count);
    void take_moved_image(const ImageMove& move);
    // research/285 X2 (llvkgl_texcompact.cpp, LLRHI::sTexCompact): at the start
    // of a frame, under sLock, while VRAM is short, the textures of the
    // texture pool blocks least in use move into fuller blocks of their pool.
    // True: it looked over the textures this frame. compact_textures_due,
    // without sLock on the same thread first: whether a check is due at all
    // (the switches and the time of the next check; no lock taken while not).
    bool compact_textures_due();
    bool compact_textures();
    // research/245, 285 (compact-hysteresis; llvkgl_texcompact.cpp): when the
    // GL buffer and the texture pool compactions run and how far, one state
    // for both (the frame's thread, under sLock). They start at 92% of the
    // driver's VRAM budget, or, while the discard bias or the texture pressure
    // is up, at 128 MB under the use where those come down
    // (LLRHI::sVRAMBiasFallBytes) if that is lower. Once started they go on
    // until VRAM in use is 64 MB under the start; while the bias or the
    // pressure is up, until it is under 85% of the budget or 64 MB under
    // their start line, the lower (a pass frees at most its limits: stopping
    // where they started left VRAM between there and where the bias comes
    // down, the bias held at 4). They also stop once each of them rested in
    // turn (nothing to give back, or passes without progress): the next start
    // is at the start line again. With the budget at its floor on a GPU that
    // shares the PC's memory, no state: each check on its own.
    // Both compactions' pace: a check this often while VRAM is short, a rest
    // after one that found nothing to do (or passes without progress), and
    // the frames after a pass before the next (what it gave back is freed
    // once the frames in flight are done: the next sees the blocks as then).
    constexpr F64 COMPACT_CHECK_SECONDS = 0.5;
    constexpr F64 COMPACT_REST_SECONDS = 10.0;
    constexpr U32 COMPACT_PASS_FRAMES = LLVKMirror::FRAMES_IN_FLIGHT + 2;
    struct CompactCheck
    {
        U64 usage = 0;      // VRAM in use (LLVKDevice::vramBudget)
        U64 budget = 0;     // the driver's budget for it
        U64 start = 0;      // the use the compactions start at
        U64 stop = 0;       // ... and run down to
        U64 fall = 0;       // the use the discard bias comes down under, as taken (0: not used)
        U64 to_free = 0;    // usage - stop (less what the last passes freed)
        bool compacting = false;    // the compactions run (to_free may still be 0 for now)
    };
    // A check of a compaction: true when it is to run now (out filled;
    // to_free less what the passes of the last frames freed and the GPU has
    // not given back yet, so the next check of either does not ask for it again).
    bool vram_compaction_check(CompactCheck& out);
    // A pass freed about `bytes` (its estimate), given back once the frames in
    // flight are done. Until then VMA keeps no empty block
    // (LLVKDevice::setTrimEmptyBlocks; not with LLRHI_VK_EMPTY_TRIM=0).
    void vram_compaction_freed(U64 bytes);
    // Every frame (LLVKGL::resetFrameStats, the frame's thread, recorded or
    // not): VMA keeps its empty block again once the passes' frees are done.
    // A flag test while none is pending.
    void vram_compaction_tick();
    enum CompactKind : U8
    {
        COMPACT_BUFFERS = 1,
        COMPACT_TEXTURES = 2,
    };
    // A check of `kind` while compacting ended in a rest (true: nothing to
    // give back, or passes without progress) or in a pass (false). Once every
    // compaction that runs rested in turn, the state ends.
    void vram_compaction_rested(CompactKind kind, bool rested);
    // Neither compaction can run any more (switched off, the device lost, the
    // viewer quitting): the state goes and VMA keeps its empty block again.
    // The frame's thread (no lock needed).
    void vram_compaction_stop();
    // research/245: LLRHI_VK_BUFFER_COMPACT=0 keeps the GL buffer compaction
    // off whatever the setting (safe mode: 0).
    inline bool buffer_compaction_allowed()
    {
        static const bool allowed = LLRHI::envDefaultOn("LLRHI_VK_BUFFER_COMPACT");
        return allowed;
    }
    // Neither runs once the device is lost or the viewer quits (nothing is
    // given back or copied again then).
    inline bool vram_compaction_halted()
    {
        return LLRHI::sQuitting.load(std::memory_order_relaxed) || LLVKDevice::instance().isLost();
    }

    // ------------------------------------------------------------------
    // Framebuffers (per context, like GL; llvkgl_entry.cpp)

    // The draw framebuffer as Vulkan attachments (resolve_draw_target).
    struct ResolvedTarget
    {
        LLVKMirror::Target target;
        Texture* colors[4] = {};        // FBO attachments to keep in attachment layout
        U32 color_textures = 0;
        Texture* depth = nullptr;
        bool is_default = false;
        // The same for the same resolution (research/154 W1, W2): changes
        // whenever the resolved attachments change, never 0 once resolved.
        U32 stamp = 0;
    };

    struct Framebuffer
    {
        GLuint color[4] = { 0, 0, 0, 0 };
        GLuint depth = 0;
        GLenum draw_buffers[4] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE };
        U32 draw_count = 1;
        GLenum read_buffer = GL_COLOR_ATTACHMENT0;
        // resolve_draw_target's last resolution of it (research/154 W1): made
        // at this sTargetState in this frame; resolved again after either
        // moved, keeping its stamp when the result is the same.
        U32 resolved_state = U32_MAX;
        U32 resolved_frame = U32_MAX;
        bool resolved_ok = false;
        ResolvedTarget resolved;
    };

    extern thread_local std::unordered_map<GLuint, Framebuffer> sFramebuffers;

    // ------------------------------------------------------------------
    // Fixed-function state (per context; llvkgl_entry.cpp)

    struct Stencil
    {
        GLenum func = GL_ALWAYS;
        GLint ref = 0;
        GLuint compare_mask = 0xffffffff;
        GLuint write_mask = 0xffffffff;
        GLenum fail = GL_KEEP, depth_fail = GL_KEEP, pass = GL_KEEP;
    };

    struct State
    {
        bool blend = false;
        GLenum blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO, blend_src_a = GL_ONE, blend_dst_a = GL_ZERO;
        GLenum blend_eq_rgb = GL_FUNC_ADD, blend_eq_a = GL_FUNC_ADD;
        bool color_mask[4] = { true, true, true, true };
        bool cull = false;
        GLenum cull_face = GL_BACK;
        GLenum front_face = GL_CCW;
        bool depth_test = false;
        bool depth_write = true;
        GLenum depth_func = GL_LESS;
        bool scissor = false;
        GLint scissor_rect[4] = { 0, 0, 0, 0 };
        GLint viewport[4] = { 0, 0, 0, 0 };
        bool polygon_offset = false;
        bool depth_clamp = false;       // shadow passes keep casters outside the depth range
        F32 offset_factor = 0.f, offset_units = 0.f;
        GLenum polygon_mode = GL_FILL;
        F32 line_width = 1.f;
        bool stencil = false;
        Stencil stencil_front, stencil_back;
        GLuint draw_framebuffer = 0;
        GLuint read_framebuffer = 0;
        F32 clear_color[4] = { 0.f, 0.f, 0.f, 0.f };
        F32 clear_depth = 1.f;
        GLint clear_stencil = 0;
    };

    extern thread_local State sState;

    // State setters the GL wrappers (llvkgl_entry.cpp) and the native state API
    // (llvkgl_hooks.cpp) share; inline: the native calls are many a frame. Every
    // write to sState that a draw reads must move sPipelineState or sFixedState
    // (set_cap, llvkgl_entry.cpp).
    inline void set_depth_func(GLenum func)
    {
        sState.depth_func = func;
        ++sFixedState;
    }
    inline void set_depth_mask(bool write)
    {
        sState.depth_write = write;
        ++sFixedState;
    }
    // Shared by the wrappers and the native API (research/39 N4).
    inline void set_blend_func(GLenum src_rgb, GLenum dst_rgb, GLenum src_a, GLenum dst_a)
    {
        sState.blend_src_rgb = src_rgb; sState.blend_dst_rgb = dst_rgb; sState.blend_src_a = src_a; sState.blend_dst_a = dst_a;
        ++sPipelineState;
    }
    inline void set_color_mask(bool r, bool g, bool b, bool a)
    {
        sState.color_mask[0] = r; sState.color_mask[1] = g; sState.color_mask[2] = b; sState.color_mask[3] = a;
        ++sPipelineState;
    }
    inline void set_cull_face(GLenum mode)
    {
        sState.cull_face = mode;
        ++sFixedState;
    }
    // The stencil reference values only (glStencilFuncSeparate's ref): a draw
    // list's State::stencil_ref (research/137 N7).
    inline void set_stencil_ref(GLint front, GLint back)
    {
        sState.stencil_front.ref = front;
        sState.stencil_back.ref = back;
        ++sFixedState;
    }
    inline void set_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
    {
        sState.viewport[0] = x; sState.viewport[1] = y; sState.viewport[2] = w; sState.viewport[3] = h;
        ++sFixedState;
    }
    inline void set_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
    {
        sState.scissor_rect[0] = x; sState.scissor_rect[1] = y; sState.scissor_rect[2] = w; sState.scissor_rect[3] = h;
        ++sFixedState;
    }
    inline void set_polygon_offset(GLfloat factor, GLfloat units)
    {
        sState.offset_factor = factor; sState.offset_units = units;
        ++sFixedState;
    }
    inline void set_polygon_mode(GLenum mode)
    {
        sState.polygon_mode = mode;
        ++sPipelineState;
    }
    inline void set_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
    {
        sState.clear_color[0] = r; sState.clear_color[1] = g; sState.clear_color[2] = b; sState.clear_color[3] = a;
    }

    // ------------------------------------------------------------------
    // Render targets (llvkgl_pipeline.cpp)

    bool resolve_draw_target(ResolvedTarget& out);
    inline bool is_attachment_of(const ResolvedTarget& t, const Texture* tex)
    {
        for (U32 i = 0; i < t.color_textures; ++i)
        {
            if (t.colors[i] == tex)
            {
                return true;
            }
        }
        return t.depth == tex;
    }

    // ------------------------------------------------------------------
    // GLSL rewrites (llvkgl_glsl.cpp, llvkgl_glslbindless.cpp)

    // One rewritten material uniform: `count` elements of `components`
    // floats, element e in the row's vec4 first + e.
    struct MaterialMember
    {
        std::string name;
        U32 components = 1;
        U32 count = 1;
        U32 first = 0;
    };

    // The shared pipeline layout (research/50 step 3): what a marked G-buffer
    // program still had in its default uniform block after the table rewrites
    // (the mirror clip, the rigged projection, the terrains' texture planes and
    // factors) is read from a "constants row" of the frame's table instead.
    // Wanted: a live member of the default block as reflected (name, GL type,
    // array size). Member: its place in the row, every element `columns`
    // vec4s from vec4 first + element * columns (a column per vec4, `rows`
    // components of it; scalars and vectors are one column).
    struct ConstWanted
    {
        std::string name;
        U32 gl_type = 0;
        U32 size = 1;
    };

    enum ConstKind { CONST_FLOAT, CONST_INT, CONST_UINT, CONST_BOOL };

    struct ConstType
    {
        const char* name;
        U32 gl_type;
        ConstKind kind;
        U32 rows;
        U32 columns;
    };

    struct ConstMember
    {
        std::string name;
        const ConstType* type = nullptr;
        U32 count = 1;
        bool array = false;
        U32 first = 0;
    };

    // Matrix uniforms the shaders read from the draw matrix table (research/39 N1),
    // in LLVKBindless::DrawMatrices order.
    enum MatrixKind { MATRIX_MODELVIEW, MATRIX_MVP, MATRIX_TEXTURE0, MATRIX_NORMAL, MATRIX_KINDS };
    const char* const MATRIX_NAMES[MATRIX_KINDS] = { "modelview_matrix", "modelview_projection_matrix", "texture_matrix0", "normal_matrix" };

    // GLSL source as identifier and punctuation tokens, comments skipped. Enough
    // to find declarations and parameters, and far faster than std::regex, which
    // took minutes over the viewer's ~600 programs.
    struct GlslToken
    {
        size_t begin;
        size_t end;
        bool ident;
    };

    // The GLSL rewrites link_program applies (llvkgl_glsl.cpp, llvkgl_glslbindless.cpp).
    void tokenize_glsl(const std::string& text, std::vector<GlslToken>& tokens);
    std::vector<std::string> bindless_rewrite(std::vector<LLVKShader::Source>& sources, U32& matrix_mask,
                                              U32& palette_columns, U32& palette_elements, bool allow_lights, bool& light_table,
                                              bool allow_materials, std::vector<MaterialMember>& materials,
                                              const std::vector<ConstWanted>& consts_wanted, std::vector<ConstMember>& consts);
    bool draw_rows_rewrite(std::vector<LLVKShader::Source>& sources, U32 samplers, bool matrix, bool palette, bool light,
                           bool material, bool consts);
    bool draw_runs_marked(const std::vector<LLVKShader::Source>& sources);
    bool alpha_fast_rewrite(std::vector<LLVKShader::Source>& sources);

    // ------------------------------------------------------------------
    // The compile of a link (llvkgl_linkbuild.cpp, research/111 B-SC, B-PW)

    // What link_program builds for a program: its GL sources rewritten for
    // the backend (the tables, draw rows, early outs) and built, with every
    // fallback and the shared layout's second build. No program state is
    // touched, so a compile worker can run it ahead of the link (Modules::Skip:
    // only the SPIR-V cache is filled, nothing is logged).
    struct LinkBuild
    {
        std::vector<LLVKShader::Source> sources;        // in: as GL has them; out: as built (or as last tried)
        std::vector<LLVKShader::Source> unrewritten;    // out: before the table rewrites
        LLVKShader::Program vk;
        bool built = false;
        std::vector<std::string> bindless_names;
        U32 matrix_mask = 0;
        U32 palette_columns = 0;
        U32 palette_elements = 0;
        bool light_table = false;
        bool draw_rows = false;
        U32 row_words = 0;
        bool shared = false;            // its set 1 went into the constants row (the shared layout)
        std::vector<MaterialMember> materials;
        std::vector<ConstMember> consts;
    };
    // id (or name, when given: research/250's background builds have no GL
    // name of their own) names the program in the warnings.
    void compile_link(GLuint id, LinkBuild& link, LLVKShader::Modules modules, const char* name = nullptr);

    // A parallel shader reload's first pass (LLRHI::sParallelShaderReload):
    // the link's compile goes to the workers and the program stays unbuilt
    // (the second pass links it for real). True: handed over. Under sLock.
    bool link_planned(GLuint id);
    // True during that first pass (no link builds then). Under sLock.
    bool link_planning();
    // True during both passes of a parallel reload: it logs its own build
    // counts (draw_run_programs leaves them to it). Any thread.
    bool parallel_reload_running();
    // research/250: in the background reload's commit (LLRHI::ShaderReloadBuilder,
    // between commitBegin and commitEnd), the build made in the background from
    // the same GL sources goes into link (link_program then skips compile_link:
    // it is what compile_link makes of them). False: none (outside the commit,
    // or not made in the background). Main thread, under sLock.
    bool take_background_build(LinkBuild& link);
    inline LLVKShader::Stage shader_stage(GLenum type)
    {
        return type == GL_VERTEX_SHADER ? LLVKShader::Stage::Vertex :
               type == GL_FRAGMENT_SHADER ? LLVKShader::Stage::Fragment :
               type == GL_GEOMETRY_SHADER ? LLVKShader::Stage::Geometry :
               LLVKShader::Stage::Compute;
    }
    // The fragment file with main, as GL has it: names the program the same
    // way in every session and under every preset (its OIT variant is
    // prewarmed when one was made before). 0: none or several.
    U64 main_fragment_hash(const std::vector<LLVKShader::Source>& sources);
    // research/160 P1: the same GL program under every preset (its pipeline
    // keys carry over to its next link): the files with main whole, the
    // feature libraries without their #define lines (the global defines),
    // and the stages. Differs between GPU vendors (IS_AMD_CARD). 0: no main.
    U64 program_identity(const std::vector<LLVKShader::Source>& sources);
    // An OIT variant was made for the program (oit_variant): remembered on
    // disk for the prewarm.
    void note_oit_variant(U64 main_fragment);
    // LLRHI::sParallelShaderReload, sPrewarmShaders and sBakeShaders (LLVKGL::install).
    void install_link_hooks();
    // research/159 3.1 (--bake-shaders): the shaders OIT and the occlusion
    // marks make on their first use, made now (their own set-up). False: it
    // failed, or the GPU cannot do it. Takes sLock.
    bool bake_oit_shaders();
    bool bake_occlusion_shaders();
    // fix-threads4: the shader prewarm's threads end (LLVKGL::stopPrewarm):
    // its queue goes, each finishes the compile it runs, nothing starts again.
    void stop_shader_prewarm();
    bool is_sampler_type(U32 type);

    // ------------------------------------------------------------------
    // Programs (llvkgl_program.cpp)

    struct UniformLocation
    {
        S32 uniform = -1;   // index into vk.uniforms
        S32 element = 0;    // array element the location starts at
        S32 bindless = -1;  // index into Program::bindless (a rewritten 2D sampler)
        S32 matrix = -1;    // MatrixKind of a matrix read from the draw matrix table
        bool palette = false;   // the skinning palette, read from the palette table (element: as above)
        S32 material = -1;  // index into Program::material_members (a uniform read from the material row)
        S32 consts = -1;    // index into Program::consts_members (a uniform read from the constants row)
    };
    LLVKBindless::DrawMatrices identity_matrices();

    // One sampler binding of a program, resolved at link time.
    struct SamplerSlot
    {
        S32 binding = -1;
        U32 count = 1;
        U32 first = 0;              // index into Program::unit_values
        GLenum target = GL_TEXTURE_2D;
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
        bool shadow = false;
        std::string name;
    };

    struct PassCache;   // the pass cache's entry of a program (llvkgl_drawpath.h)

    // A program's order-independent transparency variant (research/41, oit_variant):
    // its shaders write list nodes instead of color. Made on the program's
    // first draw in an OIT session; layout: set 0 and 1 as the program's (an
    // empty set 1 on the shared layout), set 2 the lists, the same push constants.
    struct OITVariant
    {
        bool ok = false;
        LLVKShader::Program vk;     // its fragment module only (the other stages are the program's)
        VkShaderModule fragment = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        // research/205 3a-2: the row word a draw reading rows takes its
        // priority from (oit_row_word; U32_MAX: always the push constant).
        // Such a variant's draws never join a draw run (whose rows have no
        // priority word).
        U32 row_priority_word = U32_MAX;
    };

    struct Program
    {
        bool built = false;
        bool failed = false;
        LLVKShader::Program vk;
        std::unordered_map<std::string, GLuint> attrib_bindings;    // glBindAttribLocation
        std::vector<S32> input_attribs;                             // per vk.inputs: GL attribute index or -1
        std::unordered_map<GLint, UniformLocation> locations;
        // Native shader API slots (research/39 N4): copies of `locations` entries,
        // handed out by LLRHI::sUniformSlot; cleared with `locations` on relink.
        std::vector<UniformLocation> native_slots;
        std::unordered_map<GLint, S32> native_slot_of;              // GL location -> slot
        // Matrices read from the draw matrix table: bit per MatrixKind. GL sets
        // them as uniforms; they are kept here (per program, as GL keeps
        // uniforms) and written to a table row when a draw needs them.
        U32 matrix_mask = 0;
        LLVKBindless::DrawMatrices matrices = identity_matrices();
        U32 matrix_version = 0;
        U32 matrix_row_frame = U32_MAX;
        U32 matrix_row_version = U32_MAX;
        U32 matrix_row = 0;
        // The skinning palette (matrixPalette) read from the palette table
        // (research/42 C2): kept here as GL keeps the uniform, copied into the
        // frame's table when a draw needs it after a change. 0 columns: not
        // rewritten (the palette, if any, stays in the default block).
        U32 palette_columns = 0;    // vec4s per element: 3 (mat3x4) or 1 (vec4)
        U32 palette_elements = 0;
        std::vector<F32> palette;   // palette_elements * palette_columns vec4s
        // vec4s a table copy takes: up to the end of the writes since the last
        // copy (uploads start at element 0; a mesh reads only the joints it uploaded).
        U32 palette_extent = 0;
        bool palette_written = false;   // written since the last copy
        U32 palette_version = 0;
        U32 palette_row_frame = U32_MAX;
        U32 palette_row_version = U32_MAX;
        U32 palette_row_extent = 0;     // vec4s the row at palette_base holds
        U32 palette_base = 0;
        // LLRHI::sUsePaletteRow (research/56): the draws read this row of the
        // frame's table (palette_prebuilt_frame's) instead of `palette`, until
        // the palette uniform is set again (apply_palette).
        bool palette_prebuilt = false;
        U32 palette_prebuilt_frame = U32_MAX;
        U32 palette_prebuilt_base = 0;
        // The deferred point light read from the light table (research/47):
        // a draw's DirectDraw::table_base goes out after the palette's push
        // constant (llrhi_slots.light); NO_TABLE draws read the uniforms.
        bool light_table = false;
        // Material uniforms read from the frame's table (research/46 G4a): kept
        // here as GL keeps uniforms, one vec4 per element (material_vec4s), and
        // copied into a row (shared by content) when a draw needs them after a
        // change. Their changes leave the default block, and so its copy and
        // the pass cache, alone. Empty: not rewritten.
        std::vector<MaterialMember> material_members;
        U32 material_vec4s = 0;
        std::vector<F32> material;
        U32 material_version = 0;
        U32 material_row_frame = U32_MAX;
        U32 material_row_version = U32_MAX;
        U32 material_base = 0;      // U32_MAX: did not fit in the table this frame
        // The shared pipeline layout (research/50 step 3): a marked program
        // with nothing in set 1 once its live default-block uniforms read a
        // row of the frame's table (the constants row, kept like the
        // material: consts_members' layout, written when a value changed,
        // shared by content). Its layout is sSharedLayout (set 0 and the push
        // constants, no set 1), the same object for every such program.
        bool shared_layout = false;
        std::vector<ConstMember> consts_members;
        U32 consts_vec4s = 0;
        std::vector<F32> consts;
        U32 consts_version = 0;
        U32 consts_row_frame = U32_MAX;
        U32 consts_row_version = U32_MAX;
        U32 consts_base = 0;        // U32_MAX: did not fit in the table this frame
        // Draw runs (research/46 G4b): the viewer marked the program
        // (LLRHI_DRAW_ROWS), so consecutive draws of it may go out as one
        // indirect draw from the attribute pool (run_draw). With draw_rows its
        // shaders read the push constants from a row of the frame's table
        // (draw_rows_rewrite): draw_row_words words, the rows word pushed
        // after them. Without, only draws with the same push constants share a run.
        bool draw_runs = false;
        bool draw_rows = false;
        U32 draw_row_words = 0;
        U32 run_capacity = 64;      // indirect commands a new run reserves (follows how long its runs get)
        S32 default_block = -1;
        std::vector<U8> uniform_data;                               // default uniform block contents
        std::vector<SamplerSlot> sampler_slots;
        std::vector<GLint> unit_values;                             // texture unit per sampler element
        size_t image_count = 0;
        // Bumped when a sampler's texture unit (unit_values, bindless units) changes.
        U32 unit_version = 0;
        bool buffer_blocks = false;     // has uniform blocks backed by GL buffers
        // The default block changes only through glUniform*: copy it once per change per frame.
        U32 uniform_version = 0;
        U32 ubo_version = U32_MAX;
        U32 ubo_frame = U32_MAX;
        VkBuffer ubo_buffer = VK_NULL_HANDLE;
        VkDeviceSize ubo_offset = 0;
        // Last descriptor set written this frame and what it holds.
        U32 last_frame = U32_MAX;
        std::vector<U64> last_key;
        VkDescriptorSet last_set = VK_NULL_HANDLE;
        std::vector<S32> block_point;                               // per vk.blocks: GL binding point or -1
        // Last pipeline and what it was chosen for.
        VkPipeline last_pipeline = VK_NULL_HANDLE;
        U32 last_pipeline_state = U32_MAX;
        U32 last_pipeline_target = 0;
        U32 last_pipeline_prim = U32_MAX;
        U64 last_pipeline_inputs = 0;
        // Sampled images of the last gather, reused while no texture state, target or depth state changed.
        U32 images_texture_state = U32_MAX;
        U64 images_units = 0;       // the bindings of the units the images come from
        U32 images_target_stamp = U32_MAX;
        U32 images_frame = U32_MAX;
        U32 images_depth_state = U32_MAX;
        bool images_depth_read_only = false;
        std::vector<VkDescriptorImageInfo> images;
        std::vector<Texture*> sampled;
        std::vector<U64> image_key;
        U32 generic_draws = 0;      // draws through the generic path since the last perf report
        // Its last draw through the full path, for the pass cache (made on first use).
        std::shared_ptr<PassCache> pass;
        // Default block offsets of the matrices direct draws bring (-1: not used),
        // resolved again after any program change.
        struct DirectUniforms
        {
            S32 modelview = -1;
            S32 modelview_projection = -1;
            S32 normal = -1;
            S32 inverse_modelview = -1;
            S32 texture_matrix0 = -1;
        };
        DirectUniforms direct;
        U32 direct_resolved_at = U32_MAX;
        // The last direct draw's copy of the default block, for direct draws with
        // the same uniforms and matrices.
        U32 direct_ubo_frame = U32_MAX;
        U32 direct_ubo_version = U32_MAX;
        U32 direct_ubo_matrix_id = 0;
        const F32* direct_ubo_texture_matrix = nullptr;
        VkBuffer direct_ubo_buffer = VK_NULL_HANDLE;
        VkDeviceSize direct_ubo_offset = 0;
        // Descriptor sets written this frame, by contents (sets live in the frame's pools):
        // the hash of the contents -> set_cache_entries. Emptied once a frame without
        // freeing (fix-pipeleak: an unordered_map freed and made every entry again).
        U32 set_cache_frame = U32_MAX;
        LLVKFrameTable set_cache;
        std::vector<std::pair<std::vector<U64>, VkDescriptorSet>> set_cache_entries;    // the first set_cache.size() are this frame's
        // research/156 A-DS (LLRHI_SET_PERSIST=1): sets kept across frames, by
        // contents (with the serials of the buffers and views they hold), in
        // the mirror's persistent pools: the hash -> persist_entries. Emptied
        // when the pools are retired (sPersistGeneration) or the program linked.
        U32 persist_generation = U32_MAX;
        U32 persist_count = 0;          // the first persist_count entries are current
        U32 persist_last = U32_MAX;     // the entry the last draw used
        LLVKFrameTable persist_table;
        std::vector<std::pair<std::vector<U64>, VkDescriptorSet>> persist_entries;
        S32 depth_only_mvp = -2;    // MVP offset when usable by the depth-only path, -1 not usable, -2 unchecked,
                                    // DEPTH_ONLY_MVP_FROM_TABLE: the program's matrices (the draw matrix table)
        // Occlusion box programs (position * box_size + box_center): their offsets, else -1.
        S32 depth_only_box_center = -1;
        S32 depth_only_box_size = -1;
        // globalF.glsl's mirror clip (mirror_flag, clipPlane, clipSign): the path
        // only takes the program while mirror_flag is 0.
        S32 depth_only_mirror_flag = -1;
        // 2D samplers rewritten to index the bindless table (llvkbindless.h): the
        // GL texture unit set with glUniform1i, and where the slot number goes.
        // The slot numbers reach the shader as push constants, in this order.
        struct BindlessSampler
        {
            std::string name;
            GLint unit = 0;
            // The last resolution through the unit's binding, reused while the
            // binding, textures, render target and frame stay the same.
            U32 cached_frame = U32_MAX;
            GLint cached_unit = -1;
            U32 cached_binding = 0;
            const Texture* cached_bound = nullptr;     // the unit's texture (also when not ready)
            U32 cached_version = 0;
            U32 cached_target = 0;
            U32 cached_slot = 0;
            GLuint cached_name = 0;
            VkImageView cached_view = VK_NULL_HANDLE;
            Texture* cached_texture = nullptr;  // null: the stand-in
        };
        std::vector<BindlessSampler> bindless;
        std::vector<U32> bindless_slots;    // of the last draw (diagnostics)
        std::unordered_map<GLuint, std::string> block_names;        // GL block index -> name
        std::unordered_map<std::string, GLuint> block_points;       // name -> GL binding point
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        U32 link_serial = 0;    // which link (sPipelines key): the layout may be shared (sSharedLayout)
        // fix-pipeleak: this link's entries in sPipelines (their keys, which stay
        // where they are while the entry exists: unordered_map never moves its
        // elements). destroy_program_objects takes them out and destroys the
        // pipelines once the frames using them are done; a new link's keys never
        // meet them again (link_serial), so they used to stay for good.
        std::vector<const std::vector<U32>*> pipeline_keys;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        // Order-independent transparency (research/41): the sources as built
        // (kept while LLRHI::sOITSources), its OIT variant once tried (null: not yet),
        // and whether last_pipeline is the variant's.
        std::vector<LLVKShader::Source> oit_sources;
        std::shared_ptr<OITVariant> oit;
        U8 last_pipeline_oit = 0;          // OITDrawMode
        U64 main_fragment = 0;             // main_fragment_hash of its GL sources (research/111 B-PW)
        U64 identity = 0;                  // program_identity of its GL sources (research/160 P1)
    };
    extern std::unordered_map<GLuint, Program> sPrograms;
    // GL shader objects: type and source (glShaderSource), and each program's attached shaders.
    extern std::unordered_map<GLuint, std::pair<GLenum, std::string>> sShaderSources;
    extern std::unordered_map<GLuint, std::vector<GLuint>> sAttached;
    extern thread_local GLuint sCurrentProgram;
    extern U32 sProgramChanges;
    Program* current_program();
    // current_program()'s resolution of sCurrentProgram (the native shader API sets it too).
    extern thread_local Program* sCurrentResolved;
    extern thread_local GLuint sCurrentResolvedName;
    extern thread_local U32 sCurrentResolvedAt;

    // The link and the uniform writes (llvkgl_program.cpp).
    void destroy_program_objects(Program& program);
    U32 shared_layout_programs();
    void draw_run_programs(U32& marked, U32& rows, U32& shared);
    void link_program(GLuint id);
    void record_location(GLuint id, const char* name, GLint location);
    void apply_uniform(Program& program, const UniformLocation& loc, GLsizei count, U32 components, const void* data);
    void apply_uniform_matrix(Program& program, const UniformLocation& loc, GLsizei count, U32 columns, U32 rows, GLboolean transpose,
                              const GLfloat* data);

    // ------------------------------------------------------------------
    // Vertex attributes (single VAO in the viewer; per context; llvkgl_draw.cpp)

    struct Attrib
    {
        bool enabled = false;
        GLint size = 4;
        GLenum type = GL_FLOAT;
        bool normalized = false;
        bool integer = false;
        GLsizei stride = 0;
        size_t offset = 0;
        GLuint buffer = 0;
        F32 constant[4] = { 0.f, 0.f, 0.f, 1.f };
        // Buffer resolved from `buffer`, valid while resolved_at == sBufferDeletes.
        Buffer* resolved = nullptr;
        U32 resolved_at = U32_MAX;
    };
    extern thread_local Attrib sAttribs[32];
    Buffer* element_buffer();
    Buffer* attrib_buffer(Attrib& a);
    VkFormat attrib_format(const Attrib& a);

    // ------------------------------------------------------------------
    // Readback (llvkgl_readback.cpp, research/42 G4)

    // The last level read back from an image (glReadPixels, glGetTexImage).
    struct ReadbackCache
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageAspectFlags aspect = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        U32 level = 0, layer = 0;
        U32 width = 0, height = 0, texel = 0;
        U64 serial = 0;             // LLVKMirror::recordSerial() right after the copy
        U32 texture_deletes = 0;    // an image handle could be reused after a deletion
        std::vector<U8> data;       // one level of one layer, rows bottom first
    };

    extern ReadbackCache sReadback;
    extern U32 sReadbacks;
    extern U32 sReadbackWaits;

    // True when the copy was recorded (the level is then defined).
    bool copy_from_framebuffer(Texture* dst, GLint level, GLint xoff, GLint yoff, U32 layer, GLint x, GLint y, GLsizei width, GLsizei height);
    // texture-leak (2026-10-27): LLImageGL::scaleDown's image swap. tex gets a
    // new level 0 of width x height (internal format `internal`), copied from
    // the read framebuffer's (0, 0) corner, only when that copy can be made;
    // false and nothing changed otherwise (the old image stays, never an
    // image without contents).
    bool tex_scale_down(Texture* tex, GLint internal, GLsizei width, GLsizei height);
    bool read_pixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
    bool tex_level_parameter(GLenum target, GLint level, GLenum pname, GLint* params);
    bool get_tex_image(GLenum target, GLint level, GLenum format, GLenum type, void* pixels);
    bool get_compressed_tex_image(GLenum target, GLint level, void* pixels);

    // ------------------------------------------------------------------
    // Pixel conversion of uploads (llvkgl_texconvert.cpp, research/70)

    // One GL client image (format/type) to convert to a texture's Vulkan texel
    // layout: what the conversion reads (the GL unpack state and the texture's
    // format, taken when it is planned) and where it writes. Rows are
    // independent: run_convert can do a range of them on any thread.
    struct ConvertJob
    {
        const U8* src = nullptr;
        size_t src_row = 0;         // bytes from one source row to the next (row length, alignment)
        U8* dst = nullptr;          // width * height * texel_size bytes, rows packed
        U32 width = 0;
        U32 height = 0;
        GLenum format = 0;
        GLenum type = 0;
        U32 src_channels = 0;
        U32 src_component = 0;
        U32 dst_channels = 0;
        U32 dst_component = 0;
        U32 texel_size = 0;

        size_t size() const { return (size_t)width * height * texel_size; }
    };

    U16 float_to_half(F32 value);
    F32 half_to_float(U16 half);
    bool plan_convert(const Texture& tex, GLenum format, GLenum type, const void* pixels, U32 width, U32 height, ConvertJob& job);
    void run_convert(const ConvertJob& job, U32 first_row, U32 end_row);
    void defer_convert(const ConvertJob& job);
    void join_tex_converts(LLRHI::RunJobs run, U32 extra_count, const std::function<void(U32)>& extra);
    void join_before_submit();

    // ------------------------------------------------------------------
    // The GL entry points (llvkgl_entry.cpp) and the native API (llvkgl_hooks.cpp)

    // What the native API shares with the wrappers: the tracking of a bind
    // and the pixel unpack buffer bound.
    extern thread_local GLuint sUnpackBuffer;
    void bind_texture(GLenum target, GLuint texture);
    GLenum bind_target(GLenum target);
    void set_cap(GLenum cap, bool on);
    void bind_framebuffer(GLenum target, GLuint framebuffer);
    void draw_buffers(GLsizei n, const GLenum* buffers);
    void draw_buffer(GLenum buffer);
    void read_buffer(GLenum buffer);
    void APIENTRY wrap_glPixelStorei(GLenum pname, GLint param);
    // The native occlusion queries (research/42), with the wrappers' bodies.
    void native_begin_query(U32 target, U32 name);
    void native_end_query(U32 target);
    void native_get_query_object(U32 name, U32 pname, U32* params);
    // LLVKGL::install's parts: the GL pointers to the wrappers, then the native
    // API hooks that need them wrapped.
    void install_gl_wraps();
    void install_native_hooks();

    // GL's queries answered by the Vulkan side (llvkgl_glquery.cpp, research/117
    // G2): the answers (for the table) and a cap set_cap does not keep.
    void APIENTRY answer_glGetIntegerv(GLenum pname, GLint* data);
    void APIENTRY answer_glGetFloatv(GLenum pname, GLfloat* data);
    void APIENTRY answer_glGetBooleanv(GLenum pname, GLboolean* data);
    GLboolean APIENTRY answer_glIsEnabled(GLenum cap);
    GLenum APIENTRY answer_glGetError();
    // An error for answer_glGetError to report once, as GL keeps its first
    // error until read; per thread, as GL keeps one per context (the thread
    // that made the image reads it: the main thread for the main context, a
    // texture thread for its own). Only an image the device could not make sets one
    // (GL_OUT_OF_MEMORY), so LLRenderTarget::allocate sees it fail.
    void raise_gl_error(GLenum error);
    U32 compressed_block_bytes(VkFormat format);    // llvkgl_texture.cpp
    void APIENTRY answer_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params);
    void note_other_cap(GLenum cap, bool on);
    // research/117 G3 (llvkgl_shaderobj.cpp): shader and program objects are
    // the Vulkan side's (shader_object_function, for the table): own_link
    // after link_program works out what GL would answer (GL's view of the
    // sources, from the cache prepare_gl_view fills on a parallel reload's
    // workers, any thread); the wrappers ask own_uniform_location
    // and own_block_index, and tell of deleted programs and detached shaders.
    // Under sLock. keep false (a prewarm's other presets): onto the disk only,
    // not into the cache, which keeps the views programs hold (fix-pipeleak).
    void prepare_gl_view(const std::vector<LLVKShader::Source>& sources, bool keep);
    // After a shader reload (draw_run_programs): the cache's views no program
    // holds any more go. Kept until then, a reload that deletes every program
    // before linking the same sources again still finds them (fix-pipeleak:
    // each reload with other sources, presets or the water mirror, used to
    // add its views for good).
    void prune_gl_views();
    void own_link(GLuint program);
    GLint own_uniform_location(GLuint program, const GLchar* name);
    GLuint own_block_index(GLuint program, const GLchar* name);
    void own_program_deleted(GLuint program, const std::vector<GLuint>& detached);
    void own_shader_detached(GLuint shader);
    PROC shader_object_function(const char* name);

    // research/117 G4, G5 (llvkgl_context.cpp): object names come from here
    // (the delete wrappers give theirs back), sync objects complete at once,
    // and the GL calls no wrapper covers do nothing. get_proc_address: the
    // table LL_GET_PROC_ADDRESS reads (there is no GL context).
    extern thread_local GLuint sVertexArray;
    enum NameKind { NAMES_TEXTURE, NAMES_BUFFER, NAMES_FRAMEBUFFER, NAMES_QUERY, NAMES_VERTEX_ARRAY, NAMES_KIND_COUNT };
    void release_names(NameKind kind, GLsizei n, const GLuint* names);
    // Whether a texture name is given out (glGenTextures) and not deleted.
    bool texture_name_live(GLuint name);
    PROC get_proc_address(const char* name);

    // ------------------------------------------------------------------
    // Order-independent transparency (research/41): per-pixel linked lists.
    //
    // Inside a session (LLRHI::sOITBegin .. sOITResolve, the above-water alpha
    // of the main view) a draw of a program with an OIT variant writes no
    // color and no depth: each fragment that passes the depth test against
    // the opaque depth (early fragment tests) becomes a node of its pixel's
    // list. A node is 6 words: the color as 4 halves (HDR as the target
    // keeps it), the depth, the draw's blend state (OIT_BLEND_* below), the
    // draw's priority (LLRHI::sOITPriority, pushed at OIT_PRIORITY_OFFSET)
    // and the next node. The resolve pass sorts each pixel's nodes back to
    // front and blends them over a copy of the target with each node's own
    // GL blend equation, the alpha channel (the glow mark) included. The
    // order: by exact depth, except inside a surface: from the front, a node
    // within the tolerance (LLRHI_OIT_TOLERANCE_MM, _REL) behind the one in
    // front of it joins its surface, up to LLRHI_OIT_SPAN tolerances deep;
    // inside a surface the priority (the higher on top), then a node that
    // leaves the color as it is (the glow pass) on top, then the exact depth.
    // Where surfaces start depends on the depths only.
    //
    // The node buffer starts with { count, capacity, flags, 0, bounds };
    // count is read back a frame or two later and the buffer grows when it
    // was short. Fragments past the capacity are summed into the rest image
    // (weighted average, flags bit 0), which the resolve puts behind the
    // sorted nodes; so are the farthest nodes of a pixel with more than
    // LLRHI_OIT_MAX_LAYERS (16, at most OIT_MAX_LAYERS). The resolve: a
    // bounds pass (OIT_BOUNDS_COMPUTE) finds the pixels with a list and
    // copies the target there, then a strip over their bounds composites.
    //
    // Hybrid sessions (LLRHI::sOITHybrid): the avatars' alpha is drawn first
    // (nodes as above; a fragment past the capacity still marks its pixel,
    // the head mark below). LLRHI::sOITWorld then orders those writes before the
    // rest of the alpha, which is drawn with the same variant in its split
    // mode (OIT_SPLIT_CONSTANT_ID): the pipeline keeps the draw's own blend
    // state and color mask, and a fragment at a pixel without avatar nodes
    // (head empty) leaves frag_color to it, as without OIT; elsewhere it
    // becomes a node and writes no color (discard), or, past the capacity,
    // is summed into the front (composited over the pixel's nodes; without
    // the rest image yet it blends into the target, behind them). No world
    // fragment makes an empty head non-empty, so which pixels are split
    // does not depend on draw order. The resolve then only touches pixels
    // with nodes, whose target color has no alpha of the session in it.

    // The blend word: GL blend factors as OIT_FACTOR_*, equations as OIT_EQ_*,
    // and the color mask.
    enum OITFactor
    {
        OIT_FACTOR_ZERO, OIT_FACTOR_ONE, OIT_FACTOR_SRC_COLOR, OIT_FACTOR_ONE_MINUS_SRC_COLOR, OIT_FACTOR_DST_COLOR,
        OIT_FACTOR_ONE_MINUS_DST_COLOR, OIT_FACTOR_SRC_ALPHA, OIT_FACTOR_ONE_MINUS_SRC_ALPHA, OIT_FACTOR_DST_ALPHA,
        OIT_FACTOR_ONE_MINUS_DST_ALPHA, OIT_FACTOR_SRC_ALPHA_SATURATE,
        OIT_FACTOR_CONSTANT,                // the blend constant, which no pipeline sets (zero)
        OIT_FACTOR_ONE_MINUS_CONSTANT,
    };
    enum OITEquation { OIT_EQ_ADD, OIT_EQ_SUBTRACT, OIT_EQ_REVERSE_SUBTRACT, OIT_EQ_MIN, OIT_EQ_MAX };
    // Bits 0-3 source RGB factor, 4-7 destination RGB, 8-11 source alpha,
    // 12-15 destination alpha, 16-18 RGB equation, 19-21 alpha equation,
    // 22-25 color mask (R, G, B, A).
    constexpr U32 OIT_BLEND_CONSTANT_ID = 200;
    constexpr U32 OIT_SPLIT_CONSTANT_ID = 201;  // 1: the hybrid session's world draws
    // The draw's priority: the last push constant word, which the program's
    // own words never reach in a variant (it would not build: their offsets meet).
    constexpr U32 OIT_PRIORITY_OFFSET = LLVKBindless::PUSH_CONSTANT_BYTES - 4;
    constexpr U32 OIT_NODE_WORDS = 6;
    constexpr U32 OIT_HEADER_BYTES = 32;    // count, capacity, flags, 0, the bounds (min x, min y, max x, max y)
    constexpr U32 OIT_MAX_LAYERS = 32;     // the largest LLRHI_OIT_MAX_LAYERS
    // The rest image: the rest (layers 0-5: sum of rgb * a (3), sum of a, sum
    // of -log2(1 - a), glow) and the front (6-11, the same).
    constexpr U32 OIT_REST_LAYERS = 12;
    constexpr U32 OIT_FLAG_REST = 1;
    // How a draw uses its program's OIT variant: not at all, every fragment a
    // node (no color writes), or split (hybrid world draws: the draw's own
    // blend state, nodes only at pixels with avatar nodes).
    enum OITDrawMode : U8 { OIT_DRAW_NONE, OIT_DRAW_NODES, OIT_DRAW_SPLIT };
    // A head is a node index, 0xffffffff (no list) or 0xfffffffe (the mark:
    // covered by a node that found no room; it ends a list as any index past
    // the capacity does).

    // The OIT shaders and the variant's rewrite (llvkgl_oitshaders.cpp).
    extern const char* const OIT_RESOLVE_VERTEX;
    extern const char* const OIT_BOUNDS_COMPUTE;
    extern const char* const OIT_RESOLVE_FRAGMENT;
    // row_word (research/205 3a-2): not U32_MAX, a draw reading rows takes its
    // priority from that word of its row (row_priority: it does; false when
    // the fragment file with main reads no rows).
    bool oit_rewrite(std::vector<LLVKShader::Source>& sources, U32 row_word = U32_MAX, bool* row_priority = nullptr);
    // The row word for oit_rewrite of a program reading draw rows with
    // draw_row_words words (its rows word's place): while LLRHI_SCENE_ALPHA is
    // not off; else U32_MAX (the variants stay as baked).
    U32 oit_row_word(bool draw_rows, U32 draw_row_words);

    // The OIT sessions (llvkgl_oit.cpp): LLRHI's hooks and the report.
    bool oit_supported();
    bool oit_begin(F32 depth_a, F32 depth_b, bool hybrid);
    void oit_world();
    void oit_resolve();
    bool oit_snapshot_check();
    bool oit_program_variant(U32 gl_program);
    std::string oit_report(U32 frames);
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER

#endif // LL_LLVKGL_INTERNAL_H
