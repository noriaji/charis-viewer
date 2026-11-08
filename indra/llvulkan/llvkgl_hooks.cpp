/**
 * @file llvkgl_hooks.cpp
 * @brief The native API LLRHI hands the viewer instead of GL (shaders, palettes, texture binding and uploads, state and render targets) and install_native_hooks (research/39 N4, research/91).
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
#include "llrhipass.h"        // research/156 N12c: sBindTarget
#include "llvkavatar.h"       // research/199 (stage 4b): resident avatar palette rows

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
namespace
{
    // ------------------------------------------------------------------
    // Native shader API (research/39 N4)
    //
    // LLGLSLShader binds programs and sets uniforms through these (llrhi.h)
    // instead of glUseProgram / glUniform*: no GL function pointer, call stats,
    // current program resolution or location map lookup per call. A handle is
    // the Program in sPrograms (stable until glDeleteProgram erases it; the
    // shader drops it before deleting). A slot is an index into the program's
    // native_slots, which link_program clears with `locations`: a slot from
    // before a relink is refused (false, the caller goes through GL) until new
    // slots are handed out; LLGLSLShader drops its slots when it relinks.
    // GL gets no program state of its own, so skipping the GL call here
    // changes nothing on the GL side.

    LLRHI::ShaderProgram* native_shader_program(U32 gl_program)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto it = sPrograms.find(gl_program);
        if (it == sPrograms.end() || !it->second.built)
        {
            return nullptr;     // not translated (or failed to build): the caller stays on GL
        }
        return reinterpret_cast<LLRHI::ShaderProgram*>(&it->second);
    }

    S32 native_uniform_slot(LLRHI::ShaderProgram* handle, S32 location)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Program* program = reinterpret_cast<Program*>(handle);
        if (!program || !program->built || location < 0)
        {
            return -1;
        }
        auto known = program->native_slot_of.find(location);
        if (known != program->native_slot_of.end())
        {
            return known->second;
        }
        auto loc = program->locations.find(location);
        if (loc == program->locations.end())
        {
            return -1;  // not recorded (not reflected): the GL wrapper ignores it too
        }
        const S32 slot = (S32)program->native_slots.size();
        program->native_slots.push_back(loc->second);
        program->native_slot_of[location] = slot;
        return slot;
    }

    void native_use_program(LLRHI::ShaderProgram* handle, U32 gl_program)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        sCurrentProgram = gl_program;   // thread_local (the only writer since vk-s7g retired wrap_glUseProgram)
        if (handle)
        {
            // Saves the draw's lookup in current_program(); the handle is this
            // program's entry (the shader drops it before glDeleteProgram).
            sCurrentResolved = reinterpret_cast<Program*>(handle);
            sCurrentResolvedName = gl_program;
            sCurrentResolvedAt = sProgramChanges;
        }
    }

    bool native_set_uniform(LLRHI::ShaderProgram* handle, S32 slot, U32 count, U32 components, const void* data)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Program* program = reinterpret_cast<Program*>(handle);
        if (!program || !program->built || slot < 0 || (size_t)slot >= program->native_slots.size() || !data)
        {
            return false;
        }
        apply_uniform(*program, program->native_slots[slot], (GLsizei)count, components, data);
        return true;
    }

    bool native_set_uniform_matrix(LLRHI::ShaderProgram* handle, S32 slot, U32 count, U32 columns, U32 rows, bool transpose, const F32* data)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Program* program = reinterpret_cast<Program*>(handle);
        if (!program || !program->built || slot < 0 || (size_t)slot >= program->native_slots.size() || !data)
        {
            return false;
        }
        apply_uniform_matrix(*program, program->native_slots[slot], (GLsizei)count, columns, rows, transpose ? GL_TRUE : GL_FALSE, data);
        return true;
    }

    // ------------------------------------------------------------------
    // Prebuilt skinning palettes (research/56, LLRHI::sPalettePrebuild)

    // PREBUILT_PALETTE_ROOM: llvkgl_drawpath.h (shared with the draw lists, research/137 N7).

    // LLRHI::sPlacePalette: the joints in the frame's table once a frame.
    bool native_place_palette(const F32* data, U32 vec4s, LLRHI::PaletteRow& row)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (row.frame == sFrame)
        {
            return row.room != 0;   // placed this frame (0: it did not fit, asked once)
        }
        if (!data || !vec4s || vec4s > PREBUILT_PALETTE_ROOM)
        {
            return false;
        }
        U32 base = 0;
        F32* dst = LLVKBindless::paletteRows(PREBUILT_PALETTE_ROOM, base);
        row.frame = sFrame;
        if (!dst)
        {
            // The table is full this frame (it grows for the next): the joints go
            // as a uniform, as without rows; not asked again this frame.
            row.base = 0;
            row.room = 0;
            return false;
        }
        // As apply_palette and palette_row would put them: mat3x4 columns as given.
        memcpy(dst, data, (size_t)vec4s * 16);
        row.base = base;
        row.room = PREBUILT_PALETTE_ROOM;
        ++sPalettePlaced;
        return true;
    }

    // LLRHI::sUsePaletteRow: the program's draws read the row, as if its
    // palette uniform had been set to those joints.
    bool native_use_palette_row(LLRHI::ShaderProgram* handle, const LLRHI::PaletteRow& row)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Program* program = reinterpret_cast<Program*>(handle);
        if (!palette_row_takes(program, row))
        {
            return false;
        }
        program->palette_prebuilt = true;
        program->palette_prebuilt_frame = sFrame;
        program->palette_prebuilt_base = row.base;
        return true;
    }

    // LLRHI::sAvatarBackendReport (research/56): this side's part of the RHIAvatar line.
    std::string native_avatar_report(U32 frames)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        frames = llmax(frames, 1u);
        const F64 per = 1.0 / frames;
        const F64 ticks_per_us = perf_ticks_per_ns() * 1000.0;
        U32 shadow_copies = 0, shadow_reused = 0, shadow_rows = 0;
        LLVKNative::takePaletteStats(shadow_copies, shadow_reused, shadow_rows);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "palette uniform writes " << sAvPaletteWrites * per << " (" << sAvPaletteWriteTicks / ticks_per_us * per << " us)"
            << ", table copies by content " << sAvPaletteRows * per << " (" << sAvPaletteRowTicks / ticks_per_us * per << " us)"
            << ", draws on a prebuilt row " << sAvPaletteNamed * per
            << "; native shadow palettes copied " << shadow_copies * per << " reused " << shadow_reused * per << " from the table "
            << shadow_rows * per
            << "; rigged direct draws " << sAvRiggedDraws * per << ", in draw runs " << sAvRiggedRun * per << " (straight "
            << sAvRiggedStraight * per << ", templates " << sAvRiggedTemplate * per << ")";
        sAvPaletteWrites = sAvPaletteRows = sAvPaletteNamed = 0;
        sAvRiggedDraws = sAvRiggedRun = sAvRiggedStraight = sAvRiggedTemplate = 0;
        sAvPaletteWriteTicks = sAvPaletteRowTicks = 0;
        return out.str();
    }

    // ------------------------------------------------------------------
    // Native texture binding API (research/39 N4)
    //
    // LLTexUnit calls these (LLRHI::sActiveTexture, sBindTexture, sTexParameters)
    // instead of glActiveTexture / glBindTexture / glTexParameter*: the same
    // tracking as the wrappers (llvkgl_entry.cpp), on the calling thread's units, with one
    // lock per bind (none to change the active unit) and no GL entry point or
    // call statistics. GL gets none of them, like from the wrappers.

    void native_active_texture(U32 unit)
    {
        if (unit > 31u)
        {
            return;     // as native_bind_texture: not a real unit, the active one stays (31 is not taken in its place)
        }
        sActiveUnit = (GLuint)unit;     // thread_local: no lock needed
    }

    void native_bind_texture(U32 unit, U32 target, U32 name)
    {
        if (unit > 31u)
        {
            return;     // LLTexUnit's dummy unit (index -1): binds nothing, never a real unit
        }
        const GLuint u = unit;
        sActiveUnit = u;
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        bind_texture((GLenum)target, (GLuint)name);
    }

    void native_tex_parameters(U32 target, const LLRHI::TexParameter* params, U32 count)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        bool changed = false;
        for (U32 i = 0; i < count; ++i)
        {
            changed |= tex_parameter((GLenum)target, params[i].pname, params[i].value);
        }
        if (changed)
        {
            touch_bound((GLenum)target);
        }
    }


    // ------------------------------------------------------------------
    // Native texture upload API (research/39 N4)
    //
    // LLImageGL creates and fills textures through these (LLRHI::sTexImage2D,
    // sCompressedTexImage2D, sTexSubImage2D, sGenerateMipmap) instead of the GL
    // entry points: the texture is named, not taken from the active unit, and
    // the work is the wrappers' own (the same helpers, version and state bumps,
    // unpack row length and alignment); GL gets no texture data from either. A
    // call returns false, and does nothing, when GL must see it after all: a
    // texture image without pixels (storage that render targets and
    // framebuffers rely on), a pixel unpack buffer bound (pixels are an offset
    // into it), or a texture of another target kind; the caller then uses GL.

    // The texture `name` as binding it to target would give it (bind_texture);
    // null when it is not one GL would accept for that target.
    Texture* named_texture(GLuint name, GLenum target)
    {
        if (!name)
        {
            return nullptr;
        }
        const GLenum kind = bind_target(target);
        Texture& tex = sTextures.get(name);
        if (!tex.target) tex.target = kind;
        return tex.target == kind ? &tex : nullptr;
    }

    bool native_tex_image_2d(U32 name, U32 target, S32 level, U32 internal, S32 width, S32 height, U32 format, U32 type, const void* pixels)
    {
        if (!pixels || sUnpackBuffer)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        tex_image(tex, target, level, (GLint)internal, width, height, format, type, pixels, false, 0);
        return true;
    }

    bool native_compressed_tex_image_2d(U32 name, U32 target, S32 level, U32 internal, S32 width, S32 height, S32 size, const void* data)
    {
        if (sUnpackBuffer)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        tex_image(tex, target, level, (GLint)internal, width, height, 0, 0, data, true, (size_t)size);
        return true;
    }

    bool native_tex_sub_image_2d(U32 name, U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels)
    {
        if (sUnpackBuffer)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        tex_sub_image(tex, target, level, x, y, width, height, format, type, pixels);
        return true;
    }

    bool native_generate_mipmap(U32 name, U32 target)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        generate_mipmaps(tex);
        queue_for_bcn(name, *tex);      // research/88: complete now, may be stored BC7
        return true;
    }

    // glCopyTexSubImage3D into the texture `name` (reflection probe cube map
    // arrays), from the bound read framebuffer like the wrapper.
    bool native_copy_tex_sub_image_3d(U32 name, U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset,
                                      S32 x, S32 y, S32 width, S32 height)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        copy_from_framebuffer(tex, level, xoffset, yoffset, (U32)zoffset, x, y, width, height);
        return true;
    }

    // Storage only, and only the Vulkan image: GL gets nothing (the wrapper
    // would give GL storage too, which only framebuffer attachments need).
    bool native_tex_storage_2d(U32 name, U32 target, S32 level, U32 internal, S32 width, S32 height, U32 format, U32 type)
    {
        if (sUnpackBuffer)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        tex_image(tex, target, level, (GLint)internal, width, height, format, type, nullptr, false, 0);
        return true;
    }

    // What wrap_glCopyTexSubImage2D does, into the named texture.
    bool native_copy_tex_sub_image_2d(U32 name, U32 target, S32 level, S32 xoffset, S32 yoffset, S32 x, S32 y, S32 width, S32 height)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        if (!tex)
        {
            return false;
        }
        touch_texture(tex);
        const U32 layer = is_cube_face(target) ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
        copy_from_framebuffer(tex, level, xoffset, yoffset, layer, x, y, width, height);
        return true;
    }

    // texture-leak (2026-10-27): LLImageGL::scaleDown's image swap (tex_scale_down).
    bool native_tex_scale_down(U32 name, U32 target, U32 internal, S32 width, S32 height)
    {
        if (target != GL_TEXTURE_2D)
        {
            return false;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        Texture* tex = named_texture(name, target);
        return tex && tex_scale_down(tex, (GLint)internal, width, height);
    }

    // Unpack state for the native uploads.
    void native_pixel_storei(U32 pname, S32 value)
    {
        if (pname == GL_UNPACK_ALIGNMENT)
        {
            sUnpackAlignment = llmax(1, value);
        }
        else if (pname == GL_UNPACK_ROW_LENGTH)
        {
            sUnpackRowLength = value;
        }
        else
        {
            wrap_glPixelStorei(pname, value);
        }
    }

} // namespace
    // ------------------------------------------------------------------
    // Native state and render target API (research/39 N4)
    //
    // LLGLState, LLGLDepthTest, LLRender (blend, color mask, line width) and
    // LLRenderTarget call these through LLRHI instead of the GL entry points.
    // Same tracking as the wrappers (llvkgl_entry.cpp), without the wrapper chain (trace
    // and profiler hooks, GLCallScope); FBO 0 is the frame's default target.
    // No sLock for state: sState and sFramebuffers are per thread, and
    // sPipelineState / sTargetState are atomic, compared for change against a
    // snapshot the render thread cached itself, so a texture thread bumping them
    // at the same time cannot hide a change. Clears record Vulkan commands and
    // take the lock like the wrapper.
    U32 sNativeStateCalls = 0;
    U32 sBindTargets = 0;   // research/156 N12c: native_bind_target calls (LLVKGL::counterReport)
namespace
{

    void native_set_cap(U32 cap, bool on)
    {
        ++sNativeStateCalls;
        set_cap(cap, on);
    }

    void native_blend_func(U32 src_rgb, U32 dst_rgb, U32 src_a, U32 dst_a)
    {
        ++sNativeStateCalls;
        set_blend_func(src_rgb, dst_rgb, src_a, dst_a);
    }

    void native_color_mask(bool r, bool g, bool b, bool a)
    {
        ++sNativeStateCalls;
        set_color_mask(r, g, b, a);
    }

    void native_depth_func(U32 func)
    {
        ++sNativeStateCalls;
        set_depth_func(func);
    }

    void native_depth_mask(bool write)
    {
        ++sNativeStateCalls;
        set_depth_mask(write);
    }

    void native_line_width(F32 width)
    {
        ++sNativeStateCalls;
        sState.line_width = width;
    }

    void native_cull_face(U32 mode)
    {
        ++sNativeStateCalls;
        set_cull_face(mode);
    }

    void native_viewport(S32 x, S32 y, S32 w, S32 h)
    {
        ++sNativeStateCalls;
        set_viewport(x, y, w, h);
    }

    void native_scissor(S32 x, S32 y, S32 w, S32 h)
    {
        ++sNativeStateCalls;
        set_scissor(x, y, w, h);
    }

    void native_polygon_offset(F32 factor, F32 units)
    {
        ++sNativeStateCalls;
        set_polygon_offset(factor, units);
    }

    void native_polygon_mode(U32 face, U32 mode)
    {
        ++sNativeStateCalls;
        set_polygon_mode(mode);
    }

    void native_clear_color(F32 r, F32 g, F32 b, F32 a)
    {
        ++sNativeStateCalls;
        set_clear_color(r, g, b, a);
    }

    void native_bind_framebuffer(U32 target, U32 framebuffer)
    {
        ++sNativeStateCalls;
        bind_framebuffer(target, framebuffer);
    }

    void native_draw_buffers(S32 count, const U32* buffers)
    {
        ++sNativeStateCalls;
        draw_buffers(count, buffers);
    }

    void native_draw_buffer(U32 buffer)
    {
        ++sNativeStateCalls;
        draw_buffer(buffer);
    }

    void native_read_buffer(U32 buffer)
    {
        ++sNativeStateCalls;
        read_buffer(buffer);
    }

    // research/156 N12c: LLRenderTarget's bind in one call (the four
    // functions above, as LLRenderTarget::bindTarget and flush call them).
    void native_bind_target(U32 framebuffer, U32 color_count, S32 x, S32 y, S32 width, S32 height)
    {
        ++sNativeStateCalls;
        ++sBindTargets;
        bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
        if (framebuffer)
        {
            static const GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
            if (color_count)
            {
                draw_buffers((GLsizei)llmin(color_count, 4u), buffers);
                read_buffer(GL_COLOR_ATTACHMENT0);
            }
            else
            {
                draw_buffer(GL_NONE);
                read_buffer(GL_NONE);
            }
        }
        // (The frame's own target: draw and read buffers GL_BACK, which name nothing to track.)
        set_viewport(x, y, width, height);
    }

    void native_clear(U32 mask)
    {
        ++sNativeStateCalls;
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        clear(mask);
    }
} // namespace
} // namespace llvkgl

void llvkgl::install_native_hooks()
{
    // Native shader API (research/39 N4): LLGLSLShader skips glUseProgram /
    // glUniform* for translated programs (GL gets no program state anyway).
    if (LLRHI::sNativeShader)
    {
        LLRHI::sShaderProgram = &native_shader_program;
        LLRHI::sUniformSlot = &native_uniform_slot;
        LLRHI::sUseProgram = &native_use_program;
        LLRHI::sSetUniform = &native_set_uniform;
        LLRHI::sSetUniformMatrix = &native_set_uniform_matrix;
        // Research/56: programs take prebuilt palette rows (GL needs no uniforms here either).
        LLRHI::sUsePaletteRow = &native_use_palette_row;
    }
    // Research/56: palettes placed in the frame's table once a frame (also for
    // the native shadow casters, without the native shader API).
    LLRHI::sPlacePalette = &native_place_palette;
    LLRHI::sAvatarBackendReport = &native_avatar_report;
    // Research/199 (stage 4b): resident avatar palette rows and the compute
    // skinning check (LLRHI_AVATAR_PALETTE, LLRHI_COMPUTE_SKIN; off by default).
    LLVKAvatar::install();

    // Native uniform buffer bindings (research/42): the reflection probe block.
    if (glBindBufferBase)
    {
        LLRHI::sBindBufferBase = &native_bind_buffer_base;
    }

    // Native texture binding (research/39 N4): LLTexUnit's only way to the
    // units and sampler options (research/218 vk-s7g: glActiveTexture,
    // glBindTexture and glTexParameteri/f have no wrapper any more, nothing in
    // the viewer calls them; LLVertexBuffer::initClass needs these hooks).
    {
        LLRHI::sActiveTexture = &native_active_texture;
        LLRHI::sBindTexture = &native_bind_texture;
        LLRHI::sTexParameters = &native_tex_parameters;
    }
    // Native texture uploads (research/39 N4): LLImageGL skips the wrappers
    // (llvkgl_entry.cpp) for texture data.
    if (glTexImage2D && glCompressedTexImage2D && glTexSubImage2D && glGenerateMipmap)
    {
        LLRHI::sTexImage2D = &native_tex_image_2d;
        LLRHI::sCompressedTexImage2D = &native_compressed_tex_image_2d;
        LLRHI::sTexSubImage2D = &native_tex_sub_image_2d;
        LLRHI::sGenerateMipmap = &native_generate_mipmap;
        LLRHI::sTexStorage2D = &native_tex_storage_2d;
        LLRHI::sCopyTexSubImage2D = glCopyTexSubImage2D ? &native_copy_tex_sub_image_2d : nullptr;
        LLRHI::sTexScaleDown = &native_tex_scale_down;
        LLRHI::sPixelStorei = glPixelStorei ? &native_pixel_storei : nullptr;
        // Texture creation in batches (research/70).
        LLVKMirror::sBeforeSubmit = &join_before_submit;
        LLRHI::sTexConvertJoin = &join_tex_converts;
    }
    if (glCopyTexSubImage3D)
    {
        LLRHI::sCopyTexSubImage3D = &native_copy_tex_sub_image_3d;
    }

    // Native occlusion queries (research/42): only with all three wrapped.
    if (glGetQueryObjectuiv && glBeginQuery && glEndQuery)
    {
        LLRHI::sBeginQuery = &native_begin_query;
        LLRHI::sEndQuery = &native_end_query;
        LLRHI::sGetQueryObjectuiv = &native_get_query_object;
    }

    // Native state and render targets (research/39 N4): the viewer's state
    // classes call the functions above, which write the tracked state
    // directly (no GL entry point behind them). research/207 T4: always (the
    // switch LLRHI_NATIVE_STATE=0 and the GL fallbacks are gone): the viewer's
    // copy (llrender) writes the backend's state through these only.
    {
        LLRHI::sSetCap = &native_set_cap;
        LLRHI::sBlendFunc = &native_blend_func;
        LLRHI::sColorMask = &native_color_mask;
        LLRHI::sDepthFunc = &native_depth_func;
        LLRHI::sDepthMask = &native_depth_mask;
        LLRHI::sLineWidth = &native_line_width;
        LLRHI::sCullFace = &native_cull_face;
        LLRHI::sViewport = &native_viewport;
        LLRHI::sScissor = &native_scissor;
        LLRHI::sPolygonOffset = &native_polygon_offset;
        LLRHI::sPolygonMode = &native_polygon_mode;
        LLRHI::sClearColor = &native_clear_color;
        LLRHI::sBindFramebuffer = &native_bind_framebuffer;
        LLRHI::sDrawBuffers = &native_draw_buffers;
        LLRHI::sDrawBuffer = &native_draw_buffer;
        LLRHI::sReadBuffer = &native_read_buffer;
        LLRHI::sClear = &native_clear;
        // research/156 N12c: LLRHI_BIND_TARGET=1 (off by default).
        const char* bind_target = std::getenv("LLRHI_BIND_TARGET");
        if (bind_target && *bind_target == '1')
        {
            LLRHI::sBindTarget = &native_bind_target;
        }
    }
}

#endif // LL_GL_FUNC_POINTER
