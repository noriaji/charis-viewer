/**
 * @file llvkshader.h
 * @brief Runtime GLSL -> SPIR-V for the viewer's own shaders, plus uniform reflection.
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

#ifndef LL_LLVKSHADER_H
#define LL_LLVKSHADER_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// LLShaderMgr hands GL several shader objects per program (feature libraries
// plus the main file) for each stage. Vulkan wants one SPIR-V module per stage,
// so each stage's sources are linked as separate compilation units, exactly as
// GL would. Loose GL uniforms are gathered into the default uniform block by
// glslang's relaxed Vulkan rules, so upstream GLSL compiles unmodified.
namespace LLVKShader
{
    // Uniform blocks land this much above the binding they declare (or are
    // auto-mapped to): samplers and blocks are numbered apart, so a block
    // written "binding = 0" is at binding 64 in the module. Descriptor set
    // layouts made by hand for these modules must use the shifted number.
    constexpr U32 UNIFORM_BLOCK_BINDING_SHIFT = 64;

    enum class Stage
    {
        Vertex,
        Fragment,
        Geometry,
        Compute,
    };

    struct Source
    {
        Stage stage;
        std::string text;
    };

    // One uniform as reflected from the linked program.
    struct Uniform
    {
        std::string name;
        U32 gl_type = 0;        // GL_FLOAT_VEC4, GL_SAMPLER_2D, ... (glDefineType)
        S32 offset = -1;        // byte offset inside its block, -1 for opaque types
        S32 binding = -1;       // descriptor binding (samplers)
        S32 size = 0;           // array size
        S32 array_stride = 0;   // bytes between array elements inside the block
        S32 block = -1;         // index into Program::blocks, -1 for opaque types
        U32 stages = 0;         // stages that read it (0: a member of a block listed but not read)
    };

    struct Block
    {
        std::string name;
        S32 binding = -1;
        S32 size = 0;           // bytes
    };

    // A vertex shader input and the location glslang assigned to it.
    struct Input
    {
        std::string name;
        U32 location = 0;
        U32 gl_type = 0;
    };

    // A descriptor the SPIR-V uses, for checking it against the pipeline layout.
    struct Descriptor
    {
        std::string name;
        U32 set = 0;
        U32 binding = 0;
    };

    struct Program
    {
        std::vector<VkShaderModule> modules;    // one per stage present
        std::vector<VkShaderStageFlagBits> stages;
        std::vector<Uniform> uniforms;
        std::vector<Block> blocks;
        std::vector<Input> inputs;
        std::vector<Descriptor> descriptors;    // every stage, duplicates removed
        std::string log;                        // compile/link errors
        U64 key = 0;                            // the sources' hash (the cache file's name): the same in every session
        // research/160: the modules' SPIR-V as created (compiled, cache or bundle
        // alike): XXH3-64 of each stage in turn as u32 stage, u32 words, the words.
        // Never 0 once built. The pipeline keys name the program by it.
        U64 code_key = 0;
    };

    // Modules::Skip: reflection only (a compile worker warming the cache).
    enum class Modules
    {
        Create,
        Skip,
    };

    // Thread safe: build() may run on any number of threads at once
    // (research/111 B-SC). The same sources compile once: a second caller
    // waits for the first and then reads its result from the cache.
    void init();
    void cleanup();

    // Compile, link, reflect and create shader modules on the current device.
    // Returns false (with program.log set) when any step fails.
    // resource_set: descriptor set for resources without an explicit set
    // (translated GL programs use 1; set 0 is the bindless texture table).
    bool build(const std::vector<Source>& sources, Program& program, U32 resource_set = 0, Modules modules = Modules::Create);
    void destroy(Program& program);

    // A program as GL sees it (research/117 G3): what glGetActiveUniform,
    // glGetAttribLocation and glGetUniformBlockIndex answer, from the GLSL as
    // the viewer gave it (before the translation's rewrites), with the
    // settings build() uses. Active as a GL driver counts it (research/117
    // 7.4): the default block's members, the samplers and the vertex inputs
    // still read after dead code elimination (SPIRV-Tools), every member of
    // a named block glslang finds read. Uniforms in name order (NVIDIA's).
    struct GLUniform
    {
        std::string name;       // as glslang names it ("name", "name[0]", "Block.member")
        U32 gl_type = 0;        // GL_FLOAT_VEC4, GL_SAMPLER_2D, ...
        S32 size = 1;           // array size
        bool in_block = false;  // a member of a named uniform block (GL gives it no location)
    };
    struct GLReflection
    {
        bool linked = false;
        std::string log;                    // compile/link errors
        std::vector<GLUniform> uniforms;
        std::vector<std::string> blocks;    // named uniform blocks read
        std::vector<std::string> inputs;    // vertex inputs read
    };
    // Any thread. Kept on disk next to the SPIR-V cache (research/117 8.6):
    // the same sources are answered from there in a later session.
    bool reflectGL(const std::vector<Source>& sources, GLReflection& out);
    // reflectGL's cost since the last call (logged when the shaders are
    // loaded): views asked for, answered from the disk cache, and the time of
    // the ones worked out (parse, link and reflection; the stages' SPIR-V and
    // dead code elimination) and of all of them. Summed over threads.
    struct GLViewStats
    {
        U32 views = 0;
        U32 cache_hits = 0;
        U32 bundle_hits = 0;    // answered from the shader bundle (research/159)
        F64 front_ms = 0.0;
        F64 optimize_ms = 0.0;
        F64 total_ms = 0.0;
    };
    GLViewStats takeGLViewStats();

    // Folder of the on-disk caches (SPIR-V here, the pipeline cache in
    // LLVKDevice), in UTF-8: LLRHI_VK_SHADER_CACHE, else vk_shader_cache in
    // the viewer's cache folder (%LOCALAPPDATA%\CharisViewer unless moved in
    // the preferences, research/114). Empty when there is none or it cannot
    // be made.
    const std::string& cacheDir();

    // Cache files by UTF-8 path (the folder may hold any characters, e.g. a
    // Japanese user name). The whole file into `data`; false when it cannot
    // be opened or read.
    bool readCacheFile(const std::string& path, std::string& data);
    // Written to a temporary file of this process and thread, then renamed
    // over `path`, so a reader (or a second viewer) never sees half a file.
    // False, with nothing renamed, when the write or the rename fails.
    struct CacheBytes
    {
        const void* data;
        size_t size;
    };
    bool writeCacheFile(const std::string& path, std::initializer_list<CacheBytes> parts);
    // The trailer the cache files end in (the size and the hash of what comes
    // before it, as the .vkp files have): appended, or checked and taken off
    // (false when it does not match: a file cut short or otherwise damaged).
    void appendCacheTrailer(std::string& data);
    bool stripCacheTrailer(std::string& data);

    // The build counters since the last call (research/111: how much a reload
    // or a prewarm compiled, and where it waited). Any thread. The jobs of
    // background Workers (the prewarm) count in their thread's stats only,
    // not in takeStats(): a load's line is the load's own builds.
    struct Stats
    {
        U32 builds = 0;         // build() calls
        U32 cache_hits = 0;     // of them, read from the cache
        U32 bundle_hits = 0;    // of them, read from the shader bundle (research/159)
        U32 compiled = 0;       // of them, compiled (glslang)
        U32 waited = 0;         // of them, waited for another thread compiling the same sources
        F64 compile_ms = 0.0;   // summed over threads
        F64 wait_ms = 0.0;
        F64 span_ms = 0.0;      // takeStats(): from the first build counted to the call (wall time)
    };
    Stats takeStats();
    // The same, of the calling thread's builds only (a prewarm job's).
    Stats takeThreadStats();

    // research/159 3.1 (--bake-shaders): the backend's passes that make their
    // shaders on their first use (the full-screen passes) register here how
    // to set them up ahead of it: the step is the pass's own set-up, so the
    // shaders are built from the same sources, into the same cache entries.
    // Register from the pass's install() (main thread, before any bake). The
    // steps run where the pass's draws would set it up: on the translation
    // thread when it runs. A step returns false when its set-up failed.
    void addBakeStep(const char* name, bool (*step)());
    // Runs every step once; returns how many ran, `failed` how many of them
    // returned false (each logged with its name).
    U32 runBakeSteps(U32& failed);

    // LLRHI_SHADER_COMPILE_THREADS=<n>: compile workers for a shader reload
    // (research/111 B-SC). Unset: logical cores less 2 (at least 1). 1: none,
    // every program compiles as it links, as before.
    U32 compileThreads();

    // A small pool of compile threads. background: the lowest priority
    // (prewarming while the user plays); setBackground(false) while someone
    // waits on its work. The destructor drops the jobs not started and joins.
    class Workers
    {
    public:
        Workers(const char* name, U32 threads, bool background);
        ~Workers();
        Workers(const Workers&) = delete;
        Workers& operator=(const Workers&) = delete;

        void post(std::function<void()> job);   // dropped once stop() was called
        void wait();            // every job posted so far is done
        void cancel();          // drops the jobs not started
        // fix-threads4: drops the jobs not started, lets each thread finish the
        // job it runs and joins them; later posts are dropped. The object stays
        // (a job still running may post to it until it is joined). Not from
        // one of its own jobs.
        void stop();
        void setBackground(bool background);
        bool idle();
        U32 threads() const { return (U32)mThreads.size(); }

    private:
        void run();
        void setPriority(bool background);

        std::mutex mMutex;
        std::condition_variable mWork;
        std::condition_variable mDone;
        std::deque<std::function<void()>> mJobs;
        std::vector<std::thread> mThreads;
        U32 mRunning = 0;
        bool mStop = false;
        bool mBackground = false;
    };
}

#endif // LL_LLVKSHADER_H
