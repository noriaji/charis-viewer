/**
 * @file llvkglare.cpp
 * @brief Glare (HDR bloom), the sun's lens flare and the cross filter
 *        recorded directly in Vulkan (research/108, 119).
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

#include "llvkglare.h"

#include "llrhiglare.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <algorithm>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Shaders

    // GlareDownsample: the 13-tap average of Jimenez 2014 (36 texels through
    // 13 bilinear reads, weighted as five overlapping 2x2 boxes). With
    // prefilter (the first level): the exposure applied, each box weighted by
    // 1 / (1 + brightness) (Karis) so a single very bright texel does not
    // flicker, then only what lies above the soft threshold kept.
    const char* const DOWNSAMPLE_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Downsample
{
    vec2 source_texel;          // 1 / the source's size
    vec2 target_size;           // the bound target's size
    float threshold;            // prefilter: exposed brightness where the glare starts
    float knee;                 // prefilter: the soft range around it
    float exposure_scale;       // prefilter: RenderExposure; 0: no exposure (1, the map not read)
    int prefilter;              // 1: the first level
} u;

layout(set = 0, binding = 1) uniform sampler2D source;
layout(set = 0, binding = 2) uniform sampler2D exposure_map;

layout(location = 0) out vec4 frag_color;

// Larger values (a broken pixel, the sun's disc in a float target) are cut here.
const float MAX_VALUE = 60000.0;

vec3 tap(vec2 uv)
{
    vec3 c = textureLod(source, uv, 0.0).rgb;
    return any(isnan(c)) ? vec3(0.0) : clamp(c, vec3(0.0), vec3(MAX_VALUE));
}

float karis(vec3 c)
{
    return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
}

vec3 softThreshold(vec3 c)
{
    float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - u.threshold + u.knee, 0.0, 2.0 * u.knee);
    soft = soft * soft / (4.0 * u.knee + 1e-5);
    return c * (max(soft, brightness - u.threshold) / max(brightness, 1e-5));
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u.target_size;
    vec2 t = u.source_texel;
    vec3 a = tap(uv + t * vec2(-2.0,  2.0));
    vec3 b = tap(uv + t * vec2( 0.0,  2.0));
    vec3 c = tap(uv + t * vec2( 2.0,  2.0));
    vec3 d = tap(uv + t * vec2(-2.0,  0.0));
    vec3 e = tap(uv);
    vec3 f = tap(uv + t * vec2( 2.0,  0.0));
    vec3 g = tap(uv + t * vec2(-2.0, -2.0));
    vec3 h = tap(uv + t * vec2( 0.0, -2.0));
    vec3 i = tap(uv + t * vec2( 2.0, -2.0));
    vec3 j = tap(uv + t * vec2(-1.0,  1.0));
    vec3 k = tap(uv + t * vec2( 1.0,  1.0));
    vec3 l = tap(uv + t * vec2(-1.0, -1.0));
    vec3 m = tap(uv + t * vec2( 1.0, -1.0));

    vec3 result;
    if (u.prefilter != 0)
    {
        float exposure = u.exposure_scale > 0.0 ? texelFetch(exposure_map, ivec2(0), 0).r * u.exposure_scale : 1.0;
        vec3 box0 = (j + k + l + m) * (0.25 * exposure);
        vec3 box1 = (a + b + d + e) * (0.25 * exposure);
        vec3 box2 = (b + c + e + f) * (0.25 * exposure);
        vec3 box3 = (d + e + g + h) * (0.25 * exposure);
        vec3 box4 = (e + f + h + i) * (0.25 * exposure);
        float w0 = 0.5 * karis(box0);
        float w1 = 0.125 * karis(box1);
        float w2 = 0.125 * karis(box2);
        float w3 = 0.125 * karis(box3);
        float w4 = 0.125 * karis(box4);
        result = (box0 * w0 + box1 * w1 + box2 * w2 + box3 * w3 + box4 * w4) / (w0 + w1 + w2 + w3 + w4);
        result = softThreshold(result);
    }
    else
    {
        result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    frag_color = vec4(result, 1.0);
}
)GLSL";

    // GlareUpsample: the next smaller level through a 3x3 tent (9 bilinear
    // reads, one texel apart); the blend adds it to the bound level.
    const char* const UPSAMPLE_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Upsample
{
    vec2 source_texel;          // 1 / the source's size
    vec2 target_size;           // the bound target's size
} u;

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec2 uv = gl_FragCoord.xy / u.target_size;
    vec2 t = u.source_texel;
    vec3 sum = textureLod(source, uv, 0.0).rgb * 4.0;
    sum += (textureLod(source, uv + vec2(-t.x, 0.0), 0.0).rgb + textureLod(source, uv + vec2(t.x, 0.0), 0.0).rgb +
            textureLod(source, uv + vec2(0.0, -t.y), 0.0).rgb + textureLod(source, uv + vec2(0.0, t.y), 0.0).rgb) * 2.0;
    sum += textureLod(source, uv + vec2(-t.x, -t.y), 0.0).rgb + textureLod(source, uv + vec2(t.x, -t.y), 0.0).rgb +
           textureLod(source, uv + vec2(-t.x, t.y), 0.0).rgb + textureLod(source, uv + vec2(t.x, t.y), 0.0).rgb;
    frag_color = vec4(sum / 16.0, 0.0);
}
)GLSL";

    // GlareSunVisibility: how much of the sun is seen, from the exposed
    // brightness at 32 points of a disc around its place (a sunflower
    // pattern). The blend (premultiplied over) mixes it with the target's
    // value by u.blend: a running average that keeps leaves from flickering it.
    const char* const VISIBILITY_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Visibility
{
    vec2 sun_place;             // texture coordinates of the scene target
    vec2 radius;                // the disc's radius, in texture coordinates each way
    float dark;                 // exposed brightness: below, hidden
    float bright;               // above, seen
    float blend;                // this frame's share
    float exposure_scale;       // RenderExposure; 0: no exposure (1, the map not read)
} u;

layout(set = 0, binding = 1) uniform sampler2D screen;
layout(set = 0, binding = 2) uniform sampler2D exposure_map;

layout(location = 0) out vec4 frag_color;

const int SAMPLES = 32;
const float GOLDEN_ANGLE = 2.39996323;

void main()
{
    float exposure = u.exposure_scale > 0.0 ? texelFetch(exposure_map, ivec2(0), 0).r * u.exposure_scale : 1.0;
    float seen = 0.0;
    for (int i = 0; i < SAMPLES; ++i)
    {
        float angle = float(i) * GOLDEN_ANGLE;
        vec2 place = u.sun_place + vec2(cos(angle), sin(angle)) * sqrt((float(i) + 0.5) / float(SAMPLES)) * u.radius;
        vec3 c = textureLod(screen, clamp(place, vec2(0.0), vec2(1.0)), 0.0).rgb * exposure;
        float brightness = dot(c, vec3(0.2126, 0.7152, 0.0722));
        seen += isnan(brightness) ? 0.0 : smoothstep(u.dark, u.bright, brightness);
    }
    seen /= float(SAMPLES);
    frag_color = vec4(seen * u.blend, 0.0, 0.0, u.blend);
}
)GLSL";

    // GlareStreakExtract: the light the cross filter's rays start from, at a
    // quarter of the screen's size. Each texel takes its 4x4 block of the
    // screen (four bilinear reads), exposed. It is kept only when it is far
    // above white (a soft gate, the colour kept as it is: the rays take the
    // light's own colour) and much brighter than the brightest of 8 points on
    // a ring around it: a lamp, the sun or a glint are; a sunlit wall, the
    // sky or the edge of either are not, as the ring finds them as bright.
    // The rays' strength rises with the light's place between the gate and
    // u.limit (squared) and is capped there, so only a strong light has long
    // rays, and the sun's do not wash the picture out; the rays' length, which
    // grows with the log of their root's brightness, stops growing. Points of the sky (the
    // scene's depth at the far end: the stars, many and all as bright once
    // the night's exposure is raised) are weakened by u.sky, except around
    // the sun.
    const char* const STREAK_EXTRACT_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform StreakExtract
{
    vec2 screen_texel;          // 1 / the scene target's size
    vec2 target_size;           // the bound target's size
    vec2 ring_radius;           // texture coordinates each way
    float threshold;            // exposed brightness where the rays start
    float knee;                 // the soft range around it
    float limit;                // the exposed brightness of full rays (the cap)
    float exposure_scale;       // RenderExposure; 0: no exposure (1, the map not read)
    float point_full;           // the ring's brightest at most this share: a light
    float point_none;           // at least this share: a bright area
    vec2 sun_place;             // texture coordinates
    vec2 sun_radius;            // each way; 0: no sun
    float sky;                  // points of the sky times this; 1: scene_depth is not read
} u;

layout(set = 0, binding = 1) uniform sampler2D screen;
layout(set = 0, binding = 2) uniform sampler2D exposure_map;
layout(set = 0, binding = 3) uniform sampler2D scene_depth;

layout(location = 0) out vec4 frag_color;

// Larger values (a broken pixel, the sun's disc in a float target) are cut here.
const float MAX_VALUE = 60000.0;
const int RING = 8;
const float TWO_PI = 6.28318531;
const float PEAK = 4.0;     // a full light's brightness at the rays' root
// The sun's and the moon's: the rays as they were before research/269.
const float SUN_LIMIT = 4.0;
const float SUN_POINT_FULL = 0.25;
const float SUN_POINT_NONE = 0.75;

vec3 tap(vec2 uv)
{
    vec3 c = textureLod(screen, uv, 0.0).rgb;
    return any(isnan(c)) ? vec3(0.0) : clamp(c, vec3(0.0), vec3(MAX_VALUE));
}

float brightness(vec3 c)
{
    return max(c.r, max(c.g, c.b));
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u.target_size;
    vec2 t = u.screen_texel;
    float exposure = u.exposure_scale > 0.0 ? texelFetch(exposure_map, ivec2(0), 0).r * u.exposure_scale : 1.0;
    exposure = (isnan(exposure) || isinf(exposure)) ? 1.0 : exposure;
    vec3 c = (tap(uv + t * vec2(-1.0, -1.0)) + tap(uv + t * vec2(1.0, -1.0)) +
              tap(uv + t * vec2(-1.0,  1.0)) + tap(uv + t * vec2(1.0,  1.0))) * (0.25 * exposure);
    float b = brightness(c);
    if (!(b > u.threshold - u.knee))
    {
        frag_color = vec4(0.0);     // most of the picture
        return;
    }
    // Around the sun (and the moon at night) the rays stay as they were: whole,
    // by the light's roll-off towards SUN_LIMIT and a plain point test.
    float sun = 0.0;
    if (u.sun_radius.y > 0.0)
    {
        vec2 d = (uv - u.sun_place) / u.sun_radius;
        sun = 1.0 - smoothstep(0.5, 1.0, length(d));
    }
    float ring = 0.0;
    for (int i = 0; i < RING; ++i)
    {
        float angle = (float(i) + 0.5) * (TWO_PI / float(RING));
        ring = max(ring, brightness(tap(uv + vec2(cos(angle), sin(angle)) * u.ring_radius)));
    }
    float share = ring * exposure / b;
    float point = 1.0 - smoothstep(mix(u.point_full, SUN_POINT_FULL, sun), mix(u.point_none, SUN_POINT_NONE, sun), share);
    float gate = smoothstep(u.threshold - u.knee, u.threshold + u.knee, b);
    // The rays' strength rises with the square of the light's place between
    // the start of the gate and u.limit, where it is full (and stays, so the
    // sun does not wash the picture out): a camera's rays are only the
    // strongest lights'. The root's brightness is then PEAK at most.
    float level = smoothstep(u.threshold - u.knee, max(u.limit, u.threshold), b);
    float weight = gate * point * mix(PEAK * level * level / b, 1.0 / (1.0 + b / SUN_LIMIT), sun);
    if (u.sky < 1.0 && weight > 0.0)
    {
        // The block is the sky only when all of its four reads are (the sky
        // writes no depth: 1, as atmosphere's passes take it); a lamp in
        // front of the sky, even a small one, has some of them nearer.
        vec2 size = vec2(textureSize(scene_depth, 0));
        ivec2 last = ivec2(size) - 1;
        float nearest = 1.0;
        for (int i = 0; i < 4; ++i)
        {
            vec2 corner = uv + t * vec2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0);
            ivec2 pixel = clamp(ivec2(corner * size), ivec2(0), last);
            nearest = min(nearest, texelFetch(scene_depth, pixel, 0).r);
        }
        if (nearest >= 1.0)
        {
            weight *= mix(u.sky, 1.0, sun);
        }
    }
    frag_color = vec4(c * weight, 1.0);
}
)GLSL";

    // GlareStreak: one pass of one ray (Kawase). The taps lie behind the texel
    // towards the ray's root, u.step apart, each weighted by u.tap_decay times
    // the one before. With n taps a pass and the passes n^0, n^1, n^2 ...
    // texels apart, every distance along the ray is reached in exactly one
    // way, so the passes together weigh it by decay^distance: the ray falls
    // off smoothly, without beads. Taps off the picture end the ray (the
    // default mirrored edge would send it back in).
    const char* const STREAK_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Streak
{
    vec2 step_uv;               // from one tap to the next, texture coordinates
    vec2 target_size;           // the bound target's size
    float tap_decay;            // each tap's weight, times the one before
    int taps;
} u;

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec2 uv = gl_FragCoord.xy / u.target_size;
    vec3 sum = vec3(0.0);
    float weight = 1.0;
    for (int s = 0; s < u.taps; ++s)
    {
        vec2 p = uv + u.step_uv * float(s);
        if (any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0))))
        {
            break;
        }
        sum += textureLod(source, p, 0.0).rgb * weight;
        weight *= u.tap_decay;
    }
    frag_color = vec4(sum, 0.0);
}
)GLSL";

    // GlareComposite: the bloom (the first level, tent-filtered), the sun's
    // lens flare and the cross filter's rays, in exposed units, brought back
    // to the scene's units by the exposure; the blend adds them.
    //
    // The flare is drawn from the sun's place, not read from the picture: six
    // ghosts on the line from the sun through the centre of the screen (a
    // soft disc each, its radius a little different for red, green and blue
    // so the rim shows the colours of a rainbow), a thin rainbow ring around
    // the sun's mirror image across the centre, and a faint warm halo around
    // the sun. Distances are in shares of the screen's height.
    const char* const COMPOSITE_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Composite
{
    vec4 flare_color;           // rgb: the sun's colour (about 1 in brightness), a: the flare's strength (0: none)
    vec2 target_size;           // the bound target's size
    vec2 bloom_texel;           // 1 / the first level's size
    vec2 sun_place;             // texture coordinates of the target
    float bloom_intensity;      // 0: no bloom
    float exposure_scale;       // RenderExposure; 0: no exposure (1, the map not read)
    vec2 streak_texel;          // 1 / the rays' size
    float streak_intensity;     // 0: no cross filter
    float streak_rays;          // how many rays hold the extracted light at their root
} u;

layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D exposure_map;
layout(set = 0, binding = 3) uniform sampler2D visibility;
layout(set = 0, binding = 4) uniform sampler2D streak;
layout(set = 0, binding = 5) uniform sampler2D extract;

layout(location = 0) out vec4 frag_color;

// The ghosts. x: the place on the line, 1 at the sun, 0 at the centre,
// below 0 beyond the centre; y: the radius; z: the brightness.
const int GHOST_COUNT = 6;
const vec3 GHOSTS[GHOST_COUNT] = vec3[GHOST_COUNT](
    vec3( 0.45, 0.025, 0.35),
    vec3(-0.20, 0.060, 0.18),
    vec3(-0.50, 0.110, 0.10),
    vec3(-0.80, 0.035, 0.28),
    vec3(-1.15, 0.160, 0.07),
    vec3(-1.60, 0.075, 0.14));
const vec3 GHOST_TINTS[GHOST_COUNT] = vec3[GHOST_COUNT](
    vec3(1.00, 0.85, 0.60),
    vec3(0.65, 1.00, 0.75),
    vec3(0.70, 0.80, 1.00),
    vec3(1.00, 0.75, 0.55),
    vec3(0.65, 0.75, 1.00),
    vec3(0.90, 0.65, 1.00));
// Red's radius is this much larger, blue's this much smaller (the rainbow rims).
const float ABERRATION = 0.035;

const float RING_RADIUS = 0.26;
const float RING_WIDTH = 0.012;
const float RING_BRIGHTNESS = 0.06;

const vec3 HALO_TINT = vec3(1.0, 0.72, 0.45);
const float HALO_FALLOFF = 0.12;    // the glow around the sun drops to 1/e at this distance
const float HALO_GLOW = 0.25;
const float HALO_RADIUS = 0.32;     // and a faint ring around it
const float HALO_WIDTH = 0.03;
const float HALO_RING = 0.03;

vec3 tent(vec2 uv)
{
    vec2 t = u.bloom_texel;
    vec3 sum = textureLod(bloom, uv, 0.0).rgb * 4.0;
    sum += (textureLod(bloom, uv + vec2(-t.x, 0.0), 0.0).rgb + textureLod(bloom, uv + vec2(t.x, 0.0), 0.0).rgb +
            textureLod(bloom, uv + vec2(0.0, -t.y), 0.0).rgb + textureLod(bloom, uv + vec2(0.0, t.y), 0.0).rgb) * 2.0;
    sum += textureLod(bloom, uv + vec2(-t.x, -t.y), 0.0).rgb + textureLod(bloom, uv + vec2(t.x, -t.y), 0.0).rgb +
           textureLod(bloom, uv + vec2(-t.x, t.y), 0.0).rgb + textureLod(bloom, uv + vec2(t.x, t.y), 0.0).rgb;
    return sum / 16.0;
}

// A quarter-size target, smoothed over 2x2 of its texels as it is enlarged
// (a single bilinear read shows the texels' diamonds along a thin ray).
vec3 smoothQuarter(sampler2D image, vec2 uv)
{
    vec2 h = 0.5 * u.streak_texel;
    return 0.25 * (textureLod(image, uv + vec2(-h.x, -h.y), 0.0).rgb + textureLod(image, uv + vec2(h.x, -h.y), 0.0).rgb +
                   textureLod(image, uv + vec2(-h.x, h.y), 0.0).rgb + textureLod(image, uv + vec2(h.x, h.y), 0.0).rgb);
}

// A soft disc, a little brighter towards its rim.
float disc(float d, float r)
{
    float inside = 1.0 - smoothstep(0.8 * r, r, d);
    float rim = smoothstep(0.6 * r, 0.95 * r, d);
    return inside * (0.55 + 0.45 * rim);
}

vec3 rainbowDisc(float d, float r)
{
    return vec3(disc(d, r * (1.0 + ABERRATION)), disc(d, r), disc(d, r * (1.0 - ABERRATION)));
}

float ring(float d, float r, float width)
{
    float x = (d - r) / width;
    return exp(-x * x);
}

vec3 flare(vec2 uv)
{
    vec2 scale = vec2(u.target_size.x / u.target_size.y, 1.0);
    vec2 p = uv * scale;
    vec2 centre = 0.5 * scale;
    vec2 sun = u.sun_place * scale;
    vec2 axis = sun - centre;

    vec3 sum = vec3(0.0);
    for (int i = 0; i < GHOST_COUNT; ++i)
    {
        float d = distance(p, centre + axis * GHOSTS[i].x);
        sum += rainbowDisc(d, GHOSTS[i].y) * GHOST_TINTS[i] * GHOSTS[i].z;
    }

    float mirrored = distance(p, centre - axis);
    sum += vec3(ring(mirrored, RING_RADIUS * (1.0 + 2.0 * ABERRATION), RING_WIDTH), ring(mirrored, RING_RADIUS, RING_WIDTH),
                ring(mirrored, RING_RADIUS * (1.0 - 2.0 * ABERRATION), RING_WIDTH)) * RING_BRIGHTNESS;

    float from_sun = distance(p, sun);
    sum += HALO_TINT * (exp(-from_sun / HALO_FALLOFF) * HALO_GLOW + ring(from_sun, HALO_RADIUS, HALO_WIDTH) * HALO_RING);
    return sum;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u.target_size;
    float exposure = u.exposure_scale > 0.0 ? max(texelFetch(exposure_map, ivec2(0), 0).r * u.exposure_scale, 1e-4) : 1.0;
    vec3 light = vec3(0.0);
    if (u.bloom_intensity > 0.0)
    {
        light += tent(uv) * u.bloom_intensity;
    }
    if (u.flare_color.a > 0.0)
    {
        float seen = texelFetch(visibility, ivec2(0), 0).r;
        if (seen > 0.001)
        {
            light += flare(uv) * u.flare_color.rgb * (u.flare_color.a * seen);
        }
    }
    if (u.streak_intensity > 0.0)
    {
        // Each ray starts with the light itself; that is already in the picture.
        vec3 rays = smoothQuarter(streak, uv) - smoothQuarter(extract, uv) * u.streak_rays;
        light += max(rays, vec3(0.0)) * u.streak_intensity;
    }
    frag_color = vec4(light / exposure, 0.0);
}
)GLSL";

    // ------------------------------------------------------------------
    // Uniform blocks, as the shaders' std140 blocks lay them out.

    struct DownsampleUniforms
    {
        F32 source_texel[2];
        F32 target_size[2];
        F32 threshold;
        F32 knee;
        F32 exposure_scale;
        S32 prefilter;
    };
    static_assert(sizeof(DownsampleUniforms) == 32, "DownsampleUniforms must match the std140 block Downsample");

    struct UpsampleUniforms
    {
        F32 source_texel[2];
        F32 target_size[2];
    };
    static_assert(sizeof(UpsampleUniforms) == 16, "UpsampleUniforms must match the std140 block Upsample");

    struct VisibilityUniforms
    {
        F32 sun_place[2];
        F32 radius[2];
        F32 dark;
        F32 bright;
        F32 blend;
        F32 exposure_scale;
    };
    static_assert(sizeof(VisibilityUniforms) == 32, "VisibilityUniforms must match the std140 block Visibility");

    struct StreakExtractUniforms
    {
        F32 screen_texel[2];
        F32 target_size[2];
        F32 ring_radius[2];
        F32 threshold;
        F32 knee;
        F32 limit;
        F32 exposure_scale;
        F32 point_full;
        F32 point_none;
        F32 sun_place[2];
        F32 sun_radius[2];
        F32 sky;
        F32 padding[3];     // not in the block (its size is 68): keeps the struct a whole number of vec4s
    };
    static_assert(sizeof(StreakExtractUniforms) == 80, "StreakExtractUniforms must match the std140 block StreakExtract");

    struct StreakUniforms
    {
        F32 step_uv[2];
        F32 target_size[2];
        F32 tap_decay;
        S32 taps;
        F32 padding[2];     // not in the block (its size is 24): keeps the struct a whole number of vec4s
    };
    static_assert(sizeof(StreakUniforms) == 32, "StreakUniforms must match the std140 block Streak");

    struct CompositeUniforms
    {
        F32 flare_color[4];
        F32 target_size[2];
        F32 bloom_texel[2];
        F32 sun_place[2];
        F32 bloom_intensity;
        F32 exposure_scale;
        F32 streak_texel[2];
        F32 streak_intensity;
        F32 streak_rays;
    };
    static_assert(sizeof(CompositeUniforms) == 64, "CompositeUniforms must match the std140 block Composite");

    constexpr U32 DOWNSAMPLE_IMAGES = 2;    // the level above, the exposure map
    constexpr U32 UPSAMPLE_IMAGES = 1;      // the next smaller level
    constexpr U32 VISIBILITY_IMAGES = 2;    // the scene target, the exposure map
    constexpr U32 STREAK_EXTRACT_IMAGES = 3;    // the scene target, the exposure map, the scene depth
    constexpr U32 STREAK_IMAGES = 1;        // the extracted light or the ray's previous pass
    constexpr U32 COMPOSITE_IMAGES = 5;     // the first level, the exposure map, the sun's visibility, the rays, the extracted light

    // A ray's pass reads at most this many taps (glare.cpp chooses within it).
    constexpr U32 MAX_STREAK_TAPS = 16;

    // The passes write only R of the visibility target (R16F).
    constexpr VkColorComponentFlags WRITE_R = VK_COLOR_COMPONENT_R_BIT;

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool downsample(const LLRHI::GlareDownsample& pass);
        bool upsample(const LLRHI::GlareUpsample& pass);
        bool sunVisibility(const LLRHI::GlareSunVisibility& pass);
        bool streakExtract(const LLRHI::GlareStreakExtract& pass);
        bool streak(const LLRHI::GlareStreak& pass);
        bool composite(const LLRHI::GlareComposite& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();
        // Opens the pass on the bound target; null (nothing opened) when it
        // cannot, or when the target is not width x height (warned once).
        VkCommandBuffer begin(std::initializer_list<LLVKTracker::GLName> sampled, U32 width, U32 height, LLVKMirror::Target& target);

        // begin()'s textures, each once: kept so that no frame allocates them anew.
        std::vector<LLVKTracker::GLName> mUnique;
        bool mTried = false;
        bool mReady = false;
        LLVKFullscreen::Pass mDownsample;
        LLVKFullscreen::Pass mUpsample;
        LLVKFullscreen::Pass mVisibility;       // mixed with the target's value
        LLVKFullscreen::Pass mVisibilityFirst;  // written as it is (a new target holds nothing to mix with)
        LLVKFullscreen::Pass mStreakExtract;
        LLVKFullscreen::Pass mStreak;           // written as it is
        LLVKFullscreen::Pass mStreakAdd;        // added to the target (the rays after the first)
        LLVKFullscreen::Pass mComposite;
        bool mWarnedTarget = false;
        bool mLoggedFirst = false;
        bool mLoggedStreak = false;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = mDownsample.init("glare downsample", DOWNSAMPLE_FRAGMENT, DOWNSAMPLE_IMAGES) &&
                     mUpsample.init("glare upsample", UPSAMPLE_FRAGMENT, UPSAMPLE_IMAGES, LLVKFullscreen::Blend::Add) &&
                     mVisibility.init("glare sun visibility", VISIBILITY_FRAGMENT, VISIBILITY_IMAGES,
                                      LLVKFullscreen::Blend::PremultipliedOver, WRITE_R) &&
                     mVisibilityFirst.init("glare sun visibility (first)", VISIBILITY_FRAGMENT, VISIBILITY_IMAGES,
                                           LLVKFullscreen::Blend::Replace, WRITE_R) &&
                     mStreakExtract.init("glare streak extract", STREAK_EXTRACT_FRAGMENT, STREAK_EXTRACT_IMAGES) &&
                     mStreak.init("glare streak", STREAK_FRAGMENT, STREAK_IMAGES) &&
                     mStreakAdd.init("glare streak (added)", STREAK_FRAGMENT, STREAK_IMAGES, LLVKFullscreen::Blend::Add) &&
                     mComposite.init("glare composite", COMPOSITE_FRAGMENT, COMPOSITE_IMAGES, LLVKFullscreen::Blend::Add);
            if (!mReady)
            {
                LL_WARNS("VKGlare") << "Glare, lens flare and cross filter could not be set up; they stay off" << LL_ENDL;
            }
        }
        return mReady;
    }

    VkCommandBuffer Renderer::begin(std::initializer_list<LLVKTracker::GLName> sampled, U32 width, U32 height, LLVKMirror::Target& target)
    {
        // A stand-in texture may be named twice: each is moved to shader-read layout once.
        mUnique.clear();
        for (LLVKTracker::GLName texture : sampled)
        {
            if (std::find(mUnique.begin(), mUnique.end(), texture) == mUnique.end())
            {
                mUnique.push_back(texture);
            }
        }
        VkCommandBuffer cmd = LLVKTracker::beginPass(mUnique, target);
        if (cmd && (target.width != width || target.height != height))
        {
            LLVKTracker::endPass();
            if (!mWarnedTarget)
            {
                mWarnedTarget = true;
                LL_WARNS("VKGlare") << "Glare: the bound target is " << target.width << "x" << target.height << ", not the " << width
                                    << "x" << height << " it was called for; not drawn" << LL_ENDL;
            }
            return VK_NULL_HANDLE;
        }
        return cmd;
    }

    bool Renderer::downsample(const LLRHI::GlareDownsample& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.source_texture || !pass.source_width || !pass.source_height || !pass.width || !pass.height ||
            (pass.prefilter && !pass.exposure.texture))
        {
            return false;
        }
        // Without the prefilter the exposure map is not read: the source stands in for it.
        const LLVKTracker::GLName exposure = pass.prefilter ? pass.exposure.texture : pass.source_texture;
        LLVKMirror::Target target;
        VkCommandBuffer cmd = begin({ pass.source_texture, exposure }, pass.width, pass.height, target);
        if (!cmd)
        {
            return false;
        }
        DownsampleUniforms uniforms = {};
        uniforms.source_texel[0] = 1.f / (F32)pass.source_width;
        uniforms.source_texel[1] = 1.f / (F32)pass.source_height;
        uniforms.target_size[0] = (F32)pass.width;
        uniforms.target_size[1] = (F32)pass.height;
        uniforms.threshold = pass.threshold;
        uniforms.knee = llmax(pass.knee, 1e-3f);
        uniforms.exposure_scale = pass.exposure.scale;
        uniforms.prefilter = pass.prefilter ? 1 : 0;
        const LLVKFullscreen::Image images[DOWNSAMPLE_IMAGES] = { { pass.source_texture }, { exposure } };
        const bool drawn = mDownsample.draw(cmd, target, &uniforms, sizeof(uniforms), images, DOWNSAMPLE_IMAGES);
        LLVKTracker::endPass();
        if (drawn && pass.prefilter && !mLoggedFirst)
        {
            mLoggedFirst = true;
            LL_INFOS("VKGlare") << "Glare: first level " << pass.width << "x" << pass.height << " from " << pass.source_width << "x"
                                << pass.source_height << ", target format " << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::upsample(const LLRHI::GlareUpsample& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.source_texture || !pass.source_width || !pass.source_height || !pass.width || !pass.height)
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = begin({ pass.source_texture }, pass.width, pass.height, target);
        if (!cmd)
        {
            return false;
        }
        UpsampleUniforms uniforms = {};
        uniforms.source_texel[0] = 1.f / (F32)pass.source_width;
        uniforms.source_texel[1] = 1.f / (F32)pass.source_height;
        uniforms.target_size[0] = (F32)pass.width;
        uniforms.target_size[1] = (F32)pass.height;
        const LLVKFullscreen::Image images[UPSAMPLE_IMAGES] = { { pass.source_texture } };
        const bool drawn = mUpsample.draw(cmd, target, &uniforms, sizeof(uniforms), images, UPSAMPLE_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::sunVisibility(const LLRHI::GlareSunVisibility& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.screen_texture || !pass.exposure.texture)
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = begin({ pass.screen_texture, pass.exposure.texture }, 1, 1, target);
        if (!cmd)
        {
            return false;
        }
        VisibilityUniforms uniforms = {};
        uniforms.sun_place[0] = pass.sun_place[0];
        uniforms.sun_place[1] = pass.sun_place[1];
        uniforms.radius[0] = pass.radius[0];
        uniforms.radius[1] = pass.radius[1];
        uniforms.dark = pass.dark;
        uniforms.bright = llmax(pass.bright, pass.dark + 1e-3f);
        uniforms.blend = llclamp(pass.blend, 0.f, 1.f);
        uniforms.exposure_scale = pass.exposure.scale;
        const LLVKFullscreen::Image images[VISIBILITY_IMAGES] = { { pass.screen_texture }, { pass.exposure.texture } };
        // Blending a new target's undefined value, even with a factor of 0, could keep a NaN for good.
        LLVKFullscreen::Pass& visibility = uniforms.blend >= 1.f ? mVisibilityFirst : mVisibility;
        const bool drawn = visibility.draw(cmd, target, &uniforms, sizeof(uniforms), images, VISIBILITY_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::streakExtract(const LLRHI::GlareStreakExtract& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.screen_texture || !pass.exposure.texture || !pass.screen_width || !pass.screen_height || !pass.width ||
            !pass.height)
        {
            return false;
        }
        // Without the scene depth the sky is not told apart: the exposure map stands in (never read, u.sky 1).
        const bool sky = pass.depth_texture && pass.sky < 1.f;
        const LLVKTracker::GLName depth = sky ? pass.depth_texture : pass.exposure.texture;
        LLVKMirror::Target target;
        VkCommandBuffer cmd = begin({ pass.screen_texture, pass.exposure.texture, depth }, pass.width, pass.height, target);
        if (!cmd)
        {
            return false;
        }
        StreakExtractUniforms uniforms = {};
        uniforms.sky = sky ? llmax(pass.sky, 0.f) : 1.f;
        uniforms.sun_place[0] = pass.sun_place[0];
        uniforms.sun_place[1] = pass.sun_place[1];
        uniforms.sun_radius[0] = llmax(pass.sun_radius[0], 0.f);
        uniforms.sun_radius[1] = uniforms.sun_radius[0] > 0.f ? llmax(pass.sun_radius[1], 0.f) : 0.f;
        uniforms.screen_texel[0] = 1.f / (F32)pass.screen_width;
        uniforms.screen_texel[1] = 1.f / (F32)pass.screen_height;
        uniforms.target_size[0] = (F32)pass.width;
        uniforms.target_size[1] = (F32)pass.height;
        uniforms.ring_radius[0] = pass.ring_radius[0];
        uniforms.ring_radius[1] = pass.ring_radius[1];
        uniforms.knee = llmax(pass.knee, 1e-3f);
        uniforms.threshold = llmax(pass.threshold, uniforms.knee);  // the gate starts above 0
        uniforms.limit = llmax(pass.limit, 1e-3f);
        uniforms.exposure_scale = pass.exposure.scale;
        uniforms.point_full = pass.point_full;
        uniforms.point_none = llmax(pass.point_none, pass.point_full + 1e-3f);
        // The scene depth is not the bound target's (a quarter-size one without depth): sampled in shader-read layout.
        LLVKFullscreen::Image images[STREAK_EXTRACT_IMAGES];
        images[0].texture = pass.screen_texture;
        images[1].texture = pass.exposure.texture;
        images[2] = { depth, sky && target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const bool drawn = mStreakExtract.draw(cmd, target, &uniforms, sizeof(uniforms), images, STREAK_EXTRACT_IMAGES);
        LLVKTracker::endPass();
        if (drawn && !mLoggedStreak)
        {
            mLoggedStreak = true;
            LL_INFOS("VKGlare") << "Cross filter: light extracted at " << pass.width << "x" << pass.height << " from " << pass.screen_width
                                << "x" << pass.screen_height << ", target format " << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::streak(const LLRHI::GlareStreak& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.source_texture || !pass.width || !pass.height || !pass.taps)
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = begin({ pass.source_texture }, pass.width, pass.height, target);
        if (!cmd)
        {
            return false;
        }
        StreakUniforms uniforms = {};
        uniforms.step_uv[0] = pass.step[0];
        uniforms.step_uv[1] = pass.step[1];
        uniforms.target_size[0] = (F32)pass.width;
        uniforms.target_size[1] = (F32)pass.height;
        uniforms.tap_decay = llclamp(pass.tap_decay, 0.f, 1.f);
        uniforms.taps = (S32)llmin(pass.taps, MAX_STREAK_TAPS);
        const LLVKFullscreen::Image images[STREAK_IMAGES] = { { pass.source_texture } };
        LLVKFullscreen::Pass& streak = pass.add ? mStreakAdd : mStreak;
        const bool drawn = streak.draw(cmd, target, &uniforms, sizeof(uniforms), images, STREAK_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::composite(const LLRHI::GlareComposite& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const bool bloom = pass.bloom_texture && pass.bloom_width && pass.bloom_height && pass.bloom_intensity > 0.f;
        const bool flare = pass.visibility_texture && pass.flare_intensity > 0.f;
        const bool streak = pass.streak_texture && pass.extract_texture && pass.streak_width && pass.streak_height &&
                            pass.streak_intensity > 0.f;
        if (!ready() || !pass.exposure.texture || !pass.width || !pass.height || (!bloom && !flare && !streak))
        {
            return false;
        }
        // What is not drawn is not read: the exposure map stands in for it.
        const LLVKTracker::GLName bloom_texture = bloom ? pass.bloom_texture : pass.exposure.texture;
        const LLVKTracker::GLName visibility_texture = flare ? pass.visibility_texture : pass.exposure.texture;
        const LLVKTracker::GLName streak_texture = streak ? pass.streak_texture : pass.exposure.texture;
        const LLVKTracker::GLName extract_texture = streak ? pass.extract_texture : pass.exposure.texture;
        LLVKMirror::Target target;
        VkCommandBuffer cmd = begin({ bloom_texture, pass.exposure.texture, visibility_texture, streak_texture, extract_texture },
                                    pass.width, pass.height, target);
        if (!cmd)
        {
            return false;
        }
        CompositeUniforms uniforms = {};
        if (flare)
        {
            for (U32 c = 0; c < 3; ++c)
            {
                uniforms.flare_color[c] = pass.flare_color[c];
            }
            uniforms.flare_color[3] = pass.flare_intensity;
        }
        uniforms.target_size[0] = (F32)pass.width;
        uniforms.target_size[1] = (F32)pass.height;
        if (bloom)
        {
            uniforms.bloom_texel[0] = 1.f / (F32)pass.bloom_width;
            uniforms.bloom_texel[1] = 1.f / (F32)pass.bloom_height;
            uniforms.bloom_intensity = pass.bloom_intensity;
        }
        uniforms.sun_place[0] = pass.sun_place[0];
        uniforms.sun_place[1] = pass.sun_place[1];
        uniforms.exposure_scale = pass.exposure.scale;
        if (streak)
        {
            uniforms.streak_texel[0] = 1.f / (F32)pass.streak_width;
            uniforms.streak_texel[1] = 1.f / (F32)pass.streak_height;
            uniforms.streak_intensity = pass.streak_intensity;
            uniforms.streak_rays = pass.streak_rays;
        }
        const LLVKFullscreen::Image images[COMPOSITE_IMAGES] = { { bloom_texture }, { pass.exposure.texture }, { visibility_texture },
                                                                 { streak_texture }, { extract_texture } };
        const bool drawn = mComposite.draw(cmd, target, &uniforms, sizeof(uniforms), images, COMPOSITE_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool record_downsample(const LLRHI::GlareDownsample& pass)
    {
        return sRenderer.downsample(pass);
    }

    bool record_upsample(const LLRHI::GlareUpsample& pass)
    {
        return sRenderer.upsample(pass);
    }

    bool record_sun_visibility(const LLRHI::GlareSunVisibility& pass)
    {
        return sRenderer.sunVisibility(pass);
    }

    bool record_streak_extract(const LLRHI::GlareStreakExtract& pass)
    {
        return sRenderer.streakExtract(pass);
    }

    bool record_streak(const LLRHI::GlareStreak& pass)
    {
        return sRenderer.streak(pass);
    }

    bool record_composite(const LLRHI::GlareComposite& pass)
    {
        return sRenderer.composite(pass);
    }
}

void LLVKGlare::install()
{
    if (!LLRHI::sGlare)
    {
        LL_INFOS("VKGlare") << "Glare, lens flare and cross filter off (LLRHI_GLARE=0)" << LL_ENDL;
        return;
    }
    LLRHI::sGlareDownsample = &record_downsample;
    LLRHI::sGlareUpsample = &record_upsample;
    LLRHI::sGlareSunVisibility = &record_sun_visibility;
    LLRHI::sGlareStreakExtract = &record_streak_extract;
    LLRHI::sGlareStreak = &record_streak;
    LLRHI::sGlareComposite = &record_composite;
    LLVKShader::addBakeStep("glare", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
