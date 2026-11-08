/**
 * @file llrhiglstats.cpp
 * @brief Opt-in GL call counter (LLRHI_TRACE=1), built on GL function pointer redirection.
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

#include "llrhiglstats.h"
#include "llrhiuploadtrace.h"

#include "llframetimer.h"
#include "llglheaders.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sstream>

#if LL_GL_FUNC_POINTER

namespace
{
    enum Counter
    {
        DRAWS,          // every glDraw* call
        PRIMS,          // vertices / indices submitted
        PROGRAMS,       // glUseProgram
        TEXTURES,       // glBindTexture
        BUFFERS,        // glBindBuffer / glBindVertexArray
        FRAMEBUFFERS,   // glBindFramebuffer
        UPLOAD_BYTES,   // glBufferData / glBufferSubData sizes
        TEX_UPLOADS,    // glTex(Sub)Image* / glCompressedTexImage2D
        UNIFORMS,       // glUniform*
        STATE,          // glEnable / glDisable / blend / depth / cull
        COUNTER_COUNT
    };

    const char* const sNames[COUNTER_COUNT] =
    {
        "draws", "prims", "programs", "tex_binds", "buf_binds",
        "fbo_binds", "upload_kb", "tex_uploads", "uniforms", "state"
    };

    std::atomic<U64> sCurrent[COUNTER_COUNT];

    // Accumulated over the reporting window (main thread only).
    U64 sTotal[COUNTER_COUNT];
    U64 sPeak[COUNTER_COUNT];
    U32 sFrames = 0;
    U32 sLastFrame = 0;
    std::chrono::steady_clock::time_point sWindowStart;

    constexpr double REPORT_SECONDS = 5.0;

    void report(double seconds)
    {
        std::ostringstream out;
        out << sFrames << " frames in " << seconds << "s (" << (sFrames / seconds) << " fps). avg/peak per frame:";
        for (int i = 0; i < COUNTER_COUNT; ++i)
        {
            U64 avg = sTotal[i] / sFrames;
            U64 peak = sPeak[i];
            if (i == UPLOAD_BYTES)
            {
                avg /= 1024;
                peak /= 1024;
            }
            out << ' ' << sNames[i] << '=' << avg << '/' << peak;
        }
        LL_INFOS("RHIStats") << out.str() << LL_ENDL;

        if (LLRHIUploadTrace::enabled())
        {
            LLRHIUploadTrace::report(sFrames);
        }
    }

    // Allocation-only glBufferData (null data) moves nothing and is not counted.
    inline void add_upload(U64 bytes)
    {
        if (!bytes)
        {
            return;
        }
        sCurrent[UPLOAD_BYTES].fetch_add(bytes, std::memory_order_relaxed);
        if (LLRHIUploadTrace::enabled())
        {
            LLRHIUploadTrace::record(bytes);
        }
    }

    // Close out the previous frame when the viewer's frame counter moves on.
    void tick()
    {
        U32 frame = LLFrameTimer::getFrameCount();
        if (frame == sLastFrame)
        {
            return;
        }
        sLastFrame = frame;

        for (int i = 0; i < COUNTER_COUNT; ++i)
        {
            U64 n = sCurrent[i].exchange(0, std::memory_order_relaxed);
            sTotal[i] += n;
            sPeak[i] = llmax(sPeak[i], n);
        }
        ++sFrames;

        auto now = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration<double>(now - sWindowStart).count();
        if (seconds >= REPORT_SECONDS)
        {
            report(seconds);
            memset(sTotal, 0, sizeof(sTotal));
            memset(sPeak, 0, sizeof(sPeak));
            sFrames = 0;
            sWindowStart = now;
        }
    }

    inline void add(Counter c, U64 n = 1)
    {
        sCurrent[c].fetch_add(n, std::memory_order_relaxed);
    }
}

// One wrapper per GL entry point: keep the driver's pointer, count, forward.
// The frame tick runs on draws only so texture threads never touch the totals.
#define LLRHI_WRAP(fn, params, args, counting)                 \
    namespace { decltype(fn) real_##fn = nullptr; }             \
    static void APIENTRY wrap_##fn params                       \
    {                                                           \
        counting;                                               \
        real_##fn args;                                         \
    }

#define LLRHI_DRAW(n) tick(); add(DRAWS); add(PRIMS, (U64)(n))

LLRHI_WRAP(glDrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count), LLRHI_DRAW(count))
LLRHI_WRAP(glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices), (mode, count, type, indices), LLRHI_DRAW(count))
LLRHI_WRAP(glDrawRangeElements, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices), (mode, start, end, count, type, indices), LLRHI_DRAW(count))
LLRHI_WRAP(glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instances), (mode, first, count, instances), LLRHI_DRAW((U64)count * instances))
LLRHI_WRAP(glDrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances), (mode, count, type, indices, instances), LLRHI_DRAW((U64)count * instances))

LLRHI_WRAP(glUseProgram, (GLuint program), (program), add(PROGRAMS))
LLRHI_WRAP(glBindTexture, (GLenum target, GLuint texture), (target, texture), add(TEXTURES))
LLRHI_WRAP(glBindBuffer, (GLenum target, GLuint buffer), (target, buffer), add(BUFFERS))
LLRHI_WRAP(glBindVertexArray, (GLuint array), (array), add(BUFFERS))
LLRHI_WRAP(glBindFramebuffer, (GLenum target, GLuint framebuffer), (target, framebuffer), add(FRAMEBUFFERS))

LLRHI_WRAP(glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage), (target, size, data, usage), add_upload(data ? (U64)size : 0))
LLRHI_WRAP(glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data), (target, offset, size, data), add_upload((U64)size))
LLRHI_WRAP(glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels), (target, level, internalformat, width, height, border, format, type, pixels), add(TEX_UPLOADS))
LLRHI_WRAP(glTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels), (target, level, xoffset, yoffset, width, height, format, type, pixels), add(TEX_UPLOADS))
LLRHI_WRAP(glCompressedTexImage2D, (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data), (target, level, internalformat, width, height, border, imageSize, data), add(TEX_UPLOADS))

LLRHI_WRAP(glUniform1i, (GLint location, GLint v0), (location, v0), add(UNIFORMS))
LLRHI_WRAP(glUniform1f, (GLint location, GLfloat v0), (location, v0), add(UNIFORMS))
LLRHI_WRAP(glUniform2f, (GLint location, GLfloat v0, GLfloat v1), (location, v0, v1), add(UNIFORMS))
LLRHI_WRAP(glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2), (location, v0, v1, v2), add(UNIFORMS))
LLRHI_WRAP(glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3), (location, v0, v1, v2, v3), add(UNIFORMS))
LLRHI_WRAP(glUniform1iv, (GLint location, GLsizei count, const GLint* value), (location, count, value), add(UNIFORMS))
LLRHI_WRAP(glUniform1fv, (GLint location, GLsizei count, const GLfloat* value), (location, count, value), add(UNIFORMS))
LLRHI_WRAP(glUniform2fv, (GLint location, GLsizei count, const GLfloat* value), (location, count, value), add(UNIFORMS))
LLRHI_WRAP(glUniform3fv, (GLint location, GLsizei count, const GLfloat* value), (location, count, value), add(UNIFORMS))
LLRHI_WRAP(glUniform4fv, (GLint location, GLsizei count, const GLfloat* value), (location, count, value), add(UNIFORMS))
LLRHI_WRAP(glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value), add(UNIFORMS))
LLRHI_WRAP(glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value), add(UNIFORMS))
LLRHI_WRAP(glUniformMatrix3x4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value), add(UNIFORMS))

LLRHI_WRAP(glEnable, (GLenum cap), (cap), add(STATE))
LLRHI_WRAP(glDisable, (GLenum cap), (cap), add(STATE))
LLRHI_WRAP(glBlendFunc, (GLenum sfactor, GLenum dfactor), (sfactor, dfactor), add(STATE))
LLRHI_WRAP(glBlendFuncSeparate, (GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha), (sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha), add(STATE))
LLRHI_WRAP(glDepthMask, (GLboolean flag), (flag), add(STATE))
LLRHI_WRAP(glDepthFunc, (GLenum func), (func), add(STATE))
LLRHI_WRAP(glCullFace, (GLenum mode), (mode), add(STATE))
LLRHI_WRAP(glColorMask, (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha), (red, green, blue, alpha), add(STATE))

#undef LLRHI_DRAW
#undef LLRHI_WRAP

// Swap a loaded entry point for its wrapper; entry points the driver did not
// provide stay null so the viewer's own capability checks are unaffected.
#define LLRHI_INSTALL(fn)        \
    if (fn)                      \
    {                            \
        real_##fn = fn;          \
        fn = wrap_##fn;          \
        ++installed;             \
    }

void LLRHIGLStats::install()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;

    sWindowStart = std::chrono::steady_clock::now();
    sLastFrame = LLFrameTimer::getFrameCount();

    int installed = 0;
    LLRHI_INSTALL(glDrawArrays)
    LLRHI_INSTALL(glDrawElements)
    LLRHI_INSTALL(glDrawRangeElements)
    LLRHI_INSTALL(glDrawArraysInstanced)
    LLRHI_INSTALL(glDrawElementsInstanced)
    LLRHI_INSTALL(glUseProgram)
    LLRHI_INSTALL(glBindTexture)
    LLRHI_INSTALL(glBindBuffer)
    LLRHI_INSTALL(glBindVertexArray)
    LLRHI_INSTALL(glBindFramebuffer)
    LLRHI_INSTALL(glBufferData)
    LLRHI_INSTALL(glBufferSubData)
    LLRHI_INSTALL(glTexImage2D)
    LLRHI_INSTALL(glTexSubImage2D)
    LLRHI_INSTALL(glCompressedTexImage2D)
    LLRHI_INSTALL(glUniform1i)
    LLRHI_INSTALL(glUniform1f)
    LLRHI_INSTALL(glUniform2f)
    LLRHI_INSTALL(glUniform3f)
    LLRHI_INSTALL(glUniform4f)
    LLRHI_INSTALL(glUniform1iv)
    LLRHI_INSTALL(glUniform1fv)
    LLRHI_INSTALL(glUniform2fv)
    LLRHI_INSTALL(glUniform3fv)
    LLRHI_INSTALL(glUniform4fv)
    LLRHI_INSTALL(glUniformMatrix3fv)
    LLRHI_INSTALL(glUniformMatrix4fv)
    LLRHI_INSTALL(glUniformMatrix3x4fv)
    LLRHI_INSTALL(glEnable)
    LLRHI_INSTALL(glDisable)
    LLRHI_INSTALL(glBlendFunc)
    LLRHI_INSTALL(glBlendFuncSeparate)
    LLRHI_INSTALL(glDepthMask)
    LLRHI_INSTALL(glDepthFunc)
    LLRHI_INSTALL(glCullFace)
    LLRHI_INSTALL(glColorMask)

    LL_INFOS("RHIStats") << "GL call counter installed on " << installed << " entry points; reporting every "
                         << REPORT_SECONDS << "s" << LL_ENDL;
}

#undef LLRHI_INSTALL

#else // !LL_GL_FUNC_POINTER

void LLRHIGLStats::install()
{
    LL_WARNS("RHIStats") << "GL call counter needs GL function pointers (LL_GL_FUNC_POINTER); not available on this platform" << LL_ENDL;
}

#endif // LL_GL_FUNC_POINTER

// ----------------------------------------------------------------------
// Draw sources (diagnostics)

#include <unordered_set>

namespace LLRHIDrawSources
{
    bool sEnabled = std::getenv("LLRHI_DRAW_SOURCES") != nullptr;
    int sProbeFace = -1;
    int sShadowCascade = -1;
}

namespace
{
    // Groups: main view, main shadow cascades, probe faces, probe shadows.
    enum Group { MAIN, MAIN_SHADOW, PROBE, PROBE_SHADOW, GROUP_COUNT };
    const char* const sGroupNames[GROUP_COUNT] = { "main view", "main sun shadow", "probe faces", "probe sun shadow" };

    struct SourceStats
    {
        U64 draws[GROUP_COUNT] = {};
        U64 distinct[GROUP_COUNT] = {};
        U64 passes[GROUP_COUNT] = {};
        U32 frames = 0;
    };
    SourceStats sSources;
    std::unordered_set<U64> sSeen[GROUP_COUNT];
    std::unordered_set<int> sPasses[GROUP_COUNT];
    U32 sSourceFrame = 0;
    F64 sSourceLogTime = 0.0;

    void flush_source_frame()
    {
        for (int g = 0; g < GROUP_COUNT; ++g)
        {
            sSources.distinct[g] += sSeen[g].size();
            sSources.passes[g] += sPasses[g].size();
            sSeen[g].clear();
            sPasses[g].clear();
        }
        ++sSources.frames;
        F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - sSourceLogTime >= 5.0)
        {
            std::ostringstream out;
            out << "per frame over " << sSources.frames << " frames:";
            for (int g = 0; g < GROUP_COUNT; ++g)
            {
                U32 f = llmax(sSources.frames, 1u);
                out << " " << sGroupNames[g] << " draws " << sSources.draws[g] / f << " distinct " << sSources.distinct[g] / f
                    << " passes " << (F32)sSources.passes[g] / f << ";";
            }
            LL_INFOS("RHIDrawSources") << out.str() << LL_ENDL;
            sSources = SourceStats();
            sSourceLogTime = now;
        }
    }
}

void LLRHIDrawSources::countDraw(const void* geometry, unsigned int offset)
{
    U32 frame = LLFrameTimer::getFrameCount();
    if (frame != sSourceFrame)
    {
        flush_source_frame();
        sSourceFrame = frame;
    }
    int group = sProbeFace < 0 ? (sShadowCascade < 0 ? MAIN : MAIN_SHADOW) : (sShadowCascade < 0 ? PROBE : PROBE_SHADOW);
    ++sSources.draws[group];
    sSeen[group].insert((U64)(uintptr_t)geometry ^ ((U64)offset << 40));
    sPasses[group].insert((sProbeFace + 1) * 8 + sShadowCascade + 1);
}
