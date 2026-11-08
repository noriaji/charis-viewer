/**
 * @file llvkssr.cpp
 * @brief Screen space reflections in passes of their own, recorded directly
 *        in Vulkan (research/129: a port of the official
 *        project/graphics-care-package's Hi-Z tracing).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
 * Portions (the shaders, from the official project/graphics-care-package)
 * Copyright (C) 2007-2026, Linden Research, Inc.
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

// The shaders are GCP's (project/graphics-care-package 2c79cbae84,
// class3/deferred/screenSpaceReflUtil.glsl, screenSpaceReflTraceF.glsl,
// screenSpaceReflResolveF.glsl, class1/deferred/hiZReduceF.glsl,
// class1/interface/ssrDownsampleF.glsl),
// written for these passes' bindings. Where they differ from GCP's, a comment
// says so (research/129 section 2 lists why).

#include "linden_common.h"

#include "llvkssr.h"

#include "llrhiskinsss.h"
#include "llrhissr.h"
#include "llvkdevice.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvkmirror.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: the barriers go to the frame's recorder in order

#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Shaders

    // The G-buffer as the official lighting reads it (gbufferUtil.glsl,
    // globalF.glsl's decodeNormal, llshadermgr.cpp's GBUFFER_FLAG_*).
    const char* const GBUFFER_GLSL = R"GLSL(
const float FLAG_HAS_ATMOS = 0.34;      // legacy materials
const float FLAG_HAS_PBR = 0.67;
const float FLAG_TOLERANCE = 0.1;

vec3 decodeNormal(vec2 encoded)
{
    vec2 fenc = encoded * 4.0 - 2.0;
    float f = dot(fenc, fenc);
    float g = sqrt(max(1.0 - f / 4.0, 0.0));
    return vec3(fenc * g, 1.0 - f / 2.0);
}

// A G-buffer pixel's roughness, or -1 where the lighting reads no
// reflection (the sky, HDRI, fullbright without atmospherics). PBR stores
// occlusion, roughness and metallic; legacy materials the specular colour and
// the gloss (softenLightF.glsl).
float surfaceRoughness(vec4 specular, vec4 normal_info, out bool pbr)
{
    pbr = abs(normal_info.a - FLAG_HAS_PBR) < FLAG_TOLERANCE;
    if (pbr)
    {
        return specular.g;
    }
    if (abs(normal_info.a - FLAG_HAS_ATMOS) < FLAG_TOLERANCE)
    {
        return 1.0 - specular.a;
    }
    return -1.0;
}
)GLSL";

    // The depth pyramid (GCP's hiZReduceF.glsl): each texel the nearest
    // (smallest) depth of the 2x2 below it, and of the third column and row
    // where the level below is odd. Level 0 is the scene copy's depth; this
    // pass writes levels 1.. into R32F.
    const char* const HIZ_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SsrChain
{
    ivec2 source_size;
    vec2 texel_step;            // unused here
    vec2 inverse_target_size;   // unused here
    vec2 padding;
} u;

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out float frag_depth;

float fetch(ivec2 p)
{
    return texelFetch(source, min(p, u.source_size - 1), 0).r;
}

void main()
{
    ivec2 src = ivec2(gl_FragCoord.xy) * 2;
    float depth = min(min(fetch(src), fetch(src + ivec2(1, 0))), min(fetch(src + ivec2(0, 1)), fetch(src + ivec2(1, 1))));
    bool odd_x = (u.source_size.x & 1) == 1;
    bool odd_y = (u.source_size.y & 1) == 1;
    if (odd_x)
    {
        depth = min(depth, min(fetch(src + ivec2(2, 0)), fetch(src + ivec2(2, 1))));
    }
    if (odd_y)
    {
        depth = min(depth, min(fetch(src + ivec2(0, 2)), fetch(src + ivec2(1, 2))));
    }
    if (odd_x && odd_y)
    {
        depth = min(depth, fetch(src + ivec2(2, 2)));
    }
    frag_depth = depth;
}
)GLSL";

    // The blur pyramid (GCP's ssrDownsampleF.glsl without the Karis weights,
    // which only its experimental cone mips used): the centre four times and
    // four diagonal taps texel_step away.
    const char* const DOWNSAMPLE_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SsrChain
{
    ivec2 source_size;          // unused here
    vec2 texel_step;            // the diagonal taps' offset (source uv)
    vec2 inverse_target_size;
    vec2 padding;
} u;

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec2 uv = gl_FragCoord.xy * u.inverse_target_size;
    vec4 c = textureLod(source, uv, 0.0) * 4.0;
    c += textureLod(source, uv + vec2(-u.texel_step.x, -u.texel_step.y), 0.0);
    c += textureLod(source, uv + vec2(u.texel_step.x, -u.texel_step.y), 0.0);
    c += textureLod(source, uv + vec2(-u.texel_step.x, u.texel_step.y), 0.0);
    c += textureLod(source, uv + vec2(u.texel_step.x, u.texel_step.y), 0.0);
    frag_color = c * 0.125;
}
)GLSL";

    // The trace (LLRHI::SsrTrace; GCP's screenSpaceReflTraceF.glsl and its
    // screenSpaceReflUtil.glsl). The scene copy is the last frame's picture:
    // rays start in this frame's eye space and are carried into the copy's
    // (inverse_modelview_delta), then traced in the copy's screen.
    const char* const TRACE_BODY = R"GLSL(
layout(std140, set = 0, binding = 0) uniform SsrTrace
{
    mat4 inverse_projection;        // this frame: clip -> eye
    mat4 prev_projection;           // the scene copy's frame: eye -> clip
    mat4 inverse_prev_projection;
    mat4 inverse_modelview_delta;   // this frame's eye space -> the scene copy's
    vec4 trace;                     // x: max thickness (m), y: depth bias, z: max depth (m), w: max roughness
    vec4 rays;                      // x: the noise's phase, y: rays a pixel
    vec2 depth_to_ndc;              // ndc z = depth * x + y
    ivec2 size;                     // the target's
    ivec2 gbuffer_size;
    ivec2 scene_size;               // the scene copy's (the pyramid's level 0)
    int iterations;
    int hiz_levels;                 // the pyramid's levels, level 0 included
    int padding0;
    int padding1;
    mat4 self_box;                  // the copy's eye space -> your avatar's box (inside: -1..1)
    vec4 self_plane;                // your reflection's plane in this frame's eye space
    vec4 self_params;               // x its weight (0: off), y the plane's thickness (m), z the least normal match
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;       // this frame's G-buffer depth
layout(set = 0, binding = 2) uniform sampler2D specular_map;
layout(set = 0, binding = 3) uniform sampler2D normal_map;
layout(set = 0, binding = 4) uniform sampler2D scene_color;       // the scene copy (lit, HDR)
layout(set = 0, binding = 5) uniform sampler2D scene_copy_depth;  // its depth: the pyramid's level 0
layout(set = 0, binding = 6) uniform sampler2D hiz;               // the pyramid's levels 1.. as 0..

layout(location = 0) out vec4 frag_color;
#ifdef HIT_DISTANCE
// The hits' mean distance (m) from the surface along the ray, weighted as the
// colour is; 0 where nothing was hit (LLRHI::SsrTrace::hit_distance).
layout(location = 1) out float frag_hit_distance;
float hit_distance_sum = 0.0;
float hit_distance_weight = 0.0;
#endif

// Rays that pass behind a surface thicker than max thickness go on (missing
// it); after this many such surfaces a ray gives up (GCP's SSR_MAX_TUNNEL).
const int MAX_TUNNEL = 2;

float toNDC(float depth)
{
    return depth * u.depth_to_ndc.x + u.depth_to_ndc.y;
}

float fromNDC(float z)
{
    return (z - u.depth_to_ndc.y) / u.depth_to_ndc.x;
}

// This frame's eye space at a screen uv and stored depth.
vec3 eyeNow(vec2 uv, float depth)
{
    vec4 p = u.inverse_projection * vec4(uv * 2.0 - 1.0, toNDC(depth), 1.0);
    return p.xyz / p.w;
}

// The scene copy's eye space at its screen uv and stored depth. GCP uses this
// frame's projection for the copy's frame (a frame out while zooming).
vec3 eyeCopy(vec2 uv, float depth)
{
    vec4 p = u.inverse_prev_projection * vec4(uv * 2.0 - 1.0, toNDC(depth), 1.0);
    return p.xyz / p.w;
}

// A point of the copy's eye space in its screen: uv and stored depth.
vec3 projectCopy(vec3 p)
{
    vec4 clip = u.prev_projection * vec4(p, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    return vec3(ndc.xy * 0.5 + 0.5, fromNDC(ndc.z));
}

// GCP: sin-hash breaks down at pixel-scale args and correlates across seeds.
uvec2 pcg2d(uvec2 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v;
}

// Interleaved gradient noise, turned each frame by the golden ratio.
float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))) + u.rays.x * 0.61803398875);
}

// The pyramid at a cell of a level: the nearest depth in it.
float hizDepth(ivec2 cell, int level)
{
    return level == 0 ? texelFetch(scene_copy_depth, cell, 0).r : texelFetch(hiz, cell, level - 1).r;
}

// GCP's hiZTrace (after Godot's screen_space_reflection.glsl, parametric T).
// origin and dir in the copy's screen: (uv, stored depth). The hit (uv,
// stored depth, 1), or -1 on a miss.
vec4 hiZTrace(vec3 origin, vec3 dir)
{
    vec2 screen_res = vec2(u.scene_size);
    int max_level = u.hiz_levels - 1;

    if (abs(dir.z) < 1e-7)
    {
        dir.z = 1e-7;
    }
    // |dir.z| = 1: the depth's T compares directly with the cell edges' T.
    vec3 ray_dir = dir / abs(dir.z);
    // Towards the camera the stored depth falls (0 is near).
    bool facing_camera = dir.z < 0.0;
    float z_dir = ray_dir.z;
    vec2 cell_step = vec2(ray_dir.x < 0.0 ? -1.0 : 1.0, ray_dir.y < 0.0 ? -1.0 : 1.0);

    int tunnels = 0;
    int level = 0;

    // T at the screen's edge, where tracing stops.
    vec2 t0 = (vec2(0.0) - origin.xy) / ray_dir.xy;
    vec2 t1 = (vec2(1.0) - origin.xy) / ray_dir.xy;
    vec2 t2 = max(t0, t1);
    float t_max = min(t2.x, t2.y);

    // Out of the start's own cell, then a jittered part of a cell more
    // (the stair-step banding becomes noise).
    float t;
    {
        vec2 cell_index = floor(origin.xy * screen_res);
        vec2 new_cell_index = cell_index + clamp(cell_step, vec2(0.0), vec2(1.0));
        vec2 new_cell_pos = new_cell_index / screen_res + cell_step * 0.000001;
        vec2 pos_t = (new_cell_pos - origin.xy) / ray_dir.xy;
        t = min(pos_t.x, pos_t.y);
        vec2 cell_t = 1.0 / (screen_res * max(abs(ray_dir.xy), vec2(1e-6)));
        t += ign(gl_FragCoord.xy) * min(cell_t.x, cell_t.y);
    }

    for (int i = 0; i < u.iterations && level >= 0 && t < t_max; i++)
    {
        vec3 pos = origin + ray_dir * t;

        ivec2 cell_count = max(ivec2(1), u.scene_size >> level);
        ivec2 cell_index = clamp(ivec2(floor(pos.xy * vec2(cell_count))), ivec2(0), cell_count - 1);
        float cell_depth = hizDepth(cell_index, level);

        float depth_t = (cell_depth - origin.z) * z_dir;

        vec2 new_cell_index = vec2(cell_index) + clamp(cell_step, vec2(0.0), vec2(1.0));
        vec2 new_cell_pos = new_cell_index / vec2(cell_count) + cell_step * 0.000001;
        vec2 pos_t = (new_cell_pos - origin.xy) / ray_dir.xy;
        float edge_t = min(pos_t.x, pos_t.y);

        // Away from the camera: a hit when the surface comes before the cell's
        // edge. Towards it: when the ray has not passed the surface yet.
        bool hit = facing_camera ? (t <= depth_t) : (depth_t <= edge_t);
        int level_step = hit ? -1 : 1;

        // At level 0, a surface more than max thickness in front of the ray
        // is passed behind (a thin object), up to MAX_TUNNEL times.
        if (level == 0 && hit)
        {
            float z0 = eyeCopy(pos.xy, cell_depth).z;
            float z1 = eyeCopy(pos.xy, pos.z).z;
            if (z0 - z1 > u.trace.x)
            {
                tunnels++;
                if (tunnels > MAX_TUNNEL)
                {
                    return vec4(-1.0);
                }
                hit = false;
                level_step = 1;
            }
        }

        if (hit)
        {
            if (!facing_camera)
            {
                t = max(t, depth_t);
            }
        }
        else
        {
            t = edge_t;
        }
        level = min(level + level_step, max_level);
    }

    // A ray that ran out of steps before it came down to level 0 missed
    // (GCP's comment says so; its code took wherever the ray stopped, which
    // the confidence test below then mostly turned down).
    if (level >= 0)
    {
        return vec4(-1.0);
    }

    vec3 hit_pos = origin + ray_dir * t;
    if (hit_pos.x < 0.0 || hit_pos.x > 1.0 || hit_pos.y < 0.0 || hit_pos.y > 1.0 || t >= t_max)
    {
        return vec4(-1.0);
    }
    return vec4(hit_pos, 1.0);
}

float edgeFade(vec2 p)
{
    vec2 fade = smoothstep(0.85, 1.0, abs(p * 2.0 - 1.0));
    return 1.0 - max(fade.x, fade.y);
}
)GLSL";

    // The trace's rays and main, after TRACE_BODY (one literal would pass the
    // compiler's 16 KB limit on a string).
    const char* const TRACE_MAIN = R"GLSL(
// Your avatar's reflection in the floor (plus/avatarrefl) shows your avatar on
// its plane; the scene copy holds your avatar too (the camera's side, a frame
// late, blurred), and both showed (research/210 D1). How much of a hit on your
// avatar to drop here: the reflection's own weight at this pixel
// (reflectionProbeF.glsl tapAvatarReflection: on the plane, full from a
// glossiness of 0.9, none at 0.7). Elsewhere (walls, windows) 0: the rays
// still show you there.
float selfSkip(vec3 view_pos, vec3 normal, float roughness)
{
    if (u.self_params.x <= 0.0)
    {
        return 0.0;
    }
    float dist = dot(view_pos, u.self_plane.xyz) + u.self_plane.w;
    if (abs(dist) > u.self_params.y || dot(normal, u.self_plane.xyz) < u.self_params.z)
    {
        return 0.0;
    }
    return u.self_params.x * clamp((0.3 - roughness) * 5.0, 0.0, 1.0);
}

// A hit's share: 1, or 1 - skip inside your avatar's box (the copy's eye space).
float selfKeep(float skip, vec3 hit)
{
    if (skip <= 0.0)
    {
        return 1.0;
    }
    vec3 b = (u.self_box * vec4(hit, 1.0)).xyz;
    return all(lessThanEqual(abs(b), vec3(1.0))) ? 1.0 - skip : 1.0;
}

// GCP's tapScreenSpaceReflection: rgb the hits' colours (each compressed by
// c / (1 + luma)) times their weights, a the weights, both averaged over the
// rays and faded at the screen's edge.
vec4 traceReflection(vec2 tc, vec3 view_pos, vec3 n, float roughness)
{
    if (roughness >= u.trace.w)
    {
        return vec4(0.0);
    }
    vec3 view_dir = normalize(view_pos);
    vec3 normal = normalize(n);
    if (dot(-view_dir, normal) <= 0.0)
    {
        return vec4(0.0);
    }
    vec2 dist = abs(tc * 2.0 - 1.0);
    float base_edge_fade = 1.0 - smoothstep(0.85, 1.0, max(dist.x, dist.y));
    if (base_edge_fade <= 0.001)
    {
        return vec4(0.0);
    }

    // The start pulled back along the normal by the distance (grazing rays
    // must not scrape their own surface where the depth's precision is low).
    float bias = max(0.01, -view_pos.z * u.trace.y);
    vec3 biased_pos = view_pos - normal * bias;
    vec3 copy_pos = (u.inverse_modelview_delta * vec4(biased_pos, 1.0)).xyz;
    if (-copy_pos.z > u.trace.z)
    {
        return vec4(0.0);
    }

    // Then pushed off along the normal for the ray's origin (not random, so no noise).
    float clearance = max(0.05, -view_pos.z * 0.002);
    vec3 copy_origin = (u.inverse_modelview_delta * vec4(biased_pos + normal * clearance, 1.0)).xyz;
    vec3 ss_origin = projectCopy(copy_origin);

    vec3 perfect = normalize(reflect(view_dir, normal));
    float self_skip = selfSkip(view_pos, normal, roughness);
#ifdef HIT_DISTANCE
    vec3 copy_surface = (u.inverse_modelview_delta * vec4(view_pos, 1.0)).xyz;   // the distances are from here
#endif
    // Below this roughness the rays are not jittered: one is enough.
    int samples = roughness <= 0.001 ? 1 : max(1, int(u.rays.y));
    uint phase = uint(u.rays.x);

    vec3 accum_color = vec3(0.0);
    float accum_fade = 0.0;
    int hits = 0;
    for (int s = 0; s < samples; s++)
    {
        vec3 reflect_dir = perfect;
        if (roughness > 0.001)
        {
            // Importance-sampled GGX.
            float alpha = roughness * roughness;
            uvec2 hash = pcg2d(uvec2(gl_FragCoord.xy) + uvec2(phase * 7919u + uint(s) * 104729u, phase * 6271u + uint(s) * 15013u));
            float u1 = float(hash.x) * 2.3283064365386963e-10;
            float u2 = float(hash.y) * 2.3283064365386963e-10;
            float theta = atan(alpha * sqrt(u1) / sqrt(1.0 - u1));
            float phi = 6.28318530718 * u2;
            vec3 up = abs(reflect_dir.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 tangent = normalize(cross(up, reflect_dir));
            vec3 bitangent = cross(reflect_dir, tangent);
            vec3 h = normalize(sin(theta) * cos(phi) * tangent + sin(theta) * sin(phi) * bitangent + cos(theta) * reflect_dir);
            reflect_dir = normalize(reflect(-reflect_dir, h));
        }

        vec3 copy_target = (u.inverse_modelview_delta * vec4(view_pos + reflect_dir, 1.0)).xyz;
        vec3 copy_dir = normalize(copy_target - copy_pos);
        if (copy_dir.z >= 0.5)
        {
            continue;   // back towards the camera: not in the picture
        }
        float camera_facing_fade = 1.0 - smoothstep(0.45, 0.5, copy_dir.z);

        // A short step (a share of the distance) keeps the end in front of the camera.
        float step_dist = max(0.1, -copy_origin.z * 0.1);
        vec3 ss_far = projectCopy(copy_origin + copy_dir * step_dist);
        vec4 result = hiZTrace(ss_origin, ss_far - ss_origin);
        if (result.x < 0.0)
        {
            continue;
        }

        vec2 hit_tc = result.xy;
        ivec2 hit_pixel = clamp(ivec2(hit_tc * vec2(u.scene_size)), ivec2(0), u.scene_size - 1);
        float hit_surface_depth = texelFetch(scene_copy_depth, hit_pixel, 0).r;
        // The sky is the probes' (GCP let it through when the draw distance
        // is shorter than max depth).
        if (hit_surface_depth >= 1.0)
        {
            continue;
        }
        vec3 surface = eyeCopy(hit_tc, hit_surface_depth);
        float hit_depth = -surface.z;
        if (hit_depth > u.trace.z)
        {
            continue;
        }

        // How close the ray's end is to the surface there.
        float confidence = 1.0 - smoothstep(0.0, u.trace.x, length(surface - eyeCopy(hit_tc, result.z)));
        confidence *= confidence;
        if (confidence < 0.95)
        {
            continue;
        }
        float keep = selfKeep(self_skip, surface);
        if (keep <= 0.0)
        {
            continue;   // your avatar, which your reflection shows here
        }

        // A ray of under 3 pixels: turned down when it leaves the hit
        // surface (its own surface again). GCP compared this frame's
        // direction with the copy's normal; both are the copy's here.
        float ray_len = length(result.xy - ss_origin.xy);
        if (ray_len * float(max(u.scene_size.x, u.scene_size.y)) < 3.0)
        {
            ivec2 right = min(hit_pixel + ivec2(1, 0), u.scene_size - 1);
            ivec2 above = min(hit_pixel + ivec2(0, 1), u.scene_size - 1);
            vec3 pos_r = eyeCopy((vec2(right) + 0.5) / vec2(u.scene_size), texelFetch(scene_copy_depth, right, 0).r);
            vec3 pos_a = eyeCopy((vec2(above) + 0.5) / vec2(u.scene_size), texelFetch(scene_copy_depth, above, 0).r);
            vec3 geom_normal = cross(pos_r - surface, pos_a - surface);
            if (dot(geom_normal, geom_normal) > 1e-12)
            {
                geom_normal = normalize(geom_normal.z < 0.0 ? -geom_normal : geom_normal);
                if (dot(copy_dir, geom_normal) >= 0.0)
                {
                    continue;
                }
            }
        }

        float z_fade = 1.0 - smoothstep(u.trace.z * 0.8, u.trace.z, hit_depth);
        vec4 sampled = textureLod(scene_color, hit_tc, 0.0);
        if (any(isnan(sampled.rgb)) || any(isinf(sampled.rgb)))
        {
            continue;   // nothing that is not a number goes into the blur, the history or the lighting
        }
        // Compressed before any averaging (no fireflies).
        sampled.rgb /= 1.0 + dot(sampled.rgb, vec3(0.2126, 0.7152, 0.0722));
        // Faded in from the origin (self-intersection) and out at long rays.
        float fade_in = smoothstep(0.0, 0.01, ray_len);
        float ray_fade = 1.0 - smoothstep(0.6, 1.0, ray_len);
        float weight = edgeFade(hit_tc) * z_fade * fade_in * ray_fade * confidence * camera_facing_fade * keep;

        accum_color += sampled.rgb * weight;
        accum_fade += weight;
        hits++;
#ifdef HIT_DISTANCE
        hit_distance_sum += length(surface - copy_surface) * weight;   // the camera's motion keeps lengths
        hit_distance_weight += weight;
#endif
    }

    if (hits == 0)
    {
        return vec4(0.0);
    }
    accum_color /= float(samples);
    accum_fade /= float(samples);
    return vec4(accum_color * base_edge_fade, accum_fade * base_edge_fade);
}

void main()
{
    frag_color = vec4(0.0);
#ifdef HIT_DISTANCE
    frag_hit_distance = 0.0;
#endif
    vec2 uv = gl_FragCoord.xy / vec2(u.size);
    ivec2 g = clamp(ivec2(uv * vec2(u.gbuffer_size)), ivec2(0), u.gbuffer_size - 1);
    float depth = texelFetch(scene_depth, g, 0).r;
    if (depth >= 1.0)
    {
        return;     // the sky
    }
    vec4 normal_info = texelFetch(normal_map, g, 0);
    bool pbr;
    float roughness = surfaceRoughness(texelFetch(specular_map, g, 0), normal_info, pbr);
    if (roughness < 0.0)
    {
        return;
    }
    vec2 g_uv = (vec2(g) + 0.5) / vec2(u.gbuffer_size);
    frag_color = traceReflection(g_uv, eyeNow(g_uv, depth), decodeNormal(normal_info.xy), roughness);
#ifdef HIT_DISTANCE
    frag_hit_distance = hit_distance_weight > 0.0 ? hit_distance_sum / hit_distance_weight : 0.0;
#endif
}
)GLSL";

    // The temporal accumulation (LLRHI::SsrResolve; GCP's
    // screenSpaceReflResolveF.glsl): this frame's one ray a pixel and the
    // history moved with the camera (not with what moved in the picture),
    // clipped to the 3x3 neighbourhood's spread (Salvi's variance clipping).
    // In the trace's encoding (premultiplied, compressed), alpha blended too.
    // RESOLVE_COMMON + RESOLVE_MAIN is that pass as it was before research/294;
    // with REFLECTION_MOTION defined, RESOLVE_COMMON + RESOLVE_MOTION_MAIN
    // (LLRHI::SsrResolve::hit_distance_texture) takes the history where the
    // reflection was rather than the surface.
    const char* const RESOLVE_COMMON = R"GLSL(
layout(std140, set = 0, binding = 0) uniform SsrResolve
{
    mat4 inverse_projection;        // this frame: clip -> eye
    mat4 prev_projection;           // the scene copy's (the history's) frame: eye -> clip
    mat4 inverse_prev_projection;
    mat4 inverse_modelview_delta;   // this frame's eye space -> the scene copy's
    vec4 history;                   // x: the history's weight (0: none), y: its share on avatars,
                                    // z: the disocclusion threshold, w: 1 with the avatar mask
    vec2 depth_to_ndc;              // ndc z = depth * x + y
    ivec2 size;                     // the target's (the trace's, the history's)
    ivec2 gbuffer_size;
    ivec2 scene_size;
#ifdef REFLECTION_MOTION
    mat4 projection;                // this frame: eye -> clip
    vec4 motion;                    // x: the clip box's half width, in the 5x5's standard deviations
#endif
} u;

layout(set = 0, binding = 1) uniform sampler2D current_trace;     // this frame's
layout(set = 0, binding = 2) uniform sampler2D history_map;       // the last frame's result (bilinear)
layout(set = 0, binding = 3) uniform sampler2D scene_depth;       // this frame's G-buffer depth
layout(set = 0, binding = 4) uniform sampler2D specular_map;
layout(set = 0, binding = 5) uniform sampler2D normal_map;
layout(set = 0, binding = 6) uniform sampler2D scene_copy_depth;  // the scene copy's depth: what the last frame saw
layout(set = 0, binding = 7) uniform sampler2D avatar_mask;       // 1 on avatars (the G-buffer's size; history.w 1)
#ifdef REFLECTION_MOTION
layout(set = 0, binding = 8) uniform sampler2D hit_distance;      // this frame's trace's (m from the surface; 0: no hit)
#endif

layout(location = 0) out vec4 frag_color;

float toNDC(float depth)
{
    return depth * u.depth_to_ndc.x + u.depth_to_ndc.y;
}

vec3 eyeNow(vec2 uv, float depth)
{
    vec4 p = u.inverse_projection * vec4(uv * 2.0 - 1.0, toNDC(depth), 1.0);
    return p.xyz / p.w;
}

vec3 eyeCopy(vec2 uv, float depth)
{
    vec4 p = u.inverse_prev_projection * vec4(uv * 2.0 - 1.0, toNDC(depth), 1.0);
    return p.xyz / p.w;
}

// One scalar for all four channels: clamping each on its own would break
// the premultiplied rgb / a ratios.
vec4 clipToAABB(vec4 v, vec4 mn, vec4 mx)
{
    vec4 center = 0.5 * (mx + mn);
    vec4 extent = 0.5 * (mx - mn) + 1e-4;
    vec4 delta = v - center;
    vec4 ts = abs(delta) / extent;
    float t = max(max(ts.x, ts.y), max(ts.z, ts.w));
    return t > 1.0 ? center + delta / t : v;
}

// Five-tap Catmull-Rom: bilinear refetching blurs more each frame.
vec4 sampleHistoryCatmullRom(vec2 uv, vec2 tex_size)
{
    vec2 sample_pos = uv * tex_size;
    vec2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
    vec2 f = sample_pos - tex_pos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 tex_pos0 = (tex_pos1 - 1.0) / tex_size;
    vec2 tex_pos3 = (tex_pos1 + 2.0) / tex_size;
    vec2 tex_pos12 = (tex_pos1 + offset12) / tex_size;

    vec4 result = vec4(0.0);
    float weight = 0.0;
    result += textureLod(history_map, vec2(tex_pos12.x, tex_pos0.y), 0.0) * (w12.x * w0.y);
    weight += w12.x * w0.y;
    result += textureLod(history_map, vec2(tex_pos0.x, tex_pos12.y), 0.0) * (w0.x * w12.y);
    weight += w0.x * w12.y;
    result += textureLod(history_map, tex_pos12, 0.0) * (w12.x * w12.y);
    weight += w12.x * w12.y;
    result += textureLod(history_map, vec2(tex_pos3.x, tex_pos12.y), 0.0) * (w3.x * w12.y);
    weight += w3.x * w12.y;
    result += textureLod(history_map, vec2(tex_pos12.x, tex_pos3.y), 0.0) * (w12.x * w3.y);
    weight += w12.x * w3.y;
    return max(result / weight, vec4(0.0));
}

// How surely this surface was hidden in the last frame (1) or seen (0): its
// view depth then against the scene copy's there, the nearest and the
// farthest of the 2x2 texels around (a slope's depth spread is no reason).
// GCP kept the history where the camera's motion uncovered a surface.
float hidden(vec2 prev_uv, float expected)
{
    vec2 p = prev_uv * vec2(u.scene_size) - 0.5;
    ivec2 base = ivec2(floor(p));
    float nearest = 1e30;
    float farthest = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 texel = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), u.scene_size - 1);
        float z = -eyeCopy((vec2(texel) + 0.5) / vec2(u.scene_size), texelFetch(scene_copy_depth, texel, 0).r).z;
        nearest = min(nearest, z);
        farthest = max(farthest, z);
    }
    float outside = max(max(nearest - expected, expected - farthest), 0.0);
    float relative = outside / max(expected, 0.1);
    return smoothstep(u.history.z, 2.0 * u.history.z, relative);
}
)GLSL";

    const char* const RESOLVE_MAIN = R"GLSL(
void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(current_trace, px, 0);
    vec2 uv = gl_FragCoord.xy / vec2(u.size);
    ivec2 g = clamp(ivec2(uv * vec2(u.gbuffer_size)), ivec2(0), u.gbuffer_size - 1);
    float depth = texelFetch(scene_depth, g, 0).r;
    if (depth >= 1.0)
    {
        frag_color = cur;   // the sky
        return;
    }
    bool pbr;
    float roughness = surfaceRoughness(texelFetch(specular_map, g, 0), texelFetch(normal_map, g, 0), pbr);
    if (roughness < 0.0)
    {
        roughness = 1.0;
    }

    // The 3x3 moments: the mean also reuses the neighbours' rays, the spread bounds the history.
    vec4 m1 = vec4(0.0);
    vec4 m2 = vec4(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec4 s = texelFetch(current_trace, clamp(px + ivec2(x, y), ivec2(0), u.size - 1), 0);
            m1 += s;
            m2 += s * s;
        }
    }
    vec4 mu = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mu * mu, vec4(0.0)));

    // Where the lobe is wide the neighbours' rays count; a mirror keeps its own.
    vec4 filtered = mix(cur, mu, smoothstep(0.0, 0.2, roughness));
    frag_color = filtered;

    float w = u.history.x;
    if (w <= 0.0)
    {
        return;
    }

    // Where this surface was in the last frame's picture (the camera's motion only).
    vec2 g_uv = (vec2(g) + 0.5) / vec2(u.gbuffer_size);
    vec3 prev_pos = (u.inverse_modelview_delta * vec4(eyeNow(g_uv, depth), 1.0)).xyz;
    vec4 prev_clip = u.prev_projection * vec4(prev_pos, 1.0);
    if (prev_clip.w <= 0.0)
    {
        return;
    }
    vec2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0))))
    {
        return;
    }
    w *= 1.0 - hidden(prev_uv, -prev_pos.z);
    // An avatar moves (its skin, its clothes) where the camera's motion does
    // not take it: less of the history on it. GCP has no motion of its own
    // for the reflections (its velocity buffer feeds SMAA T2x only).
    if (u.history.w > 0.0)
    {
        w *= mix(1.0, u.history.y, texelFetch(avatar_mask, g, 0).r);
    }
    if (w <= 0.0)
    {
        return;
    }

    vec4 hist = clipToAABB(sampleHistoryCatmullRom(prev_uv, vec2(textureSize(history_map, 0))), mu - sigma * 1.25, mu + sigma * 1.25);
    if (any(isnan(hist)) || any(isinf(hist)))
    {
        return;     // never feed a bad texel back
    }
    frag_color = mix(filtered, hist, w);
}
)GLSL";

    // research/294 S1 and S2 (PlusSSRReflectionMotion). A reflection is not
    // where its surface is: what a mirror shows lies behind it, on the view
    // ray, the ray's length beyond the surface (its mirror image), and slides
    // over the surface as the camera moves, the more the farther it is. The
    // last frame's picture of it is where that image was: the history is taken
    // there as surely as the last frame's picture shows this surface there
    // (its plane), the rest where the surface was (as RESOLVE_MAIN, which a
    // miss and a hit on an avatar also fall back to). The hits' distance is
    // the pixel's own (where it missed, its nearest neighbour hit's), smoothed
    // only with neighbours at about the same distance (not across a depth step).
    // The clip box comes from the 5x5 (a steadier spread than the 3x3's, N3),
    // and where the history was lost (off the picture, uncovered, no history)
    // the 3x3's mean stands in for the pixel's own ray where that ray is
    // noisy: not on near mirrors, whose rays barely leave the pixel.
    const char* const RESOLVE_MOTION_MAIN = R"GLSL(
// A hit weight at or below this does not make a distance the reference (the
// pixel's own: it missed; a neighbour's: not picked), and a smoothing weight
// sum at or below it keeps the reference. Weak hits still join the smoothing
// by their own (small) weight.
const float MIN_HIT_WEIGHT = 0.001;
// Neighbours whose distance differs from the pixel's by more than the first
// share of it count less, none from the second.
const vec2 DISTANCE_MATCH = vec2(0.1, 0.25);
// The trace jitters the half vector by about atan(alpha) (the GGX alpha,
// roughness squared; the median of its sample), so the reflected ray by about
// twice that. Lost history leans on the neighbours from where that jitter is
// the first number of the trace's pixels (the rays start to leave the pixel),
// fully from the second. A pixel's angle comes from this frame's projection
// and the target's height (the resolution multiplier, the window, the zoom).
const vec2 NOISY_JITTER_PIXELS = vec2(1.0, 6.0);
// The thickness, as a share of the view depth, that keeps a point in front of
// a hit from being taken for it (at least the second, in metres).
const vec2 IN_FRONT_OF_HIT = vec2(0.1, 0.25);

// Where a point of this frame's eye space was in the last frame's picture;
// false off the picture or behind the camera.
bool lastPlace(vec3 now, out vec2 prev_uv, out vec3 prev_pos)
{
    prev_pos = (u.inverse_modelview_delta * vec4(now, 1.0)).xyz;
    vec4 prev_clip = u.prev_projection * vec4(prev_pos, 1.0);
    prev_uv = vec2(0.0);
    if (prev_clip.w <= 0.0)
    {
        return false;
    }
    prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
    return all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)));
}

// This frame's avatar mask where a hit (a point of this frame's eye space) is:
// 0 off the screen, and 0 where this frame's G-buffer shows something well in
// front of it there (an avatar standing before the reflected wall is not
// what the ray hit).
float avatarAt(vec3 hit)
{
    vec4 clip = u.projection * vec4(hit, 1.0);
    if (clip.w <= 0.0)
    {
        return 0.0;
    }
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return 0.0;
    }
    ivec2 g = clamp(ivec2(uv * vec2(u.gbuffer_size)), ivec2(0), u.gbuffer_size - 1);
    float seen_depth = -eyeNow((vec2(g) + 0.5) / vec2(u.gbuffer_size), texelFetch(scene_depth, g, 0).r).z;
    float hit_depth = -hit.z;
    if (seen_depth < hit_depth - max(IN_FRONT_OF_HIT.x * hit_depth, IN_FRONT_OF_HIT.y))
    {
        return 0.0;
    }
    return texelFetch(avatar_mask, g, 0).r;
}

// How surely the last frame's picture at prev_uv shows a point of this
// surface's plane (1) or another surface (0), in the same shape as hidden()
// (from the disocclusion share, fading out up to twice it). hidden() compares
// view depths; a point's distance from the plane is the difference of
// distances along its view ray times |n.v| (v that ray), so the limit is too:
// on a floor seen at a slant a thing standing on it is not taken for the
// floor. The limit takes the view depth (-z), not the distance along the ray:
// up to about 1.5 times narrower at the screen's corners, where the surface's
// place takes the rest.
// prev_surface and prev_normal: the surface in the scene copy's eye space.
float samePlane(vec2 prev_uv, vec3 prev_surface, vec3 prev_normal)
{
    ivec2 texel = clamp(ivec2(prev_uv * vec2(u.scene_size)), ivec2(0), u.scene_size - 1);
    vec3 seen = eyeCopy((vec2(texel) + 0.5) / vec2(u.scene_size), texelFetch(scene_copy_depth, texel, 0).r);
    float slant = max(abs(dot(normalize(seen), prev_normal)), 0.05);
    float limit = u.history.z * max(-seen.z, 0.1) * slant;
    return 1.0 - smoothstep(limit, 2.0 * limit, abs(dot(seen - prev_surface, prev_normal)));
}

// The trace's one ray's jitter on a surface of this roughness, in the
// trace's pixels (at the screen's middle).
float jitterPixels(float roughness)
{
    float pixel_angle = 2.0 / max(u.projection[1][1] * float(u.size.y), 1e-3);
    return 2.0 * atan(roughness * roughness) / pixel_angle;
}

// The hits' distance (m) for this pixel from the 3x3's (index 4 is the
// pixel's own; a distance is above 0 wherever its weight is above
// MIN_HIT_WEIGHT): the pixel's own where it hit, else the nearest of the
// neighbours' hits (a distance something is at, as TAA takes the nearest
// depth: a mean could fall between a near and a far surface, and the weights,
// which the rays' jitter turns each frame, would pick a side at random),
// smoothed with the neighbours at about that distance only. 0: no hit.
float hitDistance(float dist[9], float alpha[9])
{
    float reference = alpha[4] > MIN_HIT_WEIGHT ? dist[4] : 0.0;
    if (reference <= 0.0)
    {
        for (int i = 0; i < 9; ++i)
        {
            if (alpha[i] > MIN_HIT_WEIGHT && (reference <= 0.0 || dist[i] < reference))
            {
                reference = dist[i];
            }
        }
    }
    if (!(reference > 0.0))
    {
        return 0.0;     // no hit around (NaN too)
    }
    float sum = 0.0;
    float weight = 0.0;
    for (int i = 0; i < 9; ++i)
    {
        float match = 1.0 - smoothstep(DISTANCE_MATCH.x, DISTANCE_MATCH.y, abs(dist[i] - reference) / max(reference, 0.01));
        sum += dist[i] * alpha[i] * match;
        weight += alpha[i] * match;
    }
    float d = weight > MIN_HIT_WEIGHT ? sum / weight : reference;
    return (d > 0.0 && !isinf(d)) ? d : 0.0;     // NaN too
}

// The two places the history is taken from: where the reflection's mirror
// image was (image_uv, image_share of it) and where the surface was
// (surface_uv, the rest, of which surface_kept is still good: off the picture
// or uncovered take it away).
struct HistoryPlaces
{
    vec2 image_uv;
    float image_share;
    vec2 surface_uv;
    float surface_kept;
};

// on_avatar: the avatar mask at the surface or at the hit.
HistoryPlaces historyPlaces(vec3 now, vec3 normal, float hit_len, ivec2 g, out float on_avatar)
{
    HistoryPlaces p;
    p.image_uv = vec2(0.0);
    p.image_share = 0.0;
    p.surface_uv = vec2(0.0);
    p.surface_kept = 0.0;
    vec3 prev_pos;
    bool masked = u.history.w > 0.0;
    on_avatar = masked ? texelFetch(avatar_mask, g, 0).r : 0.0;
    if (hit_len > 0.0)
    {
        vec3 view_dir = normalize(now);
        // An avatar's reflection moves on its own: the surface's place, less history (as RESOLVE_MAIN).
        float hit_on_avatar = masked ? avatarAt(now + reflect(view_dir, normal) * hit_len) : 0.0;
        on_avatar = max(on_avatar, hit_on_avatar);
        if (hit_on_avatar < 0.5 && lastPlace(now + view_dir * hit_len, p.image_uv, prev_pos))
        {
            p.image_share = samePlane(p.image_uv, (u.inverse_modelview_delta * vec4(now, 1.0)).xyz,
                                      mat3(u.inverse_modelview_delta) * normal);
        }
    }
    if (p.image_share < 1.0 && lastPlace(now, p.surface_uv, prev_pos))
    {
        p.surface_kept = 1.0 - hidden(p.surface_uv, -prev_pos.z);
    }
    return p;
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(current_trace, px, 0);
    vec2 uv = gl_FragCoord.xy / vec2(u.size);
    ivec2 g = clamp(ivec2(uv * vec2(u.gbuffer_size)), ivec2(0), u.gbuffer_size - 1);
    float depth = texelFetch(scene_depth, g, 0).r;
    if (depth >= 1.0)
    {
        frag_color = cur;   // the sky
        return;
    }
    vec4 normal_info = texelFetch(normal_map, g, 0);
    bool pbr;
    float roughness = surfaceRoughness(texelFetch(specular_map, g, 0), normal_info, pbr);
    if (roughness < 0.0)
    {
        roughness = 1.0;
    }

    // The 3x3's mean (what the pixel's own ray is mixed with, as in
    // RESOLVE_MAIN) and its hits' distances; the 5x5's moments for the clip box.
    vec4 inner = vec4(0.0);
    vec4 m1 = vec4(0.0);
    vec4 m2 = vec4(0.0);
    float dist[9];
    float alpha[9];
    int k = 0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), u.size - 1);
            vec4 s = texelFetch(current_trace, q, 0);
            m1 += s;
            m2 += s * s;
            if (abs(x) <= 1 && abs(y) <= 1)
            {
                inner += s;
                float d = texelFetch(hit_distance, q, 0).r;
                bool good = d > 0.0 && !isinf(d);      // NaN too
                dist[k] = good ? d : 0.0;
                alpha[k] = good ? s.a : 0.0;
                ++k;
            }
        }
    }
    vec4 mu = inner / 9.0;
    vec4 box_mu = m1 / 25.0;
    vec4 box_sigma = sqrt(max(m2 / 25.0 - box_mu * box_mu, vec4(0.0)));

    float w = u.history.x;
    float on_avatar = 0.0;
    HistoryPlaces places;
    places.image_uv = vec2(0.0);
    places.image_share = 0.0;
    places.surface_uv = vec2(0.0);
    places.surface_kept = 0.0;
    if (w > 0.0)
    {
        vec2 g_uv = (vec2(g) + 0.5) / vec2(u.gbuffer_size);
        places = historyPlaces(eyeNow(g_uv, depth), normalize(decodeNormal(normal_info.xy)), hitDistance(dist, alpha), g, on_avatar);
    }
    float surface_share = (1.0 - places.image_share) * places.surface_kept;
    float kept = places.image_share + surface_share;     // the share of the history still good

    // Where the lobe is wide the neighbours' rays count; a mirror keeps its
    // own. Where the history was lost they stand in for it too, but not on a
    // near mirror (its rays barely leave the pixel, and a blur would stay in
    // the history for many frames).
    float lost_share = (1.0 - kept) * smoothstep(NOISY_JITTER_PIXELS.x, NOISY_JITTER_PIXELS.y, jitterPixels(roughness));
    vec4 filtered = mix(cur, mu, max(smoothstep(0.0, 0.2, roughness), lost_share));
    frag_color = filtered;

    // An avatar moves (its skin, its clothes) where the camera's motion does not take it: less of the history on it.
    w *= kept * mix(1.0, u.history.y, on_avatar);
    if (w <= 0.0)
    {
        return;
    }

    vec2 history_size = vec2(textureSize(history_map, 0));
    vec4 hist = vec4(0.0);
    if (places.image_share > 0.0)
    {
        hist += sampleHistoryCatmullRom(places.image_uv, history_size) * places.image_share;
    }
    if (surface_share > 0.0)
    {
        hist += sampleHistoryCatmullRom(places.surface_uv, history_size) * surface_share;
    }
    float clip = u.motion.x;
    hist = clipToAABB(hist / kept, box_mu - box_sigma * clip, box_mu + box_sigma * clip);
    if (any(isnan(hist)) || any(isinf(hist)))
    {
        return;     // never feed a bad texel back
    }
    frag_color = mix(filtered, hist, w);
}
)GLSL";

    // The avatar mask (LLRHI::SsrAvatarMask): a constant where the stencil
    // test passes; two pipelines (avatars 1, the rest 0) cover every pixel.
    const char* const MASK_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SsrMask
{
    vec4 value;
} u;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = u.value;
}
)GLSL";

    // The filter (LLRHI::SsrFilter): what the lighting reads. GCP blurs its
    // SSR buffer into its own mip levels and the lighting reads the level the
    // roughness asks for (reflectionProbeF.glsl: PBR lod = roughness * mip
    // scale; legacy log2(1 + roughness^2 * mip scale * 4)). Here the pyramid
    // is the backend's and this pass reads it for each pixel of the source
    // once, into a target the lighting reads at level 0.
    const char* const FILTER_BODY = R"GLSL(
layout(std140, set = 0, binding = 0) uniform SsrFilter
{
    ivec2 size;             // the target's (the source's)
    ivec2 gbuffer_size;
    float mip_scale;        // the source's whole mip chain's levels less one (GCP's ssrMipScale)
    float top_level;        // the highest level built (0: no pyramid)
    float max_roughness;    // the trace's limit
    float fade_width;       // the reflection fades out over this much roughness below it
} u;

layout(set = 0, binding = 1) uniform sampler2D source;          // level 0
layout(set = 0, binding = 2) uniform sampler2D pyramid;         // levels 1.. as 0..
layout(set = 0, binding = 3) uniform sampler2D scene_depth;
layout(set = 0, binding = 4) uniform sampler2D specular_map;
layout(set = 0, binding = 5) uniform sampler2D normal_map;

layout(location = 0) out vec4 frag_color;

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy / vec2(u.size);
    ivec2 g = clamp(ivec2(uv * vec2(u.gbuffer_size)), ivec2(0), u.gbuffer_size - 1);
    vec4 normal_info = texelFetch(normal_map, g, 0);
    bool pbr;
    float roughness = surfaceRoughness(texelFetch(specular_map, g, 0), normal_info, pbr);
    if (roughness < 0.0 || texelFetch(scene_depth, g, 0).r >= 1.0)
    {
        frag_color = vec4(0.0);
        return;
    }
    float lod = pbr ? roughness * u.mip_scale : clamp(log2(1.0 + roughness * roughness * u.mip_scale * 4.0), 0.0, u.mip_scale);
    lod = min(lod, u.top_level);
    vec4 c = texelFetch(source, px, 0);
    if (lod > 0.0)
    {
        c = lod < 1.0 ? mix(c, textureLod(pyramid, uv, 0.0), lod) : textureLod(pyramid, uv, lod - 1.0);
    }
    // GCP fades the reflection out between roughness 0.6 and 0.7 whatever its
    // limit (RenderScreenSpaceReflectionMaxRoughness): below the limit here,
    // so a lower limit does not cut it off. rgb and a alike (premultiplied).
    frag_color = c * (1.0 - smoothstep(u.max_roughness - u.fade_width, u.max_roughness, roughness));
}
)GLSL";

    // ------------------------------------------------------------------
    // Uniform blocks, as the shaders' std140 blocks lay them out.

    struct ChainUniforms
    {
        S32 source_size[2];
        F32 texel_step[2];
        F32 inverse_target_size[2];
        F32 padding[2];
    };
    static_assert(sizeof(ChainUniforms) == 32, "ChainUniforms must match the std140 block SsrChain");

    struct TraceUniforms
    {
        F32 inverse_projection[16];
        F32 prev_projection[16];
        F32 inverse_prev_projection[16];
        F32 inverse_modelview_delta[16];
        F32 trace[4];
        F32 rays[4];
        F32 depth_to_ndc[2];
        S32 size[2];
        S32 gbuffer_size[2];
        S32 scene_size[2];
        S32 iterations;
        S32 hiz_levels;
        S32 padding[2];
        F32 self_box[16];
        F32 self_plane[4];
        F32 self_params[4];
    };
    static_assert(sizeof(TraceUniforms) == 432, "TraceUniforms must match the std140 block SsrTrace");

    struct ResolveUniforms
    {
        F32 inverse_projection[16];
        F32 prev_projection[16];
        F32 inverse_prev_projection[16];
        F32 inverse_modelview_delta[16];
        F32 history[4];
        F32 depth_to_ndc[2];
        S32 size[2];
        S32 gbuffer_size[2];
        S32 scene_size[2];
        // REFLECTION_MOTION only (RESOLVE_MAIN's block ends above).
        F32 projection[16];
        F32 motion[4];
    };
    static_assert(sizeof(ResolveUniforms) == 384, "ResolveUniforms must match the std140 block SsrResolve with REFLECTION_MOTION");
    // The block without REFLECTION_MOTION: what RESOLVE_MAIN is handed (as before research/294).
    constexpr size_t RESOLVE_PLAIN_UNIFORMS = offsetof(ResolveUniforms, projection);
    static_assert(RESOLVE_PLAIN_UNIFORMS == 304, "RESOLVE_MAIN's std140 block SsrResolve");

    struct MaskUniforms
    {
        F32 value[4];
    };

    struct FilterUniforms
    {
        S32 size[2];
        S32 gbuffer_size[2];
        F32 mip_scale;
        F32 top_level;
        F32 max_roughness;
        F32 fade_width;
    };
    static_assert(sizeof(FilterUniforms) == 32, "FilterUniforms must match the std140 block SsrFilter");

    constexpr U32 TRACE_IMAGES = 6;     // depth, specular, normal, scene colour, scene depth, the depth pyramid
    constexpr U32 RESOLVE_IMAGES = 7;   // trace, history, depth, specular, normal, scene depth, avatar mask
    constexpr U32 RESOLVE_MOTION_IMAGES = RESOLVE_IMAGES + 1;   // and the trace's hit distance
    constexpr U32 FILTER_IMAGES = 5;    // source, the blur pyramid, depth, specular, normal

    // The reflection fades out over this much roughness below the trace's limit.
    constexpr F32 ROUGHNESS_FADE = 0.1f;

    constexpr VkFormat HIZ_FORMAT = VK_FORMAT_R32_SFLOAT;
    constexpr VkFormat PYRAMID_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;  // the SSR buffer's (GL_RGBA16F)

    // GL's mip chain of a width x height texture: its levels, level 0 included.
    U32 levelCount(U32 width, U32 height)
    {
        U32 levels = 1;
        for (U32 size = llmax(width, height); size > 1; size >>= 1)
        {
            ++levels;
        }
        return levels;
    }

    // ------------------------------------------------------------------
    // A mip chain of the backend's own: the translation layer cannot draw into
    // a GL texture's levels (llvkgl_entry.cpp: attachments of level 0 only).
    // Each level is drawn from the one before; then all of them are sampled.

    struct MipChain
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkFormat format = VK_FORMAT_UNDEFINED;
        U32 width = 0;                          // level 0's size
        U32 height = 0;
        U32 levels = 0;
        VkImageView all = VK_NULL_HANDLE;       // every level, for the passes that read the chain
        std::vector<VkImageView> level_views;   // one level each: drawn into, then the next level's source
        VkSampler sampler = VK_NULL_HANDLE;     // how the chain is sampled (not owned)
    };

    void releaseChain(MipChain& chain)
    {
        if (!chain.image)
        {
            chain = MipChain();
            return;
        }
        const VkImage image = chain.image;
        const VmaAllocation memory = chain.memory;
        std::vector<VkImageView> views = chain.level_views;
        views.push_back(chain.all);
        // Once the frames that may still read it are done.
        LLVKMirror::instance().defer([image, memory, views]()
        {
            const VkDevice device = LLVKDevice::instance().getDevice();
            for (VkImageView view : views)
            {
                if (view)
                {
                    vkDestroyImageView(device, view, nullptr);
                }
            }
            LLVKDevice::instance().forgetImage(memory);
            vmaDestroyImage(LLVKDevice::instance().getAllocator(), image, memory);
        });
        chain = MipChain();
    }

    VkImageView makeView(VkImage image, VkFormat format, U32 base_level, U32 level_count)
    {
        VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base_level, level_count, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(LLVKDevice::instance().getDevice(), &info, nullptr, &view) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        return view;
    }

    // The chain at that size, made anew when it is not (what it held is lost).
    bool ensureChain(MipChain& chain, VkFormat format, U32 width, U32 height, U32 levels, VkSampler sampler, const char* what)
    {
        if (chain.image && chain.format == format && chain.width == width && chain.height == height && chain.levels == levels)
        {
            return true;
        }
        releaseChain(chain);
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = levels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (LLVKDevice::instance().createImage(info, chain.image, chain.memory) != VK_SUCCESS)
        {
            chain = MipChain();
            LL_WARNS_ONCE("VKSsr") << what << ": no image of " << width << "x" << height << ", " << levels << " levels" << LL_ENDL;
            return false;
        }
        chain.format = format;
        chain.width = width;
        chain.height = height;
        chain.levels = levels;
        chain.sampler = sampler;
        chain.all = makeView(chain.image, format, 0, levels);
        bool views = chain.all != VK_NULL_HANDLE;
        for (U32 level = 0; level < levels && views; ++level)
        {
            chain.level_views.push_back(makeView(chain.image, format, level, 1));
            views = chain.level_views.back() != VK_NULL_HANDLE;
        }
        if (!views)
        {
            releaseChain(chain);
            LL_WARNS_ONCE("VKSsr") << what << ": no image views" << LL_ENDL;
            return false;
        }
        LL_INFOS("VKSsr") << what << ": " << width << "x" << height << ", " << levels << " levels" << LL_ENDL;
        return true;
    }

    // Levels [base, base + count) of the chain from one layout to another,
    // after what the old one was for and before what the new one is for. From
    // UNDEFINED (the contents dropped) the frames before still order their
    // reads before the new writes: same queue, so a dependency on their
    // fragment shading is enough.
    void levelBarrier(VkCommandBuffer cmd, const MipChain& chain, U32 base, U32 count, VkImageLayout from, VkImageLayout to)
    {
        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        if (from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = 0;
        }
        if (to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else
        {
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = chain.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1 };
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // Draws every level of the chain with pass, level 0 from first (a GL
    // texture of source_width x source_height, already in shader-read
    // layout: LLVKTracker::prepareSampled), each next one from the level
    // before. step_texels: the diagonal taps' offset in source texels
    // (ChainUniforms::texel_step). Under LLVKTracker::lock(), while a frame
    // is recorded. Every level ends in shader-read layout, drawn or not;
    // false when one was not drawn.
    bool recordChain(MipChain& chain, LLVKFullscreen::Pass& pass, const LLVKFullscreen::Image& first, U32 source_width,
                     U32 source_height, F32 step_texels)
    {
        LLVKMirror& mirror = LLVKMirror::instance();
        // Whatever the levels held is redrawn.
        levelBarrier(mirror.transferCommands(), chain, 0, chain.levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        bool drawn = true;
        for (U32 level = 0; level < chain.levels; ++level)
        {
            const U32 width = llmax(1u, chain.width >> level);
            const U32 height = llmax(1u, chain.height >> level);
            if (drawn)
            {
                LLVKMirror::Target target;
                target.colors[0] = chain.level_views[level];
                target.color_formats[0] = chain.format;
                target.color_count = 1;
                target.width = width;
                target.height = height;
                VkCommandBuffer cmd = mirror.beginRendering(target);
                ChainUniforms uniforms = {};
                uniforms.source_size[0] = (S32)source_width;
                uniforms.source_size[1] = (S32)source_height;
                uniforms.texel_step[0] = step_texels / (F32)source_width;
                uniforms.texel_step[1] = step_texels / (F32)source_height;
                uniforms.inverse_target_size[0] = 1.f / (F32)width;
                uniforms.inverse_target_size[1] = 1.f / (F32)height;
                LLVKFullscreen::Image source = first;
                if (level > 0)
                {
                    source = LLVKFullscreen::Image();
                    source.view = chain.level_views[level - 1];
                    source.sampler = chain.sampler;
                    source.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                drawn = cmd && pass.draw(cmd, target, &uniforms, sizeof(uniforms), &source, 1);
                mirror.endRendering();
            }
            levelBarrier(mirror.transferCommands(), chain, level, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            source_width = width;
            source_height = height;
        }
        return drawn;
    }

    VkSampler makeSampler(VkFilter filter, VkSamplerMipmapMode mipmap)
    {
        VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = mipmap;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod = 0.f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(LLVKDevice::instance().getDevice(), &info, nullptr, &sampler) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        return sampler;
    }

    void copyMatrix(F32 (&out)[16], const F32 (&in)[16])
    {
        memcpy(out, in, sizeof(out));
    }

    // A G-buffer texture's layout in a pass on target: the scene depth is
    // attached read-only when it is the target's own.
    VkImageLayout depthLayout(const LLVKMirror::Target& target)
    {
        return target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool hiZ(const LLRHI::SsrHiZ& pass);
        bool trace(const LLRHI::SsrTrace& pass);
        bool avatarMask(const LLRHI::SsrAvatarMask& pass);
        bool resolve(const LLRHI::SsrResolve& pass);
        bool filter(const LLRHI::SsrFilter& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        // The reflection's motion passes count too: a release must not ship without them unnoticed.
        bool bake() { return ready() && mMotionReady; }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        bool mMotionReady = false;              // mTraceHitDistance and mResolveMotion too (LLRHI::sSsrHitDistance)
        LLVKFullscreen::Pass mHiZ;
        LLVKFullscreen::Pass mDownsample;
        LLVKFullscreen::Pass mTrace;
        LLVKFullscreen::Pass mTraceHitDistance;     // with the hits' distance (LLRHI::SsrTrace::hit_distance)
        LLVKFullscreen::Pass mResolve;
        LLVKFullscreen::Pass mResolveMotion;        // with the reflection's motion (LLRHI::SsrResolve::hit_distance_texture)
        LLVKFullscreen::Pass mFilter;
        LLVKFullscreen::Pass mMaskAvatars;      // writes 1 where the avatar bit is set
        LLVKFullscreen::Pass mMaskRest;         // writes 0 where it is not
        VkSampler mNearest = VK_NULL_HANDLE;    // the depth pyramid (read texel by texel)
        VkSampler mLinear = VK_NULL_HANDLE;     // the blur pyramid (trilinear)
        MipChain mDepthPyramid;                 // the scene copy's depth, levels 1..
        MipChain mBlurPyramid;                  // the SSR buffer, levels 1..
        U32 mDepthPyramidFrame = U32_MAX;       // LLVKTracker::frame() it was drawn in
        bool mLoggedTrace[2] = { false, false };    // the first trace drawn without and with the hits' distance
        std::vector<LLVKTracker::GLName> mResolveSampled;   // the resolve's textures (kept: no allocation each frame)
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            const std::string trace = std::string("#version 450\n") + GBUFFER_GLSL + TRACE_BODY + TRACE_MAIN;
            const std::string trace_hit_distance =
                std::string("#version 450\n#define HIT_DISTANCE 1\n") + GBUFFER_GLSL + TRACE_BODY + TRACE_MAIN;
            const std::string resolve = std::string("#version 450\n") + GBUFFER_GLSL + RESOLVE_COMMON + RESOLVE_MAIN;
            const std::string resolve_motion =
                std::string("#version 450\n#define REFLECTION_MOTION 1\n") + GBUFFER_GLSL + RESOLVE_COMMON + RESOLVE_MOTION_MAIN;
            const VkColorComponentFlags trace_writes[2] = { LLVKFullscreen::WRITE_RGBA, VK_COLOR_COMPONENT_R_BIT };
            const std::string filter = std::string("#version 450\n") + GBUFFER_GLSL + FILTER_BODY;
            mNearest = makeSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
            mLinear = makeSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
            mReady = mNearest && mLinear && mHiZ.init("ssr depth pyramid", HIZ_FRAGMENT, 1) &&
                     mDownsample.init("ssr blur pyramid", DOWNSAMPLE_FRAGMENT, 1) &&
                     mTrace.init("ssr trace", trace.c_str(), TRACE_IMAGES) &&
                     mResolve.init("ssr resolve", resolve.c_str(), RESOLVE_IMAGES) &&
                     mFilter.init("ssr filter", filter.c_str(), FILTER_IMAGES) &&
                     mMaskAvatars.init("ssr avatar mask", MASK_FRAGMENT, 0, LLVKFullscreen::Blend::Replace, VK_COLOR_COMPONENT_R_BIT,
                                       { true, LLRHI::STENCIL_AVATAR, LLRHI::STENCIL_AVATAR }) &&
                     mMaskRest.init("ssr avatar mask (the rest)", MASK_FRAGMENT, 0, LLVKFullscreen::Blend::Replace, VK_COLOR_COMPONENT_R_BIT,
                                    { true, 0, LLRHI::STENCIL_AVATAR });
            if (!mReady)
            {
                LL_WARNS("VKSsr") << "The screen space reflection passes could not be set up; the official tracing stays" << LL_ENDL;
            }
            // The reflection's motion on its own: without it the passes above still run (as before research/294).
            mMotionReady = mReady &&
                           mTraceHitDistance.initColors("ssr trace (hit distance)", trace_hit_distance.c_str(), TRACE_IMAGES, 2, trace_writes) &&
                           mResolveMotion.init("ssr resolve (reflection motion)", resolve_motion.c_str(), RESOLVE_MOTION_IMAGES);
            if (mReady && !mMotionReady)
            {
                LL_WARNS("VKSsr") << "The screen space reflections' motion passes could not be set up; the history follows the surface"
                                  << LL_ENDL;
            }
            LLRHI::sSsrHitDistance = mMotionReady;
        }
        return mReady;
    }

    bool Renderer::hiZ(const LLRHI::SsrHiZ& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        mDepthPyramidFrame = U32_MAX;
        if (!pass.scene_depth_texture)
        {
            releaseChain(mDepthPyramid);
            releaseChain(mBlurPyramid);
            return true;
        }
        if (!ready() || !pass.width || !pass.height)
        {
            return false;
        }
        const U32 width = llmax(1u, pass.width >> 1);
        const U32 height = llmax(1u, pass.height >> 1);
        if (!ensureChain(mDepthPyramid, HIZ_FORMAT, width, height, levelCount(width, height), mNearest, "SSR depth pyramid") ||
            !LLVKTracker::prepareSampled({ pass.scene_depth_texture }))
        {
            return false;
        }
        LLVKFullscreen::Image first;
        first.texture = pass.scene_depth_texture;
        const bool drawn = recordChain(mDepthPyramid, mHiZ, first, pass.width, pass.height, 0.f);
        LLVKTracker::endPass();
        if (drawn)
        {
            mDepthPyramidFrame = LLVKTracker::frame();
        }
        return drawn;
    }

    bool Renderer::trace(const LLRHI::SsrTrace& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const LLRHI::SsrFrame& frame = pass.frame;
        // The depth pyramid must be this frame's (the scene copy's depth as it is now).
        if (!ready() || !mDepthPyramid.image || mDepthPyramidFrame != LLVKTracker::frame() || !frame.depth_texture ||
            !frame.specular_texture || !frame.normal_texture || !pass.scene_color_texture || !pass.scene_depth_texture ||
            !frame.width || !frame.height || !pass.scene_width || !pass.scene_height || (pass.hit_distance && !mMotionReady))
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(
            { frame.depth_texture, frame.specular_texture, frame.normal_texture, pass.scene_color_texture, pass.scene_depth_texture }, target);
        if (!cmd)
        {
            return false;
        }

        TraceUniforms uniforms = {};
        copyMatrix(uniforms.inverse_projection, frame.inverse_projection);
        copyMatrix(uniforms.prev_projection, frame.prev_projection);
        copyMatrix(uniforms.inverse_prev_projection, frame.inverse_prev_projection);
        copyMatrix(uniforms.inverse_modelview_delta, frame.inverse_modelview_delta);
        uniforms.trace[0] = llmax(pass.max_thickness, 0.001f);
        uniforms.trace[1] = llmax(pass.depth_bias, 0.f);
        uniforms.trace[2] = llmax(pass.max_depth, 0.1f);
        uniforms.trace[3] = llclamp(pass.max_roughness, 0.f, 1.f);
        uniforms.rays[0] = pass.noise;
        uniforms.rays[1] = (F32)llclamp(pass.rays, 1u, 16u);
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;
        uniforms.gbuffer_size[0] = (S32)frame.width;
        uniforms.gbuffer_size[1] = (S32)frame.height;
        uniforms.scene_size[0] = (S32)pass.scene_width;
        uniforms.scene_size[1] = (S32)pass.scene_height;
        uniforms.iterations = (S32)llclamp(pass.iterations, 1u, 256u);
        uniforms.hiz_levels = (S32)mDepthPyramid.levels + 1;
        copyMatrix(uniforms.self_box, pass.self_box);
        memcpy(uniforms.self_plane, pass.self_plane, sizeof(uniforms.self_plane));
        memcpy(uniforms.self_params, pass.self_params, sizeof(uniforms.self_params));
        uniforms.self_params[0] = std::isfinite(uniforms.self_params[0]) ? llclamp(uniforms.self_params[0], 0.f, 1.f) : 0.f;

        LLVKFullscreen::Image images[TRACE_IMAGES];
        images[0].texture = frame.depth_texture;
        images[0].layout = depthLayout(target);
        images[1].texture = frame.specular_texture;
        images[2].texture = frame.normal_texture;
        images[3].texture = pass.scene_color_texture;
        images[4].texture = pass.scene_depth_texture;
        images[5].view = mDepthPyramid.all;
        images[5].sampler = mDepthPyramid.sampler;
        // The pass with the hits' distance draws only on a target of two attachments (and the other on one).
        LLVKFullscreen::Pass& trace_pass = pass.hit_distance ? mTraceHitDistance : mTrace;
        const bool drawn = trace_pass.draw(cmd, target, &uniforms, sizeof(uniforms), images, TRACE_IMAGES);
        LLVKTracker::endPass();
        bool& logged = mLoggedTrace[pass.hit_distance ? 1 : 0];
        if (drawn && !logged)
        {
            logged = true;
            LL_INFOS("VKSsr") << "SSR trace: " << target.width << "x" << target.height << " from a " << frame.width << "x" << frame.height
                              << " G-buffer, depth pyramid " << mDepthPyramid.levels + 1 << " levels"
                              << (pass.hit_distance ? ", with the hits' distance" : "") << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::avatarMask(const LLRHI::SsrAvatarMask& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready())
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass({}, target);
        if (!cmd)
        {
            return false;
        }
        // Both test the stencil: nothing is drawn on a target without it (no mark).
        const MaskUniforms avatars = { { 1.f, 1.f, 1.f, 1.f } };
        const MaskUniforms rest = { { 0.f, 0.f, 0.f, 0.f } };
        const bool drawn = target.stencil && mMaskAvatars.draw(cmd, target, &avatars, sizeof(avatars), nullptr, 0) &&
                           mMaskRest.draw(cmd, target, &rest, sizeof(rest), nullptr, 0);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::resolve(const LLRHI::SsrResolve& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const LLRHI::SsrFrame& frame = pass.frame;
        if (!ready() || !pass.trace_texture || !pass.history_texture || !pass.scene_depth_texture || !frame.depth_texture ||
            !frame.specular_texture || !frame.normal_texture || !frame.width || !frame.height || !pass.scene_width || !pass.scene_height ||
            (pass.hit_distance_texture && !mMotionReady))
        {
            return false;
        }
        LLVKMirror::Target target;
        // Without the mask the trace stands in (not read: history.w 0).
        const U32 mask = pass.avatar_mask_texture ? pass.avatar_mask_texture : pass.trace_texture;
        const bool motion = pass.hit_distance_texture != 0;
        mResolveSampled.assign({ pass.trace_texture, pass.history_texture, frame.depth_texture, frame.specular_texture, frame.normal_texture,
                                 pass.scene_depth_texture, mask });
        if (motion)
        {
            mResolveSampled.push_back(pass.hit_distance_texture);
        }
        VkCommandBuffer cmd = LLVKTracker::beginPass(mResolveSampled, target);
        if (!cmd)
        {
            return false;
        }
        ResolveUniforms uniforms = {};
        copyMatrix(uniforms.inverse_projection, frame.inverse_projection);
        copyMatrix(uniforms.prev_projection, frame.prev_projection);
        copyMatrix(uniforms.inverse_prev_projection, frame.inverse_prev_projection);
        copyMatrix(uniforms.inverse_modelview_delta, frame.inverse_modelview_delta);
        uniforms.history[0] = llclamp(pass.history_blend, 0.f, 0.98f);
        uniforms.history[1] = llclamp(pass.avatar_history, 0.f, 1.f);
        uniforms.history[2] = llmax(pass.disocclusion, 0.001f);
        uniforms.history[3] = pass.avatar_mask_texture ? 1.f : 0.f;
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;
        uniforms.gbuffer_size[0] = (S32)frame.width;
        uniforms.gbuffer_size[1] = (S32)frame.height;
        uniforms.scene_size[0] = (S32)pass.scene_width;
        uniforms.scene_size[1] = (S32)pass.scene_height;
        copyMatrix(uniforms.projection, frame.projection);
        uniforms.motion[0] = std::isfinite(pass.clip_sigma) ? llclamp(pass.clip_sigma, LLRHI::SSR_CLIP_SIGMA_MIN, LLRHI::SSR_CLIP_SIGMA_MAX)
                                                            : LLRHI::SSR_CLIP_SIGMA_DEFAULT;

        LLVKFullscreen::Image images[RESOLVE_MOTION_IMAGES];
        images[0].texture = pass.trace_texture;
        images[1].texture = pass.history_texture;
        images[2].texture = frame.depth_texture;
        images[2].layout = depthLayout(target);
        images[3].texture = frame.specular_texture;
        images[4].texture = frame.normal_texture;
        images[5].texture = pass.scene_depth_texture;
        images[6].texture = mask;
        images[7].texture = pass.hit_distance_texture;
        const bool drawn = motion ? mResolveMotion.draw(cmd, target, &uniforms, sizeof(uniforms), images, RESOLVE_MOTION_IMAGES)
                                  : mResolve.draw(cmd, target, &uniforms, RESOLVE_PLAIN_UNIFORMS, images, RESOLVE_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::filter(const LLRHI::SsrFilter& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const LLRHI::SsrFrame& frame = pass.frame;
        if (!ready() || !pass.source_texture || !pass.width || !pass.height || !frame.depth_texture || !frame.specular_texture ||
            !frame.normal_texture || !frame.width || !frame.height)
        {
            return false;
        }

        // GCP builds the levels the lighting can ask for: up to the trace's
        // roughness limit (it had 0.7 written in), and one more.
        const U32 full_levels = levelCount(pass.width, pass.height);
        const F32 max_roughness = llclamp(pass.max_roughness, 0.f, 1.f);
        U32 built = 1;
        if (pass.filter_scale > 0.f && full_levels > 1)
        {
            built = llmin(full_levels, (U32)std::ceil(max_roughness * (F32)(full_levels - 1)) + 2);
        }
        bool pyramid = false;
        if (built > 1)
        {
            const U32 width = llmax(1u, pass.width >> 1);
            const U32 height = llmax(1u, pass.height >> 1);
            if (ensureChain(mBlurPyramid, PYRAMID_FORMAT, width, height, built - 1, mLinear, "SSR blur pyramid") &&
                LLVKTracker::prepareSampled({ pass.source_texture }))
            {
                LLVKFullscreen::Image first;
                first.texture = pass.source_texture;
                pyramid = recordChain(mBlurPyramid, mDownsample, first, pass.width, pass.height, pass.filter_scale);
                LLVKTracker::endPass();
            }
        }
        if (!pyramid)
        {
            built = 1;
        }

        LLVKMirror::Target target;
        VkCommandBuffer cmd =
            LLVKTracker::beginPass({ pass.source_texture, frame.depth_texture, frame.specular_texture, frame.normal_texture }, target);
        if (!cmd)
        {
            return false;
        }
        FilterUniforms uniforms = {};
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;
        uniforms.gbuffer_size[0] = (S32)frame.width;
        uniforms.gbuffer_size[1] = (S32)frame.height;
        uniforms.mip_scale = (F32)(full_levels - 1);
        uniforms.top_level = (F32)(built - 1);
        uniforms.max_roughness = max_roughness;
        uniforms.fade_width = llmax(llmin(ROUGHNESS_FADE, max_roughness), 1e-4f);  // smoothstep needs edge0 < edge1

        LLVKFullscreen::Image images[FILTER_IMAGES];
        images[0].texture = pass.source_texture;
        if (pyramid)
        {
            images[1].view = mBlurPyramid.all;
            images[1].sampler = mBlurPyramid.sampler;
        }
        else
        {
            images[1].texture = pass.source_texture;    // not read (top level 0)
        }
        images[2].texture = frame.depth_texture;
        images[2].layout = depthLayout(target);
        images[3].texture = frame.specular_texture;
        images[4].texture = frame.normal_texture;
        const bool drawn = mFilter.draw(cmd, target, &uniforms, sizeof(uniforms), images, FILTER_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool record_hiz(const LLRHI::SsrHiZ& pass)
    {
        return sRenderer.hiZ(pass);
    }

    bool record_trace(const LLRHI::SsrTrace& pass)
    {
        return sRenderer.trace(pass);
    }

    bool record_avatar_mask(const LLRHI::SsrAvatarMask& pass)
    {
        return sRenderer.avatarMask(pass);
    }

    bool record_resolve(const LLRHI::SsrResolve& pass)
    {
        return sRenderer.resolve(pass);
    }

    bool record_filter(const LLRHI::SsrFilter& pass)
    {
        return sRenderer.filter(pass);
    }
}

void LLVKSsr::install()
{
    if (!LLRHI::sSsr)
    {
        LL_INFOS("VKSsr") << "Screen space reflection passes off (LLRHI_SSR=0)" << LL_ENDL;
        return;
    }
    LLRHI::sSsrHiZ = &record_hiz;
    LLRHI::sSsrTrace = &record_trace;
    LLRHI::sSsrAvatarMask = &record_avatar_mask;
    LLRHI::sSsrResolve = &record_resolve;
    LLRHI::sSsrFilter = &record_filter;
    LLVKShader::addBakeStep("SSR", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
