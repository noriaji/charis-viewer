/**
 * @file llvkgtao.cpp
 * @brief GTAO and contact shadows recorded directly in Vulkan (research/92).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
 * Portions (the occlusion pass) Copyright (C) 2016-2021, Intel Corporation
 * (XeGTAO, MIT license; the notice is below)
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
 *
 * The occlusion pass follows the GTAO integral as XeGTAO writes it
 * (Intel, MIT license; see plus/gtao/ORIGIN.md): Jimenez et al.,
 * "Practical Realtime Strategies for Accurate Indirect Occlusion", 2016.
 * The occlusion pass (OCCLUSION_FRAGMENT) is a GLSL port of XeGTAO's main
 * pass (https://github.com/GameTechDev/XeGTAO), under XeGTAO's license:
 *
 *   Copyright (C) 2016-2021, Intel Corporation
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "linden_common.h"

#include "llvkgtao.h"

#include "llrhigtao.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Shaders. Every pass starts with SHADER_COMMON: one uniform block for
    // all of them (each pass reads its own fields) and the view helpers.

    const char* const SHADER_COMMON = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Gtao
{
    vec4 ndc_to_view;           // eye x = d * (ndc x * [0] + [2]), eye y = d * (ndc y * [1] + [3])
    vec4 projection_scale;      // [0][0], [1][1], [2][0], [2][1] of the projection
    vec4 light_direction;       // xyz: towards the light, eye space (contact shadows)
    vec2 depth_to_ndc;          // ndc z = stored depth * x + y
    vec2 depth_ab;              // view depth = y / (ndc z + x)
    ivec2 full_size;            // the scene depth's size
    ivec2 source_size;          // occlusion, denoise, apply: the reduced targets' size
    int scale;                  // full-resolution pixels per reduced pixel, each way
    int steps;                  // occlusion: samples on each side; contact: along the ray
    int slices;                 // occlusion: directions
    float radius;               // occlusion: m
    float max_screen_radius;    // occlusion: reduced pixels
    float power;                // apply: visibility ^ power
    float ray_length;           // contact: m
    float thickness;            // contact: m
    float fade_start;           // contact: view depth, m
    float fade_end;
    float max_screen_length;    // contact: full-resolution pixels a ray covers at most
    float near_fade_start;      // contact: a metre's size on screen (screen heights) where the close-up fade starts
    float near_fade_end;        // and where they are gone
} u;

// The largest view depth the RG16F targets keep (half-float range, with room).
const float MAX_DEPTH = 60000.0;

// A view depth the passes can work with: a positive finite number.
bool validDepth(float d)
{
    return d > 0.0 && d <= MAX_DEPTH;      // false for NaN too
}

// A visibility as the light map takes it: 0..1, and 1 (no occlusion) for
// anything that is not a number.
float validVisibility(float v)
{
    return isnan(v) ? 1.0 : clamp(v, 0.0, 1.0);
}

// NDC xy of the centre of a full-resolution pixel.
vec2 pixelNDC(vec2 pixel)
{
    return (pixel + 0.5) / vec2(u.full_size) * 2.0 - 1.0;
}

// View depth (m, > 0) of a stored depth value.
float viewDepth(float stored)
{
    return u.depth_ab.y / (stored * u.depth_to_ndc.x + u.depth_to_ndc.y + u.depth_ab.x);
}

// Eye-space position of the point at NDC xy with view depth d.
vec3 viewPosition(vec2 ndc, float d)
{
    return vec3(d * (ndc * u.ndc_to_view.xy + u.ndc_to_view.zw), -d);
}
)GLSL";

    // GtaoDepth: the scene depth as view depth, one full-resolution pixel
    // (the top-left of its block) per target pixel.
    const char* const DEPTH_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(location = 0) out float frag_depth;

void main()
{
    ivec2 texel = min(ivec2(gl_FragCoord.xy) * u.scale, u.full_size - 1);
    frag_depth = viewDepth(texelFetch(scene_depth, texel, 0).r);
}
)GLSL";

    // GtaoOcclusion: the GTAO visibility of each pixel (XeGTAO's main pass,
    // without its depth mips: the radius on screen is capped instead).
    const char* const OCCLUSION_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D view_depth;

layout(location = 0) out vec2 frag_occlusion;   // visibility, view depth

const float PI = 3.14159265;
const float HALF_PI = 1.57079633;
const float GOLDEN_RATIO_FRACTION = 0.618033989;
// Occluders fade out over this share of the radius, at its far end.
const float FALLOFF_SHARE = 0.615;
// Samples crowd towards the pixel (s^2): near occluders matter most.
const float SAMPLE_DISTRIBUTION_POWER = 2.0;
// The nearest sample is at least this far (pixels): not the pixel itself.
const float PIXEL_TOO_CLOSE = 1.3;
// Below this radius on screen (pixels) the occlusion fades out (far away).
const float MIN_SCREEN_RADIUS = 2.0;
// Blends the projected normal's length towards 1 (XeGTAO), steadier on slopes.
const float NORMAL_LENGTH_BLEND = 0.05;
const float MIN_VISIBILITY = 0.03;

// A 4x4 ordered pattern in (0, 1): every 4x4 block holds each value once,
// so the denoise pass's 4x4 average takes it out.
float pattern4x4(ivec2 p)
{
    const float ORDER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    return (ORDER[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

float depthAt(ivec2 texel)
{
    return texelFetch(view_depth, clamp(texel, ivec2(0), u.source_size - 1), 0).r;
}

// Eye-space position of a target pixel: it stands for the full-resolution
// pixel at the top-left of its block (the depth pass's choice).
vec3 positionAt(vec2 texel, float d)
{
    return viewPosition(pixelNDC(texel * float(u.scale)), d);
}

// The surface normal from the depths around the pixel, each way taking the
// neighbour nearer in depth (so an edge does not bend it). Faces the eye.
vec3 normalAt(ivec2 texel, vec3 centre, float d)
{
    float left = depthAt(texel + ivec2(-1, 0));
    float right = depthAt(texel + ivec2(1, 0));
    float below = depthAt(texel + ivec2(0, -1));
    float above = depthAt(texel + ivec2(0, 1));
    vec3 along_x = abs(right - d) < abs(left - d) ? positionAt(vec2(texel + ivec2(1, 0)), right) - centre
                                                    : centre - positionAt(vec2(texel + ivec2(-1, 0)), left);
    vec3 along_y = abs(above - d) < abs(below - d) ? positionAt(vec2(texel + ivec2(0, 1)), above) - centre
                                                     : centre - positionAt(vec2(texel + ivec2(0, -1)), below);
    vec3 normal = cross(along_x, along_y);
    float normal_length = length(normal);
    return normal_length > 0.0 ? normal / normal_length : normalize(-centre);
}

// The cosine of the angle between the view vector and the direction to a sample,
// pulled down to low_horizon as the sample nears the end of the radius.
float horizonCos(vec3 delta, vec3 view_vec, float low_horizon, float falloff_mul, float falloff_add)
{
    float distance = max(length(delta), 1e-4);
    float weight = clamp(distance * falloff_mul + falloff_add, 0.0, 1.0);
    return mix(low_horizon, dot(delta / distance, view_vec), weight);
}

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float d = depthAt(texel);
    if (!validDepth(d))
    {
        frag_occlusion = vec2(1.0, MAX_DEPTH);
        return;
    }
    float full_radius = u.radius * u.projection_scale.y * 0.5 * float(u.source_size.y) / d;
    if (full_radius < MIN_SCREEN_RADIUS)
    {
        frag_occlusion = vec2(1.0, d);      // far away (the sky too)
        return;
    }
    float screen_radius = min(full_radius, u.max_screen_radius);
    // Close up the cap on screen shortens the radius in metres too: the falloff
    // follows it, so occluders fade out towards the cap instead of ending there.
    float radius = u.radius * screen_radius / full_radius;

    vec3 position = positionAt(vec2(texel), d);
    vec3 normal = normalAt(texel, position, d);
    vec3 view_vec = normalize(-position);

    float falloff_range = FALLOFF_SHARE * radius;
    float falloff_mul = -1.0 / falloff_range;
    float falloff_add = (radius - falloff_range) / falloff_range + 1.0;
    float min_s = PIXEL_TOO_CLOSE / screen_radius;
    float slice_noise = pattern4x4(texel);
    float step_noise = pattern4x4(ivec2(texel.y, texel.x + 2));

    float visibility = 0.0;
    for (int slice = 0; slice < u.slices; ++slice)
    {
        // The slice: the plane through the view vector and this screen direction.
        float phi = (float(slice) + slice_noise) * PI / float(u.slices);
        vec2 omega = vec2(cos(phi), sin(phi));
        vec3 direction = vec3(omega, 0.0);
        vec3 ortho_direction = direction - dot(direction, view_vec) * view_vec;
        vec3 axis = normalize(cross(ortho_direction, view_vec));
        vec3 projected_normal = normal - axis * dot(normal, axis);
        float projected_length = max(length(projected_normal), 1e-4);
        float cos_n = clamp(dot(projected_normal, view_vec) / projected_length, 0.0, 1.0);
        float n = sign(dot(ortho_direction, projected_normal)) * acos(cos_n);

        // The highest horizon on each side, starting from the surface's own.
        float low_horizon0 = cos(n + HALF_PI);
        float low_horizon1 = cos(n - HALF_PI);
        float horizon0 = low_horizon0;
        float horizon1 = low_horizon1;
        for (int step = 0; step < u.steps; ++step)
        {
            float noise = fract(step_noise + float(slice + step * u.slices) * GOLDEN_RATIO_FRACTION);
            float s = pow((float(step) + noise) / float(u.steps), SAMPLE_DISTRIBUTION_POWER) + min_s;
            vec2 offset = round(s * omega * screen_radius);
            ivec2 texel0 = texel + ivec2(offset);
            ivec2 texel1 = texel - ivec2(offset);
            vec3 delta0 = positionAt(vec2(texel0), depthAt(texel0)) - position;
            vec3 delta1 = positionAt(vec2(texel1), depthAt(texel1)) - position;
            horizon0 = max(horizon0, horizonCos(delta0, view_vec, low_horizon0, falloff_mul, falloff_add));
            horizon1 = max(horizon1, horizonCos(delta1, view_vec, low_horizon1, falloff_mul, falloff_add));
        }

        // The cosine-weighted visible arc between the two horizons.
        projected_length = mix(projected_length, 1.0, NORMAL_LENGTH_BLEND);
        float h0 = n + clamp(-acos(clamp(horizon1, -1.0, 1.0)) - n, -HALF_PI, HALF_PI);
        float h1 = n + clamp(acos(clamp(horizon0, -1.0, 1.0)) - n, -HALF_PI, HALF_PI);
        float sin_n = sin(n);
        float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n)) * 0.25;
        visibility += projected_length * (arc0 + arc1);
    }
    visibility /= float(u.slices);
    visibility = mix(1.0, visibility, clamp(screen_radius / MIN_SCREEN_RADIUS - 1.0, 0.0, 1.0));
    frag_occlusion = vec2(isnan(visibility) ? 1.0 : clamp(visibility, MIN_VISIBILITY, 1.0), d);
}
)GLSL";

    // GtaoDenoise: the average of the 4x4 block around each pixel, over the
    // pixels near it in depth (an edge keeps its two sides apart).
    const char* const DENOISE_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D occlusion;

layout(location = 0) out vec2 frag_occlusion;   // visibility, view depth

// Relative view depth difference at which a neighbour no longer counts.
const float DEPTH_TOLERANCE = 0.1;

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec2 centre = texelFetch(occlusion, texel, 0).rg;
    float d = centre.g;
    if (!validDepth(d))
    {
        frag_occlusion = vec2(1.0, MAX_DEPTH);
        return;
    }
    float sum = 0.0;
    float weight_sum = 0.0;
    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            vec2 tap = texelFetch(occlusion, clamp(texel + ivec2(x, y), ivec2(0), u.source_size - 1), 0).rg;
            float weight = max(0.0, 1.0 - abs(tap.g - d) / (d * DEPTH_TOLERANCE));
            sum += tap.r * weight;
            weight_sum += weight;
        }
    }
    frag_occlusion = vec2(validVisibility(sum / max(weight_sum, 1e-4)), d);    // the pixel itself always counts (weight 1)
}
)GLSL";

    // The full-resolution passes (apply, contact and the two in one) all read
    // the scene depth at binding 1 and write one colour.
    const char* const FULL_RESOLUTION_COMMON = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(location = 0) out vec4 frag_color;
)GLSL";

    // GtaoApply: the visibility at each full-resolution pixel, from the four
    // nearest reduced pixels weighted by distance and by how close their view
    // depth is to this pixel's. The pipeline writes only g.
    const char* const APPLY_FUNCTIONS = R"GLSL(
layout(set = 0, binding = 2) uniform sampler2D occlusion;

// Relative view depth difference at which a reduced pixel no longer counts.
const float DEPTH_TOLERANCE = 0.1;
// Below this total weight no reduced pixel is near in depth: take the nearest.
const float MIN_WEIGHT = 1e-4;

// What the light map's g gets at pixel, whose view depth is d.
float ambientAt(ivec2 pixel, float d)
{
    // Reduced pixel q stands for full-resolution pixel q * scale.
    vec2 place = vec2(pixel) / float(u.scale);
    ivec2 base = ivec2(floor(place));
    vec2 f = place - vec2(base);

    float sum = 0.0;
    float weight_sum = 0.0;
    float nearest = 1.0;
    float nearest_difference = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 offset = ivec2(i & 1, i >> 1);
        vec2 tap = texelFetch(occlusion, min(base + offset, u.source_size - 1), 0).rg;
        vec2 along = mix(1.0 - f, f, vec2(offset));
        float difference = abs(tap.g - d) / max(d, 1e-3);
        float weight = along.x * along.y * max(0.0, 1.0 - difference / DEPTH_TOLERANCE);
        sum += tap.r * weight;
        weight_sum += weight;
        if (difference < nearest_difference)
        {
            nearest_difference = difference;
            nearest = tap.r;
        }
    }
    float visibility = validVisibility(weight_sum > MIN_WEIGHT ? sum / weight_sum : nearest);
    // The light map holds 0..1 (the official sun program clamps it too).
    // Power 0 (PlusGtaoDebugPasses 4): no occlusion, whatever was read.
    return u.power > 0.0 ? clamp(pow(visibility, u.power), 0.0, 1.0) : 1.0;
}
)GLSL";

    const char* const APPLY_MAIN = R"GLSL(
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float d = viewDepth(texelFetch(scene_depth, pixel, 0).r);
    frag_color = vec4(1.0, ambientAt(pixel, d), 1.0, 1.0);
}
)GLSL";

    // ContactShadows: a short ray from each pixel towards the light; where
    // the depth buffer has a surface in front of the ray (by no more than the
    // thickness), the pixel is in shadow. Each 2x2 quad averages its four rays.
    // The pipeline keeps the smaller of this and the light map's r.
    //
    // Close up (research/92 section 11) a ray of fixed length would cross
    // hundreds of pixels, its steps tens of pixels apart: the start offsets
    // showed as dots. So a ray covers max_screen_length pixels at most (it is
    // shortened in metres), takes no more steps than it covers pixels, and
    // fades out where a metre spans near_fade_start..near_fade_end screen
    // heights (a face filling the screen: the shadow maps are finer there).
    const char* const CONTACT_FUNCTIONS = R"GLSL(
// The ray starts this share of the view depth off the surface, along its
// normal (the depth's own precision: no self-shadowing).
const float NORMAL_OFFSET = 0.002;
// A surface must be this share of its view depth in front of the ray to count.
const float DEPTH_BIAS = 0.001;
// A ray takes no more steps than this many per pixel it covers (far away).
const float STEPS_PER_PIXEL = 1.0;
// Relative view depth difference at which a pixel of the quad no longer counts.
const float QUAD_DEPTH_TOLERANCE = 0.1;

// Interleaved gradient noise (Jimenez 2014).
float gradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Where along its steps a ray's samples sit, in [0, 1): the four pixels of a
// 2x2 quad take a quarter of the step each, so the quad's average marches four
// times as finely; the quads are shifted by interleaved gradient noise.
float stepOffset(ivec2 pixel)
{
    const float QUARTER[4] = float[4](0.0, 2.0, 3.0, 1.0);
    return (QUARTER[(pixel.y & 1) * 2 + (pixel.x & 1)] + gradientNoise(vec2(pixel >> 1))) * 0.25;
}

// The size of a metre on screen at view depth d, in screen heights.
float metreOnScreen(float d)
{
    return u.projection_scale.y * 0.5 / d;
}

// How far (m) the ray from start goes: the set length, shortened to cover
// max_screen_length pixels at most; pixels gets what it covers. A point
// start + t * light moves on screen at |k| / depth(t)^2 pixels per metre (k is
// the same all along the ray), so it covers |k| t / (depth(0) depth(t)) pixels.
float rayLength(vec3 start, out float pixels)
{
    vec3 light = u.light_direction.xyz;
    float depth0 = -start.z;
    float k = length(u.projection_scale.xy * (light.xy * start.z - start.xy * light.z) * 0.5 * vec2(u.full_size));
    float cap = u.max_screen_length;
    // <= 0: the ray never covers cap pixels (it heads away, towards its vanishing point).
    float denominator = k + cap * depth0 * light.z;
    float ray = denominator > 0.0 ? min(u.ray_length, cap * depth0 * depth0 / denominator) : u.ray_length;
    float depth1 = depth0 - ray * light.z;      // > 0: a ray towards the eye is capped before it
    pixels = depth1 > 0.0 ? k * ray / (depth0 * depth1) : cap;
    return ray;
}

// 1 when a surface in the depth buffer blocks the ray from start towards the light.
float rayBlocked(vec3 start, float ray, int steps, float offset)
{
    float step_length = ray / float(steps);
    vec2 size = vec2(u.full_size);
    for (int i = 0; i < steps; ++i)
    {
        vec3 p = start + u.light_direction.xyz * (step_length * (float(i) + offset));
        if (p.z >= 0.0)
        {
            break;      // behind the eye
        }
        vec2 ndc = (u.projection_scale.xy * p.xy + u.projection_scale.zw * p.z) / -p.z;
        if (any(greaterThan(abs(ndc), vec2(1.0))))
        {
            break;      // off the screen: nothing known there
        }
        ivec2 texel = min(ivec2((ndc * 0.5 + 0.5) * size), u.full_size - 1);
        float scene = viewDepth(texelFetch(scene_depth, texel, 0).r);
        float behind = -p.z - scene;
        if (behind > DEPTH_BIAS * scene && behind < u.thickness)
        {
            return 1.0;
        }
    }
    return 0.0;
}

// The average of shadow over this pixel's 2x2 quad (whose rays took the four
// quarter offsets), over the pixels near it in depth: an edge keeps its two
// sides apart. Needs the whole quad: call it outside any branch.
float quadAverage(float shadow, float d, ivec2 pixel)
{
    vec2 side = vec2(1.0) - 2.0 * vec2(pixel & 1);     // towards the quad's other column and row
    vec2 here = vec2(shadow, validDepth(d) ? d : MAX_DEPTH);
    vec2 across = here + side.x * dFdxFine(here);
    vec2 other_row = here + side.y * dFdyFine(here);
    vec2 diagonal = across + side.y * dFdyFine(across);
    vec3 shadows = vec3(across.x, other_row.x, diagonal.x);
    vec3 depths = vec3(across.y, other_row.y, diagonal.y);
    vec3 weights = max(vec3(0.0), 1.0 - abs(depths - here.y) / (here.y * QUAD_DEPTH_TOLERANCE));
    return (shadow + dot(shadows, weights)) / (1.0 + weights.x + weights.y + weights.z);
}

// What the light map's r is kept at most at pixel (1 unshadowed); d gets the
// pixel's view depth. Call it outside any branch (it needs the whole quad).
float contactLight(ivec2 pixel, out float d)
{
    // Clamped: a helper pixel of a quad on the edge may lie outside.
    float stored = texelFetch(scene_depth, min(pixel, u.full_size - 1), 0).r;
    d = viewDepth(stored);
    vec3 position = viewPosition(pixelNDC(vec2(pixel)), d);
    // Derivatives before any branch (they need the whole quad).
    vec3 normal = cross(dFdx(position), dFdy(position));

    float shadow = 0.0;
    float normal_length = length(normal);
    float fade = (1.0 - smoothstep(u.fade_start, u.fade_end, d)) *
                 (1.0 - smoothstep(u.near_fade_start, u.near_fade_end, metreOnScreen(d)));
    // The sky, beyond the fades, and surfaces turned from the light (already dark) are left alone.
    if (stored < 1.0 && validDepth(d) && fade > 0.0 && normal_length > 0.0)
    {
        normal /= normal_length;
        if (dot(normal, u.light_direction.xyz) > 0.0)
        {
            vec3 start = position + normal * (NORMAL_OFFSET * d);
            float pixels;
            float ray = rayLength(start, pixels);
            int steps = clamp(int(ceil(pixels * STEPS_PER_PIXEL)), 1, u.steps);
            shadow = fade * rayBlocked(start, ray, steps, stepOffset(pixel));
        }
    }
    return 1.0 - quadAverage(shadow, d, pixel);
}
)GLSL";

    const char* const CONTACT_MAIN = R"GLSL(
void main()
{
    float d;
    frag_color = vec4(contactLight(ivec2(gl_FragCoord.xy), d));
}
)GLSL";

    // GtaoApply and ContactShadows in one pass (research/125 #9). The pipeline
    // keeps the smaller of each and the light map's, r and g only: r as the
    // contact pass does; g is 1 there (the sun program drew without the
    // official SSAO), so it gets the apply pass's value (<= 1) as it is. A
    // pixel's d is the same as the apply pass's own: the clamp in contactLight
    // changes only helper pixels outside the target.
    const char* const APPLY_CONTACT_MAIN = R"GLSL(
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float d;
    float light = contactLight(pixel, d);
    frag_color = vec4(light, ambientAt(pixel, d), 1.0, 1.0);
}
)GLSL";

    // ------------------------------------------------------------------
    // The uniform block Gtao, as std140 lays it out.

    struct Uniforms
    {
        F32 ndc_to_view[4];
        F32 projection_scale[4];
        F32 light_direction[4];
        F32 depth_to_ndc[2];
        F32 depth_ab[2];
        S32 full_size[2];
        S32 source_size[2];
        S32 scale;
        S32 steps;
        S32 slices;
        F32 radius;
        F32 max_screen_radius;
        F32 power;
        F32 ray_length;
        F32 thickness;
        F32 fade_start;
        F32 fade_end;
        F32 max_screen_length;
        F32 near_fade_start;
        F32 near_fade_end;
        F32 padding[3];
    };
    static_assert(sizeof(Uniforms) == 144, "Uniforms must match the std140 block Gtao");

    constexpr U32 MAX_SLICES = 8;
    constexpr U32 MAX_STEPS = 32;
    constexpr U32 MAX_CONTACT_STEPS = 64;

    // The block with the view's fields filled in; false when the view cannot be used.
    bool viewUniforms(const LLRHI::GtaoView& view, Uniforms& uniforms)
    {
        uniforms = {};
        const F32* projection = view.projection;
        if (!view.depth_texture || !view.width || !view.height || projection[0] == 0.f || projection[1] == 0.f)
        {
            return false;
        }
        uniforms.ndc_to_view[0] = 1.f / projection[0];
        uniforms.ndc_to_view[1] = 1.f / projection[1];
        uniforms.ndc_to_view[2] = projection[2] / projection[0];
        uniforms.ndc_to_view[3] = projection[3] / projection[1];
        memcpy(uniforms.projection_scale, projection, sizeof(uniforms.projection_scale));
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.depth_ab[0] = view.depth_a;
        uniforms.depth_ab[1] = view.depth_b;
        uniforms.full_size[0] = (S32)view.width;
        uniforms.full_size[1] = (S32)view.height;
        uniforms.scale = 1;
        return true;
    }

    bool sameView(const LLRHI::GtaoView& a, const LLRHI::GtaoView& b)
    {
        return a.depth_a == b.depth_a && a.depth_b == b.depth_b && !memcmp(a.projection, b.projection, sizeof(a.projection)) &&
               a.depth_texture == b.depth_texture && a.width == b.width && a.height == b.height;
    }

    // The scene depth as a pass samples it: when it is the bound target's own
    // depth, beginPass attaches it read-only and it is sampled in that layout.
    LLVKFullscreen::Image sceneDepth(LLVKTracker::GLName texture, const LLVKMirror::Target& target)
    {
        return { texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool depth(const LLRHI::GtaoDepth& pass);
        bool occlusion(const LLRHI::GtaoOcclusion& pass);
        bool denoise(const LLRHI::GtaoDenoise& pass);
        bool apply(const LLRHI::GtaoApply& pass);
        bool contact(const LLRHI::ContactShadows& pass);
        bool applyContact(const LLRHI::GtaoApply& apply, const LLRHI::ContactShadows& contact);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();
        // The uniforms of each; false when the pass cannot be drawn.
        bool applyUniforms(const LLRHI::GtaoApply& pass, Uniforms& uniforms);
        bool contactUniforms(const LLRHI::ContactShadows& pass, Uniforms& uniforms);
        void logContact(const LLRHI::ContactShadows& pass, const Uniforms& uniforms);
        // Opens the pass on the bound target, draws it and closes it. The
        // target must be width x height: anything else is not the target the
        // pass was meant for, and it is left alone.
        bool record(LLVKFullscreen::Pass& pass, U32 width, U32 height, const std::vector<LLVKTracker::GLName>& sampled,
                    Uniforms& uniforms, LLVKFullscreen::Image* images, U32 image_count, bool scene_depth_first);

        bool mTried = false;
        bool mReady = false;
        bool mApplyContactReady = false;
        LLVKFullscreen::Pass mDepth;
        LLVKFullscreen::Pass mOcclusion;
        LLVKFullscreen::Pass mDenoise;
        LLVKFullscreen::Pass mApply;
        LLVKFullscreen::Pass mContact;
        LLVKFullscreen::Pass mApplyContact;
        bool mLoggedOcclusion = false;
        bool mLoggedContact = false;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            const std::string common = SHADER_COMMON;
            const std::string full = common + FULL_RESOLUTION_COMMON;
            mReady = mDepth.init("gtao depth", (common + DEPTH_FRAGMENT).c_str(), 1) &&
                     mOcclusion.init("gtao occlusion", (common + OCCLUSION_FRAGMENT).c_str(), 1) &&
                     mDenoise.init("gtao denoise", (common + DENOISE_FRAGMENT).c_str(), 1) &&
                     mApply.init("gtao apply", (full + APPLY_FUNCTIONS + APPLY_MAIN).c_str(), 2, LLVKFullscreen::Blend::Replace,
                                 VK_COLOR_COMPONENT_G_BIT) &&
                     mContact.init("contact shadows", (full + CONTACT_FUNCTIONS + CONTACT_MAIN).c_str(), 1,
                                   LLVKFullscreen::Blend::Min, VK_COLOR_COMPONENT_R_BIT);
            if (!mReady)
            {
                LL_WARNS("VKGtao") << "GTAO and contact shadows could not be set up; they stay off" << LL_ENDL;
            }
            // Without it the two passes are drawn one after the other, as before.
            mApplyContactReady = mReady && mApplyContact.init("gtao apply and contact shadows",
                                                              (full + APPLY_FUNCTIONS + CONTACT_FUNCTIONS + APPLY_CONTACT_MAIN).c_str(),
                                                              2, LLVKFullscreen::Blend::Min,
                                                              VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT);
            if (mReady && !mApplyContactReady)
            {
                LL_WARNS("VKGtao") << "GTAO apply and contact shadows in one pass could not be set up; they are drawn apart" << LL_ENDL;
            }
        }
        return mReady;
    }

    // scene_depth_first: images[0] is the scene depth, which may be the bound
    // target's own (its layout is then the read-only attachment's).
    bool Renderer::record(LLVKFullscreen::Pass& pass, U32 width, U32 height, const std::vector<LLVKTracker::GLName>& sampled,
                          Uniforms& uniforms, LLVKFullscreen::Image* images, U32 image_count, bool scene_depth_first)
    {
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(sampled, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != width || target.height != height)
        {
            LL_WARNS_ONCE("VKGtao") << pass.name() << ": the bound target is " << target.width << "x" << target.height << ", not "
                                    << width << "x" << height << "; not drawn" << LL_ENDL;
            LLVKTracker::endPass();
            return false;
        }
        if (scene_depth_first)
        {
            images[0] = sceneDepth(images[0].texture, target);
        }
        const bool drawn = pass.draw(cmd, target, &uniforms, sizeof(uniforms), images, image_count);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::depth(const LLRHI::GtaoDepth& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !viewUniforms(pass.view, uniforms))
        {
            return false;
        }
        uniforms.scale = (S32)llmax(1u, pass.scale);
        const U32 width = (pass.view.width + uniforms.scale - 1) / uniforms.scale;
        const U32 height = (pass.view.height + uniforms.scale - 1) / uniforms.scale;
        LLVKFullscreen::Image images[1] = { { pass.view.depth_texture } };
        return record(mDepth, width, height, { pass.view.depth_texture }, uniforms, images, 1, true);
    }

    bool Renderer::occlusion(const LLRHI::GtaoOcclusion& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !viewUniforms(pass.view, uniforms) || !pass.view_depth_texture || !pass.width || !pass.height ||
            !pass.slices || !pass.steps || pass.radius <= 0.f)
        {
            return false;
        }
        uniforms.source_size[0] = (S32)pass.width;
        uniforms.source_size[1] = (S32)pass.height;
        uniforms.scale = (S32)llmax(1u, pass.scale);
        uniforms.slices = (S32)llmin(pass.slices, MAX_SLICES);
        uniforms.steps = (S32)llmin(pass.steps, MAX_STEPS);
        uniforms.radius = pass.radius;
        uniforms.max_screen_radius = pass.max_screen_radius;
        LLVKFullscreen::Image images[1] = { { pass.view_depth_texture } };
        const bool drawn = record(mOcclusion, pass.width, pass.height, { pass.view_depth_texture }, uniforms, images, 1, false);
        if (drawn && !mLoggedOcclusion)
        {
            mLoggedOcclusion = true;
            LL_INFOS("VKGtao") << "GTAO: " << pass.width << "x" << pass.height << " (1/" << uniforms.scale << "), "
                               << uniforms.slices << " slices x " << uniforms.steps << " steps a side, radius "
                               << pass.radius << " m" << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::denoise(const LLRHI::GtaoDenoise& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.occlusion_texture || !pass.width || !pass.height)
        {
            return false;
        }
        Uniforms uniforms = {};
        uniforms.source_size[0] = (S32)pass.width;
        uniforms.source_size[1] = (S32)pass.height;
        LLVKFullscreen::Image images[1] = { { pass.occlusion_texture } };
        return record(mDenoise, pass.width, pass.height, { pass.occlusion_texture }, uniforms, images, 1, false);
    }

    bool Renderer::applyUniforms(const LLRHI::GtaoApply& pass, Uniforms& uniforms)
    {
        if (!viewUniforms(pass.view, uniforms) || !pass.occlusion_texture || !pass.width || !pass.height)
        {
            return false;
        }
        uniforms.source_size[0] = (S32)pass.width;
        uniforms.source_size[1] = (S32)pass.height;
        uniforms.scale = (S32)llmax(1u, pass.scale);
        uniforms.power = pass.power;
        return true;
    }

    bool Renderer::contactUniforms(const LLRHI::ContactShadows& pass, Uniforms& uniforms)
    {
        // The fades' ends must be in order (smoothstep is undefined otherwise).
        if (!viewUniforms(pass.view, uniforms) || !pass.steps || pass.length <= 0.f || pass.fade_end <= pass.fade_start ||
            pass.max_screen_length <= 0.f || pass.near_fade_end <= pass.near_fade_start)
        {
            return false;
        }
        memcpy(uniforms.light_direction, pass.light_direction, sizeof(pass.light_direction));
        uniforms.steps = (S32)llmin(pass.steps, MAX_CONTACT_STEPS);
        uniforms.ray_length = pass.length;
        uniforms.thickness = pass.thickness;
        uniforms.fade_start = pass.fade_start;
        uniforms.fade_end = pass.fade_end;
        uniforms.max_screen_length = pass.max_screen_length;
        uniforms.near_fade_start = pass.near_fade_start;
        uniforms.near_fade_end = pass.near_fade_end;
        return true;
    }

    void Renderer::logContact(const LLRHI::ContactShadows& pass, const Uniforms& uniforms)
    {
        if (!mLoggedContact)
        {
            mLoggedContact = true;
            LL_INFOS("VKGtao") << "Contact shadows: " << pass.view.width << "x" << pass.view.height << ", up to " << uniforms.steps
                               << " steps over " << pass.length << " m (at most " << pass.max_screen_length
                               << " pixels on screen; close up they fade out from a metre of " << pass.near_fade_start
                               << " to " << pass.near_fade_end << " screen heights)" << LL_ENDL;
        }
    }

    bool Renderer::apply(const LLRHI::GtaoApply& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !applyUniforms(pass, uniforms))
        {
            return false;
        }
        LLVKFullscreen::Image images[2] = { { pass.view.depth_texture }, { pass.occlusion_texture } };
        return record(mApply, pass.view.width, pass.view.height, { pass.view.depth_texture, pass.occlusion_texture }, uniforms, images, 2, true);
    }

    bool Renderer::contact(const LLRHI::ContactShadows& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !contactUniforms(pass, uniforms))
        {
            return false;
        }
        LLVKFullscreen::Image images[1] = { { pass.view.depth_texture } };
        const bool drawn = record(mContact, pass.view.width, pass.view.height, { pass.view.depth_texture }, uniforms, images, 1, true);
        if (drawn)
        {
            logContact(pass, uniforms);
        }
        return drawn;
    }

    bool Renderer::applyContact(const LLRHI::GtaoApply& apply, const LLRHI::ContactShadows& contact)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        // One block for both: the view's fields are the same, each pass's own do not overlap.
        Uniforms uniforms;
        Uniforms apply_uniforms;
        if (!ready() || !mApplyContactReady || !sameView(apply.view, contact.view) || !contactUniforms(contact, uniforms) ||
            !applyUniforms(apply, apply_uniforms))
        {
            return false;
        }
        uniforms.source_size[0] = apply_uniforms.source_size[0];
        uniforms.source_size[1] = apply_uniforms.source_size[1];
        uniforms.scale = apply_uniforms.scale;
        uniforms.power = apply_uniforms.power;
        LLVKFullscreen::Image images[2] = { { contact.view.depth_texture }, { apply.occlusion_texture } };
        const bool drawn = record(mApplyContact, contact.view.width, contact.view.height,
                                  { contact.view.depth_texture, apply.occlusion_texture }, uniforms, images, 2, true);
        if (drawn)
        {
            logContact(contact, uniforms);
        }
        return drawn;
    }

    bool record_depth(const LLRHI::GtaoDepth& pass)
    {
        return sRenderer.depth(pass);
    }

    bool record_occlusion(const LLRHI::GtaoOcclusion& pass)
    {
        return sRenderer.occlusion(pass);
    }

    bool record_denoise(const LLRHI::GtaoDenoise& pass)
    {
        return sRenderer.denoise(pass);
    }

    bool record_apply(const LLRHI::GtaoApply& pass)
    {
        return sRenderer.apply(pass);
    }

    bool record_contact(const LLRHI::ContactShadows& pass)
    {
        return sRenderer.contact(pass);
    }

    bool record_apply_contact(const LLRHI::GtaoApply& apply, const LLRHI::ContactShadows& contact)
    {
        return sRenderer.applyContact(apply, contact);
    }
}

void LLVKGtao::install()
{
    if (!LLRHI::sGtao)
    {
        LL_INFOS("VKGtao") << "GTAO and contact shadows off (LLRHI_GTAO=0)" << LL_ENDL;
        return;
    }
    LLRHI::sGtaoDepth = &record_depth;
    LLRHI::sGtaoOcclusion = &record_occlusion;
    LLRHI::sGtaoDenoise = &record_denoise;
    LLRHI::sGtaoApply = &record_apply;
    LLRHI::sContactShadows = &record_contact;
    LLRHI::sGtaoApplyContact = &record_apply_contact;
    LLVKShader::addBakeStep("GTAO", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
