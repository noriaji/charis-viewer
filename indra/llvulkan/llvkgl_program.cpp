/**
 * @file llvkgl_program.cpp
 * @brief GL programs on Vulkan: the link (sources, layouts, sampler slots), the shared pipeline layout, uniform writes (research/91).
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

#include "lldir.h"

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // ------------------------------------------------------------------
    // Programs

    LLVKBindless::DrawMatrices identity_matrices()
    {
        LLVKBindless::DrawMatrices m = {};
        for (U32 i = 0; i < 4; ++i)
        {
            m.modelview[i * 5] = m.modelview_projection[i * 5] = m.texture0[i * 5] = 1.f;
        }
        m.normal[0] = m.normal[5] = m.normal[10] = 1.f;
        return m;
    }
    std::unordered_map<GLuint, std::pair<GLenum, std::string>> sShaderSources;
    std::unordered_map<GLuint, std::vector<GLuint>> sAttached;
    std::unordered_map<GLuint, Program> sPrograms;
    thread_local GLuint sCurrentProgram = 0;
    U32 sProgramChanges = 0;        // a program was linked or deleted (cached Program* may be stale)
    thread_local Program* sCurrentResolved = nullptr;
    thread_local GLuint sCurrentResolvedName = 0;
    thread_local U32 sCurrentResolvedAt = U32_MAX;

    // The bound program, or null when it does not exist.
    Program* current_program()
    {
        if (sCurrentResolvedName != sCurrentProgram || sCurrentResolvedAt != sProgramChanges)
        {
            auto it = sPrograms.find(sCurrentProgram);
            sCurrentResolved = it != sPrograms.end() ? &it->second : nullptr;
            sCurrentResolvedName = sCurrentProgram;
            sCurrentResolvedAt = sProgramChanges;
        }
        return sCurrentResolved;
    }

    bool is_sampler_type(U32 type)
    {
        switch (type)
        {
        case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_3D: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_SAMPLER_2D_RECT: case GL_SAMPLER_2D_MULTISAMPLE: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_SAMPLER_CUBE_SHADOW:
            return true;
        default:
            return false;
        }
    }
namespace
{

    VkImageViewType view_type_for(U32 sampler_type, GLenum& target)
    {
        switch (sampler_type)
        {
        case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW:
            target = GL_TEXTURE_CUBE_MAP; return VK_IMAGE_VIEW_TYPE_CUBE;
        case GL_SAMPLER_CUBE_MAP_ARRAY: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
            target = GL_TEXTURE_CUBE_MAP_ARRAY; return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW:
            target = GL_TEXTURE_2D_ARRAY; return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        default:
            target = GL_TEXTURE_2D; return VK_IMAGE_VIEW_TYPE_2D;
        }
    }

    bool is_shadow_sampler(U32 type)
    {
        return type == GL_SAMPLER_2D_SHADOW || type == GL_SAMPLER_CUBE_SHADOW ||
               type == GL_SAMPLER_2D_ARRAY_SHADOW || type == GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW;
    }

} // namespace
    void destroy_program_objects(Program& program)
    {
        VkDescriptorSetLayout set_layout = program.set_layout;
        VkPipelineLayout layout = program.shared_layout ? VK_NULL_HANDLE : program.layout;     // sSharedLayout stays
        LLVKShader::Program vk = program.vk;
        std::shared_ptr<OITVariant> oit = program.oit;      // research/41: made with this link's sources and set layout
        // fix-pipeleak: this link's pipelines (its OIT variant's too) leave the
        // table now, so nothing binds them again; they are destroyed with the
        // rest once the frames that may have bound them are done. Under sLock.
        std::vector<VkPipeline> pipelines;
        pipelines.reserve(program.pipeline_keys.size());
        for (const std::vector<U32>* key : program.pipeline_keys)
        {
            auto it = sPipelines.find(*key);
            if (it != sPipelines.end())
            {
                if (it->second)
                {
                    pipelines.push_back(it->second);
                }
                sPipelines.erase(it);
            }
        }
        program.pipeline_keys.clear();
        LLVKMirror::instance().defer([set_layout, layout, vk, oit, pipelines = std::move(pipelines)]() mutable
        {
            retire_pipeline_objects();      // research/111 B-PL
            VkDevice device = LLVKDevice::instance().getDevice();
            for (VkPipeline pipeline : pipelines)
            {
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (oit)
            {
                if (oit->layout)
                {
                    vkDestroyPipelineLayout(device, oit->layout, nullptr);
                }
                LLVKShader::destroy(oit->vk);
            }
            if (layout)
            {
                vkDestroyPipelineLayout(device, layout, nullptr);
            }
            if (set_layout)
            {
                vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            }
            LLVKShader::destroy(vk);
        });
        program.vk = LLVKShader::Program();
        program.set_layout = VK_NULL_HANDLE;
        program.layout = VK_NULL_HANDLE;
        program.shared_layout = false;
        program.built = false;
        program.oit.reset();
        program.oit_sources.clear();
        program.last_pipeline = VK_NULL_HANDLE;     // one of those just taken out
        program.last_pipeline_oit = 0;
        // The sets cached this frame were written for the old set layout.
        program.last_frame = U32_MAX;
        program.set_cache_frame = U32_MAX;
        program.persist_generation = U32_MAX;   // research/156 A-DS: the kept ones too
        program.images_frame = U32_MAX;         // the image list was made for the old sampler slots
    }
namespace
{

    // The pipeline layout of the programs on the shared layout: set 0 (the
    // bindless table and the matrix and palette tables) and the push
    // constants as in every translated layout, and no set 1. One object for
    // all of them (pipelines of different programs are layout-compatible as
    // a whole, as device generated commands need). Made once, never destroyed.
    VkPipelineLayout sSharedLayout = VK_NULL_HANDLE;

    VkPipelineLayout shared_pipeline_layout()
    {
        if (!sSharedLayout)
        {
            const VkDescriptorSetLayout set_layout = LLVKBindless::layout();
            const VkPushConstantRange push_range = LLVKBindless::pushConstantRange();
            VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            info.setLayoutCount = 1;
            info.pSetLayouts = &set_layout;
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &push_range;
            if (vkCreatePipelineLayout(LLVKDevice::instance().getDevice(), &info, nullptr, &sSharedLayout) != VK_SUCCESS)
            {
                sSharedLayout = VK_NULL_HANDLE;
            }
        }
        return sSharedLayout;
    }

} // namespace
    // Programs on the shared layout (perfReport).
    U32 shared_layout_programs()
    {
        U32 count = 0;
        for (const auto& program : sPrograms)
        {
            count += program.second.shared_layout ? 1 : 0;
        }
        return count;
    }

    // LLRHI::sDrawRunPrograms: the linked programs marked for draw runs, with
    // draw rows, and on the shared layout (logged after the shaders load).
    void draw_run_programs(U32& marked, U32& rows, U32& shared)
    {
        // research/117 8.6: what GL's view of the programs cost this load
        // (the shaders are loaded once this is asked: LLViewerShaderMgr::setShaders).
        const LLVKShader::GLViewStats views = LLVKShader::takeGLViewStats();
        prune_gl_views();   // fix-pipeleak: the reload is done; the views of the programs it deleted go
        if (views.views)
        {
            LL_INFOS("VKShader") << "GL view of " << views.views << " programs (research/117): " << views.cache_hits
                                 << " from the disk cache, " << views.bundle_hits << " from the bundle; " << views.total_ms / 1000.0
                                 << " s in all, of which worked out: parse, link and reflection " << views.front_ms / 1000.0
                                 << " s, SPIR-V and dead code elimination " << views.optimize_ms / 1000.0 << " s" << LL_ENDL;
        }
        // research/159 section 2.1: what the load built and where from (the
        // start's load among them, which had no line). A parallel reload
        // writes its own line.
        if (!parallel_reload_running())
        {
            const LLVKShader::Stats builds = LLVKShader::takeStats();
            if (builds.builds)
            {
                LL_INFOS("VKShader") << "Shader load: " << builds.builds << " builds in " << builds.span_ms / 1000.0
                                     << " s (from the first one to the load's end), " << builds.compiled << " compiled ("
                                     << builds.compile_ms / 1000.0 << " s of compiling), " << builds.cache_hits << " from the cache, "
                                     << builds.bundle_hits << " from the bundle, " << builds.waited << " waited for another thread ("
                                     << builds.wait_ms / 1000.0 << " s)" << LL_ENDL;
            }
        }
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        marked = rows = shared = 0;
        for (const auto& program : sPrograms)
        {
            marked += program.second.draw_runs ? 1 : 0;
            rows += program.second.draw_rows ? 1 : 0;
            shared += program.second.shared_layout ? 1 : 0;
        }
    }
namespace
{

    U32 sLinkSerials = 0;

} // namespace
    void link_program(GLuint id)
    {
        ++sProgramChanges;
        Program& program = sPrograms[id];
        if (program.built)
        {
            destroy_program_objects(program);
        }
        // A new link never meets the pipelines of an earlier one (same GL name,
        // same shared layout, other shader code).
        program.link_serial = ++sLinkSerials;
        program.failed = false;
        program.locations.clear();
        program.native_slots.clear();
        program.native_slot_of.clear();
        program.sampler_slots.clear();
        program.unit_values.clear();
        program.image_count = 0;
        program.last_frame = U32_MAX;
        program.ubo_frame = U32_MAX;
        program.depth_only_mvp = -2;
        ++program.uniform_version;
        ++program.unit_version;
        program.buffer_blocks = false;

        // research/111 B-SC: the first pass of a parallel reload only starts the compile.
        if (link_planned(id))
        {
            return;
        }
        LinkBuild link;
        for (GLuint shader : sAttached[id])
        {
            auto it = sShaderSources.find(shader);
            if (it == sShaderSources.end())
            {
                continue;
            }
            link.sources.push_back({ shader_stage(it->second.first), it->second.second });
        }
        program.main_fragment = main_fragment_hash(link.sources);
        program.identity = program_identity(link.sources);
        // research/250: the background reload's commit takes what its build threads made of the same sources.
        if (!take_background_build(link))
        {
            compile_link(id, link, LLVKShader::Modules::Create);
        }
        const bool built = link.built;
        const bool light_table = link.light_table;
        const bool draw_rows = link.draw_rows;
        const bool shared = link.shared;
        const std::vector<LLVKShader::Source>& sources = link.sources;
        const std::vector<std::string>& bindless_names = link.bindless_names;
        std::vector<ConstMember>& consts = link.consts;
        program.vk = link.vk;
        program.matrix_mask = link.matrix_mask;
        program.palette_columns = link.palette_columns;
        program.palette_elements = link.palette_elements;
        program.light_table = built && light_table;
        program.draw_runs = built && draw_runs_marked(link.unrewritten);
        program.draw_rows = built && draw_rows;
        program.draw_row_words = program.draw_rows ? link.row_words + (consts.empty() ? 0 : 1) : 0;
        // GL starts uniforms at zero: so does the material.
        program.material_members = built ? link.materials : std::vector<MaterialMember>();
        program.material_vec4s = 0;
        for (const MaterialMember& member : program.material_members)
        {
            program.material_vec4s = llmax(program.material_vec4s, member.first + member.count);
        }
        program.material.assign((size_t)program.material_vec4s * 4, 0.f);
        program.material_version = 0;
        program.material_row_frame = U32_MAX;
        program.matrices = identity_matrices();
        program.matrix_version = 0;
        program.matrix_row_frame = U32_MAX;
        // GL starts uniforms at zero: the first copy takes the whole palette.
        program.palette.assign((size_t)program.palette_elements * program.palette_columns * 4, 0.f);
        program.palette_extent = program.palette_elements * program.palette_columns;
        program.palette_written = false;
        program.palette_version = 0;
        program.palette_row_frame = U32_MAX;
        program.palette_prebuilt = false;
        program.palette_prebuilt_frame = U32_MAX;
        // GL starts uniforms at zero: so do the constants.
        program.consts_members = built ? consts : std::vector<ConstMember>();
        program.consts_vec4s = 0;
        for (const ConstMember& member : program.consts_members)
        {
            program.consts_vec4s = llmax(program.consts_vec4s, member.first + member.count * member.type->columns);
        }
        program.consts.assign((size_t)program.consts_vec4s * 4, 0.f);
        program.consts_version = 0;
        program.consts_row_frame = U32_MAX;
        if (!built)
        {
            program.failed = true;
            if (!sources.empty())
            {
                LL_WARNS("VKGL") << "program " << id << " failed to build for Vulkan: " << program.vk.log << LL_ENDL;
                // Keep what was compiled, for looking at the failure.
                // In the viewer's cache folder (research/114), by a UTF-8 path.
                const std::string cache = gDirUtilp->getCacheDir();
                if (!cache.empty())
                {
                    const std::string dir = cache + gDirUtilp->getDirDelimiter() + "vk_shader_failed";
                    LLFile::mkdir(dir);
                    for (size_t s = 0; s < sources.size(); ++s)
                    {
                        llofstream out(dir + "/" + std::to_string(id) + "_" + std::to_string(s) + ".glsl");
                        out << sources[s].text;
                    }
                }
            }
            return;
        }
        if (LLRHI::sOITSources)
        {
            program.oit_sources = sources;  // research/41: its OIT variant is made from these (oit_variant)
        }

        // Vertex inputs -> GL attribute indices (LLShaderMgr binds them by name before linking).
        program.input_attribs.clear();
        for (const auto& input : program.vk.inputs)
        {
            auto it = program.attrib_bindings.find(input.name);
            program.input_attribs.push_back(it != program.attrib_bindings.end() ? (S32)it->second : -1);
        }

        // Descriptor layout: every uniform block and every sampler binding.
        std::map<S32, VkDescriptorSetLayoutBinding> bindings;
        program.default_block = -1;
        for (size_t i = 0; i < program.vk.blocks.size(); ++i)
        {
            const auto& block = program.vk.blocks[i];
            if (block.binding < 0)
            {
                continue;
            }
            VkDescriptorSetLayoutBinding b = {};
            b.binding = block.binding;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings[block.binding] = b;
            if (block.name == "gl_DefaultUniformBlock")
            {
                bindings[block.binding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                program.default_block = (S32)i;
                program.uniform_data.assign(block.size, 0);
            }
            else
            {
                program.buffer_blocks = true;
            }
        }
        for (const auto& uniform : program.vk.uniforms)
        {
            if (uniform.binding < 0 || !is_sampler_type(uniform.gl_type) || uniform.name.compare(0, 14, "llrhi_textures") == 0)
            {
                continue;
            }
            if (bindings.count(uniform.binding))
            {
                LL_WARNS("VKGL") << "program " << id << ": sampler " << uniform.name << " shares binding " << uniform.binding << LL_ENDL;
            }
            VkDescriptorSetLayoutBinding& b = bindings[uniform.binding];
            b.binding = uniform.binding;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = llmax(b.descriptorCount, (U32)llmax(1, uniform.size));
            b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        }
        for (const auto& uniform : program.vk.uniforms)
        {
            if (uniform.binding < 0 || !is_sampler_type(uniform.gl_type) || uniform.name.compare(0, 14, "llrhi_textures") == 0)
            {
                continue;
            }
            bool seen = false;
            for (const SamplerSlot& slot : program.sampler_slots)
            {
                seen |= slot.binding == uniform.binding;
            }
            if (seen)
            {
                continue;
            }
            SamplerSlot slot;
            slot.binding = uniform.binding;
            slot.count = (U32)llmax(1, uniform.size);
            slot.first = (U32)program.unit_values.size();
            slot.view_type = view_type_for(uniform.gl_type, slot.target);
            slot.shadow = is_shadow_sampler(uniform.gl_type);
            slot.name = uniform.name;
            program.unit_values.resize(program.unit_values.size() + slot.count, 0);
            program.image_count += slot.count;
            program.sampler_slots.push_back(slot);
        }

        program.bindless.clear();
        program.bindless_slots.clear();
        for (const std::string& name : bindless_names)
        {
            Program::BindlessSampler sampler;
            sampler.name = name;
            program.bindless.push_back(sampler);
        }

        program.block_point.assign(program.vk.blocks.size(), -1);
        for (size_t i = 0; i < program.vk.blocks.size(); ++i)
        {
            auto point = program.block_points.find(program.vk.blocks[i].name);
            if (point != program.block_points.end())
            {
                program.block_point[i] = (S32)point->second;
            }
        }
        program.images_frame = U32_MAX;
        program.last_pipeline = VK_NULL_HANDLE;

        program.bindings.clear();
        for (auto& b : bindings)
        {
            program.bindings.push_back(b.second);
        }
        // Every descriptor the SPIR-V uses must be in the layout: a miss only
        // shows when the program first draws, which may be far from login.
        for (const LLVKShader::Descriptor& d : program.vk.descriptors)
        {
            const bool table = d.set == 0 && ((d.binding == 0 && d.name == "llrhi_textures") || (d.binding == 1 && d.name == "llrhi_mt") ||
                                              (d.binding == 2 && d.name == "llrhi_pal"));
            if (!table && (d.set != 1 || !bindings.count(d.binding)))
            {
                LL_WARNS("VKGL") << "program " << id << ": " << d.name << " (set " << d.set << ", binding " << d.binding
                                 << ") is not in its pipeline layout" << LL_ENDL;
            }
        }

        // On the shared layout (research/50 step 3): nothing in set 1, so no
        // set layout of its own and sSharedLayout.
        program.shared_layout = shared && program.bindings.empty() && shared_pipeline_layout() != VK_NULL_HANDLE;
        if (program.shared_layout)
        {
            program.set_layout = VK_NULL_HANDLE;
            program.layout = sSharedLayout;
            program.built = true;
            return;
        }

        VkDevice device = LLVKDevice::instance().getDevice();
        VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_info.bindingCount = (U32)program.bindings.size();
        set_info.pBindings = program.bindings.data();
        program.set_layout = VK_NULL_HANDLE;
        program.layout = VK_NULL_HANDLE;
        const VkResult set_result = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &program.set_layout);
        if (set_result != VK_SUCCESS)
        {
            LL_WARNS("VKGL") << "program " << id << ": vkCreateDescriptorSetLayout failed (" << (S32)set_result << ")" << LL_ENDL;
            program.set_layout = VK_NULL_HANDLE;
            destroy_program_objects(program);       // the shader modules; built stays false
            program.failed = true;
            return;
        }

        // Set 0: bindless table, set 1: the program's own; the push constant range is
        // shared with the native passes so set 0 stays bound across them.
        VkDescriptorSetLayout set_layouts[2] = { LLVKBindless::layout(), program.set_layout };
        const VkPushConstantRange push_range = LLVKBindless::pushConstantRange();
        VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout_info.setLayoutCount = 2;
        layout_info.pSetLayouts = set_layouts;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
        const VkResult layout_result = vkCreatePipelineLayout(device, &layout_info, nullptr, &program.layout);
        if (layout_result != VK_SUCCESS)
        {
            LL_WARNS("VKGL") << "program " << id << ": vkCreatePipelineLayout failed (" << (S32)layout_result << ")" << LL_ENDL;
            program.layout = VK_NULL_HANDLE;
            destroy_program_objects(program);       // the set layout and shader modules; built stays false
            program.failed = true;
            return;
        }
        program.built = true;
    }

    // "name", "name[3]" -> reflected uniform + starting element.
    void record_location(GLuint id, const char* name, GLint location)
    {
        auto it = sPrograms.find(id);
        if (it == sPrograms.end() || !it->second.built || location < 0 || !name)
        {
            return;
        }
        Program& program = it->second;
        std::string base = name;
        S32 element = 0;
        size_t bracket = base.find('[');
        if (bracket != std::string::npos)
        {
            element = atoi(base.c_str() + bracket + 1);
            base = base.substr(0, bracket);
        }
        for (U32 kind = 0; kind < MATRIX_KINDS; ++kind)
        {
            if ((program.matrix_mask & (1u << kind)) && base == MATRIX_NAMES[kind])
            {
                program.locations[location] = { -1, 0, -1, (S32)kind };
                return;
            }
        }
        if (program.palette_columns && base == "matrixPalette")
        {
            program.locations[location] = { -1, element, -1, -1, true };
            return;
        }
        for (size_t i = 0; i < program.material_members.size(); ++i)
        {
            if (program.material_members[i].name == base)
            {
                program.locations[location] = { -1, element, -1, -1, false, (S32)i };
                return;
            }
        }
        for (size_t i = 0; i < program.consts_members.size(); ++i)
        {
            if (program.consts_members[i].name == base)
            {
                program.locations[location] = { -1, element, -1, -1, false, -1, (S32)i };
                return;
            }
        }
        for (size_t i = 0; i < program.vk.uniforms.size(); ++i)
        {
            const std::string& reflected = program.vk.uniforms[i].name;
            if (reflected == base || reflected == base + "[0]")
            {
                program.locations[location] = { (S32)i, element };
                return;
            }
        }
        for (size_t i = 0; i < program.bindless.size(); ++i)
        {
            if (program.bindless[i].name == base)
            {
                program.locations[location] = { -1, 0, (S32)i };
                return;
            }
        }
    }
namespace
{

    // A write to the skinning palette (glUniform4fv on vec4[], glUniformMatrix3x4fv
    // on mat3x4[]): `columns` vectors of `rows` floats per element. Kept with the
    // program; the next draw copies it into the palette table (palette_row).
    void apply_palette(Program& program, const UniformLocation& loc, GLsizei count, U32 columns, U32 rows, bool transpose, const F32* data)
    {
        const U32 per = program.palette_columns;
        if (columns != per || rows > 4 || loc.element < 0)
        {
            return;     // not how GL would set this palette
        }
        // The uniform is set again: the draws read `palette` from here on, not a prebuilt row.
        program.palette_prebuilt = false;
        const U64 stats_start = LLRHI::sAvatarStats ? __rdtsc() : 0;
        bool changed = false;
        U32 end = 0;
        for (GLsizei i = 0; i < count; ++i)
        {
            const U32 element = (U32)loc.element + (U32)i;
            if (element >= program.palette_elements)
            {
                break;
            }
            const F32* m = data + (size_t)i * columns * rows;
            for (U32 c = 0; c < columns; ++c)
            {
                F32 column[4];
                for (U32 r = 0; r < rows; ++r)
                {
                    column[r] = transpose ? m[r * columns + c] : m[c * rows + r];
                }
                F32* dst = &program.palette[((size_t)element * per + c) * 4];
                if (!same_words_at(dst, column, rows))
                {
                    memcpy(dst, column, rows * 4);
                    changed = true;
                }
            }
            end = (element + 1) * per;
        }
        if (end)
        {
            program.palette_extent = program.palette_written ? llmax(program.palette_extent, end) : end;
            program.palette_written = true;
        }
        if (changed)
        {
            ++program.palette_version;
        }
        if (stats_start)
        {
            ++sAvPaletteWrites;
            sAvPaletteWriteTicks += __rdtsc() - stats_start;
        }
    }

    // A write to a material uniform read from the material row: kept with the
    // program (one vec4 per element); the next draw copies it into the table
    // (material_row). The default block, and so its copy, stays as it was.
    void apply_material(Program& program, const UniformLocation& loc, GLsizei count, U32 components, const void* data)
    {
        if (loc.material < 0 || (size_t)loc.material >= program.material_members.size() || loc.element < 0)
        {
            return;
        }
        const MaterialMember& member = program.material_members[loc.material];
        const U32 words = llmin(components, 4u);
        bool changed = false;
        for (GLsizei i = 0; i < count; ++i)
        {
            const U32 element = (U32)loc.element + (U32)i;
            if (element >= member.count)
            {
                break;
            }
            F32* dst = &program.material[((size_t)member.first + element) * 4];
            const U8* src = (const U8*)data + (size_t)i * components * 4;
            if (!same_words_at(dst, src, words))
            {
                memcpy(dst, src, words * 4);
                changed = true;
            }
        }
        if (changed)
        {
            ++program.material_version;
        }
    }

    // A write to a uniform read from the constants row (research/50 step 3):
    // kept with the program as GL keeps it in the default block (each element
    // its columns' vec4s, the call's components at the start of each); the
    // next draw copies the row into the table (consts_row). columns: 0 for
    // glUniform*, else glUniformMatrix* (`columns` vectors of `rows` floats per
    // element, as apply_uniform_matrix writes the default block).
    void apply_consts(Program& program, const UniformLocation& loc, GLsizei count, U32 columns, U32 rows, bool transpose, const void* data)
    {
        if (loc.consts < 0 || (size_t)loc.consts >= program.consts_members.size() || loc.element < 0)
        {
            return;
        }
        const ConstMember& member = program.consts_members[loc.consts];
        const U32 per = member.type->columns;
        if (columns ? columns != per || rows > 4 : per != 1)
        {
            return;     // a matrix through glUniform*, or a vector through glUniformMatrix* (GL refuses both)
        }
        const U32 words = llmin(rows, 4u);
        bool changed = false;
        for (GLsizei i = 0; i < count; ++i)
        {
            const U32 element = (U32)loc.element + (U32)i;
            if (element >= member.count)
            {
                break;
            }
            for (U32 c = 0; c < per; ++c)
            {
                U32 column[4] = {};
                if (columns)
                {
                    const F32* m = (const F32*)data + (size_t)i * columns * rows;
                    for (U32 r = 0; r < rows; ++r)
                    {
                        const F32 v = transpose ? m[r * columns + c] : m[c * rows + r];
                        memcpy(&column[r], &v, 4);
                    }
                }
                else
                {
                    memcpy(column, (const U8*)data + (size_t)i * rows * 4, words * 4);
                }
                F32* dst = &program.consts[((size_t)member.first + (size_t)element * per + c) * 4];
                if (!same_words_at(dst, column, words))
                {
                    memcpy(dst, column, words * 4);
                    changed = true;
                }
            }
        }
        if (changed)
        {
            ++program.consts_version;
        }
    }

} // namespace
    // glUniform{1,2,3,4}{f,i,ui}v and the scalar forms (all 4-byte components),
    // on a built program: shared by the GL wrappers and the native shader API.
    void apply_uniform(Program& program, const UniformLocation& loc, GLsizei count, U32 components, const void* data)
    {
        if (loc.palette)
        {
            apply_palette(program, loc, count, 1, components, false, (const F32*)data);
            return;
        }
        if (loc.material >= 0)
        {
            apply_material(program, loc, count, components, data);
            return;
        }
        if (loc.consts >= 0)
        {
            apply_consts(program, loc, count, 0, components, false, data);
            return;
        }
        // Versions move only when a value changes: shaders set the same values
        // draw after draw, and every move costs a uniform copy and a pass cache miss.
        if (loc.bindless >= 0)
        {
            GLint& unit = program.bindless[loc.bindless].unit;
            if (unit != *(const GLint*)data)
            {
                unit = *(const GLint*)data;
                ++program.unit_version;
            }
            return;
        }
        if (loc.uniform < 0)
        {
            return;     // a table matrix set through a non-matrix call
        }
        const LLVKShader::Uniform& uniform = program.vk.uniforms[loc.uniform];

        if (uniform.binding >= 0 && is_sampler_type(uniform.gl_type))
        {
            const GLint* units = (const GLint*)data;
            bool changed = false;
            for (const SamplerSlot& slot : program.sampler_slots)
            {
                if (slot.binding != uniform.binding)
                {
                    continue;
                }
                for (GLsizei i = 0; i < count; ++i)
                {
                    U32 element = (U32)(loc.element + i);
                    if (element < slot.count && program.unit_values[slot.first + element] != units[i])
                    {
                        program.unit_values[slot.first + element] = units[i];
                        changed = true;
                    }
                }
            }
            if (changed)
            {
                ++program.unit_version;
            }
            return;
        }
        if (uniform.block != program.default_block || uniform.offset < 0)
        {
            return;
        }
        U32 stride = uniform.array_stride > 0 ? (U32)uniform.array_stride : components * 4;
        // As GL: elements past the declared array (or past a single value) are
        // dropped, not written over the uniforms after it.
        const U32 elements = (U32)llmax(uniform.size, 1);
        bool changed = false;
        for (GLsizei i = 0; i < count && (U32)(loc.element + i) < elements; ++i)
        {
            size_t offset = (size_t)uniform.offset + (size_t)(loc.element + i) * stride;
            if (offset + components * 4 > program.uniform_data.size())
            {
                break;
            }
            const U8* src = (const U8*)data + (size_t)i * components * 4;
            if (!same_words_at(&program.uniform_data[offset], src, components))
            {
                memcpy(&program.uniform_data[offset], src, components * 4);
                changed = true;
            }
        }
        if (changed)
        {
            ++program.uniform_version;
        }
    }

    // glUniformMatrix{2,3,4,3x4}fv: std140 columns are 16 bytes apart. On a
    // built program: shared by the GL wrappers and the native shader API.
    void apply_uniform_matrix(Program& program, const UniformLocation& loc, GLsizei count, U32 columns, U32 rows, GLboolean transpose, const GLfloat* data)
    {
        if (loc.bindless >= 0 || loc.material >= 0)
        {
            return;     // material uniforms are no matrices (GL would refuse this too)
        }
        if (loc.palette)
        {
            apply_palette(program, loc, count, columns, rows, transpose != GL_FALSE, data);
            return;
        }
        if (loc.consts >= 0)
        {
            apply_consts(program, loc, count, columns, rows, transpose != GL_FALSE, data);
            return;
        }
        // research/194 11: a texture scroll's description becomes its matrix
        // before it is kept and compared (the table row and the default block).
        F32 tex_anim[16];
        if (count == 1 && columns == 4 && rows == 4 && transpose == GL_FALSE)
        {
            data = LLRHI::texAnimMatrix(data, tex_anim);
        }
        if (loc.matrix >= 0)
        {
            // Kept with the program; a draw writes it into a table row.
            LLVKBindless::DrawMatrices& m = program.matrices;
            F32* dst = loc.matrix == MATRIX_MODELVIEW ? m.modelview : loc.matrix == MATRIX_MVP ? m.modelview_projection :
                       loc.matrix == MATRIX_TEXTURE0 ? m.texture0 : m.normal;
            const U32 dst_rows = 4;     // columns are 4 floats apart (the normal matrix too)
            bool changed = false;
            for (U32 c = 0; c < columns && c < 4; ++c)
            {
                for (U32 r = 0; r < rows && r < 4; ++r)
                {
                    const F32 value = transpose ? data[r * columns + c] : data[c * rows + r];
                    changed |= dst[c * dst_rows + r] != value;
                    dst[c * dst_rows + r] = value;
                }
            }
            if (changed)
            {
                ++program.matrix_version;
            }
            return;
        }
        if (loc.uniform < 0)
        {
            return;
        }
        const LLVKShader::Uniform& uniform = program.vk.uniforms[loc.uniform];
        if (uniform.block != program.default_block || uniform.offset < 0)
        {
            return;
        }
        U32 stride = uniform.array_stride > 0 ? (U32)uniform.array_stride : columns * 16;
        const U32 elements = (U32)llmax(uniform.size, 1);     // as apply_uniform: no writes past the array
        bool changed = false;
        for (GLsizei i = 0; i < count && (U32)(loc.element + i) < elements; ++i)
        {
            const GLfloat* m = data + (size_t)i * columns * rows;
            size_t base = (size_t)uniform.offset + (size_t)(loc.element + i) * stride;
            for (U32 c = 0; c < columns; ++c)
            {
                size_t offset = base + c * 16;
                if (offset + rows * 4 > program.uniform_data.size())
                {
                    break;
                }
                F32 column[4];
                for (U32 r = 0; r < rows; ++r)
                {
                    column[r] = transpose ? m[r * columns + c] : m[c * rows + r];
                }
                if (!same_words_at(&program.uniform_data[offset], column, rows))
                {
                    memcpy(&program.uniform_data[offset], column, rows * 4);
                    changed = true;
                }
            }
        }
        if (changed)
        {
            ++program.uniform_version;     // only when a value changed (see apply_uniform)
        }
    }

} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
