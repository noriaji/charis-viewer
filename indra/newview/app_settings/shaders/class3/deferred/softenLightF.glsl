/**
 * @file class3/deferred/softenLightF.glsl
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

/*[EXTRA_CODE_HERE]*/

#define FLT_MAX 3.402823466e+38

out vec4 frag_color;

const float M_PI = 3.14159265;

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
uniform sampler2D lightMap;
#endif

uniform sampler2D     lightFunc;

uniform float blur_size;
uniform float blur_fidelity;

#if defined(HAS_SSAO)
uniform float ssao_irradiance_scale;
uniform float ssao_irradiance_max;
#endif

// Inputs
uniform vec4 clipPlane;
uniform mat3 env_mat;
uniform mat3  ssao_effect_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int  sun_up_factor;
uniform int classic_mode;

in vec2 vary_fragcoord;

uniform mat4 inv_proj;
uniform vec2 screen_res;

vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec3  atmosFragLightingLinear(vec3 l, vec3 additive, vec3 atten);
vec3  scaleSoftClipFragLinear(vec3 l);

// reflection probe interface
void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
    vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);
float getDepth(vec2 pos_screen);

vec3 linear_to_srgb(vec3 c);
vec3 srgb_to_linear(vec3 c);

uniform vec4 waterPlane;

uniform int cube_snapshot;

uniform float sky_hdr_scale;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten);

GBufferInfo getGBuffer(vec2 screenpos);
vec3 clampHDRRange(vec3 color);

// <plus> plus/ssgi (research/164): screen space indirect light, the light
// near surfaces send back, from passes of its own (a visibility bitmask over
// GTAO's slices). Added after the ambient occlusion, without it: the bitmask's
// occlusion is the light map's g, so it comes back where that darkened.
// plusSsgiParams: x the strength (0: nothing added, the map is not read),
// y full-resolution pixels per map pixel, each way; plusSsgiSize: the
// screen's size, then the map's. The map: rgb irradiance, a view depth (m).
#if defined(HAS_SSAO)
uniform sampler2D plusSsgiMap;
uniform vec4 plusSsgiParams;
uniform vec4 plusSsgiSize;

// The map at this pixel (view depth d): the four nearest map pixels,
// weighted by distance and by how close their depth is to d.
vec3 plusSsgiIrradiance(vec2 tc, float d)
{
    if (plusSsgiParams.x <= 0.0)
    {
        return vec3(0.0);
    }
    ivec2 full_size = ivec2(plusSsgiSize.xy);
    ivec2 map_size = ivec2(plusSsgiSize.zw);
    ivec2 pixel = clamp(ivec2(tc * plusSsgiSize.xy), ivec2(0), full_size - 1);
    // A map pixel stands for the full-resolution pixel at the top-left of its block.
    vec2 place = vec2(pixel) / plusSsgiParams.y;
    ivec2 base = ivec2(floor(place));
    vec2 f = place - vec2(base);
    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    vec3 nearest = vec3(0.0);
    float nearest_difference = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 offset = ivec2(i & 1, i >> 1);
        vec4 tap = texelFetch(plusSsgiMap, clamp(base + offset, ivec2(0), map_size - 1), 0);
        vec2 along = mix(vec2(1.0) - f, f, vec2(offset));
        float difference = abs(tap.a - d) / max(d, 1e-3);
        float weight = along.x * along.y * max(0.0, 1.0 - difference / 0.1);
        sum += tap.rgb * weight;
        weight_sum += weight;
        if (difference < nearest_difference)
        {
            nearest_difference = difference;
            nearest = tap.rgb;
        }
    }
    vec3 light = weight_sum > 1e-4 ? sum / weight_sum : nearest;
    // Nothing that is not a number, nothing below 0 (the passes write neither).
    return (any(isnan(light)) || any(isinf(light))) ? vec3(0.0) : max(light, vec3(0.0)) * plusSsgiParams.x;
}
#endif
// </plus>

void adjustIrradiance(inout vec3 irradiance, float ambocc)
{
    // use sky settings ambient or irradiance map sample, whichever is brighter
    //irradiance = max(amblit_linear, irradiance);

#if defined(HAS_SSAO)
    irradiance = mix(ssao_effect_mat * min(irradiance.rgb*ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance.rgb, ambocc);
#endif
}

void main()
{
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    vec4  pos          = getPositionWithDepth(tc, depth);

    GBufferInfo gb = getGBuffer(tc);

    vec3 colorEmissive = gb.emissive.rgb;
    float envIntensity = gb.envIntensity;
    vec3  light_dir   = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec4 baseColor     = gb.albedo;
    vec4 spec        = gb.specular; // NOTE: PBR linear Emissive

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
    vec2 scol_ambocc = texture(lightMap, vary_fragcoord.xy).rg;
#endif

#if defined(HAS_SUN_SHADOW)
    float scol       = max(scol_ambocc.r, baseColor.a);
#else
    float scol = 1.0;
#endif
#if defined(HAS_SSAO)
    float ambocc     = scol_ambocc.g;
#else
    float ambocc = 1.0;
#endif

    vec3  color = vec3(0);
    float bloom = 0.0;

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, gb.normal, light_dir, sunlit, amblit, additive, atten);

    if (classic_mode > 0)
        sunlit *= 1.35;

    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3  radiance  = vec3(0);

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = orm.g;
        float metallic = orm.b;
        float ao = orm.r;

        vec3  irradiance = amblit_linear;

        // PBR IBL
        float gloss      = 1.0 - perceptualRoughness;

        sampleReflectionProbes(irradiance, radiance, tc, pos.xyz, gb.normal, gloss, false, amblit_linear);

        adjustIrradiance(irradiance, ambocc);
        // <plus> plus/ssgi (research/164): the indirect light, after the occlusion, without it
#if defined(HAS_SSAO)
        irradiance += plusSsgiIrradiance(tc, -pos.z);
#endif
        // </plus>

        vec3 diffuseColor = vec3(0.0);
        vec3 specularColor = vec3(0.0);
        calcDiffuseSpecular(baseColor.rgb, metallic, diffuseColor, specularColor);

        vec3 v = -normalize(pos.xyz);
        color = pbrBaseLight(diffuseColor, specularColor, metallic, v, gb.normal, perceptualRoughness, light_dir, sunlit_linear, scol, radiance, irradiance, colorEmissive, ao, additive, atten);
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI))
    {
        // actual HDRI sky, just copy color value
        color = colorEmissive.rgb;
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        //should only be true of WL sky, port over base color value and scale for fake HDR
#if defined(HAS_EMISSIVE)
        color = colorEmissive.rgb;
#else
        color = baseColor.rgb;
#endif
        color = srgb_to_linear(color);
        color *= sky_hdr_scale;
    }
    else
    {
        // legacy shaders are still writng sRGB to gbuffer
        baseColor.rgb = srgb_to_linear(baseColor.rgb);

        spec.rgb = srgb_to_linear(spec.rgb);

        float da          = clamp(dot(gb.normal, light_dir.xyz), 0.0, 1.0);

        vec3 irradiance = amblit;
        vec3 glossenv = vec3(0);
        vec3 legacyenv = vec3(0);

        sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, spec.a, envIntensity, false, amblit_linear);

        adjustIrradiance(irradiance, ambocc);
        // <plus> plus/ssgi (research/164): the indirect light, after the occlusion, without it
#if defined(HAS_SSAO)
        irradiance += plusSsgiIrradiance(tc, -pos.z);
#endif
        // </plus>

        // apply lambertian IBL only (see pbrIbl)
        color.rgb = irradiance;

        if (classic_mode > 0)
        {
            da = pow(da,1.2);
            vec3 sun_contrib = vec3(min(da, scol));

            color.rgb = srgb_to_linear(color.rgb * 0.9 + (linear_to_srgb(sun_contrib) * sunlit_linear * 0.7));
            sunlit_linear = srgb_to_linear(sunlit_linear);
        }
        else
        {
            vec3 sun_contrib = min(da, scol) * sunlit_linear;
            color.rgb += sun_contrib;
        }

        color.rgb *= baseColor.rgb;

        vec3 refnormpersp = reflect(pos.xyz, gb.normal);

        if (spec.a > 0.0)
        {
            vec3  lv = light_dir.xyz;
            vec3  h, l, v = -normalize(pos.xyz);
            float nh, nl, nv, vh, lightDist;
            vec3 n = gb.normal;
            calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

            if (nl > 0.0 && nh > 0.0)
            {
                float lit = min(nl*6.0, 1.0);

                float sa = nh;
                float fres = pow(1 - vh, 5) * 0.4+0.5;
                float gtdenom = 2 * nh;
                float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

                scol *= fres*texture(lightFunc, vec2(nh, spec.a)).r*gt/(nh*nl);
                color.rgb += lit*scol*sunlit_linear.rgb*spec.rgb;
            }

            // add radiance map
            applyGlossEnv(color, glossenv, spec, pos.xyz, gb.normal);

        }

        color.rgb = mix(color.rgb, baseColor.rgb, baseColor.a);

        if (envIntensity > 0.0)
        {  // add environment map
            applyLegacyEnv(color, legacyenv, spec, pos.xyz, gb.normal, envIntensity);
        }

        // <plus> plus/materialize (research/256): the emissive target on a
        // legacy face, which only the band's edge (and the wireframe's lines) writes (every other legacy
        // writer leaves it 0, and without the target getGBuffer reads 0):
        // added as pbrBaseLight adds the PBR emissive, so the classic sky's
        // final 1.1 scales it as it does that (its 1.35 is the sun's only).
        // Adding 0 leaves every other face as it was.
        color.rgb += colorEmissive;
        // </plus>
   }

    //color.r = classic_mode > 0 ? 1.0 : 0.0;
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;

    frag_color.rgb = clampHDRRange(color.rgb * final_scale); //output linear since local lights will be added to this shader's results
    frag_color.a = 0.0;
}
