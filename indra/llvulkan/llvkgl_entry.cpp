/**
 * @file llvkgl_entry.cpp
 * @brief The GL entry points the viewer calls, wrapped (glBindBuffer ... glGetQueryObjectuiv): the GL context they track and install_gl_wraps (research/91).
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

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    U32 sGLCalls = 0;
namespace
{
    // Calls and time of each wrapped GL function (the GL calls between draws).
    struct GLCallStat
    {
        const char* name;
        U64 ticks = 0;
        U32 count = 0;
        // research/218 vk-s7f (research/207 7.3 T10): every call, with or
        // without sPerf (the draws' too), for the VKPerf "old paths" line.
        U32 calls = 0;
        explicit GLCallStat(const char* n);
    };

    std::vector<GLCallStat*>& gl_call_stats()
    {
        static std::vector<GLCallStat*> stats;
        return stats;
    }

    // Guards the list itself: a wrapper's first call registers its stat from any
    // thread while gl_call_report() copies the list.
    std::mutex sGLCallStatsMutex;

    GLCallStat::GLCallStat(const char* n) : name(n)
    {
        std::lock_guard<std::mutex> lock(sGLCallStatsMutex);
        gl_call_stats().push_back(this);
    }

    // Made after the wrapper's lock_guard on sLock, so the counts below are
    // only touched under sLock, as perfReport() reads and clears them.
    struct GLCallScope
    {
        GLCallStat& stat;
        U64 start = 0;
        explicit GLCallScope(GLCallStat& s) : stat(s)
        {
            ++sGLCalls;
            ++stat.calls;
            if (sPerf)
            {
                ++stat.count;
                start = __rdtsc();
            }
        }
        ~GLCallScope()
        {
            if (start)
            {
                const U64 ticks = __rdtsc() - start;
                stat.ticks += ticks;
                sPerfTicks[PERF_GL_CALLS] += ticks;
            }
        }
    };
    // ------------------------------------------------------------------
    // Framebuffers (per context, like GL)

} // namespace
    thread_local std::unordered_map<GLuint, Framebuffer> sFramebuffers;
    U32 sDrawBuffersKept = 0;
namespace
{

    // The bound program's record of a GL uniform location, or null (lookup part
    // of the glUniform* wrappers; the native shader API keeps its own copies).
    const UniformLocation* current_location(Program*& program, GLint location)
    {
        program = current_program();
        if (!program || !program->built || location < 0)
        {
            return nullptr;
        }
        auto loc = program->locations.find(location);
        return loc != program->locations.end() ? &loc->second : nullptr;
    }

    void set_uniform(GLint location, GLsizei count, U32 components, const void* data)
    {
        Program* program = nullptr;
        if (const UniformLocation* loc = current_location(program, location))
        {
            apply_uniform(*program, *loc, count, components, data);
        }
    }

    void set_uniform_matrix(GLint location, GLsizei count, U32 columns, U32 rows, GLboolean transpose, const GLfloat* data)
    {
        Program* program = nullptr;
        if (const UniformLocation* loc = current_location(program, location))
        {
            apply_uniform_matrix(*program, *loc, count, columns, rows, transpose, data);
        }
    }

    // ------------------------------------------------------------------
    // Fixed-function state (per context)

} // namespace
    thread_local State sState;
namespace
{

    // ------------------------------------------------------------------
    // The GL entry points, tracked.

    // State, data, objects and rendering work: there is no GL (research/117),
    // the wrappers keep everything on the Vulkan side.
#define LLVKGL_WRAP_STATE(fn, params, args, ...)                \
    void APIENTRY wrap_##fn params                              \
    {                                                           \
        LLVKGL_REDIRECT(#fn, wrap_##fn args)                    \
        static GLCallStat gl_stat(#fn);                         \
        std::lock_guard<LLVKTrackerMutex> lock(sLock);      \
        GLCallScope gl_scope(gl_stat);                          \
        __VA_ARGS__;                                            \
    }

#define LLVKGL_WRAP_DRAW(fn, params, args, ...)                 \
    void APIENTRY wrap_##fn params                              \
    {                                                           \
        LLVKGL_REDIRECT(#fn, wrap_##fn args)                    \
        static GLCallStat gl_stat(#fn);                         \
        std::lock_guard<LLVKTrackerMutex> lock(sLock);      \
        ++gl_stat.calls;    /* research/218 vk-s7f: counted, not timed */ \
        __VA_ARGS__;                                            \
    }

    // Buffers. Pixel transfer buffers (LLImageGL's PBO downscale,
    // RenderDownScaleMethod 1, not the default) are tracked by name only:
    // the Vulkan side keeps no contents for them (research/117 1.2).
} // namespace
    // The pixel unpack buffer bound (per thread, like a GL context): the
    // native uploads (llvkgl_hooks.cpp) leave uploads from one to the wrappers.
    thread_local GLuint sUnpackBuffer = 0;
namespace
{
    thread_local GLuint sPackBuffer = 0;     // a readback into one is not answered

    LLVKGL_WRAP_STATE(glBindBuffer, (GLenum target, GLuint buffer), (target, buffer),
        {
            GLuint* b = binding_for(target);
            if (b) *b = buffer;
            if (target == GL_PIXEL_UNPACK_BUFFER) sUnpackBuffer = buffer;
            if (target == GL_PIXEL_PACK_BUFFER) sPackBuffer = buffer;
        })

    LLVKGL_WRAP_STATE(glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage), (target, size, data, usage),
        { buffer_data(bound_name(target), bound_buffer(target), (size_t)size, data); })

    // As GL: a deleted buffer leaves this thread's (context's) uniform binding
    // points and vertex attributes at 0, so its name, given out again at once,
    // does not reach the next buffer through a stale binding.
    void unbind_deleted_buffer(GLuint name)
    {
        if (!name)
        {
            return;
        }
        if (sUniformBuffer == name)
        {
            sUniformBuffer = 0;
        }
        for (UniformBinding& binding : sUniformBindings)
        {
            if (binding.buffer == name)
            {
                binding = UniformBinding();
                ++sUniformBlockState;
            }
        }
        for (Attrib& attrib : sAttribs)
        {
            if (attrib.buffer == name)
            {
                attrib.buffer = 0;
                attrib.resolved = nullptr;
                attrib.resolved_at = U32_MAX;
                ++sVertexState;
            }
        }
    }

    LLVKGL_WRAP_STATE(glDeleteBuffers, (GLsizei n, const GLuint* buffers), (n, buffers),
        {
            for (GLsizei i = 0; i < n; ++i)
            {
                if (Buffer* b = sBuffers.find(buffers[i]))
                {
                    forget_resolved_buffer(buffers[i]);
                    buffer_changed(buffers[i]);
                    retire_gpu(*b);
                    sBuffers.erase(buffers[i]);
                    ++sBufferDeletes;
                }
                if (sArrayBuffer == buffers[i]) sArrayBuffer = 0;
                if (sElementBuffer == buffers[i]) sElementBuffer = 0;
                if (sUnpackBuffer == buffers[i]) sUnpackBuffer = 0;
                if (sPackBuffer == buffers[i]) sPackBuffer = 0;
                unbind_deleted_buffer(buffers[i]);
            }
            release_names(NAMES_BUFFER, n, buffers);
        })

    LLVKGL_WRAP_STATE(glBindBufferBase, (GLenum target, GLuint index, GLuint buffer), (target, index, buffer),
        { bind_buffer_base(target, index, buffer); })

    // Textures
} // namespace
    // glBindTexture on the active unit, as tracked (under sLock). Also used by
    // the native texture binding API (llvkgl_hooks.cpp).
    void bind_texture(GLenum target, GLuint texture)
    {
        UnitBindings& u = sUnits[sActiveUnit];
        int slot = target_slot(target);
        // research/180 stage 1: a published name (a 2D texture that has its
        // target) is bound by name only; unit_texture looks it up if asked.
        const bool published = texture && published_slot(texture);
        if (u.names[slot] == texture && u.resolved_at[slot] == sTextureDeletes && (!texture || u.textures[slot] || published))
        {
            return;     // already bound (the viewer rebinds every draw): keep the caches valid
        }
        u.names[slot] = texture;
        u.textures[slot] = nullptr;
        u.resolved_at[slot] = sTextureDeletes;
        if (texture && !published)
        {
            Texture& t = sTextures.get(texture);
            if (!t.target) t.target = target;
            u.textures[slot] = &t;
        }
        ++sUnitBindings[sActiveUnit];
    }
namespace
{
} // namespace
    GLenum bind_target(GLenum target)
    {
        return is_cube_face(target) ? GL_TEXTURE_CUBE_MAP : target;
    }
namespace
{

    // A deleted texture's name stays on the units it was bound to (unlike GL,
    // which unbinds it in the deleting context). The viewer's LLTexUnit keeps
    // the last name it bound per unit and skips a glBindTexture of the same
    // name; a deletion does not clear that cache. With the name given out
    // again (NameAllocator: smallest free first), a unit cleared here would
    // stay empty while the viewer believes the new texture is bound to it.
    // Nothing deleted is read: unit_texture looks the name up again after
    // every deletion (resolved_at), finding nothing until it is made again.

    LLVKGL_WRAP_STATE(glDeleteTextures, (GLsizei n, const GLuint* textures), (n, textures),
        {
            for (GLsizei i = 0; i < n; ++i)
            {
                if (sTextureDeleted) sTextureDeleted(textures[i]);
                if (sTextureDeletedMore) sTextureDeletedMore(textures[i]);
                unpublish_name(textures[i]);    // research/180 I1 (also through the bindless release above)
                if (Texture* tex = sTextures.find(textures[i]))
                {
                    destroy_texture_objects(*tex);
                    sTextures.erase(textures[i]);
                    ++sTextureDeletes;
                    ++sTextureState;
                    ++sTargetState;
                }
            }
            release_names(NAMES_TEXTURE, n, textures);
        })
} // namespace
    LLVKGL_WRAP_STATE(glPixelStorei, (GLenum pname, GLint param), (pname, param),
        {
            if (pname == GL_UNPACK_ALIGNMENT) sUnpackAlignment = llmax(1, param);
            else if (pname == GL_UNPACK_ROW_LENGTH) sUnpackRowLength = param;
            else if (pname == GL_PACK_ALIGNMENT) sPackAlignment = llmax(1, param);
            else if (pname == GL_PACK_ROW_LENGTH) sPackRowLength = param;
        })
namespace
{
    // texture-leak (2026-10-27): with a pixel unpack buffer bound, pixels is an
    // offset into it, and the Vulkan side keeps no contents for such buffers:
    // the image is made without texels (never defined by this call) instead
    // of reading the offset as a pointer. (LLImageGL::scaleDown's PBO way,
    // the one user, is not taken any more.)
    const void* unpack_pixels(const void* pixels, const char* call)
    {
        if (sUnpackBuffer)
        {
            ++sTexDefined.pbo_images;
            unhandled(std::string(call) + " from a pixel unpack buffer");
            return nullptr;
        }
        return pixels;
    }

    // Texture images: the Vulkan image (storage alone for render targets and
    // the like, made without pixels).
    LLVKGL_WRAP_STATE(glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels),
        (target, level, internal, width, height, border, format, type, pixels),
        { Texture* tex = bound_texture(target); touch_texture(tex);
          tex_image(tex, target, level, internal, width, height, format, type, unpack_pixels(pixels, "glTexImage2D"), false, 0); })

    LLVKGL_WRAP_STATE(glCompressedTexImage2D, (GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height, GLint border, GLsizei size, const void* data),
        (target, level, internal, width, height, border, size, data),
        { Texture* tex = bound_texture(target); touch_texture(tex); tex_image(tex, target, level, internal, width, height, 0, 0, unpack_pixels(data, "glCompressedTexImage2D"), true, (size_t)size); })

    LLVKGL_WRAP_STATE(glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels),
        (target, level, x, y, width, height, format, type, pixels),
        { Texture* tex = bound_texture(target); touch_texture(tex);
          tex_sub_image(tex, target, level, x, y, width, height, format, type, unpack_pixels(pixels, "glTexSubImage2D")); })

    LLVKGL_WRAP_STATE(glTexImage3D, (GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels),
        (target, level, internal, width, height, depth, border, format, type, pixels),
        { touch_bound(target); tex_image_3d(target, level, internal, width, height, depth, format, type, pixels); })

    LLVKGL_WRAP_STATE(glTexSubImage3D, (GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels),
        (target, level, x, y, z, width, height, depth, format, type, pixels),
        { touch_bound(target); tex_sub_image_3d(target, level, x, y, z, width, height, depth, format, type, pixels); })

    // research/288: the constellation pictures (BC4 blocks into a 2D array's layers)
    LLVKGL_WRAP_STATE(glCompressedTexSubImage3D, (GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei size, const void* data),
        (target, level, x, y, z, width, height, depth, format, size, data),
        { touch_bound(target); tex_compressed_sub_image_3d(target, level, x, y, z, width, height, depth, format, size, unpack_pixels(data, "glCompressedTexSubImage3D")); })

    LLVKGL_WRAP_STATE(glCopyTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height),
        (target, level, xoffset, yoffset, x, y, width, height),
        { touch_bound(target);
            U32 layer = is_cube_face(target) ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
            copy_from_framebuffer(bound_texture(target), level, xoffset, yoffset, layer, x, y, width, height);
        })

    LLVKGL_WRAP_STATE(glCopyTexSubImage3D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height),
        (target, level, xoffset, yoffset, zoffset, x, y, width, height),
        { Texture* tex = bound_texture(target); touch_texture(tex); copy_from_framebuffer(tex, level, xoffset, yoffset, (U32)zoffset, x, y, width, height); })

    // Readbacks the Vulkan side cannot answer are logged once and left
    // unanswered (there is no GL to ask).

    void APIENTRY wrap_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
    {
        LLVKGL_REDIRECT_QUERY("glGetTexLevelParameteriv", wrap_glGetTexLevelParameteriv(target, level, pname, params))
        static GLCallStat gl_stat("glGetTexLevelParameteriv");
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            GLCallScope gl_scope(gl_stat);
            if (tex_level_parameter(target, level, pname, params))
            {
                return;
            }
            unhandled("glGetTexLevelParameteriv " + hex(pname));
            if (params) *params = 0;
        }
    }

    void APIENTRY wrap_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
    {
        LLVKGL_REDIRECT("glGetTexImage", wrap_glGetTexImage(target, level, format, type, pixels))
        static GLCallStat gl_stat("glGetTexImage");
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            GLCallScope gl_scope(gl_stat);
            if (!sPackBuffer && get_tex_image(target, level, format, type, pixels))
            {
                return;
            }
            unhandled(std::string("glGetTexImage ") + (sPackBuffer ? std::string("into a pixel pack buffer") : "format " + hex(format)));
        }
    }

    void APIENTRY wrap_glGetCompressedTexImage(GLenum target, GLint level, void* pixels)
    {
        LLVKGL_REDIRECT("glGetCompressedTexImage", wrap_glGetCompressedTexImage(target, level, pixels))
        static GLCallStat gl_stat("glGetCompressedTexImage");
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            GLCallScope gl_scope(gl_stat);
            if (!sPackBuffer && get_compressed_tex_image(target, level, pixels))
            {
                return;
            }
            unhandled("glGetCompressedTexImage");
        }
    }

    void APIENTRY wrap_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
    {
        LLVKGL_REDIRECT("glReadPixels", wrap_glReadPixels(x, y, width, height, format, type, pixels))
        static GLCallStat gl_stat("glReadPixels");
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        GLCallScope gl_scope(gl_stat);
        if (sPackBuffer)
        {
            unhandled("glReadPixels into a pixel pack buffer");
            return;
        }
        if (!read_pixels(x, y, width, height, format, type, pixels))
        {
            unhandled("glReadPixels format " + hex(format) + " type " + hex(type) + " from framebuffer " + std::to_string(sState.read_framebuffer));
        }
    }

    LLVKGL_WRAP_STATE(glGenerateMipmap, (GLenum target), (target),
        { Texture* tex = bound_texture(target); touch_texture(tex); generate_mipmaps(tex); })

    LLVKGL_WRAP_STATE(glTexParameteriv, (GLenum target, GLenum pname, const GLint* params), (target, pname, params),
        {
            bool changed = false;
            if (params && pname == GL_TEXTURE_SWIZZLE_RGBA)
            {
                for (int c = 0; c < 4; ++c) changed |= tex_parameter(target, GL_TEXTURE_SWIZZLE_R + c, (F32)params[c]);
            }
            else if (params)
            {
                changed = tex_parameter(target, pname, (F32)params[0]);
            }
            if (changed) touch_bound(target);
        })

    // Framebuffers
    Framebuffer* framebuffer_for(GLenum target)
    {
        GLuint id = target == GL_READ_FRAMEBUFFER ? sState.read_framebuffer : sState.draw_framebuffer;
        return id ? &sFramebuffers[id] : nullptr;
    }
} // namespace
    // Tracking shared by the wrappers and the native API (research/39 N4).
    void bind_framebuffer(GLenum target, GLuint framebuffer)
    {
        if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) sState.draw_framebuffer = framebuffer;
        if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) sState.read_framebuffer = framebuffer;
    }

    void draw_buffers(GLsizei n, const GLenum* buffers)
    {
        Framebuffer* fb = framebuffer_for(GL_DRAW_FRAMEBUFFER);
        if (!fb || !buffers) return;
        const U32 count = (U32)llclamp(n, 0, 4);
        bool same = fb->draw_count == count;
        for (U32 i = 0; same && i < count; ++i) same = fb->draw_buffers[i] == buffers[i];
        if (same)
        {
            // research/154 W1: LLRenderTarget::bindTarget names its buffers at
            // every bind; the same ones change nothing a resolution reads.
            ++sDrawBuffersKept;
            return;
        }
        ++sTargetState;
        fb->draw_count = count;
        for (U32 i = 0; i < fb->draw_count; ++i) fb->draw_buffers[i] = buffers[i];
    }

    void draw_buffer(GLenum buffer)
    {
        Framebuffer* fb = framebuffer_for(GL_DRAW_FRAMEBUFFER);
        if (!fb) return;
        const U32 count = buffer == GL_NONE ? 0 : 1;
        if (fb->draw_count == count && (!count || fb->draw_buffers[0] == buffer))
        {
            ++sDrawBuffersKept;     // research/154 W1, as in draw_buffers
            return;
        }
        ++sTargetState;
        fb->draw_count = count;
        fb->draw_buffers[0] = buffer;
    }

    void read_buffer(GLenum buffer)
    {
        Framebuffer* fb = framebuffer_for(GL_READ_FRAMEBUFFER);
        if (fb) fb->read_buffer = buffer;
    }

    LLVKGL_WRAP_STATE(glBindFramebuffer, (GLenum target, GLuint framebuffer), (target, framebuffer), { bind_framebuffer(target, framebuffer); })
namespace
{
    LLVKGL_WRAP_STATE(glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level),
        (target, attachment, textarget, texture, level),
        {
            Framebuffer* fb = framebuffer_for(target);
            if (!fb) return;
            if (texture && (level != 0 || textarget != GL_TEXTURE_2D))
            {
                unhandled("framebuffer attachment target " + hex(textarget) + " level " + std::to_string(level));
            }
            ++sTargetState;
            if (texture)
            {
                // research/70: attachable from now on (remade if it was made without).
                // (get: a name not specified yet still remembers it for its first image.)
                make_attachable(sTextures.get(texture));
            }
            if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 4)
            {
                fb->color[attachment - GL_COLOR_ATTACHMENT0] = texture;
            }
            else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT)
            {
                fb->depth = texture;
            }
        })

    // The Vulkan side makes every attachment it is given renderable.
    GLenum APIENTRY wrap_glCheckFramebufferStatus(GLenum)
    {
        // Not counted (no GLCallScope): the counts are kept under sLock, and
        // this constant answer takes no lock.
        return GL_FRAMEBUFFER_COMPLETE;
    }
} // namespace
namespace
{
    LLVKGL_WRAP_STATE(glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers), (n, framebuffers),
        {
            for (GLsizei i = 0; i < n; ++i) sFramebuffers.erase(framebuffers[i]);
            ++sTargetState;
            release_names(NAMES_FRAMEBUFFER, n, framebuffers);
        })

    // Programs
    LLVKGL_WRAP_STATE(glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths), (shader, count, strings, lengths),
        {
            std::string source;
            for (GLsizei i = 0; i < count; ++i)
            {
                if (!strings[i]) continue;
                if (lengths && lengths[i] >= 0) source.append(strings[i], lengths[i]);
                else source.append(strings[i]);
            }
            GLint type = 0;
            glGetShaderiv(shader, GL_SHADER_TYPE, &type);
            sShaderSources[shader] = { (GLenum)type, source };
        })

    LLVKGL_WRAP_STATE(glAttachShader, (GLuint program, GLuint shader), (program, shader),
        { sAttached[program].push_back(shader); })

    LLVKGL_WRAP_STATE(glDetachShader, (GLuint program, GLuint shader), (program, shader),
        {
            auto& list = sAttached[program];
            list.erase(std::remove(list.begin(), list.end(), shader), list.end());
            own_shader_detached(shader);
        })

    LLVKGL_WRAP_STATE(glBindAttribLocation, (GLuint program, GLuint index, const GLchar* name), (program, index, name),
        { if (name) sPrograms[program].attrib_bindings[name] = index; })

    LLVKGL_WRAP_STATE(glLinkProgram, (GLuint program), (program),
        {
            link_program(program);
            own_link(program);  // what GL would answer about it (research/117 G3)
        })

    LLVKGL_WRAP_STATE(glDeleteProgram, (GLuint program), (program),
        {
            auto it = sPrograms.find(program);
            if (it != sPrograms.end())
            {
                destroy_program_objects(it->second);
                sPrograms.erase(it);
                ++sProgramChanges;
            }
            forget_depth_only_program(program);     // fix-pipeleak
            // GL detaches the program's shaders and deletes those marked (research/117 G3).
            std::vector<GLuint> detached;
            auto attached = sAttached.find(program);
            if (attached != sAttached.end())
            {
                detached.swap(attached->second);
                sAttached.erase(attached);
            }
            own_program_deleted(program, detached);
        })

    LLVKGL_WRAP_STATE(glProgramBinary, (GLuint program, GLenum format, const void* binary, GLsizei length), (program, format, binary, length),
        { unhandled("glProgramBinary (run with --set RenderShaderCacheEnabled 0)"); })

    GLint APIENTRY wrap_glGetUniformLocation(GLuint program, const GLchar* name)
    {
        LLVKGL_REDIRECT_QUERY("glGetUniformLocation", wrap_glGetUniformLocation(program, name))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const GLint location = own_uniform_location(program, name);     // research/117 G3
        record_location(program, name, location);
        return location;
    }

    GLuint APIENTRY wrap_glGetUniformBlockIndex(GLuint program, const GLchar* name)
    {
        LLVKGL_REDIRECT_QUERY("glGetUniformBlockIndex", wrap_glGetUniformBlockIndex(program, name))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const GLuint index = own_block_index(program, name);     // research/117 G3
        if (name && index != GL_INVALID_INDEX)
        {
            sPrograms[program].block_names[index] = name;
        }
        return index;
    }

    LLVKGL_WRAP_STATE(glUniformBlockBinding, (GLuint program, GLuint block, GLuint point), (program, block, point),
        {
            Program& p = sPrograms[program];
            auto name = p.block_names.find(block);
            ++sUniformBlockState;
            if (name != p.block_names.end())
            {
                p.block_points[name->second] = point;
                for (size_t i = 0; i < p.vk.blocks.size() && i < p.block_point.size(); ++i)
                {
                    if (p.vk.blocks[i].name == name->second) p.block_point[i] = (S32)point;
                }
            }
        })

    LLVKGL_WRAP_STATE(glUniform1i, (GLint loc, GLint v0), (loc, v0), { set_uniform(loc, 1, 1, &v0); })

    LLVKGL_WRAP_STATE(glUniform2i, (GLint loc, GLint v0, GLint v1), (loc, v0, v1), { GLint v[] = { v0, v1 }; set_uniform(loc, 1, 2, v); })

    LLVKGL_WRAP_STATE(glUniform1f, (GLint loc, GLfloat v0), (loc, v0), { set_uniform(loc, 1, 1, &v0); })

    LLVKGL_WRAP_STATE(glUniform2f, (GLint loc, GLfloat v0, GLfloat v1), (loc, v0, v1), { GLfloat v[] = { v0, v1 }; set_uniform(loc, 1, 2, v); })

    LLVKGL_WRAP_STATE(glUniform3f, (GLint loc, GLfloat v0, GLfloat v1, GLfloat v2), (loc, v0, v1, v2), { GLfloat v[] = { v0, v1, v2 }; set_uniform(loc, 1, 3, v); })

    LLVKGL_WRAP_STATE(glUniform4f, (GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3), (loc, v0, v1, v2, v3), { GLfloat v[] = { v0, v1, v2, v3 }; set_uniform(loc, 1, 4, v); })

    LLVKGL_WRAP_STATE(glUniform1iv, (GLint loc, GLsizei n, const GLint* v), (loc, n, v), { set_uniform(loc, n, 1, v); })

    LLVKGL_WRAP_STATE(glUniform4iv, (GLint loc, GLsizei n, const GLint* v), (loc, n, v), { set_uniform(loc, n, 4, v); })

    LLVKGL_WRAP_STATE(glUniform4uiv, (GLint loc, GLsizei n, const GLuint* v), (loc, n, v), { set_uniform(loc, n, 4, v); })

    LLVKGL_WRAP_STATE(glUniform1fv, (GLint loc, GLsizei n, const GLfloat* v), (loc, n, v), { set_uniform(loc, n, 1, v); })

    LLVKGL_WRAP_STATE(glUniform2fv, (GLint loc, GLsizei n, const GLfloat* v), (loc, n, v), { set_uniform(loc, n, 2, v); })

    LLVKGL_WRAP_STATE(glUniform3fv, (GLint loc, GLsizei n, const GLfloat* v), (loc, n, v), { set_uniform(loc, n, 3, v); })

    LLVKGL_WRAP_STATE(glUniform4fv, (GLint loc, GLsizei n, const GLfloat* v), (loc, n, v), { set_uniform(loc, n, 4, v); })

    LLVKGL_WRAP_STATE(glUniformMatrix2fv, (GLint loc, GLsizei n, GLboolean t, const GLfloat* v), (loc, n, t, v), { set_uniform_matrix(loc, n, 2, 2, t, v); })

    LLVKGL_WRAP_STATE(glUniformMatrix3fv, (GLint loc, GLsizei n, GLboolean t, const GLfloat* v), (loc, n, t, v), { set_uniform_matrix(loc, n, 3, 3, t, v); })

    LLVKGL_WRAP_STATE(glUniformMatrix4fv, (GLint loc, GLsizei n, GLboolean t, const GLfloat* v), (loc, n, t, v), { set_uniform_matrix(loc, n, 4, 4, t, v); })

    LLVKGL_WRAP_STATE(glUniformMatrix3x4fv, (GLint loc, GLsizei n, GLboolean t, const GLfloat* v), (loc, n, t, v), { set_uniform_matrix(loc, n, 3, 4, t, v); })

    // Vertex attributes
    LLVKGL_WRAP_STATE(glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer),
        (index, size, type, normalized, stride, pointer),
        {
            if (index < 32)
            {
                Attrib& a = sAttribs[index];
                a.size = size; a.type = type; a.normalized = normalized; a.integer = false;
                a.stride = stride; a.offset = (size_t)pointer; a.buffer = sArrayBuffer; a.resolved_at = U32_MAX;
                ++sVertexState;
            }
        })

    LLVKGL_WRAP_STATE(glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer),
        (index, size, type, stride, pointer),
        {
            if (index < 32)
            {
                Attrib& a = sAttribs[index];
                a.size = size; a.type = type; a.normalized = false; a.integer = true;
                a.stride = stride; a.offset = (size_t)pointer; a.buffer = sArrayBuffer; a.resolved_at = U32_MAX;
                ++sVertexState;
            }
        })

    LLVKGL_WRAP_STATE(glEnableVertexAttribArray, (GLuint index), (index), { if (index < 32) { sAttribs[index].enabled = true; ++sVertexState; } })

    LLVKGL_WRAP_STATE(glDisableVertexAttribArray, (GLuint index), (index), { if (index < 32) { sAttribs[index].enabled = false; ++sVertexState; } })

    LLVKGL_WRAP_STATE(glVertexAttrib4f, (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w), (index, x, y, z, w),
        { if (index < 32) { F32* c = sAttribs[index].constant; c[0] = x; c[1] = y; c[2] = z; c[3] = w; ++sVertexState; ++sAttribConstants; } })

    LLVKGL_WRAP_STATE(glVertexAttrib4fv, (GLuint index, const GLfloat* v), (index, v),
        { if (index < 32 && v) { memcpy(sAttribs[index].constant, v, 16); ++sVertexState; ++sAttribConstants; } })

} // namespace
    // Fixed-function state
    // Every write to sState that a draw reads must move sPipelineState (state
    // baked into pipelines) or sFixedState (dynamic state): the pass cache
    // (research/39 N3) trusts the state while neither moved.
    void set_cap(GLenum cap, bool on)
    {
        // Only blending and depth clamp are baked into pipelines; the other caps
        // are dynamic state (set_dynamic_state), so they keep the pipeline cache.
        bool* field = nullptr;
        switch (cap)
        {
        case GL_BLEND: field = &sState.blend; break;
        case GL_CULL_FACE: field = &sState.cull; break;
        case GL_DEPTH_TEST: field = &sState.depth_test; break;
        case GL_SCISSOR_TEST: field = &sState.scissor; break;
        case GL_POLYGON_OFFSET_FILL: field = &sState.polygon_offset; break;
        // GL_POLYGON_OFFSET_LINE is not translated (select-flicker): the official
        // selection outline enables it with glPolygonOffset(+3, +3), which pushes
        // the lines behind the faces they outline. If it ever is, check the sign first.
        case GL_DEPTH_CLAMP: field = &sState.depth_clamp; break;
        case GL_STENCIL_TEST: field = &sState.stencil; break;
        default: note_other_cap(cap, on); return;   // for glIsEnabled (research/117 G2)
        }
        if (!field || *field == on)
        {
            return;     // unchanged: the caches stay valid
        }
        *field = on;
        if (cap == GL_BLEND || cap == GL_DEPTH_CLAMP)
        {
            ++sPipelineState;
        }
        else
        {
            ++sFixedState;
        }
    }
namespace
{
    void set_stencil_func(GLenum face, GLenum func, GLint ref, GLuint mask)
    {
        if (face != GL_BACK) { sState.stencil_front.func = func; sState.stencil_front.ref = ref; sState.stencil_front.compare_mask = mask; }
        if (face != GL_FRONT) { sState.stencil_back.func = func; sState.stencil_back.ref = ref; sState.stencil_back.compare_mask = mask; }
        ++sFixedState;
    }

    void set_stencil_op(GLenum face, GLenum fail, GLenum depth_fail, GLenum pass)
    {
        if (face != GL_BACK) { sState.stencil_front.fail = fail; sState.stencil_front.depth_fail = depth_fail; sState.stencil_front.pass = pass; }
        if (face != GL_FRONT) { sState.stencil_back.fail = fail; sState.stencil_back.depth_fail = depth_fail; sState.stencil_back.pass = pass; }
        ++sFixedState;
    }

    void set_stencil_mask(GLenum face, GLuint mask)
    {
        if (face != GL_BACK) sState.stencil_front.write_mask = mask;
        if (face != GL_FRONT) sState.stencil_back.write_mask = mask;
        ++sFixedState;
    }

    LLVKGL_WRAP_STATE(glEnable, (GLenum cap), (cap), { set_cap(cap, true); })

    LLVKGL_WRAP_STATE(glStencilFunc, (GLenum func, GLint ref, GLuint mask), (func, ref, mask), { set_stencil_func(GL_FRONT_AND_BACK, func, ref, mask); })

    LLVKGL_WRAP_STATE(glStencilOp, (GLenum fail, GLenum zfail, GLenum zpass), (fail, zfail, zpass), { set_stencil_op(GL_FRONT_AND_BACK, fail, zfail, zpass); })

    LLVKGL_WRAP_STATE(glStencilMask, (GLuint mask), (mask), { set_stencil_mask(GL_FRONT_AND_BACK, mask); })

    LLVKGL_WRAP_STATE(glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a), { set_clear_color(r, g, b, a); })

    LLVKGL_WRAP_STATE(glClearStencil, (GLint s), (s), { sState.clear_stencil = s; })

    // Clears and draws: replayed into the Vulkan frame.
    LLVKGL_WRAP_DRAW(glDrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count),
        { draw(mode, first, count, 1, IndexInfo()); })

    LLVKGL_WRAP_DRAW(glDrawRangeElements, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices),
        (mode, start, end, count, type, indices),
        { draw(mode, 0, count, 1, { true, type, (size_t)indices }); })

    // Occlusion queries: Vulkan's (GL draws nothing). The bodies (under sLock)
    // are shared with the native entry points below.
    void begin_query(GLenum target, GLuint id)
    {
        if (sOcclusionMode && (target == GL_ANY_SAMPLES_PASSED || target == GL_SAMPLES_PASSED))
        {
            if (GLQuery* q = gl_query(id))
            {
                *q = GLQuery();
                q->frame = sFrame;
                sActiveQuery = id;
            }
        }
    }

    void end_query(GLenum target)
    {
        if (target == GL_ANY_SAMPLES_PASSED || target == GL_SAMPLES_PASSED)
        {
            sActiveQuery = 0;
        }
    }

    LLVKGL_WRAP_STATE(glBeginQuery, (GLenum target, GLuint id), (target, id), { begin_query(target, id); })

    LLVKGL_WRAP_STATE(glEndQuery, (GLenum target), (target), { end_query(target); })
    // glGetQueryObjectuiv; false: not an occlusion result (other pnames, answered 0).
    bool get_query_object(GLuint id, GLenum pname, GLuint* params)
    {
        if (!params || (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE))
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        read_query(id, pname, params);
        if (sOcclusionMode != 2)
        {
            *params = 1;    // boxes marked but unused (mode 1) or skipped (0); never hidden
        }
        return true;
    }

    void APIENTRY wrap_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params)
    {
        LLVKGL_REDIRECT("glGetQueryObjectuiv", wrap_glGetQueryObjectuiv(id, pname, params))
        if (!get_query_object(id, pname, params) && params)
        {
            *params = 0;
        }
    }

} // namespace
    // Native occlusion queries (research/42): LLOcclusionCullingGroup and
    // LLReflectionMap call these instead of the wrappers above (thousands a
    // frame at a heavy place). Same bodies and lock, without the GL entry
    // point and call stats.
    void native_begin_query(U32 target, U32 name)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        begin_query(target, name);
    }

    void native_end_query(U32 target)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        end_query(target);
    }

    void native_get_query_object(U32 name, U32 pname, U32* params)
    {
        if (!get_query_object(name, pname, params) && params)
        {
            *params = 0;
        }
    }

#undef LLVKGL_WRAP_DRAW
#undef LLVKGL_WRAP_STATE

    // The costliest GL calls since the last report, per frame: "name count ms".
    // Called under sLock (LLVKGL::perfReport), which guards the counts.
    std::string gl_call_report(U32 frames)
    {
        if (!sPerf)
        {
            return std::string();
        }
        std::vector<GLCallStat*> stats;
        {
            std::lock_guard<std::mutex> lock(sGLCallStatsMutex);
            stats = gl_call_stats();
        }
        std::sort(stats.begin(), stats.end(), [](const GLCallStat* a, const GLCallStat* b) { return a->ticks > b->ticks; });
        const F64 ticks_per_ns = perf_ticks_per_ns();
        std::ostringstream out;
        for (size_t i = 0; i < stats.size() && i < 12 && stats[i]->count; ++i)
        {
            out << (i ? ", " : "") << stats[i]->name << " " << stats[i]->count / frames << " "
                << (F64)stats[i]->ticks / ticks_per_ns / frames / 1e6;
        }
        for (GLCallStat* stat : stats)
        {
            stat->ticks = 0;
            stat->count = 0;
        }
        return out.str();
    }

namespace
{
    // research/218 vk-s7f (research/207 7.3 T10): every wrapped GL entry
    // point called since the last report, most called first, "name N"
    // (totals). Under sLock, which guards the counts.
    std::string gl_entry_report()
    {
        std::vector<GLCallStat*> stats;
        {
            std::lock_guard<std::mutex> lock(sGLCallStatsMutex);
            stats = gl_call_stats();
        }
        std::stable_sort(stats.begin(), stats.end(), [](const GLCallStat* a, const GLCallStat* b) { return a->calls > b->calls; });
        std::ostringstream out;
        bool any = false;
        for (GLCallStat* stat : stats)
        {
            if (stat->calls)
            {
                out << (any ? ", " : "") << stat->name << " " << stat->calls;
                any = true;
                stat->calls = 0;
            }
        }
        return any ? out.str() : std::string("none");
    }
} // namespace
} // namespace llvkgl

// research/218 vk-s7f (research/207 7.1, 7.3): the old paths' use since the
// last report, totals, for one VKPerf line. Taken before perfReport (which
// starts the publication's hits and the direct refusals over).
std::string LLVKGL::oldPathReport(U32 frames)
{
    std::lock_guard<LLVKTrackerMutex> lock(llvkgl::sLock);
    std::ostringstream out;
    out << "old paths over " << llmax(frames, 1u) << " frames (research/207 7.1, totals): " << llvkgl::old_texture_path_report()
        << ", " << LLVKAttribPool::takeOldPathStats()
        << ", material names " << LLRHI::sOldMaterialNames.exchange(0, std::memory_order_relaxed)
        << "; gl entry: " << llvkgl::gl_entry_report()
        << "; direct refusals [" << llvkgl::direct_refusals(1) << "]";
    return out.str();
}

// A GL pointer (the table's stub, research/117 G5) is replaced by its
// wrapper: non-null after install_gl_wraps() exactly when it was there and is
// now wrapped, which is what the checks of install_native_hooks()
// (llvkgl_hooks.cpp) read.
#define LLVKGL_REPLACE(fn) if (fn) { fn = wrap_##fn; }

void llvkgl::install_gl_wraps()
{
    LLVKGL_REPLACE(glBindBuffer)
    LLVKGL_REPLACE(glBufferData)
    LLVKGL_REPLACE(glDeleteBuffers)
    LLVKGL_REPLACE(glBindBufferBase)

    LLVKGL_REPLACE(glDeleteTextures)
    LLVKGL_REPLACE(glPixelStorei)
    LLVKGL_REPLACE(glTexImage2D)
    LLVKGL_REPLACE(glCompressedTexImage2D)
    LLVKGL_REPLACE(glTexSubImage2D)
    LLVKGL_REPLACE(glTexImage3D)
    LLVKGL_REPLACE(glTexSubImage3D)
    LLVKGL_REPLACE(glCompressedTexSubImage3D)   // research/288
    LLVKGL_REPLACE(glCopyTexSubImage2D)
    LLVKGL_REPLACE(glGetTexImage)
    LLVKGL_REPLACE(glReadPixels)
    LLVKGL_REPLACE(glGetCompressedTexImage)
    LLVKGL_REPLACE(glGetTexLevelParameteriv)
    LLVKGL_REPLACE(glCopyTexSubImage3D)
    LLVKGL_REPLACE(glGenerateMipmap)
    LLVKGL_REPLACE(glTexParameteriv)

    LLVKGL_REPLACE(glBindFramebuffer)
    LLVKGL_REPLACE(glFramebufferTexture2D)
    LLVKGL_REPLACE(glCheckFramebufferStatus)
    LLVKGL_REPLACE(glDeleteFramebuffers)

    LLVKGL_REPLACE(glShaderSource)
    LLVKGL_REPLACE(glAttachShader)
    LLVKGL_REPLACE(glDetachShader)
    LLVKGL_REPLACE(glBindAttribLocation)
    LLVKGL_REPLACE(glLinkProgram)
    LLVKGL_REPLACE(glDeleteProgram)
    LLVKGL_REPLACE(glProgramBinary)
    LLVKGL_REPLACE(glGetUniformLocation)
    LLVKGL_REPLACE(glGetUniformBlockIndex)
    LLVKGL_REPLACE(glUniformBlockBinding)
    LLVKGL_REPLACE(glUniform1i)
    LLVKGL_REPLACE(glUniform2i)
    LLVKGL_REPLACE(glUniform1f)
    LLVKGL_REPLACE(glUniform2f)
    LLVKGL_REPLACE(glUniform3f)
    LLVKGL_REPLACE(glUniform4f)
    LLVKGL_REPLACE(glUniform1iv)
    LLVKGL_REPLACE(glUniform4iv)
    LLVKGL_REPLACE(glUniform4uiv)
    LLVKGL_REPLACE(glUniform1fv)
    LLVKGL_REPLACE(glUniform2fv)
    LLVKGL_REPLACE(glUniform3fv)
    LLVKGL_REPLACE(glUniform4fv)
    LLVKGL_REPLACE(glUniformMatrix2fv)
    LLVKGL_REPLACE(glUniformMatrix3fv)
    LLVKGL_REPLACE(glUniformMatrix4fv)
    LLVKGL_REPLACE(glUniformMatrix3x4fv)

    LLVKGL_REPLACE(glVertexAttribPointer)
    LLVKGL_REPLACE(glVertexAttribIPointer)
    LLVKGL_REPLACE(glEnableVertexAttribArray)
    LLVKGL_REPLACE(glDisableVertexAttribArray)
    LLVKGL_REPLACE(glVertexAttrib4f)
    LLVKGL_REPLACE(glVertexAttrib4fv)

    LLVKGL_REPLACE(glEnable)
    LLVKGL_REPLACE(glStencilFunc)
    LLVKGL_REPLACE(glStencilOp)
    LLVKGL_REPLACE(glStencilMask)
    LLVKGL_REPLACE(glClearColor)
    LLVKGL_REPLACE(glClearStencil)

    LLVKGL_REPLACE(glDrawArrays)
    LLVKGL_REPLACE(glDrawRangeElements)
    LLVKGL_REPLACE(glGetQueryObjectuiv)
    LLVKGL_REPLACE(glBeginQuery)
    LLVKGL_REPLACE(glEndQuery)
}

#undef LLVKGL_REPLACE

#else // !LL_GL_FUNC_POINTER

std::string LLVKGL::oldPathReport(U32) { return std::string(); }

#endif // LL_GL_FUNC_POINTER
