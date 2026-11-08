/**
 * @file llvkwetlook.cpp
 * @brief Wet look (wet ground and puddles) recorded directly in Vulkan
 *        (research/120 section 6).
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

#include "llvkwetlook.h"

#include "llrhiskinsss.h"
#include "llrhiwetlook.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvkrain.h"
#include "llvktracker.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Shaders

    // The compute pass (LLRHI::WetLookCompute): the G-buffer as the official
    // lighting will read it (softenLightF.glsl, gbufferUtil.glsl), made wet,
    // into the three attachments of the copy target. Pixels that are not
    // lit by the sun (the sky, HDRI, no atmospherics) are copied as they are.
    // In three parts: this head, the overhead map's lookup (LLVKRain::OVERHEAD_GLSL), the body.
    const char* const COMPUTE_HEAD = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform WetLookCompute
{
    mat4 inverse_projection;    // clip -> eye
    mat4 eye_to_world;          // eye -> world (agent coordinates plus the wrapped offset)
    mat4 to_map;                // eye -> the overhead map's clip space (research/120 section 8)
    vec4 up;                    // xyz: world up in eye space
    vec4 wet;                   // x: film, y: darkening share, z: puddle coverage, w: ripple tilt
    vec4 map_info;              // x: 1 with a map, y: cover bias (clip z), w: edge fade (clip)
    vec4 map_info2;             // xy: the map's depth to ndc, z: normal offset (m)
    vec4 map_shift;             // xy: clip per metre of height, towards where the rain comes from
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    ivec2 size;                 // the target's size (the G-buffer's too)
    float time;                 // s, wrapped
    float ripple_rate;          // rings per ripple cell per s (times the time period, whole)
    float soaked;               // 0..1: how much of the film is falling rain's
    float water_height;         // world z of the water: nothing below it is made wet
    vec4 snow;                  // x: snow cover
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D diffuse_map;
layout(set = 0, binding = 3) uniform sampler2D specular_map;
layout(set = 0, binding = 4) uniform sampler2D normal_map;
layout(set = 0, binding = 5) uniform sampler2D overhead_map;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;
layout(location = 2) out vec4 out_normal;
)GLSL";

    const char* const COMPUTE_BODY = R"GLSL(
const float PI = 3.14159265;

// The G-buffer flags (llshadermgr.cpp: GBUFFER_FLAG_*, GET_GBUFFER_FLAG).
const float FLAG_HAS_ATMOS = 0.34;
const float FLAG_HAS_PBR = 0.67;
const float FLAG_TOLERANCE = 0.1;

// The water film by how a surface faces: all of it facing up from UP_FULL
// (N.up), WALL_SHARE on a vertical wall, none on faces turned down by
// UNDERSIDE_FADE or more.
const float UP_FULL = 0.7;
const float WALL_SHARE = 0.35;
const float UNDERSIDE_FADE = 0.5;
// Falling rain (u.soaked) wets walls more: they take this share instead.
const float RAIN_WALL_SHARE = 0.75;

// Darker and more saturated where water soaks in (Lagarde, "Water drop 3b -
// Physically based wet surfaces", 2013): porous (rough) surfaces most,
// sealed ones (glossy, metal) only by SEALED_SHARE.
const float WET_DARKEN = 0.45;          // the base colour of a soaked porous surface, times
const float WET_SATURATION = 0.35;      // its saturation raised by this share
const float POROUS_LOW = 0.15;          // PBR roughness from which a surface soaks up water
const float POROUS_HIGH = 0.6;          // ... up to which it soaks up more
const float SEALED_SHARE = 0.15;

// Glossier: a soaked surface's PBR roughness goes down to FILM_ROUGHNESS,
// still water's to PUDDLE_ROUGHNESS. Legacy materials (specular colour and
// gloss as stored, sRGB; environment intensity) rise to at least these.
const float FILM_ROUGHNESS = 0.3;
const float PUDDLE_ROUGHNESS = 0.03;
const float LEGACY_FILM_SPECULAR = 0.25;
const float LEGACY_FILM_GLOSS = 0.6;
const float LEGACY_FILM_ENV = 0.08;
const float LEGACY_PUDDLE_SPECULAR = 0.6;
const float LEGACY_PUDDLE_GLOSS = 0.95;
const float LEGACY_PUDDLE_ENV = 0.45;
// Under falling rain (u.soaked) a surface facing up carries a thin sheet of
// water: its film goes glossier still, between the after-rain film above
// and a puddle. Walls keep the after-rain film's values.
const float RAIN_FILM_ROUGHNESS = 0.15;
const float LEGACY_RAIN_FILM_SPECULAR = 0.3;
const float LEGACY_RAIN_FILM_GLOSS = 0.8;
const float LEGACY_RAIN_FILM_ENV = 0.2;

// Puddles: where two octaves of value noise in world xy rise above a level
// (lower with more coverage), on ground flat enough to hold water. Every
// cell size divides WORLD_PERIOD (LLRHI::WET_LOOK_WORLD_PERIOD).
const float WORLD_PERIOD = 4096.0;
const float PUDDLE_CELL_LARGE = 8.0;    // m
const float PUDDLE_CELL_SMALL = 2.0;    // m
const float PUDDLE_LARGE_SHARE = 0.65;
const float PUDDLE_LEVEL_FEW = 0.78;    // the noise level above which there is water, at coverage 0+
const float PUDDLE_LEVEL_MANY = 0.42;   // ... at coverage 1 (about half the flat ground)
const float PUDDLE_EDGE = 0.035;        // noise units from the dry edge to water
const float DAMP_RING = 0.12;           // noise units below the edge where the ground is soaked
const float DRY_PATCH_SHARE = 0.75;     // the film on flat ground far from any puddle, times
// Low coverage (u.wet.z) makes the few puddles shallow too, so they come and
// go smoothly as the rain fills them and they dry, not all at once.
const float PUDDLE_FADE_IN = 0.1;
// Below the water nothing is wet (the bed, what stands in it); the wetness
// fades in over this much (m) below the waterline.
const float WATERLINE_FADE = 0.15;
const float LEVEL_LOW = 0.94;           // the geometry's N.up from which water gathers
const float LEVEL_HIGH = 0.985;

// Rain ripples on the puddles: in each RIPPLE_CELL square, a ring grows from
// a random point u.ripple_rate times a second (0.8 without rain, more while
// it rains; times LLRHI::WET_LOOK_TIME_PERIOD a whole number), fading as it
// grows; two layers of cells, offset.
const float RIPPLE_CELL = 0.5;          // m (divides WORLD_PERIOD)
const float RIPPLE_MAX_RADIUS = 0.24;   // cell units: a ring stays inside its cell
const float RIPPLE_WAVES = 110.0;       // radians per cell unit: a ring some 3 cm wide
const uint RIPPLE_LAYERS = 2u;

// globalF.glsl's decodeNormal and encodeNormal.
vec3 decodeNormal(vec2 encoded)
{
    vec2 fenc = encoded * 4.0 - 2.0;
    float f = dot(fenc, fenc);
    float g = sqrt(max(1.0 - f / 4.0, 0.0));
    return vec3(fenc * g, 1.0 - f / 2.0);
}

vec2 encodeNormal(vec3 n)
{
    float f = sqrt(max(8.0 * n.z + 8.0, 1e-6));
    return n.xy / f + 0.5;
}

vec3 srgbToLinear(vec3 c)
{
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

vec3 linearToSrgb(vec3 c)
{
    c = max(c, vec3(0.0));
    vec3 low = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(c, vec3(0.0031308)));
}

vec3 eyePosition(ivec2 pixel)
{
    ivec2 p = clamp(pixel, ivec2(0), u.size - 1);
    float depth = texelFetch(scene_depth, p, 0).r;
    vec2 ndc_xy = (vec2(p) + 0.5) / vec2(u.size) * 2.0 - 1.0;
    vec4 eye = u.inverse_projection * vec4(ndc_xy, depth * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
    return eye.xyz / eye.w;
}

// The geometry's normal from the depth (not the normal map: water fills the
// bumps), from the neighbours on the nearer side (not across an edge),
// facing the camera.
vec3 geometryNormal(ivec2 pixel, vec3 pos)
{
    vec3 left = eyePosition(pixel - ivec2(1, 0));
    vec3 right = eyePosition(pixel + ivec2(1, 0));
    vec3 below = eyePosition(pixel - ivec2(0, 1));
    vec3 above = eyePosition(pixel + ivec2(0, 1));
    bool use_right = pixel.x == 0 || (pixel.x + 1 < u.size.x && abs(right.z - pos.z) < abs(pos.z - left.z));
    bool use_above = pixel.y == 0 || (pixel.y + 1 < u.size.y && abs(above.z - pos.z) < abs(pos.z - below.z));
    vec3 dx = use_right ? right - pos : pos - left;
    vec3 dy = use_above ? above - pos : pos - below;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len <= 0.0)
    {
        return vec3(0.0, 0.0, 1.0);
    }
    n /= len;
    return dot(n, pos) > 0.0 ? -n : n;
}

uint hash(uvec2 q, uint seed)
{
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float random(uvec2 q, uint seed)
{
    return float(hash(q, seed) >> 8) * (1.0 / 16777216.0);
}

// A lattice cell's coordinates wrapped at period cells.
uvec2 wrapCell(vec2 cell, float period)
{
    return uvec2(mod(cell, period));
}

float valueNoise(vec2 p, float cell_size, uint seed)
{
    float period = WORLD_PERIOD / cell_size;
    vec2 q = p / cell_size;
    vec2 i = floor(q);
    vec2 f = q - i;
    vec2 s = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = random(wrapCell(i, period), seed);
    float b = random(wrapCell(i + vec2(1.0, 0.0), period), seed);
    float c = random(wrapCell(i + vec2(0.0, 1.0), period), seed);
    float d = random(wrapCell(i + vec2(1.0, 1.0), period), seed);
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

// At a point of flat ground: x how much of it is under water, y how close
// it is to a puddle (1 at the edge, 0 a damp ring away).
vec2 puddleAt(vec2 world)
{
    float n = PUDDLE_LARGE_SHARE * valueNoise(world, PUDDLE_CELL_LARGE, 1u) +
              (1.0 - PUDDLE_LARGE_SHARE) * valueNoise(world, PUDDLE_CELL_SMALL, 2u);
    float level = mix(PUDDLE_LEVEL_FEW, PUDDLE_LEVEL_MANY, u.wet.z);
    return vec2(smoothstep(level, level + PUDDLE_EDGE, n), smoothstep(level - DAMP_RING, level, n));
}

// The rain ripples' slope (d height / d world xy, about -1..1) at world.
vec2 rippleSlope(vec2 world)
{
    vec2 slope = vec2(0.0);
    float period = WORLD_PERIOD / RIPPLE_CELL;
    for (uint layer = 0u; layer < RIPPLE_LAYERS; ++layer)
    {
        vec2 q = world / RIPPLE_CELL + vec2(0.37, 0.61) * float(layer);
        vec2 cell = floor(q);
        uvec2 id = wrapCell(cell, period);
        vec2 centre = 0.25 + 0.5 * vec2(random(id, 10u + layer), random(id, 20u + layer));
        float life = fract(u.time * u.ripple_rate + random(id, 30u + layer));
        vec2 d = q - cell - centre;
        float dist = length(d);
        float x = (dist - life * RIPPLE_MAX_RADIUS) * RIPPLE_WAVES;
        if (dist > 0.0 && abs(x) < PI)
        {
            // height = sin(x) * (0.5 + 0.5 cos(x)): one crest and trough, no edge.
            float dheight = cos(x) * (0.5 + 0.5 * cos(x)) - 0.5 * sin(x) * sin(x);
            float fade = (1.0 - life) * (1.0 - life);
            slope += (d / dist) * dheight * fade;
        }
    }
    return slope;
}

// Slanted rain reaches a surface from upwind: OVERHEAD_GLSL's EAVE_HEIGHT,
// MAX_EAVE_REACH and COVERED_REACH_SHARE (shared with the rain's streaks and
// splashes, fix-round67 2).

// The wet edge under a roof's edge is softened over about twice this (m):
// the map is looked up at the point and this far to its four sides (world x
// and y), the point weighing ROOF_EDGE_CENTRE.
const float ROOF_EDGE_SOFTEN = 0.25;
const float ROOF_EDGE_CENTRE = 0.4;

// overheadOpen at c (the map's clip xy), z, softened over its four sides.
float softOpen(vec2 c, float z, vec2 step_x, vec2 step_y)
{
    float sides = overheadOpen(vec3(c + step_x, z), u.map_info, u.map_info2.xy) +
                  overheadOpen(vec3(c - step_x, z), u.map_info, u.map_info2.xy) +
                  overheadOpen(vec3(c + step_y, z), u.map_info, u.map_info2.xy) +
                  overheadOpen(vec3(c - step_y, z), u.map_info, u.map_info2.xy);
    return ROOF_EDGE_CENTRE * overheadOpen(vec3(c, z), u.map_info, u.map_info2.xy) + (1.0 - ROOF_EDGE_CENTRE) * 0.25 * sides;
}

// How much of the rain reaches a point (eye space, its normal): the overhead
// map (research/120 section 8) says 0 under a roof, 1 in the open (and
// everywhere without a map). Looked up a little out along the normal, so a
// wall beside a roof's edge takes the rain and one under the eaves does not.
// With wind, also where the rain comes from: the lee of a roof's edge stays
// dry, the windward eaves get a little wet.
float skyExposure(vec3 eye_pos, vec3 n)
{
    if (u.map_info.x < 0.5)
    {
        return 1.0;     // no map: open everywhere
    }
    vec4 m = u.to_map * vec4(eye_pos + n * u.map_info2.z, 1.0);
    // World x and y in eye space (the rows of eye -> world's rotation), into the map.
    mat3 world_to_eye = transpose(mat3(u.eye_to_world));
    vec2 step_x = (u.to_map * vec4(world_to_eye[0] * ROOF_EDGE_SOFTEN, 0.0)).xy;
    vec2 step_y = (u.to_map * vec4(world_to_eye[1] * ROOF_EDGE_SOFTEN, 0.0)).xy;
    float here = softOpen(m.xy, m.z, step_x, step_y);
    // The upwind shift, at most MAX_EAVE_REACH (step_x is ROOF_EDGE_SOFTEN m long in the map).
    float clip_per_metre = length(step_x) / ROOF_EDGE_SOFTEN;
    vec2 shift = upwindShift(u.map_shift.xy, clip_per_metre);
    if (length(shift) <= clip_per_metre * MAX_EAVE_REACH * 0.05)
    {
        return here;    // calm: straight down
    }
    float upwind = softOpen(m.xy + shift, m.z, step_x, step_y);
    return windOpen(here, upwind);
}

)GLSL"
    // (Two literals: MSVC takes at most 16 KB in one.)
    R"GLSL(
// Snow cover (u.snow.x, 0..1: the layer laid by falling snow, or the ground
// snow the photo tools ask for): it settles on what faces up first and
// spreads to steeper faces as it gets deeper; walls take only patches of it,
// undersides none. A world-space noise breaks it into soft, irregular drifts:
// thin cover lies only where the noise is high (patches), deep cover
// everywhere (the noise only shades it).
const float SNOW_FLAT_LOW = 0.35;       // N.up from which snow settles ...
const float SNOW_FLAT_HIGH = 0.95;      // ... and where it settles fully
const float SNOW_WALL_SHARE = 0.25;     // the share of the cover a vertical wall takes
// The noise: three octaves of gradient noise (fix-round81: value noise on a
// 4 m square lattice, with a 0.5 m one on the same lines, showed the lattice
// as soft square blocks). Each octave's lattice is turned by the whole-number
// matrix (x, -y; y, x) of its SNOW_TURN_*, so no two share their lines and
// none runs along the world axes, and it stays seamless where the world
// wraps (every SNOW_CELL_* divides WORLD_PERIOD). The size of a drift is the
// cell divided by the turn's length: about 3.6 m, 0.9 m and 0.2 m.
const float SNOW_CELL_LARGE = 8.0;      // m: the drifts across the ground
const float SNOW_CELL_MID = 2.0;        // m: their ragged outline
const float SNOW_CELL_SMALL = 1.0;      // m: the small bumps underfoot
const vec2 SNOW_TURN_LARGE = vec2(2.0, 1.0);    // 26.6 degrees, length 2.24
const vec2 SNOW_TURN_MID = vec2(1.0, 2.0);      // 63.4 degrees, length 2.24
const vec2 SNOW_TURN_SMALL = vec2(3.0, 4.0);    // 53.1 degrees, length 5
const float SNOW_LARGE_SHARE = 0.55;    // each octave's weight (the small one takes the rest)
const float SNOW_MID_SHARE = 0.3;
const float SNOW_NOISE_CONTRAST = 2.0;  // the summed noise (about -0.3..0.3) to 0..1 around 0.5
// The lattice moved off the world's origin (lattice units; any fraction).
const vec2 SNOW_LATTICE_OFFSET = vec2(0.37, 0.71);
// How the noise cuts the cover into patches: snow where
// cover * face * SNOW_THICKNESS > (1 - noise) * SNOW_RAGGED, so a thin cover
// shows only on the noise's highs and a deep one everywhere; it fades in over
// SNOW_EDGE of that difference (wider: softer, hazier edges).
const float SNOW_THICKNESS = 1.6;
const float SNOW_RAGGED = 0.8;
const float SNOW_EDGE = 0.35;
// Fresh snow: bright, a little blue, matte. PBR: roughness SNOW_ROUGHNESS, no
// metal. Legacy: specular colour SNOW_LEGACY_SPECULAR, gloss SNOW_LEGACY_GLOSS,
// environment intensity SNOW_LEGACY_ENV. The normal leans up by this share.
const vec3 SNOW_ALBEDO = vec3(0.82, 0.86, 0.92);    // linear
const float SNOW_ROUGHNESS = 0.8;
const float SNOW_LEGACY_SPECULAR = 0.06;
const float SNOW_LEGACY_GLOSS = 0.3;
const float SNOW_LEGACY_ENV = 0.04;
const float SNOW_NORMAL_LEAN = 0.5;

// A lattice point's gradient, each component -1..1.
vec2 gradientAt(uvec2 q, uint seed)
{
    uint h = hash(q, seed);
    return vec2(float(h & 0xffffu), float(h >> 16)) * (2.0 / 65535.0) - 1.0;
}

// Gradient noise (Perlin) at q (lattice units), the lattice wrapped at
// period cells: mostly within -0.4..0.4 (at most about -1..1), 0 at the
// lattice points, smooth (quintic) between them, with no flat plateau at a
// lattice point to show its grid.
float gradientNoise(vec2 q, float period, uint seed)
{
    vec2 i = floor(q);
    vec2 f = q - i;
    vec2 s = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(gradientAt(wrapCell(i, period), seed), f);
    float b = dot(gradientAt(wrapCell(i + vec2(1.0, 0.0), period), seed), f - vec2(1.0, 0.0));
    float c = dot(gradientAt(wrapCell(i + vec2(0.0, 1.0), period), seed), f - vec2(0.0, 1.0));
    float d = dot(gradientAt(wrapCell(i + vec2(1.0, 1.0), period), seed), f - vec2(1.0, 1.0));
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

// One octave of the snow's noise at world xy: the lattice of cell m turned by
// (turn.x, -turn.y; turn.y, turn.x). A step of WORLD_PERIOD in world x or y
// moves q by whole multiples of the period, so the wrap leaves no seam.
float snowOctave(vec2 world, float cell, vec2 turn, uint seed)
{
    vec2 turned = vec2(turn.x * world.x - turn.y * world.y, turn.y * world.x + turn.x * world.y);
    return gradientNoise(turned / cell + SNOW_LATTICE_OFFSET, WORLD_PERIOD / cell, seed);
}

// The snow's noise at world xy, 0..1 (mostly 0.1..0.9, around 0.5).
float snowNoise(vec2 world)
{
    float sum = SNOW_LARGE_SHARE * snowOctave(world, SNOW_CELL_LARGE, SNOW_TURN_LARGE, 3u) +
                SNOW_MID_SHARE * snowOctave(world, SNOW_CELL_MID, SNOW_TURN_MID, 4u) +
                (1.0 - SNOW_LARGE_SHARE - SNOW_MID_SHARE) * snowOctave(world, SNOW_CELL_SMALL, SNOW_TURN_SMALL, 5u);
    return clamp(0.5 + SNOW_NOISE_CONTRAST * sum, 0.0, 1.0);
}

// How much of a surface (world position, its normal in eye space, how much
// sky it sees) is under snow, 0..1.
float snowAmount(vec2 world, vec3 n, vec3 up, float exposure)
{
    float facing = dot(n, up);
    float flat_face = smoothstep(SNOW_FLAT_LOW, SNOW_FLAT_HIGH, facing);
    float face = mix(SNOW_WALL_SHARE, 1.0, flat_face) * smoothstep(-0.2, 0.1, facing);
    float noise = snowNoise(world);
    float layer = u.snow.x * face * SNOW_THICKNESS - (1.0 - noise) * SNOW_RAGGED * smoothstep(0.0, 0.2, u.snow.x);
    return smoothstep(0.0, SNOW_EDGE, layer) * exposure;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 base = texelFetch(diffuse_map, pixel, 0);
    vec4 spec = texelFetch(specular_map, pixel, 0);
    vec4 normal_info = texelFetch(normal_map, pixel, 0);
    out_diffuse = base;
    out_specular = spec;
    out_normal = normal_info;

    bool pbr = abs(normal_info.a - FLAG_HAS_PBR) < FLAG_TOLERANCE;
    bool legacy = abs(normal_info.a - FLAG_HAS_ATMOS) < FLAG_TOLERANCE;
    if (!pbr && !legacy)
    {
        return;     // the sky, HDRI, no atmospherics: as it is
    }

    vec3 pos = eyePosition(pixel);
    vec3 n = normalize(decodeNormal(normal_info.xy));
    vec3 up = u.up.xyz;
    // Under the water (the bed, what stands in it) nothing is wetter for the
    // rain; a little above the waterline it is.
    float world_z = (u.eye_to_world * vec4(pos, 1.0)).z;
    float above_water = clamp((world_z - u.water_height) / WATERLINE_FADE + 1.0, 0.0, 1.0);
    float exposure = above_water > 0.0 ? skyExposure(pos, n) * above_water : 0.0;

    // The water film by how the surface faces.
    float facing = dot(n, up);
    float standing = smoothstep(0.0, UP_FULL, facing);     // 1 facing up, 0 on a wall or below
    float wall_share = mix(WALL_SHARE, RAIN_WALL_SHARE, u.soaked);
    float share = facing >= 0.0 ? mix(wall_share, 1.0, standing)
                                : wall_share * clamp(1.0 + facing / UNDERSIDE_FADE, 0.0, 1.0);
    float film = u.wet.x * share * exposure;
    // Falling rain's sheet of water on what faces up.
    float sheet = u.soaked * standing;

    // Puddles on level ground; around them the ground is soaked, farther
    // away a little less.
    float puddle = 0.0;
    vec2 world = vec2(0.0);
    if (u.wet.z > 0.0 && exposure > 0.0)
    {
        float level_ground = smoothstep(LEVEL_LOW, LEVEL_HIGH, dot(geometryNormal(pixel, pos), up));
        if (level_ground > 0.0)
        {
            world = (u.eye_to_world * vec4(pos, 1.0)).xy;
            vec2 water = puddleAt(world);
            puddle = water.x * level_ground * exposure * smoothstep(0.0, PUDDLE_FADE_IN, u.wet.z);
            // Falling rain leaves no dry patches.
            float patch_share = mix(mix(DRY_PATCH_SHARE, 1.0, water.y), 1.0, u.soaked);
            film *= mix(1.0, patch_share, level_ground);
        }
    }

    // The surface: its base colour (linear), how much of it the sun lights
    // (legacy fullbright is left as it is) and how porous it is.
    vec3 albedo;
    float porosity;
    if (pbr)
    {
        albedo = base.rgb;
        porosity = smoothstep(POROUS_LOW, POROUS_HIGH, spec.g) * (1.0 - spec.b);
    }
    else
    {
        albedo = srgbToLinear(base.rgb);
        porosity = 1.0 - spec.a;
        film *= 1.0 - base.a;
        puddle *= 1.0 - base.a;
    }
    float snow = 0.0;
    if (u.snow.x > 0.0 && exposure > 0.0)
    {
        if (world == vec2(0.0))
        {
            world = (u.eye_to_world * vec4(pos, 1.0)).xy;
        }
        snow = snowAmount(world, n, up, exposure) * (pbr ? 1.0 : 1.0 - base.a);
    }
    if (film <= 0.0 && puddle <= 0.0 && snow <= 0.0)
    {
        return;
    }
    // Snow is laid over the wet look: under it the surface is not wet.
    film *= 1.0 - snow;
    puddle *= 1.0 - snow;

    // Darker and more saturated by the water soaked in (under a puddle, all
    // the surface can take).
    float soak = max(film, puddle) * mix(SEALED_SHARE, 1.0, porosity) * u.wet.y;
    vec3 wet_albedo = albedo * mix(1.0, WET_DARKEN, soak);
    float luma = dot(wet_albedo, vec3(0.2126, 0.7152, 0.0722));
    wet_albedo = max(vec3(luma) + (wet_albedo - vec3(luma)) * (1.0 + WET_SATURATION * soak), vec3(0.0));

    if (pbr)
    {
        out_diffuse.rgb = wet_albedo;
        float film_roughness = mix(FILM_ROUGHNESS, RAIN_FILM_ROUGHNESS, sheet);
        float roughness = mix(spec.g, min(spec.g, film_roughness), film);
        out_specular.g = mix(roughness, PUDDLE_ROUGHNESS, puddle);
        out_specular.b = spec.b * (1.0 - puddle);   // under water the surface is the water's
    }
    else
    {
        out_diffuse.rgb = linearToSrgb(wet_albedo);
        float film_specular = mix(LEGACY_FILM_SPECULAR, LEGACY_RAIN_FILM_SPECULAR, sheet);
        float film_gloss = mix(LEGACY_FILM_GLOSS, LEGACY_RAIN_FILM_GLOSS, sheet);
        float film_env = mix(LEGACY_FILM_ENV, LEGACY_RAIN_FILM_ENV, sheet);
        vec3 specular = mix(spec.rgb, max(spec.rgb, vec3(film_specular)), film);
        float gloss = mix(spec.a, max(spec.a, film_gloss), film);
        float env = mix(normal_info.b, max(normal_info.b, film_env), film);
        out_specular = vec4(mix(specular, max(specular, vec3(LEGACY_PUDDLE_SPECULAR)), puddle),
                            mix(gloss, max(gloss, LEGACY_PUDDLE_GLOSS), puddle));
        out_normal.b = mix(env, max(env, LEGACY_PUDDLE_ENV), puddle);
    }

    // Still water is level whatever the surface's own bumps; the rain
    // ripples tilt it.
    if (puddle > 0.0)
    {
        vec2 slope = rippleSlope(world) * u.wet.w;
        vec3 water = normalize(transpose(mat3(u.eye_to_world)) * normalize(vec3(-slope, 1.0)));
        vec3 surface = mix(n, water, puddle);
        float len = length(surface);
        out_normal.xy = encodeNormal(len > 1e-4 ? surface / len : water);
    }

    // Snow over it all: white, matte, the normal leaning up a little (the
    // layer fills the small bumps).
    if (snow > 0.0)
    {
        if (pbr)
        {
            out_diffuse.rgb = mix(out_diffuse.rgb, SNOW_ALBEDO, snow);
            out_specular.g = mix(out_specular.g, SNOW_ROUGHNESS, snow);
            out_specular.b = out_specular.b * (1.0 - snow);
        }
        else
        {
            out_diffuse.rgb = mix(out_diffuse.rgb, linearToSrgb(SNOW_ALBEDO), snow);
            out_specular = vec4(mix(out_specular.rgb, vec3(SNOW_LEGACY_SPECULAR), snow), mix(out_specular.a, SNOW_LEGACY_GLOSS, snow));
            out_normal.b = mix(out_normal.b, SNOW_LEGACY_ENV, snow);
        }
        vec3 leaned = mix(n, up, SNOW_NORMAL_LEAN * snow);
        float lean_length = length(leaned);
        out_normal.xy = encodeNormal(lean_length > 1e-4 ? leaned / lean_length : n);
    }
}
)GLSL";

    // The write-back (LLRHI::WetLookWriteBack): the copy target's three
    // attachments into the G-buffer's first three, except on the sky. With
    // the G-buffer's emissive attachment (RenderEnableEmissiveBuffer), a
    // fourth output that the pass's write mask keeps out of it.
    const char* const WRITE_BACK_BODY = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D wet_diffuse;
layout(set = 0, binding = 2) uniform sampler2D wet_specular;
layout(set = 0, binding = 3) uniform sampler2D wet_normal;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;
layout(location = 2) out vec4 out_normal;
#ifdef EMISSIVE_ATTACHMENT
layout(location = 3) out vec4 out_emissive;
#endif

const float FLAG_HAS_ATMOS = 0.34;
const float FLAG_HAS_PBR = 0.67;
const float FLAG_TOLERANCE = 0.1;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 normal_info = texelFetch(wet_normal, pixel, 0);
    if (abs(normal_info.a - FLAG_HAS_PBR) >= FLAG_TOLERANCE && abs(normal_info.a - FLAG_HAS_ATMOS) >= FLAG_TOLERANCE)
    {
        discard;    // the sky: the compute pass left it as it is
    }
    out_diffuse = texelFetch(wet_diffuse, pixel, 0);
    out_specular = texelFetch(wet_specular, pixel, 0);
    out_normal = normal_info;
#ifdef EMISSIVE_ATTACHMENT
    out_emissive = vec4(0.0);
#endif
}
)GLSL";

    // ------------------------------------------------------------------
    // Uniform blocks, as the shaders' std140 blocks lay them out.

    struct ComputeUniforms
    {
        F32 inverse_projection[16];
        F32 eye_to_world[16];
        F32 to_map[16];
        F32 up[4];
        F32 wet[4];
        F32 map_info[4];
        F32 map_info2[4];
        F32 map_shift[4];
        F32 depth_to_ndc[2];
        S32 size[2];
        F32 time;
        F32 ripple_rate;
        F32 soaked;
        F32 water_height;
        F32 snow[4];
    };
    static_assert(sizeof(ComputeUniforms) == 320, "ComputeUniforms must match the std140 block WetLookCompute");

    // The write-back reads no uniforms; the pass still binds a block.
    struct WriteBackUniforms
    {
        F32 unused[4];
    };

    static_assert(LLRHI::WET_LOOK_WORLD_PERIOD == 4096.f, "COMPUTE_BODY's WORLD_PERIOD");
    static_assert(LLRHI::WET_LOOK_TIME_PERIOD == 1000.f, "WET_LOOK_RIPPLE_RATE (0.8) times the time period must be whole");

    constexpr U32 COMPUTE_IMAGES = 5;       // depth, diffuse, specular, normal, the overhead map
    constexpr U32 WRITE_BACK_IMAGES = 3;    // the copy target's three attachments
    constexpr U32 COPY_COLORS = 3;          // the copy target's attachments (the G-buffer's first three)
    constexpr U32 PIXEL_CHOICES = 4;        // LLRHI::WetLookPixels

    // The stencil test of each LLRHI::WetLookPixels.
    constexpr LLVKFullscreen::StencilTest STENCIL_TESTS[PIXEL_CHOICES] =
    {
        {},                                             // everywhere
        { true, 0, LLRHI::STENCIL_AVATAR },             // not on avatars
        { true, LLRHI::STENCIL_AVATAR, LLRHI::STENCIL_AVATAR },   // on avatars
        { true, LLRHI::STENCIL_AVATAR_SKIN, LLRHI::STENCIL_AVATAR_SKIN },     // on avatars' bare skin
    };
    static_assert(LLRHI::WET_LOOK_AVATAR_SKIN == PIXEL_CHOICES - 1, "a stencil test and a compute pass for each LLRHI::WetLookPixels");

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool compute(const LLRHI::WetLookCompute& pass);
        bool writeBack(const LLRHI::WetLookWriteBack& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        LLVKFullscreen::Pass mCompute[PIXEL_CHOICES];
        // [0]: the G-buffer without the emissive attachment, [1]: with it;
        // everywhere and not on avatars.
        LLVKFullscreen::Pass mWriteBack[2][2];
        bool mLoggedCompute = false;
        bool mLoggedWriteBack = false;
        // The textures each pass samples: kept so that no frame allocates them anew. (fix-round67 6)
        std::vector<LLVKTracker::GLName> mComputeSampled;
        std::vector<LLVKTracker::GLName> mWriteBackSampled;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            const VkColorComponentFlags write_all[LLVKFullscreen::MAX_COLORS] = { LLVKFullscreen::WRITE_RGBA, LLVKFullscreen::WRITE_RGBA,
                                                                                LLVKFullscreen::WRITE_RGBA, 0 };
            const std::string write_back = std::string("#version 450\n") + WRITE_BACK_BODY;
            const std::string write_back_emissive = std::string("#version 450\n#define EMISSIVE_ATTACHMENT\n") + WRITE_BACK_BODY;
            const std::string compute = std::string(COMPUTE_HEAD) + LLVKRain::OVERHEAD_GLSL + COMPUTE_BODY;
            const char* const compute_fragment = compute.c_str();
            mReady = mCompute[LLRHI::WET_LOOK_EVERYWHERE].initColors("wet look", compute_fragment, COMPUTE_IMAGES, COPY_COLORS, write_all,
                                                                     STENCIL_TESTS[LLRHI::WET_LOOK_EVERYWHERE]) &&
                     mCompute[LLRHI::WET_LOOK_NOT_AVATARS].initColors("wet look not avatars", compute_fragment, COMPUTE_IMAGES, COPY_COLORS,
                                                                      write_all, STENCIL_TESTS[LLRHI::WET_LOOK_NOT_AVATARS]) &&
                     mCompute[LLRHI::WET_LOOK_AVATARS].initColors("wet look avatars", compute_fragment, COMPUTE_IMAGES, COPY_COLORS, write_all,
                                                                  STENCIL_TESTS[LLRHI::WET_LOOK_AVATARS]) &&
                     mCompute[LLRHI::WET_LOOK_AVATAR_SKIN].initColors("wet look avatar skin", compute_fragment, COMPUTE_IMAGES, COPY_COLORS,
                                                                      write_all, STENCIL_TESTS[LLRHI::WET_LOOK_AVATAR_SKIN]) &&
                     mWriteBack[0][0].initColors("wet look write-back", write_back.c_str(), WRITE_BACK_IMAGES, COPY_COLORS, write_all,
                                                 STENCIL_TESTS[LLRHI::WET_LOOK_EVERYWHERE]) &&
                     mWriteBack[0][1].initColors("wet look write-back not avatars", write_back.c_str(), WRITE_BACK_IMAGES, COPY_COLORS,
                                                 write_all, STENCIL_TESTS[LLRHI::WET_LOOK_NOT_AVATARS]) &&
                     mWriteBack[1][0].initColors("wet look write-back emissive", write_back_emissive.c_str(), WRITE_BACK_IMAGES,
                                                 COPY_COLORS + 1, write_all, STENCIL_TESTS[LLRHI::WET_LOOK_EVERYWHERE]) &&
                     mWriteBack[1][1].initColors("wet look write-back emissive not avatars", write_back_emissive.c_str(),
                                                 WRITE_BACK_IMAGES, COPY_COLORS + 1, write_all, STENCIL_TESTS[LLRHI::WET_LOOK_NOT_AVATARS]);
            if (!mReady)
            {
                LL_WARNS("VKWetLook") << "The wet look passes could not be set up; they stay off" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::compute(const LLRHI::WetLookCompute& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.depth_texture || !pass.diffuse_texture || !pass.specular_texture || !pass.normal_texture ||
            pass.pixels >= PIXEL_CHOICES)
        {
            return false;
        }
        // The overhead map only when there is one (a missing one is bound as a 1x1 white stand-in).
        const LLRHI::RainOverhead& map = pass.overhead;
        mComputeSampled.assign({ pass.depth_texture, pass.diffuse_texture, pass.specular_texture, pass.normal_texture });
        if (map.depth_texture)
        {
            mComputeSampled.push_back(map.depth_texture);
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mComputeSampled, target);
        if (!cmd)
        {
            return false;
        }

        ComputeUniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.eye_to_world, pass.eye_to_world, sizeof(uniforms.eye_to_world));
        memcpy(uniforms.to_map, map.to_map, sizeof(uniforms.to_map));
        memcpy(uniforms.up, pass.up, sizeof(pass.up));
        uniforms.wet[0] = llclamp(pass.wetness, 0.f, 1.f);
        uniforms.wet[1] = llclamp(pass.darkening, 0.f, 1.f);
        uniforms.wet[2] = llclamp(pass.puddles, 0.f, 1.f);
        uniforms.wet[3] = llmax(pass.ripples, 0.f);
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.map_info[0] = map.depth_texture ? 1.f : 0.f;
        uniforms.map_info[1] = llmax(map.cover_bias, 0.f);
        uniforms.map_info[3] = llmax(map.edge, 1e-4f);
        uniforms.map_info2[0] = uniforms.depth_to_ndc[0];
        uniforms.map_info2[1] = uniforms.depth_to_ndc[1];
        uniforms.map_info2[2] = llmax(map.normal_offset, 0.f);
        uniforms.map_shift[0] = map.rain_shift[0];
        uniforms.map_shift[1] = map.rain_shift[1];
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;
        uniforms.time = pass.time;
        uniforms.ripple_rate = pass.ripple_rate > 0.f ? pass.ripple_rate : LLRHI::WET_LOOK_RIPPLE_RATE;
        uniforms.soaked = llclamp(pass.soaked, 0.f, 1.f);
        uniforms.water_height = std::isfinite(pass.water_height) ? pass.water_height : LLRHI::WET_LOOK_NO_WATER;
        uniforms.snow[0] = llclamp(pass.snow, 0.f, 1.f);

        // The scene depth is the target's own (shared): attached read-only and sampled in that layout.
        LLVKFullscreen::Image images[COMPUTE_IMAGES];
        images[0] = { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1].texture = pass.diffuse_texture;
        images[2].texture = pass.specular_texture;
        images[3].texture = pass.normal_texture;
        images[4].texture = map.depth_texture;      // 0: the stand-in (map_info.x 0: never read)
        // A stencil test needs the scene depth attached (its stencil holds the avatar mark).
        const bool drawn = (pass.pixels == LLRHI::WET_LOOK_EVERYWHERE || target.depth_read_only) &&
                           mCompute[pass.pixels].draw(cmd, target, &uniforms, sizeof(uniforms), images, COMPUTE_IMAGES);
        LLVKTracker::endPass();
        if (drawn && !mLoggedCompute)
        {
            mLoggedCompute = true;
            LL_INFOS("VKWetLook") << "Wet look: " << target.width << "x" << target.height << ", " << target.color_count
                                  << " attachments, depth format " << (U32)target.depth_format
                                  << (target.stencil ? " with" : " without") << " stencil" << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::writeBack(const LLRHI::WetLookWriteBack& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.diffuse_texture || !pass.specular_texture || !pass.normal_texture ||
            (pass.pixels != LLRHI::WET_LOOK_EVERYWHERE && pass.pixels != LLRHI::WET_LOOK_NOT_AVATARS))
        {
            return false;
        }
        mWriteBackSampled.assign({ pass.diffuse_texture, pass.specular_texture, pass.normal_texture });
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mWriteBackSampled, target);
        if (!cmd)
        {
            return false;
        }
        bool drawn = false;
        if (target.color_count == COPY_COLORS || target.color_count == COPY_COLORS + 1)
        {
            const WriteBackUniforms uniforms = {};
            LLVKFullscreen::Image images[WRITE_BACK_IMAGES];
            images[0].texture = pass.diffuse_texture;
            images[1].texture = pass.specular_texture;
            images[2].texture = pass.normal_texture;
            LLVKFullscreen::Pass& draw_pass = mWriteBack[target.color_count == COPY_COLORS ? 0 : 1]
                                                        [pass.pixels == LLRHI::WET_LOOK_EVERYWHERE ? 0 : 1];
            drawn = draw_pass.draw(cmd, target, &uniforms, sizeof(uniforms), images, WRITE_BACK_IMAGES);
        }
        LLVKTracker::endPass();
        if (drawn && !mLoggedWriteBack)
        {
            mLoggedWriteBack = true;
            LL_INFOS("VKWetLook") << "Wet look: written back into the G-buffer, " << target.color_count << " attachments" << LL_ENDL;
        }
        return drawn;
    }

    bool record_compute(const LLRHI::WetLookCompute& pass)
    {
        return sRenderer.compute(pass);
    }

    bool record_write_back(const LLRHI::WetLookWriteBack& pass)
    {
        return sRenderer.writeBack(pass);
    }
}

void LLVKWetLook::install()
{
    if (!LLRHI::sWetLook)
    {
        LL_INFOS("VKWetLook") << "Wet look off (LLRHI_WET_LOOK=0)" << LL_ENDL;
        return;
    }
    LLRHI::sWetLookCompute = &record_compute;
    LLRHI::sWetLookWriteBack = &record_write_back;
    LLVKShader::addBakeStep("wet look", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
