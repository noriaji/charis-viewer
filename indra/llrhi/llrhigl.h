/**
 * @file llrhigl.h
 * @brief GL state calls that go straight to the render backend when it has a native entry point.
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

#ifndef LL_LLRHIGL_H
#define LL_LLRHIGL_H

#include "llglheaders.h"
#include "llrhi.h"

// Drop-in replacements for the GL state calls the viewer makes outside its
// state classes (research/39 N4): each one calls the backend's native entry
// point, so the call site needs a one-word change only. Same arguments and
// meaning as the GL call. <plus> research/207 T4: the backend installs the
// state hooks always (LLVertexBuffer::initClass stops without them), so the
// GL fallbacks of the first six, drawBuffers and texParameteri are gone; a call before the
// install (none is made) does nothing. The others still hand to the backend's
// GL entry points what their hook does not take. </plus>
namespace LLRHI
{
    inline void scissor(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        if (sScissor) sScissor(x, y, width, height);
    }

    inline void viewport(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        if (sViewport) sViewport(x, y, width, height);
    }

    inline void clear(GLbitfield mask)
    {
        if (sClear) sClear(mask);
    }

    inline void clearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
    {
        if (sClearColor) sClearColor(r, g, b, a);
    }

    inline void polygonOffset(GLfloat factor, GLfloat units)
    {
        if (sPolygonOffset) sPolygonOffset(factor, units);
    }

    inline void polygonMode(GLenum face, GLenum mode)
    {
        if (sPolygonMode) sPolygonMode(face, mode);
    }

    // Of the bound draw framebuffer, like glDrawBuffers (research/207 6.3).
    inline void drawBuffers(GLsizei n, const GLenum* buffers)
    {
        if (sDrawBuffers) sDrawBuffers(n, buffers);
    }

    // Unpack row length and alignment go native; other parameters to GL.
    inline void pixelStorei(GLenum pname, GLint value)
    {
        if (sPixelStorei && (pname == GL_UNPACK_ROW_LENGTH || pname == GL_UNPACK_ALIGNMENT)) sPixelStorei(pname, value);
        else glPixelStorei(pname, value);
    }

    // On the active unit's texture, like glTexParameteri.
    inline void texParameteri(GLenum target, GLenum pname, GLint value)
    {
        if (sTexParameters)
        {
            const TexParameter param{ pname, (F32)value };
            sTexParameters(target, &param, 1);
        }
    }

    inline void beginQuery(GLenum target, GLuint name)
    {
        if (sBeginQuery) sBeginQuery(target, name);
        else glBeginQuery(target, name);
    }

    inline void endQuery(GLenum target)
    {
        if (sEndQuery) sEndQuery(target);
        else glEndQuery(target);
    }

    inline void getQueryObjectuiv(GLuint name, GLenum pname, GLuint* params)
    {
        if (sGetQueryObjectuiv) sGetQueryObjectuiv(name, pname, params);
        else glGetQueryObjectuiv(name, pname, params);
    }

    inline void bindBufferBase(GLenum target, GLuint index, GLuint buffer)
    {
        if (sBindBufferBase) sBindBufferBase(target, index, buffer);
        else glBindBufferBase(target, index, buffer);
    }

    // Into the texture `name`, which the caller has bound to target on the
    // active unit (what GL writes to when the backend leaves the call to it).
    inline void copyTexSubImage3D(GLuint name, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                  GLint x, GLint y, GLsizei width, GLsizei height)
    {
        if (!sCopyTexSubImage3D || !sCopyTexSubImage3D(name, target, level, xoffset, yoffset, zoffset, x, y, width, height))
        {
            glCopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height);
        }
    }
}

#endif // LL_LLRHIGL_H
