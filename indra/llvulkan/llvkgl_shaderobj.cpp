/**
 * @file llvkgl_shaderobj.cpp
 * @brief GL's shader and program objects as the Vulkan side answers them: research/117 G3.
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
#include "llrhihash.h"

#if LL_GL_FUNC_POINTER

// What the viewer learns from GL about a linked program (LLShaderMgr::
// linkProgramObject, LLGLSLShader::mapAttributes, mapUniforms): whether it
// linked, its active uniforms in order with their types and sizes, which of
// them have a location, its active vertex attributes and uniform blocks.
// Without GL these come from LLVKShader::reflectGL, over the GLSL as the
// viewer gave it (research/117 2.1, 7, 8: measured against NVIDIA's GL driver
// with the check mode the stage 1 branch had).

namespace llvkgl
{
namespace
{
    // One entry of glGetActiveUniform's list: the name as listed, GL type,
    // array size, and the location glGetUniformLocation gives the name (-1:
    // none, as for block members).
    struct ActiveUniform
    {
        std::string name;
        U32 type = 0;
        S32 size = 1;
        GLint location = -1;
    };

    std::string base_name(const std::string& name)
    {
        const size_t bracket = name.find("[0]");
        return bracket != std::string::npos && bracket + 3 == name.size() ? name.substr(0, bracket) : name;
    }

    // The Vulkan side's list, in reflectGL's order. Locations: a uniform that
    // is not a block member gets the next ones (an array one per element),
    // as a driver numbers them; only the presence matters to the check.
    std::vector<ActiveUniform> vulkan_list(const LLVKShader::GLReflection& reflection)
    {
        std::vector<ActiveUniform> list;
        GLint next = 0;
        for (const LLVKShader::GLUniform& uniform : reflection.uniforms)
        {
            ActiveUniform u;
            u.name = uniform.name;
            u.type = uniform.gl_type;
            u.size = uniform.size;
            if (!uniform.in_block)
            {
                u.location = next;
                next += llmax(1, uniform.size);
            }
            list.push_back(u);
        }
        return list;
    }

} // namespace

    // ------------------------------------------------------------------
    // The Vulkan side's own shader and program objects (research/117 G3):
    // there is no GL to compile or link. Under sLock.

namespace
{
    // A program's answers as of its last link.
    struct GLView
    {
        bool linked = false;
        std::string log;
        std::vector<ActiveUniform> uniforms;
        std::vector<std::string> blocks;
        std::vector<std::string> inputs;
        // Its entry in sViewCache, held while the program has it (fix-pipeleak:
        // prune_gl_views drops the entries no program holds any more).
        std::shared_ptr<const LLVKShader::GLReflection> view;
    };
    std::unordered_map<GLuint, GLView> sGLViews;

    struct ShaderObject
    {
        GLenum type = 0;
        bool deleted = false;   // glDeleteShader while attached: goes once detached everywhere (as GL)
    };
    std::unordered_map<GLuint, ShaderObject> sShaderObjects;
    std::unordered_set<GLuint> sProgramObjects;
    // Shaders and programs share one namespace in GL. Names are never used
    // again: sShaderSources and sPrograms stay keyed by one object each.
    GLuint sNextObjectName = 1;

    // GL's view of the same sources computed once (a parallel reload's first
    // pass works them out on its compile workers, research/111 B-SC).
    std::mutex sViewCacheMutex;
    std::unordered_map<U64, std::shared_ptr<const LLVKShader::GLReflection>> sViewCache;

    U64 sources_key(const std::vector<LLVKShader::Source>& sources)
    {
        U64 hash = LLRHI::FNV1A_BASIS_SHORT;
        auto mix = [&hash](const void* data, size_t size) { hash = LLRHI::fnv1a(hash, data, size); };
        for (const LLVKShader::Source& source : sources)
        {
            const U32 stage = (U32)source.stage;
            const U64 length = source.text.size();
            mix(&stage, sizeof(stage));
            mix(&length, sizeof(length));
            mix(source.text.data(), source.text.size());
        }
        return hash;
    }

    // keep false: worked out (or read from reflectGL's file on disk) without
    // going into the cache; a later reload reads it back from the disk.
    std::shared_ptr<const LLVKShader::GLReflection> gl_reflection(const std::vector<LLVKShader::Source>& sources, bool keep = true)
    {
        const U64 key = sources_key(sources);
        {
            std::lock_guard<std::mutex> lock(sViewCacheMutex);
            auto it = sViewCache.find(key);
            if (it != sViewCache.end())
            {
                return it->second;
            }
        }
        auto reflection = std::make_shared<LLVKShader::GLReflection>();
        LLVKShader::reflectGL(sources, *reflection);
        if (!keep)
        {
            return reflection;
        }
        std::lock_guard<std::mutex> lock(sViewCacheMutex);
        return sViewCache.emplace(key, std::move(reflection)).first->second;
    }

    std::vector<LLVKShader::Source> attached_sources(GLuint program)
    {
        std::vector<LLVKShader::Source> sources;
        for (GLuint shader : sAttached[program])
        {
            auto it = sShaderSources.find(shader);
            if (it != sShaderSources.end())
            {
                sources.push_back({ shader_stage(it->second.first), it->second.second });
            }
        }
        return sources;
    }

    bool attached_anywhere(GLuint shader)
    {
        for (const auto& program : sAttached)
        {
            if (std::find(program.second.begin(), program.second.end(), shader) != program.second.end())
            {
                return true;
            }
        }
        return false;
    }

    // A shader a program let go of (glDetachShader, glDeleteProgram) goes
    // if it was deleted and no program holds it any more.
    void release_shader(GLuint shader)
    {
        auto it = sShaderObjects.find(shader);
        if (it != sShaderObjects.end() && it->second.deleted && !attached_anywhere(shader))
        {
            sShaderObjects.erase(it);
            sShaderSources.erase(shader);
        }
    }

    void copy_text(const std::string& text, GLsizei size, GLsizei* length, GLchar* out)
    {
        GLsizei copied = 0;
        if (out && size > 0)
        {
            copied = (GLsizei)llmin(text.size(), (size_t)(size - 1));
            memcpy(out, text.data(), copied);
            out[copied] = 0;
        }
        if (length)
        {
            *length = copied;
        }
    }

    // The shader and program objects are under sLock.
    GLuint APIENTRY own_glCreateShader(GLenum type)
    {
        LLVKGL_REDIRECT("glCreateShader", own_glCreateShader(type))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const GLuint name = sNextObjectName++;
        sShaderObjects[name].type = type;
        sShaderSources[name] = { type, std::string() };
        return name;
    }

    // Errors show at the link (research/117 2.1): the viewer's GLSL all builds.
    void APIENTRY own_glCompileShader(GLuint)
    {
    }

    void APIENTRY own_glGetShaderiv(GLuint shader, GLenum pname, GLint* params)
    {
        LLVKGL_REDIRECT_QUERY("glGetShaderiv", own_glGetShaderiv(shader, pname, params))
        if (!params)
        {
            return;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto it = sShaderObjects.find(shader);
        if (it == sShaderObjects.end())
        {
            return;     // GL: GL_INVALID_VALUE, params untouched
        }
        switch (pname)
        {
        case GL_SHADER_TYPE: *params = (GLint)it->second.type; break;
        case GL_COMPILE_STATUS: *params = GL_TRUE; break;
        case GL_DELETE_STATUS: *params = it->second.deleted ? GL_TRUE : GL_FALSE; break;
        case GL_INFO_LOG_LENGTH: *params = 0; break;
        case GL_SHADER_SOURCE_LENGTH:
            {
                auto source = sShaderSources.find(shader);
                *params = source != sShaderSources.end() && !source->second.second.empty() ? (GLint)source->second.second.size() + 1 : 0;
            }
            break;
        default:
            unhandled("glGetShaderiv " + hex(pname));
            *params = 0;
            break;
        }
    }

    void APIENTRY own_glGetShaderInfoLog(GLuint, GLsizei size, GLsizei* length, GLchar* log)
    {
        copy_text(std::string(), size, length, log);
    }

    void APIENTRY own_glDeleteShader(GLuint shader)
    {
        LLVKGL_REDIRECT("glDeleteShader", own_glDeleteShader(shader))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto it = sShaderObjects.find(shader);
        if (it == sShaderObjects.end())
        {
            return;
        }
        if (attached_anywhere(shader))
        {
            it->second.deleted = true;
        }
        else
        {
            sShaderObjects.erase(it);
            sShaderSources.erase(shader);
        }
    }

    GLboolean APIENTRY own_glIsShader(GLuint shader)
    {
        LLVKGL_REDIRECT_QUERY("glIsShader", own_glIsShader(shader))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return sShaderObjects.count(shader) ? GL_TRUE : GL_FALSE;
    }

    GLuint APIENTRY own_glCreateProgram()
    {
        LLVKGL_REDIRECT("glCreateProgram", own_glCreateProgram())
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        const GLuint name = sNextObjectName++;
        sProgramObjects.insert(name);
        return name;
    }

    GLboolean APIENTRY own_glIsProgram(GLuint program)
    {
        LLVKGL_REDIRECT_QUERY("glIsProgram", own_glIsProgram(program))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return sProgramObjects.count(program) ? GL_TRUE : GL_FALSE;
    }

    void APIENTRY own_glGetProgramiv(GLuint program, GLenum pname, GLint* params)
    {
        LLVKGL_REDIRECT_QUERY("glGetProgramiv", own_glGetProgramiv(program, pname, params))
        if (!params)
        {
            return;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (!sProgramObjects.count(program))
        {
            return;     // GL: GL_INVALID_VALUE, params untouched
        }
        const GLView* view = nullptr;
        auto it = sGLViews.find(program);
        if (it != sGLViews.end())
        {
            view = &it->second;
        }
        switch (pname)
        {
        case GL_LINK_STATUS:
        case GL_VALIDATE_STATUS:
            *params = view && view->linked ? GL_TRUE : GL_FALSE;
            break;
        case GL_DELETE_STATUS: *params = GL_FALSE; break;
        case GL_INFO_LOG_LENGTH: *params = view && !view->log.empty() ? (GLint)view->log.size() + 1 : 0; break;
        case GL_ATTACHED_SHADERS:
            {
                auto attached = sAttached.find(program);
                *params = attached != sAttached.end() ? (GLint)attached->second.size() : 0;
            }
            break;
        case GL_ACTIVE_UNIFORMS: *params = view ? (GLint)view->uniforms.size() : 0; break;
        case GL_ACTIVE_UNIFORM_MAX_LENGTH:
            {
                size_t longest = 0;
                for (size_t i = 0; view && i < view->uniforms.size(); ++i)
                {
                    longest = llmax(longest, view->uniforms[i].name.size() + 1);
                }
                *params = (GLint)longest;
            }
            break;
        case GL_ACTIVE_ATTRIBUTES: *params = view ? (GLint)view->inputs.size() : 0; break;
        case GL_ACTIVE_UNIFORM_BLOCKS: *params = view ? (GLint)view->blocks.size() : 0; break;
        case GL_PROGRAM_BINARY_LENGTH: *params = 0; break;     // no GL program binaries
        default:
            unhandled("glGetProgramiv " + hex(pname));
            *params = 0;
            break;
        }
    }

    void APIENTRY own_glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei* length, GLchar* log)
    {
        LLVKGL_REDIRECT_QUERY("glGetProgramInfoLog", own_glGetProgramInfoLog(program, size, length, log))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto it = sGLViews.find(program);
        copy_text(it != sGLViews.end() ? it->second.log : std::string(), size, length, log);
    }

    // Validity against the current state: the Vulkan side makes its
    // pipelines at the draw, so a linked program is valid.
    void APIENTRY own_glValidateProgram(GLuint)
    {
    }

    void APIENTRY own_glGetActiveUniform(GLuint program, GLuint index, GLsizei size, GLsizei* length, GLint* array_size, GLenum* type, GLchar* name)
    {
        LLVKGL_REDIRECT_QUERY("glGetActiveUniform", own_glGetActiveUniform(program, index, size, length, array_size, type, name))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto it = sGLViews.find(program);
        if (it == sGLViews.end() || index >= it->second.uniforms.size())
        {
            return;     // GL: GL_INVALID_VALUE, outputs untouched
        }
        const ActiveUniform& u = it->second.uniforms[index];
        copy_text(u.name, size, length, name);
        if (array_size)
        {
            *array_size = u.size;
        }
        if (type)
        {
            *type = u.type;
        }
    }

    GLint APIENTRY own_glGetAttribLocation(GLuint program, const GLchar* name)
    {
        LLVKGL_REDIRECT_QUERY("glGetAttribLocation", own_glGetAttribLocation(program, name))
        if (!name)
        {
            return -1;
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto view = sGLViews.find(program);
        if (view == sGLViews.end() ||
            std::find(view->second.inputs.begin(), view->second.inputs.end(), name) == view->second.inputs.end())
        {
            return -1;  // not read: GL lists only active attributes
        }
        auto it = sPrograms.find(program);
        if (it != sPrograms.end())
        {
            auto bound = it->second.attrib_bindings.find(name);
            if (bound != it->second.attrib_bindings.end())
            {
                return (GLint)bound->second;
            }
            for (const LLVKShader::Input& input : it->second.vk.inputs)
            {
                if (input.name == name)
                {
                    return (GLint)input.location;   // read but not bound: where the shader has it
                }
            }
        }
        return -1;
    }

    void APIENTRY own_glGetAttachedShaders(GLuint program, GLsizei max_count, GLsizei* count, GLuint* shaders)
    {
        LLVKGL_REDIRECT_QUERY("glGetAttachedShaders", own_glGetAttachedShaders(program, max_count, count, shaders))
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        GLsizei n = 0;
        auto attached = sAttached.find(program);
        for (size_t i = 0; attached != sAttached.end() && i < attached->second.size(); ++i)
        {
            if (n >= max_count || !shaders)
            {
                break;
            }
            shaders[n++] = attached->second[i];
        }
        if (count)
        {
            *count = n;
        }
    }

    // The program binary cache is off in this viewer (plus/vkcheck).
    void APIENTRY own_glProgramParameteri(GLuint, GLenum, GLint)
    {
    }

    void APIENTRY own_glGetProgramBinary(GLuint, GLsizei, GLsizei* length, GLenum*, void*)
    {
        if (length)
        {
            *length = 0;
        }
    }
} // namespace

    void prepare_gl_view(const std::vector<LLVKShader::Source>& sources, bool keep)
    {
        if (!sources.empty())
        {
            gl_reflection(sources, keep);
        }
    }

    void prune_gl_views()
    {
        std::lock_guard<std::mutex> lock(sViewCacheMutex);
        for (auto it = sViewCache.begin(); it != sViewCache.end();)
        {
            // Copies come out of the cache under this lock: one means only the
            // cache has it. A worker's passing copy keeps it until the next reload.
            if (it->second.use_count() == 1)
            {
                it = sViewCache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void own_link(GLuint program)
    {
        GLView& view = sGLViews[program];
        view = GLView();
        if (link_planning())
        {
            // A parallel reload's first pass: this program is thrown away and
            // linked again in the second (link_planned started its compile).
            view.linked = true;
            return;
        }
        const std::shared_ptr<const LLVKShader::GLReflection> reflection = gl_reflection(attached_sources(program));
        view.view = reflection;
        auto it = sPrograms.find(program);
        const bool built = it != sPrograms.end() && it->second.built;
        view.linked = built && reflection->linked;
        if (!reflection->linked)
        {
            view.log = reflection->log;
        }
        else if (!built && it != sPrograms.end())
        {
            view.log = it->second.vk.log;
        }
        if (reflection->linked)
        {
            view.uniforms = vulkan_list(*reflection);
            view.blocks = reflection->blocks;
            view.inputs = reflection->inputs;
        }
    }

    GLint own_uniform_location(GLuint program, const GLchar* name)
    {
        auto it = sGLViews.find(program);
        if (!name || it == sGLViews.end())
        {
            return -1;
        }
        const std::string wanted = name;
        for (const ActiveUniform& u : it->second.uniforms)
        {
            if (u.location >= 0 && (u.name == wanted || base_name(u.name) == wanted))
            {
                return u.location;
            }
        }
        // "name[k]": the k-th element of an array.
        const size_t open = wanted.rfind('[');
        if (open != std::string::npos && wanted.back() == ']')
        {
            const std::string base = wanted.substr(0, open);
            const S32 element = atoi(wanted.c_str() + open + 1);
            for (const ActiveUniform& u : it->second.uniforms)
            {
                if (u.location >= 0 && base_name(u.name) == base && element >= 0 && element < u.size)
                {
                    return u.location + element;
                }
            }
        }
        return -1;
    }

    GLuint own_block_index(GLuint program, const GLchar* name)
    {
        auto it = sGLViews.find(program);
        if (!name || it == sGLViews.end())
        {
            return GL_INVALID_INDEX;
        }
        const std::vector<std::string>& blocks = it->second.blocks;
        auto block = std::find(blocks.begin(), blocks.end(), name);
        return block != blocks.end() ? (GLuint)(block - blocks.begin()) : GL_INVALID_INDEX;
    }

    void own_program_deleted(GLuint program, const std::vector<GLuint>& detached)
    {
        sGLViews.erase(program);    // lets go of its view (prune_gl_views)
        sProgramObjects.erase(program);
        for (GLuint shader : detached)
        {
            release_shader(shader);
        }
    }

    void own_shader_detached(GLuint shader)
    {
        release_shader(shader);
    }

    // The table without a GL context (llvkgl_context.cpp's get_proc_address):
    // these functions by GL name, null for others. tools/gl-coverage.py reads
    // the LLVKGL_ANSWER lines.
    PROC shader_object_function(const char* name)
    {
#define LLVKGL_ANSWER(gl_name, function) if (!strcmp(name, #gl_name)) return (PROC)(function);
        LLVKGL_ANSWER(glCreateShader, &own_glCreateShader)
        LLVKGL_ANSWER(glCompileShader, &own_glCompileShader)
        LLVKGL_ANSWER(glGetShaderiv, &own_glGetShaderiv)
        LLVKGL_ANSWER(glGetShaderInfoLog, &own_glGetShaderInfoLog)
        LLVKGL_ANSWER(glDeleteShader, &own_glDeleteShader)
        LLVKGL_ANSWER(glIsShader, &own_glIsShader)
        LLVKGL_ANSWER(glCreateProgram, &own_glCreateProgram)
        LLVKGL_ANSWER(glIsProgram, &own_glIsProgram)
        LLVKGL_ANSWER(glGetProgramiv, &own_glGetProgramiv)
        LLVKGL_ANSWER(glGetProgramInfoLog, &own_glGetProgramInfoLog)
        LLVKGL_ANSWER(glValidateProgram, &own_glValidateProgram)
        LLVKGL_ANSWER(glGetActiveUniform, &own_glGetActiveUniform)
        LLVKGL_ANSWER(glGetAttribLocation, &own_glGetAttribLocation)
        LLVKGL_ANSWER(glGetAttachedShaders, &own_glGetAttachedShaders)
        LLVKGL_ANSWER(glProgramParameteri, &own_glProgramParameteri)
        LLVKGL_ANSWER(glGetProgramBinary, &own_glGetProgramBinary)
#undef LLVKGL_ANSWER
        return nullptr;
    }

} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
