/**
 * @file llvkgl_glslbindless.cpp
 * @brief bindless_rewrite: GL programs read textures, matrices, palettes, lights, materials and constants from the frame's tables (research/91).
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
    // Per-draw material uniforms the shaders read from a row of the frame's
    // table (research/46 G4a) instead of the default uniform block: the
    // legacy material's, the alpha cutoff, and the GLTF / PBR factors and
    // texture transforms. Float scalars, vectors and their arrays only.
    const char* const MATERIAL_NAMES[] = { "minimum_alpha", "emissive_brightness", "env_intensity", "specular_color",
                                           "roughnessFactor", "metallicFactor", "emissiveColor",
                                           "texture_base_color_transform", "texture_normal_transform",
                                           "texture_metallic_roughness_transform", "texture_occlusion_transform",
                                           "texture_emissive_transform" };

    const ConstType CONST_TYPES[] = {
        { "float", 0x1406, CONST_FLOAT, 1, 1 }, { "vec2", 0x8B50, CONST_FLOAT, 2, 1 }, { "vec3", 0x8B51, CONST_FLOAT, 3, 1 },
        { "vec4", 0x8B52, CONST_FLOAT, 4, 1 },
        { "int", 0x1404, CONST_INT, 1, 1 }, { "ivec2", 0x8B53, CONST_INT, 2, 1 }, { "ivec3", 0x8B54, CONST_INT, 3, 1 },
        { "ivec4", 0x8B55, CONST_INT, 4, 1 },
        { "uint", 0x1405, CONST_UINT, 1, 1 }, { "uvec2", 0x8DC6, CONST_UINT, 2, 1 }, { "uvec3", 0x8DC7, CONST_UINT, 3, 1 },
        { "uvec4", 0x8DC8, CONST_UINT, 4, 1 },
        { "bool", 0x8B56, CONST_BOOL, 1, 1 }, { "bvec2", 0x8B57, CONST_BOOL, 2, 1 }, { "bvec3", 0x8B58, CONST_BOOL, 3, 1 },
        { "bvec4", 0x8B59, CONST_BOOL, 4, 1 },
        { "mat2", 0x8B5A, CONST_FLOAT, 2, 2 }, { "mat3", 0x8B5B, CONST_FLOAT, 3, 3 }, { "mat4", 0x8B5C, CONST_FLOAT, 4, 4 },
        { "mat2x2", 0x8B5A, CONST_FLOAT, 2, 2 }, { "mat3x3", 0x8B5B, CONST_FLOAT, 3, 3 }, { "mat4x4", 0x8B5C, CONST_FLOAT, 4, 4 },
        { "mat2x3", 0x8B65, CONST_FLOAT, 3, 2 }, { "mat2x4", 0x8B66, CONST_FLOAT, 4, 2 }, { "mat3x2", 0x8B67, CONST_FLOAT, 2, 3 },
        { "mat3x4", 0x8B68, CONST_FLOAT, 4, 3 }, { "mat4x2", 0x8B69, CONST_FLOAT, 2, 4 }, { "mat4x3", 0x8B6A, CONST_FLOAT, 3, 4 },
    };

    const char* const MATRIX_TYPES[MATRIX_KINDS] = { "mat4", "mat4", "mat4", "mat3" };
    const char* const MATRIX_MEMBERS[MATRIX_KINDS] = { "mv", "mvp", "tex0", "nrm" };

    // A library file of the point light program that declares some of the
    // light's uniforms (research/52: deferredUtil.glsl has `uniform vec3 color;
    // uniform float size;` for the spot lights). Its uses stay uniforms, which a
    // draw from the light table leaves at their last values, so the rewrite
    // holds only when none of them can run from the light's own file
    // (light_file, the stage's main). True when every function that names one
    // of them (a use, or a local or parameter of that name: conservative) and,
    // by name across the stage's files, every function that calls such a
    // function, are never named in light_file nor on a preprocessor line. A
    // name outside a function other than in its declaration or a prototype's
    // parameter refuses too.
    bool light_library_unreachable(const std::vector<LLVKShader::Source>& sources, const std::vector<std::vector<GlslToken>>& tokens,
                                   size_t library, const std::vector<std::string>& names,
                                   const std::vector<std::pair<size_t, size_t>>& declarations, size_t light_file)
    {
        const LLVKShader::Stage stage = sources[library].stage;
        auto text_of = [&](size_t s, size_t k)
        {
            return sources[s].text.substr(tokens[s][k].begin, tokens[s][k].end - tokens[s][k].begin);
        };
        auto is = [&](size_t s, size_t k, char c)
        {
            return k < tokens[s].size() && !tokens[s][k].ident && sources[s].text[tokens[s][k].begin] == c;
        };
        // On a preprocessor line, or a continuation line of one.
        auto on_directive = [&](size_t s, size_t k)
        {
            const std::string& text = sources[s].text;
            size_t pos = tokens[s][k].begin;
            for (;;)
            {
                const size_t newline = pos == 0 ? std::string::npos : text.rfind('\n', pos - 1);
                const size_t start = newline == std::string::npos ? 0 : newline + 1;
                const size_t first = text.find_first_not_of(" \t", start);
                if (first != std::string::npos && text[first] == '#')
                {
                    return true;
                }
                if (newline == std::string::npos)
                {
                    return false;
                }
                size_t end = newline;
                if (end > 0 && text[end - 1] == '\r')
                {
                    --end;
                }
                if (end == 0 || text[end - 1] != '\\')
                {
                    return false;
                }
                pos = end - 1;      // on the line it continues
            }
        };

        // Function definitions of the stage: tokens from the name to the closing brace.
        struct Function
        {
            std::string name;
            size_t file;
            size_t first;
            size_t last;
        };
        std::vector<Function> functions;
        std::vector<size_t> files;
        for (size_t s = 0; s < sources.size(); ++s)
        {
            if (sources[s].stage != stage)
            {
                continue;
            }
            files.push_back(s);
            const std::vector<GlslToken>& tok = tokens[s];
            S32 depth = 0;
            bool in_function = false;
            size_t name_token = 0;
            for (size_t k = 0; k < tok.size(); ++k)
            {
                if (is(s, k, '{'))
                {
                    if (depth == 0 && k > 0 && is(s, k - 1, ')'))
                    {
                        // `name ( ... ) {`
                        size_t open = k - 1;
                        bool found = false;
                        for (S32 nested = 0;; --open)
                        {
                            if (is(s, open, ')'))
                            {
                                ++nested;
                            }
                            else if (is(s, open, '(') && --nested == 0)
                            {
                                found = true;
                                break;
                            }
                            if (open == 0)
                            {
                                break;
                            }
                        }
                        if (found && open > 0 && tok[open - 1].ident)
                        {
                            in_function = true;
                            name_token = open - 1;
                        }
                    }
                    ++depth;
                }
                else if (is(s, k, '}'))
                {
                    if (--depth < 0)
                    {
                        return false;
                    }
                    if (depth == 0 && in_function)
                    {
                        functions.push_back({ text_of(s, name_token), s, name_token, k });
                        in_function = false;
                    }
                }
            }
            if (depth != 0)
            {
                return false;
            }
        }
        auto function_at = [&](size_t s, size_t k) -> const Function*
        {
            for (const Function& f : functions)
            {
                if (f.file == s && k >= f.first && k <= f.last)
                {
                    return &f;
                }
            }
            return nullptr;
        };

        // The functions that name the library's light uniforms.
        std::unordered_set<std::string> reach;
        const std::vector<GlslToken>& lib = tokens[library];
        for (size_t k = 0; k < lib.size(); ++k)
        {
            if (!lib[k].ident || std::find(names.begin(), names.end(), text_of(library, k)) == names.end())
            {
                continue;
            }
            bool declaration = false;
            for (const auto& d : declarations)
            {
                declaration |= k >= d.first && k <= d.second;
            }
            if (declaration || (k > 0 && is(library, k - 1, '.')))
            {
                continue;
            }
            if (on_directive(library, k))
            {
                return false;
            }
            if (const Function* f = function_at(library, k))
            {
                reach.insert(f->name);
                continue;
            }
            if (k > 0 && lib[k - 1].ident && (is(library, k + 1, ',') || is(library, k + 1, ')')))
            {
                continue;   // a prototype's parameter
            }
            return false;
        }

        // And every function that calls one of them, by name.
        for (bool grew = !reach.empty(); grew;)
        {
            grew = false;
            for (const Function& f : functions)
            {
                if (reach.count(f.name))
                {
                    continue;
                }
                for (size_t k = f.first + 1; k <= f.last; ++k)
                {
                    if (tokens[f.file][k].ident && reach.count(text_of(f.file, k)))
                    {
                        reach.insert(f.name);
                        grew = true;
                        break;
                    }
                }
            }
        }
        if (reach.empty())
        {
            return true;
        }

        // None of them named in the light's file, on a preprocessor line (a
        // macro could call it), or outside a function except as a prototype.
        for (size_t s : files)
        {
            const std::vector<GlslToken>& tok = tokens[s];
            for (size_t k = 0; k < tok.size(); ++k)
            {
                if (!tok[k].ident || !reach.count(text_of(s, k)))
                {
                    continue;
                }
                if (s == light_file || on_directive(s, k))
                {
                    return false;
                }
                if (!function_at(s, k) && !(k > 0 && tok[k - 1].ident && is(s, k + 1, '(')))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // The point light read from the light table (research/47), for
    // bindless_rewrite. The pipeline draws all point lights of a pass as one
    // instanced draw of 18 vertices: the box's triangle fan (8 indices from
    // get_box_fan_indices, picked by where the camera is) as the 6 triangles
    // the translation makes of a fan, (0, i, i + 1). Each instance's three
    // vec4s from table_base on: center and size, color and falloff, the fan's
    // first index. The vertex shader builds the corner from gl_VertexIndex
    // with the same fan table and corner positions as ll_create_cube_vb, so
    // the boxes, their triangles and their order are those of one draw per
    // light. Color, falloff and size reach the fragment shader flat (exact).
    // Draws without a table_base (NO_TABLE: one light at a time, GL draws)
    // read the vertex buffer and the uniforms as before. Reads for those use
    // row 0, so nothing outside the table is read even if both sides of a
    // selection are evaluated.
    enum LightCode { LIGHT_VERTEX_DECLARATIONS, LIGHT_VERTEX_MAIN, LIGHT_FRAGMENT_DECLARATIONS };

    std::string light_table_code(LightCode part)
    {
        switch (part)
        {
        case LIGHT_VERTEX_DECLARATIONS:
            return "flat out uint llrhi_light_on;\n"
                   "flat out vec4 llrhi_light_cf;\n"
                   "flat out float llrhi_light_s;\n"
                   "#define llrhi_light_table (llrhi_slots.light != 0xffffffffu)\n"
                   "#define llrhi_light_row (llrhi_light_table ? llrhi_slots.light + uint(gl_InstanceIndex) * 3u : 0u)\n"
                   // llvieweroctree.cpp sOcclusionIndices: 8 fans of 8 corners.
                   "const uint llrhi_box_fans[64] = uint[64](7u, 6u, 2u, 3u, 1u, 5u, 4u, 6u,  3u, 2u, 0u, 1u, 5u, 7u, 6u, 2u,\n"
                   "    5u, 4u, 6u, 7u, 3u, 1u, 0u, 4u,  1u, 0u, 4u, 5u, 7u, 3u, 2u, 0u,  6u, 0u, 2u, 3u, 7u, 5u, 4u, 0u,\n"
                   "    2u, 4u, 0u, 1u, 3u, 7u, 6u, 4u,  4u, 2u, 6u, 7u, 5u, 1u, 0u, 2u,  0u, 6u, 4u, 5u, 1u, 3u, 2u, 6u);\n"
                   // Triangle t = v / 3 of the fan: corners (0, t + 1, t + 2).
                   // Corner c of ll_create_cube_vb: x from bit 2, y bit 1, z bit 0.
                   "vec3 llrhi_light_corner()\n"
                   "{\n"
                   "    uint v = uint(gl_VertexIndex);\n"
                   "    uint t = v / 3u;\n"
                   "    uint k = v - t * 3u;\n"
                   "    uint c = llrhi_box_fans[min(uint(llrhi_pal.v[llrhi_light_row + 2u].x), 56u) + (k == 0u ? 0u : min(t + k, 7u))];\n"
                   "    return vec3((c & 4u) != 0u ? 1.0 : -1.0, (c & 2u) != 0u ? 1.0 : -1.0, (c & 1u) != 0u ? 1.0 : -1.0);\n"
                   "}\n"
                   "#define llrhi_light_position (llrhi_light_table ? llrhi_light_corner() : position)\n"
                   "#define llrhi_light_center (llrhi_light_table ? llrhi_pal.v[llrhi_light_row].xyz : center)\n"
                   "#define llrhi_light_size (llrhi_light_table ? llrhi_pal.v[llrhi_light_row].w : size)\n";
        case LIGHT_VERTEX_MAIN:
            // Appended to the vertex file; its own main is renamed llrhi_light_main.
            return "\nvoid main()\n"
                   "{\n"
                   "    llrhi_light_main();\n"
                   "    llrhi_light_on = llrhi_light_table ? 1u : 0u;\n"
                   "    llrhi_light_cf = llrhi_pal.v[llrhi_light_row + 1u];\n"
                   "    llrhi_light_s = llrhi_pal.v[llrhi_light_row].w;\n"
                   "}\n";
        case LIGHT_FRAGMENT_DECLARATIONS:
        default:
            return "flat in uint llrhi_light_on;\n"
                   "flat in vec4 llrhi_light_cf;\n"
                   "flat in float llrhi_light_s;\n"
                   "#define llrhi_light_color (llrhi_light_on != 0u ? llrhi_light_cf.xyz : color)\n"
                   "#define llrhi_light_falloff (llrhi_light_on != 0u ? llrhi_light_cf.w : falloff)\n"
                   "#define llrhi_light_size (llrhi_light_on != 0u ? llrhi_light_s : size)\n";
        }
    }

    // A constant's value in the constants row (research/50 step 3), as the
    // declared type: each column a vec4 of the row (its first `rows`
    // components), integers and booleans as the bits apply_consts stored (as
    // GL keeps them in the default block); an array as an array constructor.
    std::string const_value(const ConstMember& member)
    {
        const ConstType& type = *member.type;
        static const char* const SWIZZLE[5] = { "", ".x", ".xy", ".xyz", "" };
        auto column = [&](U32 vec4)
        {
            const std::string raw = "llrhi_pal.v[llrhi_slots.consts + " + std::to_string(vec4) + "u]" + SWIZZLE[type.rows];
            switch (type.kind)
            {
            case CONST_INT:
                return "floatBitsToInt(" + raw + ")";
            case CONST_UINT:
                return "floatBitsToUint(" + raw + ")";
            case CONST_BOOL:
                return type.rows == 1 ? "(floatBitsToUint(" + raw + ") != 0u)"
                                      : "notEqual(floatBitsToUint(" + raw + "), uvec" + std::to_string(type.rows) + "(0u))";
            case CONST_FLOAT:
            default:
                return raw;
            }
        };
        auto element = [&](U32 e)
        {
            const U32 first = member.first + e * type.columns;
            if (type.columns == 1)
            {
                return column(first);
            }
            std::string value = std::string(type.name) + "(";
            for (U32 c = 0; c < type.columns; ++c)
            {
                value += (c ? ", " : "") + column(first + c);
            }
            return value + ")";
        };
        if (!member.array)
        {
            return element(0);
        }
        std::string value = std::string(type.name) + "[" + std::to_string(member.count) + "](";
        for (U32 e = 0; e < member.count; ++e)
        {
            value += (e ? ", " : "") + element(e);
        }
        return value + ")";
    }
} // namespace

    // Rewrites `uniform sampler2D name;` to index the bindless table (set 0):
    // the slot number arrives in the default uniform block as llrhi_tex_<name>.
    // Samplers named *depth* stay descriptors: a depth buffer may be sampled
    // while attached read-only, which needs a layout the table does not use.
    // Returns the rewritten sampler names.
    // Rewrites the program's 2D samplers to index the bindless table, its
    // matrix uniforms (MATRIX_NAMES) to read the draw matrix table, and its
    // skinning palette (matrixPalette) to read the palette table; matrix_mask
    // gets a bit per rewritten matrix, palette_columns / palette_elements the
    // palette's shape (vec4s per element, elements; 0: not rewritten).
    // With allow_lights, the deferred point light (light_table: rewritten) as
    // well: see light_table_code below.
    // With allow_materials, its material uniforms (MATERIAL_NAMES) to read a
    // row of the same table (materials: their layout; empty: none rewritten).
    // With consts_wanted (research/50 step 3), those uniforms to read the
    // constants row of the same table (consts: the layout of the ones
    // rewritten; any other stays a uniform).
    // Returns the rewritten samplers.
    std::vector<std::string> bindless_rewrite(std::vector<LLVKShader::Source>& sources, U32& matrix_mask,
                                              U32& palette_columns, U32& palette_elements, bool allow_lights, bool& light_table,
                                              bool allow_materials, std::vector<MaterialMember>& materials,
                                              const std::vector<ConstWanted>& consts_wanted, std::vector<ConstMember>& consts)
    {
        std::vector<std::string> names;
        matrix_mask = 0;
        palette_columns = 0;
        palette_elements = 0;
        light_table = false;
        materials.clear();
        consts.clear();
        // LLRHI_BINDLESS=0 keeps every sampler a per-draw descriptor (for comparisons).
        static const bool enabled = !std::getenv("LLRHI_BINDLESS") || std::string(std::getenv("LLRHI_BINDLESS")) != "0";
        // LLRHI_MATRIX_TABLE=0 keeps the matrices in the default uniform block.
        static const bool matrices_enabled = enabled && !(std::getenv("LLRHI_MATRIX_TABLE") && *std::getenv("LLRHI_MATRIX_TABLE") == '0');
        // LLRHI_PALETTE_TABLE=0 keeps the skinning palette in the default uniform block.
        static const bool palette_enabled = enabled && !(std::getenv("LLRHI_PALETTE_TABLE") && *std::getenv("LLRHI_PALETTE_TABLE") == '0');
        // LLRHI_LIGHT_BATCH=0 leaves the point light program as it is (LLRHI::sLightBatch).
        const bool lights = enabled && LLRHI::sLightBatch && allow_lights;
        // LLRHI_MATERIAL_TABLE=0 keeps the material uniforms in the default uniform block.
        static const bool materials_enabled = enabled && !(std::getenv("LLRHI_MATERIAL_TABLE") && *std::getenv("LLRHI_MATERIAL_TABLE") == '0');
        const bool material_rewrite = materials_enabled && allow_materials;
        const bool consts_rewrite = !consts_wanted.empty();
        if (!enabled)
        {
            return names;
        }

        // The deferred point light (research/47): a program marked with
        // `#define LLRHI_LIGHT_TABLE` (LLViewerShaderMgr, gDeferredLightProgram)
        // whose vertex file declares `in vec3 position; uniform vec3 center;
        // uniform float size;` and fragment file `uniform vec3 color; uniform
        // float falloff; uniform float size;` (pointLightV / pointLightF).
        enum LightName { LIGHT_POSITION, LIGHT_CENTER, LIGHT_SIZE, LIGHT_COLOR, LIGHT_FALLOFF, LIGHT_NAMES };
        static const char* const LIGHT_NAME[LIGHT_NAMES] = { "position", "center", "size", "color", "falloff" };
        static const char* const LIGHT_TYPE[LIGHT_NAMES] = { "vec3", "vec3", "float", "vec3", "float" };
        static const char* const LIGHT_MACRO[LIGHT_NAMES] = { "llrhi_light_position", "llrhi_light_center", "llrhi_light_size",
                                                              "llrhi_light_color", "llrhi_light_falloff" };
        bool light_marker = false;
        std::vector<U32> light_declared(sources.size(), 0);     // LightName bits per file
        std::vector<std::vector<std::pair<size_t, size_t>>> light_declaration_tokens(sources.size());  // first, last token
        std::vector<size_t> main_token(sources.size(), 0);      // `main` of `void main(`; 0: none

        struct Declaration
        {
            std::string name;
            size_t begin;   // `uniform`
            size_t end;     // past `;`
        };
        std::vector<std::vector<GlslToken>> tokens(sources.size());
        std::vector<std::vector<Declaration>> declarations(sources.size());
        std::vector<std::vector<Declaration>> matrix_declarations(sources.size());
        // `uniform mat3x4 matrixPalette[N];` (rigged meshes) or `uniform vec4 matrixPalette[N];` (the system avatar).
        struct PaletteDeclaration
        {
            size_t begin;
            size_t end;
            size_t first_token;
            size_t last_token;
            U32 columns;
            std::string size;       // N: a number or a #define'd name
        };
        std::vector<std::vector<PaletteDeclaration>> palette_declarations(sources.size());
        // `uniform T name;`, `uniform T name[N];` or `uniform T[N] name;` with T
        // float / vec2 / vec3 / vec4 and the name one of MATERIAL_NAMES.
        struct MaterialDeclaration
        {
            std::string name;
            U32 components;
            std::string size;       // N: a number or a #define'd name; empty: not an array
            size_t begin;
            size_t end;
        };
        std::vector<std::vector<MaterialDeclaration>> material_declarations(sources.size());
        // The same forms with T one of CONST_TYPES and the name one of consts_wanted.
        struct ConstDeclaration
        {
            std::string name;
            const ConstType* type;
            std::string size;       // N: a number or a #define'd name; empty: not an array
            size_t begin;
            size_t end;
        };
        std::vector<std::vector<ConstDeclaration>> const_declarations(sources.size());
        // Identifiers on preprocessor lines: a constant's #define would change
        // what such a line tests or defines, so those names are not rewritten.
        std::vector<std::string> directive_names;
        std::unordered_map<std::string, std::string> numeric_defines;     // `#define NAME 110`
        std::vector<std::string> sampler_macros;    // M in `#define M(x) sampler2D x`
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const std::string& text = sources[s].text;
            std::vector<GlslToken>& tok = tokens[s];
            tokenize_glsl(text, tok);
            auto is = [&](size_t k, const char* word)
            {
                const size_t len = strlen(word);
                return k < tok.size() && tok[k].end - tok[k].begin == len && text.compare(tok[k].begin, len, word) == 0;
            };
            auto word = [&](size_t k) { return text.substr(tok[k].begin, tok[k].end - tok[k].begin); };
            for (size_t k = 0; k < tok.size(); ++k)
            {
                if (consts_rewrite && is(k, "#"))
                {
                    const size_t line_start = text.rfind('\n', tok[k].begin);
                    if (text.find_first_not_of(" \t\r", line_start == std::string::npos ? 0 : line_start + 1) == tok[k].begin)
                    {
                        // The directive's line, with its continuation lines.
                        size_t line_end = text.find('\n', tok[k].begin);
                        while (line_end != std::string::npos && line_end > 0 &&
                               (text[line_end - 1] == '\\' || (text[line_end - 1] == '\r' && line_end > 1 && text[line_end - 2] == '\\')))
                        {
                            line_end = text.find('\n', line_end + 1);
                        }
                        for (size_t j = k + 1; j < tok.size() && (line_end == std::string::npos || tok[j].begin < line_end); ++j)
                        {
                            if (tok[j].ident)
                            {
                                directive_names.push_back(word(j));
                            }
                        }
                    }
                }
                if (lights)
                {
                    if (is(k, "#") && is(k + 1, "define") && is(k + 2, "LLRHI_LIGHT_TABLE"))
                    {
                        light_marker = true;
                    }
                    else if (is(k, "void") && is(k + 1, "main") && is(k + 2, "("))
                    {
                        main_token[s] = k + 1;
                    }
                    else if ((is(k, "uniform") || is(k, "in")) && is(k + 3, ";"))
                    {
                        // `uniform T name;` (`in vec3 position;` for the position)
                        const bool uniform = is(k, "uniform");
                        for (U32 n = 0; n < LIGHT_NAMES; ++n)
                        {
                            if (uniform == (n != LIGHT_POSITION) && is(k + 1, LIGHT_TYPE[n]) && is(k + 2, LIGHT_NAME[n]))
                            {
                                light_declared[s] |= 1u << n;
                                light_declaration_tokens[s].push_back({ k, k + 3 });
                            }
                        }
                    }
                }
                if (is(k, "uniform"))
                {
                    size_t m = k + 1;
                    if (is(m, "lowp") || is(m, "mediump") || is(m, "highp"))
                    {
                        ++m;
                    }
                    const U32 material_components = !material_rewrite ? 0 : is(m, "float") ? 1 : is(m, "vec2") ? 2 :
                                                    is(m, "vec3") ? 3 : is(m, "vec4") ? 4 : 0;
                    if (material_components)
                    {
                        size_t name_token = 0, size_token = 0, end_token = 0;
                        if (m + 2 < tok.size() && tok[m + 1].ident && is(m + 2, ";"))
                        {
                            name_token = m + 1;
                            end_token = m + 2;
                        }
                        else if (m + 5 < tok.size() && tok[m + 1].ident && is(m + 2, "[") && tok[m + 3].ident && is(m + 4, "]") && is(m + 5, ";"))
                        {
                            name_token = m + 1;
                            size_token = m + 3;
                            end_token = m + 5;
                        }
                        else if (m + 5 < tok.size() && is(m + 1, "[") && tok[m + 2].ident && is(m + 3, "]") && tok[m + 4].ident && is(m + 5, ";"))
                        {
                            name_token = m + 4;
                            size_token = m + 2;
                            end_token = m + 5;
                        }
                        if (name_token &&
                            std::find_if(std::begin(MATERIAL_NAMES), std::end(MATERIAL_NAMES),
                                         [&](const char* n) { return is(name_token, n); }) != std::end(MATERIAL_NAMES))
                        {
                            material_declarations[s].push_back({ word(name_token), material_components,
                                                                 size_token ? word(size_token) : std::string(),
                                                                 tok[k].begin, tok[end_token].end });
                            continue;
                        }
                    }
                    if (consts_rewrite)
                    {
                        const ConstType* type = nullptr;
                        for (const ConstType& t : CONST_TYPES)
                        {
                            if (is(m, t.name))
                            {
                                type = &t;
                                break;
                            }
                        }
                        size_t name_token = 0, size_token = 0, end_token = 0;
                        if (type && m + 2 < tok.size() && tok[m + 1].ident && is(m + 2, ";"))
                        {
                            name_token = m + 1;
                            end_token = m + 2;
                        }
                        else if (type && m + 5 < tok.size() && tok[m + 1].ident && is(m + 2, "[") && tok[m + 3].ident && is(m + 4, "]") && is(m + 5, ";"))
                        {
                            name_token = m + 1;
                            size_token = m + 3;
                            end_token = m + 5;
                        }
                        else if (type && m + 5 < tok.size() && is(m + 1, "[") && tok[m + 2].ident && is(m + 3, "]") && tok[m + 4].ident && is(m + 5, ";"))
                        {
                            name_token = m + 4;
                            size_token = m + 2;
                            end_token = m + 5;
                        }
                        if (name_token &&
                            std::find_if(consts_wanted.begin(), consts_wanted.end(),
                                         [&](const ConstWanted& c) { return is(name_token, c.name.c_str()); }) != consts_wanted.end())
                        {
                            const_declarations[s].push_back({ word(name_token), type, size_token ? word(size_token) : std::string(),
                                                              tok[k].begin, tok[end_token].end });
                            continue;
                        }
                    }
                    if (is(m, "sampler2D") && m + 2 < tok.size() && tok[m + 1].ident && is(m + 2, ";"))
                    {
                        declarations[s].push_back({ word(m + 1), tok[k].begin, tok[m + 2].end });
                    }
                    else if (palette_enabled && m + 5 < tok.size() && (is(m, "mat3x4") || is(m, "vec4")) && is(m + 1, "matrixPalette") &&
                             is(m + 2, "[") && tok[m + 3].ident && is(m + 4, "]") && is(m + 5, ";"))
                    {
                        palette_declarations[s].push_back({ tok[k].begin, tok[m + 5].end, k, m + 5, is(m, "mat3x4") ? 3u : 1u, word(m + 3) });
                    }
                    else if (matrices_enabled && m + 2 < tok.size() && tok[m + 1].ident && is(m + 2, ";"))
                    {
                        for (U32 kind = 0; kind < MATRIX_KINDS; ++kind)
                        {
                            if (is(m, MATRIX_TYPES[kind]) && word(m + 1) == MATRIX_NAMES[kind])
                            {
                                matrix_declarations[s].push_back({ word(m + 1), tok[k].begin, tok[m + 2].end });
                                matrix_mask |= 1u << kind;
                            }
                        }
                    }
                }
                else if (is(k, "#") && is(k + 1, "define") && k + 3 < tok.size() && tok[k + 2].ident && tok[k + 3].ident &&
                         text.find('\n', tok[k + 2].end) > tok[k + 3].begin)
                {
                    numeric_defines[word(k + 2)] = word(k + 3);     // checked for digits where used
                }
                else if (is(k, "#") && is(k + 1, "define") && k + 7 < tok.size() && tok[k + 2].ident && is(k + 3, "(") &&
                         tok[k + 4].ident && is(k + 5, ")") && is(k + 6, "sampler2D") && tok[k + 7].ident && word(k + 4) == word(k + 7))
                {
                    sampler_macros.push_back(word(k + 2));
                }
            }
            for (const Declaration& d : declarations[s])
            {
                if (d.name.find("depth") == std::string::npos && d.name.find("Depth") == std::string::npos &&
                    std::find(names.begin(), names.end(), d.name) == names.end())
                {
                    names.push_back(d.name);
                }
            }
        }

        // A function parameter with the same name would turn into an array
        // declaration under the #define: keep such samplers as descriptors.
        // Parameters may also be declared through a macro that expands to
        // `sampler2D name` (SMAA's SMAATexture2D(tex)).
        std::vector<std::string> shadowed;
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const std::string& text = sources[s].text;
            const std::vector<GlslToken>& tok = tokens[s];
            auto word = [&](size_t k) { return text.substr(tok[k].begin, tok[k].end - tok[k].begin); };
            auto punct = [&](size_t k, char c) { return k < tok.size() && !tok[k].ident && text[tok[k].begin] == c; };
            for (size_t k = 0; k + 2 < tok.size(); ++k)
            {
                if (!tok[k].ident)
                {
                    continue;
                }
                size_t param = 0;
                const std::string w = word(k);
                if (w == "sampler2D" && tok[k + 1].ident)
                {
                    param = k + 1;
                }
                else if (punct(k + 1, '(') && tok[k + 2].ident &&
                         std::find(sampler_macros.begin(), sampler_macros.end(), w) != sampler_macros.end())
                {
                    param = k + 2;
                }
                if (param && (punct(param + 1, ',') || punct(param + 1, ')')))
                {
                    shadowed.push_back(word(param));
                }
            }
        }
        names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& name)
                    { return std::find(shadowed.begin(), shadowed.end(), name) != shadowed.end(); }),
                    names.end());
        // The skinning palette: every use must be `matrixPalette[index]` in a file
        // that declares it, the index free of side effects (the mat3x4 read
        // evaluates it three times), and the size known. Otherwise it stays a
        // uniform in the default block.
        struct PaletteUse
        {
            size_t begin;       // `matrixPalette`
            size_t end;         // past `]`
            size_t index_begin;
            size_t index_end;
        };
        std::vector<std::vector<PaletteUse>> palette_uses(sources.size());
        {
            bool ok = true;
            U32 columns = 0;
            std::string size;
            for (const auto& declared : palette_declarations)
            {
                for (const PaletteDeclaration& d : declared)
                {
                    ok &= !columns || (columns == d.columns && size == d.size);
                    columns = d.columns;
                    size = d.size;
                }
            }
            ok &= columns != 0;
            U32 elements = 0;
            if (ok)
            {
                auto define = numeric_defines.find(size);
                const std::string& number = define != numeric_defines.end() ? define->second : size;
                ok = !number.empty() && number.size() <= 6 && number.find_first_not_of("0123456789") == std::string::npos;
                elements = ok ? (U32)atoi(number.c_str()) : 0;
                ok = elements > 0 && (size_t)elements * columns <= LLVKBindless::PALETTE_INITIAL_VEC4S;
            }
            for (size_t s = 0; ok && s < sources.size(); ++s)
            {
                const std::string& text = sources[s].text;
                const std::vector<GlslToken>& tok = tokens[s];
                auto is = [&](size_t k, const char* word)
                {
                    const size_t len = strlen(word);
                    return k < tok.size() && tok[k].end - tok[k].begin == len && text.compare(tok[k].begin, len, word) == 0;
                };
                for (size_t k = 0; ok && k < tok.size(); ++k)
                {
                    if (!is(k, "matrixPalette"))
                    {
                        continue;
                    }
                    bool declaration = false;
                    for (const PaletteDeclaration& d : palette_declarations[s])
                    {
                        declaration |= k >= d.first_token && k <= d.last_token;
                    }
                    if (declaration)
                    {
                        continue;
                    }
                    if (palette_declarations[s].empty() || !is(k + 1, "["))
                    {
                        ok = false;
                        break;
                    }
                    size_t close = k + 2;
                    for (S32 depth = 1; close < tok.size(); ++close)
                    {
                        depth += is(close, "[") ? 1 : is(close, "]") ? -1 : 0;
                        if (!depth)
                        {
                            break;
                        }
                    }
                    if (close >= tok.size() || close == k + 2)
                    {
                        ok = false;
                        break;
                    }
                    const std::string index = text.substr(tok[k + 2].begin, tok[close - 1].end - tok[k + 2].begin);
                    if (index.find('=') != std::string::npos || index.find("++") != std::string::npos ||
                        index.find("--") != std::string::npos)
                    {
                        ok = false;
                        break;
                    }
                    palette_uses[s].push_back({ tok[k].begin, tok[close].end, tok[k + 2].begin, tok[close - 1].end });
                    k = close;
                }
            }
            if (ok && columns)
            {
                palette_columns = columns;
                palette_elements = elements;
            }
        }
        const bool palette = palette_columns != 0;

        // The material row: one member per name, every declaration of it the
        // same shape (as GL requires across the program's files) and its array
        // size known. A name declared otherwise stays a uniform in the default
        // block. Uses need no edit: each file that declares a name gets a
        // #define reading the row (a use the macro does not fit, such as a
        // member or another declaration of the name, fails to build and the
        // program is made again without the material rewrite: link_program).
        std::vector<bool> material_array;
        if (material_rewrite)
        {
            std::vector<std::string> rejected;
            for (const auto& declared : material_declarations)
            {
                for (const MaterialDeclaration& d : declared)
                {
                    U32 count = 1;
                    bool ok = true;
                    if (!d.size.empty())
                    {
                        auto define = numeric_defines.find(d.size);
                        const std::string& number = define != numeric_defines.end() ? define->second : d.size;
                        ok = !number.empty() && number.size() <= 3 && number.find_first_not_of("0123456789") == std::string::npos;
                        count = ok ? (U32)atoi(number.c_str()) : 0;
                        ok = ok && count > 0 && count <= 64;
                    }
                    size_t i = 0;
                    while (i < materials.size() && materials[i].name != d.name)
                    {
                        ++i;
                    }
                    if (!ok || (i < materials.size() && (materials[i].components != d.components || materials[i].count != count ||
                                                          material_array[i] != !d.size.empty())))
                    {
                        rejected.push_back(d.name);
                    }
                    else if (i == materials.size())
                    {
                        MaterialMember member;
                        member.name = d.name;
                        member.components = d.components;
                        member.count = count;
                        materials.push_back(member);
                        material_array.push_back(!d.size.empty());
                    }
                }
            }
            U32 first = 0;
            size_t kept = 0;
            for (size_t i = 0; i < materials.size(); ++i)
            {
                if (std::find(rejected.begin(), rejected.end(), materials[i].name) != rejected.end())
                {
                    continue;
                }
                materials[kept] = materials[i];
                material_array[kept] = material_array[i];
                materials[kept].first = first;
                first += materials[kept].count;
                ++kept;
            }
            materials.resize(kept);
            material_array.resize(kept);
        }

        // The constants row (research/50 step 3): each wanted name declared
        // in one of the forms above, every declaration of it the same shape,
        // as reflected (type and array size), at most 16 elements (an array is
        // read as an array constructor) and on no preprocessor line. Uses need
        // no edit, as for the material row. A name short of that stays a
        // uniform (the caller sees whether the default block is still used).
        if (consts_rewrite)
        {
            U32 first = 0;
            for (const ConstWanted& want : consts_wanted)
            {
                bool ok = true;
                const ConstType* type = nullptr;
                U32 count = 0;
                bool array = false;
                for (const auto& declared : const_declarations)
                {
                    for (const ConstDeclaration& d : declared)
                    {
                        if (d.name != want.name)
                        {
                            continue;
                        }
                        U32 n = 1;
                        if (!d.size.empty())
                        {
                            auto define = numeric_defines.find(d.size);
                            const std::string& number = define != numeric_defines.end() ? define->second : d.size;
                            const bool digits = !number.empty() && number.size() <= 3 && number.find_first_not_of("0123456789") == std::string::npos;
                            n = digits ? (U32)atoi(number.c_str()) : 0;
                        }
                        if (!n || n > 16 || (type && (type->gl_type != d.type->gl_type || count != n || array != !d.size.empty())))
                        {
                            ok = false;
                        }
                        type = d.type;
                        count = n;
                        array = !d.size.empty();
                    }
                }
                ok = ok && type && type->gl_type == want.gl_type && count == want.size &&
                     std::find(directive_names.begin(), directive_names.end(), want.name) == directive_names.end();
                if (!ok)
                {
                    continue;
                }
                ConstMember member;
                member.name = want.name;
                member.type = type;
                member.count = count;
                member.array = array;
                member.first = first;
                first += count * type->columns;
                consts.push_back(member);
            }
        }

        // The point light: one vertex file and one fragment file declaring
        // exactly its names, and every use of a name in them an expression
        // (not a member, nor another declaration except a prototype's
        // parameter). Uses become the llrhi_light_* macros (light_table_code),
        // which read the light table for draws with a table_base and the
        // uniforms otherwise. Anything else leaves the program as it is.
        struct LightEdit
        {
            size_t begin;
            size_t end;
            const char* with;
        };
        std::vector<std::vector<LightEdit>> light_edits(sources.size());
        S32 light_vertex = -1;
        S32 light_fragment = -1;
        if (lights && light_marker)
        {
            const U32 vertex_names = (1u << LIGHT_POSITION) | (1u << LIGHT_CENTER) | (1u << LIGHT_SIZE);
            const U32 fragment_names = (1u << LIGHT_COLOR) | (1u << LIGHT_FALLOFF) | (1u << LIGHT_SIZE);
            bool ok = true;
            for (size_t s = 0; s < sources.size(); ++s)
            {
                if (sources[s].stage == LLVKShader::Stage::Vertex && light_declared[s] == vertex_names && main_token[s] && light_vertex < 0)
                {
                    light_vertex = (S32)s;
                }
                else if (sources[s].stage == LLVKShader::Stage::Fragment && light_declared[s] == fragment_names && main_token[s] &&
                         light_fragment < 0)
                {
                    light_fragment = (S32)s;
                }
            }
            ok = light_vertex >= 0 && light_fragment >= 0;
            // Any other file with some of the names: a library of the same stage
            // whose uses cannot run from the light's file (research/52: before
            // this, deferredUtil.glsl's `color` and `size` refused the rewrite
            // without a word, and every light went one at a time).
            for (size_t s = 0; ok && s < sources.size(); ++s)
            {
                if (!light_declared[s] || (S32)s == light_vertex || (S32)s == light_fragment)
                {
                    continue;
                }
                const bool vertex = sources[s].stage == LLVKShader::Stage::Vertex;
                const U32 stage_names = vertex ? vertex_names : sources[s].stage == LLVKShader::Stage::Fragment ? fragment_names : 0;
                std::vector<std::string> library_names;
                for (U32 n = 0; n < LIGHT_NAMES; ++n)
                {
                    if ((light_declared[s] >> n) & 1u)
                    {
                        library_names.push_back(LIGHT_NAME[n]);
                    }
                }
                ok = stage_names && !(light_declared[s] & ~stage_names) &&
                     light_library_unreachable(sources, tokens, s, library_names, light_declaration_tokens[s],
                                               (size_t)(vertex ? light_vertex : light_fragment));
            }
            for (S32 file : { light_vertex, light_fragment })
            {
                if (!ok)
                {
                    break;
                }
                const size_t s = (size_t)file;
                const std::string& text = sources[s].text;
                const std::vector<GlslToken>& tok = tokens[s];
                auto is = [&](size_t k, const char* word)
                {
                    const size_t len = strlen(word);
                    return k < tok.size() && tok[k].end - tok[k].begin == len && text.compare(tok[k].begin, len, word) == 0;
                };
                for (size_t k = 0; ok && k < tok.size(); ++k)
                {
                    U32 n = 0;
                    while (n < LIGHT_NAMES && !((light_declared[s] >> n) & 1u && is(k, LIGHT_NAME[n])))
                    {
                        ++n;
                    }
                    if (n == LIGHT_NAMES)
                    {
                        continue;
                    }
                    bool declaration = false;
                    for (const auto& d : light_declaration_tokens[s])
                    {
                        declaration |= k >= d.first && k <= d.second;
                    }
                    if (declaration || (k > 0 && is(k - 1, ".")))
                    {
                        continue;
                    }
                    if (k > 0 && tok[k - 1].ident && !is(k - 1, "return"))
                    {
                        // Another declaration of the name: harmless only as a
                        // parameter of a prototype (`float f(float falloff);`).
                        bool prototype = false;
                        if (is(k + 1, ",") || is(k + 1, ")"))
                        {
                            size_t close = k + 1;
                            for (S32 depth = 0; close < tok.size(); ++close)
                            {
                                if (is(close, "("))
                                {
                                    ++depth;
                                }
                                else if (is(close, ")") && depth-- == 0)
                                {
                                    break;
                                }
                            }
                            prototype = is(close + 1, ";");
                        }
                        ok = prototype;
                        continue;
                    }
                    light_edits[s].push_back({ tok[k].begin, tok[k].end, LIGHT_MACRO[n] });
                }
            }
            if (ok)
            {
                const GlslToken& main_name = tokens[(size_t)light_vertex][main_token[(size_t)light_vertex]];
                light_edits[(size_t)light_vertex].push_back({ main_name.begin, main_name.end, "llrhi_light_main" });
            }
            else
            {
                LL_WARNS("VKGL") << "light table rewrite refused (the light's names declared or used where it cannot rewrite them), "
                                    "lights go one at a time" << LL_ENDL;
            }
            light_table = ok;
        }

        // Slot numbers travel as push constants (and the matrix row, the
        // palette's first vec4, the light table's first row and the material
        // row's first vec4 after them): samplers beyond that room stay descriptors.
        // The constants row's first vec4 (research/50 step 3) goes last; not
        // with the point light (draw runs, which the row is for, take no lights).
        if (light_table)
        {
            consts.clear();
        }
        const size_t room = LLVKBindless::MAX_PUSHED_SLOTS - (matrix_mask ? 1 : 0) - (palette ? 1 : 0) - (light_table ? 1 : 0) -
                            (materials.empty() ? 0 : 1) - (consts.empty() ? 0 : 1);
        if (names.size() > room)
        {
            names.resize(room);
        }
        if (names.empty() && !matrix_mask && !palette && !light_table && materials.empty() && consts.empty())
        {
            return names;
        }
        for (size_t s = 0; s < sources.size(); ++s)
        {
            LLVKShader::Source& source = sources[s];
            // Drop the declarations of the rewritten samplers. Only a file that
            // declares a sampler gets its #define: other files may use the same
            // name for something else (SMAA passes textures as macro parameters).
            std::string text;
            std::vector<std::string> declared;
            std::vector<U32> declared_matrices;
            // Declarations dropped (`with` empty) and palette reads replaced.
            struct Edit
            {
                size_t begin;
                size_t end;
                std::string with;
            };
            std::vector<Edit> edits;
            for (const Declaration& d : declarations[s])
            {
                if (std::find(names.begin(), names.end(), d.name) == names.end())
                {
                    continue;
                }
                if (std::find(declared.begin(), declared.end(), d.name) == declared.end())
                {
                    declared.push_back(d.name);
                }
                edits.push_back({ d.begin, d.end, std::string() });
            }
            for (const Declaration& d : matrix_declarations[s])
            {
                const U32 kind = (U32)(std::find(MATRIX_NAMES, MATRIX_NAMES + MATRIX_KINDS, d.name) - MATRIX_NAMES);
                if (std::find(declared_matrices.begin(), declared_matrices.end(), kind) == declared_matrices.end())
                {
                    declared_matrices.push_back(kind);
                }
                edits.push_back({ d.begin, d.end, std::string() });
            }
            const bool declares_palette = palette && !palette_declarations[s].empty();
            if (declares_palette)
            {
                for (const PaletteDeclaration& d : palette_declarations[s])
                {
                    edits.push_back({ d.begin, d.end, std::string() });
                }
                for (const PaletteUse& use : palette_uses[s])
                {
                    edits.push_back({ use.begin, use.end, "llrhi_palette(" + source.text.substr(use.index_begin, use.index_end - use.index_begin) + ")" });
                }
            }
            const bool light_vertex_file = light_table && (S32)s == light_vertex;
            const bool light_fragment_file = light_table && (S32)s == light_fragment;
            if (light_vertex_file || light_fragment_file)
            {
                for (const LightEdit& edit : light_edits[s])
                {
                    edits.push_back({ edit.begin, edit.end, edit.with });
                }
            }
            std::vector<size_t> declared_materials;     // indices into materials
            for (const MaterialDeclaration& d : material_declarations[s])
            {
                size_t i = 0;
                while (i < materials.size() && materials[i].name != d.name)
                {
                    ++i;
                }
                if (i == materials.size())
                {
                    continue;   // rejected: stays a uniform
                }
                if (std::find(declared_materials.begin(), declared_materials.end(), i) == declared_materials.end())
                {
                    declared_materials.push_back(i);
                }
                edits.push_back({ d.begin, d.end, std::string() });
            }
            std::vector<size_t> declared_consts;        // indices into consts
            for (const ConstDeclaration& d : const_declarations[s])
            {
                size_t i = 0;
                while (i < consts.size() && consts[i].name != d.name)
                {
                    ++i;
                }
                if (i == consts.size())
                {
                    continue;   // not rewritten: stays a uniform
                }
                if (std::find(declared_consts.begin(), declared_consts.end(), i) == declared_consts.end())
                {
                    declared_consts.push_back(i);
                }
                edits.push_back({ d.begin, d.end, std::string() });
            }
            std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin; });
            size_t copied = 0;
            for (const Edit& edit : edits)
            {
                text.append(source.text, copied, edit.begin - copied);
                text += edit.with;
                copied = edit.end;
            }
            text.append(source.text, copied, std::string::npos);
            if (light_vertex_file)
            {
                text += light_table_code(LIGHT_VERTEX_MAIN);
            }
            if (declared.empty() && declared_matrices.empty() && !declares_palette && !light_vertex_file && !light_fragment_file &&
                declared_materials.empty() && declared_consts.empty())
            {
                continue;
            }

            // Declare the table and the slot numbers after the last #version / #extension line.
            // Every file gets the same push constant block (all of the program's
            // samplers, in order), so the stage's compilation units agree on it.
            std::string inject = "#extension GL_EXT_nonuniform_qualifier : require\n";
            // The palette table (set 0 binding 2): the palette, the light table,
            // the material rows and the constants rows.
            const bool reads_table = declares_palette || light_vertex_file || !declared_materials.empty() || !declared_consts.empty();
            if (!declared_matrices.empty() || reads_table)
            {
                inject += "#extension GL_ARB_shader_storage_buffer_object : require\n";
            }
            if (!names.empty())
            {
                inject += "layout(set = 0, binding = 0) uniform sampler2D llrhi_textures[];\n";
            }
            inject += "layout(push_constant) uniform llrhi_slot_block {";
            if (!names.empty())
            {
                inject += " uint slot[" + std::to_string(names.size()) + "];";
            }
            if (matrix_mask)
            {
                inject += " uint matrix;";
            }
            if (palette)
            {
                inject += " uint palette;";
            }
            if (light_table)
            {
                inject += " uint light;";
            }
            if (!materials.empty())
            {
                inject += " uint material;";
            }
            if (!consts.empty())
            {
                inject += " uint consts;";
            }
            inject += " } llrhi_slots;\n";
            for (const std::string& name : declared)
            {
                const size_t index = std::find(names.begin(), names.end(), name) - names.begin();
                inject += "#define " + name + " llrhi_textures[llrhi_slots.slot[" + std::to_string(index) + "]]\n";
            }
            if (!declared_matrices.empty())
            {
                // std430 rows of LLVKBindless::DrawMatrices (240 bytes).
                inject += "struct llrhi_draw_matrices { mat4 mv; mat4 mvp; mat4 tex0; mat3 nrm; };\n"
                          "layout(std430, set = 0, binding = 1) readonly buffer llrhi_matrix_table { llrhi_draw_matrices m[]; } llrhi_mt;\n";
                for (U32 kind : declared_matrices)
                {
                    inject += std::string("#define ") + MATRIX_NAMES[kind] + " (llrhi_mt.m[llrhi_slots.matrix]." + MATRIX_MEMBERS[kind] + ")\n";
                }
            }
            if (reads_table)
            {
                inject += "layout(std430, set = 0, binding = 2) readonly buffer llrhi_palette_table { vec4 v[]; } llrhi_pal;\n";
            }
            if (declares_palette)
            {
                // matrixPalette[i] -> llrhi_palette(i): the element's vec4s from
                // the draw's first one (LLVKBindless::paletteRows). A macro, not a
                // function: two files of a stage may both declare the palette.
                if (palette_columns == 3)
                {
                    inject += "#define llrhi_palette(i) mat3x4(llrhi_pal.v[llrhi_slots.palette + uint(i) * 3u], "
                              "llrhi_pal.v[llrhi_slots.palette + uint(i) * 3u + 1u], llrhi_pal.v[llrhi_slots.palette + uint(i) * 3u + 2u])\n";
                }
                else
                {
                    inject += "#define llrhi_palette(i) (llrhi_pal.v[llrhi_slots.palette + uint(i)])\n";
                }
            }
            if (light_vertex_file)
            {
                inject += light_table_code(LIGHT_VERTEX_DECLARATIONS);
            }
            for (size_t i : declared_materials)
            {
                // The member's vec4s from the draw's first one (material_row):
                // the declared type, an array as an array constructor.
                const MaterialMember& member = materials[i];
                const char* swizzle = member.components == 1 ? ".x" : member.components == 2 ? ".xy" : member.components == 3 ? ".xyz" : "";
                const std::string type = member.components == 1 ? "float" : "vec" + std::to_string(member.components);
                auto element = [&](U32 e)
                {
                    return "llrhi_pal.v[llrhi_slots.material + " + std::to_string(member.first + e) + "u]" + swizzle;
                };
                std::string value;
                if (!material_array[i])
                {
                    value = element(0);
                }
                else
                {
                    value = type + "[" + std::to_string(member.count) + "](";
                    for (U32 e = 0; e < member.count; ++e)
                    {
                        value += (e ? ", " : "") + element(e);
                    }
                    value += ")";
                }
                inject += "#define " + member.name + " (" + value + ")\n";
            }
            for (size_t i : declared_consts)
            {
                inject += "#define " + consts[i].name + " (" + const_value(consts[i]) + ")\n";
            }
            if (light_fragment_file)
            {
                inject += light_table_code(LIGHT_FRAGMENT_DECLARATIONS);
            }
            size_t at = 0;
            for (size_t pos = 0; pos < text.size(); )
            {
                size_t line_end = text.find('\n', pos);
                if (line_end == std::string::npos)
                {
                    line_end = text.size();
                }
                if (text.compare(pos, 8, "#version") == 0 || text.compare(pos, 10, "#extension") == 0)
                {
                    at = line_end + 1;
                }
                pos = line_end + 1;
            }
            text.insert(llmin(at, text.size()), inject);
            source.text = text;
        }
        return names;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
