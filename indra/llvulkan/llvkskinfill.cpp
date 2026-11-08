/**
 * @file llvkskinfill.cpp
 * @brief Skin fill (softer shadows on faces) recorded directly in Vulkan
 *        (research/110).
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

#include "llvkskinfill.h"

#include "llrhiskinfill.h"
#include "llrhiskinsss.h"
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

    // The mask (LLRHI::SkinFillMask): one value on every pixel the stencil
    // test lets through (the skin; drawn again for the eyes), into R.
    const char* const MASK_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SkinFillMask
{
    vec4 value;                 // x: written into R
} u;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = u.value;
}
)GLSL";

    // The shrink (LLRHI::SkinFillShrink): the share of the factor x factor
    // block of the mask this texel covers that is set (skin or eye, whatever
    // its value).
    const char* const SHRINK_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SkinFillShrink
{
    ivec2 mask_size;
    int factor;
    int padding;
} u;

layout(set = 0, binding = 1) uniform sampler2D skin_mask;

layout(location = 0) out vec4 frag_color;

void main()
{
    ivec2 origin = ivec2(gl_FragCoord.xy) * u.factor;
    float sum = 0.0;
    for (int y = 0; y < u.factor; ++y)
    {
        for (int x = 0; x < u.factor; ++x)
        {
            sum += texelFetch(skin_mask, min(origin + ivec2(x, y), u.mask_size - 1), 0).r > 0.0 ? 1.0 : 0.0;
        }
    }
    frag_color = vec4(sum / float(u.factor * u.factor));
}
)GLSL";

    // The soften (LLRHI::SkinFillSoften): a 5 x 5 binomial blur (1 4 6 4 1 each way).
    const char* const SOFTEN_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SkinFillSoften
{
    ivec2 size;                 // the shrunk mask's (and the target's)
    ivec2 padding;
} u;

layout(set = 0, binding = 1) uniform sampler2D shrunk;

layout(location = 0) out vec4 frag_color;

const float BINOMIAL[5] = float[5](1.0, 4.0, 6.0, 4.0, 1.0);

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float sum = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float weight = BINOMIAL[x + 2] * BINOMIAL[y + 2];
            sum += weight * texelFetch(shrunk, clamp(texel + ivec2(x, y), ivec2(0), u.size - 1), 0).r;
        }
    }
    frag_color = vec4(sum / 256.0);
}
)GLSL";

    // The occlusion (LLRHI::SkinFillOcclusion): on the light map, where the
    // mask is set, (0, m, 0, m) laid over it (premultiplied, G written only):
    // g becomes g * (1 - m) + m, the occlusion weakened by m: the mask's share
    // (1 on skin, less on the eyes) times the lift, faded out towards the
    // edge of the skin.
    const char* const OCCLUSION_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SkinFillOcclusion
{
    vec2 soft_uv_scale;         // full-resolution pixel -> soft mask uv
    float lift;                 // 0..1
    float padding;
} u;

layout(set = 0, binding = 1) uniform sampler2D skin_mask;
layout(set = 0, binding = 2) uniform sampler2D soft_mask;

layout(location = 0) out vec4 frag_color;

// The soft mask is about 0.5 on the edge of the skin and 1 well inside.
const float EDGE_LOW = 0.5;
const float EDGE_HIGH = 0.95;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float share = texelFetch(skin_mask, pixel, 0).r;
    float edge = smoothstep(EDGE_LOW, EDGE_HIGH, textureLod(soft_mask, (vec2(pixel) + 0.5) * u.soft_uv_scale, 0.0).r);
    float m = share * edge * u.lift;
    if (m <= 0.0)
    {
        discard;                // not skin or eye, or its very edge: the light map stays as it is
    }
    frag_color = vec4(0.0, m, 0.0, m);
}
)GLSL";

    // The fill light (LLRHI::SkinFillLight), on the skin pixels of the lit
    // HDR scene, added to it; drawn again on the eye pixels, with the light's
    // colour times the eyes' share. The G-buffer is read as softenLight (official)
    // reads it: the normal's alpha is the G-buffer flag, legacy materials
    // store sRGB base colours, PBR materials linear ones. Everything that
    // weighs the fill is continuous: the light map's r (the official sun
    // shadow, filtered and blurred), the near shadow (filtered as the official
    // shadows are, at two distances) and the fade at the skin's edge.
    const char* const LIGHT_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform SkinFillLight
{
    mat4 inverse_projection;
    mat4 shadow_matrices[3];    // eye space -> shadow map texture space
    vec4 shadow_far;            // xyz: each cascade's far end (view depth, m)
    vec4 light_direction;       // xyz: towards the light, eye space
    vec4 light_color;           // rgb: linear, times the fill's strength (grey)
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    vec2 shadow_depth;          // stored shadow depth = texture space z * x + y
    ivec2 size;                 // the target's size (the G-buffer's too)
    vec2 soft_uv_scale;         // pixel -> soft mask uv
    float directionality;       // 0..1: how much the fill favours faces turned to the camera
    float near_reach;           // m: shadows cast from nearer than this are filled
    int shadows;                // 1: the light map's r and the shadow maps are there to read
    vec4 limit;                 // x: the light's luminance, y: the ambient's, z: the share of the sunlit skin the fill stops at
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D diffuse_map;
layout(set = 0, binding = 3) uniform sampler2D normal_map;
layout(set = 0, binding = 4) uniform sampler2D soft_mask;
layout(set = 0, binding = 5) uniform sampler2D light_map;
layout(set = 0, binding = 6) uniform sampler2D shadow_map0;
layout(set = 0, binding = 7) uniform sampler2D shadow_map1;
layout(set = 0, binding = 8) uniform sampler2D shadow_map2;

layout(location = 0) out vec4 frag_color;

// The G-buffer flags (llshadermgr.cpp: GBUFFER_FLAG_*, GET_GBUFFER_FLAG).
const float FLAG_HAS_ATMOS = 0.34;
const float FLAG_HAS_PBR = 0.67;
const float FLAG_TOLERANCE = 0.1;
// A PBR material's diffuse colour is its base colour times 1 - F0 (calcDiffuseSpecular).
const float PBR_DIFFUSE = 0.96;
// Shadow depth a point may lie behind the stored depth and still be lit (the
// depth's own precision: the point is away from any surface).
const float SHADOW_BIAS = 0.0002;
// The near shadow is the average of the light at these three distances (times
// near_reach) towards the light: its edge, where the occluders end, is soft,
// and a hat's brim that reaches past the nearest of them does not cut the
// fill off in a line across the face (fix-round41).
const float NEAR_SPREAD_LOW = 0.5;
const float NEAR_SPREAD_MID = 1.0;
const float NEAR_SPREAD_HIGH = 1.5;
// The soft mask is about 0.5 on the edge of the skin and 1 well inside.
const float EDGE_LOW = 0.5;
const float EDGE_HIGH = 0.95;

// globalF.glsl's decodeNormal.
vec3 decodeNormal(vec2 encoded)
{
    vec2 fenc = encoded * 4.0 - 2.0;
    float f = dot(fenc, fenc);
    float g = sqrt(max(1.0 - f / 4.0, 0.0));
    return vec3(fenc * g, 1.0 - f / 2.0);
}

vec3 srgbToLinear(vec3 c)
{
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

// The colour the sun's light is multiplied by on this pixel; black where the
// sun does not light it (sky, HDRI, no atmospherics).
vec3 litAlbedo(vec4 base, float flag)
{
    if (abs(flag - FLAG_HAS_PBR) < FLAG_TOLERANCE)
    {
        return base.rgb * PBR_DIFFUSE;
    }
    if (abs(flag - FLAG_HAS_ATMOS) < FLAG_TOLERANCE)
    {
        // softenLight mixes the lit colour towards the unlit base colour by its alpha (fullbright).
        return srgbToLinear(base.rgb) * (1.0 - base.a);
    }
    return vec3(0.0);
}

ivec2 shadowSize(int cascade)
{
    switch (cascade)
    {
    case 0: return textureSize(shadow_map0, 0);
    case 1: return textureSize(shadow_map1, 0);
    default: return textureSize(shadow_map2, 0);
    }
}

float storedShadowDepth(int cascade, ivec2 texel)
{
    switch (cascade)
    {
    case 0: return texelFetch(shadow_map0, texel, 0).r;
    case 1: return texelFetch(shadow_map1, texel, 0).r;
    default: return texelFetch(shadow_map2, texel, 0).r;
    }
}

// The light reaching depth at texel position q (texel units, centres at
// i + 0.5): the four nearest texels compared, then weighted bilinearly, as a
// comparison sampler with linear filtering does.
float bilinearLit(int cascade, ivec2 size, vec2 q, float depth)
{
    vec2 p = q - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    ivec2 last = size - 1;
    float a = depth <= storedShadowDepth(cascade, clamp(i, ivec2(0), last)) + SHADOW_BIAS ? 1.0 : 0.0;
    float b = depth <= storedShadowDepth(cascade, clamp(i + ivec2(1, 0), ivec2(0), last)) + SHADOW_BIAS ? 1.0 : 0.0;
    float c = depth <= storedShadowDepth(cascade, clamp(i + ivec2(0, 1), ivec2(0), last)) + SHADOW_BIAS ? 1.0 : 0.0;
    float d = depth <= storedShadowDepth(cascade, clamp(i + ivec2(1, 1), ivec2(0), last)) + SHADOW_BIAS ? 1.0 : 0.0;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// How much of the light reaches pos (eye space), 0..1: a 3 x 3 texel tent
// from four bilinear compares, as the official sun shadows (shadowUtil.glsl's
// pcfShadow) filter them.
float lightVisibility(vec3 pos)
{
    float view_depth = -pos.z;
    if (view_depth >= u.shadow_far.z)
    {
        return 1.0;     // beyond the cascades read here: no shadow known
    }
    int cascade = view_depth < u.shadow_far.x ? 0 : view_depth < u.shadow_far.y ? 1 : 2;
    vec4 coord = u.shadow_matrices[cascade] * vec4(pos, 1.0);
    coord.xyz /= coord.w;
    if (any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0))))
    {
        return 1.0;     // outside the map: nothing there casts a shadow
    }
    float depth = coord.z * u.shadow_depth.x + u.shadow_depth.y;
    ivec2 size = shadowSize(cascade);
    vec2 uv = coord.xy * vec2(size);
    vec2 base = floor(uv + 0.5);
    vec2 st = uv + 0.5 - base;
    base -= 0.5;
    vec2 w0 = 3.0 - 2.0 * st;
    vec2 w1 = 1.0 + 2.0 * st;
    vec2 o0 = (2.0 - st) / w0 - 1.0;
    vec2 o1 = st / w1 + 1.0;
    float lit = w0.x * w0.y * bilinearLit(cascade, size, base + vec2(o0.x, o0.y), depth);
    lit += w1.x * w0.y * bilinearLit(cascade, size, base + vec2(o1.x, o0.y), depth);
    lit += w0.x * w1.y * bilinearLit(cascade, size, base + vec2(o0.x, o1.y), depth);
    lit += w1.x * w1.y * bilinearLit(cascade, size, base + vec2(o1.x, o1.y), depth);
    return clamp(lit / 16.0, 0.0, 1.0);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float edge = smoothstep(EDGE_LOW, EDGE_HIGH, textureLod(soft_mask, (vec2(pixel) + 0.5) * u.soft_uv_scale, 0.0).r);
    if (edge <= 0.0)
    {
        frag_color = vec4(0.0);     // the very edge of the skin, or a speck of it
        return;
    }
    float depth = texelFetch(scene_depth, pixel, 0).r;
    vec2 ndc_xy = (vec2(pixel) + 0.5) / vec2(u.size) * 2.0 - 1.0;
    vec4 eye = u.inverse_projection * vec4(ndc_xy, depth * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
    vec3 pos = eye.xyz / eye.w;

    vec4 normal_info = texelFetch(normal_map, pixel, 0);
    vec3 albedo = litAlbedo(texelFetch(diffuse_map, pixel, 0), normal_info.a);
    vec3 n = normalize(decodeNormal(normal_info.xy));
    vec3 l = u.light_direction.xyz;
    vec3 v = -normalize(pos);

    // The sun's shadow on this pixel, and how much of what casts it is near.
    float shadow = 1.0;
    float near_shadow = 1.0;
    if (u.shadows != 0)
    {
        shadow = texelFetch(light_map, pixel, 0).r;
        near_shadow = (lightVisibility(pos + l * (u.near_reach * NEAR_SPREAD_LOW)) +
                       lightVisibility(pos + l * (u.near_reach * NEAR_SPREAD_MID)) +
                       lightVisibility(pos + l * (u.near_reach * NEAR_SPREAD_HIGH))) / 3.0;
    }

    // The sun reaching the pixel now (softenLight: min(N.L, shadow)), lifted
    // towards the fill's shape, never lowered.
    float reach = min(max(dot(n, l), 0.0), shadow);
    float shape = (1.0 - u.directionality) + u.directionality * max(dot(n, v), 0.0);
    float lift = edge * near_shadow * max(shape - reach, 0.0);
    // Never past limit.z of the skin in full sun (about ambient + sun): the
    // official lighting gives this pixel about ambient + sun * reach already.
    // Under a bright sky (much ambient) the shadows are light already and the
    // fill holds back; a filled face does not come out paler than a sunlit one.
    float room = max(u.limit.z * (u.limit.x + u.limit.y) - (u.limit.y + u.limit.x * reach), 0.0);
    frag_color = vec4(albedo * min(u.light_color.rgb * lift, vec3(room)), 0.0);
}
)GLSL";

    // ------------------------------------------------------------------
    // Uniform blocks, as the shaders' std140 blocks lay them out.

    struct MaskUniforms
    {
        F32 value[4];
    };
    static_assert(sizeof(MaskUniforms) == 16, "MaskUniforms must match the std140 block SkinFillMask");

    struct ShrinkUniforms
    {
        S32 mask_size[2];
        S32 factor;
        S32 padding;
    };
    static_assert(sizeof(ShrinkUniforms) == 16, "ShrinkUniforms must match the std140 block SkinFillShrink");

    struct SoftenUniforms
    {
        S32 size[2];
        S32 padding[2];
    };
    static_assert(sizeof(SoftenUniforms) == 16, "SoftenUniforms must match the std140 block SkinFillSoften");

    struct OcclusionUniforms
    {
        F32 soft_uv_scale[2];
        F32 lift;
        F32 padding;
    };
    static_assert(sizeof(OcclusionUniforms) == 16, "OcclusionUniforms must match the std140 block SkinFillOcclusion");

    constexpr U32 CASCADES = LLRHI::SKIN_FILL_CASCADES;

    struct LightUniforms
    {
        F32 inverse_projection[16];
        F32 shadow_matrices[CASCADES][16];
        F32 shadow_far[4];
        F32 light_direction[4];
        F32 light_color[4];
        F32 depth_to_ndc[2];
        F32 shadow_depth[2];
        S32 size[2];
        F32 soft_uv_scale[2];
        F32 directionality;
        F32 near_reach;
        S32 shadows;
        S32 padding;
        F32 limit[4];
    };
    static_assert(sizeof(LightUniforms) == 368, "LightUniforms must match the std140 block SkinFillLight");
    static_assert(CASCADES == 3, "LIGHT_FRAGMENT declares three shadow maps");

    constexpr U32 SHRINK_IMAGES = 1;        // the mask
    constexpr U32 SOFTEN_IMAGES = 1;        // the shrunk mask
    constexpr U32 OCCLUSION_IMAGES = 2;     // the mask, the soft mask
    constexpr U32 LIGHT_IMAGES = 8;         // depth, diffuse, normal, soft mask, light map, three shadow maps
    constexpr U32 MAX_SHRINK_FACTOR = 16;

    // Only where the skin bit is set; only where the eye bit is set.
    constexpr LLVKFullscreen::StencilTest ON_SKIN = { true, LLRHI::STENCIL_SKIN, LLRHI::STENCIL_SKIN };
    constexpr LLVKFullscreen::StencilTest ON_EYE = { true, LLRHI::STENCIL_EYE, LLRHI::STENCIL_EYE };

    // A shadow matrix's texture space z ((clip + 1) / 2) as the shadow map
    // stores it: the same, with depth clip control (required, research/124 V1).
    void shadowDepthStored(F32 out[2])
    {
        out[0] = 1.f;
        out[1] = 0.f;
    }

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool mask(const LLRHI::SkinFillMask& pass);
        bool shrink(const LLRHI::SkinFillShrink& pass);
        bool soften(const LLRHI::SkinFillSoften& pass);
        bool occlusion(const LLRHI::SkinFillOcclusion& pass);
        bool light(const LLRHI::SkinFillLight& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        LLVKFullscreen::Pass mMask;
        LLVKFullscreen::Pass mMaskEye;
        LLVKFullscreen::Pass mShrink;
        LLVKFullscreen::Pass mSoften;
        LLVKFullscreen::Pass mOcclusion;
        LLVKFullscreen::Pass mLight;
        LLVKFullscreen::Pass mLightEye;
        bool mLoggedMask = false;
        bool mLoggedLight = false;
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = mMask.init("skin fill mask", MASK_FRAGMENT, 0, LLVKFullscreen::Blend::Replace, VK_COLOR_COMPONENT_R_BIT, ON_SKIN) &&
                     mMaskEye.init("skin fill mask eyes", MASK_FRAGMENT, 0, LLVKFullscreen::Blend::Replace, VK_COLOR_COMPONENT_R_BIT,
                                   ON_EYE) &&
                     mShrink.init("skin fill shrink", SHRINK_FRAGMENT, SHRINK_IMAGES, LLVKFullscreen::Blend::Replace,
                                  VK_COLOR_COMPONENT_R_BIT) &&
                     mSoften.init("skin fill soften", SOFTEN_FRAGMENT, SOFTEN_IMAGES, LLVKFullscreen::Blend::Replace,
                                  VK_COLOR_COMPONENT_R_BIT) &&
                     mOcclusion.init("skin fill occlusion", OCCLUSION_FRAGMENT, OCCLUSION_IMAGES, LLVKFullscreen::Blend::PremultipliedOver,
                                     VK_COLOR_COMPONENT_G_BIT) &&
                     mLight.init("skin fill light", LIGHT_FRAGMENT, LIGHT_IMAGES, LLVKFullscreen::Blend::Add, LLVKFullscreen::WRITE_RGBA,
                                 ON_SKIN) &&
                     mLightEye.init("skin fill light eyes", LIGHT_FRAGMENT, LIGHT_IMAGES, LLVKFullscreen::Blend::Add,
                                    LLVKFullscreen::WRITE_RGBA, ON_EYE);
            if (!mReady)
            {
                LL_WARNS("VKSkinFill") << "The skin fill passes could not be set up; they stay off" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::mask(const LLRHI::SkinFillMask& pass)
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
        const MaskUniforms uniforms = { { pass.value, 0.f, 0.f, 0.f } };
        bool drawn = mMask.draw(cmd, target, &uniforms, sizeof(uniforms), nullptr, 0);
        if (drawn && pass.eye_value > 0.f)
        {
            const MaskUniforms eye_uniforms = { { llmin(pass.eye_value, 1.f), 0.f, 0.f, 0.f } };
            drawn = mMaskEye.draw(cmd, target, &eye_uniforms, sizeof(eye_uniforms), nullptr, 0);
        }
        LLVKTracker::endPass();
        if (drawn && !mLoggedMask)
        {
            mLoggedMask = true;
            LL_INFOS("VKSkinFill") << "Skin fill: mask " << target.width << "x" << target.height << ", color format "
                                   << (U32)target.color_formats[0] << ", depth format " << (U32)target.depth_format << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::shrink(const LLRHI::SkinFillShrink& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.mask_texture || !pass.mask_width || !pass.mask_height)
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass({ pass.mask_texture }, target);
        if (!cmd)
        {
            return false;
        }
        const ShrinkUniforms uniforms = { { (S32)pass.mask_width, (S32)pass.mask_height },
                                          (S32)llclamp(pass.factor, 1u, MAX_SHRINK_FACTOR), 0 };
        LLVKFullscreen::Image images[SHRINK_IMAGES];
        images[0].texture = pass.mask_texture;
        const bool drawn = mShrink.draw(cmd, target, &uniforms, sizeof(uniforms), images, SHRINK_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::soften(const LLRHI::SkinFillSoften& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.shrunk_texture)
        {
            return false;
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass({ pass.shrunk_texture }, target);
        if (!cmd)
        {
            return false;
        }
        const SoftenUniforms uniforms = { { (S32)target.width, (S32)target.height }, { 0, 0 } };
        LLVKFullscreen::Image images[SOFTEN_IMAGES];
        images[0].texture = pass.shrunk_texture;
        const bool drawn = mSoften.draw(cmd, target, &uniforms, sizeof(uniforms), images, SOFTEN_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::occlusion(const LLRHI::SkinFillOcclusion& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.mask_texture)
        {
            return false;
        }
        std::vector<LLVKTracker::GLName> sampled = { pass.mask_texture };
        if (pass.soft_mask_texture)
        {
            sampled.push_back(pass.soft_mask_texture);
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(sampled, target);
        if (!cmd)
        {
            return false;
        }
        const OcclusionUniforms uniforms = { { pass.soft_uv_scale[0], pass.soft_uv_scale[1] }, llclamp(pass.lift, 0.f, 1.f), 0.f };
        LLVKFullscreen::Image images[OCCLUSION_IMAGES];
        images[0].texture = pass.mask_texture;
        images[1].texture = pass.soft_mask_texture;     // 0: a white stand-in, no fade
        const bool drawn = mOcclusion.draw(cmd, target, &uniforms, sizeof(uniforms), images, OCCLUSION_IMAGES);
        LLVKTracker::endPass();
        return drawn;
    }

    bool Renderer::light(const LLRHI::SkinFillLight& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.depth_texture || !pass.diffuse_texture || !pass.normal_texture)
        {
            return false;
        }
        // The optional textures only when they are read (missing ones are bound as 1x1 white stand-ins).
        std::vector<LLVKTracker::GLName> sampled = { pass.depth_texture, pass.diffuse_texture, pass.normal_texture };
        if (pass.soft_mask_texture)
        {
            sampled.push_back(pass.soft_mask_texture);
        }
        if (pass.shadows)
        {
            sampled.push_back(pass.light_map_texture);
            for (U32 i = 0; i < CASCADES; ++i)
            {
                sampled.push_back(pass.shadow_textures[i]);
            }
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(sampled, target);
        if (!cmd)
        {
            return false;
        }

        LightUniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.shadow_matrices, pass.shadow_matrices, sizeof(uniforms.shadow_matrices));
        memcpy(uniforms.shadow_far, pass.shadow_far, sizeof(pass.shadow_far));
        memcpy(uniforms.light_direction, pass.light_direction, sizeof(pass.light_direction));
        memcpy(uniforms.light_color, pass.light_color, sizeof(pass.light_color));
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        shadowDepthStored(uniforms.shadow_depth);
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;
        uniforms.soft_uv_scale[0] = pass.soft_uv_scale[0];
        uniforms.soft_uv_scale[1] = pass.soft_uv_scale[1];
        uniforms.directionality = llclamp(pass.directionality, 0.f, 1.f);
        uniforms.near_reach = llmax(pass.near_reach, 0.f);
        uniforms.shadows = pass.shadows ? 1 : 0;
        uniforms.limit[0] = llmax(pass.sun_luminance, 0.f);
        uniforms.limit[1] = llmax(pass.ambient_luminance, 0.f);
        uniforms.limit[2] = llclamp(pass.sunlit_share, 0.f, 1.f);

        // The scene depth is the target's own: attached read-only and sampled in that layout.
        LLVKFullscreen::Image images[LIGHT_IMAGES];
        images[0] = { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1].texture = pass.diffuse_texture;
        images[2].texture = pass.normal_texture;
        images[3].texture = pass.soft_mask_texture;     // 0: a white stand-in, no fade
        images[4].texture = pass.shadows ? pass.light_map_texture : 0;
        for (U32 i = 0; i < CASCADES; ++i)
        {
            images[5 + i].texture = pass.shadows ? pass.shadow_textures[i] : 0;
        }
        // Drawn only with the scene depth attached (its stencil holds the skin mark).
        bool drawn = target.depth_read_only && mLight.draw(cmd, target, &uniforms, sizeof(uniforms), images, LIGHT_IMAGES);
        const F32 eye_share = llclamp(pass.eye_share, 0.f, 1.f);
        if (drawn && eye_share > 0.f)
        {
            LightUniforms eye_uniforms = uniforms;
            for (U32 c = 0; c < 3; ++c)
            {
                eye_uniforms.light_color[c] *= eye_share;
            }
            drawn = mLightEye.draw(cmd, target, &eye_uniforms, sizeof(eye_uniforms), images, LIGHT_IMAGES);
        }
        LLVKTracker::endPass();
        if (drawn && !mLoggedLight)
        {
            mLoggedLight = true;
            LL_INFOS("VKSkinFill") << "Skin fill: light " << target.width << "x" << target.height << (pass.shadows ? ", with" : ", without")
                                   << " shadows, " << (pass.soft_mask_texture ? "soft edge" : "no soft edge") << LL_ENDL;
        }
        return drawn;
    }

    bool record_mask(const LLRHI::SkinFillMask& pass)
    {
        return sRenderer.mask(pass);
    }

    bool record_shrink(const LLRHI::SkinFillShrink& pass)
    {
        return sRenderer.shrink(pass);
    }

    bool record_soften(const LLRHI::SkinFillSoften& pass)
    {
        return sRenderer.soften(pass);
    }

    bool record_occlusion(const LLRHI::SkinFillOcclusion& pass)
    {
        return sRenderer.occlusion(pass);
    }

    bool record_light(const LLRHI::SkinFillLight& pass)
    {
        return sRenderer.light(pass);
    }
}

void LLVKSkinFill::install()
{
    if (!LLRHI::sSkinFill)
    {
        LL_INFOS("VKSkinFill") << "Skin fill off (LLRHI_SKIN_FILL=0)" << LL_ENDL;
        return;
    }
    LLRHI::sSkinFillMask = &record_mask;
    LLRHI::sSkinFillShrink = &record_shrink;
    LLRHI::sSkinFillSoften = &record_soften;
    LLRHI::sSkinFillOcclusion = &record_occlusion;
    LLRHI::sSkinFillLight = &record_light;
    LLVKShader::addBakeStep("skin fill", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
