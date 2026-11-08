/**
 * @file llvkssgi.cpp
 * @brief Screen space indirect light with a visibility bitmask, recorded
 *        directly in Vulkan (research/164).
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
 *
 * Written for this viewer from the method of O. Therrien, Y. Levesque and
 * G. Gilet, "Screen Space Indirect Lighting with Visibility Bitmask" (2023,
 * arXiv 2301.11376): no code of theirs or anyone else's is taken (see
 * newview/plus/ssgi/ORIGIN.md).
 */

#include "linden_common.h"

#include "llvkssgi.h"

#include "llrhissgi.h"
#include "llvkdevice.h"
#include "llvkfullscreen.h"
#include "llvkshader.h"
#include "llvktracker.h"

#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // ------------------------------------------------------------------
    // Shaders. Every pass starts with SHADER_COMMON: one uniform block for
    // all of them (each pass reads its own fields) and the view helpers.

    const char* const SHADER_COMMON = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Ssgi
{
    mat4 reproject;             // reproject: this frame's eye space -> the capture's clip space
    vec4 ndc_to_view;           // eye x = d * (ndc x * [0] + [2]), eye y = d * (ndc y * [1] + [3])
    vec4 projection_scale;      // [0][0], [1][1], [2][0], [2][1] of the projection
    vec2 depth_to_ndc;          // ndc z = stored depth * x + y
    vec2 depth_ab;              // view depth = y / (ndc z + x)
    ivec2 full_size;            // the scene depth's size
    ivec2 source_size;          // the reduced grid's size (GTAO's)
    ivec2 light_size;           // reproject: the capture's size
    int scale;                  // full-resolution pixels per reduced pixel, each way
    int slices;                 // occlusion: directions
    int steps;                  // occlusion: samples on each side of a direction
    float occlusion_radius;     // occlusion: m, how far the ambient occlusion reaches
    float light_radius;         // occlusion: m, how far the indirect light reaches
    float max_screen_radius;    // occlusion: reduced pixels
    float thickness;            // occlusion: m
    float light_cap;            // capture: the brightest channel at most
    float disocclusion;         // reproject: relative depth difference of a point hidden then
    float edge_fade;            // reproject: share of the screen at its edges
    int flags;                  // reproject: FLAG_*
    float padding0;
    float padding1;
    float padding2;
} u;

const int FLAG_HAS_LIGHT = 1;       // reproject: the capture is the last frame's
const int FLAG_SHOW_MISSES = 2;     // reproject: red where no light was found (PlusSsgiDebug 3)

// The largest view depth the 16-bit float targets keep (with room).
const float MAX_DEPTH = 60000.0;

// A view depth the passes can work with: a positive finite number.
bool validDepth(float d)
{
    return d > 0.0 && d <= MAX_DEPTH;      // false for NaN too
}

bool finiteColor(vec3 c)
{
    return !any(isnan(c)) && !any(isinf(c));
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

// Eye-space position of a reduced pixel: it stands for the full-resolution
// pixel at the top-left of its block (GtaoDepth takes that one's depth).
vec3 reducedPosition(ivec2 texel, float d)
{
    return viewPosition(pixelNDC(vec2(texel * u.scale)), d);
}
)GLSL";

    // SsgiCapture: the lit screen at half size, the sky left out, each
    // block's light held at light_cap (hue kept), with its nearest depth.
    const char* const CAPTURE_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D scene_color;
layout(set = 0, binding = 2) uniform sampler2D scene_depth;

layout(location = 0) out vec4 frag_light;   // rgb light, a view depth

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec3 sum = vec3(0.0);
    float count = 0.0;
    float nearest = MAX_DEPTH;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 pixel = min(texel * 2 + ivec2(i & 1, i >> 1), u.full_size - 1);
        float stored = texelFetch(scene_depth, pixel, 0).r;
        float d = viewDepth(stored);
        vec3 color = texelFetch(scene_color, pixel, 0).rgb;
        // The sky (the far plane) gives no light here: the probes hold it.
        if (stored >= 1.0 || !validDepth(d) || !finiteColor(color))
        {
            continue;
        }
        color = max(color, vec3(0.0));
        float peak = max(color.r, max(color.g, color.b));
        if (peak > u.light_cap)
        {
            color *= u.light_cap / peak;
        }
        sum += color;
        count += 1.0;
        nearest = min(nearest, d);
    }
    frag_light = count > 0.0 ? vec4(sum / count, nearest) : vec4(0.0, 0.0, 0.0, MAX_DEPTH);
}
)GLSL";

    // SsgiReproject: per reduced pixel, the capture's light where the point
    // was a frame ago; nothing where it was off that screen or hidden.
    const char* const REPROJECT_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D view_depth;
layout(set = 0, binding = 2) uniform sampler2D captured_light;

layout(location = 0) out vec4 frag_light;

vec4 noLight()
{
    return (u.flags & FLAG_SHOW_MISSES) != 0 ? vec4(1.0, 0.0, 0.0, 0.0) : vec4(0.0);
}

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float d = texelFetch(view_depth, min(texel, u.source_size - 1), 0).r;
    if (!validDepth(d))
    {
        frag_light = vec4(0.0);     // the sky
        return;
    }
    if ((u.flags & FLAG_HAS_LIGHT) == 0)
    {
        frag_light = noLight();
        return;
    }
    vec4 clip = u.reproject * vec4(reducedPosition(texel, d), 1.0);
    if (clip.w <= 1e-4)
    {
        frag_light = noLight();     // behind the last frame's eye
        return;
    }
    vec2 ndc = clip.xy / clip.w;
    vec2 inside = vec2(1.0) - abs(ndc);
    if (any(lessThanEqual(inside, vec2(0.0))))
    {
        frag_light = noLight();     // off the last frame's screen
        return;
    }
    ivec2 at = clamp(ivec2((ndc * 0.5 + 0.5) * vec2(u.light_size)), ivec2(0), u.light_size - 1);
    vec4 then = texelFetch(captured_light, at, 0);
    // clip.w is the point's view depth in the capture's view (a perspective projection).
    if (!validDepth(then.a) || abs(then.a - clip.w) > u.disocclusion * clip.w)
    {
        frag_light = noLight();     // something else was in front of it then (or it was the sky)
        return;
    }
    // NDC spans 2 across the screen: edge_fade's share of it is twice that in NDC.
    float fade = clamp(min(inside.x, inside.y) / (2.0 * u.edge_fade), 0.0, 1.0);
    frag_light = vec4(then.rgb * fade, 1.0);
}
)GLSL";

    // SsgiOcclusion: the visibility bitmask. Per slice (a direction on
    // screen through the pixel), the hemisphere around the normal within the
    // slice is cut into 32 sectors of equal cosine weight. Each sample
    // covers the sectors between the directions to its front and to a point
    // thickness behind it; the sectors it covers first (none nearer covered
    // them) bring its light, from the reprojected capture. Visibility is the
    // share of sectors nothing within the occlusion radius covers.
    const char* const OCCLUSION_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D view_depth;
layout(set = 0, binding = 2) uniform sampler2D light;   // SsgiReproject's target

layout(location = 0) out vec2 frag_occlusion;   // visibility, view depth
layout(location = 1) out vec4 frag_indirect;    // irradiance, view depth

const float PI = 3.14159265;
const float HALF_PI = 1.57079633;
const float SECTORS = 32.0;
const float GOLDEN_RATIO_FRACTION = 0.618033989;
// Samples crowd towards the pixel (s^2): near occluders matter most.
const float SAMPLE_SPREAD = 2.0;
// The nearest sample is at least this far (reduced pixels): not the pixel itself.
const float NEAREST_SAMPLE = 1.3;
// Below this radius on screen (reduced pixels) both fade out (far away).
const float MIN_SCREEN_RADIUS = 2.0;
const float MIN_VISIBILITY = 0.03;

// A 4x4 Bayer order in (0, 1): every 4x4 block holds each value once, so the
// denoise pass's 4x4 average takes the pattern out.
float bayer4(ivec2 p)
{
    const float ORDER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    return (ORDER[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

float depthAt(ivec2 texel)
{
    return texelFetch(view_depth, clamp(texel, ivec2(0), u.source_size - 1), 0).r;
}

// The surface normal from the depth around the pixel: along each axis, the
// neighbour nearer in depth (so an edge does not bend it). Faces the eye.
vec3 surfaceNormal(ivec2 texel, vec3 position, float d)
{
    vec3 tangents[2];
    for (int a = 0; a < 2; ++a)
    {
        ivec2 next = a == 0 ? ivec2(1, 0) : ivec2(0, 1);
        float ahead = depthAt(texel + next);
        float behind = depthAt(texel - next);
        tangents[a] = abs(ahead - d) <= abs(behind - d) ? reducedPosition(texel + next, ahead) - position
                                                        : position - reducedPosition(texel - next, behind);
    }
    vec3 normal = cross(tangents[0], tangents[1]);
    float normal_length = length(normal);
    normal = normal_length > 0.0 ? normal / normal_length : normalize(-position);
    return dot(normal, position) > 0.0 ? -normal : normal;
}

// Where a direction from the pixel falls across the slice's hemisphere, 0..1
// from its far side to the side of the slice direction: the sine of its
// angle from the normal within the slice, so equal spans hold equal shares
// of the slice's cosine-weighted light. side: +1 on the slice direction's
// side of the pixel, -1 on the other; normal_angle: the normal's angle from
// the view vector, positive towards the slice direction.
float sectorPlace(vec3 direction, vec3 view_vec, float side, float normal_angle)
{
    float angle = side * acos(clamp(dot(direction, view_vec), -1.0, 1.0));
    return 0.5 + 0.5 * sin(clamp(angle - normal_angle, -HALF_PI, HALF_PI));
}

// The sectors between two places: those the span covers at least half of.
uint sectorBits(float place0, float place1)
{
    uint first = uint(clamp(round(min(place0, place1) * SECTORS), 0.0, SECTORS));
    uint last = uint(clamp(round(max(place0, place1) * SECTORS), 0.0, SECTORS));
    uint count = last - first;
    if (count == 0u)
    {
        return 0u;
    }
    uint bits = count >= 32u ? 0xFFFFFFFFu : (1u << count) - 1u;
    return bits << first;       // first < 32 here
}

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float d = depthAt(texel);
    if (!validDepth(d))
    {
        frag_occlusion = vec2(1.0, MAX_DEPTH);
        frag_indirect = vec4(0.0, 0.0, 0.0, MAX_DEPTH);
        return;
    }
    float march_radius = max(u.occlusion_radius, u.light_radius);
    float full_radius = march_radius * u.projection_scale.y * 0.5 * float(u.source_size.y) / d;
    if (full_radius < MIN_SCREEN_RADIUS)
    {
        frag_occlusion = vec2(1.0, d);      // far away
        frag_indirect = vec4(0.0, 0.0, 0.0, d);
        return;
    }
    float screen_radius = min(full_radius, u.max_screen_radius);
    // Close up the cap on screen shortens both radii in metres alike.
    float shrink = screen_radius / full_radius;
    float occlusion_radius = u.occlusion_radius * shrink;
    float light_radius = u.light_radius * shrink;
    float reach_limit = max(occlusion_radius, light_radius);

    vec3 position = reducedPosition(texel, d);
    vec3 normal = surfaceNormal(texel, position, d);
    vec3 view_vec = normalize(-position);
    float nearest_s = NEAREST_SAMPLE / screen_radius;
    float slice_noise = bayer4(texel);
    float step_noise = bayer4(ivec2(texel.y, texel.x + 2));

    float visibility_sum = 0.0;
    vec3 light_sum = vec3(0.0);
    float weight_sum = 0.0;
    for (int slice = 0; slice < u.slices; ++slice)
    {
        float phi = (float(slice) + slice_noise) * PI / float(u.slices);
        vec2 omega = vec2(cos(phi), sin(phi));
        // The slice's plane holds the view vector and this direction on screen.
        vec3 on_screen = vec3(omega, 0.0);
        vec3 tangent = normalize(on_screen - dot(on_screen, view_vec) * view_vec);
        vec3 axis = cross(tangent, view_vec);
        vec3 projected = normal - axis * dot(normal, axis);
        float projected_length = length(projected);
        if (projected_length < 1e-4)
        {
            continue;       // the normal is across the slice: it says nothing
        }
        float normal_angle = sign(dot(projected, tangent)) * acos(clamp(dot(projected, view_vec) / projected_length, -1.0, 1.0));

        uint occluded = 0u;     // sectors covered within the occlusion radius
        uint taken = 0u;        // sectors whose light was taken (within the light radius)
        vec3 slice_light = vec3(0.0);
        for (int i = 0; i < u.steps; ++i)
        {
            float jitter = fract(step_noise + float(slice + i * u.slices) * GOLDEN_RATIO_FRACTION);
            float s = pow((float(i) + jitter) / float(u.steps), SAMPLE_SPREAD) + nearest_s;
            ivec2 offset = ivec2(round(s * omega * screen_radius));
            for (int k = 0; k < 2; ++k)
            {
                float side = k == 0 ? 1.0 : -1.0;
                ivec2 at = k == 0 ? texel + offset : texel - offset;
                if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, u.source_size)))
                {
                    continue;       // off the screen: nothing known there
                }
                float sample_depth = texelFetch(view_depth, at, 0).r;
                if (!validDepth(sample_depth))
                {
                    continue;       // the sky: the probes' light, nothing in the way
                }
                vec3 sample_position = reducedPosition(at, sample_depth);
                vec3 front = sample_position - position;
                float reach = length(front);
                if (reach > reach_limit || reach < 1e-4)
                {
                    continue;
                }
                // The surface is taken to go on thickness behind its depth, along its view ray.
                vec3 back = front + normalize(sample_position) * u.thickness;
                uint bits = sectorBits(sectorPlace(front / reach, view_vec, side, normal_angle),
                                       sectorPlace(normalize(back), view_vec, side, normal_angle));
                if (reach <= occlusion_radius)
                {
                    occluded |= bits;
                }
                if (reach <= light_radius)
                {
                    uint fresh = bits & ~taken;
                    if (fresh != 0u)
                    {
                        slice_light += texelFetch(light, at, 0).rgb * (float(bitCount(fresh)) / SECTORS);
                        taken |= bits;
                    }
                }
            }
        }
        // A slice counts by how much of the normal lies in it.
        visibility_sum += projected_length * (1.0 - float(bitCount(occluded)) / SECTORS);
        light_sum += projected_length * slice_light;
        weight_sum += projected_length;
    }
    float visibility = weight_sum > 0.0 ? visibility_sum / weight_sum : 1.0;
    vec3 indirect = weight_sum > 0.0 ? light_sum / weight_sum : vec3(0.0);
    // Far away, where the radius covers few pixels, both fade out.
    float presence = clamp(screen_radius / MIN_SCREEN_RADIUS - 1.0, 0.0, 1.0);
    visibility = mix(1.0, visibility, presence);
    indirect *= presence;
    frag_occlusion = vec2(isnan(visibility) ? 1.0 : clamp(visibility, MIN_VISIBILITY, 1.0), d);
    frag_indirect = vec4(finiteColor(indirect) ? max(indirect, vec3(0.0)) : vec3(0.0), d);
}
)GLSL";

    // SsgiDenoise: the average of the 4x4 block around each pixel, over the
    // pixels near it in depth (an edge keeps its two sides apart), of both.
    const char* const DENOISE_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D occlusion;
layout(set = 0, binding = 2) uniform sampler2D indirect;

layout(location = 0) out vec2 frag_occlusion;   // visibility, view depth
layout(location = 1) out vec4 frag_indirect;    // irradiance, view depth

// Relative view depth difference at which a neighbour no longer counts.
const float DEPTH_TOLERANCE = 0.1;

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float d = texelFetch(occlusion, texel, 0).g;
    if (!validDepth(d))
    {
        frag_occlusion = vec2(1.0, MAX_DEPTH);
        frag_indirect = vec4(0.0, 0.0, 0.0, MAX_DEPTH);
        return;
    }
    float visibility = 0.0;
    vec3 light = vec3(0.0);
    float weight_sum = 0.0;
    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            ivec2 at = clamp(texel + ivec2(x, y), ivec2(0), u.source_size - 1);
            vec2 tap = texelFetch(occlusion, at, 0).rg;
            float weight = max(0.0, 1.0 - abs(tap.g - d) / (d * DEPTH_TOLERANCE));
            visibility += tap.r * weight;
            light += texelFetch(indirect, at, 0).rgb * weight;
            weight_sum += weight;
        }
    }
    // The pixel itself always counts (weight 1).
    visibility /= max(weight_sum, 1e-4);
    light /= max(weight_sum, 1e-4);
    frag_occlusion = vec2(isnan(visibility) ? 1.0 : clamp(visibility, 0.0, 1.0), d);
    frag_indirect = vec4(finiteColor(light) ? max(light, vec3(0.0)) : vec3(0.0), d);
}
)GLSL";

    // SsgiDebugView: the source over the lit screen's opaque pixels (the
    // nearest reduced pixel); the sky is left as it is.
    const char* const DEBUG_FRAGMENT = R"GLSL(
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D source;

layout(location = 0) out vec4 frag_color;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (texelFetch(scene_depth, min(pixel, u.full_size - 1), 0).r >= 1.0)
    {
        discard;
    }
    ivec2 at = min(pixel / u.scale, u.source_size - 1);
    frag_color = vec4(texelFetch(source, at, 0).rgb, 1.0);
}
)GLSL";

    // ------------------------------------------------------------------
    // The uniform block Ssgi, as std140 lays it out.

    struct Uniforms
    {
        F32 reproject[16];
        F32 ndc_to_view[4];
        F32 projection_scale[4];
        F32 depth_to_ndc[2];
        F32 depth_ab[2];
        S32 full_size[2];
        S32 source_size[2];
        S32 light_size[2];
        S32 scale;
        S32 slices;
        S32 steps;
        F32 occlusion_radius;
        F32 light_radius;
        F32 max_screen_radius;
        F32 thickness;
        F32 light_cap;
        F32 disocclusion;
        F32 edge_fade;
        S32 flags;
        F32 padding[3];
    };
    static_assert(sizeof(Uniforms) == 192, "Uniforms must match the std140 block Ssgi");

    constexpr S32 FLAG_HAS_LIGHT = 1;
    constexpr S32 FLAG_SHOW_MISSES = 2;

    constexpr U32 MAX_SLICES = 8;
    constexpr U32 MAX_STEPS = 32;

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

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool capture(const LLRHI::SsgiCapture& pass);
        bool reproject(const LLRHI::SsgiReproject& pass);
        bool occlusion(const LLRHI::SsgiOcclusion& pass);
        bool denoise(const LLRHI::SsgiDenoise& pass);
        bool debugView(const LLRHI::SsgiDebugView& pass);
        bool compactLight();

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();
        // Opens the pass on the bound target, draws it and closes it. The
        // target must be width x height: anything else is not the target the
        // pass was meant for, and it is left alone. scene_depth_image: the
        // index in images of the scene depth, which may be the bound target's
        // own (its layout is then the read-only attachment's); -1 for none.
        bool record(LLVKFullscreen::Pass& pass, U32 width, U32 height, std::initializer_list<LLVKTracker::GLName> sampled,
                    Uniforms& uniforms, LLVKFullscreen::Image* images, U32 image_count, S32 scene_depth_image);

        bool mTried = false;
        bool mReady = false;
        S32 mCompactLight = -1;     // -1 not asked yet
        LLVKFullscreen::Pass mCapture;
        LLVKFullscreen::Pass mReproject;
        LLVKFullscreen::Pass mOcclusion;
        LLVKFullscreen::Pass mDenoise;
        LLVKFullscreen::Pass mDebugView;
        std::vector<LLVKTracker::GLName> mSampled;     // record()'s, kept: no allocation each pass
        bool mLoggedOcclusion = false;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            const std::string common = SHADER_COMMON;
            const VkColorComponentFlags two_targets[2] = { LLVKFullscreen::WRITE_RGBA, LLVKFullscreen::WRITE_RGBA };
            mReady = mCapture.init("ssgi capture", (common + CAPTURE_FRAGMENT).c_str(), 2) &&
                     mReproject.init("ssgi reproject", (common + REPROJECT_FRAGMENT).c_str(), 2) &&
                     mOcclusion.initColors("ssgi occlusion", (common + OCCLUSION_FRAGMENT).c_str(), 2, 2, two_targets) &&
                     mDenoise.initColors("ssgi denoise", (common + DENOISE_FRAGMENT).c_str(), 2, 2, two_targets) &&
                     mDebugView.init("ssgi debug view", (common + DEBUG_FRAGMENT).c_str(), 2, LLVKFullscreen::Blend::Replace,
                                     VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT);
            if (!mReady)
            {
                LL_WARNS("VKSsgi") << "Screen space indirect light could not be set up; it stays off" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::record(LLVKFullscreen::Pass& pass, U32 width, U32 height, std::initializer_list<LLVKTracker::GLName> sampled,
                          Uniforms& uniforms, LLVKFullscreen::Image* images, U32 image_count, S32 scene_depth_image)
    {
        mSampled.assign(sampled);
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != width || target.height != height)
        {
            LL_WARNS_ONCE("VKSsgi") << pass.name() << ": the bound target is " << target.width << "x" << target.height << ", not "
                                    << width << "x" << height << "; not drawn" << LL_ENDL;
            LLVKTracker::endPass();
            return false;
        }
        if (scene_depth_image >= 0)
        {
            images[scene_depth_image].layout = target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        const bool drawn = pass.draw(cmd, target, &uniforms, sizeof(uniforms), images, image_count);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::capture(const LLRHI::SsgiCapture& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !viewUniforms(pass.view, uniforms) || !pass.color_texture || !pass.width || !pass.height ||
            !(pass.light_cap > 0.f))
        {
            return false;
        }
        uniforms.light_cap = pass.light_cap;
        LLVKFullscreen::Image images[2] = { { pass.color_texture }, { pass.view.depth_texture } };
        return record(mCapture, pass.width, pass.height, { pass.color_texture, pass.view.depth_texture }, uniforms, images, 2, 1);
    }

    bool Renderer::reproject(const LLRHI::SsgiReproject& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !viewUniforms(pass.view, uniforms) || !pass.view_depth_texture || !pass.width || !pass.height ||
            !(pass.edge_fade > 0.f))
        {
            return false;
        }
        const bool has_light = pass.light_texture && pass.light_width && pass.light_height;
        memcpy(uniforms.reproject, pass.reproject, sizeof(uniforms.reproject));
        uniforms.source_size[0] = (S32)pass.width;
        uniforms.source_size[1] = (S32)pass.height;
        uniforms.light_size[0] = (S32)pass.light_width;
        uniforms.light_size[1] = (S32)pass.light_height;
        uniforms.scale = (S32)llmax(1u, pass.scale);
        uniforms.disocclusion = pass.disocclusion;
        uniforms.edge_fade = pass.edge_fade;
        uniforms.flags = (has_light ? FLAG_HAS_LIGHT : 0) | (pass.show_misses ? FLAG_SHOW_MISSES : 0);
        // Without a capture the view depth stands in for it (bound, never read).
        const LLVKTracker::GLName light = has_light ? pass.light_texture : pass.view_depth_texture;
        LLVKFullscreen::Image images[2] = { { pass.view_depth_texture }, { light } };
        return record(mReproject, pass.width, pass.height, { pass.view_depth_texture, light }, uniforms, images, 2, -1);
    }

    bool Renderer::occlusion(const LLRHI::SsgiOcclusion& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const LLRHI::GtaoOcclusion& o = pass.occlusion;
        Uniforms uniforms;
        if (!ready() || !viewUniforms(o.view, uniforms) || !o.view_depth_texture || !pass.light_texture || !o.width || !o.height ||
            !o.slices || !o.steps || !(o.radius > 0.f) || !(pass.light_radius > 0.f) || !(pass.thickness >= 0.f))
        {
            return false;
        }
        uniforms.source_size[0] = (S32)o.width;
        uniforms.source_size[1] = (S32)o.height;
        uniforms.scale = (S32)llmax(1u, o.scale);
        uniforms.slices = (S32)llmin(o.slices, MAX_SLICES);
        uniforms.steps = (S32)llmin(o.steps, MAX_STEPS);
        uniforms.occlusion_radius = o.radius;
        uniforms.light_radius = pass.light_radius;
        uniforms.max_screen_radius = o.max_screen_radius;
        uniforms.thickness = pass.thickness;
        LLVKFullscreen::Image images[2] = { { o.view_depth_texture }, { pass.light_texture } };
        const bool drawn = record(mOcclusion, o.width, o.height, { o.view_depth_texture, pass.light_texture }, uniforms, images, 2, -1);
        if (drawn && !mLoggedOcclusion)
        {
            mLoggedOcclusion = true;
            LL_INFOS("VKSsgi") << "SSGI: " << o.width << "x" << o.height << " (1/" << uniforms.scale << "), " << uniforms.slices
                               << " slices x " << uniforms.steps << " steps a side, occlusion " << o.radius << " m, light "
                               << pass.light_radius << " m, thickness " << pass.thickness << " m" << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::denoise(const LLRHI::SsgiDenoise& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.occlusion_texture || !pass.indirect_texture || !pass.width || !pass.height)
        {
            return false;
        }
        Uniforms uniforms = {};
        uniforms.source_size[0] = (S32)pass.width;
        uniforms.source_size[1] = (S32)pass.height;
        LLVKFullscreen::Image images[2] = { { pass.occlusion_texture }, { pass.indirect_texture } };
        return record(mDenoise, pass.width, pass.height, { pass.occlusion_texture, pass.indirect_texture }, uniforms, images, 2, -1);
    }

    bool Renderer::debugView(const LLRHI::SsgiDebugView& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Uniforms uniforms;
        if (!ready() || !viewUniforms(pass.view, uniforms) || !pass.source_texture || !pass.width || !pass.height)
        {
            return false;
        }
        uniforms.source_size[0] = (S32)pass.width;
        uniforms.source_size[1] = (S32)pass.height;
        uniforms.scale = (S32)llmax(1u, pass.scale);
        LLVKFullscreen::Image images[2] = { { pass.view.depth_texture }, { pass.source_texture } };
        return record(mDebugView, pass.view.width, pass.view.height, { pass.view.depth_texture, pass.source_texture }, uniforms,
                      images, 2, 0);
    }

    bool Renderer::compactLight()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (mCompactLight < 0)
        {
            // Sampled and drawn into: not required of B10G11R11 by Vulkan (research/164 R9).
            const VkFormatFeatureFlags wanted = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            VkFormatProperties properties = {};
            vkGetPhysicalDeviceFormatProperties(LLVKDevice::instance().getPhysicalDevice(), VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                                &properties);
            mCompactLight = (properties.optimalTilingFeatures & wanted) == wanted ? 1 : 0;
            LL_INFOS("VKSsgi") << "SSGI: the reprojected light in "
                               << (mCompactLight ? "B10G11R11 (32 bits a pixel)" : "RGBA16F (B10G11R11 cannot be drawn into here)")
                               << LL_ENDL;
        }
        return mCompactLight > 0;
    }

    bool record_capture(const LLRHI::SsgiCapture& pass)
    {
        return sRenderer.capture(pass);
    }

    bool record_reproject(const LLRHI::SsgiReproject& pass)
    {
        return sRenderer.reproject(pass);
    }

    bool record_occlusion(const LLRHI::SsgiOcclusion& pass)
    {
        return sRenderer.occlusion(pass);
    }

    bool record_denoise(const LLRHI::SsgiDenoise& pass)
    {
        return sRenderer.denoise(pass);
    }

    bool record_debug_view(const LLRHI::SsgiDebugView& pass)
    {
        return sRenderer.debugView(pass);
    }

    bool compact_light()
    {
        return sRenderer.compactLight();
    }
}

void LLVKSsgi::install()
{
    if (!LLRHI::sSsgi)
    {
        LL_INFOS("VKSsgi") << "Screen space indirect light off (LLRHI_SSGI=0)" << LL_ENDL;
        return;
    }
    LLRHI::sSsgiCapture = &record_capture;
    LLRHI::sSsgiReproject = &record_reproject;
    LLRHI::sSsgiOcclusion = &record_occlusion;
    LLRHI::sSsgiDenoise = &record_denoise;
    LLRHI::sSsgiDebugView = &record_debug_view;
    LLRHI::sSsgiCompactLight = &compact_light;
    LLVKShader::addBakeStep("SSGI", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
