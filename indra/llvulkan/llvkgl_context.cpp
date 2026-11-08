/**
 * @file llvkgl_context.cpp
 * @brief What GL's context still gave the translation: object names, sync objects, the rest of GL's entry points, and the function table without a GL context (research/117 G4, G5).
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

#include <array>
#include <utility>

#if LL_GL_FUNC_POINTER

// research/117 G4, G5: there is no GL context. The names of textures,
// buffers, framebuffers, queries and vertex arrays come from here, sync
// objects complete at once, and the GL entry points the viewer calls that no
// wrapper covers (glFlush, glHint, timer queries ...) do nothing. These
// functions, the queries (llvkgl_glquery.cpp) and the shader objects
// (llvkgl_shaderobj.cpp) make the table LL_GET_PROC_ADDRESS reads instead of
// wglGetProcAddress; names nothing answers get a function that logs its name
// once and returns 0.

namespace llvkgl
{
    thread_local GLuint sVertexArray = 0;   // glBindVertexArray (per context, like GL)
namespace
{
    // GL's names: the smallest free one first (the tables index names
    // directly, research/91 NameTable); a deleted name is free again, as GL
    // drivers reuse them. Any thread (the texture threads make textures).
    // The delete wrappers release the names after the objects.
    class NameAllocator
    {
    public:
        void generate(GLsizei n, GLuint* names)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (GLsizei i = 0; i < n; ++i)
            {
                GLuint name;
                if (!mFree.empty())
                {
                    std::pop_heap(mFree.begin(), mFree.end(), std::greater<GLuint>());
                    name = mFree.back();
                    mFree.pop_back();
                }
                else
                {
                    name = mNext++;
                }
                mLive.insert(name);
                names[i] = name;
            }
        }
        void release(GLsizei n, const GLuint* names)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (GLsizei i = 0; names && i < n; ++i)
            {
                if (names[i] && mLive.erase(names[i]))
                {
                    mFree.push_back(names[i]);
                    std::push_heap(mFree.begin(), mFree.end(), std::greater<GLuint>());
                }
            }
        }
        bool live(GLuint name)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            return mLive.count(name) != 0;
        }
    private:
        std::mutex mMutex;
        GLuint mNext = 1;
        std::vector<GLuint> mFree;      // a min-heap
        std::unordered_set<GLuint> mLive;
    };

    NameAllocator sNames[NAMES_KIND_COUNT];

    void APIENTRY own_glGenTextures(GLsizei n, GLuint* names) { sNames[NAMES_TEXTURE].generate(n, names); }
    void APIENTRY own_glGenBuffers(GLsizei n, GLuint* names) { sNames[NAMES_BUFFER].generate(n, names); }
    void APIENTRY own_glGenFramebuffers(GLsizei n, GLuint* names) { sNames[NAMES_FRAMEBUFFER].generate(n, names); }
    void APIENTRY own_glGenQueries(GLsizei n, GLuint* names) { sNames[NAMES_QUERY].generate(n, names); }
    void APIENTRY own_glGenVertexArrays(GLsizei n, GLuint* names) { sNames[NAMES_VERTEX_ARRAY].generate(n, names); }
    GLboolean APIENTRY own_glIsTexture(GLuint name) { return sNames[NAMES_TEXTURE].live(name) ? GL_TRUE : GL_FALSE; }
    GLboolean APIENTRY own_glIsBuffer(GLuint name) { return sNames[NAMES_BUFFER].live(name) ? GL_TRUE : GL_FALSE; }
    GLboolean APIENTRY own_glIsFramebuffer(GLuint name) { return sNames[NAMES_FRAMEBUFFER].live(name) ? GL_TRUE : GL_FALSE; }
    GLboolean APIENTRY own_glIsQuery(GLuint name) { return sNames[NAMES_QUERY].live(name) ? GL_TRUE : GL_FALSE; }
    GLboolean APIENTRY own_glIsVertexArray(GLuint name) { return sNames[NAMES_VERTEX_ARRAY].live(name) ? GL_TRUE : GL_FALSE; }

    void APIENTRY own_glDeleteQueries(GLsizei n, const GLuint* names)
    {
        LLVKGL_REDIRECT("glDeleteQueries", own_glDeleteQueries(n, names))
        sNames[NAMES_QUERY].release(n, names);
    }

    void APIENTRY own_glDeleteVertexArrays(GLsizei n, const GLuint* names)
    {
        LLVKGL_REDIRECT("glDeleteVertexArrays", own_glDeleteVertexArrays(n, names))
        for (GLsizei i = 0; names && i < n; ++i)
        {
            if (sVertexArray == names[i])
            {
                sVertexArray = 0;
            }
        }
        sNames[NAMES_VERTEX_ARRAY].release(n, names);
    }

    // The translation reads vertex attributes as set (glVertexAttribPointer),
    // not per vertex array: LLRender binds one at init and keeps it.
    void APIENTRY own_glBindVertexArray(GLuint name)
    {
        LLVKGL_REDIRECT("glBindVertexArray", own_glBindVertexArray(name))
        sVertexArray = name;
    }

    // Sync objects: the texture threads record into the frame under sLock in
    // call order, so what they wrote is before anything the render thread
    // records after (research/85 3, research/117 3): a fence is signalled
    // when made. A distinct non-null value each, as GL's.
    std::atomic<uintptr_t> sNextSync{ 1 };

    GLsync APIENTRY own_glFenceSync(GLenum, GLbitfield)
    {
        return (GLsync)sNextSync.fetch_add(1);
    }

    GLenum APIENTRY own_glClientWaitSync(GLsync, GLbitfield, GLuint64)
    {
        return GL_ALREADY_SIGNALED;
    }

    void APIENTRY own_glWaitSync(GLsync, GLbitfield, GLuint64)
    {
    }

    void APIENTRY own_glDeleteSync(GLsync)
    {
    }

    GLboolean APIENTRY own_glIsSync(GLsync sync)
    {
        return sync ? GL_TRUE : GL_FALSE;
    }

    // The Vulkan frame is submitted at the present; nothing to flush or wait for here.
    void APIENTRY own_glFlush()
    {
    }

    void APIENTRY own_glFinish()
    {
    }

    // Hints, point sizes (debug views), GL debug output and object labels
    // (RenderDoc builds): no Vulkan state. The validation layer
    // (LLRHI_VK_VALIDATION) stands in for GL's debug output.
    void APIENTRY own_glHint(GLenum, GLenum)
    {
    }

    void APIENTRY own_glPointSize(GLfloat)
    {
    }

    void APIENTRY own_glDebugMessageCallback(GLDEBUGPROC, const void*)
    {
    }

    void APIENTRY own_glDebugMessageControl(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean)
    {
    }

    void APIENTRY own_glDebugMessageInsert(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*)
    {
    }

    void APIENTRY own_glObjectLabel(GLenum, GLuint, GLsizei, const GLchar*)
    {
    }

    // Timer and primitive queries (LLGLSLShader's profile, LLVOAvatar's GPU
    // time): available at once, 0. As now: GL renders nothing to time.
    void APIENTRY own_glGetQueryObjectui64v(GLuint, GLenum pname, GLuint64* params)
    {
        if (params)
        {
            *params = pname == GL_QUERY_RESULT_AVAILABLE ? 1 : 0;
        }
    }

    void APIENTRY own_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params)
    {
        LLVKGL_REDIRECT("glGetQueryObjectiv", own_glGetQueryObjectiv(id, pname, params))
        GLuint value = 0;
        native_get_query_object(id, pname, &value);
        if (params)
        {
            *params = (GLint)value;
        }
    }

    // ------------------------------------------------------------------
    // G5: the function table without a GL context.

    // The swap interval LLWindowWin32::toggleVSync sets (LLGLManager::initGL
    // loads the two WGL functions) and the Vulkan present reads back
    // (LLRHIVKMirror's present_frame).
    std::atomic<int> sSwapInterval{ 1 };    // GL drivers start with VSync on

    BOOL WINAPI own_wglSwapIntervalEXT(int interval)
    {
        sSwapInterval.store(interval);
        return TRUE;
    }

    int WINAPI own_wglGetSwapIntervalEXT()
    {
        return sSwapInterval.load();
    }

    // Names nothing here answers: a function per name (it knows its slot),
    // which logs the name the first time and returns 0. Every Windows x64
    // calling convention is the same one, with the caller cleaning up its
    // arguments, so one signature serves every GL function.
    static_assert(sizeof(void*) == 8, "the GL stubs need the x64 calling convention");
    constexpr U32 STUB_SLOTS = 1024;
    std::mutex sStubMutex;
    std::vector<std::string> sStubNames;
    std::unordered_map<std::string, U32> sStubSlots;
    std::atomic<U32> sStubCalls[STUB_SLOTS];

    void stub_called(U32 slot)
    {
        if (sStubCalls[slot].fetch_add(1) == 0)
        {
            std::string name;
            {
                std::lock_guard<std::mutex> lock(sStubMutex);
                name = slot < sStubNames.size() ? sStubNames[slot] : std::string("?");
            }
            LL_WARNS("GLStub") << name << " called without a GL context: not answered by the Vulkan translation, returns 0 and writes"
                               << " none of its outputs (research/117)" << LL_ENDL;
        }
    }

    using StubFunction = void* (APIENTRY*)();

    template <U32 SLOT>
    void* APIENTRY gl_stub()
    {
        stub_called(SLOT);
        return nullptr;
    }

    template <U32... SLOTS>
    constexpr std::array<StubFunction, sizeof...(SLOTS)> make_stubs(std::integer_sequence<U32, SLOTS...>)
    {
        return { { &gl_stub<SLOTS>... } };
    }

    const std::array<StubFunction, STUB_SLOTS> sStubs = make_stubs(std::make_integer_sequence<U32, STUB_SLOTS>());

    PROC stub_for(const char* name)
    {
        std::lock_guard<std::mutex> lock(sStubMutex);
        auto it = sStubSlots.find(name);
        if (it == sStubSlots.end())
        {
            if (sStubNames.size() >= STUB_SLOTS)
            {
                LL_WARNS("GLStub") << "no stub slot left for " << name << LL_ENDL;
                return nullptr;
            }
            it = sStubSlots.emplace(name, (U32)sStubNames.size()).first;
            sStubNames.push_back(name);
        }
        return (PROC)sStubs[it->second];
    }

    // Every function the Vulkan side answers itself without GL: the ones
    // above and the queries (llvkgl_glquery.cpp); the shader and program
    // objects are llvkgl_shaderobj.cpp's (shader_object_function). The GL
    // entry points the translation wraps (llvkgl_entry.cpp) are not here:
    // they get a stub at load and their wrapper at install (LLVKGL_REPLACE
    // needs a pointer to replace). tools/gl-coverage.py reads the
    // LLVKGL_ANSWER lines.
    struct Answer
    {
        const char* name;
        PROC function;
    };
#define LLVKGL_ANSWER(gl_name, function) { #gl_name, (PROC)(function) },
    const Answer sAnswers[] =
    {
        LLVKGL_ANSWER(glGetIntegerv, &answer_glGetIntegerv)
        LLVKGL_ANSWER(glGetFloatv, &answer_glGetFloatv)
        LLVKGL_ANSWER(glGetBooleanv, &answer_glGetBooleanv)
        LLVKGL_ANSWER(glIsEnabled, &answer_glIsEnabled)
        LLVKGL_ANSWER(glGetError, &answer_glGetError)
        LLVKGL_ANSWER(glGetBufferParameteriv, &answer_glGetBufferParameteriv)
        LLVKGL_ANSWER(glGenTextures, &own_glGenTextures)
        LLVKGL_ANSWER(glGenBuffers, &own_glGenBuffers)
        LLVKGL_ANSWER(glGenFramebuffers, &own_glGenFramebuffers)
        LLVKGL_ANSWER(glGenQueries, &own_glGenQueries)
        LLVKGL_ANSWER(glGenVertexArrays, &own_glGenVertexArrays)
        LLVKGL_ANSWER(glIsTexture, &own_glIsTexture)
        LLVKGL_ANSWER(glIsBuffer, &own_glIsBuffer)
        LLVKGL_ANSWER(glIsFramebuffer, &own_glIsFramebuffer)
        LLVKGL_ANSWER(glIsQuery, &own_glIsQuery)
        LLVKGL_ANSWER(glIsVertexArray, &own_glIsVertexArray)
        LLVKGL_ANSWER(glDeleteQueries, &own_glDeleteQueries)
        LLVKGL_ANSWER(glDeleteVertexArrays, &own_glDeleteVertexArrays)
        LLVKGL_ANSWER(glBindVertexArray, &own_glBindVertexArray)
        LLVKGL_ANSWER(glFenceSync, &own_glFenceSync)
        LLVKGL_ANSWER(glClientWaitSync, &own_glClientWaitSync)
        LLVKGL_ANSWER(glWaitSync, &own_glWaitSync)
        LLVKGL_ANSWER(glDeleteSync, &own_glDeleteSync)
        LLVKGL_ANSWER(glIsSync, &own_glIsSync)
        LLVKGL_ANSWER(glFlush, &own_glFlush)
        LLVKGL_ANSWER(glFinish, &own_glFinish)
        LLVKGL_ANSWER(glHint, &own_glHint)
        LLVKGL_ANSWER(glPointSize, &own_glPointSize)
        LLVKGL_ANSWER(glDebugMessageCallback, &own_glDebugMessageCallback)
        LLVKGL_ANSWER(glDebugMessageControl, &own_glDebugMessageControl)
        LLVKGL_ANSWER(glDebugMessageInsert, &own_glDebugMessageInsert)
        LLVKGL_ANSWER(glObjectLabel, &own_glObjectLabel)
        LLVKGL_ANSWER(glGetQueryObjectui64v, &own_glGetQueryObjectui64v)
        LLVKGL_ANSWER(glGetQueryObjectiv, &own_glGetQueryObjectiv)
        LLVKGL_ANSWER(wglSwapIntervalEXT, &own_wglSwapIntervalEXT)
        LLVKGL_ANSWER(wglGetSwapIntervalEXT, &own_wglGetSwapIntervalEXT)
    };
#undef LLVKGL_ANSWER
} // namespace

    void release_names(NameKind kind, GLsizei n, const GLuint* names)
    {
        sNames[kind].release(n, names);
    }

    bool texture_name_live(GLuint name)
    {
        return sNames[NAMES_TEXTURE].live(name);
    }

    PROC get_proc_address(const char* name)
    {
        if (!name)
        {
            return nullptr;
        }
        for (const Answer& answer : sAnswers)
        {
            if (!strcmp(answer.name, name))
            {
                return answer.function;
            }
        }
        if (PROC function = shader_object_function(name))
        {
            return function;
        }
        return stub_for(name);
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
