/**
 * @file llvkgl_glsl.cpp
 * @brief GLSL rewrites of the GL programs for Vulkan: tokens, draw rows, early outs (research/91).
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

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
namespace
{
    // research/59: the blended material shader's early out also takes alpha
    // below 0.004 (not exact; chosen 2026-10-11). On unless
    // LLRHI_MATERIAL_CUTOFF=0; needs LLRHI_MATERIAL_FAST.
    const bool sMaterialCutoff = LLRHI::envDefaultOn("LLRHI_MATERIAL_CUTOFF");
} // namespace

    // GLSL source as tokens (GlslToken, llvkgl_internal.h).
    void tokenize_glsl(const std::string& text, std::vector<GlslToken>& tokens)
    {
        tokens.clear();
        const size_t n = text.size();
        auto ident_char = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
        for (size_t i = 0; i < n; )
        {
            const char c = text[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                ++i;
            }
            else if (c == '/' && i + 1 < n && text[i + 1] == '/')
            {
                i = text.find('\n', i);
                i = i == std::string::npos ? n : i + 1;
            }
            else if (c == '/' && i + 1 < n && text[i + 1] == '*')
            {
                i = text.find("*/", i + 2);
                i = i == std::string::npos ? n : i + 2;
            }
            else if (ident_char(c))
            {
                size_t j = i + 1;
                while (j < n && ident_char(text[j]))
                {
                    ++j;
                }
                tokens.push_back({ i, j, true });
                i = j;
            }
            else
            {
                tokens.push_back({ i, i + 1, false });
                ++i;
            }
        }
    }

    // Stage G4b of research/46: a run of draws goes out as one indirect draw,
    // so what each draw pushes (its slot numbers, matrix row, palette and
    // material rows) must come from somewhere the draw itself names. For a
    // program the viewer marked (`#define LLRHI_DRAW_ROWS`), every read of
    // llrhi_slots.X that bindless_rewrite made becomes llrhi_dw(word, X): with
    // the rows word pushed as NO_TABLE (every draw outside a run) it is the
    // push constant as before; otherwise the word of the draw's row in the
    // palette table (set 0 binding 2), the row's first vec4 being the draw's
    // first instance (VkDrawIndexedIndirectCommand::firstInstance, so
    // gl_InstanceIndex in the vertex shader; the fragment shader gets it flat).
    // The row holds the words the push constants would (run_draw), so the
    // values are the same. The vertex main is renamed and called by a main
    // that passes the row on. Only vertex and fragment stages, one vertex
    // main, no instancing of its own, and room for one more push constant.
    // samplers / matrix / palette / light / material / consts: what
    // bindless_rewrite pushes, in its order. Returns whether the sources were rewritten.
    bool draw_rows_rewrite(std::vector<LLVKShader::Source>& sources, U32 samplers, bool matrix, bool palette, bool light,
                           bool material, bool consts)
    {
        const U32 words = samplers + (matrix ? 1 : 0) + (palette ? 1 : 0) + (light ? 1 : 0) + (material ? 1 : 0) + (consts ? 1 : 0);
        if (light || !words || words + 1 > LLVKBindless::MAX_PUSHED_SLOTS)
        {
            return false;
        }
        static const std::string BLOCK_END = " } llrhi_slots;\n";
        static const std::string ROWS_BLOCK_END = " uint rows; } llrhi_slots;\n";
        bool marker = false;
        S32 vertex_main = -1;
        // Every `main` of `void main(` in the vertex file: one file may have
        // several, of which the preprocessor keeps one (pbropaqueV: IS_HUD).
        std::vector<std::pair<size_t, size_t>> mains;
        std::vector<GlslToken> tok;
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const LLVKShader::Source& source = sources[s];
            if (source.stage != LLVKShader::Stage::Vertex && source.stage != LLVKShader::Stage::Fragment)
            {
                return false;
            }
            const std::string& text = source.text;
            tokenize_glsl(text, tok);
            auto is = [&](size_t k, const char* word)
            {
                const size_t len = strlen(word);
                return k < tok.size() && tok[k].end - tok[k].begin == len && text.compare(tok[k].begin, len, word) == 0;
            };
            for (size_t k = 0; k < tok.size(); ++k)
            {
                if (is(k, "#") && is(k + 1, "define") && is(k + 2, "LLRHI_DRAW_ROWS"))
                {
                    marker = true;
                }
                else if (is(k, "gl_InstanceID") || is(k, "gl_InstanceIndex") || is(k, "llrhi_draw_row") || is(k, "llrhi_drow") ||
                         is(k, "llrhi_dw") || is(k, "llrhi_draw_main"))
                {
                    return false;   // instancing of its own, or the names below taken
                }
                else if (source.stage == LLVKShader::Stage::Vertex && is(k, "void") && is(k + 1, "main") && is(k + 2, "("))
                {
                    if (vertex_main >= 0 && vertex_main != (S32)s)
                    {
                        return false;   // mains in two files
                    }
                    vertex_main = (S32)s;
                    mains.emplace_back(tok[k + 1].begin, tok[k + 1].end);
                }
            }
        }
        if (!marker || vertex_main < 0 || sources[(size_t)vertex_main].text.find(BLOCK_END) == std::string::npos)
        {
            return false;
        }

        std::vector<std::string> texts(sources.size());
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const bool vertex = sources[s].stage == LLVKShader::Stage::Vertex;
            std::string text = sources[s].text;
            if (text.find(BLOCK_END) == std::string::npos)
            {
                continue;   // reads no push constants
            }
            if ((S32)s == vertex_main)
            {
                for (auto it = mains.rbegin(); it != mains.rend(); ++it)     // from the back: positions stay valid
                {
                    text.replace(it->first, it->second - it->first, "llrhi_draw_main");
                }
            }
            // llrhi_slots.X -> llrhi_dw(word, llrhi_slots.X); a sampler's slot also
            // nonuniformEXT (each draw of an indirect draw is an invocation group
            // of its own, so it is uniform per draw anyway; this keeps it right
            // on any implementation).
            std::string out;
            size_t pos = 0;
            for (size_t at = text.find("llrhi_slots.", pos); at != std::string::npos; at = text.find("llrhi_slots.", pos))
            {
                out.append(text, pos, at - pos);
                const size_t member = at + 12;
                size_t end = member;
                while (end < text.size() && ((text[end] >= 'a' && text[end] <= 'z') || (text[end] >= 'A' && text[end] <= 'Z') ||
                                             (text[end] >= '0' && text[end] <= '9') || text[end] == '_'))
                {
                    ++end;
                }
                const std::string name = text.substr(member, end - member);
                U32 word = 0;
                if (name == "slot")
                {
                    const size_t close = end < text.size() && text[end] == '[' ? text.find(']', end) : std::string::npos;
                    const std::string index = close != std::string::npos ? text.substr(end + 1, close - end - 1) : std::string();
                    if (index.empty() || index.size() > 2 || index.find_first_not_of("0123456789") != std::string::npos)
                    {
                        return false;
                    }
                    word = (U32)atoi(index.c_str());
                    if (word >= samplers)
                    {
                        return false;
                    }
                    end = close + 1;
                }
                else if (name == "matrix" && matrix)
                {
                    word = samplers;
                }
                else if (name == "palette" && palette)
                {
                    word = samplers + (matrix ? 1 : 0);
                }
                else if (name == "material" && material)
                {
                    word = samplers + (matrix ? 1 : 0) + (palette ? 1 : 0);
                }
                else if (name == "consts" && consts)
                {
                    word = samplers + (matrix ? 1 : 0) + (palette ? 1 : 0) + (material ? 1 : 0);
                }
                else
                {
                    return false;
                }
                const std::string read = "llrhi_dw(" + std::to_string(word) + "u, " + text.substr(at, end - at) + ")";
                const bool sampler = out.size() >= 15 && out.compare(out.size() - 15, 15, "llrhi_textures[") == 0;
                out += sampler ? "nonuniformEXT(" + read + ")" : read;
                pos = end;
            }
            out.append(text, pos, std::string::npos);
            text.swap(out);

            // The rows word, and the row's reads after the block.
            const size_t block = text.find(BLOCK_END);
            if (block == std::string::npos)
            {
                return false;
            }
            text.replace(block, BLOCK_END.size(), ROWS_BLOCK_END);
            std::string declarations;
            if (text.find("llrhi_palette_table") == std::string::npos)
            {
                declarations += "layout(std430, set = 0, binding = 2) readonly buffer llrhi_palette_table { vec4 v[]; } llrhi_pal;\n";
            }
            if (vertex)
            {
                declarations += "#define llrhi_drow (llrhi_slots.rows != 0xffffffffu ? uint(gl_InstanceIndex) : 0xffffffffu)\n";
            }
            else
            {
                declarations += "flat in uint llrhi_draw_row;\n"
                                "#define llrhi_drow llrhi_draw_row\n";
            }
            // NO_TABLE reads row 0 on the side not taken, so nothing outside the
            // table is read even if both sides of the selection are evaluated.
            declarations += "#define llrhi_dw(w, pc) (llrhi_drow != 0xffffffffu ? "
                            "floatBitsToUint(llrhi_pal.v[(llrhi_drow != 0xffffffffu ? llrhi_drow : 0u) + ((w) >> 2u)][(w) & 3u]) : (pc))\n";
            text.insert(block + ROWS_BLOCK_END.size(), declarations);
            if (text.find("GL_ARB_shader_storage_buffer_object") == std::string::npos)
            {
                static const std::string NONUNIFORM = "#extension GL_EXT_nonuniform_qualifier : require\n";
                const size_t ext = text.find(NONUNIFORM);
                if (ext == std::string::npos || ext > block)
                {
                    return false;
                }
                text.insert(ext + NONUNIFORM.size(), "#extension GL_ARB_shader_storage_buffer_object : require\n");
            }
            if ((S32)s == vertex_main)
            {
                text += "\nflat out uint llrhi_draw_row;\n"
                        "void main()\n"
                        "{\n"
                        "    llrhi_draw_row = llrhi_drow;\n"
                        "    llrhi_draw_main();\n"
                        "}\n";
            }
            texts[s].swap(text);
        }
        for (size_t s = 0; s < sources.size(); ++s)
        {
            if (!texts[s].empty())
            {
                sources[s].text.swap(texts[s]);
            }
        }
        return true;
    }

    // The viewer marked the program for draw runs (`#define LLRHI_DRAW_ROWS`).
    bool draw_runs_marked(const std::vector<LLVKShader::Source>& sources)
    {
        std::vector<GlslToken> tok;
        for (const LLVKShader::Source& source : sources)
        {
            const std::string& text = source.text;
            if (text.find("LLRHI_DRAW_ROWS") == std::string::npos)
            {
                continue;
            }
            tokenize_glsl(text, tok);
            for (size_t k = 0; k + 2 < tok.size(); ++k)
            {
                auto is = [&](size_t i, const char* word)
                {
                    const size_t len = strlen(word);
                    return tok[i].end - tok[i].begin == len && text.compare(tok[i].begin, len, word) == 0;
                };
                if (is(k, "#") && is(k + 1, "define") && is(k + 2, "LLRHI_DRAW_ROWS"))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // research/54, 59: a fragment file with code for one of the early outs gets
    // its define (and the extensions its code uses) after its #version line
    // (before what bindless_rewrite puts after the last #version / #extension
    // line), while its switch is on: LLRHI_ALPHA_FAST (alphaF.glsl,
    // LLRHI::sAlphaFast), LLRHI_MATERIAL_FAST (materialF.glsl,
    // LLRHI::sMaterialFast) and LLRHI_PROBE_FUSE (class3 reflectionProbeF.glsl,
    // no extensions; LLRHI::sProbeFuse). GL never sees the defines. Returns
    // whether a file got one.
    bool alpha_fast_rewrite(std::vector<LLVKShader::Source>& sources)
    {
        static const std::string EXTENSIONS = "#extension GL_EXT_terminate_invocation : require\n"
                                              "#extension GL_EXT_demote_to_helper_invocation : require\n"
                                              "#extension GL_KHR_shader_subgroup_quad : require\n";
        struct EarlyOut
        {
            const char* name;
            const bool* on;
            bool extensions;
        };
        static const EarlyOut EARLY_OUTS[] = {
            { "LLRHI_ALPHA_FAST", &LLRHI::sAlphaFast, true },
            { "LLRHI_MATERIAL_FAST", &LLRHI::sMaterialFast, true },
            { "LLRHI_PROBE_FUSE", &LLRHI::sProbeFuse, false },
            // With LLRHI_MATERIAL_FAST only (the code is inside its block).
            { "LLRHI_MATERIAL_CUTOFF", &sMaterialCutoff, false },
        };
        bool done = false;
        for (LLVKShader::Source& source : sources)
        {
            std::string& text = source.text;
            if (source.stage != LLVKShader::Stage::Fragment)
            {
                continue;
            }
            std::string inject;
            bool extensions = false;
            for (const EarlyOut& early_out : EARLY_OUTS)
            {
                if (*early_out.on && text.find(early_out.name) != std::string::npos)
                {
                    inject += std::string("#define ") + early_out.name + " 1\n";
                    extensions = extensions || early_out.extensions;
                }
            }
            if (inject.empty())
            {
                continue;
            }
            if (extensions)
            {
                inject = EXTENSIONS + inject;
            }
            const size_t version = text.compare(0, 8, "#version") == 0 ? 0 : text.find("\n#version");
            const size_t line_end = version == std::string::npos ? std::string::npos : text.find('\n', version + 1);
            if (line_end != std::string::npos)
            {
                text.insert(line_end + 1, inject);
                done = true;
            }
        }
        return done;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
