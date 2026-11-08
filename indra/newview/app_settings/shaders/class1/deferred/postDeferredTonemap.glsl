/**
 * @file postDeferredTonemap.glsl
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;

in vec2 vary_fragcoord;

#ifdef GAMMA_CORRECT
uniform float gamma;
#endif

vec3 linear_to_srgb(vec3 cl);
vec3 toneMap(vec3 color);

vec3 clampHDRRange(vec3 color);

// <plus> glare (plus/glare, research/125 #7): with PlusGlareInTonemap the glare, the sun's
// lens flare and the cross filter's rays are added here, where llvulkan/llvkglare.cpp's
// composite pass would otherwise have added them to diffuseRect in a pass of its own: the same
// sums (keep the two in step), one read and write of the screen fewer. Set by
// plus::glare::bindTonemap; plusGlare 0: nothing is added and nothing below is read.
uniform int plusGlare;
uniform sampler2D plusGlareBloom;       // the first level (all levels added up)
uniform sampler2D plusGlareVisibility;  // 1x1: how much of the sun is seen
uniform sampler2D plusGlareStreak;      // the cross filter's rays added up (a quarter of the screen)
uniform sampler2D plusGlareExtract;     // the light the rays start from
uniform sampler2D exposureMap;          // tonemapUtilF.glsl's
uniform vec4 plusGlareFlareColor;       // rgb: the sun's colour (about 1 in brightness), a: the flare's strength (0: none)
uniform vec4 plusGlareSizes;            // xy: diffuseRect's size, zw: 1 / the bloom's size
uniform vec4 plusGlareSun;              // xy: the sun's place (texture coordinates), z: the bloom's strength (0: none), w: RenderExposure (0: none, "no post": exposureMap not read)
uniform vec4 plusGlareRays;             // xy: 1 / the rays' size, z: their strength (0: none), w: how many rays

const int PLUS_GHOST_COUNT = 6;
const vec3 PLUS_GHOSTS[PLUS_GHOST_COUNT] = vec3[PLUS_GHOST_COUNT](
    vec3( 0.45, 0.025, 0.35),
    vec3(-0.20, 0.060, 0.18),
    vec3(-0.50, 0.110, 0.10),
    vec3(-0.80, 0.035, 0.28),
    vec3(-1.15, 0.160, 0.07),
    vec3(-1.60, 0.075, 0.14));
const vec3 PLUS_GHOST_TINTS[PLUS_GHOST_COUNT] = vec3[PLUS_GHOST_COUNT](
    vec3(1.00, 0.85, 0.60),
    vec3(0.65, 1.00, 0.75),
    vec3(0.70, 0.80, 1.00),
    vec3(1.00, 0.75, 0.55),
    vec3(0.65, 0.75, 1.00),
    vec3(0.90, 0.65, 1.00));
const float PLUS_ABERRATION = 0.035;
const float PLUS_RING_RADIUS = 0.26;
const float PLUS_RING_WIDTH = 0.012;
const float PLUS_RING_BRIGHTNESS = 0.06;
const vec3 PLUS_HALO_TINT = vec3(1.0, 0.72, 0.45);
const float PLUS_HALO_FALLOFF = 0.12;
const float PLUS_HALO_GLOW = 0.25;
const float PLUS_HALO_RADIUS = 0.32;
const float PLUS_HALO_WIDTH = 0.03;
const float PLUS_HALO_RING = 0.03;

vec3 plusGlareTent(vec2 uv)
{
    vec2 t = plusGlareSizes.zw;
    vec3 sum = textureLod(plusGlareBloom, uv, 0.0).rgb * 4.0;
    sum += (textureLod(plusGlareBloom, uv + vec2(-t.x, 0.0), 0.0).rgb + textureLod(plusGlareBloom, uv + vec2(t.x, 0.0), 0.0).rgb +
            textureLod(plusGlareBloom, uv + vec2(0.0, -t.y), 0.0).rgb + textureLod(plusGlareBloom, uv + vec2(0.0, t.y), 0.0).rgb) * 2.0;
    sum += textureLod(plusGlareBloom, uv + vec2(-t.x, -t.y), 0.0).rgb + textureLod(plusGlareBloom, uv + vec2(t.x, -t.y), 0.0).rgb +
           textureLod(plusGlareBloom, uv + vec2(-t.x, t.y), 0.0).rgb + textureLod(plusGlareBloom, uv + vec2(t.x, t.y), 0.0).rgb;
    return sum / 16.0;
}

vec3 plusGlareQuarter(sampler2D image, vec2 uv)
{
    vec2 h = 0.5 * plusGlareRays.xy;
    return 0.25 * (textureLod(image, uv + vec2(-h.x, -h.y), 0.0).rgb + textureLod(image, uv + vec2(h.x, -h.y), 0.0).rgb +
                   textureLod(image, uv + vec2(-h.x, h.y), 0.0).rgb + textureLod(image, uv + vec2(h.x, h.y), 0.0).rgb);
}

float plusGlareDisc(float d, float r)
{
    float inside = 1.0 - smoothstep(0.8 * r, r, d);
    float rim = smoothstep(0.6 * r, 0.95 * r, d);
    return inside * (0.55 + 0.45 * rim);
}

vec3 plusGlareRainbowDisc(float d, float r)
{
    return vec3(plusGlareDisc(d, r * (1.0 + PLUS_ABERRATION)), plusGlareDisc(d, r), plusGlareDisc(d, r * (1.0 - PLUS_ABERRATION)));
}

float plusGlareRing(float d, float r, float width)
{
    float x = (d - r) / width;
    return exp(-x * x);
}

vec3 plusGlareFlare(vec2 uv)
{
    vec2 scale = vec2(plusGlareSizes.x / plusGlareSizes.y, 1.0);
    vec2 p = uv * scale;
    vec2 centre = 0.5 * scale;
    vec2 sun = plusGlareSun.xy * scale;
    vec2 axis = sun - centre;

    vec3 sum = vec3(0.0);
    for (int i = 0; i < PLUS_GHOST_COUNT; ++i)
    {
        float d = distance(p, centre + axis * PLUS_GHOSTS[i].x);
        sum += plusGlareRainbowDisc(d, PLUS_GHOSTS[i].y) * PLUS_GHOST_TINTS[i] * PLUS_GHOSTS[i].z;
    }

    float mirrored = distance(p, centre - axis);
    sum += vec3(plusGlareRing(mirrored, PLUS_RING_RADIUS * (1.0 + 2.0 * PLUS_ABERRATION), PLUS_RING_WIDTH),
                plusGlareRing(mirrored, PLUS_RING_RADIUS, PLUS_RING_WIDTH),
                plusGlareRing(mirrored, PLUS_RING_RADIUS * (1.0 - 2.0 * PLUS_ABERRATION), PLUS_RING_WIDTH)) * PLUS_RING_BRIGHTNESS;

    float from_sun = distance(p, sun);
    sum += PLUS_HALO_TINT * (exp(-from_sun / PLUS_HALO_FALLOFF) * PLUS_HALO_GLOW + plusGlareRing(from_sun, PLUS_HALO_RADIUS, PLUS_HALO_WIDTH) * PLUS_HALO_RING);
    return sum;
}

// What the composite pass adds at uv, in the scene's units.
vec3 plusGlareLight(vec2 uv)
{
    float exposure = plusGlareSun.w > 0.0 ? max(texelFetch(exposureMap, ivec2(0), 0).r * plusGlareSun.w, 1e-4) : 1.0;
    vec3 light = vec3(0.0);
    if (plusGlareSun.z > 0.0)
    {
        light += plusGlareTent(uv) * plusGlareSun.z;
    }
    if (plusGlareFlareColor.a > 0.0)
    {
        float seen = texelFetch(plusGlareVisibility, ivec2(0), 0).r;
        if (seen > 0.001)
        {
            light += plusGlareFlare(uv) * plusGlareFlareColor.rgb * (plusGlareFlareColor.a * seen);
        }
    }
    if (plusGlareRays.z > 0.0)
    {
        // Each ray starts with the light itself; that is already in the picture.
        vec3 rays = plusGlareQuarter(plusGlareStreak, uv) - plusGlareQuarter(plusGlareExtract, uv) * plusGlareRays.w;
        light += max(rays, vec3(0.0)) * plusGlareRays.z;
    }
    return light / exposure;
}
// </plus>

#ifdef GAMMA_CORRECT
vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma)); // s/b inverted already CPU-side

    return c;
}
#endif

void main()
{
    //this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(diffuseRect, vary_fragcoord);

    // <plus> glare (plus/glare): added before the tone mapping, as the composite pass would have
    if (plusGlare != 0)
    {
        diff.rgb += plusGlareLight(vary_fragcoord);
    }
    // </plus>

#ifndef NO_POST
    diff.rgb = toneMap(diff.rgb);
#else
    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));
#endif

#ifdef GAMMA_CORRECT
    diff.rgb = linear_to_srgb(diff.rgb);

#ifdef LEGACY_GAMMA
    diff.rgb = legacyGamma(diff.rgb);
#endif

#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0)); // We should always be 0-1 past this point

    //debugExposure(diff.rgb);
    frag_color = diff;
}

