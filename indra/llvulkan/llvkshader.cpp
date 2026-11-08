/**
 * @file llvkshader.cpp
 * @brief Runtime GLSL -> SPIR-V for the viewer's own shaders, plus uniform reflection.
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

#include "linden_common.h"

#include "llvkshader.h"
#include "llvkdevice.h"
#include "llrhi.h"      // LLRHI::envNumber
#include "llrhihash.h"

#include "fsyspath.h"
#include "hbxxh.h"
#include "llapp.h"
#include "lldir.h"
#include "llfile.h"
#include "llstring.h"
#include "llthread.h"

#if LL_WINDOWS
#include "llwin32headers.h"     // SetThreadPriority, GetCurrentProcessId
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/build_info.h>     // GLSLANG_VERSION_* (the GL view cache, research/117 8.6)
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv-tools/optimizer.hpp>     // SPIRV-Tools-opt: linked with glslang already (llvulkan.cmake)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::once_flag sInitOnce;
    std::atomic<bool> sInitialized{ false };
    std::atomic<U32> sLiveWorkers{ 0 };     // threads of LLVKShader::Workers (cleanup must not finalize under them)

    using Clock = std::chrono::steady_clock;

    F64 ms_since(Clock::time_point start)
    {
        return std::chrono::duration<F64, std::milli>(Clock::now() - start).count();
    }

    std::mutex sStatsMutex;
    LLVKShader::Stats sStats;
    Clock::time_point sStatsFirst;              // sStats' first build since the last takeStats
    thread_local LLVKShader::Stats tStats;
    thread_local bool tBackgroundJob = false;   // a job of background Workers runs on this thread (not in sStats)

    void add_stats(LLVKShader::Stats& to, const LLVKShader::Stats& from)
    {
        to.builds += from.builds;
        to.cache_hits += from.cache_hits;
        to.bundle_hits += from.bundle_hits;
        to.compiled += from.compiled;
        to.waited += from.waited;
        to.compile_ms += from.compile_ms;
        to.wait_ms += from.wait_ms;
    }

    // ------------------------------------------------------------------
    // LLRHI_VK_SHADER_KEYLOG=<file> (research/159 section 2.1): one line a build(),
    // tab separated: the key (the cache file's name), where it came from
    // (cache, bundle, compiled, waited: read from the cache after another
    // thread compiled it, failed), milliseconds, the program's files (the
    // "@file" of each stage's file with main) and the defines that tell the
    // presets and the GPU makers apart. To see what misses the bundle and
    // add those settings to the next bake. Unset: nothing is worked out.

    enum class From
    {
        Cache,
        Bundle,
        Compiled,
        Failed,
    };

    const char* from_name(From from, bool waited)
    {
        switch (from)
        {
        case From::Cache:    return waited ? "waited" : "cache";
        case From::Bundle:   return "bundle";
        case From::Compiled: return "compiled";
        default:             return "failed";
        }
    }

    // The defines of sGlobalDefines and the GPU maker (research/159 section 1.4),
    // and those of the programs a preset picks (anti-aliasing).
    bool keylog_define(const std::string& name)
    {
        static const char* const NAMES[] = { "SUN_SHADOW", "SPOT_SHADOW", "REFMAP_LEVEL", "SSR", "HERO_PROBES", "HAS_EMISSIVE",
                                             "PLUS_AVATAR_REFL", "PLUS_SSR", "IS_AMD_CARD", "AVATAR_CLOTH", "TERRAIN_PBR_DETAIL",
                                             "FXAA_QUALITY__PRESET" };
        for (const char* known : NAMES)
        {
            if (name == known)
            {
                return true;
            }
        }
        return name.compare(0, 12, "SMAA_PRESET_") == 0;
    }

    // "files<TAB>defines" of a build's sources.
    std::string keylog_program(const std::vector<LLVKShader::Source>& sources, U32 resource_set)
    {
        std::string files;
        std::set<std::string> defines;
        bool oit = false;
        for (const LLVKShader::Source& source : sources)
        {
            const std::string& text = source.text;
            const size_t file = text.find("@file ");
            if (file != std::string::npos && text.find("main(") != std::string::npos)
            {
                const size_t start = file + 6;
                const size_t end = text.find_first_of(" \t\r\n", start);
                files += (files.empty() ? "" : "+") + text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            }
            for (size_t at = text.find("#define "); at != std::string::npos; at = text.find("#define ", at + 8))
            {
                if (at && text[at - 1] != '\n')
                {
                    continue;   // only at a line's start
                }
                const size_t name_start = at + 8;
                const size_t name_end = text.find_first_of(" \t\r\n", name_start);
                const std::string name = text.substr(name_start, name_end == std::string::npos ? std::string::npos : name_end - name_start);
                if (!keylog_define(name))
                {
                    continue;
                }
                std::string value;
                if (name_end != std::string::npos && text[name_end] != '\n' && text[name_end] != '\r')
                {
                    const size_t line_end = text.find_first_of("\r\n", name_end);
                    value = text.substr(name_end + 1, line_end == std::string::npos ? std::string::npos : line_end - name_end - 1);
                    LLStringUtil::trim(value);
                }
                defines.insert(value.empty() ? name : name + "=" + value);
            }
            oit |= text.find("llrhi_oit") != std::string::npos;
        }
        if (files.empty())
        {
            files = "?";    // the Vulkan side's own passes have no "@file"
        }
        std::string out = files + "\tset=" + std::to_string(resource_set);
        if (oit)
        {
            out += " OIT";
        }
        for (const std::string& define : defines)
        {
            out += " " + define;
        }
        return out;
    }

    std::mutex sKeylogMutex;
    LLFILE* sKeylog = nullptr;      // under sKeylogMutex; closed by LLVKShader::cleanup

    bool keylog_on()
    {
        static const std::string path = LLStringUtil::getenv("LLRHI_VK_SHADER_KEYLOG");    // UTF-8, as LLFile takes it
        return !path.empty();
    }

    void keylog(U64 key, From from, bool waited, F64 ms, const std::vector<LLVKShader::Source>& sources, U32 resource_set)
    {
        if (!keylog_on())
        {
            return;
        }
        // Worked out before the lock: builds on other threads go on meanwhile.
        const std::string program = keylog_program(sources, resource_set);
        char head[64];
        snprintf(head, sizeof(head), "%016llx\t%s\t%.1f\t", (unsigned long long)key, from_name(from, waited), ms);
        std::lock_guard<std::mutex> lock(sKeylogMutex);
        static bool opened = false;
        if (!opened)
        {
            opened = true;
            const std::string path = LLStringUtil::getenv("LLRHI_VK_SHADER_KEYLOG");
            sKeylog = LLFile::fopen(path, LLFILE_MODE("ab"));
            if (!sKeylog)
            {
                LL_WARNS("VKShader") << "LLRHI_VK_SHADER_KEYLOG: " << path << " cannot be opened: no key log" << LL_ENDL;
                return;
            }
            LL_INFOS("VKShader") << "Shader key log: " << path << LL_ENDL;
            fputs("# key\tfrom\tms\tfiles\tset, defines\n", sKeylog);
        }
        if (sKeylog)
        {
            fputs(head, sKeylog);
            fputs(program.c_str(), sKeylog);
            fputc('\n', sKeylog);
            fflush(sKeylog);    // a crash keeps the lines so far
        }
    }

    // research/111 B-SC: sources being compiled by some thread right now (by
    // their hash). Another build() of the same sources waits for that one and
    // reads its result from the cache instead of compiling them again.
    std::mutex sInFlightMutex;
    std::condition_variable sInFlightDone;
    std::unordered_set<U64> sInFlight;

    // A thread's claim on sources in sInFlight (the caller inserted the key
    // under sInFlightMutex), given back when the compile is over, however it
    // ends. Made while sInFlightMutex is held and destroyed after it is let
    // go: the destructor locks it. Not copyable, so no temporary gives the
    // claim back early (or locks the mutex its maker still holds: the login
    // crash of 2026-10-17, std::system_error thrown out of a destructor).
    class InFlightClaim
    {
    public:
        explicit InFlightClaim(U64 key) : mKey(key) {}
        InFlightClaim(const InFlightClaim&) = delete;
        InFlightClaim& operator=(const InFlightClaim&) = delete;
        ~InFlightClaim()
        {
            {
                std::lock_guard<std::mutex> lock(sInFlightMutex);
                sInFlight.erase(mKey);
            }
            sInFlightDone.notify_all();
        }

    private:
        U64 mKey;
    };

    EShLanguage to_glslang(LLVKShader::Stage stage)
    {
        switch (stage)
        {
        case LLVKShader::Stage::Vertex:   return EShLangVertex;
        case LLVKShader::Stage::Fragment: return EShLangFragment;
        case LLVKShader::Stage::Geometry: return EShLangGeometry;
        default:                          return EShLangCompute;
        }
    }

    // SPIR-V words we touch: OpName, OpVariable, OpDecorate Location / BuiltIn.
    constexpr U32 OP_NAME = 5, OP_VARIABLE = 59, OP_DECORATE = 71;
    constexpr U32 DECORATION_LOCATION = 30;
    constexpr U32 STORAGE_INPUT = 1, STORAGE_OUTPUT = 3;

    struct Interface
    {
        std::unordered_map<U32, std::string> names;     // id -> OpName
        std::unordered_map<U32, U32> storage;           // variable id -> storage class
        std::unordered_map<U32, size_t> location_word;  // variable id -> index of its Location literal
    };

    // An instruction's literal string from word `first`, up to its NUL and never past
    // the instruction's last word (end: the word after it). A module without the NUL
    // (a damaged cache file) gives a name cut at the instruction's end.
    std::string spirv_string(const std::vector<unsigned int>& spirv, size_t first, size_t end)
    {
        if (first >= end || end > spirv.size())
        {
            return std::string();
        }
        const char* text = (const char*)(spirv.data() + first);
        return std::string(text, strnlen(text, (end - first) * sizeof(unsigned int)));
    }

    Interface scan(const std::vector<unsigned int>& spirv)
    {
        Interface out;
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16;
            U32 op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                break;
            }
            if (op == OP_NAME && count > 2)
            {
                out.names[spirv[i + 1]] = spirv_string(spirv, i + 2, i + count);
            }
            else if (op == OP_VARIABLE && count >= 4)
            {
                out.storage[spirv[i + 2]] = spirv[i + 3];
            }
            else if (op == OP_DECORATE && count >= 4 && spirv[i + 2] == DECORATION_LOCATION)
            {
                out.location_word[spirv[i + 1]] = i + 3;
            }
            i += count;
        }
        return out;
    }

    // Turn consumer inputs no earlier stage writes into Private variables.
    // GL gives them undefined values; Vulkan rejects an input without a
    // matching output, so keep the shader valid with the same semantics.
    void privatize_inputs(std::vector<unsigned int>& spirv, const std::vector<U32>& variables)
    {
        constexpr U32 OP_TYPE_POINTER = 32, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67;
        constexpr U32 STORAGE_PRIVATE = 6;
        if (variables.empty() || spirv.size() < 5)
        {
            return;
        }
        std::unordered_map<U32, U32> pointee;           // Input pointer type -> pointee type
        std::unordered_map<U32, bool> affected;         // variables and access chains rooted at them
        std::unordered_map<U32, U32> type_of;           // affected id -> its (Input) pointer type
        for (U32 v : variables)
        {
            affected[v] = true;
        }
        // Pass 1: pointer types, variable types, and access chains through affected ids (in order).
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                return;
            }
            if (op == OP_TYPE_POINTER && count >= 4 && spirv[i + 2] == STORAGE_INPUT)
            {
                pointee[spirv[i + 1]] = spirv[i + 3];
            }
            else if (op == OP_VARIABLE && count >= 4 && affected.count(spirv[i + 2]))
            {
                type_of[spirv[i + 2]] = spirv[i + 1];
            }
            else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN) && count >= 4 &&
                     affected.count(spirv[i + 3]))
            {
                affected[spirv[i + 2]] = true;
                type_of[spirv[i + 2]] = spirv[i + 1];
            }
            i += count;
        }
        // A Private twin for every Input pointer type those ids use.
        U32 bound = spirv[3];
        std::unordered_map<U32, U32> twin;
        for (const auto& t : type_of)
        {
            if (pointee.count(t.second) && !twin.count(t.second))
            {
                twin[t.second] = bound++;
            }
        }
        // Pass 2: rebuild.
        std::vector<unsigned int> out(spirv.begin(), spirv.begin() + 5);
        out[3] = bound;
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (op == OP_DECORATE && count >= 3 && affected.count(spirv[i + 1]))
            {
                i += count;     // Location, Flat, ... are not valid on Private variables
                continue;
            }
            size_t start = out.size();
            out.insert(out.end(), spirv.begin() + i, spirv.begin() + i + count);
            if (op == OP_TYPE_POINTER && twin.count(spirv[i + 1]))
            {
                out.push_back((4u << 16) | OP_TYPE_POINTER);
                out.push_back(twin[spirv[i + 1]]);
                out.push_back(STORAGE_PRIVATE);
                out.push_back(spirv[i + 3]);
            }
            else if (op == OP_VARIABLE && affected.count(spirv[i + 2]) && twin.count(spirv[i + 1]))
            {
                out[start + 1] = twin[spirv[i + 1]];
                out[start + 3] = STORAGE_PRIVATE;
            }
            else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN) &&
                     affected.count(spirv[i + 2]) && twin.count(spirv[i + 1]))
            {
                out[start + 1] = twin[spirv[i + 1]];
            }
            i += count;
        }
        spirv.swap(out);
    }

    void match_varyings(const std::vector<unsigned int>& producer, std::vector<unsigned int>& consumer)
    {
        Interface out = scan(producer);
        Interface in = scan(consumer);

        std::unordered_map<std::string, U32> produced;
        U32 next_free = 0;
        for (const auto& var : out.location_word)
        {
            if (out.storage[var.first] == STORAGE_OUTPUT)
            {
                produced[out.names[var.first]] = producer[var.second];
                next_free = llmax(next_free, producer[var.second] + 4);
            }
        }
        std::vector<U32> unmatched;
        for (const auto& var : in.location_word)
        {
            if (in.storage[var.first] != STORAGE_INPUT)
            {
                continue;
            }
            auto match = produced.find(in.names[var.first]);
            if (match != produced.end())
            {
                consumer[var.second] = match->second;
            }
            else
            {
                consumer[var.second] = next_free++;
                unmatched.push_back(var.first);
            }
        }
        privatize_inputs(consumer, unmatched);
    }

    // glslang lists every declared global in the entry point interface, read or
    // not, and the validation layers treat listed descriptors as used. GL only
    // binds what the program reads (reflection drops the rest), so take
    // descriptors nothing reads out of the interface. A descriptor is only read
    // inside a function; the module's header holds literals (OpSource's GLSL
    // version 420 met a sampler's id: research/106), so only function bodies count.
    void drop_unread_descriptors(std::vector<unsigned int>& spirv)
    {
        constexpr U32 OP_ENTRY_POINT = 15, OP_FUNCTION = 54, OP_LINE = 8, OP_NO_LINE = 317, OP_EXT_INST = 12;
        constexpr U32 STORAGE_UNIFORM_CONSTANT = 0, STORAGE_UNIFORM = 2, STORAGE_STORAGE_BUFFER = 12;
        std::unordered_map<U32, U32> reads;     // descriptor variable id -> references outside annotations
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                return;
            }
            if (op == OP_VARIABLE && count >= 4 && (spirv[i + 3] == STORAGE_UNIFORM_CONSTANT || spirv[i + 3] == STORAGE_UNIFORM ||
                                                    spirv[i + 3] == STORAGE_STORAGE_BUFFER))
            {
                reads[spirv[i + 2]] = 0;
            }
            i += count;
        }
        if (reads.empty())
        {
            return;
        }
        // In a function body, any word equal to the id counts (a literal there
        // may match too: that only keeps a variable). Line info and an extended
        // instruction's number are literals only.
        bool in_function = false;
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            in_function |= op == OP_FUNCTION;
            if (in_function && op != OP_LINE && op != OP_NO_LINE)
            {
                // OpExtInst: result type, result, set, instruction number, operands.
                const size_t first = op == OP_EXT_INST ? i + 5 : i + 1;
                for (size_t w = first; w < i + count; ++w)
                {
                    auto it = reads.find(spirv[w]);
                    if (it != reads.end())
                    {
                        ++it->second;
                    }
                }
            }
            i += count;
        }
        std::vector<unsigned int> out(spirv.begin(), spirv.begin() + 5);
        out.reserve(spirv.size());
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (op != OP_ENTRY_POINT || count < 4)
            {
                out.insert(out.end(), spirv.begin() + i, spirv.begin() + i + count);
                i += count;
                continue;
            }
            // execution model, id, null-terminated name, then the interface ids
            size_t w = i + 3;
            while (w < i + count && (spirv[w] & 0xff) && (spirv[w] & 0xff00) && (spirv[w] & 0xff0000) && (spirv[w] & 0xff000000))
            {
                ++w;
            }
            ++w;
            const size_t start = out.size();
            out.insert(out.end(), spirv.begin() + i, spirv.begin() + llmin(w, (size_t)(i + count)));
            for (; w < i + count; ++w)
            {
                auto it = reads.find(spirv[w]);
                if (it == reads.end() || it->second)
                {
                    out.push_back(spirv[w]);
                }
            }
            out[start] = (U32)((out.size() - start) << 16) | OP_ENTRY_POINT;
            i += count;
        }
        spirv.swap(out);
    }

    // glslang's resource-set option misses some samplers (left in set 0 next to
    // the bindless table): move every descriptor except the table into `set`.
    // The OIT variant's lists (llrhi_oit*, set 2: research/41) stay too.
    void move_to_set(std::vector<unsigned int>& spirv, U32 set)
    {
        constexpr U32 DECORATION_DESCRIPTOR_SET = 34;
        Interface io = scan(spirv);
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16;
            U32 op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                break;
            }
            if (op == OP_DECORATE && count >= 4 && spirv[i + 2] == DECORATION_DESCRIPTOR_SET &&
                io.names[spirv[i + 1]] != "llrhi_textures" && io.names[spirv[i + 1]] != "llrhi_mt" &&
                io.names[spirv[i + 1]] != "llrhi_pal" && io.names[spirv[i + 1]].compare(0, 9, "llrhi_oit") != 0)
            {
                spirv[i + 3] = set;
            }
            i += count;
        }
    }

    // Declared vertex inputs with a Location, typed as GL_FLOAT / GL_INT / GL_UNSIGNED_INT
    // by their scalar component.
    std::vector<LLVKShader::Input> vertex_inputs(const std::vector<unsigned int>& spirv)
    {
        constexpr U32 OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_POINTER = 32;
        constexpr U32 GL_INT_TYPE = 0x1404, GL_UINT_TYPE = 0x1405, GL_FLOAT_TYPE = 0x1406;
        std::unordered_map<U32, U32> scalar;       // type id -> GL scalar type
        std::unordered_map<U32, U32> pointee;      // pointer type id -> pointee type id
        std::unordered_map<U32, U32> var_type;     // input variable id -> pointer type id
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16;
            U32 op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                break;
            }
            if (op == OP_TYPE_INT && count >= 4)
            {
                scalar[spirv[i + 1]] = spirv[i + 3] ? GL_INT_TYPE : GL_UINT_TYPE;
            }
            else if (op == OP_TYPE_FLOAT && count >= 3)
            {
                scalar[spirv[i + 1]] = GL_FLOAT_TYPE;
            }
            else if (op == OP_TYPE_VECTOR && count >= 4)
            {
                scalar[spirv[i + 1]] = scalar[spirv[i + 2]];
            }
            else if (op == OP_TYPE_POINTER && count >= 4)
            {
                pointee[spirv[i + 1]] = spirv[i + 3];
            }
            else if (op == OP_VARIABLE && count >= 4 && spirv[i + 3] == STORAGE_INPUT)
            {
                var_type[spirv[i + 2]] = spirv[i + 1];
            }
            i += count;
        }
        Interface io = scan(spirv);
        std::vector<LLVKShader::Input> inputs;
        for (const auto& var : io.location_word)
        {
            auto type = var_type.find(var.first);
            if (type == var_type.end())
            {
                continue;
            }
            U32 gl_type = scalar[pointee[type->second]];
            inputs.push_back({ io.names[var.first], spirv[var.second], gl_type ? gl_type : GL_FLOAT_TYPE });
        }
        return inputs;
    }

    // ------------------------------------------------------------------
    // On-disk cache: SPIR-V per stage (varyings already matched) plus the
    // reflection, keyed by the exact assembled sources. Compiling all the
    // viewer's programs takes seconds; loading them takes milliseconds.

    constexpr U32 CACHE_MAGIC = 0x4b564c4c;     // "LLVK"
    constexpr U32 CACHE_VERSION = 7;            // bump when the build pipeline or the file layout changes (7: the trailer)
    // A program (a .vkp file, a bundle's program part) has a module per stage at most (vertex .. compute): a count
    // above it is a damaged or made-up file, whose loop would go on for as many entries as the file can point at
    // (each a copy of a module: gigabytes).
    constexpr U32 MAX_PROGRAM_MODULES = (U32)EShLangCompute + 1;
    // The GL view files (below) and the shader bundle say which glslang made
    // them; a .vkp file's key does not yet (research/159 section 1.1): from the next
    // CACHE_VERSION on it does (source_hash). Until then a new SDK's glslang
    // comes with a CACHE_VERSION bump.
    constexpr U32 GL_VIEW_GLSLANG = GLSLANG_VERSION_MAJOR * 10000 + GLSLANG_VERSION_MINOR * 100 + GLSLANG_VERSION_PATCH;
    constexpr U32 GL_VIEW_MAGIC = 0x56474c4c;   // "LLGV"
    constexpr U32 GL_VIEW_VERSION = 2;          // bump when reflectGL's answers or the file layout change (2: the trailer)

    // The cache's key: the cache version, the resource set and every stage's
    // source. Also LLVKShader::Program::key (research/111 B-PL).
    U64 source_hash(const std::vector<LLVKShader::Source>& sources, U32 resource_set)
    {
        U64 hash = LLRHI::FNV1A_BASIS;
        U32 version = CACHE_VERSION;
        hash = LLRHI::fnv1a(hash, &version, sizeof(version));
        if constexpr (CACHE_VERSION >= 8)
        {
            // Not before: every key of today's caches would change without a bump.
            const U32 glslang = GL_VIEW_GLSLANG;
            hash = LLRHI::fnv1a(hash, &glslang, sizeof(glslang));
        }
        hash = LLRHI::fnv1a(hash, &resource_set, sizeof(resource_set));
        for (const auto& source : sources)
        {
            U32 stage = (U32)source.stage;
            hash = LLRHI::fnv1a(hash, &stage, sizeof(stage));
            hash = LLRHI::fnv1a(hash, source.text.data(), source.text.size());
        }
        return hash;
    }

    std::string cache_path(U64 hash)
    {
        const std::string& dir = LLVKShader::cacheDir();
        if (dir.empty())
        {
            return std::string();
        }
        char name[32];
        snprintf(name, sizeof(name), "/%016llx.vkp", (unsigned long long)hash);
        return dir + name;
    }

    void put_u32(std::string& out, U32 v) { out.append((const char*)&v, 4); }
    void put_s32(std::string& out, S32 v) { out.append((const char*)&v, 4); }
    void put_str(std::string& out, const std::string& s) { put_u32(out, (U32)s.size()); out += s; }

    // Over a cache file read into a string, or over a part of the mapped bundle.
    struct Reader
    {
        const char* data;
        size_t size;
        size_t pos = 0;
        bool ok = true;
        explicit Reader(const std::string& text) : data(text.data()), size(text.size()) {}
        Reader(const char* bytes, size_t length) : data(bytes), size(length) {}
        U32 u32() { U32 v = 0; if (pos + 4 <= size) { memcpy(&v, data + pos, 4); pos += 4; } else ok = false; return v; }
        S32 s32() { return (S32)u32(); }
        std::string str() { U32 n = u32(); if (ok && n <= size - pos) { std::string s(data + pos, n); pos += n; return s; } ok = false; return std::string(); }
    };

    // Every .vkp and .glv file ends in a trailer: the size and the hash of
    // what comes before it. A file cut short, padded with zeros or otherwise
    // damaged (a crash, a full disk) is then made again instead of read.
    constexpr size_t TRAILER_SIZE = 2 * sizeof(U64);

    void put_trailer(std::string& out)
    {
        const U64 size = out.size();
        const U64 hash = HBXXH64::digest(out.data(), out.size());
        out.append((const char*)&size, sizeof(size));
        out.append((const char*)&hash, sizeof(hash));
    }

    // The file without its trailer, or false when the trailer does not match.
    bool strip_trailer(std::string& data)
    {
        if (data.size() < TRAILER_SIZE)
        {
            return false;
        }
        const size_t body = data.size() - TRAILER_SIZE;
        U64 size = 0;
        U64 hash = 0;
        memcpy(&size, data.data() + body, sizeof(size));
        memcpy(&hash, data.data() + body + sizeof(size), sizeof(hash));
        if (size != body || hash != HBXXH64::digest(data.data(), body))
        {
            return false;
        }
        data.resize(body);
        return true;
    }

    // fix-logcache: a .vkp or .glv file read and found whole is marked used
    // by its write time, at most once a day (TOUCH_AFTER: a hit then costs
    // one more stat, not a write), so that trim_cache keeps what is in use
    // and removes what no start reads any more. A failure changes nothing.
    constexpr auto TOUCH_AFTER = std::chrono::hours(24);

    void mark_used(const std::string& path)
    {
        std::error_code ec;
        const std::filesystem::path file = fsyspath(path);
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto written = std::filesystem::last_write_time(file, ec);
        if (!ec && now - written > TOUCH_AFTER)
        {
            std::filesystem::last_write_time(file, now, ec);
        }
    }

    // Write then rename (LLVKShader::writeCacheFile). The temporary file is
    // this thread's own: another viewer (a second instance) may be writing
    // the same entry (research/111 B-SC).
    void write_atomically(const std::string& path, std::string& out)
    {
        put_trailer(out);
        // A failure leaves the entry as it was: the next miss writes it again.
        LLVKShader::writeCacheFile(path, { { out.data(), out.size() } });
    }

    void save_cache(const std::string& path, const std::vector<std::pair<EShLanguage, std::vector<unsigned int>>>& modules,
                    const LLVKShader::Program& program)
    {
        if (path.empty())
        {
            return;
        }
        std::string out;
        put_u32(out, CACHE_MAGIC);
        put_u32(out, CACHE_VERSION);
        put_u32(out, (U32)modules.size());
        for (const auto& m : modules)
        {
            put_u32(out, (U32)m.first);
            put_u32(out, (U32)m.second.size());
            out.append((const char*)m.second.data(), m.second.size() * 4);
        }
        put_u32(out, (U32)program.uniforms.size());
        for (const auto& u : program.uniforms)
        {
            put_str(out, u.name); put_u32(out, u.gl_type); put_s32(out, u.offset); put_s32(out, u.binding);
            put_s32(out, u.size); put_s32(out, u.array_stride); put_s32(out, u.block); put_u32(out, u.stages);
        }
        put_u32(out, (U32)program.blocks.size());
        for (const auto& b : program.blocks)
        {
            put_str(out, b.name); put_s32(out, b.binding); put_s32(out, b.size);
        }
        put_u32(out, (U32)program.inputs.size());
        for (const auto& in : program.inputs)
        {
            put_str(out, in.name); put_u32(out, in.location); put_u32(out, in.gl_type);
        }
        write_atomically(path, out);
    }

    // A cached module looks like SPIR-V: the magic number, then instructions that
    // end exactly at its end (the scans and the driver read by their lengths).
    bool whole_module(const std::vector<unsigned int>& spirv)
    {
        constexpr U32 SPIRV_MAGIC = 0x07230203;
        if (spirv.size() < 5 || spirv[0] != SPIRV_MAGIC)
        {
            return false;
        }
        for (size_t i = 5; i < spirv.size();)
        {
            const U32 count = spirv[i] >> 16;
            if (!count || i + count > spirv.size())
            {
                return false;
            }
            i += count;
        }
        return true;
    }

    // The reflection after the modules (the .vkp layout, also the bundle's),
    // to the reader's end. False, with the modules and the reflection
    // cleared, when anything is missing or left over.
    bool read_reflection(Reader& r, std::vector<std::pair<EShLanguage, std::vector<unsigned int>>>& modules,
                         LLVKShader::Program& program)
    {
        U32 count = r.u32();
        for (U32 i = 0; i < count && r.ok; ++i)
        {
            LLVKShader::Uniform u;
            u.name = r.str(); u.gl_type = r.u32(); u.offset = r.s32(); u.binding = r.s32();
            u.size = r.s32(); u.array_stride = r.s32(); u.block = r.s32(); u.stages = r.u32();
            program.uniforms.push_back(u);
        }
        count = r.u32();
        for (U32 i = 0; i < count && r.ok; ++i)
        {
            LLVKShader::Block b;
            b.name = r.str(); b.binding = r.s32(); b.size = r.s32();
            program.blocks.push_back(b);
        }
        count = r.u32();
        for (U32 i = 0; i < count && r.ok; ++i)
        {
            LLVKShader::Input in;
            in.name = r.str(); in.location = r.u32(); in.gl_type = r.u32();
            program.inputs.push_back(in);
        }
        if (!r.ok || modules.empty() || r.pos != r.size)
        {
            modules.clear();
            program.uniforms.clear();
            program.blocks.clear();
            program.inputs.clear();
            return false;
        }
        return true;
    }

    bool load_cache(const std::string& path, std::vector<std::pair<EShLanguage, std::vector<unsigned int>>>& modules,
                    LLVKShader::Program& program)
    {
        std::string data;
        if (!LLVKShader::readCacheFile(path, data) || !strip_trailer(data))
        {
            return false;
        }
        Reader r{ data };
        if (r.u32() != CACHE_MAGIC || r.u32() != CACHE_VERSION)
        {
            return false;
        }
        U32 module_count = r.u32();
        if (module_count > MAX_PROGRAM_MODULES)
        {
            return false;
        }
        for (U32 i = 0; i < module_count && r.ok; ++i)
        {
            const U32 lang_number = r.u32();
            const EShLanguage lang = (EShLanguage)lang_number;
            U32 words = r.u32();
            if (!r.ok || lang_number > (U32)EShLangCompute || words < 5 || r.pos + (size_t)words * 4 > data.size())
            {
                r.ok = false;   // no module is shorter than its header: vkCreateShaderModule would be given nothing
                break;
            }
            std::vector<unsigned int> spirv(words);
            memcpy(spirv.data(), &data[r.pos], (size_t)words * 4);
            r.pos += (size_t)words * 4;
            if (!whole_module(spirv))
            {
                r.ok = false;
                break;
            }
            modules.emplace_back(lang, std::move(spirv));
        }
        if (!read_reflection(r, modules, program))
        {
            return false;
        }
        mark_used(path);
        return true;
    }

    // ------------------------------------------------------------------
    // The shader bundle (research/159 sections 3.3, 4): SPIR-V made at
    // release time for the presets and their neighbours, shipped in
    // app_settings/vk_shader_bundle/shaders.vkb (tools/pack-shader-bundle.py
    // makes it from cache folders). Read after the user's cache misses and
    // before glslang; what it answers is not copied into the cache. The file
    // is mapped once, the first time the cache misses, and stays mapped.
    //
    // Layout (little endian), made by tools/pack-shader-bundle.py:
    //   header, BUNDLE_HEADER bytes:
    //     u32 magic "LLVB", u32 BUNDLE_VERSION, u32 CACHE_VERSION,
    //     u32 GL_VIEW_VERSION, u32 GL_VIEW_GLSLANG,
    //     u32 programs, u32 GL views, u32 modules,
    //     u64 program table, u64 GL view table, u64 module table (offsets),
    //     char[40] the viewer's commit (ASCII, zero padded; told in the log)
    //   program and GL view tables: BUNDLE_ENTRY bytes an entry, by key:
    //     u64 key (the .vkp / .glv file's name), u64 offset, u32 size, u32 0
    //   module table: BUNDLE_MODULE_ENTRY bytes an entry:
    //     u64 offset, u32 words, u32 0
    //   a program: u32 modules, then u32 stage (EShLanguage) and u32 module
    //     index each, then the reflection as a .vkp file has it
    //   a GL view: the .glv file without its trailer
    //   a module: its SPIR-V words (4 byte aligned; one copy of each module)
    //   the trailer of the cache files: u64 size, u64 XXH3-64 of all before it
    // Any version, the size or the hash differing: the bundle is not used.

    constexpr U32 BUNDLE_MAGIC = 0x42564c4c;    // "LLVB"
    constexpr U32 BUNDLE_VERSION = 1;           // bump with the layout (and the packing tool's)
    constexpr size_t BUNDLE_HEADER = 96;
    constexpr size_t BUNDLE_COMMIT = 40;
    constexpr size_t BUNDLE_ENTRY = 24;
    constexpr size_t BUNDLE_MODULE_ENTRY = 16;

    struct Bundle
    {
        const char* data = nullptr;     // the file without its trailer; null: no bundle
        size_t size = 0;
        U32 programs = 0;
        U32 views = 0;
        U32 modules = 0;
        U64 program_table = 0;
        U64 view_table = 0;
        U64 module_table = 0;
    };
    std::once_flag sBundleOnce;
    Bundle sBundle;                     // set once (sBundleOnce), then only read
    std::atomic<bool> sBundleDamageTold{ false };

    U32 read_u32(const char* at) { U32 v; memcpy(&v, at, sizeof(v)); return v; }
    U64 read_u64(const char* at) { U64 v; memcpy(&v, at, sizeof(v)); return v; }

    // LLRHI_VK_SHADER_BUNDLE: unset or 1, the one in app_settings; 0, none
    // (as before the bundle: the control for measuring a first start);
    // anything else, that file. Empty: none.
    std::string bundle_path()
    {
        const std::string env = LLStringUtil::getenv("LLRHI_VK_SHADER_BUNDLE");     // UTF-8
        if (env == "0")
        {
            return std::string();
        }
        if (!env.empty() && env != "1")
        {
            return env;
        }
        return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "vk_shader_bundle", "shaders.vkb");
    }

    // The whole file, read only. Null when it cannot be opened, is empty or
    // cannot be mapped.
    const char* map_file(const std::string& path, size_t& size)
    {
#if LL_WINDOWS
        const std::filesystem::path file_path = fsyspath(path);
        HANDLE file = CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return nullptr;
        }
        const char* view = nullptr;
        LARGE_INTEGER length = {};
        if (GetFileSizeEx(file, &length) && length.QuadPart > 0 && (U64)length.QuadPart <= (U64)SIZE_MAX)
        {
            HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping)
            {
                view = (const char*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
                CloseHandle(mapping);   // the view keeps the mapping
            }
        }
        CloseHandle(file);              // and the mapping the file
        size = view ? (size_t)length.QuadPart : 0;
        return view;
#else
        std::string* data = new std::string();
        if (!LLVKShader::readCacheFile(path, *data) || data->empty())
        {
            delete data;
            return nullptr;
        }
        size = data->size();
        return data->data();    // kept for the process, as a mapping would be
#endif
    }

    void unmap_file(const char* data)
    {
#if LL_WINDOWS
        UnmapViewOfFile(data);
#else
        (void)data;     // a refused file's copy stays (once a process; no bundle is the rare case)
#endif
    }

    // Once (sBundleOnce): sBundle, or nothing with one line saying why.
    void open_bundle()
    {
        const std::string path = bundle_path();
        if (path.empty())
        {
            LL_INFOS("VKShader") << "Shader bundle: off (LLRHI_VK_SHADER_BUNDLE=0)" << LL_ENDL;
            return;
        }
        if (!LLFile::isfile(path))
        {
            LL_INFOS("VKShader") << "Shader bundle: none at " << path << " (shaders not in the cache are compiled)" << LL_ENDL;
            return;
        }
        const Clock::time_point start = Clock::now();
        size_t size = 0;
        const char* data = map_file(path, size);
        auto refuse = [&](const std::string& why)
        {
            LL_WARNS("VKShader") << "Shader bundle " << path << " not used: " << why << LL_ENDL;
            if (data)
            {
                unmap_file(data);
            }
        };
        if (!data)
        {
            refuse("it cannot be read");
            return;
        }
        if (size < BUNDLE_HEADER + TRAILER_SIZE || read_u32(data) != BUNDLE_MAGIC)
        {
            refuse("not a shader bundle");
            return;
        }
        const U32 version = read_u32(data + 4);
        const U32 cache_version = read_u32(data + 8);
        const U32 view_version = read_u32(data + 12);
        const U32 glslang = read_u32(data + 16);
        if (version != BUNDLE_VERSION || cache_version != CACHE_VERSION || view_version != GL_VIEW_VERSION || glslang != GL_VIEW_GLSLANG)
        {
            std::ostringstream why;
            why << "made for another build (bundle " << version << ", cache " << cache_version << ", GL view " << view_version
                << ", glslang " << glslang << "; this build: " << BUNDLE_VERSION << ", " << CACHE_VERSION << ", " << GL_VIEW_VERSION << ", "
                << GL_VIEW_GLSLANG << ")";
            refuse(why.str());
            return;
        }
        const size_t body = size - TRAILER_SIZE;
        if (read_u64(data + body) != body || read_u64(data + body + sizeof(U64)) != HBXXH64::digest(data, body))
        {
            refuse("damaged (its trailer does not match)");
            return;
        }
        Bundle bundle;
        bundle.data = data;
        bundle.size = body;
        bundle.programs = read_u32(data + 20);
        bundle.views = read_u32(data + 24);
        bundle.modules = read_u32(data + 28);
        bundle.program_table = read_u64(data + 32);
        bundle.view_table = read_u64(data + 40);
        bundle.module_table = read_u64(data + 48);
        auto inside = [body](U64 table, U32 count, size_t entry)
        {
            return table >= BUNDLE_HEADER && table <= body && (U64)count * entry <= body - table;
        };
        if (!inside(bundle.program_table, bundle.programs, BUNDLE_ENTRY) || !inside(bundle.view_table, bundle.views, BUNDLE_ENTRY) ||
            !inside(bundle.module_table, bundle.modules, BUNDLE_MODULE_ENTRY))
        {
            refuse("damaged (a table lies outside it)");
            return;
        }
        std::string commit(data + 56, BUNDLE_COMMIT);
        commit.resize(strnlen(commit.c_str(), BUNDLE_COMMIT));
        sBundle = bundle;
        LL_INFOS("VKShader") << "Shader bundle: " << path << ": " << bundle.programs << " programs, " << bundle.views << " GL views, "
                             << bundle.modules << " modules, " << (size >> 20) << " MB, made at " << (commit.empty() ? "?" : commit)
                             << "; checked in " << ms_since(start) << " ms" << LL_ENDL;
    }

    const Bundle* bundle()
    {
        std::call_once(sBundleOnce, open_bundle);
        return sBundle.data ? &sBundle : nullptr;
    }

    // `key` in a sorted table of the bundle: its part of the bundle.
    bool bundle_find(const Bundle& b, U64 table, U32 count, U64 key, const char*& part, size_t& size)
    {
        U32 low = 0, high = count;
        while (low < high)
        {
            const U32 middle = low + (high - low) / 2;
            if (read_u64(b.data + table + (size_t)middle * BUNDLE_ENTRY) < key)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        if (low == count)
        {
            return false;
        }
        const char* entry = b.data + table + (size_t)low * BUNDLE_ENTRY;
        if (read_u64(entry) != key)
        {
            return false;
        }
        const U64 offset = read_u64(entry + 8);
        const U32 length = read_u32(entry + 16);
        if (offset > b.size || length > b.size - offset)
        {
            return false;
        }
        part = b.data + offset;
        size = length;
        return true;
    }

    void tell_bundle_damage()
    {
        if (!sBundleDamageTold.exchange(true))
        {
            LL_WARNS("VKShader") << "Shader bundle: an entry is damaged (compiled instead); the bundle should be made again" << LL_ENDL;
        }
    }

    // The program of `key` from the bundle, as load_cache reads a .vkp file.
    bool load_bundle(U64 key, std::vector<std::pair<EShLanguage, std::vector<unsigned int>>>& modules, LLVKShader::Program& program)
    {
        const Bundle* b = bundle();
        const char* part = nullptr;
        size_t size = 0;
        if (!b || !bundle_find(*b, b->program_table, b->programs, key, part, size))
        {
            return false;
        }
        Reader r(part, size);
        const U32 module_count = r.u32();
        if (module_count > MAX_PROGRAM_MODULES)
        {
            tell_bundle_damage();
            return false;
        }
        for (U32 i = 0; i < module_count && r.ok; ++i)
        {
            const U32 lang = r.u32();
            const U32 index = r.u32();
            if (!r.ok || lang > (U32)EShLangCompute || index >= b->modules)
            {
                r.ok = false;
                break;
            }
            const char* entry = b->data + b->module_table + (size_t)index * BUNDLE_MODULE_ENTRY;
            const U64 offset = read_u64(entry);
            const U32 words = read_u32(entry + 8);
            if (words < 5 || offset > b->size || (U64)words * 4 > b->size - offset)
            {
                r.ok = false;
                break;
            }
            std::vector<unsigned int> spirv(words);
            memcpy(spirv.data(), b->data + offset, (size_t)words * 4);
            if (!whole_module(spirv))
            {
                r.ok = false;
                break;
            }
            modules.emplace_back((EShLanguage)lang, std::move(spirv));
        }
        if (!read_reflection(r, modules, program))
        {
            tell_bundle_damage();
            return false;
        }
        return true;
    }

    VkShaderStageFlagBits to_vulkan(EShLanguage stage);

    // Set and binding of every descriptor variable the module uses, added to `out` once each.
    void collect_descriptors(const std::vector<unsigned int>& spirv, std::vector<LLVKShader::Descriptor>& out)
    {
        constexpr U32 OP_ENTRY_POINT = 15, DECORATION_BINDING = 33, DECORATION_DESCRIPTOR_SET = 34;
        Interface io = scan(spirv);
        std::unordered_map<U32, std::pair<S32, S32>> found;     // variable id -> set, binding
        std::unordered_map<U32, bool> used;                     // SPIR-V 1.4+: the entry point lists every global it uses
        for (size_t i = 5; i < spirv.size();)
        {
            U32 count = spirv[i] >> 16;
            U32 op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                break;
            }
            if (op == OP_ENTRY_POINT && count >= 4)
            {
                // execution model, id, null-terminated name, then the interface ids
                size_t w = i + 3;
                while (w < i + count && (spirv[w] & 0xff) && (spirv[w] & 0xff00) && (spirv[w] & 0xff0000) && (spirv[w] & 0xff000000))
                {
                    ++w;
                }
                for (++w; w < i + count; ++w)
                {
                    used[spirv[w]] = true;
                }
            }
            else if (op == OP_DECORATE && count >= 4 && (spirv[i + 2] == DECORATION_BINDING || spirv[i + 2] == DECORATION_DESCRIPTOR_SET))
            {
                auto it = found.emplace(spirv[i + 1], std::make_pair(-1, -1)).first;
                (spirv[i + 2] == DECORATION_BINDING ? it->second.second : it->second.first) = (S32)spirv[i + 3];
            }
            i += count;
        }
        for (const auto& var : found)
        {
            if (var.second.first < 0 || var.second.second < 0 || !used.count(var.first))
            {
                continue;
            }
            LLVKShader::Descriptor d;
            d.name = io.names[var.first];
            d.set = (U32)var.second.first;
            d.binding = (U32)var.second.second;
            bool known = false;
            for (const auto& have : out)
            {
                known |= have.set == d.set && have.binding == d.binding && have.name == d.name;
            }
            if (!known)
            {
                out.push_back(d);
            }
        }
    }

    // The descriptors always; the modules unless a worker only warms the cache.
    bool create_modules(const std::vector<std::pair<EShLanguage, std::vector<unsigned int>>>& modules, LLVKShader::Program& program,
                        LLVKShader::Modules mode)
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        program.descriptors.clear();
        HBXXH64 code_hash;      // Program::code_key (research/160)
        for (const auto& stage_module : modules)
        {
            const std::vector<unsigned int>& spirv = stage_module.second;
            const U32 stage_words[2] = { (U32)stage_module.first, (U32)spirv.size() };
            code_hash.update(stage_words, sizeof(stage_words));
            code_hash.update(spirv.data(), spirv.size() * sizeof(unsigned int));
            collect_descriptors(spirv, program.descriptors);
            if (mode == LLVKShader::Modules::Skip)
            {
                continue;
            }
            VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = spirv.size() * sizeof(unsigned int);
            info.pCode = spirv.data();
            VkShaderModule module = VK_NULL_HANDLE;
            VkResult result = vkCreateShaderModule(device, &info, nullptr, &module);
            if (result != VK_SUCCESS)
            {
                program.log += "vkCreateShaderModule failed: " + std::to_string(result) + "\n";
                LLVKShader::destroy(program);
                return false;
            }
            program.modules.push_back(module);
            program.stages.push_back(to_vulkan(stage_module.first));
        }
        const U64 code_key = code_hash.digest();
        program.code_key = code_key ? code_key : 1;     // 0 is "none"
        return true;
    }

    VkShaderStageFlagBits to_vulkan(EShLanguage stage)
    {
        switch (stage)
        {
        case EShLangVertex:   return VK_SHADER_STAGE_VERTEX_BIT;
        case EShLangFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case EShLangGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        default:              return VK_SHADER_STAGE_COMPUTE_BIT;
        }
    }

    // writeCacheFile's temporary files are "<entry>.charis-tmp.<process>.<thread>.tmp".
    // Only files named so are cleared from the folder: LLRHI_VK_SHADER_CACHE may
    // name a folder other programs keep their own .tmp files in.
    constexpr char TEMP_MARK[] = ".charis-tmp.";

    bool own_temp_file(const std::filesystem::path& file)
    {
        // ours are ASCII; another program's name may not convert to a narrow string
        std::string name;
        for (const wchar_t c : file.filename().wstring())
        {
            if (c == 0 || c >= 128)
            {
                return false;
            }
            name += (char)c;
        }
        const size_t mark = name.find(TEMP_MARK);
        const size_t ids = mark + sizeof(TEMP_MARK) - 1;
        if (mark == std::string::npos || mark == 0 || name.size() < ids + 4 || name.compare(name.size() - 4, 4, ".tmp") != 0)
        {
            return false;
        }
        // "<process>.<thread>": digits, one dot between them
        const std::string numbers = name.substr(ids, name.size() - 4 - ids);
        const size_t dot = numbers.find('.');
        return dot != std::string::npos && dot > 0 && dot + 1 < numbers.size() && numbers.find('.', dot + 1) == std::string::npos &&
               numbers.find_first_not_of("0123456789.") == std::string::npos;
    }
}

void LLVKShader::init()
{
    // Once per process, whichever thread builds first. glslang keeps its
    // working memory per thread after this, so builds may run side by side.
    std::call_once(sInitOnce, []()
    {
        glslang::InitializeProcess();
        sInitialized = true;
    });
}

namespace
{
    // fix-logcache: the shader cache folder's upkeep. Before it nothing but
    // old .tmp files was ever removed: every CACHE_VERSION or GL_VIEW_VERSION
    // bump, and every update that changed a shader's text, left its files
    // behind for good (430 MB measured). Now, once a start, on a background
    // thread a while after the start (TRIM_DELAY: the start's own reads go
    // first): .vkp and .glv files no start has read for UNUSED_FOR (mark_used)
    // are removed, then, while the rest are over TRIM_LIMIT bytes, the
    // longest unused ones, never one used within KEEP_USED (this session's
    // and the last ones'). Only files named as this cache names them (16 hex
    // digits, .vkp or .glv): nothing else in the folder (pipelines.vkpc,
    // pipeline_keys.bin, oit_variants.txt, .tmp files; in a folder given by
    // LLRHI_VK_SHADER_CACHE, the user's own files) is touched. A file another viewer is reading or
    // writing just stays (the removal fails), and one removed under a reader
    // is a miss: it is built again and written again.
    constexpr auto TRIM_DELAY = std::chrono::seconds(120);
    constexpr auto UNUSED_FOR = std::chrono::hours(24 * 30);
    constexpr auto KEEP_USED = std::chrono::hours(48);     // > TOUCH_AFTER: a file read now is never older
    constexpr uintmax_t TRIM_LIMIT = 256ull << 20;

    bool is_cache_entry(const std::filesystem::path& file)
    {
        const std::filesystem::path ext = file.extension();
        if (ext != ".vkp" && ext != ".glv")
        {
            return false;
        }
        const std::wstring stem = file.stem().wstring();
        if (stem.size() != 16)
        {
            return false;
        }
        for (wchar_t c : stem)
        {
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
            {
                return false;
            }
        }
        return true;
    }

    // Its log line, or empty.
    std::string trim_cache(const std::string& dir)
    {
        struct Entry
        {
            std::filesystem::path path;
            std::filesystem::file_time_type written;
            uintmax_t size = 0;
        };
        std::vector<Entry> entries;
        std::error_code list_ec;
        for (std::filesystem::directory_iterator it(fsyspath(dir), list_ec), end; !list_ec && it != end; it.increment(list_ec))
        {
            std::error_code fec;
            if (!is_cache_entry(it->path()) || !it->is_regular_file(fec))
            {
                continue;
            }
            Entry entry;
            entry.path = it->path();
            entry.written = it->last_write_time(fec);
            if (fec)
            {
                continue;
            }
            entry.size = it->file_size(fec);
            if (fec)
            {
                continue;
            }
            entries.push_back(std::move(entry));
        }
        if (list_ec)
        {
            return "shader cache upkeep: " + dir + " could not be listed (" + list_ec.message() + ")";
        }

        // Longest unused first.
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.written < b.written; });
        uintmax_t total = 0;
        for (const Entry& entry : entries)
        {
            total += entry.size;
        }
        const auto now = std::filesystem::file_time_type::clock::now();
        U32 removed = 0;
        uintmax_t removed_bytes = 0;
        for (const Entry& entry : entries)
        {
            const auto unused = now - entry.written;
            if (unused <= KEEP_USED || (unused <= UNUSED_FOR && total <= TRIM_LIMIT))
            {
                break;      // sorted: every later file was used as recently or more
            }
            std::error_code fec;
            if (std::filesystem::remove(entry.path, fec))
            {
                ++removed;
                removed_bytes += entry.size;
                total -= entry.size;
            }
        }
        if (!removed)
        {
            return std::string();
        }
        std::ostringstream line;
        line << "shader cache upkeep: removed " << removed << " files (" << (removed_bytes >> 20) << " MB) unused for 30 days or over the "
             << (TRIM_LIMIT >> 20) << " MB limit; " << entries.size() - removed << " files (" << (total >> 20) << " MB) kept";
        return line.str();
    }

#if LL_WINDOWS
    // Builds before fix-cachepath kept the cache in %LOCALAPPDATA%\SecondLife,
    // the official viewer's folder, which this viewer does not clear (research/114):
    // one line a start says it may be deleted, while it is there.
    // Its log line, or empty.
    std::string note_old_cache(const std::string& dir)
    {
        const std::string local = LLStringUtil::getenv("LOCALAPPDATA");
        if (local.empty())
        {
            return std::string();
        }
        const std::string old = local + "\\SecondLife\\vk_shader_cache";
        std::error_code ec;
        const std::filesystem::path old_path = fsyspath(old);
        if (!std::filesystem::is_directory(old_path, ec) || std::filesystem::equivalent(old_path, fsyspath(dir), ec))
        {
            return std::string();
        }
        uintmax_t bytes = 0;
        U32 files = 0;
        std::error_code list_ec;
        for (std::filesystem::directory_iterator it(old_path, list_ec), end; !list_ec && it != end; it.increment(list_ec))
        {
            std::error_code fec;
            if (it->is_regular_file(fec))
            {
                const uintmax_t size = it->file_size(fec);
                bytes += fec ? 0 : size;
                ++files;
            }
        }
        std::ostringstream line;
        line << "the shader cache of earlier builds, " << old << " (" << files << " files, " << (bytes >> 20)
             << " MB), is no longer used: it may be deleted";
        return line.str();
    }
#endif

    void start_cache_upkeep(const std::string& dir)
    {
        std::thread([dir]()
        {
#if LL_WINDOWS
            // Low CPU and disk priority: the upkeep never matters for a frame.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
            std::this_thread::sleep_for(TRIM_DELAY);
            // Detached, so it may still run while the viewer quits; once the
            // static objects go (the log's among them) it must touch none.
            // The viewer stops "running" when the shutdown begins, long before
            // that, and never runs again (quitting or error, then stopped by
            // ~LLApp(): isExiting() would be false again then): an upkeep
            // that finds it not running does nothing more, and its lines are
            // dropped (fix-round5). Only std::filesystem and its own data are
            // used in between.
            if (!LLApp::isRunning())
            {
                return;
            }
            std::vector<std::string> lines = { trim_cache(dir) };
#if LL_WINDOWS
            lines.push_back(note_old_cache(dir));
            // Normal priority again before the log's lock is taken.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#endif
            if (!LLApp::isRunning())
            {
                return;
            }
            for (const std::string& line : lines)
            {
                if (!line.empty())
                {
                    LL_INFOS("VKShader") << line << LL_ENDL;
                }
            }
        }).detach();
    }
}

const std::string& LLVKShader::cacheDir()
{
    static const std::string dir = []()
    {
        // UTF-8 throughout, as LLFile and llifstream take it: std::getenv
        // would give the bytes of the ANSI code page (CP932 for a Japanese
        // user name), which are not UTF-8.
        std::string path = LLStringUtil::getenv("LLRHI_VK_SHADER_CACHE");
        if (path.empty())
        {
            // The viewer's cache folder is set (initCache) before the window
            // is made, so before the first call here (LLVKDevice::init).
            // "Clear cache" deletes only the files at its top: ours stay.
            const std::string cache = gDirUtilp->getCacheDir();
            if (cache.empty())
            {
                return path;
            }
            path = cache + gDirUtilp->getDirDelimiter() + "vk_shader_cache";
        }
        std::error_code ec;
        std::filesystem::create_directories(fsyspath(path), ec);
        if (!LLFile::isdir(path))
        {
            LL_WARNS("VKShader") << "the shader cache folder " << path << " cannot be made (" << ec.message()
                                 << "): no disk cache this session" << LL_ENDL;
            return std::string();
        }
        // Temporary files (writeCacheFile) of a viewer that exited or died
        // while writing: an hour old, so never one another viewer writes now.
        // Ours only (own_temp_file), whatever else the folder holds.
        const auto now = std::filesystem::file_time_type::clock::now();
        std::error_code list_ec;
        for (std::filesystem::directory_iterator it(fsyspath(path), list_ec), end; !list_ec && it != end; it.increment(list_ec))
        {
            std::error_code fec;
            if (it->path().extension() != ".tmp" || !own_temp_file(it->path()) || !it->is_regular_file(fec))
            {
                continue;
            }
            const auto written = it->last_write_time(fec);
            if (!fec && now - written > std::chrono::hours(1))
            {
                std::filesystem::remove(it->path(), fec);
            }
        }
        LL_INFOS("VKShader") << "Shader cache folder: " << path << LL_ENDL;
        start_cache_upkeep(path);
        return path;
    }();
    return dir;
}

bool LLVKShader::readCacheFile(const std::string& path, std::string& data)
{
    data.clear();
    if (path.empty())
    {
        return false;
    }
    llifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return false;
    }
    // Straight into `data` at its size (fix-logcache: a stream buffer grew
    // by steps and its copies held pipelines.vkpc, tens of MB, more than once).
    const std::streamoff size = file.tellg();
    if (size < 0)
    {
        return false;
    }
    data.resize((size_t)size);
    file.seekg(0, std::ios::beg);
    if (size && !file.read(data.data(), (std::streamsize)size))
    {
        data.clear();
        return false;   // cut short while read (or a read error): as missing
    }
    return true;
}

bool LLVKShader::writeCacheFile(const std::string& path, std::initializer_list<CacheBytes> parts)
{
    if (path.empty())
    {
        return false;
    }
    std::ostringstream temp_name;
    temp_name << path << TEMP_MARK;     // own_temp_file() knows the name
#if LL_WINDOWS
    temp_name << GetCurrentProcessId() << '.';
#else
    temp_name << "0.";
#endif
    temp_name << std::this_thread::get_id() << ".tmp";
    const std::string temp = temp_name.str();
    bool written = false;
    {
        llofstream file(temp, std::ios::binary | std::ios::trunc);
        for (const CacheBytes& part : parts)
        {
            file.write((const char*)part.data, (std::streamsize)part.size);
        }
        file.close();
        written = !file.fail();
    }
    // Not renamed when the write failed (a full disk): the old file stays whole.
    if (!written || LLFile::rename(temp, path) != 0)
    {
        LLFile::remove(temp, ENOENT);
        return false;
    }
    return true;
}

void LLVKShader::appendCacheTrailer(std::string& data)
{
    put_trailer(data);
}

bool LLVKShader::stripCacheTrailer(std::string& data)
{
    return strip_trailer(data);
}

void LLVKShader::cleanup()
{
    {
        std::lock_guard<std::mutex> lock(sKeylogMutex);
        if (sKeylog)
        {
            fclose(sKeylog);
            sKeylog = nullptr;  // a later build writes nothing
        }
    }
    if (sLiveWorkers.load() != 0)
    {
        LL_WARNS("VKShader") << "compile workers still running: glslang stays initialized" << LL_ENDL;
        return;
    }
    if (sInitialized.exchange(false))
    {
        glslang::FinalizeProcess();
    }
}

LLVKShader::Stats LLVKShader::takeStats()
{
    std::lock_guard<std::mutex> lock(sStatsMutex);
    Stats out = sStats;
    if (out.builds)
    {
        out.span_ms = ms_since(sStatsFirst);
    }
    sStats = Stats();
    return out;
}

LLVKShader::Stats LLVKShader::takeThreadStats()
{
    Stats out = tStats;
    tStats = Stats();
    return out;
}

namespace
{
    struct BakeStep
    {
        const char* name;
        bool (*step)();
    };

    // Filled at install (main thread), read by the bake: no lock.
    std::vector<BakeStep>& bake_steps()
    {
        static std::vector<BakeStep> steps;
        return steps;
    }
}

void LLVKShader::addBakeStep(const char* name, bool (*step)())
{
    bake_steps().push_back({ name, step });
}

U32 LLVKShader::runBakeSteps(U32& failed)
{
    failed = 0;
    for (const BakeStep& step : bake_steps())
    {
        if (!step.step())
        {
            LL_WARNS("VKShader") << "Shader bake: the " << step.name << " pass could not be set up" << LL_ENDL;
            ++failed;
        }
    }
    return (U32)bake_steps().size();
}

U32 LLVKShader::compileThreads()
{
    static const U32 threads = []()
    {
        U64 wanted = 0;
        LLRHI::envNumber("LLRHI_SHADER_COMPILE_THREADS", 0, U32_MAX, wanted);
        if (wanted > 0)
        {
            return (U32)wanted;
        }
        // The main thread and the recorder keep a core each. At most 10
        // (fix-round107): each thread holds a glslang compile's memory, and
        // a reload of some 250 programs gains little from more (an 8700K's
        // 12 logical cores give 10, as measured in research/111 B-SC).
        const U32 cores = std::thread::hardware_concurrency();
        return cores > 3 ? llmin(cores - 2, 10u) : 1u;
    }();
    return threads;
}

namespace
{
    // ------------------------------------------------------------------
    // GL's "active" (research/117 7.4): what a module still reads once dead
    // code is gone, as a GL driver counts uniforms and attributes.

    // What a module reads in its function bodies: variables by name
    // (samplers, vertex inputs) and the members of the default uniform block
    // by member name (all of them when the block is read whole).
    struct LiveNames
    {
        std::unordered_set<std::string> variables;
        std::unordered_set<std::string> default_members;
        bool all_default = false;
    };

    void read_names(const std::vector<unsigned int>& spirv, LiveNames& live)
    {
        constexpr U32 OP_MEMBER_NAME = 6, OP_EXT_INST = 12, OP_LINE = 8, OP_NO_LINE = 317, OP_TYPE_POINTER = 32,
                      OP_CONSTANT = 43, OP_FUNCTION = 54, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
                      OP_PTR_ACCESS_CHAIN = 67;
        std::unordered_map<U32, std::string> names;                 // id -> OpName
        std::map<std::pair<U32, U32>, std::string> member_names;    // (struct type, member) -> OpMemberName
        std::unordered_map<U32, U32> pointee;                       // pointer type -> pointee type
        std::unordered_map<U32, U32> constants;                     // constant id -> its first word
        std::unordered_map<U32, U32> variables;                     // variable id -> pointer type
        for (size_t i = 5; i < spirv.size();)
        {
            const U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                return;
            }
            if (op == OP_FUNCTION)
            {
                break;  // declarations end here
            }
            if (op == OP_NAME && count > 2)
            {
                names[spirv[i + 1]] = spirv_string(spirv, i + 2, i + count);
            }
            else if (op == OP_MEMBER_NAME && count > 3)
            {
                member_names[{ spirv[i + 1], spirv[i + 2] }] = spirv_string(spirv, i + 3, i + count);
            }
            else if (op == OP_TYPE_POINTER && count >= 4)
            {
                pointee[spirv[i + 1]] = spirv[i + 3];
            }
            else if (op == OP_CONSTANT && count >= 4)
            {
                constants[spirv[i + 2]] = spirv[i + 3];
            }
            else if (op == OP_VARIABLE && count >= 4)
            {
                variables[spirv[i + 2]] = spirv[i + 1];
            }
            i += count;
        }
        U32 default_block = 0, default_type = 0;
        for (const auto& variable : variables)
        {
            const U32 type = pointee[variable.second];
            auto name = names.find(type);
            if (name != names.end() && name->second == "gl_DefaultUniformBlock")
            {
                default_block = variable.first;
                default_type = type;
            }
        }
        bool in_function = false;
        for (size_t i = 5; i < spirv.size();)
        {
            const U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                return;
            }
            in_function |= op == OP_FUNCTION;
            if (in_function && op != OP_LINE && op != OP_NO_LINE)
            {
                size_t first = op == OP_EXT_INST ? i + 5 : i + 1;
                const bool chain = op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN;
                if (chain && count >= 5 && default_block && spirv[i + 3] == default_block)
                {
                    // result type, result, base, first index (the member): the member is read.
                    auto index = constants.find(spirv[i + 4]);
                    auto member = index != constants.end() ? member_names.find({ default_type, index->second }) : member_names.end();
                    if (member != member_names.end())
                    {
                        live.default_members.insert(member->second);
                    }
                    else
                    {
                        live.all_default = true;
                    }
                    first = i + 4;
                }
                // Any word equal to a variable's id counts (a literal may match
                // too: that only keeps a variable, as drop_unread_descriptors).
                for (size_t w = first; w < i + count; ++w)
                {
                    auto variable = variables.find(spirv[w]);
                    if (variable == variables.end())
                    {
                        continue;
                    }
                    if (variable->first == default_block)
                    {
                        live.all_default = true;
                    }
                    else
                    {
                        auto name = names.find(variable->first);
                        if (name != names.end())
                        {
                            live.variables.insert(name->second);
                        }
                    }
                }
            }
            i += count;
        }
    }

    // A GL linker drops what one stage writes and the next does not read
    // (a varying the fragment shader never uses), and with it what fed it:
    // a vertex input passed on to nothing is not an active attribute
    // (research/117 8.2). The stores to the stage's outputs (by name, as GL
    // matches varyings) that next_reads does not have go, so the dead code
    // elimination after it takes the rest. Built-ins (gl_Position, the
    // gl_PerVertex block: unnamed or "gl_") stay.
    void drop_unread_output_stores(std::vector<unsigned int>& spirv, const std::unordered_set<std::string>& next_reads)
    {
        constexpr U32 OP_FUNCTION = 54, OP_STORE = 62, OP_COPY_MEMORY = 63, OP_ACCESS_CHAIN = 65,
                      OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67, DECORATION_BUILT_IN = 11;
        Interface io = scan(spirv);
        std::unordered_set<U32> built_ins;
        for (size_t i = 5; i < spirv.size();)
        {
            const U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            if (!count || i + count > spirv.size())
            {
                return;
            }
            if (op == OP_DECORATE && count >= 3 && spirv[i + 2] == DECORATION_BUILT_IN)
            {
                built_ins.insert(spirv[i + 1]);
            }
            i += count;
        }
        std::unordered_set<U32> dead;      // unread outputs, and pointers into them
        for (const auto& variable : io.storage)
        {
            if (variable.second != STORAGE_OUTPUT || built_ins.count(variable.first))
            {
                continue;
            }
            auto name = io.names.find(variable.first);
            if (name == io.names.end() || name->second.empty() || name->second.compare(0, 3, "gl_") == 0 ||
                next_reads.count(name->second))
            {
                continue;
            }
            dead.insert(variable.first);
        }
        // An output the stage reads back (gl_Position = vertex_position after
        // vertex_position = ..., Draw Color: research/117 8.3) keeps its
        // stores: taking them out would cut what the read feeds. Any use of
        // the output, or of a pointer into it, other than as the target of a
        // store or the base of an access chain counts as a read.
        {
            constexpr U32 OP_LINE = 8, OP_NO_LINE = 317, OP_EXT_INST = 12;
            std::unordered_map<U32, U32> root;     // pointer id -> the unread output it points into
            for (U32 variable : dead)
            {
                root[variable] = variable;
            }
            bool body = false;
            for (size_t i = 5; i < spirv.size();)
            {
                const U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
                if (!count || i + count > spirv.size())
                {
                    return;
                }
                body |= op == OP_FUNCTION;
                if (body && op != OP_LINE && op != OP_NO_LINE)
                {
                    const bool chain = op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN;
                    if (chain && count >= 4)
                    {
                        auto base = root.find(spirv[i + 3]);
                        if (base != root.end())
                        {
                            root[spirv[i + 2]] = base->second;
                        }
                    }
                    for (size_t w = op == OP_EXT_INST ? i + 5 : i + 1; w < i + count; ++w)
                    {
                        auto it = root.find(spirv[w]);
                        if (it == root.end())
                        {
                            continue;
                        }
                        const bool written = (op == OP_STORE || op == OP_COPY_MEMORY) && w == i + 1;
                        const bool chained = chain && (w == i + 2 || w == i + 3);
                        if (!written && !chained)
                        {
                            dead.erase(it->second);     // read back: kept whole
                        }
                    }
                }
                i += count;
            }
        }
        if (dead.empty())
        {
            return;
        }
        std::vector<unsigned int> out(spirv.begin(), spirv.begin() + 5);
        out.reserve(spirv.size());
        bool in_function = false;
        for (size_t i = 5; i < spirv.size();)
        {
            const U32 count = spirv[i] >> 16, op = spirv[i] & 0xffff;
            in_function |= op == OP_FUNCTION;
            bool keep = true;
            if (in_function)
            {
                if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN) && count >= 4 &&
                    dead.count(spirv[i + 3]))
                {
                    dead.insert(spirv[i + 2]);  // a pointer into an unread output (the chain itself goes with its stores)
                }
                else if ((op == OP_STORE || op == OP_COPY_MEMORY) && count >= 3 && dead.count(spirv[i + 1]))
                {
                    keep = false;
                }
            }
            if (keep)
            {
                out.insert(out.end(), spirv.begin() + i, spirv.begin() + i + count);
            }
            i += count;
        }
        spirv.swap(out);
    }

    // The stage's module after SPIRV-Tools' performance passes (inlining,
    // propagation, dead code elimination), read as above. next_reads: what
    // the next stage reads (null for the last stage). False: the optimizer
    // refused it (then nothing is dropped).
    bool read_names_optimized(const glslang::TIntermediate& intermediate, const std::unordered_set<std::string>* next_reads,
                              LiveNames& live)
    {
        std::vector<unsigned int> spirv;
        glslang::SpvOptions options;
        options.validate = false;
        glslang::GlslangToSpv(intermediate, spirv, &options);
        if (next_reads)
        {
            drop_unread_output_stores(spirv, *next_reads);
        }
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
        optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
        optimizer.RegisterPerformancePasses();
        spvtools::OptimizerOptions optimizer_options;
        optimizer_options.set_run_validator(false);
        std::vector<uint32_t> optimized;
        if (!optimizer.Run(spirv.data(), spirv.size(), &optimized, optimizer_options))
        {
            return false;
        }
        read_names(std::vector<unsigned int>(optimized.begin(), optimized.end()), live);
        return true;
    }

    // "name[3]", "name.member", "name[0].member" -> "name"
    std::string top_name(const std::string& name)
    {
        const size_t end = name.find_first_of("[.");
        return end == std::string::npos ? name : name.substr(0, end);
    }

    // A stage's compilation unit with the settings every build uses: GLSL as
    // the viewer writes it (loose uniforms gathered into the default uniform
    // block by glslang's relaxed Vulkan rules), auto-mapped bindings and
    // locations. resource_set: see build(). glslang keeps the pointer `text`
    // (not the string): it must stay valid until parse().
    std::unique_ptr<glslang::TShader> make_shader(LLVKShader::Stage source_stage, const char* const* text, U32 resource_set)
    {
        EShLanguage stage = to_glslang(source_stage);
        auto shader = std::make_unique<glslang::TShader>(stage);
        shader->setStrings(text, 1);
        shader->setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
        shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
        shader->setEnvInputVulkanRulesRelaxed();
        shader->setAutoMapBindings(true);
        // Keep the namespaces apart: auto-mapping numbers samplers and uniform
        // blocks independently, so without a shift both start at binding 0.
        shader->setShiftBinding(glslang::EResUbo, LLVKShader::UNIFORM_BLOCK_BINDING_SHIFT);
        shader->setGlobalUniformBinding(63);
        shader->setAutoMapLocations(true);
        if (resource_set)
        {
            shader->setResourceSetBinding({ std::to_string(resource_set) });
            shader->setGlobalUniformSet(resource_set);
        }
        return shader;
    }
}

bool LLVKShader::build(const std::vector<Source>& sources, Program& program, U32 resource_set, Modules modules_wanted)
{
    init();

    const Clock::time_point start = Clock::now();
    const U64 key = source_hash(sources, resource_set);
    const std::string cache = cache_path(key);
    bool waited = false;
    F64 wait_ms = 0.0;
    auto count = [&](From from)
    {
        const bool compiled = from == From::Compiled || from == From::Failed;
        const F64 ms = ms_since(start);
        Stats one;
        one.builds = 1;
        one.cache_hits = from == From::Cache ? 1 : 0;
        one.bundle_hits = from == From::Bundle ? 1 : 0;
        one.compiled = compiled ? 1 : 0;
        one.waited = waited ? 1 : 0;
        one.wait_ms = wait_ms;
        one.compile_ms = compiled ? ms - wait_ms : 0.0;
        add_stats(tStats, one);
        if (!tBackgroundJob)
        {
            std::lock_guard<std::mutex> lock(sStatsMutex);
            if (!sStats.builds || start < sStatsFirst)
            {
                sStatsFirst = start;    // the earliest start among the builds counted
            }
            add_stats(sStats, one);
        }
        keylog(key, from, waited, ms, sources, resource_set);
    };

    // Modules read from the cache or the bundle: made, or false (then a
    // fresh compile follows).
    auto use = [&](std::vector<std::pair<EShLanguage, std::vector<unsigned int>>>& cached, From from)
    {
        // Modules cached before the function-body rule may still list an
        // unread descriptor; dropping again is cheap and changes nothing else.
        for (auto& cached_module : cached)
        {
            drop_unread_descriptors(cached_module.second);
        }
        if (create_modules(cached, program, modules_wanted))
        {
            program.key = key;
            count(from);
            return true;
        }
        program = Program();
        return false;
    };

    // From the cache, else from the bundle (research/159 section 4); else
    // claim the sources (or wait for the thread that has, then read its
    // result).
    bool bundle_tried = false;
    std::optional<InFlightClaim> claim;     // destroyed after every lock below is let go
    while (!claim)
    {
        std::vector<std::pair<EShLanguage, std::vector<unsigned int>>> cached;
        if (load_cache(cache, cached, program) && use(cached, From::Cache))
        {
            return true;
        }
        if (!bundle_tried)
        {
            // Once: had it been there, no other thread would compile it.
            bundle_tried = true;
            cached.clear();
            if (load_bundle(key, cached, program) && use(cached, From::Bundle))
            {
                return true;
            }
        }
        std::unique_lock<std::mutex> lock(sInFlightMutex);
        if (sInFlight.count(key) && !cache.empty())
        {
            const Clock::time_point wait_start = Clock::now();
            sInFlightDone.wait(lock, [key]() { return sInFlight.count(key) == 0; });
            wait_ms += ms_since(wait_start);
            waited = true;
            continue;   // its result is in the cache now (unless it failed)
        }
        sInFlight.insert(key);
        claim.emplace(key);
    }

    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    glslang::TProgram linker;

    for (const Source& source : sources)
    {
        const char* text = source.text.c_str();
        std::unique_ptr<glslang::TShader> shader = make_shader(source.stage, &text, resource_set);

        if (!shader->parse(GetDefaultResources(), 100, false, messages))
        {
            program.log += shader->getInfoLog();
            count(From::Failed);
            return false;
        }
        linker.addShader(shader.get());
        shaders.push_back(std::move(shader));
    }

    if (!linker.link(messages) || !linker.mapIO())
    {
        program.log += linker.getInfoLog();
        count(From::Failed);
        return false;
    }

    std::vector<std::pair<EShLanguage, std::vector<unsigned int>>> modules;
    for (int lang = EShLangVertex; lang <= EShLangCompute; ++lang)
    {
        glslang::TIntermediate* intermediate = linker.getIntermediate((EShLanguage)lang);
        if (!intermediate)
        {
            continue;
        }
        std::vector<unsigned int> spirv;
        glslang::SpvOptions options;
        options.validate = false;
        glslang::GlslangToSpv(*intermediate, spirv, &options);
        drop_unread_descriptors(spirv);
        if (resource_set)
        {
            move_to_set(spirv, resource_set);
        }
        modules.emplace_back((EShLanguage)lang, std::move(spirv));
    }

    // GL matches varyings between stages by name; auto-mapped locations follow
    // declaration order per stage, so re-point each consumer input at the
    // location its producer gave the same name.
    for (size_t i = 1; i < modules.size(); ++i)
    {
        match_varyings(modules[i - 1].second, modules[i].second);
    }

    if (!create_modules(modules, program, modules_wanted))
    {
        count(From::Failed);
        return false;
    }
    program.key = key;

    // Name -> offset/binding table, so LLGLSLShader's by-name uniform calls can
    // be served from a uniform buffer without touching any caller.
    linker.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers | EShReflectionAllBlockVariables);
    for (int i = 0; i < linker.getNumUniformVariables(); ++i)
    {
        const glslang::TObjectReflection& reflected = linker.getUniform(i);
        Uniform uniform;
        uniform.name = reflected.name;
        uniform.gl_type = (U32)reflected.glDefineType;
        uniform.offset = reflected.offset;
        uniform.binding = reflected.getBinding();
        uniform.size = reflected.size;
        uniform.array_stride = reflected.arrayStride;
        uniform.block = reflected.index;
        uniform.stages = (U32)reflected.stages;
        program.uniforms.push_back(uniform);
    }
    for (int i = 0; i < linker.getNumUniformBlocks(); ++i)
    {
        const glslang::TObjectReflection& reflected = linker.getUniformBlock(i);
        program.blocks.push_back({ reflected.name, reflected.getBinding(), reflected.size });
    }
    // Vertex inputs straight from the SPIR-V: every declared input must be fed
    // (reflection lists only the live ones), with the shader's numeric type.
    for (const auto& stage_module : modules)
    {
        if (stage_module.first == EShLangVertex)
        {
            program.inputs = vertex_inputs(stage_module.second);
        }
    }
    save_cache(cache, modules, program);
    count(From::Compiled);

    // LLRHI_VK_DUMP_SPIRV=<dir>: final modules for offline spirv-val.
    static const std::string dump = LLStringUtil::getenv("LLRHI_VK_DUMP_SPIRV");     // UTF-8, as llofstream takes it
    if (!dump.empty())
    {
        static std::atomic<U32> dumps{ 0 };
        const U32 counter = ++dumps;
        for (const auto& m : modules)
        {
            llofstream file(dump + "/p" + std::to_string(counter) + "_" + std::to_string((int)m.first) + ".spv", std::ios::binary);
            file.write((const char*)m.second.data(), m.second.size() * 4);
        }
    }
    return true;
}

namespace
{
    // reflectGL without its cache. front_ms: parse, link and glslang's
    // reflection; optimize_ms: each stage's SPIR-V and dead code elimination.
    bool reflect_gl(const std::vector<LLVKShader::Source>& sources, LLVKShader::GLReflection& out, F64& front_ms, F64& optimize_ms)
    {
        out = LLVKShader::GLReflection();
        const Clock::time_point front_start = Clock::now();
        if (sources.empty())
        {
            out.log = "no shaders attached";
            return false;
        }
        const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
        std::vector<std::unique_ptr<glslang::TShader>> shaders;
        glslang::TProgram linker;
        for (const LLVKShader::Source& source : sources)
        {
            const char* text = source.text.c_str();
            std::unique_ptr<glslang::TShader> shader = make_shader(source.stage, &text, 1);
            if (!shader->parse(GetDefaultResources(), 100, false, messages))
            {
                out.log += shader->getInfoLog();
                return false;
            }
            linker.addShader(shader.get());
            shaders.push_back(std::move(shader));
        }
        if (!linker.link(messages))
        {
            out.log += linker.getInfoLog();
            return false;
        }
        // Every member of a block is listed (GL lists the members of an active
        // std140 block); of the default block only the read ones (GL drops loose
        // uniforms the program never reads).
        linker.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers | EShReflectionAllBlockVariables);
        S32 default_block = -1;
        for (int i = 0; i < linker.getNumUniformBlocks(); ++i)
        {
            const glslang::TObjectReflection& block = linker.getUniformBlock(i);
            if (block.name == "gl_DefaultUniformBlock")
            {
                default_block = i;
            }
            else
            {
                out.blocks.push_back(block.name);
            }
        }
        front_ms += ms_since(front_start);
        const Clock::time_point optimize_start = Clock::now();
        // glslang's reflection counts what any reachable code mentions; a GL
        // driver first drops what cannot change the program's outputs (atmosphere
        // values computed and not used, a sampler read on a path that is cut
        // off, a vertex input passed on to nothing). Read each stage again after
        // dead code elimination: the default block's members, the samplers and
        // the vertex inputs it still reads are the active ones. Named blocks stay
        // as reflected (the Vulkan module reads them unoptimized: a block GL
        // would call inactive must still be bound).
        // From the last stage back, so each stage knows what the next one reads.
        LiveNames live;
        bool optimized = true;
        LiveNames vertex_live;
        std::unordered_set<std::string> next_reads;
        bool have_next = false;
        for (int lang = EShLangCompute; lang >= EShLangVertex && optimized; --lang)
        {
            const glslang::TIntermediate* intermediate = linker.getIntermediate((EShLanguage)lang);
            if (!intermediate)
            {
                continue;
            }
            LiveNames stage;
            optimized = read_names_optimized(*intermediate, have_next ? &next_reads : nullptr, stage);
            next_reads = stage.variables;   // its inputs among them, by name
            have_next = true;
            LiveNames& into = lang == EShLangVertex ? vertex_live : live;
            into.variables.insert(stage.variables.begin(), stage.variables.end());
            into.default_members.insert(stage.default_members.begin(), stage.default_members.end());
            into.all_default |= stage.all_default;
        }
        optimize_ms += ms_since(optimize_start);
        if (optimized)
        {
            live.variables.insert(vertex_live.variables.begin(), vertex_live.variables.end());
            live.default_members.insert(vertex_live.default_members.begin(), vertex_live.default_members.end());
            live.all_default |= vertex_live.all_default;
        }
        else
        {
            out.log += "the optimizer refused a stage: activity as glslang reflects it\n";
        }
        for (int i = 0; i < linker.getNumUniformVariables(); ++i)
        {
            const glslang::TObjectReflection& reflected = linker.getUniform(i);
            const bool in_default = reflected.index == default_block && default_block >= 0;
            if (in_default && !reflected.stages)
            {
                continue;   // listed for AllBlockVariables, not read (samplers are listed only when read)
            }
            if (optimized && in_default && !live.all_default && !live.default_members.count(top_name(reflected.name)))
            {
                continue;   // read only by dead code
            }
            if (optimized && reflected.index < 0 && !live.variables.count(top_name(reflected.name)))
            {
                continue;   // a sampler read only by dead code
            }
            LLVKShader::GLUniform uniform;
            uniform.name = reflected.name;
            uniform.gl_type = (U32)reflected.glDefineType;
            uniform.size = llmax(1, reflected.size);
            uniform.in_block = reflected.index >= 0 && !in_default;
            out.uniforms.push_back(uniform);
        }
        // In name order, as NVIDIA's GL driver lists active uniforms (the order
        // gives the texture channels, LLGLSLShader::mapUniforms).
        std::stable_sort(out.uniforms.begin(), out.uniforms.end(),
                         [](const LLVKShader::GLUniform& a, const LLVKShader::GLUniform& b) { return a.name < b.name; });
        for (int i = 0; i < linker.getNumPipeInputs(); ++i)
        {
            const std::string& name = linker.getPipeInput(i).name;
            if (name.compare(0, 3, "gl_") != 0 && (!optimized || vertex_live.variables.count(name)))
            {
                out.inputs.push_back(name);
            }
        }
        out.linked = true;
        return true;
    }

    // ------------------------------------------------------------------
    // GL's view on disk (research/117 8.6): a program's reflectGL result,
    // next to the SPIR-V cache and keyed the same way (the exact sources),
    // so a start with the sources of an earlier one makes no glslang link
    // and no optimizer run for it. A GL view depends on the sources, on this
    // code (GL_VIEW_VERSION) and on glslang and SPIRV-Tools (the SDK's
    // glslang version), not on the GPU or its driver.

    // (GL_VIEW_MAGIC, GL_VIEW_VERSION and GL_VIEW_GLSLANG are with the cache's constants above.)

    // The .glv file's name, and its key in the shader bundle.
    U64 gl_view_key(const std::vector<LLVKShader::Source>& sources)
    {
        U64 hash = source_hash(sources, 0);
        const U32 salt[] = { GL_VIEW_MAGIC, GL_VIEW_VERSION, GL_VIEW_GLSLANG };
        return LLRHI::fnv1a(hash, salt, sizeof(salt));
    }

    std::string gl_view_path(U64 key)
    {
        const std::string& dir = LLVKShader::cacheDir();
        if (dir.empty())
        {
            return std::string();
        }
        char name[32];
        snprintf(name, sizeof(name), "/%016llx.glv", (unsigned long long)key);
        return dir + name;
    }

    void save_gl_view(const std::string& path, const LLVKShader::GLReflection& view)
    {
        if (path.empty())
        {
            return;
        }
        std::string out;
        put_u32(out, GL_VIEW_MAGIC);
        put_u32(out, GL_VIEW_VERSION);
        put_u32(out, GL_VIEW_GLSLANG);
        put_u32(out, view.linked ? 1 : 0);
        put_str(out, view.log);
        put_u32(out, (U32)view.uniforms.size());
        for (const LLVKShader::GLUniform& u : view.uniforms)
        {
            put_str(out, u.name); put_u32(out, u.gl_type); put_s32(out, u.size); put_u32(out, u.in_block ? 1 : 0);
        }
        put_u32(out, (U32)view.blocks.size());
        for (const std::string& block : view.blocks)
        {
            put_str(out, block);
        }
        put_u32(out, (U32)view.inputs.size());
        for (const std::string& input : view.inputs)
        {
            put_str(out, input);
        }
        write_atomically(path, out);
    }

    // A GL view as a .glv file holds it (without its trailer): from the file
    // or from the shader bundle. False, with `view` as it was, when damaged.
    bool parse_gl_view(const char* bytes, size_t size, LLVKShader::GLReflection& view)
    {
        Reader r(bytes, size);
        if (r.u32() != GL_VIEW_MAGIC || r.u32() != GL_VIEW_VERSION || r.u32() != GL_VIEW_GLSLANG)
        {
            return false;
        }
        LLVKShader::GLReflection loaded;
        loaded.linked = r.u32() != 0;
        loaded.log = r.str();
        U32 count = r.u32();
        for (U32 i = 0; i < count && r.ok; ++i)
        {
            LLVKShader::GLUniform u;
            u.name = r.str(); u.gl_type = r.u32(); u.size = r.s32(); u.in_block = r.u32() != 0;
            loaded.uniforms.push_back(u);
        }
        count = r.u32();
        for (U32 i = 0; i < count && r.ok; ++i)
        {
            loaded.blocks.push_back(r.str());
        }
        count = r.u32();
        for (U32 i = 0; i < count && r.ok; ++i)
        {
            loaded.inputs.push_back(r.str());
        }
        if (!r.ok || r.pos != size)
        {
            return false;   // cut short or damaged: made again
        }
        view = std::move(loaded);
        return true;
    }

    bool load_gl_view(const std::string& path, LLVKShader::GLReflection& view)
    {
        std::string data;
        if (!LLVKShader::readCacheFile(path, data) || !strip_trailer(data) || !parse_gl_view(data.data(), data.size(), view))
        {
            return false;
        }
        mark_used(path);
        return true;
    }

    // The GL view of `key` (gl_view_key) from the bundle.
    bool load_bundle_gl_view(U64 key, LLVKShader::GLReflection& view)
    {
        const Bundle* b = bundle();
        const char* part = nullptr;
        size_t size = 0;
        if (!b || !bundle_find(*b, b->view_table, b->views, key, part, size))
        {
            return false;
        }
        if (!parse_gl_view(part, size, view))
        {
            tell_bundle_damage();
            return false;
        }
        return true;
    }

    std::mutex sGLViewStatsMutex;
    LLVKShader::GLViewStats sGLViewStats;
}

bool LLVKShader::reflectGL(const std::vector<Source>& sources, GLReflection& out)
{
    init();
    const Clock::time_point start = Clock::now();
    const U64 key = gl_view_key(sources);
    const std::string path = gl_view_path(key);
    GLViewStats one;
    one.views = 1;
    bool linked = false;
    if (load_gl_view(path, out))
    {
        one.cache_hits = 1;
        linked = out.linked;
    }
    else if (!sources.empty() && load_bundle_gl_view(key, out))
    {
        one.bundle_hits = 1;    // research/159 section 4: not copied into the cache
        linked = out.linked;
    }
    else
    {
        linked = reflect_gl(sources, out, one.front_ms, one.optimize_ms);
        if (!sources.empty())
        {
            save_gl_view(path, out);
        }
    }
    one.total_ms = ms_since(start);
    std::lock_guard<std::mutex> lock(sGLViewStatsMutex);
    sGLViewStats.views += one.views;
    sGLViewStats.cache_hits += one.cache_hits;
    sGLViewStats.bundle_hits += one.bundle_hits;
    sGLViewStats.front_ms += one.front_ms;
    sGLViewStats.optimize_ms += one.optimize_ms;
    sGLViewStats.total_ms += one.total_ms;
    return linked;
}

LLVKShader::GLViewStats LLVKShader::takeGLViewStats()
{
    std::lock_guard<std::mutex> lock(sGLViewStatsMutex);
    GLViewStats stats = sGLViewStats;
    sGLViewStats = GLViewStats();
    return stats;
}

void LLVKShader::destroy(Program& program)
{
    VkDevice device = LLVKDevice::instance().getDevice();
    for (VkShaderModule module : program.modules)
    {
        vkDestroyShaderModule(device, module, nullptr);
    }
    program.modules.clear();
    program.stages.clear();
    program.uniforms.clear();
    program.blocks.clear();
    program.inputs.clear();
}

// ------------------------------------------------------------------
// Compile workers (research/111 B-SC, B-PW)

LLVKShader::Workers::Workers(const char* name, U32 threads, bool background)
    : mBackground(background)
{
    init();     // before any worker builds
    for (U32 i = 0; i < llmax(threads, 1u); ++i)
    {
        sLiveWorkers.fetch_add(1);
        mThreads.emplace_back([this, name = std::string(name)]()
        {
            set_thread_name(name.c_str());
            run();
        });
    }
    setPriority(background);
}

LLVKShader::Workers::~Workers()
{
    stop();
}

void LLVKShader::Workers::stop()
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mJobs.clear();
        mStop = true;
    }
    mWork.notify_all();
    for (std::thread& thread : mThreads)
    {
        if (thread.joinable())
        {
            thread.join();
            sLiveWorkers.fetch_sub(1);
        }
    }
    // Nothing runs a job now (and post() keeps none): a wait() returns.
    std::lock_guard<std::mutex> lock(mMutex);
    mDone.notify_all();
}

void LLVKShader::Workers::post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStop)
        {
            return;     // stopped: no thread would run it
        }
        mJobs.push_back(std::move(job));
    }
    mWork.notify_one();
}

void LLVKShader::Workers::wait()
{
    std::unique_lock<std::mutex> lock(mMutex);
    mDone.wait(lock, [this]() { return mJobs.empty() && !mRunning; });
}

void LLVKShader::Workers::cancel()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mJobs.clear();
    if (!mRunning)
    {
        mDone.notify_all();
    }
}

bool LLVKShader::Workers::idle()
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mJobs.empty() && !mRunning;
}

void LLVKShader::Workers::setBackground(bool background)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mBackground != background)
    {
        mBackground = background;
        setPriority(background);
    }
}

void LLVKShader::Workers::setPriority(bool background)
{
#if LL_WINDOWS
    // Below everything the frame needs (main, recorder, culling, texture
    // decoding): a prewarm only takes what would otherwise idle.
    for (std::thread& thread : mThreads)
    {
        SetThreadPriority((HANDLE)thread.native_handle(), background ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL);
    }
#endif
}

void LLVKShader::Workers::run()
{
    std::unique_lock<std::mutex> lock(mMutex);
    for (;;)
    {
        mWork.wait(lock, [this]() { return mStop || !mJobs.empty(); });
        if (mStop)
        {
            return;
        }
        std::function<void()> job = std::move(mJobs.front());
        mJobs.pop_front();
        ++mRunning;
        // A prewarm's builds stay out of takeStats() (a load's own count), as
        // the job was posted: one that started in the background stays there.
        tBackgroundJob = mBackground;
        lock.unlock();
        // As fix-threads4: an exception leaving the thread would end the
        // process, and skipping the --mRunning below would leave wait() hung.
        // The job is logged and dropped.
        const char* failure = nullptr;
        std::string what;
        try
        {
            job();
        }
        catch (const std::exception& e)
        {
            failure = "an exception";
            try { what = e.what(); } catch (...) {}
        }
        catch (...)
        {
            failure = "an unknown exception";
        }
        if (failure)
        {
            try
            {
                LL_WARNS("VKShader") << "a shader worker job failed (" << failure << (what.empty() ? "" : ": ") << what
                                     << "): skipped" << LL_ENDL;
            }
            catch (...)
            {
            }
        }
        // What the job holds goes before the lock is taken again (fix-round71
        // 5a): a destructor that posts to these workers would otherwise wait
        // for mMutex held by its own thread, and wait() would return before
        // the job's captures were gone.
        job = nullptr;
        lock.lock();
        --mRunning;
        if (mJobs.empty() && !mRunning)
        {
            mDone.notify_all();
        }
    }
}
