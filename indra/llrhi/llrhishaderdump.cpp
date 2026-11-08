/**
 * @file llrhishaderdump.cpp
 * @brief Opt-in dump of every final GLSL source the viewer compiles (LLRHI_DUMP_SHADERS=<dir>).
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

#include "llrhishaderdump.h"

#include "llfile.h"
#include "llglheaders.h"

#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if LL_GL_FUNC_POINTER

namespace
{
    std::string sDir;
    std::mutex sMutex;
    std::unordered_set<size_t> sWritten;
    U32 sCount = 0;

    std::unordered_map<GLuint, std::string> sShaderFiles;             // shader object -> dumped file
    std::unordered_map<GLuint, std::vector<GLuint>> sProgramShaders;  // program -> attached objects
    std::unordered_set<std::string> sProgramsWritten;

    decltype(glShaderSource) real_glShaderSource = nullptr;
    decltype(glAttachShader) real_glAttachShader = nullptr;
    decltype(glLinkProgram) real_glLinkProgram = nullptr;

    const char* stage_name(GLuint shader)
    {
        GLint type = 0;
        glGetShaderiv(shader, GL_SHADER_TYPE, &type);
        switch (type)
        {
        case GL_VERTEX_SHADER:   return "vert";
        case GL_FRAGMENT_SHADER: return "frag";
        case GL_GEOMETRY_SHADER: return "geom";
        case GL_COMPUTE_SHADER:  return "comp";
        default:                 return "unknown";
        }
    }

    void APIENTRY wrap_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
    {
        std::string source;
        for (GLsizei i = 0; i < count; ++i)
        {
            if (!strings[i])
            {
                continue;
            }
            if (lengths && lengths[i] >= 0)
            {
                source.append(strings[i], lengths[i]);
            }
            else
            {
                source.append(strings[i]);
            }
        }

        const char* stage = stage_name(shader);
        size_t hash = std::hash<std::string>()(source) ^ std::hash<std::string>()(stage);
        {
            std::lock_guard<std::mutex> lock(sMutex);
            char name[64];
            snprintf(name, sizeof(name), "%s_%016llx.glsl", stage, (unsigned long long)hash);
            sShaderFiles[shader] = name;
            if (!sWritten.count(hash))
            {
                // Remember the hash only once the file is written, so a failed
                // open (folder missing, file locked) is retried next time.
                llofstream out(sDir + "/" + name, std::ios::binary);
                if (out.is_open())
                {
                    out << source;
                    sWritten.insert(hash);
                    ++sCount;
                }
            }
        }

        real_glShaderSource(shader, count, strings, lengths);
    }

    void APIENTRY wrap_glAttachShader(GLuint program, GLuint shader)
    {
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sProgramShaders[program].push_back(shader);
        }
        real_glAttachShader(program, shader);
    }

    // One line per distinct program in programs.txt: the files linked together.
    void APIENTRY wrap_glLinkProgram(GLuint program)
    {
        {
            std::lock_guard<std::mutex> lock(sMutex);
            std::string line;
            for (GLuint shader : sProgramShaders[program])
            {
                auto it = sShaderFiles.find(shader);
                line += ' ';
                line += it != sShaderFiles.end() ? it->second : std::string("?");
            }
            sProgramShaders.erase(program);
            if (sProgramsWritten.insert(line).second)
            {
                llofstream out(sDir + "/programs.txt", std::ios::app);
                out << "program" << line << '\n';
            }
        }
        real_glLinkProgram(program);
    }
}

void LLRHIShaderDump::install(const std::string& dir)
{
    if (dir.empty())
    {
        return;
    }
    sDir = dir;
    LLFile::mkdir(sDir);
    LL_INFOS("RHI") << "Dumping compiled GLSL sources to " << sDir << LL_ENDL;
    if (real_glShaderSource || !glShaderSource)
    {
        return;
    }

    real_glShaderSource = glShaderSource;
    glShaderSource = wrap_glShaderSource;
    real_glAttachShader = glAttachShader;
    glAttachShader = wrap_glAttachShader;
    real_glLinkProgram = glLinkProgram;
    glLinkProgram = wrap_glLinkProgram;
}

#else // !LL_GL_FUNC_POINTER

void LLRHIShaderDump::install(const std::string&)
{
    LL_WARNS("RHI") << "Shader dump needs GL function pointers (LL_GL_FUNC_POINTER)" << LL_ENDL;
}

#endif // LL_GL_FUNC_POINTER
