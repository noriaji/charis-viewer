/**
 * @file class1/deferred/shadowUtil.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

uniform sampler2D   normalMap;

#if defined(SUN_SHADOW)
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
#endif

// <plus> plus/shadowpenumbra (research/187): the two nearest sun cascades read again as depth (not compared), to
// find what shadows a pixel and how far in front of it. Only the sun light pass defines PLUS_PENUMBRA.
#if defined(SUN_SHADOW) && defined(PLUS_PENUMBRA)
uniform sampler2D shadowMapRaw0;
uniform sampler2D shadowMapRaw1;
// 0: off (the official read); else how many times the physical penumbra (set by the sun light pass only).
uniform float plus_penumbra;
// x: unused, y: taps of the blocker search, z: taps of the filter, w: the widest penumbra in metres.
uniform vec4 plus_penumbra_params;
#endif
// </plus>

#if defined(SPOT_SHADOW)
uniform sampler2DShadow shadowMap4;
uniform sampler2DShadow shadowMap5;
#endif

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform vec2 shadow_res;
uniform vec2 proj_shadow_res;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;
uniform float shadow_offset;
uniform float spot_shadow_bias;
uniform float spot_shadow_offset;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;

float pcfShadow(sampler2DShadow shadowMap, vec3 norm, vec4 stc, float bias_mul, vec2 pos_screen, vec3 light_dir)
{
#if defined(SUN_SHADOW)
    float offset = shadow_bias * bias_mul;
    stc.xyz /= stc.w;
    stc.z += offset * 2.0;
    // <plus> plus/shadowdist (research/107 section 24): a 3x3 tent over the texels from four
    // bilinear compares (Castano), in place of the x snap with its jitter and the five taps,
    // which left texel steps and a dotted pattern on the edges seen at a slant. No texel
    // farther than 2 from the read (the shadow receiver cull's 3.5 x 2.5 still holds), the
    // same at every pixel; explicit LOD: no derivatives inside the cascade branches.
    vec2 uv = stc.xy * shadow_res;
    vec2 base = floor(uv + 0.5);
    vec2 st = uv + 0.5 - base;
    base -= 0.5;
    vec2 w0 = 3.0 - 2.0 * st;
    vec2 w1 = 1.0 + 2.0 * st;
    vec2 o0 = (2.0 - st) / w0 - 1.0;
    vec2 o1 = st / w1 + 1.0;
    vec2 inv_res = 1.0 / shadow_res;
    float shadow = w0.x * w0.y * textureLod(shadowMap, vec3((base + vec2(o0.x, o0.y)) * inv_res, stc.z), 0.0);
    shadow += w1.x * w0.y * textureLod(shadowMap, vec3((base + vec2(o1.x, o0.y)) * inv_res, stc.z), 0.0);
    shadow += w0.x * w1.y * textureLod(shadowMap, vec3((base + vec2(o0.x, o1.y)) * inv_res, stc.z), 0.0);
    shadow += w1.x * w1.y * textureLod(shadowMap, vec3((base + vec2(o1.x, o1.y)) * inv_res, stc.z), 0.0);
    return clamp(shadow / 16.0, 0.0, 1.0);
    // </plus>
#else
    return 1.0;
#endif
}

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    stc.x = floor(proj_shadow_res.x * stc.x + fract(pos_screen.y*0.666666666)) / proj_shadow_res.x; // snap

    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs;

    vec2 off = 1.0/proj_shadow_res;
    off.y *= 1.5;

    shadow += texture(shadowMap, stc.xyz+vec3(off.x*2.0, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(off.x, -off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x*2.0, -off.y, 0.0));
    return shadow*0.2;
#else
    return 1.0;
#endif
}

// <plus> plus/shadowdist (research/107 section 31): the receiver moved along its normal
// before a cascade is read, by plus_normal_offset_texels of that cascade's texels where the
// light grazes it ((1 - n.l)^2: next to nothing where it faces the light), at most
// NORMAL_OFFSET_MAX metres. A curved surface the light grazes (the side of a nose) then reads
// past its own depth instead of in a band of it (acne). Set by bindDeferredShader
// (PlusShadowNormalOffset; 0: none). The shadow receiver cull counts the most it can move
// (plus::shadowdist::receiverNormalOffsetMax, the same 0.1 m).
uniform float plus_normal_offset_texels;
const float NORMAL_OFFSET_MAX = 0.1;
// However fine a close-up's texels: the shadow pass and the main view differ by about this much.
const float MIN_TEXEL_FOR_OFFSET = 0.004;

vec4 normalOffsetPos(mat4 m, vec4 spos, vec3 norm, float ndl)
{
    // A texel of the map in eye space metres, from the matrix's rate along its x and y
    // (exact for an orthographic cascade; the perspective ones scaled by their w).
    float w = abs((m * spos).w);
    float tx = w / max(length(vec3(m[0][0], m[1][0], m[2][0])) * shadow_res.x, 1e-6);
    float ty = w / max(length(vec3(m[0][1], m[1][1], m[2][1])) * shadow_res.y, 1e-6);
    float graze = 1.0 - ndl;
    float d = min(plus_normal_offset_texels * max(max(tx, ty), MIN_TEXEL_FOR_OFFSET) * graze * graze, NORMAL_OFFSET_MAX);
    return spos + vec4(norm * d, 0.0);
}
// </plus>

// <plus> plus/shadowpenumbra (research/187): a penumbra as wide as the sun's disc makes it at the distance
// of what shadows the pixel (percentage-closer soft shadows), in the two nearest cascades: sharp where a
// shadow starts at its caster's foot, softer the farther it falls. The cascades must be orthographic
// (PlusShadowStabilize): the depth difference is then a length along the light, from the matrix's
// third row, as the texel size is from its first two (normalOffsetPos).
#if defined(SUN_SHADOW) && defined(PLUS_PENUMBRA)
// The sun's (and the moon's) disc is about 0.53 degrees across: a penumbra of 2 * tan(0.265 degrees) per metre.
const float PLUS_SUN_PENUMBRA_PER_METRE = 0.00925;
const float PLUS_GOLDEN_ANGLE = 2.39996323;

// A fixed noise pattern in screen space (Jimenez' interleaved gradient noise), 0..1.
float plusPenumbraNoise()
{
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// Point i of n on a disc of radius 1 (Vogel), turned by angle.
vec2 plusVogelDisc(int i, int n, float angle)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * PLUS_GOLDEN_ANGLE + angle;
    return r * vec2(cos(a), sin(a));
}

float plusPenumbraShadow(sampler2DShadow cmp, sampler2D raw, mat4 m, vec4 lpos, vec3 norm, vec2 pos_screen, vec3 light_dir)
{
    // The official read (the 3x3 tent): kept where the penumbra is no wider than it, and under it.
    float base = pcfShadow(cmp, norm, lpos, 1.0, pos_screen, light_dir);

    vec3 stc = lpos.xyz / lpos.w;
    stc.z += shadow_bias * 2.0;     // as pcfShadow's receiver bias

    // Metres: one texel across the map, and the whole depth range of the cascade.
    float texel_m = 1.0 / max(length(vec3(m[0][0], m[1][0], m[2][0])) * shadow_res.x, 1e-6);
    float range_m = 1.0 / max(length(vec3(m[0][2], m[1][2], m[2][2])), 1e-6);

    float max_width = plus_penumbra_params.w;
    float search_texels = clamp(0.5 * max_width / texel_m, 2.0, 16.0);
    float search_m = search_texels * texel_m;

    // A receiver the light grazes rises across the search by up to its slope: what is that much nearer
    // than the pixel is its own surface, not a blocker.
    float ndl = clamp(dot(norm, light_dir), 0.05, 1.0);
    float slope = min(sqrt(max(1.0 - ndl * ndl, 0.0)) / ndl, 8.0);
    float blocker_gap = (search_m * slope + texel_m) / range_m;

    float angle = plusPenumbraNoise() * 6.2831853;
    int blocker_taps = int(clamp(plus_penumbra_params.y, 1.0, 24.0));
    ivec2 size = ivec2(shadow_res);
    vec2 centre = stc.xy * shadow_res;

    float blocker_sum = 0.0;
    float blocker_count = 0.0;
    for (int i = 0; i < blocker_taps; ++i)
    {
        ivec2 tc = ivec2(floor(centre + plusVogelDisc(i, blocker_taps, angle) * search_texels));
        if (any(lessThan(tc, ivec2(0))) || any(greaterThanEqual(tc, size)))
        {
            continue;
        }
        float d = texelFetch(raw, tc, 0).r;
        if (d < stc.z - blocker_gap)
        {
            blocker_sum += d;
            blocker_count += 1.0;
        }
    }
    if (blocker_count < 0.5)
    {
        return base;    // nothing shadows it near enough to matter
    }

    float distance_m = (stc.z - blocker_sum / blocker_count) * range_m;
    float width_m = clamp(distance_m * PLUS_SUN_PENUMBRA_PER_METRE * plus_penumbra, 0.0, max_width);
    float radius = 0.5 * width_m / texel_m;     // texels
    float soft = smoothstep(1.0, 2.5, radius);
    if (soft <= 0.0)
    {
        return base;
    }

    int filter_taps = int(clamp(plus_penumbra_params.z, 1.0, 32.0));
    float lit = 0.0;
    for (int i = 0; i < filter_taps; ++i)
    {
        vec2 uv = (centre + plusVogelDisc(i, filter_taps, angle + 1.3) * radius) / shadow_res;
        lit += textureLod(cmp, vec3(uv, stc.z), 0.0);
    }
    return mix(base, lit / float(filter_taps), soft);
}
#endif
// </plus>

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
#if defined(SUN_SHADOW)
    float shadow = 0.0f;
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float dp_directional_light = max(0.0, dot(norm.xyz, light_dir));
          dp_directional_light = clamp(dp_directional_light, 0.0, 1.0);

    vec3 shadow_pos = pos.xyz;

    vec3 offset = light_dir.xyz * (1.0 - dp_directional_light);

    shadow_pos += offset * shadow_offset * 2.0;

    vec4 spos = vec4(shadow_pos.xyz, 1.0);

    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;
        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        if (spos.z < near_split.z)
        {
            // <plus> plus/shadowdist: the receiver moved along its normal (was: shadow_matrix[3]*spos)
            lpos = shadow_matrix[3]*normalOffsetPos(shadow_matrix[3], spos, norm, dp_directional_light);
            // </plus>

            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap3, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        if (spos.z < near_split.y && spos.z > far_split.z)
        {
            // <plus> plus/shadowdist: the receiver moved along its normal (was: shadow_matrix[2]*spos)
            lpos = shadow_matrix[2]*normalOffsetPos(shadow_matrix[2], spos, norm, dp_directional_light);
            // </plus>

            float w = 1.0;
            w -= max(spos.z-far_split.y, 0.0)/transition_domain.y;
            w -= max(near_split.z-spos.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap2, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z < near_split.x && spos.z > far_split.y)
        {
            // <plus> plus/shadowdist: the receiver moved along its normal (was: shadow_matrix[1]*spos)
            lpos = shadow_matrix[1]*normalOffsetPos(shadow_matrix[1], spos, norm, dp_directional_light);
            // </plus>

            float w = 1.0;
            w -= max(spos.z-far_split.x, 0.0)/transition_domain.x;
            w -= max(near_split.y-spos.z, 0.0)/transition_domain.y;
            //w = clamp(w, 0.0, 1.0);
            // <plus> plus/shadowpenumbra (research/187): the nearest two cascades with a penumbra that grows with distance from the caster (was: pcfShadow(shadowMap1, ...))
#if defined(PLUS_PENUMBRA)
            float contrib;
            if (plus_penumbra > 0.0)
            {
                contrib = plusPenumbraShadow(shadowMap1, shadowMapRaw1, shadow_matrix[1], lpos, norm, pos_screen, light_dir)*w;
            }
            else
            {
                contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir)*w;
            }
#else
            float contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir)*w;
#endif
            // </plus>
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z > far_split.x)
        {
            // <plus> plus/shadowdist: the receiver moved along its normal (was: shadow_matrix[0]*spos)
            lpos = shadow_matrix[0]*normalOffsetPos(shadow_matrix[0], spos, norm, dp_directional_light);
            // </plus>

            float w = 1.0;
            w -= max(near_split.x-spos.z, 0.0)/transition_domain.x;
            //w = clamp(w, 0.0, 1.0);
            // <plus> plus/shadowpenumbra (research/187): as cascade 1 (was: pcfShadow(shadowMap0, ...))
#if defined(PLUS_PENUMBRA)
            float contrib;
            if (plus_penumbra > 0.0)
            {
                contrib = plusPenumbraShadow(shadowMap0, shadowMapRaw0, shadow_matrix[0], lpos, norm, pos_screen, light_dir)*w;
            }
            else
            {
                contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir)*w;
            }
#else
            float contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir)*w;
#endif
            // </plus>
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        shadow /= weight;
    }
    else
    {
        return 1.0f; // lit beyond the far split...
    }
    //shadow = min(dp_directional_light,shadow);
    return shadow;
#else
    return 1.0;
#endif
}

float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    float shadow = 0.0f;
    pos += norm * spot_shadow_offset;

    vec4 spos = vec4(pos,1.0);
    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;

        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        {
            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;

            if (index == 0)
            {
                lpos = shadow_matrix[4]*spos;
                shadow += pcfSpotShadow(shadowMap4, lpos, 0.8, spos.xy)*w;
            }
            else
            {
                lpos = shadow_matrix[5]*spos;
                shadow += pcfSpotShadow(shadowMap5, lpos, 0.8, spos.xy)*w;
            }
            weight += w;
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        shadow /= weight;
    }
    else
    {
        shadow = 1.0f;
    }
    return shadow;
#else
    return 1.0;
#endif
}

