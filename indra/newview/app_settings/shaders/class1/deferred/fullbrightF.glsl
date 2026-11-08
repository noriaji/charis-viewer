/**
 * @file deferred/fullbrightF.glsl
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

out vec4 frag_color;

#if !defined(HAS_DIFFUSE_LOOKUP)
uniform sampler2D diffuseMap;
#endif

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cl);

#ifdef HAS_ALPHA_MASK
uniform float minimum_alpha;
// <plus> focus through glass (plus/dofalpha, research/218): above 0 only in the alpha
// pool's depth pass for depth of field, for the world's batches (no colour is written
// there): fragments less opaque than it (texture alpha times vertex alpha, a pane's
// transparency) write no depth. 0 (every colour pass) changes nothing.
uniform float plus_dof_alpha_cutoff;
// </plus>
#endif

#ifdef IS_ALPHA
uniform vec4 waterPlane;
void waterClip(vec3 pos);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
#endif

void mirrorClip(vec3 pos);
// <plus> plus/materialize (research/256): set by name, all 0 unless a band
// is drawn; the helpers are in globalF.glsl. Not on HUDs
#ifndef IS_HUD
uniform vec4 materialize_plane;
uniform vec4 materialize_params;
uniform vec4 materialize_color;
void materializeClip(vec3 pos, vec4 plane, vec4 params);
vec4 materializeEdge(vec3 pos, vec4 plane, vec4 params, vec3 color);
#endif
// </plus>

void main()
{
    mirrorClip(vary_position);
    // <plus> plus/materialize (research/256): not on HUDs
#ifndef IS_HUD
    materializeClip(vary_position, materialize_plane, materialize_params);
#endif
    // </plus>
#ifdef IS_ALPHA
    waterClip(vary_position.xyz);
#endif

#ifdef HAS_DIFFUSE_LOOKUP
    vec4 color = diffuseLookup(vary_texcoord0.xy);
#else
    vec4 color = texture(diffuseMap, vary_texcoord0.xy);
#endif

    float final_alpha = color.a * vertex_color.a;

#ifdef HAS_ALPHA_MASK
    if (color.a < minimum_alpha)
    {
        discard;
    }
    // <plus> focus through glass (plus/dofalpha): the depth pass, colour not written
    if (plus_dof_alpha_cutoff > 0.0)
    {
        if (final_alpha < plus_dof_alpha_cutoff)
        {
            discard;
        }
        frag_color = vec4(0.0);
        return;
    }
    // </plus>
#endif

    color.rgb *= vertex_color.rgb;

    vec3 pos = vary_position;

    color.a = final_alpha;
#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
#ifdef IS_ALPHA

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

#endif

    // <plus> plus/materialize (research/256): the band's edge (and the wireframe's
    // lines) glows, in linear light as the colour is here (inside the IS_HUD guard: not on HUDs)
    color.rgb += materializeEdge(vary_position, materialize_plane, materialize_params, materialize_color.rgb).rgb;
    // </plus>
#endif

    frag_color = max(color, vec4(0));
}

