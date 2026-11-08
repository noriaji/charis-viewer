/**
* @file materialF.glsl
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

//class1/deferred/materialF.glsl

// This shader is used for both writing opaque/masked content to the gbuffer and writing blended content to the framebuffer during the alpha pass.

#define DIFFUSE_ALPHA_MODE_NONE     0
#define DIFFUSE_ALPHA_MODE_BLEND    1
#define DIFFUSE_ALPHA_MODE_MASK     2
#define DIFFUSE_ALPHA_MODE_EMISSIVE 3

// LLRHI (research/59): only the Vulkan backend defines LLRHI_MATERIAL_FAST
// (with the extensions it needs); GL compiles this file as it was. The
// blended path then works out its alpha before the costly lighting and lets
// a quad whose fragments all come out with alpha 0 skip it, and takes the
// sun shadow only for quads where a fragment's color depends on it. Same
// pixels: see main.
#if defined(LLRHI_MATERIAL_FAST) && (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)
#define MATERIAL_EARLY_OUT 1
#endif

uniform float emissive_brightness;  // fullbright flag, 1.0 == fullbright, 0.0 otherwise
uniform int sun_up_factor;
uniform int classic_mode;

vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
vec3 scaleSoftClipFragLinear(vec3 l);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cs);

uniform mat4 modelview_matrix;
uniform mat3 normal_matrix;

in vec3 vary_position;

void mirrorClip(vec3 pos);
// <plus> plus/materialize (research/256): set by name, all 0 unless a band
// is drawn; the helpers are in globalF.glsl
uniform vec4 materialize_plane;
uniform vec4 materialize_params;
uniform vec4 materialize_color;
void materializeClip(vec3 pos, vec4 plane, vec4 params);
vec4 materializeEdge(vec3 pos, vec4 plane, vec4 params, vec3 color);
vec4 materializeLegacyAlbedo(vec4 albedo, vec4 edge);
// </plus>
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)

out vec4 frag_color;

// <plus> depth of field behind alpha (plus/dofalpha): above 0 only in the alpha
// pool's depth pass for depth of field, which writes no colour; there the
// fragments less opaque than it write no depth. 0 (every colour pass) changes nothing.
uniform float plus_dof_alpha_cutoff;
// </plus>

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);

uniform samplerCube environmentMap;
uniform sampler2D     lightFunc;

// Inputs
uniform vec4 morphFactor;
uniform vec3 camPosLocal;
uniform mat3 env_mat;

uniform float is_mirror;

uniform vec3 sun_dir;
uniform vec3 moon_dir;

uniform mat4 proj_mat;
uniform mat4 inv_proj;
uniform vec2 screen_res;

uniform vec4 light_position[8];
uniform vec3 light_direction[8];
uniform vec4 light_attenuation[8];
uniform vec3 light_diffuse[8];

float getAmbientClamp();
void waterClip(vec3 pos);

vec3 calcPointLightOrSpotLight(vec3 light_col, vec3 npos, vec3 diffuse, vec4 spec, vec3 v, vec3 n, vec4 lp, vec3 ln, float la, float fa, float is_pointlight, inout float glare, float ambiance)
{
    // SL-14895 inverted attenuation work-around
    // This routine is tweaked to match deferred lighting, but previously used an inverted la value. To reconstruct
    // that previous value now that the inversion is corrected, we reverse the calculations in LLPipeline::setupHWLights()
    // to recover the `adjusted_radius` value previously being sent as la.
    float falloff_factor = (12.0 * fa) - 9.0;
    float inverted_la = falloff_factor / la;
    // Yes, it makes me want to cry as well. DJH

    vec3 col = vec3(0);

    //get light vector
    vec3 lv = lp.xyz - v;

    //get distance
    float dist = length(lv);
    float da = 1.0;

    dist /= inverted_la;

    if (dist > 0.0 && inverted_la > 0.0)
    {
        //normalize light vector
        lv = normalize(lv);

        //distance attenuation
        float dist_atten = clamp(1.0 - (dist - 1.0*(1.0 - fa)) / fa, 0.0, 1.0);
        dist_atten *= dist_atten;
        dist_atten *= 2.0f;

        if (dist_atten <= 0.0)
        {
            return col;
        }

        // spotlight coefficient.
        float spot = max(dot(-ln, lv), is_pointlight);
        da *= spot*spot; // GL_SPOT_EXPONENT=2

        //angular attenuation
        da *= dot(n, lv);

        float lit = 0.0f;

        float amb_da = ambiance;
        if (da >= 0)
        {
            lit = clamp(da * dist_atten, 0.0, 1.0);
            col = lit * light_col * diffuse;
            amb_da += (da*0.5 + 0.5) * ambiance;
        }
        amb_da += (da*da*0.5 + 0.5) * ambiance;
        amb_da *= dist_atten;
        amb_da = min(amb_da, 1.0f - lit);

        // SL-10969 need to see why these are blown out
        //col.rgb += amb_da * light_col * diffuse;

        if (spec.a > 0.0)
        {
            //vec3 ref = dot(pos+lv, norm);
            vec3 h = normalize(lv + npos);
            float nh = dot(n, h);
            float nv = dot(n, npos);
            float vh = dot(npos, h);
            float sa = nh;
            float fres = pow(1 - dot(h, npos), 5)*0.4 + 0.5;

            float gtdenom = 2 * nh;
            float gt = max(0, min(gtdenom * nv / vh, gtdenom * da / vh));

            if (nh > 0.0)
            {
                float scol = fres*texture(lightFunc, vec2(nh, spec.a)).r*gt / (nh*da);
                vec3 speccol = lit*scol*light_col.rgb*spec.rgb;
                speccol = clamp(speccol, vec3(0), vec3(1));
                col += speccol;

                float cur_glare = max(speccol.r, speccol.g);
                cur_glare = max(cur_glare, speccol.b);
                glare = max(glare, speccol.r);
                glare += max(cur_glare, 0.0);
            }
        }
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    return max(col * final_scale, vec3(0.0, 0.0, 0.0));
}

#ifdef MATERIAL_EARLY_OUT
// Whether b holds in all four fragments of the quad (helpers take part; all
// four lanes swap, so the quad agrees).
bool llrhiQuadAll(bool b)
{
    bool h = subgroupQuadSwapHorizontal(b);
    bool v = subgroupQuadSwapVertical(b);
    bool d = subgroupQuadSwapDiagonal(b);
    return b && h && v && d;
}
#endif

#else
out vec4 frag_data[4];
#endif

uniform sampler2D diffuseMap;  //always in sRGB space

#ifdef HAS_NORMAL_MAP
uniform sampler2D bumpMap;
#endif

#ifdef HAS_SPECULAR_MAP
uniform sampler2D specularMap;

in vec2 vary_texcoord2;
#endif

uniform float env_intensity;
uniform vec4 specular_color;  // specular color RGB and specular exponent (glossiness) in alpha

#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_MASK)
uniform float minimum_alpha;
#endif

#ifdef HAS_NORMAL_MAP
in vec3 vary_normal;
in vec3 vary_tangent;
flat in float vary_sign;
in vec2 vary_texcoord1;
#else
in vec3 vary_normal;
#endif

in vec4 vertex_color;
in vec2 vary_texcoord0;

// get the transformed normal and apply glossiness component from normal map
vec3 getNormal(inout float glossiness)
{
#ifdef HAS_NORMAL_MAP
    vec4 vNt = texture(bumpMap, vary_texcoord1.xy);
    glossiness *= vNt.a;
    vNt.xyz = vNt.xyz * 2 - 1;
    float sign = vary_sign;
    vec3 vN = vary_normal;
    vec3 vT = vary_tangent.xyz;

    vec3 vB = sign * cross(vN, vT);
    vec3 tnorm = normalize( vNt.x * vT + vNt.y * vB + vNt.z * vN );

    return tnorm;
#else
    return normalize(vary_normal);
#endif
}

vec4 getSpecular()
{
#ifdef HAS_SPECULAR_MAP
    vec4 spec = texture(specularMap, vary_texcoord2.xy);
    spec.rgb *= specular_color.rgb;
#else
    vec4 spec = vec4(specular_color.rgb, 1.0);
#endif
    return spec;
}

void alphaMask(float alpha)
{
#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_MASK)
    // Comparing floats cast from 8-bit values, produces acne right at the 8-bit transition points
    float bias = 0.001953125; // 1/512, or half an 8-bit quantization
    if (alpha < minimum_alpha-bias)
    {
        discard;
    }
#endif
}

void applyWaterClip()
{
#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)
    waterClip(vary_position.xyz);
#endif
}

float getEmissive(vec4 diffcol)
{
#if (DIFFUSE_ALPHA_MODE != DIFFUSE_ALPHA_MODE_EMISSIVE)
    return emissive_brightness;
#else
    return max(diffcol.a, emissive_brightness);
#endif
}

float getShadow(vec3 pos, vec3 norm)
{
#ifdef HAS_SUN_SHADOW
    #if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)
        return sampleDirectionalShadow(pos, norm, vary_texcoord0.xy);
    #else
        return 1.0;
    #endif
#else
    return 1.0;
#endif
}

void main()
{
    mirrorClip(vary_position);
    materializeClip(vary_position, materialize_plane, materialize_params); // <plus> plus/materialize (research/256) </plus>
    applyWaterClip();

    // diffcol == diffuse map combined with vertex color
    vec4 diffcol = texture(diffuseMap, vary_texcoord0.xy);
    diffcol.rgb *= vertex_color.rgb;
    alphaMask(diffcol.a);

#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)
    // <plus> depth of field behind alpha (plus/dofalpha): the depth pass, as the
    // plain alpha's (fullbrightF.glsl with minimum_alpha); its colour is not written
    if (plus_dof_alpha_cutoff > 0.0)
    {
        if (diffcol.a * vertex_color.a < plus_dof_alpha_cutoff)
        {
            discard;
        }
        frag_color = vec4(0.0);
        return;
    }
    // </plus>
#endif

    // spec == specular map combined with specular color
    vec4 spec = getSpecular();
    float env = env_intensity * spec.a;
    float glossiness = specular_color.a;
    vec3 norm = getNormal(glossiness);

    float emissive = getEmissive(diffcol);

#if (DIFFUSE_ALPHA_MODE == DIFFUSE_ALPHA_MODE_BLEND)
    //forward rendering, output lit linear color
    diffcol.rgb = srgb_to_linear(diffcol.rgb);
    spec.rgb = srgb_to_linear(spec.rgb);
    spec.a = glossiness; // pack glossiness into spec alpha for lighting functions

    vec3 pos = vary_position;

#ifdef MATERIAL_EARLY_OUT
    float shadow = 1.0;     // taken below where it counts
#else
    float shadow = getShadow(pos, norm);
#endif

    vec4 diffuse = diffcol;

    vec3 color = vec3(0,0,0);

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;

#define LIGHT_LOOP(i) light.rgb += calcPointLightOrSpotLight(light_diffuse[i].rgb, npos, diffuse.rgb, spec, pos.xyz, norm.xyz, light_position[i], light_direction[i].xyz, light_attenuation[i].x, light_attenuation[i].y, light_attenuation[i].z, glare, light_attenuation[i].w );

#ifdef MATERIAL_EARLY_OUT
    // The alpha written below is max(max(diffcol.a, glare) * vertex_color.a, 0),
    // and glare only grows in the env > 0 branch and in the point lights, which
    // read nothing the sun, probes, shadow or haze make. So where no fragment
    // of the quad (helpers aside) has env > 0, the point lights go first, as
    // the same calls on the same values with glare starting at 0 as below,
    // and give that alpha exactly. A quad whose fragments all end at alpha <= 0
    // (or are helpers) stops: alpha 0 under the draw's SOURCE_ALPHA,
    // ONE_MINUS_SOURCE_ALPHA (color) and ZERO, ONE_MINUS_SOURCE_ALPHA (alpha)
    // leaves the target as it was, and an OIT node with alpha 0 leaves the
    // resolve's color as it was too (so the node is still written, as are depth
    // writes where the pass has them: return, not discard). The quad goes
    // together, so the lights' lightFunc taps see the derivatives they did.
    vec3 npos = normalize(-pos.xyz);
    vec3 light = vec3(0, 0, 0);
    float glare = 0.0;
    bool dead = helperInvocationEXT();
    bool lights_first = llrhiQuadAll(dead || !(env > 0.0));
    if (lights_first)
    {
        LIGHT_LOOP(1)
        LIGHT_LOOP(2)
        LIGHT_LOOP(3)
        LIGHT_LOOP(4)
        LIGHT_LOOP(5)
        LIGHT_LOOP(6)
        LIGHT_LOOP(7)

        float final_glare = glare * (1.0 - emissive);
        final_glare = min(final_glare, 1.0);
        float final_al = max(diffcol.a, final_glare) * vertex_color.a;
#ifdef LLRHI_MATERIAL_CUTOFF
        // LLRHI (research/59, chosen 2026-10-11): also alpha below 0.004, the
        // alpha pool's minimum_alpha that alphaF discards below. Not exact: drops
        // contributions under about one display step (the rain's faint noise).
        dead = dead || final_al < 0.004;
#else
        dead = dead || final_al <= 0.0;
#endif
        if (llrhiQuadAll(dead))
        {
            frag_color = vec4(0.0);
            return;
        }
    }

    // The sun shadow counts through min(da, shadow), which is 0 for any
    // shadow >= 0 (the cascade weights are > 0) where da is 0 outside classic
    // mode, and in the sun's specular term, which runs where glossiness > 0
    // (nl and nh are clamped above 0) and adds 0 when spec.rgb is 0. A quad
    // takes it when a live fragment of it needs it, all four in step, so the
    // taps' derivatives are those they were (it came first before).
    {
        bool want_shadow = !dead && (classic_mode > 0 || !(clamp(dot(norm.xyz, light_dir.xyz), 0.0, 1.0) <= 0.0) ||
                                     (glossiness > 0.0 && any(notEqual(spec.rgb, vec3(0.0)))));
        bool want_h = subgroupQuadSwapHorizontal(want_shadow);
        bool want_v = subgroupQuadSwapVertical(want_shadow);
        bool want_d = subgroupQuadSwapDiagonal(want_shadow);
        if (want_shadow || want_h || want_v || want_d)
        {
            shadow = getShadow(pos, norm);
        }
    }
#endif

    float bloom = 0.0;
    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos.xyz, norm.xyz, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
        sunlit *= 1.35;
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3 ambenv = amblit;
    vec3 glossenv = vec3(0.0);
    vec3 legacyenv = vec3(0.0);
    sampleReflectionProbesLegacy(ambenv, glossenv, legacyenv, pos.xy*0.5+0.5, pos.xyz, norm.xyz, glossiness, env, true, amblit_linear);

    color = ambenv;

    float da          = clamp(dot(norm.xyz, light_dir.xyz), 0.0, 1.0);
    if (classic_mode > 0)
    {
        da = pow(da,1.2);
        vec3 sun_contrib = vec3(min(da, shadow));

        color.rgb = srgb_to_linear(color.rgb * 0.9 + linear_to_srgb(sun_contrib) * sunlit_linear * 0.7);
        sunlit_linear = srgb_to_linear(sunlit_linear);
    }
    else
    {
        vec3 sun_contrib = min(da, shadow) * sunlit_linear;
        color.rgb += sun_contrib;
    }

    color *= diffcol.rgb;

    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

#ifndef MATERIAL_EARLY_OUT
    float glare = 0.0;
#endif

    if (glossiness > 0.0)
    {
        vec3  lv = light_dir.xyz;
        vec3  h, l, v = -normalize(pos.xyz);
        float nh, nl, nv, vh, lightDist;
        vec3 n = norm.xyz;
        calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

        if (nl > 0.0 && nh > 0.0)
        {
            float lit = min(nl*6.0, 1.0);

            float sa = nh;
            float fres = pow(1 - vh, 5) * 0.4+0.5;
            float gtdenom = 2 * nh;
            float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

            float scol = shadow*fres*texture(lightFunc, vec2(nh, glossiness)).r*gt/(nh*nl);
            color.rgb += lit*scol*sunlit_linear.rgb*spec.rgb;
        }

        // add radiance map
        applyGlossEnv(color, glossenv, spec, pos.xyz, norm.xyz);
    }

    color = mix(color.rgb, diffcol.rgb, emissive);

    if (env > 0.0)
    {  // add environmentmap
        applyLegacyEnv(color, legacyenv, spec, pos.xyz, norm.xyz, env);

        float cur_glare = max(max(legacyenv.r, legacyenv.g), legacyenv.b);
        cur_glare = clamp(cur_glare, 0, 1);
        cur_glare *= env;
        glare += cur_glare;
    }

#ifdef MATERIAL_EARLY_OUT
    if (!lights_first)
#else
    vec3 npos = normalize(-pos.xyz);
    vec3 light = vec3(0, 0, 0);
#endif
    {
    LIGHT_LOOP(1)
        LIGHT_LOOP(2)
        LIGHT_LOOP(3)
        LIGHT_LOOP(4)
        LIGHT_LOOP(5)
        LIGHT_LOOP(6)
        LIGHT_LOOP(7)
    }

    color += light;

    color.rgb = applySkyAndWaterFog(pos.xyz, additive, atten, vec4(color, 1.0)).rgb;

    glare *= 1.0-emissive;
    glare = min(glare, 1.0);
    float al = max(diffcol.a, glare) * vertex_color.a;
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;
    frag_color = max(vec4(color * final_scale, al), vec4(0));

#else // mode is not DIFFUSE_ALPHA_MODE_BLEND, encode to gbuffer
    // deferred path               // See: C++: addDeferredAttachment(), shader: softenLightF.glsl

    float flag = GBUFFER_FLAG_HAS_ATMOS;

    frag_data[0] = max(vec4(diffcol.rgb, emissive), vec4(0));        // gbuffer is sRGB for legacy materials
    frag_data[1] = max(vec4(spec.rgb, glossiness), vec4(0));           // XYZ = Specular color. W = Specular exponent.
    frag_data[2] = encodeNormal(norm, env, flag);   // XY = Normal.  Z = Env. intensity. W = 1 skip atmos (mask off fog)

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0, 0, 0, 0);
#endif

    // <plus> plus/materialize (research/256): the band's edge (and the wireframe's
    // lines) glows, in the emissive target (softenLightF.glsl adds it to legacy faces) or, without
    // one, as fullbright albedo. Not in the blended path above (alpha is not banded).
    vec4 materialize_edge = materializeEdge(vary_position, materialize_plane, materialize_params, materialize_color.rgb);
#if defined(HAS_EMISSIVE)
    frag_data[3].rgb += materialize_edge.rgb;
#else
    frag_data[0] = materializeLegacyAlbedo(frag_data[0], materialize_edge);
#endif
    // </plus>

#endif
}
