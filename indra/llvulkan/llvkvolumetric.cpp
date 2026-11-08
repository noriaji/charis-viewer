/**
 * @file llvkvolumetric.cpp
 * @brief Volumetric light (sun shafts) recorded directly in Vulkan (research/84).
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

#include "llvkvolumetric.h"

#include "llrhivolumetric.h"
#include "llvkdevice.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Shaders

    // The march (LLRHI::VolumetricMarch): sun light scattered towards the eye
    // along the view ray of one full-resolution pixel per march pixel, up to
    // the scene or the end of the last shadow cascade. With the clouds' shadow
    // in the air (LLRHI::VolumetricClouds, research/253) its channels are shares
    // instead: the near air's light over the light's colour, and the far air's
    // (past the march, up to the clouds' base or the scene) brighter and darker.
    const char* const MARCH_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform March
{
    mat4 inverse_projection;
    mat4 shadow_matrices[4];    // eye space -> shadow map texture space
    vec4 shadow_far;            // each cascade's far end (view depth, m)
    vec4 light_direction;       // xyz: towards the light, eye space
    vec4 light_color;           // rgb: linear, at full scattering, times the phase function's normalisation
    vec4 mist_up;               // xyz: world up in eye space, w: the camera's height above the mist's level (m)
    vec4 mist;                  // x: 1/height scale (1/m; 0 no mist), y: extra scattering (times the base's), z: extra extinction (1/m)
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    vec2 shadow_depth;          // stored shadow depth = texture space z * x + y
    ivec2 depth_size;           // the scene depth's size
    int scale;                  // full-resolution pixels per march pixel, each way
    int steps;                  // samples along a ray that crosses every cascade
    float extinction;           // per metre
    float anisotropy;           // Henyey-Greenstein g
    float max_length;           // m: no ray marches farther (0: up to the last cascade's end)
    // The clouds' shadow in the air (LLRHI::VolumetricClouds, research/253)
    vec4 cloud_east;            // xyz: east in eye space; w: 1 / the map's width (1/m)
    vec4 cloud_north;           // xyz: north in eye space; w: the layer's base above the camera (m)
    vec4 cloud_up;              // xyz: up in eye space; w: the layer's middle above the camera (m)
    vec4 cloud_light;           // xy: the way to the light, m east and north per m up; z: the shadow's strength (0: none); w: the reference's mip
    vec4 cloud_far;             // xy: the map's uv of the camera's way to the light; z: the far air's extinction (1/m; 0: none); w: its stages
    vec4 haze;                  // the sky haze's ambient share, its sun share in every direction, around the light (x the glow); the sun's as drawn below the clouds
    vec4 glow;                  // x, y: the sky's glow.x and glow.z; z: the glow's factor (0: none)
    vec4 scene_light;           // xyz: hazeF.glsl's lightnorm (Windlight's order north, up, east), for the scene haze's glow
    vec4 scene_shares;          // the scene haze's ambient share, its sun share in every direction, around the light (x the glow): the light dimmed for lightnorm.y, as drawn below the clouds
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D shadow_map0;
layout(set = 0, binding = 3) uniform sampler2D shadow_map1;
layout(set = 0, binding = 4) uniform sampler2D shadow_map2;
layout(set = 0, binding = 5) uniform sampler2D shadow_map3;
layout(set = 0, binding = 6) uniform sampler2D cloud_map;   // plus/volclouds' map, with mips (read only while cloud_light.z > 0)

layout(location = 0) out vec4 frag_color;

// Short rays (the scene is near) take fewer samples, down to this many.
const int MIN_STEPS = 4;
// Shadow depth a sample may lie behind the stored depth and still be lit
// (the depth's own precision; there is no surface to bias away from).
const float SHADOW_BIAS = 0.0002;
// A ray to the sky has no far air below this much of up (the horizon, a long way under the clouds' far
// edge), full above that much.
const vec2 FAR_HORIZON = vec2(0.02, 0.06);
// The far air stops where the map's uv leaves this square (0.9 of its radius: its outer tenth fades out).
const vec2 MAP_INNER = vec2(0.05, 0.95);
// A far stage is read at the mip of this share of its length (its whole length blurs the shafts across).
const float FAR_LOD_SHARE = 0.5;
// The share of the sky haze's light that is its sun's around the camera is held above this.
const float MIN_REFERENCE_LIGHT = 0.1;

// Henyey-Greenstein, scaled so that isotropic scattering is 1.
float phase(float cos_theta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5);
}

// The first sample's offset within its step: interleaved gradient noise
// (Jimenez 2014), a fixed pattern the upsample's four taps smooth out.
float dither(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

vec2 shadowMapSize(int cascade)
{
    switch (cascade)
    {
    case 0: return vec2(textureSize(shadow_map0, 0));
    case 1: return vec2(textureSize(shadow_map1, 0));
    case 2: return vec2(textureSize(shadow_map2, 0));
    default: return vec2(textureSize(shadow_map3, 0));
    }
}

// The four stored depths of the 2 x 2 texels around uv (textureGather
// order: (0,1) (1,1) (1,0) (0,0)).
vec4 storedShadowDepths(int cascade, vec2 uv)
{
    switch (cascade)
    {
    case 0: return textureGather(shadow_map0, uv, 0);
    case 1: return textureGather(shadow_map1, uv, 0);
    case 2: return textureGather(shadow_map2, uv, 0);
    default: return textureGather(shadow_map3, uv, 0);
    }
}

// 1 where the light reaches pos (eye space), 0 where it is shadowed; in
// between across a shadow map texel: the four texels around the point each
// compared, then weighted bilinearly (a hard single-texel compare drew the
// far cascades' coarse texels as blocks and steps in the shafts).
float lightVisibility(vec3 pos)
{
    float view_depth = -pos.z;
    int cascade = view_depth < u.shadow_far.x ? 0 : view_depth < u.shadow_far.y ? 1 : view_depth < u.shadow_far.z ? 2 : 3;
    vec4 coord = u.shadow_matrices[cascade] * vec4(pos, 1.0);
    coord.xyz /= coord.w;
    if (any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0))))
    {
        return 1.0;     // outside the map: nothing there casts a shadow
    }
    float depth = coord.z * u.shadow_depth.x + u.shadow_depth.y;
    vec2 size = shadowMapSize(cascade);
    vec2 texel = coord.xy * size - 0.5;
    vec2 f = fract(texel);
    vec4 stored = storedShadowDepths(cascade, (floor(texel) + 1.0) / size);
    vec4 lit = vec4(greaterThanEqual(stored + SHADOW_BIAS, vec4(depth)));
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

// volumetric march step: begin
// One step of the march, step_length long: the share of the light (lit) its air scatters towards
// the eye where the light reaches it (visibility), dimmed by the air before it (transmittance, to
// the step's start), integrated exactly over the step; then the transmittance past it. The air
// scatters at most what it takes out (scattering <= extinction), so lit + transmittance never
// exceeds 1: thicker air goes towards the light's colour, never past it (fix-r9).
void marchStep(float visibility, float scattering, float extinction, float step_length, inout float transmittance, inout float lit)
{
    float sigma = max(extinction, 1e-6);
    float step_transmittance = exp(-sigma * step_length);
    lit += visibility * min(scattering / sigma, 1.0) * transmittance * (1.0 - step_transmittance);
    transmittance *= step_transmittance;
}
// volumetric march step: end

// volumetric cloud visibility: begin
// The clouds' shadow along a view ray (research/253; world: the ray's direction east, north, up). The map's
// uv where the way to the light from the point t metres along the ray crosses the layer's middle is a line
// in t: cloud_far.xy + mapStep(world) * t.
vec2 mapStep(vec3 world)
{
    return (world.xy - u.cloud_light.xy * world.z) * u.cloud_east.w;
}

// How much of the map counts at uv: none outside it, fading in over its outer tenth.
float mapEdge(vec2 uv)
{
    float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    return smoothstep(0.0, 0.1, edge);
}

// How much of the light reaches the point t metres along the ray: sunLightF.glsl's plusCloudShadow for
// that point (1 above the layer's middle).
float cloudVisibility(vec3 world, vec2 map_step, float t)
{
    if (world.z * t >= u.cloud_up.w)
    {
        return 1.0;
    }
    vec2 uv = u.cloud_far.xy + map_step * t;
    return 1.0 - u.cloud_light.z * mapEdge(uv) * textureLod(cloud_map, uv, 0.0).x;
}

// The near march's cloud visibility at t, from its reads at 0, half_length and twice that: straight lines
// between (the clouds' shadows are km wide, the near march at most MAX_MARCH_METRES long).
float nearCloudVisibility(vec3 reads, float t, float half_length)
{
    float f = t / max(half_length, 1e-3);
    return f < 1.0 ? mix(reads.x, reads.y, f) : mix(reads.y, reads.z, min(f - 1.0, 1.0));
}
// volumetric cloud visibility: end
)GLSL"
    // (two literals: one may hold at most about 16,000 characters, MSVC C2026)
    R"GLSL(
// volumetric far air: begin
// The far air (research/253 3.3): past the near march, up to the clouds' base (a ray to the sky) or the
// scene, the sky already draws the haze. Where the map is clearer than around the camera (reference, its
// map value there) the haze is lit more, where darker less: the deviation, sum of w_i d_i over stages of
// equal light (w_i = share / stages, the share 1 - exp(-k (t1 - t0)) of the light the stretch holds), each
// read once (offset within it) at the mip of FAR_LOD_SHARE of its length, d_i = strength x edge x
// (reference - c_i): the low light's fade and the map's edge bring it to 0, never to a brighter sky.
float farAirDeviation(vec3 world, vec2 map_step, float t0, float t1, float offset, float reference, out float share)
{
    float k = u.cloud_far.z;
    share = 1.0 - exp(-k * max(t1 - t0, 0.0));
    if (share <= 0.0)
    {
        return 0.0;
    }
    float stages = u.cloud_far.w;
    float texels_per_metre = length(map_step) * float(textureSize(cloud_map, 0).x);
    float sum = 0.0;
    float start = t0;
    for (float i = 1.0; i <= stages; i += 1.0)
    {
        float end = t0 - log(max(1.0 - share * i / stages, 1e-7)) / k;
        float t = mix(start, end, offset);
        vec2 uv = u.cloud_far.xy + map_step * t;
        float c = world.z * t >= u.cloud_up.w ? 0.0 : textureLod(cloud_map, uv, log2(max(texels_per_metre * (end - start) * FAR_LOD_SHARE, 1.0))).x;
        sum += mapEdge(uv) * (reference - c);
        start = end;
    }
    return u.cloud_light.z * share / stages * sum;
}

// How far along the ray (from the camera) the map's uv stays inside MAP_INNER.
float mapExit(vec2 map_step)
{
    float t = 1e9;
    for (int axis = 0; axis < 2; ++axis)
    {
        float from = u.cloud_far[axis];
        float along = map_step[axis];
        if (along > 0.0)
        {
            t = min(t, (MAP_INNER.y - from) / along);
        }
        else if (along < 0.0)
        {
            t = min(t, (MAP_INNER.x - from) / along);
        }
    }
    return t;
}

// Where the far air ends along the ray (m from the camera): for a ray to the sky (sky; the volumetric
// clouds write no depth: theirs too) the clouds' base, none below FAR_HORIZON.x of up; else the scene
// (ray_length); never past where the map's uv leaves MAP_INNER.
float farAirEnd(bool sky, vec3 world, vec2 map_step, float ray_length)
{
    float t1 = sky ? (world.z > FAR_HORIZON.x ? u.cloud_north.w / world.z : 0.0) : ray_length;
    return min(t1, mapExit(map_step));
}

// The sky's glow around the light (skyF.glsl's haze_glow) at the cosine of the angle to it; 0 without one
// (the moon). The power is held under 1e5 as atmosphericsFuncs.glsl holds it: glow.z goes down to -10, which
// near the light (or with glow.x 0) reaches 1e10 and more, past what a 16-bit float holds (and 0 x inf is a
// NaN without a glow). The same in the composite: keep the two in step.
float hazeGlow(float cos_theta)
{
    return u.glow.z * (min(pow(max(max(1.0 - cos_theta, 0.001) * u.glow.x, 1e-6), u.glow.y), 1e5) + 0.25);
}

// The sky haze's ambient light over its sun light in a direction at cos_theta to the light (research/253
// middle 1: a(theta), the sky's own terms; the sun's as the sky draws it below the clouds).
float hazeAmbientRatio(float cos_theta)
{
    return u.haze.x / max(u.haze.y + u.haze.z * hazeGlow(cos_theta), 1e-4);
}

// The scene haze's glow angle for a ray (eye space) at cos_theta to the light: calcAtmosphericVars' own, the ray
// dotted with lightnorm (Windlight's world order, not turned to eye space), times cos_theta where the ray faces the
// light. The same in the composite: keep the two in step.
float sceneGlowCos(vec3 ray, float cos_theta)
{
    return dot(ray, u.scene_light.xyz) * max(cos_theta, 0.0);
}

// The scene haze's ambient light over its sun light for a ray to the scene: its own terms, as the composite's
// sceneHaze has them (the glow at sceneGlowCos, the light dimmed for lightnorm.y, tmpAmbient).
float sceneAmbientRatio(vec3 ray, float cos_theta)
{
    return u.scene_shares.x / max(u.scene_shares.y + u.scene_shares.z * hazeGlow(sceneGlowCos(ray, cos_theta)), 1e-4);
}

// a(theta) along the ray: the sky haze's for a ray to the sky (sky), the scene's own haze's for a ray to the scene.
// The composite takes the darker share of the haze the pixel shows, so the share must hold no more than that haze's
// own sun light (research/253 review 4 middle 1: with the sky's terms the share was some 3 times that towards a low
// sun, and a ground 1 km off in a cloud's shadow kept 0.49 of its light for 0.78 in shaftsim's A-6AM).
float farAirAmbientRatio(bool sky, vec3 ray, float cos_theta)
{
    return sky ? hazeAmbientRatio(cos_theta) : sceneAmbientRatio(ray, cos_theta);
}

// The far air's darker share: the share of a pixel's light its shadows take out. The pixel is taken as the
// sky's haze, whose sun share is reference_light (the light around the camera) to its ambient share
// ambient (a(theta)): the deviation's share of that, never more than the air's share of the light.
float farAirDarker(float deviation, float reference_light, float ambient, float share)
{
    return min(max(-deviation, 0.0) / (reference_light + ambient), share);
}
// volumetric far air: end

void main()
{
    // The full-resolution pixel this march pixel stands for: the top-left of its block.
    ivec2 texel = min(ivec2(gl_FragCoord.xy) * u.scale, u.depth_size - 1);
    float depth = texelFetch(scene_depth, texel, 0).r;
    vec2 ndc_xy = (vec2(texel) + 0.5) / vec2(u.depth_size) * 2.0 - 1.0;
    vec4 eye = u.inverse_projection * vec4(ndc_xy, depth * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
    vec3 pos = eye.xyz / eye.w;
    float ray_length = length(pos);
    vec3 ray = pos / ray_length;

    // Where the ray leaves the last cascade (no shadows known beyond it), or
    // the longest march, whichever is nearer: the full march, u.steps samples.
    float full_length = u.shadow_far.w / max(-ray.z, 1e-3);
    if (u.max_length > 0.0)
    {
        full_length = min(full_length, u.max_length);
    }
    // Never 0 (no cascade end known: shadow_far.w 0), which would divide 0 by 0 below.
    full_length = max(full_length, 1e-3);
    float march_length = min(ray_length, full_length);
    int steps = max(MIN_STEPS, int(ceil(float(u.steps) * march_length / full_length)));
    float step_length = march_length / float(steps);

    // With the clouds' shadow in the air (research/253), the light reaching each step is dimmed by the
    // clouds above it too.
    bool clouds = u.cloud_light.z > 0.0;
    vec3 world = vec3(dot(u.cloud_east.xyz, ray), dot(u.cloud_north.xyz, ray), dot(u.cloud_up.xyz, ray));
    vec2 map_step = mapStep(world);
    float half_length = 0.5 * march_length;
    vec3 near_clouds = vec3(1.0);
    if (clouds)
    {
        near_clouds = vec3(cloudVisibility(world, map_step, 0.0), cloudVisibility(world, map_step, half_length),
                           cloudVisibility(world, map_step, march_length));
    }

    // Step i covers [i, i + 1) step lengths, its light read at a dithered point within it. With
    // mist the air gets denser near its level, so the transmittance is carried step by step.
    float offset = dither(gl_FragCoord.xy);
    float t = step_length * offset;
    float transmittance = 1.0;
    float lit = 0.0;
    for (int i = 0; i < steps; ++i, t += step_length)
    {
        float scattering = u.extinction;
        float extinction = u.extinction;
        if (u.mist.x > 0.0)
        {
            float height = u.mist_up.w + dot(u.mist_up.xyz, ray) * t;
            float p = exp(-u.mist.x * clamp(height, 0.0, 1000.0));
            scattering += u.extinction * u.mist.y * p;
            extinction += u.mist.z * p;
        }
        float visibility = lightVisibility(ray * t);
        if (clouds)
        {
            visibility *= nearCloudVisibility(near_clouds, t, half_length);
        }
        marchStep(visibility, scattering, extinction, step_length, transmittance, lit);
    }
    float cos_theta = dot(ray, u.light_direction.xyz);
    if (!clouds)
    {
        frag_color = vec4(u.light_color.rgb * (phase(cos_theta, u.anisotropy) * lit), -pos.z);
        return;
    }

    // The far air, from where the near march stopped (farAirEnd), against the map around the camera.
    bool sky = depth >= 1.0;
    float deviation = 0.0;
    float share = 0.0;
    float reference_light = 1.0;
    if (u.cloud_far.z > 0.0)
    {
        float reference = textureLod(cloud_map, u.cloud_far.xy, u.cloud_light.w).x;
        reference_light = max(1.0 - u.cloud_light.z * reference, MIN_REFERENCE_LIGHT);
        deviation = farAirDeviation(world, map_step, march_length, farAirEnd(sky, world, map_step, ray_length), offset,
                                    reference, share);
        if (sky)
        {
            deviation *= smoothstep(FAR_HORIZON.x, FAR_HORIZON.y, world.z);
        }
    }
    // r: the near air's light over the light's colour; g: the far air's brighter share; b: its darker share.
    frag_color = vec4(phase(cos_theta, u.anisotropy) * lit, max(deviation, 0.0),
                      farAirDarker(deviation, reference_light, farAirAmbientRatio(sky, ray, cos_theta), share), -pos.z);
}
)GLSL";

    // The composite (LLRHI::VolumetricComposite): the march's light at each
    // full-resolution pixel, from the 4 x 4 nearest march pixels weighted by a
    // tent two march pixels wide each way and by how close their view depth is
    // to this pixel's (so light in front of a near object does not bleed onto
    // the far one beside it). The wide tent also smooths the march's dither
    // (interleaved gradient noise, whose pattern repeats within a few pixels):
    // with the four nearest alone it showed as grain (alpha-sky).
    // Laid over the target by the blend state (premultiplied): the target
    // dimmed by the air's transmittance, the air's light added (fix-r9).
    const char* const COMPOSITE_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Composite
{
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    float depth_a;              // projection [2][2]
    float depth_b;              // projection [3][2]
    ivec2 scatter_size;
    int scale;                  // full-resolution pixels per march pixel, each way
    float march_far;            // view depth where the march stops at the latest; 0: no edge fallback
    vec4 ray;                   // xy: 1 / projection [0][0], [1][1]; zw: projection [2][0], [2][1]
    vec4 air;                   // rgb: the sky's light the air scatters (linear), a: extinction per metre
    vec2 march_end;             // x: the last cascade's end (view depth, m), y: the longest march (m; 0: none)
    // With the clouds' shadow in the air (LLRHI::VolumetricCloudsLight, research/253). The sky's haze is in the
    // units skyF.glsl writes the sky in (sRGB, before softenLightF.glsl takes it to the screen's light).
    vec4 light;                 // rgb: the near air's light (the march's light_color); a: 1: the march's channels are shares, 0: its rgb is the light
    vec4 haze_sun;              // rgb: the sky haze's sun light in every direction, for a share of 1, before the air dims it; a: the brightest each channel of the haze may be lit to
    vec4 haze_glow;             // rgb: its sun light around the light, times the glow, likewise; a: the sky's colour to the screen's light (sky_hdr_scale, x 1.1 classic)
    vec4 glow;                  // x, y: the sky's glow.x and glow.z; z: the glow's factor (0: none)
    vec4 light_dir;             // xyz: towards the light, eye space
    vec4 haze_ambient;          // rgb: the sky haze's ambient light below the clouds; a: the share of the light the sky draws below them
    vec4 haze_above;            // rgb: its ambient light above the clouds
    vec4 sky_up;                // xyz: up, eye space
    vec4 sky_depth;             // rgb: the light's optical depth to a ray (skyF's light_atten); a: the light's up
    vec4 sky_air;               // rgb: the air's optical depth straight up (skyF's combined haze x density_multiplier x max_y)
    vec4 scene_air;             // rgb: the scene's haze's extinction per metre (atmosphericsFuncs.glsl's combined_haze x density_multiplier x distance_multiplier); a: its colour to the screen's light (hazeF.glsl's sky_hdr_scale)
    vec4 scene_light;           // xyz: hazeF.glsl's lightnorm (LLEnvironment::getClampedLightNorm: the light in Windlight's order north, up, east, not turned to eye space)
    vec4 scene_ambient;         // rgb: the scene's haze's ambient light below the clouds (atmosphericsFuncs.glsl's tmpAmbient, without skyF's max(0, ...))
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D scatter;

layout(location = 0) out vec4 frag_color;

// Relative view depth difference at which a march pixel no longer counts.
const float DEPTH_TOLERANCE = 0.1;
// Below this total weight no march pixel is near in depth: take the nearest.
const float MIN_WEIGHT = 1e-4;

float viewDepth(float depth)
{
    return u.depth_b / (depth * u.depth_to_ndc.x + u.depth_to_ndc.y + u.depth_a);
}

// volumetric air transmittance: begin
// The length of this pixel's ray (ndc: its x and y) up to view_depth.
float rayLength(float view_depth, vec2 ndc)
{
    vec2 slope = (ndc + u.ray.zw) * u.ray.xy;   // eye x / -z, y / -z
    return view_depth * sqrt(1.0 + dot(slope, slope));
}

// The share of the scene's light that comes through the air the march crossed along this pixel's
// ray (ndc: its x and y): up to the scene (view_depth), the last cascade's end and the longest
// march, as the march's rays; the base air only (the ground fog dims the scene in its own pass).
float airTransmittance(float view_depth, vec2 ndc)
{
    float marched = rayLength(min(view_depth, u.march_end.x), ndc);
    if (u.march_end.y > 0.0)
    {
        marched = min(marched, u.march_end.y);
    }
    return exp(-u.air.a * max(marched, 0.0));
}

// What the composite lays over the scene: the air's light (inscatter) and the sky's light (ambient) for
// the share of the scene's light the near air takes out (taken); alpha is the share of the scene's light
// that does not come through, so the blend (premultiplied) leaves scene * transmittance * kept + this.
// kept: what the far air's shadows leave of the scene's light (research/253; 1 without them): the sky's
// haze they cover is the sun's light that does not reach that air, taken out with nothing in its place.
vec4 airOver(vec3 inscatter, vec3 ambient, float transmittance, float kept)
{
    float taken = 1.0 - transmittance;
    return vec4(inscatter + ambient * taken, 1.0 - transmittance * kept);
}
// volumetric air transmittance: end
)GLSL"
    // (two literals: one may hold at most about 16,000 characters, MSVC C2026)
    R"GLSL(
// The sky's glow around the light (skyF.glsl's haze_glow) at the cosine of the angle to it; 0 without one
// (the moon). The power is held under 1e5 as atmosphericsFuncs.glsl holds it: glow.z goes down to -10, which
// near the light (or with glow.x 0) reaches 1e10 and more, past what a 16-bit float holds (and 0 x inf is a
// NaN without a glow). The same in the march: keep the two in step.
float hazeGlow(float cos_theta)
{
    return u.glow.z * (min(pow(max(max(1.0 - cos_theta, 0.001) * u.glow.x, 1e-6), u.glow.y), 1e5) + 0.25);
}

// The scene haze's glow angle for a ray (eye space) at cos_theta to the light: calcAtmosphericVars' own, the ray
// dotted with lightnorm (Windlight's world order, not turned to eye space), times cos_theta where the ray faces the
// light. The same in the march: keep the two in step.
float sceneGlowCos(vec3 ray, float cos_theta)
{
    return dot(ray, u.scene_light.xyz) * max(cos_theta, 0.0);
}

// volumetric far air light: begin
// The far air's shares are of the haze in the units skyF.glsl writes the sky in (research/253): the haze is lit
// more or less there, and the difference taken to the screen's light as the haze is taken there, so that the far
// air shows as the haze would lit that much more or less. At a pixel of the sky that is the sky's haze in its
// direction (softenLightF.glsl takes it: made linear, times sky_hdr_scale and 1.1 under a classic sky); at a pixel
// of the scene the scene's own haze up to it (atmosphericsFuncs.glsl's calcAtmosphericVars, which hazeF.glsl lays
// over the scene: made linear, times sky_hdr_scale), not the sky's behind it (research/253 review 2 middle 1).
const vec3 LUMINANCE = vec3(0.2126, 0.7152, 0.0722);

// srgbF.glsl's srgb_to_linear, one channel.
float srgbToLinear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// A colour in the sky's units (skyF.glsl's) in the screen's light: made linear, times scale.
vec3 toScreen(vec3 c, float scale)
{
    return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)) * scale;
}

// The sky's colour in the screen's light, as softenLightF.glsl takes it.
vec3 skyLight(vec3 c)
{
    return toScreen(c, u.haze_glow.a);
}

// The direction of the pixel ndc's ray, eye space (unit).
vec3 pixelRay(vec2 ndc)
{
    return normalize(vec3((ndc + u.ray.zw) * u.ray.xy, -1.0));
}

// The sky in the direction of the pixel ndc as skyF.glsl draws it (the composite cannot read the sky drawn
// under it; skyF's terms, without the rainbow and the halo): the light dimmed by the air above the ray, the
// haze above the clouds as thick as the air up the ray, mixed towards the haze below them (the light dimmed by
// their cover, more ambient) near the horizon. Each channel held to haze_sun.a: skyF's own 5, or white under the
// "no post" tone mapping (the screen clamped to 1). sun: the haze's sun light in that direction for a share of 1.
vec3 hazeColour(vec2 ndc, out vec3 sun)
{
    vec3 ray = pixelRay(ndc);
    float up = dot(ray, u.sky_up.xyz);
    vec3 dimmed = exp(-u.sky_depth.rgb / max(1e-6, max(up, 0.0) + u.sky_depth.a));
    sun = (u.haze_sun.rgb + u.haze_glow.rgb * hazeGlow(dot(ray, u.light_dir.xyz))) * dimmed;
    vec3 clear = exp(-u.sky_air.rgb / max(up, 1e-3));
    vec3 above = (sun + u.haze_above.rgb) * (1.0 - clear);
    vec3 below = sun * u.haze_ambient.a + u.haze_ambient.rgb;
    return min(above + (below - above) * (1.0 - sqrt(sqrt(clear))), vec3(u.haze_sun.a));
}

// The scene's own haze at the pixel ndc, ray_length (m) away, as atmosphericsFuncs.glsl's calcAtmosphericVars has
// it (times 2, as hazeF.glsl takes it): the haze below the clouds (the light dimmed by their cover, its own ambient),
// the light dimmed for the light's up alone (lightnorm.y: the scene's short rays), as thick as the air up to the
// scene, each channel. Its glow's angle is calcAtmosphericVars' own (sceneGlowCos); so the haze is drawn, so it is
// lit here (the true angle drew the beams over the ground several times too
// bright towards a rising sun, shaftsim's A-6AM 1 km +233% for +50%: research/253 review 3 middle 1). The moon's
// light is taken as the sky takes it (0.7 times, no glow; the scene's haze takes it whole, with a faint glow): at
// night the far air is a few thousandths of the haze. Held as hazeColour holds it (the scene's haze itself goes to
// 20: past 5 the far air's light could pass a 16-bit float). sun: the haze's sun light for a share of 1, as
// hazeColour's; whole: the haze all the way.
vec3 sceneHaze(vec2 ndc, float ray_length, out vec3 sun, out vec3 whole)
{
    vec3 ray = pixelRay(ndc);
    vec3 dimmed = exp(-u.sky_depth.rgb / max(1e-6, u.scene_light.y));
    sun = (u.haze_sun.rgb + u.haze_glow.rgb * hazeGlow(sceneGlowCos(ray, dot(ray, u.light_dir.xyz)))) * dimmed;
    vec3 below = sun * u.haze_ambient.a + u.scene_ambient.rgb;
    whole = min(below, vec3(u.haze_sun.a));
    return min(below * (1.0 - exp(-u.scene_air.rgb * ray_length)), vec3(u.haze_sun.a));
}

// The far air's brighter share's light: the haze (hazeColour's or sceneHaze's; shown: it in the screen's light,
// at scale) with that share more of its sun light, held as the two hold it, in the screen's light, less the haze
// as it shows.
vec3 farAirLight(float share, vec3 haze, vec3 sun, vec3 shown, float scale)
{
    return share > 0.0 ? toScreen(min(haze + sun * share, vec3(u.haze_sun.a)), scale) - shown : vec3(0.0);
}

// What the far air's darker share (of the haze's light, 0 .. 1) leaves of a pixel of the sky: the haze's light
// with that share taken out, over its light, both in the screen's light (luminance); all of it where the haze
// gives none.
float farAirKept(float share, vec3 haze, vec3 shown)
{
    float whole = dot(shown, LUMINANCE);
    return share > 0.0 && whole > 1e-6 ? dot(skyLight(haze * (1.0 - share)), LUMINANCE) / whole : 1.0;
}

// What it leaves of a pixel of the scene (haze: the scene's haze, shown: it in the screen's light, at scale; whole: the
// haze all the way). The march takes the share of the whole haze's light (the pixel taken as that haze): that much of
// the whole haze is taken out of the scene's haze (never past none), and what it takes is put over the whole haze's
// light, as if the scene's own light under its haze (which the blend cannot see) were as bright as the rest of the
// whole haze. A pixel all haze keeps as the sky does; a near one, little haze over its own light, little less than all
// (research/253 review 2 middle 1: shaftsim's ground, 0.5 .. 2.5 km). All of it where the haze gives none.
float sceneAirKept(float share, vec3 haze, vec3 whole, vec3 shown, float scale)
{
    float whole_light = dot(toScreen(whole, scale), LUMINANCE);
    return share > 0.0 && whole_light > 1e-6 ? 1.0 - dot(shown - toScreen(max(haze - whole * share, vec3(0.0)), scale), LUMINANCE) / whole_light : 1.0;
}
// volumetric far air light: end

// What the march left at this pixel (taps: its channels, upsampled) laid over the scene: its light as it
// is or, with the clouds' shadow in the air, its shares given their light (r the near air's, g the far
// air's brighter, b its darker share; the far air's lies behind the near air, through its transmittance):
// of the sky's haze at a pixel of the sky (sky), of the scene's own haze up to it (ray_length m away) at a
// pixel of the scene. At an edge the near air's light is scaled by near_scale and the far air's shares by
// far_scale.
vec4 marchOver(vec3 taps, float near_scale, float far_scale, float transmittance, vec2 ndc, bool sky, float ray_length)
{
    if (u.light.a <= 0.0)
    {
        return airOver(taps * near_scale, u.air.rgb, transmittance, 1.0);
    }
    vec3 inscatter = u.light.rgb * (taps.r * near_scale);
    float brighter = taps.g * far_scale;
    float darker = clamp(taps.b * far_scale, 0.0, 1.0);
    float kept = 1.0;
    if (brighter > 0.0 || darker > 0.0)
    {
        vec3 sun;
        if (sky)
        {
            vec3 haze = hazeColour(ndc, sun);
            vec3 shown = skyLight(haze);
            inscatter += farAirLight(brighter, haze, sun, shown, u.haze_glow.a) * transmittance;
            kept = farAirKept(darker, haze, shown);
        }
        else
        {
            vec3 whole;
            vec3 haze = sceneHaze(ndc, ray_length, sun, whole);
            vec3 shown = toScreen(haze, u.scene_air.a);
            inscatter += farAirLight(brighter, haze, sun, shown, u.scene_air.a) * transmittance;
            kept = sceneAirKept(darker, haze, whole, shown, u.scene_air.a);
        }
    }
    return airOver(inscatter, u.air.rgb, transmittance, kept);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float stored_depth = texelFetch(scene_depth, pixel, 0).r;
    float depth = viewDepth(stored_depth);
    vec2 ndc = (vec2(pixel) + 0.5) / vec2(textureSize(scene_depth, 0)) * 2.0 - 1.0;
    float transmittance = airTransmittance(depth, ndc);
    // The far air's haze: the sky's where nothing was drawn (depth 1, as the march takes the sky), else the scene's.
    bool sky = stored_depth >= 1.0;
    float ray_length = rayLength(depth, ndc);

    // March pixel q stands for full-resolution pixel q * scale.
    vec2 place = vec2(pixel) / float(u.scale);
    ivec2 base = ivec2(floor(place));
    vec2 f = place - vec2(base);

    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    vec3 nearest = vec3(0.0);
    float nearest_depth = depth;
    float nearest_difference = 1e30;
    for (int i = 0; i < 16; ++i)
    {
        ivec2 offset = ivec2(i & 3, i >> 2) - 1;    // -1 .. 2 each way
        vec4 tap = texelFetch(scatter, clamp(base + offset, ivec2(0), u.scatter_size - 1), 0);
        vec2 along = max(vec2(0.0), 2.0 - abs(vec2(offset) - f));
        float difference = abs(tap.a - depth) / max(depth, 1e-3);
        float weight = along.x * along.y * max(0.0, 1.0 - difference / DEPTH_TOLERANCE);
        sum += tap.rgb * weight;
        weight_sum += weight;
        if (difference < nearest_difference)
        {
            nearest_difference = difference;
            nearest = tap.rgb;
            nearest_depth = tap.a;
        }
    }
    if (weight_sum > MIN_WEIGHT)
    {
        frag_color = marchOver(sum / weight_sum, 1.0, 1.0, transmittance, ndc, sky, ray_length);
        return;
    }
    // No march pixel lies near this pixel in depth: an edge thinner than a
    // march pixel (hair strands against the sky). The light grows about with
    // the length of air marched: a pixel nearer than the nearest march pixel
    // takes its light scaled by this ray's march over its, so a strand in
    // front of the sky does not get the light of the air behind it. Never
    // scaled up: the sky between strands keeps the strands' light (a far
    // ray's shadows are unknown here, and a brighter rim would show). The
    // far air's shares lie behind the nearest march pixel: none for a pixel
    // nearer than it (research/253 3.5).
    float scale_by = 1.0;
    if (u.march_far > 0.0)
    {
        scale_by = min(1.0, min(depth, u.march_far) / max(min(nearest_depth, u.march_far), 1e-3));
    }
    frag_color = marchOver(nearest, scale_by, depth >= nearest_depth ? 1.0 : 0.0, transmittance, ndc, sky, ray_length);
}
)GLSL";

    // The blur (LLRHI::VolumetricBlur): one axis of a 9-tap Gaussian at the
    // march's size, each tap weighted also by how close its view depth (alpha)
    // is to the centre's. Alpha (the view depth) is kept for the next pass.
    const char* const BLUR_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Blur
{
    ivec2 size;
    ivec2 direction;
} u;

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out vec4 frag_color;

// Relative view depth difference at which a tap no longer counts (as the composite's).
const float DEPTH_TOLERANCE = 0.1;
// Gaussian, sigma 2 texels: centre, then 1 .. 4 away.
const float WEIGHTS[5] = float[5](0.2042, 0.1802, 0.1238, 0.0663, 0.0276);

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 centre = texelFetch(source, pixel, 0);
    vec3 sum = centre.rgb * WEIGHTS[0];
    float weight_sum = WEIGHTS[0];
    for (int i = 1; i <= 4; ++i)
    {
        for (int s = -1; s <= 1; s += 2)
        {
            vec4 tap = texelFetch(source, clamp(pixel + u.direction * (i * s), ivec2(0), u.size - 1), 0);
            float difference = abs(tap.a - centre.a) / max(centre.a, 1e-3);
            float weight = WEIGHTS[i] * max(0.0, 1.0 - difference / DEPTH_TOLERANCE);
            sum += tap.rgb * weight;
            weight_sum += weight;
        }
    }
    frag_color = vec4(sum / weight_sum, centre.a);
}
)GLSL";

    // ------------------------------------------------------------------
    // Uniform blocks, as the shaders' std140 blocks lay them out.

    struct MarchUniforms
    {
        F32 inverse_projection[16];
        F32 shadow_matrices[4][16];
        F32 shadow_far[4];
        F32 light_direction[4];
        F32 light_color[4];
        F32 mist_up[4];
        F32 mist[4];
        F32 depth_to_ndc[2];
        F32 shadow_depth[2];
        S32 depth_size[2];
        S32 scale;
        S32 steps;
        F32 extinction;
        F32 anisotropy;
        F32 max_length;
        F32 padding;
        F32 cloud_east[4];
        F32 cloud_north[4];
        F32 cloud_up[4];
        F32 cloud_light[4];
        F32 cloud_far[4];
        F32 haze[4];
        F32 glow[4];
        F32 scene_light[4];
        F32 scene_shares[4];
    };
    static_assert(sizeof(MarchUniforms) == 592, "MarchUniforms must match the std140 block March");

    struct CompositeUniforms
    {
        F32 depth_to_ndc[2];
        F32 depth_a;
        F32 depth_b;
        S32 scatter_size[2];
        S32 scale;
        F32 march_far;
        F32 ray[4];
        F32 air[4];
        F32 march_end[2];
        F32 padding[2];
        F32 light[4];
        F32 haze_sun[4];
        F32 haze_glow[4];
        F32 glow[4];
        F32 light_dir[4];
        F32 haze_ambient[4];
        F32 haze_above[4];
        F32 sky_up[4];
        F32 sky_depth[4];
        F32 sky_air[4];
        F32 scene_air[4];
        F32 scene_light[4];
        F32 scene_ambient[4];
    };
    static_assert(sizeof(CompositeUniforms) == 288, "CompositeUniforms must match the std140 block Composite");

    struct BlurUniforms
    {
        S32 size[2];
        S32 direction[2];
    };
    static_assert(sizeof(BlurUniforms) == 16, "BlurUniforms must match the std140 block Blur");

    constexpr U32 MARCH_IMAGES = 6;         // scene depth, four shadow maps, the clouds' shadow map
    constexpr U32 COMPOSITE_IMAGES = 2;     // scene depth, march result
    constexpr U32 BLUR_IMAGES = 1;          // the light to blur
    constexpr U32 SHADOW_CASCADES = 4;
    constexpr U32 MAX_STEPS = 256;

    // A shadow matrix's texture space z ((clip + 1) / 2) as the shadow map
    // stores it: the same, with depth clip control (required, research/124 V1).
    void shadowDepthStored(F32 out[2])
    {
        out[0] = 1.f;
        out[1] = 0.f;
    }

    // The clouds' shadow in the air (research/253): its values come in part from the region's sky
    // (server data), so each is held to what the shaders can take (a NaN as 0).
    constexpr U32 MAX_FAR_STAGES = 32;
    constexpr F32 MAX_FAR_EXTINCTION = 0.02f;  // per metre: 50 m of air holds 63 % of the light
    constexpr F32 MAX_REFERENCE_LOD = 16.f;
    // The sky's colour, and the scene's haze, to the screen's light (sky_hdr_scale is 1 .. 6.4 for the sky's gamma, 2
    // auto-adjusted): with the haze held to LLRHI::SKY_COLOUR_MAX the far air adds at most some 4300, inside a 16-bit float.
    constexpr F32 MAX_SKY_SCALE = 100.f;

    F32 held(F32 value, F32 low, F32 high)
    {
        return value >= low ? llmin(value, high) : low;     // also a NaN: low
    }

    void cloudUniforms(const LLRHI::VolumetricClouds& clouds, MarchUniforms& out)
    {
        const F32 strength = clouds.map_texture ? held(clouds.strength, 0.f, 1.f) : 0.f;
        out.cloud_light[2] = strength;
        if (strength <= 0.f)
        {
            return;     // the shaders read none of the rest
        }
        for (U32 c = 0; c < 3; ++c)
        {
            out.cloud_east[c] = clouds.axes[0][c];
            out.cloud_north[c] = clouds.axes[1][c];
            out.cloud_up[c] = clouds.axes[2][c];
        }
        out.cloud_east[3] = held(clouds.inverse_width, 0.f, 1.f);
        out.cloud_north[3] = held(clouds.layer_base, -1e6f, 1e6f);
        out.cloud_up[3] = held(clouds.layer_middle, -1e6f, 1e6f);
        out.cloud_light[0] = held(clouds.light_slope[0], -1e3f, 1e3f);
        out.cloud_light[1] = held(clouds.light_slope[1], -1e3f, 1e3f);
        out.cloud_light[3] = held(clouds.reference_lod, 0.f, MAX_REFERENCE_LOD);
        out.cloud_far[0] = held(clouds.camera_place[0], -1e3f, 1e3f);
        out.cloud_far[1] = held(clouds.camera_place[1], -1e3f, 1e3f);
        const bool far_air = clouds.far_stages > 0;     // ("far" is a Windows macro)
        out.cloud_far[2] = far_air ? held(clouds.far_extinction, 0.f, MAX_FAR_EXTINCTION) : 0.f;
        out.cloud_far[3] = (F32)llmin(clouds.far_stages, MAX_FAR_STAGES);
        for (U32 i = 0; i < 3; ++i)
        {
            out.haze[i] = held(clouds.haze_shares[i], 0.f, 1e3f);
            out.scene_shares[i] = held(clouds.scene_shares[i], 0.f, 1e3f);
            out.scene_light[i] = held(clouds.scene_lightnorm[i], -1.f, 1.f);
        }
        out.glow[0] = held(clouds.glow[0], 0.f, 1e3f);
        out.glow[1] = held(clouds.glow[1], -10.f, 10.f);
        out.glow[2] = held(clouds.glow[2], 0.f, 1.f);
    }

    // shares: the march before had the clouds' shadow in the air (its channels are shares).
    void cloudLightUniforms(const LLRHI::VolumetricCloudsLight& light, bool shares, CompositeUniforms& out)
    {
        out.light[3] = shares ? 1.f : 0.f;
        if (!shares)
        {
            return;
        }
        for (U32 c = 0; c < 3; ++c)
        {
            out.light[c] = held(light.light_color[c], 0.f, 1e3f);
            out.haze_sun[c] = held(light.haze_sun[c], 0.f, 1e3f);
            out.haze_glow[c] = held(light.haze_glow[c], 0.f, 1e3f);
            out.haze_ambient[c] = held(light.haze_ambient[c], 0.f, 1e3f);
            out.haze_above[c] = held(light.haze_above[c], 0.f, 1e3f);
            out.sky_depth[c] = held(light.light_depth[c], 0.f, 1e3f);
            out.sky_air[c] = held(light.air_depth[c], 0.f, 1e3f);
            out.sky_up[c] = held(light.up[c], -1.f, 1.f);
            out.light_dir[c] = light.light_direction[c];
            out.scene_air[c] = held(light.scene_extinction[c], 0.f, 1e3f);
            out.scene_light[c] = held(light.scene_lightnorm[c], -1.f, 1.f);
            out.scene_ambient[c] = held(light.scene_ambient[c], 0.f, 1e3f);
        }
        out.haze_sun[3] = held(light.far_ceiling, 0.f, LLRHI::SKY_COLOUR_MAX);
        out.haze_glow[3] = held(light.sky_scale, 0.f, MAX_SKY_SCALE);
        out.scene_air[3] = held(light.scene_scale, 0.f, MAX_SKY_SCALE);
        out.haze_ambient[3] = held(light.haze_drawn, 0.f, 1.f);
        out.sky_depth[3] = held(light.light_up, -1.f, 1.f);
        out.glow[0] = held(light.glow[0], 0.f, 1e3f);
        out.glow[1] = held(light.glow[1], -10.f, 10.f);
        out.glow[2] = held(light.glow[2], 0.f, 1.f);
    }

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool march(const LLRHI::VolumetricMarch& pass);
        bool composite(const LLRHI::VolumetricComposite& pass);
        bool blur(const LLRHI::VolumetricBlur& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        LLVKFullscreen::Pass mMarch;
        LLVKFullscreen::Pass mComposite;
        LLVKFullscreen::Pass mBlur;
        bool mLoggedFirst = false;
        // The last march wrote shares (the clouds' shadow in the air, research/253): the composite gives them their light.
        bool mMarchShares = false;
        // The textures a pass samples: kept so that no frame allocates them anew.
        std::vector<LLVKTracker::GLName> mSampled;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = mMarch.init("volumetric march", MARCH_FRAGMENT, MARCH_IMAGES) &&
                     mComposite.init("volumetric composite", COMPOSITE_FRAGMENT, COMPOSITE_IMAGES, LLVKFullscreen::Blend::PremultipliedOver) &&
                     mBlur.init("volumetric blur", BLUR_FRAGMENT, BLUR_IMAGES);
            if (!mReady)
            {
                LL_WARNS("VKVolumetric") << "Volumetric light could not be set up; it stays off" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::march(const LLRHI::VolumetricMarch& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.depth_texture || !pass.depth_width || !pass.depth_height || !pass.steps)
        {
            return false;
        }
        mSampled.assign({ pass.depth_texture, pass.shadow_textures[0], pass.shadow_textures[1], pass.shadow_textures[2],
                          pass.shadow_textures[3], pass.clouds.map_texture });     // 0 (no clouds' shadow): skipped
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }

        MarchUniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.shadow_matrices, pass.shadow_matrices, sizeof(uniforms.shadow_matrices));
        memcpy(uniforms.shadow_far, pass.shadow_far, sizeof(uniforms.shadow_far));
        memcpy(uniforms.light_direction, pass.light_direction, sizeof(pass.light_direction));
        memcpy(uniforms.light_color, pass.light_color, sizeof(pass.light_color));
        for (U32 i = 0; i < 3; ++i)
        {
            uniforms.mist_up[i] = pass.up_eye[i];
        }
        uniforms.mist_up[3] = pass.mist_camera_height;
        uniforms.mist[0] = llmax(pass.mist_inverse_height, 0.f);
        uniforms.mist[1] = llmax(pass.mist_scatter, 0.f);
        uniforms.mist[2] = llmax(pass.mist_extinction, 0.f);
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        shadowDepthStored(uniforms.shadow_depth);
        uniforms.depth_size[0] = (S32)pass.depth_width;
        uniforms.depth_size[1] = (S32)pass.depth_height;
        uniforms.scale = (S32)llmax(1u, pass.scale);
        uniforms.steps = (S32)llmin(pass.steps, MAX_STEPS);
        uniforms.extinction = pass.extinction;
        uniforms.anisotropy = pass.anisotropy;
        uniforms.max_length = llmax(pass.max_length, 0.f);
        cloudUniforms(pass.clouds, uniforms);
        mMarchShares = uniforms.cloud_light[2] > 0.f;

        LLVKFullscreen::Image images[MARCH_IMAGES];
        images[0].texture = pass.depth_texture;
        for (U32 i = 0; i < SHADOW_CASCADES; ++i)
        {
            images[i + 1].texture = pass.shadow_textures[i];
        }
        images[SHADOW_CASCADES + 1].texture = pass.clouds.map_texture;    // 0: the tracker's stand-in, never read
        // A color target of its own (the scene depth is sampled, never attached).
        const bool drawn = !target.depth && mMarch.draw(cmd, target, &uniforms, sizeof(uniforms), images, MARCH_IMAGES);
        LLVKTracker::endPass();
        if (drawn && !mLoggedFirst)
        {
            mLoggedFirst = true;
            LL_INFOS("VKVolumetric") << "Volumetric light: march " << target.width << "x" << target.height << " (1/" << uniforms.scale
                                     << " of " << pass.depth_width << "x" << pass.depth_height << "), up to " << uniforms.steps
                                     << " samples a ray" << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::composite(const LLRHI::VolumetricComposite& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.scatter_texture || !pass.depth_texture || !pass.scatter_width || !pass.scatter_height)
        {
            return false;
        }
        mSampled.assign({ pass.scatter_texture, pass.depth_texture });
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }

        CompositeUniforms uniforms = {};
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.depth_a = pass.depth_a;
        uniforms.depth_b = pass.depth_b;
        uniforms.scatter_size[0] = (S32)pass.scatter_width;
        uniforms.scatter_size[1] = (S32)pass.scatter_height;
        uniforms.scale = (S32)llmax(1u, pass.scale);
        uniforms.march_far = llmax(pass.march_far, 0.f);
        uniforms.ray[0] = pass.ray_scale[0];
        uniforms.ray[1] = pass.ray_scale[1];
        uniforms.ray[2] = pass.ray_offset[0];
        uniforms.ray[3] = pass.ray_offset[1];
        for (U32 c = 0; c < 3; ++c)
        {
            uniforms.air[c] = llmax(pass.air_ambient[c], 0.f);
        }
        uniforms.air[3] = llmax(pass.extinction, 0.f);
        uniforms.march_end[0] = llmax(pass.shadow_end, 0.f);
        uniforms.march_end[1] = llmax(pass.max_length, 0.f);
        cloudLightUniforms(pass.clouds, mMarchShares, uniforms);

        // The scene depth is usually the target's own: then it is attached read-only
        // and sampled in that layout (LLVKTracker::beginPass).
        LLVKFullscreen::Image images[COMPOSITE_IMAGES];
        images[0] = { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1].texture = pass.scatter_texture;
        const bool drawn = mComposite.draw(cmd, target, &uniforms, sizeof(uniforms), images, COMPOSITE_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::blur(const LLRHI::VolumetricBlur& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.source_texture || !pass.width || !pass.height)
        {
            return false;
        }
        mSampled.assign({ pass.source_texture });
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }
        BlurUniforms uniforms = {};
        uniforms.size[0] = (S32)pass.width;
        uniforms.size[1] = (S32)pass.height;
        uniforms.direction[0] = pass.direction[0];
        uniforms.direction[1] = pass.direction[1];
        LLVKFullscreen::Image images[BLUR_IMAGES];
        images[0].texture = pass.source_texture;
        // A color target of its own, the source's size.
        const bool drawn = !target.depth && mBlur.draw(cmd, target, &uniforms, sizeof(uniforms), images, BLUR_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool record_blur(const LLRHI::VolumetricBlur& pass)
    {
        return sRenderer.blur(pass);
    }

    bool record_march(const LLRHI::VolumetricMarch& pass)
    {
        return sRenderer.march(pass);
    }

    bool record_composite(const LLRHI::VolumetricComposite& pass)
    {
        return sRenderer.composite(pass);
    }
}

void LLVKVolumetric::install()
{
    if (!LLRHI::sVolumetric)
    {
        LL_INFOS("VKVolumetric") << "Volumetric light off (LLRHI_VOLUMETRIC=0)" << LL_ENDL;
        return;
    }
    LLRHI::sVolumetricMarch = &record_march;
    LLRHI::sVolumetricComposite = &record_composite;
    LLRHI::sVolumetricBlur = &record_blur;
    LLVKShader::addBakeStep("volumetric light", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
