/**
 * @file llvkgl_linkbuild.cpp
 * @brief The compile of a GL program's link, its parallel shader reload and the shader prewarm (research/111 B-SC, B-PW).
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

// A shader reload (a preset, a setting) links some 250 programs one after
// another, and a link only returns once its SPIR-V is built: the first time a
// combination of settings is used, glslang compiles them all on the main
// thread (research/106: 247 programs, 19 s). Two remedies here, both filling
// the same SPIR-V cache (LLVKShader), so the links themselves stay as they are:
//
// - The parallel reload (B-SC, LLRHI::sParallelShaderReload): the reload runs
//   twice. In the first pass every link hands its compile (compile_link) to
//   the compile workers and leaves the program unbuilt; GL links as usual. The
//   second pass links for real, and each build finds its compile done (the
//   cache) or waits for the worker on it.
// - The prewarm (B-PW, LLRHI::sPrewarmShaders): after login, the programs of
//   the other graphics presets are compiled on low priority threads. Their GL
//   sources are this session's with the global defines the preset would give
//   (LLGLSLShader::sGlobalDefines, in the feature library files) in place of
//   today's. So are the OIT variants of the programs that had one before
//   (oit_variants.txt beside the cache): an alpha program's first draw under
//   OIT no longer stops to compile it.
// - The background reload (research/250, LLRHI::sShaderReloadBuilder): the
//   reload's plan pass (LLGLSLShader::sPlan) hands each program's GL sources
//   here; low priority threads make all a link makes of them (GL's view and
//   compile_link with the modules) while the frames go on with the programs
//   in use, and the commit's links take those builds (take_background_build)
//   instead of making them on the main thread.

#include "linden_common.h"

#include "llvkgl_internal.h"

#if LL_GL_FUNC_POINTER

#include "llfile.h"     // llifstream, llofstream: the cache folder's path is UTF-8
#include "hbxxh.h"
#include "llrhihash.h"

namespace llvkgl
{
namespace
{
    using Clock = std::chrono::steady_clock;

    F64 seconds_since(Clock::time_point start)
    {
        return std::chrono::duration<F64>(Clock::now() - start).count();
    }

    // LLRHI_SHARED_LAYOUT=0 keeps every program's own pipeline layout (research/50 step 3).
    const bool sSharedLayoutEnabled = LLRHI::envFirstChar("LLRHI_SHARED_LAYOUT") != '0';

    // What a built program has in set 1: the default uniform block (its index
    // into vk.blocks, -1: none) and whether anything else (another uniform
    // block, a descriptor sampler, any other descriptor of set 1).
    void set1_use(const LLVKShader::Program& vk, S32& default_block, bool& other)
    {
        default_block = -1;
        other = false;
        S32 default_binding = -1;
        for (size_t i = 0; i < vk.blocks.size(); ++i)
        {
            if (vk.blocks[i].binding < 0)
            {
                continue;
            }
            if (vk.blocks[i].name == "gl_DefaultUniformBlock")
            {
                default_block = (S32)i;
                default_binding = vk.blocks[i].binding;
            }
            else
            {
                other = true;
            }
        }
        for (const LLVKShader::Uniform& uniform : vk.uniforms)
        {
            other |= uniform.binding >= 0 && is_sampler_type(uniform.gl_type) && uniform.name.compare(0, 14, "llrhi_textures") != 0;
        }
        for (const LLVKShader::Descriptor& d : vk.descriptors)
        {
            const bool table = d.set == 0 && ((d.binding == 0 && d.name == "llrhi_textures") || (d.binding == 1 && d.name == "llrhi_mt") ||
                                              (d.binding == 2 && d.name == "llrhi_pal"));
            other |= !table && !(d.set == 1 && (S32)d.binding == default_binding);
        }
    }
} // namespace

    void compile_link(GLuint id, LinkBuild& link, LLVKShader::Modules modules, const char* name)
    {
        // A worker warming the cache stays quiet: the link that follows logs.
        const bool loud = modules == LLVKShader::Modules::Create;
        const std::string who = !loud ? std::string() : name ? "'" + std::string(name) + "'" : std::to_string(id);
        std::vector<LLVKShader::Source>& sources = link.sources;
        const std::vector<LLVKShader::Source> without_alpha_fast = sources;
        const bool alpha_fast = alpha_fast_rewrite(sources);
        std::vector<LLVKShader::Source>& unrewritten = link.unrewritten;
        unrewritten = sources;
        bool& light_table = link.light_table;
        std::vector<MaterialMember>& materials = link.materials;
        const std::vector<ConstWanted> no_consts;
        std::vector<ConstMember>& consts = link.consts;
        std::vector<std::string>& bindless_names = link.bindless_names;
        bindless_names = bindless_rewrite(sources, link.matrix_mask, link.palette_columns, link.palette_elements, true, light_table, true,
                                          materials, no_consts, consts);
        // Draw runs (research/46 G4b): a marked program reads its push constants
        // from a row of the frame's table when a run says so (draw_rows_rewrite).
        const std::vector<LLVKShader::Source> without_rows = sources;
        link.row_words = (U32)bindless_names.size() + (link.matrix_mask ? 1 : 0) + (link.palette_columns ? 1 : 0) +
                         (light_table ? 1 : 0) + (materials.empty() ? 0 : 1);
        bool& draw_rows = link.draw_rows;
        draw_rows = LLRHI::sDrawRuns && draw_rows_rewrite(sources, (U32)bindless_names.size(), link.matrix_mask != 0,
                                                          link.palette_columns != 0, light_table, !materials.empty(), false);
        // Program resources live in set 1; set 0 is the bindless table.
        bool& built = link.built;
        built = !sources.empty() && LLVKShader::build(sources, link.vk, 1, modules);
        if (!built && draw_rows)
        {
            // The rewrite did not build: the program as before; its draws share
            // a run only while their push constants stay the same.
            if (loud)
            {
                LL_WARNS("VKGL") << "program " << who << ": draw row rewrite failed to build, rows stay push constants: " << link.vk.log << LL_ENDL;
            }
            LLVKShader::destroy(link.vk);
            link.vk = LLVKShader::Program();
            sources = without_rows;
            draw_rows = false;
            built = LLVKShader::build(sources, link.vk, 1, modules);
        }
        bool allow_materials = true;
        if (!built && !materials.empty())
        {
            // The material rewrite (research/46 G4a) did not build (a material
            // name used where its macro does not fit): the program without it,
            // its material uniforms in the default block as before.
            const std::string material_log = link.vk.log;
            LLVKShader::destroy(link.vk);
            link.vk = LLVKShader::Program();
            sources = unrewritten;
            allow_materials = false;
            draw_rows = false;
            bindless_names = bindless_rewrite(sources, link.matrix_mask, link.palette_columns, link.palette_elements, true, light_table,
                                              false, materials, no_consts, consts);
            built = LLVKShader::build(sources, link.vk, 1, modules);
            if (built && loud)
            {
                LL_WARNS("VKGL") << "program " << who << ": material table rewrite failed to build, materials stay uniforms: " << material_log << LL_ENDL;
            }
        }
        if (!built && light_table)
        {
            // The light table rewrite (research/47) did not build: the program
            // as before, its lights drawn one by one (sLightTableProgram false).
            if (loud)
            {
                LL_WARNS("VKGL") << "program " << who << ": light table rewrite failed to build, lights go one at a time: " << link.vk.log << LL_ENDL;
            }
            LLVKShader::destroy(link.vk);
            link.vk = LLVKShader::Program();
            sources = unrewritten;
            draw_rows = false;
            bindless_names = bindless_rewrite(sources, link.matrix_mask, link.palette_columns, link.palette_elements, false, light_table,
                                              allow_materials, materials, no_consts, consts);
            built = LLVKShader::build(sources, link.vk, 1, modules);
        }
        if (!built && alpha_fast)
        {
            // An early out (research/54, 59) did not build: the shader as
            // before (discard keeps running as a helper), with the other
            // rewrites as far as they were kept above (no draw rows: a marked
            // program's push constants then stay push constants).
            if (loud)
            {
                LL_WARNS("VKGL") << "program " << who << ": early out (alpha / material / probe fuse) failed to build, shader as before: " << link.vk.log << LL_ENDL;
            }
            LLVKShader::destroy(link.vk);
            link.vk = LLVKShader::Program();
            unrewritten = without_alpha_fast;
            sources = unrewritten;
            draw_rows = false;
            const bool allow_lights = light_table;
            bindless_names = bindless_rewrite(sources, link.matrix_mask, link.palette_columns, link.palette_elements, allow_lights,
                                              light_table, allow_materials, materials, no_consts, consts);
            built = LLVKShader::build(sources, link.vk, 1, modules);
        }
        // The shared pipeline layout (research/50 step 3): a program marked for
        // draw runs whose set 1 holds nothing but the default uniform block is
        // made again with that block's live members (as reflected) read from
        // the constants row, and then has no set 1. It keeps the build above
        // when anything else is in set 1 (the blended legacy materials'
        // shadow maps, probes and probe block), when a member cannot be
        // rewritten (bindless_rewrite's consts), or when the result does not
        // build or still has a set 1. LLRHI_SHARED_LAYOUT=0: never.
        bool& shared = link.shared;
        shared = false;
        if (built && sSharedLayoutEnabled && LLRHI::sDrawRuns && !light_table && draw_runs_marked(unrewritten))
        {
            S32 default_block = -1;
            bool other = false;
            set1_use(link.vk, default_block, other);
            std::vector<ConstWanted> wanted;
            for (const LLVKShader::Uniform& uniform : link.vk.uniforms)
            {
                // Reflection lists every member of the block: the read ones only.
                if (other || default_block < 0 || uniform.block != default_block || !uniform.stages)
                {
                    continue;
                }
                std::string name = uniform.name;
                if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0)
                {
                    name.resize(name.size() - 3);
                }
                // The matrices a direct draw writes into the default block
                // (direct_writes_block) and the palette stay where they are,
                // as does what is no plain uniform (the block then stays).
                if (uniform.offset >= 0 && name.find_first_of(".[") == std::string::npos && name != "modelview_matrix" &&
                    name != "modelview_projection_matrix" && name != "normal_matrix" && name != "inv_modelview" &&
                    name != "texture_matrix0" && name != "matrixPalette")
                {
                    wanted.push_back({ name, uniform.gl_type, (U32)llmax(1, uniform.size) });
                }
            }
            shared = !other && (default_block < 0 || !wanted.empty());
            if (shared && default_block >= 0)
            {
                std::vector<LLVKShader::Source> again = unrewritten;
                U32 matrix_mask = 0, palette_columns = 0, palette_elements = 0;
                bool again_light = false;
                std::vector<MaterialMember> again_materials;
                const std::vector<std::string> again_names = bindless_rewrite(again, matrix_mask, palette_columns, palette_elements, false,
                                                                              again_light, allow_materials, again_materials, wanted, consts);
                // The same rewrites as the build above, plus the constants.
                shared = !consts.empty() && again_names == bindless_names && matrix_mask == link.matrix_mask &&
                         palette_columns == link.palette_columns && palette_elements == link.palette_elements &&
                         again_materials.size() == materials.size();
                for (size_t i = 0; shared && i < materials.size(); ++i)
                {
                    shared = again_materials[i].name == materials[i].name && again_materials[i].components == materials[i].components &&
                             again_materials[i].count == materials[i].count && again_materials[i].first == materials[i].first;
                }
                if (shared && draw_rows)
                {
                    shared = draw_rows_rewrite(again, (U32)again_names.size(), matrix_mask != 0, palette_columns != 0, false,
                                               !again_materials.empty(), true);
                }
                // Destroyed at the end of the scope unless the link keeps it
                // (review async-shader: an exception on the way left it).
                struct TriedProgram
                {
                    LLVKShader::Program vk;
                    bool kept = false;
                    ~TriedProgram()
                    {
                        if (!kept)
                        {
                            LLVKShader::destroy(vk);
                        }
                    }
                } tried;
                LLVKShader::Program& vk = tried.vk;
                if (shared)
                {
                    shared = LLVKShader::build(again, vk, 1, modules);
                    if (!shared)
                    {
                        if (loud)
                        {
                            LL_WARNS("VKGL") << "program " << who << ": shared layout rewrite failed to build, keeps its own layout: " << vk.log << LL_ENDL;
                        }
                    }
                }
                if (shared)
                {
                    default_block = -1;
                    set1_use(vk, default_block, other);
                    shared = default_block < 0 && !other;
                    if (!shared)
                    {
                        if (loud)
                        {
                            LL_WARNS("VKGL") << "program " << who << ": shared layout rewrite still has a set 1, keeps its own layout" << LL_ENDL;
                        }
                    }
                }
                if (shared)
                {
                    LLVKShader::destroy(link.vk);
                    link.vk = vk;
                    tried.kept = true;
                    sources = again;
                }
            }
        }
        if (!shared)
        {
            consts.clear();
        }
    }

namespace
{
    // "void main" with any spacing; the feature library files have none.
    bool has_main(const std::string& text)
    {
        for (size_t at = text.find("main"); at != std::string::npos; at = text.find("main", at + 4))
        {
            size_t before = at;
            while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t'))
            {
                --before;
            }
            size_t after = at + 4;
            while (after < text.size() && (text[after] == ' ' || text[after] == '\t'))
            {
                ++after;
            }
            if (before != at && before >= 4 && text.compare(before - 4, 4, "void") == 0 && after < text.size() && text[after] == '(')
            {
                return true;
            }
        }
        return false;
    }

} // namespace

    // XXH3 (prebake-3): every link of a reload hashes its whole text (some
    // 250 links of tens of KB); a byte at a time FNV was some 20-30 ms a reload.
    U64 program_identity(const std::vector<LLVKShader::Source>& sources)
    {
        static const std::string DEFINE = "#define ";
        HBXXH64 hash;
        bool main_found = false;
        for (const LLVKShader::Source& source : sources)
        {
            const char stage = (char)source.stage;
            hash.update(&stage, 1);
            const std::string& text = source.text;
            if (has_main(text))
            {
                main_found = true;
                hash.update(text.data(), text.size());
                continue;
            }
            // A feature library: its lines but the #define ones (LLGLSLShader::sGlobalDefines),
            // each run of other lines hashed at once.
            size_t run = 0;
            for (size_t line = 0; line < text.size();)
            {
                size_t end = text.find('\n', line);
                end = end == std::string::npos ? text.size() : end + 1;
                if (text.compare(line, DEFINE.size(), DEFINE) == 0)
                {
                    hash.update(text.data() + run, line - run);
                    run = end;
                }
                line = end;
            }
            hash.update(text.data() + run, text.size() - run);
        }
        const U64 identity = hash.digest();
        return main_found && identity ? identity : 0;
    }

    U64 main_fragment_hash(const std::vector<LLVKShader::Source>& sources)
    {
        U64 found = 0;
        for (const LLVKShader::Source& source : sources)
        {
            if (source.stage != LLVKShader::Stage::Fragment || !has_main(source.text))
            {
                continue;
            }
            if (found)
            {
                return 0;
            }
            found = LLRHI::fnv1a(LLRHI::FNV1A_BASIS, source.text.data(), source.text.size());
        }
        return found;
    }

namespace
{
    // ------------------------------------------------------------------
    // The GL sources of a program, its files shared with the other programs
    // they are attached to (the feature libraries: one string, not one per program).

    struct GLFile
    {
        LLVKShader::Stage stage;
        std::shared_ptr<const std::string> text;
    };
    using GLProgram = std::vector<GLFile>;
    using GLTexts = std::unordered_map<GLuint, std::shared_ptr<const std::string>>;

    // Under sLock.
    GLProgram gl_program(GLuint id, GLTexts& texts)
    {
        GLProgram program;
        auto attached = sAttached.find(id);
        if (attached == sAttached.end())
        {
            return program;
        }
        for (GLuint shader : attached->second)
        {
            auto it = sShaderSources.find(shader);
            if (it == sShaderSources.end())
            {
                continue;
            }
            std::shared_ptr<const std::string>& text = texts[shader];
            if (!text)
            {
                text = std::make_shared<const std::string>(it->second.second);
            }
            program.push_back({ shader_stage(it->second.first), text });
        }
        return program;
    }

    std::vector<LLVKShader::Source> sources_of(const GLProgram& program)
    {
        std::vector<LLVKShader::Source> sources;
        sources.reserve(program.size());
        for (const GLFile& file : program)
        {
            sources.push_back({ file.stage, *file.text });
        }
        return sources;
    }

    // ------------------------------------------------------------------
    // The parallel reload (B-SC)

    LLVKShader::Workers* sPlan = nullptr;   // during the first pass only
    GLTexts sPlanTexts;
    U32 sPlanned = 0;
    // During both passes (research/159 section 2.1): the reload logs its own
    // build counts, so the load's line (draw_run_programs) leaves them to it.
    std::atomic<bool> sParallelReload{ false };

    // ------------------------------------------------------------------
    // OIT variants made before (B-PW): their programs' main_fragment_hash,
    // one hexadecimal number a line in oit_variants.txt beside the cache.

    constexpr size_t OIT_LIST_LIMIT = 2048;     // past it, the file keeps the newer half

    std::mutex sOITMutex;
    std::unordered_set<U64> sOITPrograms;
    bool sOITLoaded = false;

    std::string oit_list_path()
    {
        const std::string& dir = LLVKShader::cacheDir();
        return dir.empty() ? std::string() : dir + "/oit_variants.txt";
    }

    // Under sOITMutex.
    void load_oit_list()
    {
        if (sOITLoaded)
        {
            return;
        }
        sOITLoaded = true;
        const std::string path = oit_list_path();
        if (path.empty())
        {
            return;
        }
        std::vector<U64> order;
        llifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            const U64 key = strtoull(line.c_str(), nullptr, 16);
            if (key && sOITPrograms.insert(key).second)
            {
                order.push_back(key);
            }
        }
        file.close();
        if (order.size() > OIT_LIST_LIMIT)
        {
            // Old viewers' programs: keep the newer half.
            order.erase(order.begin(), order.end() - OIT_LIST_LIMIT / 2);
            sOITPrograms = std::unordered_set<U64>(order.begin(), order.end());
            // Through a temporary file (fix-round49): a failed write keeps the old list.
            std::ostringstream text;
            for (U64 key : order)
            {
                text << std::hex << key << '\n';
            }
            const std::string data = text.str();
            LLVKShader::writeCacheFile(path, { { data.data(), data.size() } });
        }
    }

    bool oit_known(U64 main_fragment)
    {
        std::lock_guard<std::mutex> lock(sOITMutex);
        load_oit_list();
        return main_fragment && sOITPrograms.count(main_fragment);
    }

    // A job's build counts (LLVKShader::takeThreadStats) into a sum of jobs.
    void add_build_counts(LLVKShader::Stats& into, const LLVKShader::Stats& from)
    {
        into.builds += from.builds;
        into.cache_hits += from.cache_hits;
        into.bundle_hits += from.bundle_hits;
        into.compiled += from.compiled;
        into.compile_ms += from.compile_ms;
    }

    // ------------------------------------------------------------------
    // The prewarm (B-PW)

    // One prewarm call's work: logged when its last job is done.
    struct PrewarmBatch
    {
        Clock::time_point start = Clock::now();
        std::atomic<U32> left{ 1 };     // the planning job's hold, given back once every job is posted
        std::atomic<U32> links{ 0 };    // other presets' links
        std::atomic<U32> oit{ 0 };      // OIT variants
        U32 presets = 0;
        std::mutex mutex;
        LLVKShader::Stats stats;
    };

    // Made on first use and never destroyed (as the device); its threads end
    // at shutdown (stop_shader_prewarm), before the statics its jobs read go.
    LLVKShader::Workers* sPrewarm = nullptr;

    LLVKShader::Workers& prewarm_workers()
    {
        if (!sPrewarm)
        {
            // A quarter of the reload's threads: the frame keeps the rest.
            sPrewarm = new LLVKShader::Workers("VKShaderPrewarm", llmax(1u, LLVKShader::compileThreads() / 4), true);
        }
        return *sPrewarm;
    }

    // A prewarm gives way to a reload: its queue goes, and what it is compiling
    // now finishes at normal priority (a link of the reload may wait for it);
    // back in the background after the reload.
    void prewarm_gives_way()
    {
        if (sPrewarm)
        {
            sPrewarm->cancel();
            sPrewarm->setBackground(false);
        }
    }

    void prewarm_resumes()
    {
        if (sPrewarm)
        {
            sPrewarm->setBackground(true);
        }
    }

    void finish_job(const std::shared_ptr<PrewarmBatch>& batch)
    {
        if (batch->left.fetch_sub(1) != 1 || (!batch->links && !batch->oit))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(batch->mutex);
        LL_INFOS("VKShader") << "Shader prewarm done in " << seconds_since(batch->start) << " s: " << batch->links << " links of "
                             << batch->presets << " other presets, " << batch->oit << " OIT variants; " << batch->stats.builds
                             << " builds, " << batch->stats.compiled << " compiled (" << batch->stats.compile_ms / 1000.0
                             << " s of compiling), " << batch->stats.cache_hits << " from the cache, " << batch->stats.bundle_hits
                             << " from the bundle" << LL_ENDL;
    }

    // The link's compile into the cache, and its OIT variant's when one was
    // made before (oit_variant builds it from the link's sources as built).
    // only_oit: nothing unless it had an OIT variant. every_oit (the shader
    // bake, research/159 3.1): the variant of every program with one fragment
    // main, made before or not. oit_made counts the variants built.
    void warm(const GLProgram& program, bool only_oit, std::atomic<U32>& oit_made, bool every_oit = false)
    {
        LinkBuild link;
        link.sources = sources_of(program);
        const U64 main_fragment = LLRHI::sOITSources ? main_fragment_hash(link.sources) : 0;
        const bool oit = main_fragment && (every_oit || oit_known(main_fragment));
        if (only_oit && !oit)
        {
            return;
        }
        prepare_gl_view(link.sources, false);   // research/117 8.6: GL's view of the other presets' programs on disk too (only there)
        compile_link(0, link, LLVKShader::Modules::Skip);
        std::vector<LLVKShader::Source> variant = link.sources;
        // research/205 3a-2: the row word as oit_variant gives it (Program::draw_row_words).
        const U32 row_word = oit_row_word(link.draw_rows, link.draw_rows ? link.row_words + (link.consts.empty() ? 0 : 1) : 0);
        if (oit && link.built && oit_rewrite(variant, row_word))
        {
            LLVKShader::Program vk;
            if (LLVKShader::build(variant, vk, 1, LLVKShader::Modules::Skip) || !every_oit)
            {
                ++oit_made;     // the prewarm counts the variants it tried, as before
            }
        }
    }

    void post_warm(const std::shared_ptr<PrewarmBatch>& batch, GLProgram program, bool only_oit)
    {
        batch->left.fetch_add(1);
        prewarm_workers().post([batch, program = std::move(program), only_oit]()
        {
            warm(program, only_oit, batch->oit);
            const LLVKShader::Stats stats = LLVKShader::takeThreadStats();
            {
                std::lock_guard<std::mutex> lock(batch->mutex);
                add_build_counts(batch->stats, stats);
            }
            finish_job(batch);
        });
    }

    std::string defines_text(const std::map<std::string, std::string>& defines)
    {
        // As LLShaderMgr::loadShaderFile writes them, in the map's order.
        std::string text;
        for (const auto& define : defines)
        {
            text += "#define " + define.first + " " + define.second + "\n";
        }
        return text;
    }

    // The program with `to` in place of `from` in the files that have it (the
    // feature libraries). Empty: no file has it (the preset changes nothing here).
    GLProgram with_defines(const GLProgram& program, const std::string& from, const std::string& to,
                           std::unordered_map<const std::string*, std::shared_ptr<const std::string>>& replaced)
    {
        bool changed = false;
        GLProgram out = program;
        for (GLFile& file : out)
        {
            auto it = replaced.find(file.text.get());
            if (it == replaced.end())
            {
                std::shared_ptr<const std::string> text;
                const size_t at = file.text->find(from);
                if (at != std::string::npos)
                {
                    std::string edited = *file.text;
                    edited.replace(at, from.size(), to);
                    text = std::make_shared<const std::string>(std::move(edited));
                }
                it = replaced.emplace(file.text.get(), text).first;
            }
            if (it->second)
            {
                file.text = it->second;
                changed = true;
            }
        }
        return changed ? out : GLProgram();
    }

    // Every built program, as GL has its sources.
    std::vector<GLProgram> linked_programs()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        GLTexts texts;
        std::vector<GLProgram> programs;
        for (const auto& entry : sPrograms)
        {
            if (entry.second.built)
            {
                GLProgram program = gl_program(entry.first, texts);
                if (!program.empty())
                {
                    programs.push_back(std::move(program));
                }
            }
        }
        return programs;
    }

    // On a prewarm thread: what to compile. This session's programs for the
    // OIT variants made before, then every program as each target preset
    // would have it.
    void plan_prewarm(const std::shared_ptr<PrewarmBatch>& batch, const std::vector<GLProgram>& programs, const std::string& from,
                      const std::vector<std::string>& targets)
    {
        if (LLRHI::sOITSources)
        {
            for (const GLProgram& program : programs)
            {
                post_warm(batch, program, true);
            }
        }
        for (const std::string& to : targets)
        {
            std::unordered_map<const std::string*, std::shared_ptr<const std::string>> replaced;
            for (const GLProgram& program : programs)
            {
                GLProgram other = with_defines(program, from, to, replaced);
                if (!other.empty())
                {
                    post_warm(batch, std::move(other), false);
                    ++batch->links;
                }
            }
        }
        if (batch->links)
        {
            LL_INFOS("VKShader") << "Shader prewarm: " << batch->links << " links of " << batch->presets << " other presets on "
                                 << prewarm_workers().threads() << " low priority threads" << LL_ENDL;
        }
        finish_job(batch);
    }

    // Replaces what an earlier prewarm has not started.
    void start_prewarm(std::vector<GLProgram> programs, std::string from, std::vector<std::string> targets)
    {
        LLVKShader::Workers& workers = prewarm_workers();
        workers.cancel();
        auto batch = std::make_shared<PrewarmBatch>();
        batch->presets = (U32)targets.size();
        workers.post([batch, programs = std::move(programs), from = std::move(from), targets = std::move(targets)]()
        {
            plan_prewarm(batch, programs, from, targets);
        });
    }

    void prewarm_shaders(const std::map<std::string, std::string>& current, const std::vector<std::map<std::string, std::string>>& targets)
    {
        if (!LLRHI::sShaderPrewarm || LLVKShader::cacheDir().empty())
        {
            return;
        }
        std::string from = defines_text(current);
        std::vector<std::string> other;
        for (const auto& target : targets)
        {
            const std::string to = defines_text(target);
            if (!from.empty() && to != from && std::find(other.begin(), other.end(), to) == other.end())
            {
                other.push_back(to);
            }
        }
        start_prewarm(linked_programs(), std::move(from), std::move(other));
    }

    // bake (the shader bake, research/159 3.1): its build counts go there,
    // and no OIT prewarm follows (the bake makes every variant itself).
    bool parallel_reload(void (*reload)(), LLVKShader::Stats* bake)
    {
        // fix-round107: a reload inside one of this reload's passes (the
        // hook at the head of setShaders, under a caller that asked for a
        // parallel reload itself) runs plainly: it is that pass.
        if (sParallelReload)
        {
            return false;
        }
        const U32 threads = LLVKShader::compileThreads();
        if (threads <= 1 || LLVKShader::cacheDir().empty())
        {
            return false;
        }
        prewarm_gives_way();
        LLVKShader::takeStats();    // counted from here
        const Clock::time_point start = Clock::now();
        F64 first_pass = 0.0;
        U32 planned = 0;
        struct Running     // cleared however the reload ends
        {
            Running() { sParallelReload = true; }
            ~Running() { sParallelReload = false; }
            Running(const Running&) = delete;
            Running& operator=(const Running&) = delete;
        } running;
        {
            LLVKShader::Workers workers("VKShaderCompile", threads, false);
            // The first pass's programs are unbuilt: its "draw runs" line would read 0.
            auto* const draw_run_programs = LLRHI::sDrawRunPrograms;
            LLRHI::sDrawRunPrograms = nullptr;
            sPlan = &workers;
            sPlanned = 0;
            reload();
            sPlan = nullptr;
            sPlanTexts.clear();
            planned = sPlanned;
            LLRHI::sDrawRunPrograms = draw_run_programs;
            first_pass = seconds_since(start);
            reload();
            workers.wait();     // compiles no link of the second pass asked for (none, normally)
        }
        prewarm_resumes();
        const LLVKShader::Stats stats = LLVKShader::takeStats();
        const F64 total = seconds_since(start);
        LL_INFOS("VKShader") << "Parallel shader reload in " << total << " s on " << threads << " threads: first pass " << first_pass
                             << " s (" << planned << " links to the workers), second pass " << total - first_pass << " s; "
                             << stats.builds << " builds, " << stats.compiled << " compiled (" << stats.compile_ms / 1000.0
                             << " s of compiling), " << stats.cache_hits << " from the cache, " << stats.bundle_hits
                             << " from the bundle, " << stats.waited
                             << " waited for another thread (" << stats.wait_ms / 1000.0 << " s); at the end "
                             << LLVKDevice::instance().memoryReport() << LL_ENDL;  // fix-round107: the second pass's old buffers still wait for two frames
        if (bake)
        {
            *bake = stats;
            return true;
        }
        // The new programs' OIT variants, before an alpha draw asks for them.
        if (LLRHI::sShaderPrewarm && LLRHI::sOITSources)
        {
            start_prewarm(linked_programs(), std::string(), std::vector<std::string>());
        }
        return true;
    }

    bool parallel_shader_reload(void (*reload)())
    {
        return parallel_reload(reload, nullptr);
    }

    // ------------------------------------------------------------------
    // The shader bake (research/159 3.1, --bake-shaders in plus/shaderbake)

    void add_stats(LLRHI::ShaderBakeCounts& out, const LLVKShader::Stats& stats)
    {
        out.builds += stats.builds;
        out.compiled += stats.compiled;
        out.cache_hits += stats.cache_hits;
    }

    // Every linked program's OIT variant, on every compile thread (the bake
    // waits for them: nothing else runs meanwhile).
    void bake_oit_variants(LLRHI::ShaderBakeCounts& out)
    {
        const std::vector<GLProgram> programs = linked_programs();
        std::atomic<U32> made{ 0 };
        std::mutex mutex;
        LLVKShader::Stats stats;
        {
            LLVKShader::Workers workers("VKShaderBake", llmax(1u, LLVKShader::compileThreads()), false);
            for (const GLProgram& program : programs)
            {
                workers.post([&program, &made, &mutex, &stats]()
                {
                    warm(program, true, made, /*every_oit*/ true);
                    const LLVKShader::Stats own = LLVKShader::takeThreadStats();
                    std::lock_guard<std::mutex> lock(mutex);
                    stats.builds += own.builds;
                    stats.compiled += own.compiled;
                    stats.cache_hits += own.cache_hits;
                });
            }
            workers.wait();
        }
        LLVKShader::takeStats();    // the same builds, counted above by thread
        out.oit_variants += made;
        add_stats(out, stats);
    }

    // The backend's own passes, set up as their first use would (their
    // shaders into the cache).
    void bake_fixed_passes(LLRHI::ShaderBakeCounts& out)
    {
        U32 failed = 0;
        out.fixed_passes += LLVKShader::runBakeSteps(failed);
        out.failed_programs += failed;
        auto step = [&out](const char* name, bool made)
        {
            ++out.fixed_passes;
            if (!made)
            {
                LL_WARNS("VKShader") << "Shader bake: " << name << " could not be set up (or this GPU cannot do it)" << LL_ENDL;
                ++out.failed_programs;
            }
        };
        if (LLRHI::sOITSupported && LLRHI::sOITSupported())
        {
            step("OIT", bake_oit_shaders());
        }
        step("the occlusion marks", bake_occlusion_shaders());
        if (LLRHI::sShadowList)
        {
            step("the GPU shadow list", LLRHI::sShadowList->ready());
        }
        if (LLRHI::sProbeList)
        {
            step("the GPU probe list", LLRHI::sProbeList->ready());
        }
        if (LLRHI::sTexBCN)
        {
            step("BC7 texture compression", LLVKTexCompress::built());     // (made, even while off for the session: bc7-fault)
        }
        const LLVKShader::Stats stats = LLVKShader::takeStats();
        add_stats(out, stats);
    }

    bool bake_shaders(void (*reload)(), bool oit, bool fixed, LLRHI::ShaderBakeCounts& out)
    {
        out = LLRHI::ShaderBakeCounts();
        if (LLVKShader::cacheDir().empty())
        {
            LL_WARNS("VKShader") << "Shader bake: no shader cache folder, nothing would be kept" << LL_ENDL;
            return false;
        }
        const Clock::time_point start = Clock::now();
        if (sPrewarm)
        {
            sPrewarm->cancel();     // its builds would count here
        }
        LLVKShader::Stats stats;
        if (!parallel_reload(reload, &stats))
        {
            // One compile thread: each link compiles as it links.
            LLVKShader::takeStats();
            reload();
            stats = LLVKShader::takeStats();
        }
        add_stats(out, stats);
        if (oit && LLRHI::sOITSources)
        {
            bake_oit_variants(out);
        }
        if (fixed)
        {
            bake_fixed_passes(out);
        }
        out.seconds = seconds_since(start);
        return true;
    }

    // ------------------------------------------------------------------
    // The background reload (research/250)

    // A program's GL sources whole (each file's stage, length and text, in
    // GL's order): the commit's link finds its build by the same sources.
    void mix_source(HBXXH64& hash, LLVKShader::Stage stage, const std::string& text)
    {
        const U32 code = (U32)stage;
        const U64 size = text.size();
        hash.update(&code, sizeof(code));
        hash.update(&size, sizeof(size));
        hash.update(text.data(), text.size());
    }

    U64 whole_sources_key(const GLProgram& program)
    {
        HBXXH64 hash;
        for (const GLFile& file : program)
        {
            mix_source(hash, file.stage, *file.text);
        }
        return hash.digest();
    }

    U64 whole_sources_key(const std::vector<LLVKShader::Source>& sources)
    {
        HBXXH64 hash;
        for (const LLVKShader::Source& source : sources)
        {
            mix_source(hash, source.stage, source.text);
        }
        return hash.digest();
    }

    // One plan's builds. A newer plan or a drop moves the generation on: a
    // build still running then drops its own result when it ends.
    struct BackgroundBuild
    {
        std::mutex mutex;
        U32 generation = 0;
        std::unordered_multimap<U64, LinkBuild> ready;     // this generation's builds, by whole_sources_key
        std::vector<std::string> failed;                    // the names of those not built
        U32 thrown = 0;                                     // of them, those whose build threw
        U32 planned = 0;
        U32 built = 0;
        LLVKShader::Stats stats;
        Clock::time_point start = Clock::now();
        Clock::time_point last_done = Clock::now();
        U32 taken = 0;      // at the commit
        U32 missed = 0;
    };
    BackgroundBuild sBackground;
    bool sBackgroundCommit = false;     // main thread: between commitBegin and commitEnd
    GLTexts sBackgroundTexts;           // main thread, in the plan pass: the files the programs share

    // Made on first use and never destroyed (as the device); its threads end
    // at shutdown (stop_shader_prewarm), before the statics its jobs read go.
    LLVKShader::Workers* sBackgroundWorkers = nullptr;

    // LLRHI_ASYNC_RELOAD_THREADS=<n>, else half the reload's threads: at the
    // lowest priority they take what the frame leaves, and fewer of them keep
    // off the cores the main and recording threads share with them.
    U32 background_threads()
    {
        U64 wanted = 0;
        LLRHI::envNumber("LLRHI_ASYNC_RELOAD_THREADS", 0, ~(U64)0, wanted);
        return wanted > 0 ? (U32)llmin(wanted, (U64)64) : llmax(1u, LLVKShader::compileThreads() / 2);
    }

    LLVKShader::Workers& background_workers()
    {
        if (!sBackgroundWorkers)
        {
            sBackgroundWorkers = new LLVKShader::Workers("VKShaderBackground", background_threads(), true);
        }
        return *sBackgroundWorkers;
    }

    // Under sBackground.mutex: the plan goes (its builds, and those still
    // running when they end). The modules were never in a pipeline.
    void drop_background_plan()
    {
        ++sBackground.generation;
        for (auto& entry : sBackground.ready)
        {
            LLVKShader::destroy(entry.second.vk);
        }
        sBackground.ready.clear();
        sBackground.failed.clear();
        sBackground.thrown = 0;
        sBackground.planned = 0;
        sBackground.built = 0;
        sBackground.stats = LLVKShader::Stats();
    }

    // On a background thread: all a link makes of the sources (link_program,
    // then own_link's GL view), kept for the commit. Every job counts as done,
    // whatever happens in it: one that threw (Workers::run would drop it
    // uncounted, and the plan would never end) or did not build is a failure,
    // and what it made goes.
    void build_in_background(U32 generation, U64 key, const std::string& name, const GLProgram& program)
    {
        {
            std::lock_guard<std::mutex> lock(sBackground.mutex);
            if (generation != sBackground.generation)
            {
                return;     // dropped before it started
            }
        }
        LinkBuild link;
        const char* failure = nullptr;
        bool threw = false;
        try
        {
            link.sources = sources_of(program);
            prepare_gl_view(link.sources, true);    // before the rewrites change the sources (as link_planned)
            compile_link(0, link, LLVKShader::Modules::Create, name.c_str());
            failure = link.built ? nullptr : "did not build";
        }
        catch (const std::exception& e)
        {
            failure = "an exception";
            threw = true;
            LL_WARNS("VKShader") << "Background shader build of '" << name << "': an exception (" << e.what() << ")" << LL_ENDL;
        }
        catch (...)
        {
            failure = "an exception";
            threw = true;
            LL_WARNS("VKShader") << "Background shader build of '" << name << "': an unknown exception" << LL_ENDL;
        }
        const LLVKShader::Stats stats = LLVKShader::takeThreadStats();
        std::lock_guard<std::mutex> lock(sBackground.mutex);
        if (generation != sBackground.generation || failure)
        {
            LLVKShader::destroy(link.vk);
        }
        if (generation != sBackground.generation)
        {
            return;
        }
        add_build_counts(sBackground.stats, stats);
        ++sBackground.built;
        sBackground.last_done = Clock::now();
        if (failure)
        {
            sBackground.failed.push_back((name.empty() ? std::string("(unnamed)") : name) + " (" + failure + ")");
            sBackground.thrown += threw ? 1 : 0;
            return;
        }
        sBackground.ready.emplace(key, std::move(link));
    }


    class BackgroundBuilder final : public LLRHI::ShaderReloadBuilder
    {
    public:
        void begin() override
        {
            cancel();
            LLVKShader::Workers& workers = background_workers();
            workers.setBackground(true);
            // The prewarm's queue goes (it starts again after the commit). What it
            // compiles now stays in the background with these builds: only the
            // commit's links may wait for it (commitBegin raises it then).
            if (sPrewarm)
            {
                sPrewarm->cancel();
            }
            sBackgroundTexts.clear();
            std::lock_guard<std::mutex> lock(sBackground.mutex);
            sBackground.start = sBackground.last_done = Clock::now();
        }

        bool plan(U32 program, const char* name) override
        {
            GLProgram files;
            {
                std::lock_guard<LLVKTrackerMutex> lock(sLock);
                files = gl_program(program, sBackgroundTexts);
            }
            if (files.empty())
            {
                return false;
            }
            const U64 key = whole_sources_key(files);
            U32 generation = 0;
            {
                std::lock_guard<std::mutex> lock(sBackground.mutex);
                generation = sBackground.generation;
                ++sBackground.planned;
            }
            background_workers().post([generation, key, label = std::string(name ? name : ""), files = std::move(files)]()
            {
                build_in_background(generation, key, label, files);
            });
            return true;
        }

        void planned() override
        {
            sBackgroundTexts.clear();   // the jobs hold what they need
        }

        void progress(LLRHI::ShaderReloadProgress& out) override
        {
            out = LLRHI::ShaderReloadProgress();
            out.threads = sBackgroundWorkers ? sBackgroundWorkers->threads() : 0;
            std::lock_guard<std::mutex> lock(sBackground.mutex);
            out.planned = sBackground.planned;
            out.built = sBackground.built;
            out.failed = (U32)sBackground.failed.size();
            out.thrown = sBackground.thrown;
            out.compiled = sBackground.stats.compiled;
            out.cache_hits = sBackground.stats.cache_hits;
            out.bundle_hits = sBackground.stats.bundle_hits;
            out.compile_ms = sBackground.stats.compile_ms;
            const Clock::time_point end = out.built >= out.planned ? sBackground.last_done : Clock::now();
            out.seconds = std::chrono::duration<F64>(end - sBackground.start).count();
        }

        std::string failures() override
        {
            std::lock_guard<std::mutex> lock(sBackground.mutex);
            std::string names;
            for (const std::string& name : sBackground.failed)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            return names;
        }

        void commitBegin() override
        {
            prewarm_gives_way();        // a link of the commit may wait for what it compiles
            sBackgroundCommit = true;
            std::lock_guard<std::mutex> lock(sBackground.mutex);
            sBackground.taken = 0;
            sBackground.missed = 0;
        }

        void commitEnd(bool made) override
        {
            sBackgroundCommit = false;
            {
                std::lock_guard<std::mutex> lock(sBackground.mutex);
                if (made)
                {
                    LL_INFOS("VKShader") << "Background shader reload: the commit's links took " << sBackground.taken << " of "
                                         << sBackground.planned << " background builds, " << sBackground.missed
                                         << " were made then, " << sBackground.ready.size() << " builds left over (dropped); at the end "
                                         << LLVKDevice::instance().memoryReport() << LL_ENDL;
                }
                drop_background_plan();
            }
            prewarm_resumes();
            // The new programs' OIT variants, before an alpha draw asks for them (as a parallel reload).
            if (made && LLRHI::sShaderPrewarm && LLRHI::sOITSources)
            {
                start_prewarm(linked_programs(), std::string(), std::vector<std::string>());
            }
        }

        void cancel() override
        {
            if (sBackgroundWorkers)
            {
                sBackgroundWorkers->cancel();
                // What still runs finishes at normal priority: a reload made now may wait for one of its compiles.
                sBackgroundWorkers->setBackground(false);
            }
            prewarm_resumes();
            std::lock_guard<std::mutex> lock(sBackground.mutex);
            drop_background_plan();
        }
    };
    BackgroundBuilder sBackgroundBuilder;
} // namespace

    bool link_planned(GLuint id)
    {
        if (!sPlan)
        {
            return false;
        }
        GLProgram program = gl_program(id, sPlanTexts);
        if (program.empty())
        {
            return false;
        }
        ++sPlanned;
        sPlan->post([program = std::move(program)]()
        {
            LinkBuild link;
            link.sources = sources_of(program);
            prepare_gl_view(link.sources, true);    // research/117 G3, before the rewrites change the sources
            compile_link(0, link, LLVKShader::Modules::Skip);
        });
        return true;
    }

    bool link_planning()
    {
        return sPlan != nullptr;
    }

    bool parallel_reload_running()
    {
        return sParallelReload;
    }

    bool take_background_build(LinkBuild& link)
    {
        if (!sBackgroundCommit)
        {
            return false;
        }
        const U64 key = whole_sources_key(link.sources);
        std::lock_guard<std::mutex> lock(sBackground.mutex);
        auto it = sBackground.ready.find(key);
        if (it == sBackground.ready.end())
        {
            ++sBackground.missed;
            return false;
        }
        link = std::move(it->second);
        sBackground.ready.erase(it);
        ++sBackground.taken;
        return true;
    }

    void note_oit_variant(U64 main_fragment)
    {
        if (!main_fragment)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(sOITMutex);
        load_oit_list();
        if (!sOITPrograms.insert(main_fragment).second)
        {
            return;
        }
        const std::string path = oit_list_path();
        if (!path.empty())
        {
            llofstream out(path, std::ios::app);
            out << std::hex << main_fragment << '\n';
        }
    }

    void install_link_hooks()
    {
        LLRHI::sParallelShaderReload = &parallel_shader_reload;
        LLRHI::sPrewarmShaders = &prewarm_shaders;
        LLRHI::sBakeShaders = &bake_shaders;    // research/159 3.1
        LLRHI::sShaderReloadBuilder = &sBackgroundBuilder;  // research/250
    }

    void stop_shader_prewarm()
    {
        // The object stays (as the device): a planning job still running
        // posts to it until it is joined, and those posts are dropped.
        if (sPrewarm)
        {
            sPrewarm->stop();
        }
        // research/250: the background builds too, and what they made goes
        // while the device is there (no module left for the validation layers).
        if (sBackgroundWorkers)
        {
            sBackgroundWorkers->stop();
        }
        LLRHI::sShaderReloadBuilder = nullptr;
        std::lock_guard<std::mutex> lock(sBackground.mutex);
        drop_background_plan();
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
