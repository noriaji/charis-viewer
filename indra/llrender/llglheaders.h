/**
 * @file llglheaders.h
 * @brief LLGL definitions
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLGLHEADERS_H
#define LL_LLGLHEADERS_H

#if LL_WINDOWS || LL_MESA_HEADLESS || LL_SDL_WINDOW || LL_LINUX
 //----------------------------------------------------------------------------
 // LL_WINDOWS || LL_MESA_HEADLESS || LL_SDL_WINDOW || LL_LINUX

#if LL_MESA_HEADLESS
#define GL_GLEXT_PROTOTYPES 1
#else
#define LL_GL_FUNC_POINTER 1
#endif

 // windows gl headers depend on things like APIENTRY, so include windows.
#include "llwin32headers.h"

// quotes so we get libraries/.../GL/ version
#include "GL/glcorearb.h"

#if LL_WINDOWS && !LL_MESA_HEADLESS
#include "GL/wglext.h"
#endif

#if LL_LINUX && LL_X11 && !LL_MESA_HEADLESS
#define GLX_GLXEXT_LEGACY
#define __gl_h_ 1
#include "GL/glx.h"
#include "GL/glxext.h"
#endif

#if LL_LINUX && LL_WAYLAND && !LL_MESA_HEADLESS
#define EGL_EGL_PROTOTYPES 0
#include "EGL/egl.h"
#endif

#elif LL_DARWIN
//----------------------------------------------------------------------------
// LL_DARWIN

#define GL_GLEXT_LEGACY 1
#include <OpenGL/gl3.h>

#define GL_EXT_separate_specular_color 1
#define GL_GLEXT_PROTOTYPES 1
#include "GL/glext.h"

#endif // LL_MESA_HEADLESS / LL_SDL_WINDOW // LL_LINUX / LL_WINDOWS / LL_DARWIN

// GL_NVX_gpu_memory_info constants
#ifndef GL_NVX_gpu_memory_info
#define GL_NVX_gpu_memory_info
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX          0x9047
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX    0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX  0x9049
#define GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX            0x904A
#define GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX            0x904B
#endif

// GL_ATI_meminfo constants
#ifndef GL_ATI_meminfo
#define GL_ATI_meminfo
#define GL_VBO_FREE_MEMORY_ATI                     0x87FB
#define GL_TEXTURE_FREE_MEMORY_ATI                 0x87FC
#define GL_RENDERBUFFER_FREE_MEMORY_ATI            0x87FD
#endif

// GL_EXT_texture_sRGB constants
#ifndef GL_EXT_texture_sRGB
#define GL_EXT_texture_sRGB 1
#define GL_SRGB_EXT                       0x8C40
#define GL_SRGB8_EXT                      0x8C41
#define GL_SRGB_ALPHA_EXT                 0x8C42
#define GL_SRGB8_ALPHA8_EXT               0x8C43
#define GL_SLUMINANCE_ALPHA_EXT           0x8C44
#define GL_SLUMINANCE8_ALPHA8_EXT         0x8C45
#define GL_SLUMINANCE_EXT                 0x8C46
#define GL_SLUMINANCE8_EXT                0x8C47
#define GL_COMPRESSED_SRGB_EXT            0x8C48
#define GL_COMPRESSED_SRGB_ALPHA_EXT      0x8C49
#define GL_COMPRESSED_SLUMINANCE_EXT      0x8C4A
#define GL_COMPRESSED_SLUMINANCE_ALPHA_EXT 0x8C4B
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT  0x8C4C
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif /* GL_EXT_texture_sRGB */

// GL_ARB_vertex_buffer_object constants
#ifndef GL_ARB_vertex_buffer_object
#define GL_ARB_vertex_buffer_object 1
#define GL_STREAM_DRAW_ARB  0x88E0
#define GL_STREAM_READ_ARB  0x88E1
#define GL_STREAM_COPY_ARB  0x88E2
#define GL_STATIC_DRAW_ARB  0x88E4
#define GL_STATIC_READ_ARB  0x88E5
#define GL_STATIC_COPY_ARB  0x88E6
#define GL_DYNAMIC_DRAW_ARB 0x88E8
#define GL_DYNAMIC_READ_ARB 0x88E9
#define GL_DYNAMIC_COPY_ARB 0x88EA
#endif

// Deprecated OpenGL defines we still use
#define GL_COLOR_INDEX                    0x1900
#define GL_ALPHA                          0x1906
#define GL_ALPHA8                         0x803C
#define GL_LUMINANCE                      0x1909
#define GL_LUMINANCE_ALPHA                0x190A
#define GL_LUMINANCE8                     0x8040
#define GL_LUMINANCE8_ALPHA8              0x8045
#define GL_COMPRESSED_ALPHA               0x84E9
#define GL_COMPRESSED_LUMINANCE           0x84EA
#define GL_COMPRESSED_LUMINANCE_ALPHA     0x84EB

#if LL_GL_FUNC_POINTER

#if LL_WINDOWS
// <plus> research/207 T3: only the GL functions something calls (llgl.cpp) </plus>
// WGL_EXT_swap_control
extern PFNWGLSWAPINTERVALEXTPROC    wglSwapIntervalEXT;
extern PFNWGLGETSWAPINTERVALEXTPROC wglGetSwapIntervalEXT;

#endif // LL_WINDOWS

#if LL_LINUX && LL_X11 && !LL_MESA_HEADLESS
// GLX_MESA_query_renderer
extern PFNGLXQUERYCURRENTRENDERERINTEGERMESAPROC glXQueryCurrentRendererIntegerMESA;
extern PFNGLXQUERYCURRENTRENDERERSTRINGMESAPROC glXQueryCurrentRendererStringMESA;
extern PFNGLXQUERYRENDERERINTEGERMESAPROC glXQueryRendererIntegerMESA;
extern PFNGLXQUERYRENDERERSTRINGMESAPROC glXQueryRendererStringMESA;
#endif

#if LL_LINUX && LL_WAYLAND &&!LL_MESA_HEADLESS
extern PFNEGLQUERYSTRINGPROC eglQueryString;
#endif

// We get all functions via getProcAddress when using SDL
// GL_VERSION_1_0
extern PFNGLCULLFACEPROC                    glCullFace;
extern PFNGLFRONTFACEPROC                   glFrontFace;
extern PFNGLLINEWIDTHPROC                   glLineWidth;
extern PFNGLPOINTSIZEPROC                   glPointSize;
extern PFNGLPOLYGONMODEPROC                 glPolygonMode;
extern PFNGLSCISSORPROC                     glScissor;
extern PFNGLTEXPARAMETERFPROC               glTexParameterf;
extern PFNGLTEXPARAMETERIPROC               glTexParameteri;
extern PFNGLTEXPARAMETERIVPROC              glTexParameteriv;
extern PFNGLTEXIMAGE2DPROC                  glTexImage2D;
extern PFNGLDRAWBUFFERPROC                  glDrawBuffer;
extern PFNGLCLEARPROC                       glClear;
extern PFNGLCLEARCOLORPROC                  glClearColor;
extern PFNGLCLEARSTENCILPROC                glClearStencil;
extern PFNGLCLEARDEPTHPROC                  glClearDepth;
extern PFNGLSTENCILMASKPROC                 glStencilMask;
extern PFNGLCOLORMASKPROC                   glColorMask;
extern PFNGLDEPTHMASKPROC                   glDepthMask;
extern PFNGLDISABLEPROC                     glDisable;
extern PFNGLENABLEPROC                      glEnable;
extern PFNGLFINISHPROC                      glFinish;
extern PFNGLFLUSHPROC                       glFlush;
extern PFNGLBLENDFUNCPROC                   glBlendFunc;
extern PFNGLSTENCILFUNCPROC                 glStencilFunc;
extern PFNGLSTENCILOPPROC                   glStencilOp;
extern PFNGLDEPTHFUNCPROC                   glDepthFunc;
extern PFNGLPIXELSTOREIPROC                 glPixelStorei;
extern PFNGLREADBUFFERPROC                  glReadBuffer;
extern PFNGLREADPIXELSPROC                  glReadPixels;
extern PFNGLGETBOOLEANVPROC                 glGetBooleanv;
extern PFNGLGETERRORPROC                    glGetError;
extern PFNGLGETFLOATVPROC                   glGetFloatv;
extern PFNGLGETINTEGERVPROC                 glGetIntegerv;
extern PFNGLGETTEXIMAGEPROC                 glGetTexImage;
extern PFNGLGETTEXLEVELPARAMETERIVPROC      glGetTexLevelParameteriv;
extern PFNGLISENABLEDPROC                   glIsEnabled;
extern PFNGLVIEWPORTPROC                    glViewport;

// GL_VERSION_1_1
extern PFNGLDRAWARRAYSPROC                  glDrawArrays;
extern PFNGLDRAWELEMENTSPROC                glDrawElements;
extern PFNGLPOLYGONOFFSETPROC               glPolygonOffset;
extern PFNGLCOPYTEXSUBIMAGE2DPROC           glCopyTexSubImage2D;
extern PFNGLTEXSUBIMAGE2DPROC               glTexSubImage2D;
extern PFNGLBINDTEXTUREPROC                 glBindTexture;
extern PFNGLDELETETEXTURESPROC              glDeleteTextures;
extern PFNGLGENTEXTURESPROC                 glGenTextures;

// GL_VERSION_1_2
extern PFNGLDRAWRANGEELEMENTSPROC           glDrawRangeElements;
extern PFNGLTEXIMAGE3DPROC                  glTexImage3D;
extern PFNGLTEXSUBIMAGE3DPROC               glTexSubImage3D;
extern PFNGLCOPYTEXSUBIMAGE3DPROC           glCopyTexSubImage3D;

// GL_VERSION_1_3
extern PFNGLACTIVETEXTUREPROC               glActiveTexture;
extern PFNGLCOMPRESSEDTEXIMAGE2DPROC        glCompressedTexImage2D;
extern PFNGLGETCOMPRESSEDTEXIMAGEPROC       glGetCompressedTexImage;
extern PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC     glCompressedTexSubImage3D; // <plus> research/288: the constellation pictures (BC4, a 2D array) </plus>

// GL_VERSION_1_4
extern PFNGLBLENDFUNCSEPARATEPROC       glBlendFuncSeparate;

// GL_VERSION_1_5
extern PFNGLGENQUERIESPROC              glGenQueries;
extern PFNGLDELETEQUERIESPROC           glDeleteQueries;
extern PFNGLBEGINQUERYPROC              glBeginQuery;
extern PFNGLENDQUERYPROC                glEndQuery;
extern PFNGLGETQUERYIVPROC              glGetQueryiv;
extern PFNGLGETQUERYOBJECTIVPROC        glGetQueryObjectiv;
extern PFNGLGETQUERYOBJECTUIVPROC       glGetQueryObjectuiv;
extern PFNGLBINDBUFFERPROC              glBindBuffer;
extern PFNGLDELETEBUFFERSPROC           glDeleteBuffers;
extern PFNGLGENBUFFERSPROC              glGenBuffers;
extern PFNGLBUFFERDATAPROC              glBufferData;
extern PFNGLBUFFERSUBDATAPROC           glBufferSubData;
extern PFNGLGETBUFFERPARAMETERIVPROC    glGetBufferParameteriv;

// GL_VERSION_2_0
extern PFNGLBLENDEQUATIONSEPARATEPROC           glBlendEquationSeparate;
extern PFNGLDRAWBUFFERSPROC                     glDrawBuffers;
extern PFNGLSTENCILOPSEPARATEPROC               glStencilOpSeparate;
extern PFNGLSTENCILFUNCSEPARATEPROC             glStencilFuncSeparate;
extern PFNGLSTENCILMASKSEPARATEPROC             glStencilMaskSeparate;
extern PFNGLATTACHSHADERPROC                    glAttachShader;
extern PFNGLBINDATTRIBLOCATIONPROC              glBindAttribLocation;
extern PFNGLCOMPILESHADERPROC                   glCompileShader;
extern PFNGLCREATEPROGRAMPROC                   glCreateProgram;
extern PFNGLCREATESHADERPROC                    glCreateShader;
extern PFNGLDELETEPROGRAMPROC                   glDeleteProgram;
extern PFNGLDELETESHADERPROC                    glDeleteShader;
extern PFNGLDETACHSHADERPROC                    glDetachShader;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC        glDisableVertexAttribArray;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC         glEnableVertexAttribArray;
extern PFNGLGETACTIVEUNIFORMPROC                glGetActiveUniform;
extern PFNGLGETATTACHEDSHADERSPROC              glGetAttachedShaders;
extern PFNGLGETATTRIBLOCATIONPROC               glGetAttribLocation;
extern PFNGLGETPROGRAMIVPROC                    glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC               glGetProgramInfoLog;
extern PFNGLGETSHADERIVPROC                     glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC                glGetShaderInfoLog;
extern PFNGLGETUNIFORMLOCATIONPROC              glGetUniformLocation;
extern PFNGLISPROGRAMPROC                       glIsProgram;
extern PFNGLISSHADERPROC                        glIsShader;
extern PFNGLLINKPROGRAMPROC                     glLinkProgram;
extern PFNGLSHADERSOURCEPROC                    glShaderSource;
extern PFNGLUSEPROGRAMPROC                      glUseProgram;
extern PFNGLUNIFORM1FPROC                       glUniform1f;
extern PFNGLUNIFORM2FPROC                       glUniform2f;
extern PFNGLUNIFORM3FPROC                       glUniform3f;
extern PFNGLUNIFORM4FPROC                       glUniform4f;
extern PFNGLUNIFORM1IPROC                       glUniform1i;
extern PFNGLUNIFORM2IPROC                       glUniform2i;
extern PFNGLUNIFORM1FVPROC                      glUniform1fv;
extern PFNGLUNIFORM2FVPROC                      glUniform2fv;
extern PFNGLUNIFORM3FVPROC                      glUniform3fv;
extern PFNGLUNIFORM4FVPROC                      glUniform4fv;
extern PFNGLUNIFORM1IVPROC                      glUniform1iv;
extern PFNGLUNIFORM4IVPROC                      glUniform4iv;
extern PFNGLUNIFORMMATRIX2FVPROC                glUniformMatrix2fv;
extern PFNGLUNIFORMMATRIX3FVPROC                glUniformMatrix3fv;
extern PFNGLUNIFORMMATRIX4FVPROC                glUniformMatrix4fv;
extern PFNGLVALIDATEPROGRAMPROC                 glValidateProgram;
extern PFNGLVERTEXATTRIB4FPROC                  glVertexAttrib4f;
extern PFNGLVERTEXATTRIB4FVPROC                 glVertexAttrib4fv;
extern PFNGLVERTEXATTRIBPOINTERPROC             glVertexAttribPointer;

// GL_VERSION_2_1
extern PFNGLUNIFORMMATRIX3X4FVPROC glUniformMatrix3x4fv;

// GL_VERSION_3_0
extern PFNGLBINDBUFFERRANGEPROC                         glBindBufferRange;
extern PFNGLBINDBUFFERBASEPROC                          glBindBufferBase;
extern PFNGLVERTEXATTRIBIPOINTERPROC                    glVertexAttribIPointer;
extern PFNGLUNIFORM4UIVPROC                             glUniform4uiv;
extern PFNGLBINDFRAMEBUFFERPROC                         glBindFramebuffer;
extern PFNGLDELETEFRAMEBUFFERSPROC                      glDeleteFramebuffers;
extern PFNGLGENFRAMEBUFFERSPROC                         glGenFramebuffers;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC                  glCheckFramebufferStatus;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC                    glFramebufferTexture2D;
extern PFNGLGENERATEMIPMAPPROC                          glGenerateMipmap;
extern PFNGLBINDVERTEXARRAYPROC                         glBindVertexArray;
extern PFNGLGENVERTEXARRAYSPROC                         glGenVertexArrays;

// GL_VERSION_3_1
extern PFNGLDRAWARRAYSINSTANCEDPROC         glDrawArraysInstanced;
extern PFNGLDRAWELEMENTSINSTANCEDPROC       glDrawElementsInstanced;
extern PFNGLGETUNIFORMBLOCKINDEXPROC        glGetUniformBlockIndex;
extern PFNGLUNIFORMBLOCKBINDINGPROC         glUniformBlockBinding;

// GL_VERSION_3_2
extern PFNGLFENCESYNCPROC                       glFenceSync;
extern PFNGLDELETESYNCPROC                      glDeleteSync;
extern PFNGLCLIENTWAITSYNCPROC                  glClientWaitSync;
extern PFNGLWAITSYNCPROC                        glWaitSync;
extern PFNGLGETINTEGER64VPROC                   glGetInteger64v;

// GL_VERSION_3_3
extern PFNGLQUERYCOUNTERPROC                 glQueryCounter;
extern PFNGLGETQUERYOBJECTUI64VPROC          glGetQueryObjectui64v;

// GL_VERSION_4_1
extern PFNGLGETPROGRAMBINARYPROC                glGetProgramBinary;
extern PFNGLPROGRAMBINARYPROC                   glProgramBinary;
extern PFNGLPROGRAMPARAMETERIPROC               glProgramParameteri;

// GL_VERSION_4_3
extern PFNGLDEBUGMESSAGECALLBACKPROC                        glDebugMessageCallback;
extern PFNGLOBJECTLABELPROC                                 glObjectLabel;

#endif

#if defined(TRACY_ENABLE) && LL_PROFILER_ENABLE_TRACY_OPENGL
    #include <tracy/TracyOpenGL.hpp>
#endif

#endif // LL_LLGLHEADERS_H
