/**
 * @file sunLightF.glsl
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

//class 2, shadows, no SSAO

// Inputs
in vec2 vary_fragcoord;

uniform vec3 sun_dir;
uniform float shadow_bias;

vec4 getNorm(vec2 pos_screen);
vec4 getPosition(vec2 pos_screen);

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen);

#if defined(PLUS_CLOUD_SHADOW)
// <plus> plus/volclouds (research/186 chapter 16.3, research/253 3.6): the volumetric clouds' shadow on
// what the sun light map is for. The same in sunLightF.glsl and sunLightSSAOF.glsl, and what
// plus::volclouds::AirShadow tells the volumetric light: keep them in step.
uniform sampler2D plus_cloud_map;   // 1 - exp(-optical depth) of the layer above a square of the ground (cloudShadowMapF.glsl), with mips
uniform vec4 plus_cloud_shadow;     // x: strength, with the low light's fade (0: none), y: 1 / the map's width (1 / m), z: the layer's middle above the camera (m)
uniform vec2 plus_cloud_light;      // the way to the light (the sun; the moon while the sun is down): metres east and north per metre up
uniform vec2 plus_cloud_map_centre; // the map's centre, east and north of the camera (m)
uniform vec3 plus_cloud_view_x;     // the view space's axes in east, north, up (the columns of the turn from view space)
uniform vec3 plus_cloud_view_y;
uniform vec3 plus_cloud_view_z;

// How much of the light reaches pos (view space): the map where the way to the light crosses the
// layer's middle.
float plusCloudShadow(vec3 pos)
{
    if (plus_cloud_shadow.x <= 0.0)
    {
        return 1.0;
    }
    vec3 rel = mat3(plus_cloud_view_x, plus_cloud_view_y, plus_cloud_view_z) * pos;   // from the camera
    float rise = plus_cloud_shadow.z - rel.z;
    if (rise <= 0.0)
    {
        return 1.0;                                 // above the layer's middle
    }
    vec2 uv = (rel.xy + plus_cloud_light * rise - plus_cloud_map_centre) * plus_cloud_shadow.y + 0.5;
    float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    float c = textureLod(plus_cloud_map, uv, 0.0).x * smoothstep(0.0, 0.1, edge);   // the map itself (its mips are its mean)
    return 1.0 - plus_cloud_shadow.x * c;
}
// </plus>
#endif

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    vec4 pos        = getPosition(pos_screen);
    vec4 norm       = getNorm(pos_screen);

    vec4 col;
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
#if defined(PLUS_CLOUD_SHADOW)
    col.r *= plusCloudShadow(pos.xyz); // <plus> plus/volclouds (research/186) </plus>
#endif
    col.g = 1.0f;
    col.b = sampleSpotShadow(pos.xyz, norm.xyz, 0, pos_screen);
    col.a = sampleSpotShadow(pos.xyz, norm.xyz, 1, pos_screen);

    frag_color = clamp(col, vec4(0), vec4(1));
}
