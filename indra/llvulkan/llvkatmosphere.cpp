/**
 * @file llvkatmosphere.cpp
 * @brief Ground fog and fireflies recorded directly in Vulkan (research/148).
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

#include "llvkatmosphere.h"

#include "llrhiatmosphere.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Ground fog

    // Fog whose density is u.fog.x at and below the reference level and falls
    // as exp(-u.fog.y * height) above it. Its optical depth along a ray is
    // integrated in closed form from the camera (u.fog.z above the level, or
    // below it: negative) to the scene or max distance.
    const char* const FOG_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform GroundFog
{
    mat4 inverse_projection;    // clip -> eye
    mat4 eye_to_world;          // eye -> world axes (rotation only)
    vec4 ambient;               // rgb: the fog's light from the sky (scene units), a: max distance (m)
    vec4 sun;                   // rgb: the sun's light scattered by the fog (scene units), a: anisotropy
    vec4 sun_direction;         // xyz: towards the light (world)
    vec4 fog;                   // x: density at the level (1/m), y: 1/height scale (1/m), z: camera above the level (m)
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    ivec2 size;                 // the target's size
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(location = 0) out vec4 frag_color;

// Henyey-Greenstein, scaled so that isotropic scattering is 1.
float phase(float cos_theta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5);
}

// The optical depth, for density 1 at the level, of a stretch of ray that
// stays above the level: it starts h0 >= 0 above it, climbs rz per metre of
// ray, and is len long.
float depthAbove(float h0, float rz, float len)
{
    float k = u.fog.y;
    float span = k * rz * len;
    if (abs(span) < 1e-4)
    {
        return exp(-k * h0) * len;
    }
    return (exp(-k * h0) - exp(-k * h0 - span)) / (k * rz);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 ndc = (vec2(pixel) + 0.5) / vec2(u.size) * 2.0 - 1.0;
    float depth = texelFetch(scene_depth, pixel, 0).r;
    vec4 near_eye = u.inverse_projection * vec4(ndc, -1.0, 1.0);
    vec3 dir_eye = normalize(near_eye.xyz / near_eye.w);
    float max_distance = u.ambient.a;
    float dist = max_distance;
    if (depth < 1.0)
    {
        vec4 eye = u.inverse_projection * vec4(ndc, depth * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
        dist = min(length(eye.xyz / eye.w), max_distance);
    }
    vec3 dir = mat3(u.eye_to_world) * dir_eye;
    float rz = dir.z;
    float h0 = u.fog.z;

    // Above the level the density falls off; at and below it, it is the level's.
    float relative;
    if (rz < 0.0)
    {
        float cross_at = h0 > 0.0 ? h0 / -rz : 0.0;
        float above = min(cross_at, dist);
        relative = (h0 > 0.0 ? depthAbove(h0, rz, above) : 0.0) + max(dist - above, 0.0);
    }
    else
    {
        float cross_at = h0 < 0.0 ? -h0 / rz : 0.0;
        float below = min(cross_at, dist);
        relative = below + (dist > below ? depthAbove(max(h0, 0.0), rz, dist - below) : 0.0);
    }
    float transmittance = exp(-u.fog.x * relative);

    vec3 light = u.ambient.rgb + u.sun.rgb * phase(dot(dir, u.sun_direction.xyz), u.sun.a);
    float cover = 1.0 - transmittance;
    frag_color = vec4(light * cover, cover);
}
)GLSL";

    struct FogUniforms
    {
        F32 inverse_projection[16];
        F32 eye_to_world[16];
        F32 ambient[4];
        F32 sun[4];
        F32 sun_direction[4];
        F32 fog[4];
        F32 depth_to_ndc[2];
        S32 size[2];
    };
    static_assert(sizeof(FogUniforms) == 208, "FogUniforms must match the std140 block GroundFog");

    constexpr U32 FOG_IMAGES = 1;       // the scene depth

    // ------------------------------------------------------------------
    // Fireflies (llrhiatmosphere.h Fireflies)

    const char* const FIREFLY_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Fireflies
{
    mat4 inverse_projection;    // clip -> eye
    mat4 eye_to_world;          // eye -> world axes (rotation only)
    vec4 layer;                 // x: bottom, y: top (m above the camera), z: cell size (m), w: range (m)
    vec4 light;                 // rgb: the point's peak (exposed units), a: the chance a cell holds a firefly
    vec4 shape;                 // x: core radius (m), y: glow radius (m), z: glow share, w: pixel angle (rad)
    vec4 clock;                 // x: time (s), y: exposure scale (0: no exposure, the map not read), zw: the camera within its cell (m)
    uvec4 base;                 // xy: the camera's cell
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    ivec2 size;                 // the target's size
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D exposure_map;

layout(location = 0) out vec4 frag_color;

// Paths and blinks repeat every PERIOD / k seconds (llrhiatmosphere.h FIREFLY_TIME_PERIOD).
const float PERIOD = 64.0;
const float TWO_PI = 6.28318531;
// At most this many cells are tried along a ray (range / cell size, both ways round).
const int MAX_CELLS = 40;
// The glow is cut off smoothly from 60% of this many radii out to it: inside its cell.
const float GLOW_REACH = 3.0;
// A firefly between blinks has this share of its light; it is bright for this
// share of each blink cycle.
const float BETWEEN_BLINKS = 0.12;
const float BLINK_ON = 0.4;

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

// The light of the firefly in a cell (counted in cells from the camera's) at a
// pixel whose ray is dir, the scene scene_dist away.
float fireflyAt(ivec2 cell, vec3 dir, float scene_dist)
{
    uvec2 id = u.base.xy + uvec2(cell);
    if (random(id, 1u) >= u.light.a)
    {
        return 0.0;
    }
    // It wanders around the middle of its cell and layer, as far as keeps its glow inside.
    float reach = GLOW_REACH * u.shape.y;
    float across = max(0.5 * u.layer.z - reach, 0.0);
    float up = max(0.5 * (u.layer.y - u.layer.x) - reach, 0.0);
    vec3 phase = vec3(random(id, 2u), random(id, 3u), random(id, 4u)) * TWO_PI;
    vec3 rate = vec3(float(3u + hash(id, 5u) % 7u), float(3u + hash(id, 8u) % 7u), float(3u + hash(id, 9u) % 7u));
    vec3 angle = TWO_PI * rate * (u.clock.x / PERIOD) + phase;
    vec2 centre = (vec2(cell) + 0.5) * u.layer.z - u.clock.zw;
    vec3 pos = vec3(centre, 0.5 * (u.layer.x + u.layer.y)) + vec3(across * sin(angle.x), across * sin(angle.y), up * sin(angle.z));

    float t = dot(pos, dir);
    if (t <= 0.0 || t >= scene_dist)
    {
        return 0.0;
    }
    float dist = length(pos - dir * t);
    // Never smaller than a pixel: then as much fainter (the light is kept).
    float pixel = t * u.shape.w;
    float core = max(u.shape.x, 0.7 * pixel);
    float glow = max(u.shape.y, 1.5 * pixel);
    float point = exp(-0.5 * dist * dist / (core * core)) * (u.shape.x * u.shape.x) / (core * core);
    float halo = u.shape.z * exp(-0.5 * dist * dist / (glow * glow)) * (u.shape.y * u.shape.y) / (glow * glow)
               * (1.0 - smoothstep(0.6 * reach, reach, dist));

    float on = fract(u.clock.x * float(6u + hash(id, 6u) % 9u) / PERIOD + random(id, 7u)) / BLINK_ON;
    float pulse = on < 1.0 ? sin(3.14159265 * on) : 0.0;
    float blink = mix(BETWEEN_BLINKS, 1.0, pulse * pulse);
    float fade = 1.0 - smoothstep(0.6 * u.layer.w, u.layer.w, t);
    return (point + halo) * blink * fade;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 ndc = (vec2(pixel) + 0.5) / vec2(u.size) * 2.0 - 1.0;
    float depth = texelFetch(scene_depth, pixel, 0).r;
    vec4 near_eye = u.inverse_projection * vec4(ndc, -1.0, 1.0);
    vec3 dir = normalize(mat3(u.eye_to_world) * (near_eye.xyz / near_eye.w));
    float scene_dist = 1e9;
    if (depth < 1.0)
    {
        vec4 eye = u.inverse_projection * vec4(ndc, depth * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
        scene_dist = length(eye.xyz / eye.w);
    }
    frag_color = vec4(0.0);

    // The part of the ray in the layer, within range and nearer than the scene.
    float t0 = 0.0;
    float t1 = min(u.layer.w, scene_dist);
    if (abs(dir.z) < 1e-5)
    {
        if (u.layer.x > 0.0 || u.layer.y < 0.0)
        {
            return;
        }
    }
    else
    {
        float ta = u.layer.x / dir.z;
        float tb = u.layer.y / dir.z;
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }
    if (t0 >= t1)
    {
        return;
    }

    // Walk the cells that part crosses (Amanatides & Woo). Positions are
    // across the layer from the camera's cell's corner.
    float cell_size = u.layer.z;
    vec2 step_sign = vec2(dir.x >= 0.0 ? 1.0 : -1.0, dir.y >= 0.0 ? 1.0 : -1.0);
    vec2 speed = max(abs(dir.xy), vec2(1e-6));
    vec2 from = u.clock.zw + dir.xy * t0;
    ivec2 cell = ivec2(floor(from / cell_size));
    vec2 edge = (vec2(cell) + vec2(step_sign.x > 0.0 ? 1.0 : 0.0, step_sign.y > 0.0 ? 1.0 : 0.0)) * cell_size;
    vec2 next = t0 + max((edge - from) * step_sign, vec2(0.0)) / speed;
    vec2 per_cell = cell_size / speed;
    ivec2 step_cell = ivec2(step_sign);

    float lit = 0.0;
    for (int i = 0; i < MAX_CELLS; ++i)
    {
        lit += fireflyAt(cell, dir, scene_dist);
        if (min(next.x, next.y) >= t1)
        {
            break;
        }
        if (next.x < next.y)
        {
            cell.x += step_cell.x;
            next.x += per_cell.x;
        }
        else
        {
            cell.y += step_cell.y;
            next.y += per_cell.y;
        }
    }
    float exposure = u.clock.y > 0.0 ? max(texelFetch(exposure_map, ivec2(0), 0).r * u.clock.y, 1e-4) : 1.0;
    frag_color = vec4(u.light.rgb * lit / exposure, 0.0);
}
)GLSL";

    struct FireflyUniforms
    {
        F32 inverse_projection[16];
        F32 eye_to_world[16];
        F32 layer[4];
        F32 light[4];
        F32 shape[4];
        F32 clock[4];
        U32 base[4];
        F32 depth_to_ndc[2];
        S32 size[2];
    };
    static_assert(sizeof(FireflyUniforms) == 224, "FireflyUniforms must match the std140 block Fireflies");

    constexpr U32 FIREFLY_IMAGES = 2;   // the scene depth, the exposure
    static_assert(LLRHI::FIREFLY_TIME_PERIOD == 64.f, "FIREFLY_FRAGMENT's PERIOD");

    // One vec4 of a block.
    void row(F32 out[4], F32 x, F32 y, F32 z, F32 w)
    {
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool groundFog(const LLRHI::GroundFog& pass);
        bool fireflies(const LLRHI::Fireflies& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { const bool fog = fogReady(); return firefliesReady() && fog; }

    private:
        bool fogReady();
        bool firefliesReady();

        bool mFogTried = false;
        bool mFogReady = false;
        LLVKFullscreen::Pass mFog;
        bool mFogLogged = false;
        bool mFirefliesTried = false;
        bool mFirefliesReady = false;
        LLVKFullscreen::Pass mFireflies;
        bool mFirefliesLogged = false;
        // The textures a pass samples: kept so that no frame allocates them anew.
        std::vector<LLVKTracker::GLName> mSampled;
    };

    Renderer sRenderer;

    bool Renderer::fogReady()
    {
        if (!mFogTried)
        {
            mFogTried = true;
            mFogReady = mFog.init("ground fog", FOG_FRAGMENT, FOG_IMAGES, LLVKFullscreen::Blend::PremultipliedOver);
            if (!mFogReady)
            {
                LL_WARNS("VKAtmosphere") << "The ground fog pass could not be set up; it stays off" << LL_ENDL;
            }
        }
        return mFogReady;
    }

    bool Renderer::firefliesReady()
    {
        if (!mFirefliesTried)
        {
            mFirefliesTried = true;
            mFirefliesReady = mFireflies.init("fireflies", FIREFLY_FRAGMENT, FIREFLY_IMAGES, LLVKFullscreen::Blend::Add);
            if (!mFirefliesReady)
            {
                LL_WARNS("VKAtmosphere") << "The fireflies pass could not be set up; it stays off" << LL_ENDL;
            }
        }
        return mFirefliesReady;
    }

    bool Renderer::groundFog(const LLRHI::GroundFog& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!fogReady() || !pass.depth_texture || !pass.width || !pass.height)
        {
            return false;
        }
        mSampled.assign({ pass.depth_texture });
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != pass.width || target.height != pass.height)
        {
            LLVKTracker::endPass();
            return false;
        }

        FogUniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.eye_to_world, pass.eye_to_world, sizeof(uniforms.eye_to_world));
        row(uniforms.ambient, pass.ambient[0], pass.ambient[1], pass.ambient[2], llmax(pass.max_distance, 1.f));
        row(uniforms.sun, pass.sun[0], pass.sun[1], pass.sun[2], llclamp(pass.anisotropy, 0.f, 0.95f));
        row(uniforms.sun_direction, pass.sun_direction[0], pass.sun_direction[1], pass.sun_direction[2], 0.f);
        row(uniforms.fog, llmax(pass.density, 0.f), llmax(pass.inverse_height, 1e-3f), pass.camera_height, 0.f);
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;

        // The scene depth is the target's own (shared): attached read-only and sampled in that layout.
        LLVKFullscreen::Image images[FOG_IMAGES];
        images[0] = { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const bool drawn = mFog.draw(cmd, target, &uniforms, sizeof(uniforms), images, FOG_IMAGES);
        LLVKTracker::endPass();
        if (drawn && !mFogLogged)
        {
            mFogLogged = true;
            LL_INFOS("VKAtmosphere") << "Ground fog: " << target.width << "x" << target.height << ", colour format "
                                     << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::fireflies(const LLRHI::Fireflies& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!firefliesReady() || !pass.depth_texture || !pass.exposure_texture || !pass.width || !pass.height || pass.cell_size <= 0.f)
        {
            return false;
        }
        mSampled.assign({ pass.depth_texture, pass.exposure_texture });
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != pass.width || target.height != pass.height)
        {
            LLVKTracker::endPass();
            return false;
        }

        FireflyUniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.eye_to_world, pass.eye_to_world, sizeof(uniforms.eye_to_world));
        row(uniforms.layer, pass.layer_bottom, llmax(pass.layer_top, pass.layer_bottom), pass.cell_size, llmax(pass.range, 1.f));
        row(uniforms.light, pass.color[0], pass.color[1], pass.color[2], llclamp(pass.probability, 0.f, 1.f));
        row(uniforms.shape, llmax(pass.core_radius, 1e-3f), llmax(pass.glow_radius, 1e-3f), llmax(pass.glow_share, 0.f),
            llmax(pass.pixel_angle, 0.f));
        row(uniforms.clock, pass.time, pass.exposure_scale, pass.camera_in_cell[0], pass.camera_in_cell[1]);
        uniforms.base[0] = pass.cell_base[0];
        uniforms.base[1] = pass.cell_base[1];
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;

        // The scene depth is the target's own (shared): attached read-only and sampled in that layout.
        LLVKFullscreen::Image images[FIREFLY_IMAGES];
        images[0] = { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1].texture = pass.exposure_texture;
        const bool drawn = mFireflies.draw(cmd, target, &uniforms, sizeof(uniforms), images, FIREFLY_IMAGES);
        LLVKTracker::endPass();
        if (drawn && !mFirefliesLogged)
        {
            mFirefliesLogged = true;
            LL_INFOS("VKAtmosphere") << "Fireflies: " << target.width << "x" << target.height << ", colour format "
                                     << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool record_fireflies(const LLRHI::Fireflies& pass)
    {
        return sRenderer.fireflies(pass);
    }

    bool record_ground_fog(const LLRHI::GroundFog& pass)
    {
        return sRenderer.groundFog(pass);
    }
}

void LLVKAtmosphere::install()
{
    if (!LLRHI::sAtmosphere)
    {
        LL_INFOS("VKAtmosphere") << "Atmosphere effects off (LLRHI_ATMOSPHERE=0)" << LL_ENDL;
        return;
    }
    LLRHI::sGroundFog = &record_ground_fog;
    LLRHI::sFireflies = &record_fireflies;
    LLVKShader::addBakeStep("ground fog and fireflies", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
