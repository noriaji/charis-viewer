/**
 * @file waterF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

// class3/environment/waterF.glsl

#define WATER_MINIMAL 1

out vec4 frag_color;

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

vec3 scaleSoftClipFragLinear(vec3 l);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec4 applyWaterFogViewLinear(vec3 pos, vec4 color);

void mirrorClip(vec3 pos);

// PBR interface
vec2 BRDF(float NoV, float roughness);

void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

void pbrIbl(vec3 diffuseColor,
    vec3 specularColor,
    vec3 radiance, // radiance map sample
    vec3 irradiance, // irradiance map sample
    float ao,       // ambient occlusion factor
    float nv,       // normal dot view vector
    float perceptualRoughness,
    out vec3 diffuse,
    out vec3 specular);

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

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

uniform sampler2D bumpMap;
uniform sampler2D bumpMap2;
uniform float     blend_factor;
#ifdef TRANSPARENT_WATER
uniform sampler2D screenTex;
uniform sampler2D depthMap;
#endif

uniform sampler2D exclusionTex;

uniform int classic_mode;
uniform vec3 lightDir;
uniform vec3 specular;
uniform float blurMultiplier;
uniform float refScale;
uniform float kd;
uniform vec3 normScale;
uniform float fresnelScale;
uniform float fresnelOffset;

// <plus> ocean (plus/ocean): the shore field (R: distance to the shore / 64 m,
// GB: unit vector to the shore, A: 1 sea, 0.5 pond, 0) and the camera's axes.
uniform sampler2D plusOceanField;
uniform vec4 plusOceanArea;     // xy: corner of the field (agent space), z: 1 / span, w: on
uniform float plusOceanTime;    // seconds, restarts at 1000
uniform vec3 plusOceanCam;
uniform vec3 plusOceanRight;
uniform vec3 plusOceanUp;
uniform vec3 plusOceanAt;
uniform float plusOceanFoamSkip; // 1: the foam is skipped where it would be 0 (PlusOceanFoamSkip)

// Smooth value noise (about 0..1) for the foam's lace.
float plusOceanHash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float plusOceanNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(plusOceanHash(i), plusOceanHash(i + vec2(1.0, 0.0)), f.x),
               mix(plusOceanHash(i + vec2(0.0, 1.0)), plusOceanHash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// </plus>

//bigWave is (refCoord.w, view.w);
in vec4 refCoord;
in vec4 littleWave;
in vec4 view;
in vec3 vary_position;
in vec3 vary_normal;
in vec3 vary_tangent;
in vec3 vary_light_dir;

vec3 BlendNormal(vec3 bump1, vec3 bump2)
{
    vec3 n = mix(bump1, bump2, blend_factor);
    return n;
}

vec3 srgb_to_linear(vec3 col);
vec3 linear_to_srgb(vec3 col);

vec3 atmosLighting(vec3 light);
vec3 scaleSoftClip(vec3 light);
vec3 toneMapNoExposure(vec3 color);

vec3 transform_normal(vec3 vN, vec3 vT, vec3 vB, vec3 vNt)
{
    return normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);
}

void sampleReflectionProbesWater(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, vec3 amblit_linear);

void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit);


vec3 getPositionWithNDC(vec3 ndc);

void generateWaveNormals(out vec3 wave1, out vec3 wave2, out vec3 wave3)
{
    // Generate all of our wave normals.
    // We layer these back and forth.

    vec2 bigwave = vec2(refCoord.w, view.w);

    vec3 wave1_a = texture(bumpMap, bigwave).xyz * 2.0 - 1.0;
    vec3 wave2_a = texture(bumpMap, littleWave.xy).xyz * 2.0 - 1.0;
    vec3 wave3_a = texture(bumpMap, littleWave.zw).xyz * 2.0 - 1.0;

    vec3 wave1_b = texture(bumpMap2, bigwave).xyz * 2.0 - 1.0;
    vec3 wave2_b = texture(bumpMap2, littleWave.xy).xyz * 2.0 - 1.0;
    vec3 wave3_b = texture(bumpMap2, littleWave.zw).xyz * 2.0 - 1.0;

    wave1 = BlendNormal(wave1_a, wave1_b);
    wave2 = BlendNormal(wave2_a, wave2_b);
    wave3 = BlendNormal(wave3_a, wave3_b);
}

void calculateFresnelFactors(out vec3 df3, out vec2 df2, vec3 viewVec, vec3 wave1, vec3 wave2, vec3 wave3, vec3 wavef)
{
    // We calculate the fresnel here.
    // We do this by getting the dot product for each sets of waves, and applying scale and offset.

    df3 = max(vec3(0), vec3(
        dot(viewVec, wave1),
        dot(viewVec, (wave2 + wave3) * 0.5),
        dot(viewVec, wave3)
    ) * fresnelScale + fresnelOffset);

    df3 *= df3;

    df2 = max(vec2(0), vec2(
        df3.x + df3.y + df3.z,
        dot(viewVec, wavef) * fresnelScale + fresnelOffset
    ));
}

void main()
{
    mirrorClip(vary_position);

    vec3 vN = vary_normal;
    vec3 vT = vary_tangent;
    vec3 vB = cross(vN, vT);

    vec3 pos = vary_position.xyz;
    float linear_depth = 1 / -pos.z;

    float dist = length(pos.xyz);

    //normalize view vector
    vec3 viewVec = normalize(pos.xyz);

    // Setup our waves.

    vec3 wave1 = vec3(0, 0, 1);
    vec3 wave2 = vec3(0, 0, 1);
    vec3 wave3 = vec3(0, 0, 1);

    generateWaveNormals(wave1, wave2, wave3);

    float dmod = sqrt(dist);
    vec2 distort = (refCoord.xy/refCoord.z) * 0.5 + 0.5;

    vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    vec3 df3 = vec3(0);
    vec2 df2 = vec2(0);

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos.xyz, wavef, vary_light_dir, sunlit, amblit, additive, atten);

    calculateFresnelFactors(df3, df2, normalize(view.xyz), wave1, wave2, wave3, wavef);

    vec3 waver = wavef*3;

    vec3 up = transform_normal(vN, vT, vB, vec3(0,0,1));
    float vdu = -dot(viewVec, up)*2;

    vec3 wave_ibl = wavef * normScale;
    wave_ibl.z *= 2.0;
    wave_ibl = transform_normal(vN, vT, vB, normalize(wave_ibl));

    vec3 norm = transform_normal(vN, vT, vB, normalize(wavef));

    vdu = clamp(vdu, 0, 1);
    //wavef.z *= max(vdu*vdu*vdu, 0.1);

    wavef = normalize(wavef);

    //wavef = vec3(0, 0, 1);
    wavef = transform_normal(vN, vT, vB, wavef);

    float dist2 = dist;
    dist = max(dist, 5.0);

    //figure out distortion vector (ripply)
    vec2 distort2 = distort + waver.xy * refScale / max(dmod, 1.0) * 2;

    distort2 = clamp(distort2, vec2(0), vec2(0.999));

    float shadow = 1.0f;

    float water_mask = texture(exclusionTex, distort).r;

#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, distort);
#endif

    vec3 sunlit_linear = sunlit;
    float fade = 1;
#ifdef TRANSPARENT_WATER
    float depth = texture(depthMap, distort).r;

    vec3 refPos = getPositionWithNDC(vec3(distort*2.0-vec2(1.0), depth*2.0-1.0));

    // Calculate some distance fade in the water to better assist with refraction blending and reducing the refraction texture's "disconnect".
#ifdef SHORELINE_FADE
    fade = max(0,min(1, (pos.z - refPos.z) / 10));
#else
    fade = 1;
#endif
    fade *= water_mask;
    distort2 = mix(distort, distort2, min(1, fade * 10));
    depth = texture(depthMap, distort2).r;

    refPos = getPositionWithNDC(vec3(distort2 * 2.0 - vec2(1.0), depth * 2.0 - 1.0));

    if (pos.z < refPos.z - 0.05)
    {
        distort2 = distort;
    }

    vec4 fb = texture(screenTex, distort2);

#else
    vec4 fb = applyWaterFogViewLinear(viewVec*2048.0, vec4(1.0));

    if (water_mask < 1)
        discard;
#endif

    float metallic = 1.0;
    float perceptualRoughness = blurMultiplier;
    float gloss      = 1 - perceptualRoughness;

    vec3  irradiance = vec3(0);
    vec3  radiance  = vec3(0);
    vec3 legacyenv = vec3(0);

    // TODO: Make this an option.
#ifdef WATER_MINIMAL
    sampleReflectionProbesWater(irradiance, radiance, distort2, pos.xyz, wave_ibl.xyz, gloss, amblit);
#elif WATER_MINIMAL_PLUS
    sampleReflectionProbes(irradiance, radiance, distort2, pos.xyz, wave_ibl.xyz, gloss, false, amblit);
#endif

    vec3 diffuseColor = vec3(0);
    vec3 specularColor = vec3(0);
    vec3 specular_linear = srgb_to_linear(specular);
    calcDiffuseSpecular(specular_linear, metallic, diffuseColor, specularColor);

    vec3 v = -normalize(pos.xyz);

    vec3 colorEmissive = vec3(0);
    float ao = 1.0;
    vec3 light_dir = transform_normal(vN, vT, vB, lightDir);

    float NdotV = clamp(abs(dot(norm, v)), 0.001, 1.0);

    float nl = 0;
    vec3 diffPunc = vec3(0);
    vec3 specPunc = vec3(0);

    pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, normalize(wavef+up*max(dist, 32.0)/32.0*(1.0-vdu)), v, normalize(light_dir), nl, diffPunc, specPunc);

    vec3 punctual = clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit_linear * shadow * atten;
    radiance *= df2.y;
    //radiance = toneMapNoExposure(radiance);
    vec3 color = vec3(0);
    color = mix(fb.rgb, radiance, min(1, df2.x)) + punctual.rgb;

    float water_haze_scale = 4;

    if (classic_mode > 0)
        water_haze_scale = 1;

    // This looks super janky, but we do this to restore water haze in the distance.
    // These values were finagled in to try and bring back some of the distant brightening on legacy water.  Also works reasonably well on PBR skies such as PBR midday.
    // color = mix(color, additive * water_haze_scale, (1 - atten));

    // We shorten the fade here at the shoreline so it doesn't appear too soft from a distance.
    fade *= 60;
    fade = min(1, fade);
    color = mix(fb.rgb, color, fade);

#ifdef TRANSPARENT_WATER
    // <plus> ocean (plus/ocean): a thin white line where the water is shallow, and on the sea
    // a thin sheet that runs up and back, and a few thin lines of surf coming in towards the
    // shore. The depth behind the water gives the line (any shore, pier or rock); the field
    // gives the sea, the distance to the shore and its direction.
    if (plusOceanArea.w > 0.5)
    {
        vec3 ocean_world = plusOceanCam + pos.x * plusOceanRight + pos.y * plusOceanUp - pos.z * plusOceanAt;
        vec2 ocean_uv = (ocean_world.xy - plusOceanArea.xy) * plusOceanArea.z;
        vec4 shore = vec4(0.0);
        bool foam_here = all(greaterThan(ocean_uv, vec2(0.0))) && all(lessThan(ocean_uv, vec2(1.0)));
        if (foam_here)
        {
            shore = textureLod(plusOceanField, ocean_uv, 0.0);
            // No foam at all 30 m or more from the shore (reach and zone below are exactly 0
            // there), nor where the field holds neither sea nor pond (sea_k and pond_k are
            // exactly 0): the colour would stay as it is, so the rest is skipped
            // (plusOceanFoamSkip, research/125 #20).
            foam_here = plusOceanFoamSkip < 0.5 || (shore.r * 64.0 < 30.0 && shore.a > 0.2);
        }
        if (foam_here)
        {
            float sea_k = smoothstep(0.6, 0.9, shore.a);
            float pond_k = (1.0 - sea_k) * smoothstep(0.2, 0.4, shore.a);
            float shore_m = shore.r * 64.0;
            vec2 to_shore = shore.gb * 2.0 - 1.0;

            // The water's depth straight down, from the depth of the scene behind it.
            float behind = max(length(refPos) - dist2, 0.0);
            float depth_m = behind * abs(dot(viewVec, vN));

            // The surge: the water runs up fast and falls back slowly, every 10 s.
            float cycle = fract(plusOceanTime * 0.1);
            float surge = cycle < 0.25 ? sin(cycle * 4.0 * 1.5707963) : cos((cycle - 0.25) / 0.75 * 1.5707963);

            // Lace: smooth noise fixed to the world, moved a little with the surge.
            vec2 lace_p = ocean_world.xy + to_shore * surge * 1.2;
            float lace = 0.65 * plusOceanNoise(lace_p * 0.17) + 0.35 * plusOceanNoise(lace_p * 0.6);

            // The line at the waterline, and (sea) the thin sheet the wave leaves going back.
            float line_w = (0.10 + 0.22 * sea_k * surge) * (0.6 + 0.8 * lace);
            float line = 1.0 - smoothstep(0.35 * line_w, line_w, depth_m);
            float sheet_w = 0.35 + 1.6 * surge;
            float sheet = (1.0 - smoothstep(0.0, sheet_w, depth_m)) * smoothstep(0.45, 0.8, lace) * 0.45 * sea_k;
            // Close to the shore only (a gentle slope would spread them far over the water).
            float reach = 1.0 - smoothstep(mix(1.0, 1.5 + 4.0 * surge, sea_k), mix(2.5, 3.5 + 8.0 * surge, sea_k), shore_m);
            float edge_foam = (line * 0.9 + sheet) * reach * (sea_k + pond_k);

            // Surf: thin lines every 14 m, coming in, each with its own gaps.
            float phase = shore_m / 14.0 + plusOceanTime * 0.07 + lace * 0.15;
            float crest = smoothstep(0.93, 0.995, 0.5 + 0.5 * sin(6.2831853 * phase));
            float zone = smoothstep(4.0, 9.0, shore_m) * (1.0 - smoothstep(16.0, 30.0, shore_m));
            float alive = smoothstep(0.5, 0.75, plusOceanNoise(ocean_world.xy * 0.09 + vec2(mod(floor(phase), 10.0) * 7.3, 0.0)));
            float wave_foam = crest * zone * alive * sea_k * smoothstep(0.35, 0.65, lace) * 0.5;

            float foam = clamp(edge_foam + wave_foam, 0.0, 1.0) * water_mask;
            vec3 foam_lit = clamp(amblit + sunlit_linear * shadow * 0.35, vec3(0.08), vec3(1.2));
            vec3 foam_color = vec3(0.92, 0.96, 1.0) * foam_lit;
            float far_k = (atten.r + atten.g + atten.b) / 3.0;
            color = mix(color, foam_color, foam * far_k * 0.85);
        }
    }
    // </plus>
#endif

    float spec = min(max(max(punctual.r, punctual.g), punctual.b), 0);

    frag_color = min(vec4(1),max(vec4(color.rgb, spec * water_mask), vec4(0)));
}

