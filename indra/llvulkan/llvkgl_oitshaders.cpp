/**
 * @file llvkgl_oitshaders.cpp
 * @brief The shaders of order-independent transparency (research/41): the variant's fragment code, the bounds and resolve passes (research/91).
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
#include "llrhiscene.h"     // research/205 3a-2: LLRHI::sceneAlphaMode

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
namespace
{
    // Appended to the program's fragment file (its main renamed llrhi_oit_main).
    // The rest image's sums are fixed point (1/4096).
    const char* const OIT_FRAGMENT_CODE = R"GLSL(

// LLRHI order-independent transparency (research/41): the fragment becomes a
// node of its pixel's list instead of reaching the target (split: only at a
// pixel that has a list already).
layout(early_fragment_tests) in;
layout(constant_id = 200) const uint llrhi_oit_blend = 0u;
layout(constant_id = 201) const uint llrhi_oit_split = 0u;
layout(set = 2, binding = 0, r32ui) uniform uimage2D llrhi_oit_heads;
layout(std430, set = 2, binding = 1) buffer llrhi_oit_nodes { uint count; uint capacity; uint flags; uint pad; uvec4 bounds; uint words[]; } llrhi_oit;
layout(set = 2, binding = 2, r32ui) uniform uimage2DArray llrhi_oit_rest;
void main()
{
    ivec2 llrhi_oit_p = ivec2(gl_FragCoord.xy);
    // Split (hybrid world draws): whether the pixel has avatar nodes. Read
    // first, its latency under the shading; heads only go from empty to
    // non-empty in the avatar half, which a barrier orders before this.
    bool llrhi_oit_classic = llrhi_oit_split != 0u && imageLoad(llrhi_oit_heads, llrhi_oit_p).x == 0xffffffffu;
    llrhi_oit_main();
    if (llrhi_oit_classic)
    {
        return;     // frag_color goes through the draw's own blend state
    }
    vec4 llrhi_oit_c = frag_color;
    uint llrhi_oit_i = atomicAdd(llrhi_oit.count, 1u);
    if (llrhi_oit_i < llrhi_oit.capacity)
    {
        uint llrhi_oit_next = imageAtomicExchange(llrhi_oit_heads, llrhi_oit_p, llrhi_oit_i);
        uint llrhi_oit_at = llrhi_oit_i * 6u;
        llrhi_oit.words[llrhi_oit_at] = packHalf2x16(llrhi_oit_c.rg);
        llrhi_oit.words[llrhi_oit_at + 1u] = packHalf2x16(llrhi_oit_c.ba);
        llrhi_oit.words[llrhi_oit_at + 2u] = floatBitsToUint(gl_FragCoord.z);
        llrhi_oit.words[llrhi_oit_at + 3u] = llrhi_oit_blend;
        llrhi_oit.words[llrhi_oit_at + 4u] = llrhi_oit_priority;
        llrhi_oit.words[llrhi_oit_at + 5u] = llrhi_oit_next;
    }
    else
    {
        // No room. Avatar (and full) sessions: the pixel is still marked as
        // covered (0xfffffffe, if it has no node yet), so a hybrid session's
        // world draws split there, and the fragment is summed into the rest
        // (behind the pixel's nodes). Split: summed into the front (layers
        // 6-11, over the nodes: world alpha comes far to near, so it is the
        // nearest drawn so far); without those images it blends into the
        // target (behind the nodes) rather than being lost.
        if (llrhi_oit_split == 0u)
        {
            imageAtomicCompSwap(llrhi_oit_heads, llrhi_oit_p, 0xffffffffu, 0xfffffffeu);
        }
        if ((llrhi_oit.flags & 1u) == 0u)
        {
            if (llrhi_oit_split != 0u)
            {
                return;
            }
        }
        else
        {
            int llrhi_oit_base = llrhi_oit_split != 0u ? 6 : 0;
            if ((llrhi_oit_blend & 255u) == 16u)
            {
                // ZERO, ONE: the color stays, only the glow mark grows.
                imageAtomicAdd(llrhi_oit_rest, ivec3(llrhi_oit_p, llrhi_oit_base + 5), uint(clamp(llrhi_oit_c.a, 0.0, 255.0) * 4096.0 + 0.5));
            }
            else
            {
                float llrhi_oit_a = clamp(llrhi_oit_c.a, 0.0, 1.0);
                vec3 llrhi_oit_rgb = clamp(llrhi_oit_c.rgb, vec3(0.0), vec3(255.0)) * llrhi_oit_a * 4096.0 + 0.5;
                imageAtomicAdd(llrhi_oit_rest, ivec3(llrhi_oit_p, llrhi_oit_base), uint(llrhi_oit_rgb.r));
                imageAtomicAdd(llrhi_oit_rest, ivec3(llrhi_oit_p, llrhi_oit_base + 1), uint(llrhi_oit_rgb.g));
                imageAtomicAdd(llrhi_oit_rest, ivec3(llrhi_oit_p, llrhi_oit_base + 2), uint(llrhi_oit_rgb.b));
                imageAtomicAdd(llrhi_oit_rest, ivec3(llrhi_oit_p, llrhi_oit_base + 3), uint(llrhi_oit_a * 4096.0 + 0.5));
                imageAtomicAdd(llrhi_oit_rest, ivec3(llrhi_oit_p, llrhi_oit_base + 4), uint(-log2(1.0 - min(llrhi_oit_a, 0.996)) * 4096.0 + 0.5));
            }
        }
    }
    if (llrhi_oit_split != 0u)
    {
        discard;    // a node (or summed): no color here
    }
}
)GLSL";
} // namespace

    // The resolve pass: a strip over the pixels with a list.
    const char* const OIT_RESOLVE_VERTEX = R"GLSL(#version 450
// A strip over the pixels the bounds pass found with a list (the node
// buffer's bounds: min x, min y, max x, max y); none: all four on one point.
layout(std430, set = 0, binding = 1) readonly buffer Nodes { uint count; uint capacity; uint flags; uint pad; uvec4 bounds; uint words[]; } nodes;
layout(push_constant) uniform Resolve
{
    uint clamp_colors;
    float depth_a;
    float depth_b;
    float ndc_scale;
    float ndc_bias;
    float tolerance;
    float relative;
    float span;
    float width;
    float height;
} pc;
void main()
{
    uvec4 b = nodes.bounds;
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 lo = vec2(b.xy);
    vec2 hi = vec2(b.zw + 1u);
    vec2 p = b.x <= b.z && b.y <= b.w ? mix(lo, hi, corner) / vec2(pc.width, pc.height) : vec2(2.0);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    // The bounds pass (compute, 16 x 16 pixels a group): the bounds of the
    // pixels with a list, into the node buffer's header; with COPY also the
    // copy of the target at those pixels (what the resolve blends over).
    const char* const OIT_BOUNDS_COMPUTE = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0, r32ui) uniform readonly uimage2D heads;
layout(std430, set = 0, binding = 1) buffer Nodes { uint count; uint capacity; uint flags; uint pad; uvec4 bounds; uint words[]; } nodes;
#ifdef COPY
layout(set = 0, binding = 4) uniform sampler2D target;
layout(set = 0, binding = 5, COPY) uniform writeonly image2D copy;
#endif
layout(push_constant) uniform Bounds { uint width; uint height; } pc;
shared uint lo_x, lo_y, hi_x, hi_y;
void main()
{
    if (gl_LocalInvocationIndex == 0u)
    {
        lo_x = 0xffffffffu;
        lo_y = 0xffffffffu;
        hi_x = 0u;
        hi_y = 0u;
    }
    barrier();
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x < pc.width && p.y < pc.height && imageLoad(heads, ivec2(p)).x != 0xffffffffu)
    {
        atomicMin(lo_x, p.x);
        atomicMin(lo_y, p.y);
        atomicMax(hi_x, p.x);
        atomicMax(hi_y, p.y);
#ifdef COPY
        imageStore(copy, ivec2(p), texelFetch(target, ivec2(p), 0));
#endif
    }
    barrier();
    if (gl_LocalInvocationIndex == 0u && lo_x != 0xffffffffu)
    {
        atomicMin(nodes.bounds.x, lo_x);
        atomicMin(nodes.bounds.y, lo_y);
        atomicMax(nodes.bounds.z, hi_x);
        atomicMax(nodes.bounds.w, hi_y);
    }
}
)GLSL";

    const char* const OIT_RESOLVE_FRAGMENT = R"GLSL(#version 450
layout(set = 0, binding = 0, r32ui) uniform readonly uimage2D heads;
layout(std430, set = 0, binding = 1) readonly buffer Nodes { uint count; uint capacity; uint flags; uint pad; uvec4 bounds; uint words[]; } nodes;
layout(set = 0, binding = 2, r32ui) uniform readonly uimage2DArray rest;     // layers 0-5 the rest, 6-11 the front
layout(set = 0, binding = 3) uniform sampler2D background;
// clamp_colors: a fixed-point target (GL clamps the blend's inputs and result).
// The distance from the eye: depth_a, depth_b (the projection's [2][2] and
// [3][2]) on the clip depth (window depth * ndc_scale + ndc_bias). Layers at
// most max(tolerance, relative * distance) (m) behind the one in front of
// them are one surface, a surface at most span such gaps deep; tolerance 0:
// only equal depths are.
layout(push_constant) uniform Resolve
{
    uint clamp_colors;
    float depth_a;
    float depth_b;
    float ndc_scale;
    float ndc_bias;
    float tolerance;
    float relative;
    float span;
    float width;        // the vertex stage's
    float height;
} pc;
layout(location = 0) out vec4 out_color;

// The nodes of a pixel kept in order (LLRHI_OIT_MAX_LAYERS): the nearest;
// the others are summed behind them.
layout(constant_id = 0) const uint MAX_LAYERS = 16u;
const uint END = 0xffffffffu;
const float FIXED = 1.0 / 4096.0;

vec3 factor_rgb(uint f, vec4 s, vec4 d)
{
    switch (f)
    {
    case 0u: return vec3(0.0);
    case 2u: return s.rgb;
    case 3u: return vec3(1.0) - s.rgb;
    case 4u: return d.rgb;
    case 5u: return vec3(1.0) - d.rgb;
    case 6u: return vec3(s.a);
    case 7u: return vec3(1.0 - s.a);
    case 8u: return vec3(d.a);
    case 9u: return vec3(1.0 - d.a);
    case 10u: return vec3(min(s.a, 1.0 - d.a));
    case 11u: return vec3(0.0);
    default: return vec3(1.0);
    }
}

float factor_a(uint f, vec4 s, vec4 d)
{
    switch (f)
    {
    case 0u: return 0.0;
    case 2u: case 6u: return s.a;
    case 3u: case 7u: return 1.0 - s.a;
    case 4u: case 8u: return d.a;
    case 5u: case 9u: return 1.0 - d.a;
    case 11u: return 0.0;
    default: return 1.0;
    }
}

vec3 combine_rgb(uint e, vec3 a, vec3 b, vec3 s, vec3 d)
{
    return e == 1u ? a - b : e == 2u ? b - a : e == 3u ? min(s, d) : e == 4u ? max(s, d) : a + b;
}

float combine_a(uint e, float a, float b, float s, float d)
{
    return e == 1u ? a - b : e == 2u ? b - a : e == 3u ? min(s, d) : e == 4u ? max(s, d) : a + b;
}

// One node over what is behind it, as GL would blend it.
vec4 blend(vec4 s, vec4 d, uint w)
{
    if (pc.clamp_colors != 0u)
    {
        s = clamp(s, 0.0, 1.0);
    }
    vec4 r;
    r.rgb = combine_rgb((w >> 16u) & 7u, s.rgb * factor_rgb(w & 15u, s, d), d.rgb * factor_rgb((w >> 4u) & 15u, s, d), s.rgb, d.rgb);
    r.a = combine_a((w >> 19u) & 7u, s.a * factor_a((w >> 8u) & 15u, s, d), d.a * factor_a((w >> 12u) & 15u, s, d), s.a, d.a);
    r = mix(d, r, notEqual((uvec4(w) >> uvec4(22u, 23u, 24u, 25u)) & 1u, uvec4(0u)));
    return pc.clamp_colors != 0u ? clamp(r, 0.0, 1.0) : r;
}

// A node that leaves the color as it is: the glow pass (ZERO, ONE), or no
// color channel written.
bool modifier(uint w)
{
    return (w & 255u) == 16u || ((w >> 22u) & 7u) == 0u;
}

// The distance from the eye of a window depth.
float eye_distance(uint depth_bits)
{
    float z = uintBitsToFloat(depth_bits) * pc.ndc_scale + pc.ndc_bias;
    return max(pc.depth_b / min(z + pc.depth_a, -1e-7), 0.0);
}

// Inside a surface: whether node a (key: depth bits, bit 31 set for a node
// leaving the color as it is) goes on top of node b. The higher priority,
// then the glow pass, then the nearer.
bool on_top(uint key_a, uint id_a, uint key_b, uint id_b)
{
    uint priority_a = nodes.words[id_a * 6u + 4u];
    uint priority_b = nodes.words[id_b * 6u + 4u];
    if (priority_a != priority_b)
    {
        return priority_a > priority_b;
    }
    if ((key_a >> 31u) != (key_b >> 31u))
    {
        return (key_a >> 31u) != 0u;
    }
    return (key_a & 0x7fffffffu) < (key_b & 0x7fffffffu);
}

// Layers summed without order: their weighted average color over d.
vec4 over_average(vec4 d, vec3 sum_c, float sum_a, float t, float glow)
{
    if (sum_a > 0.0)
    {
        d.rgb = d.rgb * t + sum_c / sum_a * (1.0 - t);
        d.a *= t;
    }
    d.a += glow;
    return pc.clamp_colors != 0u ? clamp(d, 0.0, 1.0) : d;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint head = imageLoad(heads, p).x;
    if (head == END)
    {
        discard;    // no list (a fragment summed into the rest or front marked its head)
    }
    bool has_rest = (nodes.flags & 1u) != 0u;
    uint rest_a = 0u;
    uint rest_glow = 0u;
    if (has_rest)
    {
        rest_a = imageLoad(rest, ivec3(p, 3)).x;
        rest_glow = imageLoad(rest, ivec3(p, 5)).x;
    }

    // The nearest MAX_LAYERS nodes, nearest first by exact depth. Farther
    // ones go to the tail, summed like the rest. Only depths and node
    // numbers stay in registers (a key: the depth bits, bit 31 set for a
    // node that leaves the color as it is; depths in [0, 1] leave it free);
    // the priority is read from the node when needed.
    uint keys[MAX_LAYERS];
    uint ids[MAX_LAYERS];
    uint n = 0u;
    vec3 tail_c = vec3(0.0);
    float tail_a = 0.0;
    float tail_t = 1.0;
    float tail_glow = 0.0;
    uint capacity = min(nodes.capacity, uint(nodes.words.length()) / 6u);
    uint i = head;
    for (uint steps = 0u; i != END && i < capacity && steps < 65536u; ++steps)
    {
        uint at = i * 6u;
        uint depth = nodes.words[at + 2u] & 0x7fffffffu;
        uint key = depth | (modifier(nodes.words[at + 3u]) ? 0x80000000u : 0u);
        uint id = i;
        i = nodes.words[at + 5u];
        uint tail = END;
        if (n == MAX_LAYERS)
        {
            if (depth >= (keys[n - 1u] & 0x7fffffffu))
            {
                tail = id;
            }
            else
            {
                tail = ids[n - 1u];
                --n;
            }
        }
        if (tail != id)
        {
            uint k = n;
            while (k > 0u && (keys[k - 1u] & 0x7fffffffu) > depth)
            {
                keys[k] = keys[k - 1u];
                ids[k] = ids[k - 1u];
                --k;
            }
            keys[k] = key;
            ids[k] = id;
            ++n;
        }
        if (tail != END)
        {
            uint t_at = tail * 6u;
            vec4 c = vec4(unpackHalf2x16(nodes.words[t_at]), unpackHalf2x16(nodes.words[t_at + 1u]));
            if ((nodes.words[t_at + 3u] & 255u) == 16u)
            {
                tail_glow += max(c.a, 0.0);
            }
            else
            {
                float a = clamp(c.a, 0.0, 1.0);
                tail_c += max(c.rgb, vec3(0.0)) * a;
                tail_a += a;
                tail_t *= 1.0 - a;
            }
        }
    }

    // Surfaces: from the front, a node at most the tolerance behind the one
    // in front of it joins its surface while the surface stays within span
    // tolerances of its front; inside a surface the nodes go by on_top
    // instead of depth (clothing layers on a body do not speckle). Only the
    // depths decide where surfaces start: the same every frame.
    bool exact = pc.tolerance <= 0.0 || pc.depth_b == 0.0;
    uint start = 0u;
    float front = 0.0;
    float previous = 0.0;
    for (uint k = 0u; k <= n; ++k)
    {
        bool joins = false;
        float distance = 0.0;
        if (k > 0u && k < n)
        {
            if (exact)
            {
                joins = (keys[k] & 0x7fffffffu) == (keys[k - 1u] & 0x7fffffffu);
            }
            else
            {
                distance = eye_distance(keys[k] & 0x7fffffffu);
                float tolerance = max(pc.tolerance, pc.relative * distance);
                joins = distance - previous <= tolerance && distance - front <= pc.span * tolerance;
            }
        }
        else if (k == 0u && n > 0u && !exact)
        {
            distance = eye_distance(keys[0] & 0x7fffffffu);
        }
        if (k > 0u && !joins)
        {
            // The surface [start, k): on top first.
            for (uint a = start + 1u; a < k; ++a)
            {
                uint key = keys[a];
                uint id = ids[a];
                uint b = a;
                while (b > start && on_top(key, id, keys[b - 1u], ids[b - 1u]))
                {
                    keys[b] = keys[b - 1u];
                    ids[b] = ids[b - 1u];
                    --b;
                }
                keys[b] = key;
                ids[b] = id;
            }
            start = k;
        }
        if (!joins)
        {
            front = distance;
        }
        previous = distance;
    }

    vec4 d = texelFetch(background, p, 0);
    if (pc.clamp_colors != 0u)
    {
        d = clamp(d, 0.0, 1.0);
    }
    if (rest_a != 0u || rest_glow != 0u)
    {
        vec3 sum_c = vec3(imageLoad(rest, ivec3(p, 0)).x, imageLoad(rest, ivec3(p, 1)).x, imageLoad(rest, ivec3(p, 2)).x) * FIXED;
        float t = exp2(-float(imageLoad(rest, ivec3(p, 4)).x) * FIXED);
        d = over_average(d, sum_c, float(rest_a) * FIXED, t, float(rest_glow) * FIXED);
    }
    d = over_average(d, tail_c, tail_a, tail_t, tail_glow);
    for (uint k = n; k > 0u; --k)
    {
        uint at = ids[k - 1u] * 6u;
        vec4 s = vec4(unpackHalf2x16(nodes.words[at]), unpackHalf2x16(nodes.words[at + 1u]));
        d = blend(s, d, nodes.words[at + 3u]);
    }
    if (has_rest)
    {
        // The front: world fragments of a hybrid session that found no room,
        // the nearest drawn when they came (world alpha comes far to near).
        uint front_a = imageLoad(rest, ivec3(p, 9)).x;
        uint front_glow = imageLoad(rest, ivec3(p, 11)).x;
        if (front_a != 0u || front_glow != 0u)
        {
            vec3 sum_c = vec3(imageLoad(rest, ivec3(p, 6)).x, imageLoad(rest, ivec3(p, 7)).x, imageLoad(rest, ivec3(p, 8)).x) * FIXED;
            float t = exp2(-float(imageLoad(rest, ivec3(p, 10)).x) * FIXED);
            d = over_average(d, sum_c, float(front_a) * FIXED, t, float(front_glow) * FIXED);
        }
    }
    out_color = d;
}
)GLSL";

    // The OIT variant's sources: the fragment file with main renames it and
    // gets OIT_FRAGMENT_CODE (which reads the program's frag_color: a program
    // without it fails to build, and draws as before). The draw's priority is
    // the push constant word at OIT_PRIORITY_OFFSET: a member of the
    // program's push constant block (llrhi_slots, in every file that declares
    // it, so the files of a stage still agree on it; the fragment file with
    // main gets the block if another fragment file has it), else a block of
    // its own in the fragment file. Every other file and declaration stays as
    // it is. False: no fragment main, mains in more than one fragment file,
    // or the llrhi_oit names already taken.
    U32 oit_row_word(bool draw_rows, U32 draw_row_words)
    {
        return draw_rows && LLRHI::sceneAlphaMode() != LLRHI::SceneMode::Off ? draw_row_words : U32_MAX;
    }

    bool oit_rewrite(std::vector<LLVKShader::Source>& sources, U32 row_word, bool* row_priority)
    {
        if (row_priority)
        {
            *row_priority = false;
        }
        S32 main_file = -1;
        // Every `main` of `void main(` in the file: the preprocessor keeps one (pbralphaF).
        std::vector<std::pair<size_t, size_t>> mains;
        std::vector<GlslToken> tok;
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const std::string& text = sources[s].text;
            if (text.find("llrhi_oit") != std::string::npos)
            {
                return false;
            }
            if (sources[s].stage != LLVKShader::Stage::Fragment)
            {
                continue;
            }
            tokenize_glsl(text, tok);
            auto is = [&](size_t k, const char* word)
            {
                const size_t len = strlen(word);
                return k < tok.size() && tok[k].end - tok[k].begin == len && text.compare(tok[k].begin, len, word) == 0;
            };
            for (size_t k = 0; k + 2 < tok.size(); ++k)
            {
                if (is(k, "void") && is(k + 1, "main") && is(k + 2, "("))
                {
                    if (main_file >= 0 && main_file != (S32)s)
                    {
                        return false;
                    }
                    main_file = (S32)s;
                    mains.emplace_back(tok[k + 1].begin, tok[k + 1].end);
                }
            }
        }
        if (main_file < 0)
        {
            return false;
        }
        std::string& text = sources[(size_t)main_file].text;
        for (auto it = mains.rbegin(); it != mains.rend(); ++it)    // from the back: positions stay valid
        {
            text.replace(it->first, it->second - it->first, "llrhi_oit_main");
        }

        // The draw's priority in the push constants.
        static const std::string BLOCK_START = "layout(push_constant) uniform llrhi_slot_block {";
        static const std::string BLOCK_END = "} llrhi_slots;";
        const std::string member = "layout(offset = " + std::to_string(OIT_PRIORITY_OFFSET) + ") uint oit_priority; ";
        std::string block;      // the program's block as a fragment file declares it (before the member)
        for (const LLVKShader::Source& source : sources)
        {
            const size_t start = source.text.find(BLOCK_START);
            const size_t end = start == std::string::npos ? std::string::npos : source.text.find(BLOCK_END, start);
            if (source.stage == LLVKShader::Stage::Fragment && end != std::string::npos)
            {
                block = source.text.substr(start, end + BLOCK_END.size() - start);
                break;
            }
        }
        std::string declarations;
        std::string sequence;
        if (!block.empty())
        {
            if (text.find(BLOCK_START) == std::string::npos)
            {
                declarations = block + "\n";   // gets the member below, as the other files
            }
            sequence = "llrhi_slots.oit_priority";
        }
        else
        {
            declarations = "layout(push_constant) uniform llrhi_oit_block { " + member + "} llrhi_oit_pc;\n";
            sequence = "llrhi_oit_pc.oit_priority";
        }
        if (text.find("GL_ARB_shader_storage_buffer_object") == std::string::npos)
        {
            declarations = "#extension GL_ARB_shader_storage_buffer_object : require\n" + declarations;
        }
        if (!declarations.empty())
        {
            // After the last #version / #extension line, as bindless_rewrite does.
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
            text.insert(llmin(at, text.size()), declarations);
        }
        if (!block.empty())
        {
            for (LLVKShader::Source& source : sources)
            {
                const size_t start = source.text.find(BLOCK_START);
                const size_t end = start == std::string::npos ? std::string::npos : source.text.find(BLOCK_END, start);
                if (end != std::string::npos)
                {
                    source.text.insert(end, member);
                }
            }
        }
        // research/205 3a-2: a program reading draw rows (draw_rows_rewrite)
        // takes its priority from word row_word of the draw's row when it
        // reads rows (the scene's indirect draws, one priority per record),
        // else from the push constant as before (llrhi_dw's NO_TABLE side:
        // every node draw outside a draw run). Only where the file with main
        // has the rows' reads (llrhi_dw is defined after its push block).
        if (row_word < LLVKBindless::MAX_PUSHED_SLOTS && text.find("#define llrhi_dw(") != std::string::npos)
        {
            sequence = "llrhi_dw(" + std::to_string(row_word) + "u, " + sequence + ")";
            if (row_priority)
            {
                *row_priority = true;
            }
        }
        text += "\n#define llrhi_oit_priority (" + sequence + ")";
        text += OIT_FRAGMENT_CODE;
        return true;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
