/**
 * @file diffuseIndexedF.glsl
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

out vec4 frag_data[4];

in vec3 vary_normal;
in vec4 vertex_color;
in vec2 vary_texcoord0;
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

vec3 linear_to_srgb(vec3 c);

void main()
{
    mirrorClip(vary_position);
    materializeClip(vary_position, materialize_plane, materialize_params); // <plus> plus/materialize (research/256) </plus>
    vec3 col = vertex_color.rgb * diffuseLookup(vary_texcoord0.xy).rgb;

    vec3 spec;
    spec.rgb = vec3(vertex_color.a);

    frag_data[0] = vec4(col, 0.0);
    frag_data[1] = vec4(spec, vertex_color.a); // spec
    vec3 nvn = normalize(vary_normal);
    frag_data[2] = encodeNormal(nvn.xyz, vertex_color.a, GBUFFER_FLAG_HAS_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0, 0, 0, 0);
#endif

    // <plus> plus/materialize (research/256): the band's edge (and the wireframe's
    // lines) glows, in the emissive target (softenLightF.glsl adds it to legacy faces) or, without
    // one, as fullbright albedo
    vec4 materialize_edge = materializeEdge(vary_position, materialize_plane, materialize_params, materialize_color.rgb);
#if defined(HAS_EMISSIVE)
    frag_data[3].rgb += materialize_edge.rgb;
#else
    frag_data[0] = materializeLegacyAlbedo(frag_data[0], materialize_edge);
#endif
    // </plus>
}
