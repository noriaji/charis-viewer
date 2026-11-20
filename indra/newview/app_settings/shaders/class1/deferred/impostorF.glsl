/**
 * @file impostorF.glsl
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
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

#if defined(HAS_MOTION) // <plus> tu-motion-v1: frag_data[4] the motion vectors (research/373, llrhimotion.h)
out vec4 frag_data[5];
#else // </plus>
out vec4 frag_data[4];
#endif // <plus> tu-motion-v1 </plus>

uniform float minimum_alpha;


uniform sampler2D diffuseMap;
// <plus> research/344: IMPOSTOR_FLAT, the plate of a flat silhouette (PlusJellyDollFlat): the picture is the colour alone,
// the normal is a constant facing the camera, no specular. The G-buffer's other pictures do not exist then.
#if !defined(IMPOSTOR_FLAT)
uniform sampler2D normalMap;
uniform sampler2D specularMap;
#endif
// </plus>

in vec2 vary_texcoord0;

vec3 linear_to_srgb(vec3 c);

vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

void main()
{
    vec4 col = texture(diffuseMap, vary_texcoord0.xy);
// <plus> research/344: the filter mixes the colour with the clear colour (0,0,0,0) at the edge: the colour is of the covered part
#if defined(IMPOSTOR_FLAT)
    col.rgb /= max(col.a, 0.001);
#endif
// </plus>

    if (col.a < minimum_alpha)
    {
        discard;
    }

// <plus> research/344 (was: the normal and specular pictures only)
#if defined(IMPOSTOR_FLAT)
    // lit as a flat face towards the camera (eye space), the sun, the lights and the haze as any other
    frag_data[0] = vec4(col.rgb, 0.0);
    frag_data[1] = vec4(0.0);
#if defined(HAS_MOTION) // tu-motion-v1 (in the block above): no motion of its own, the camera's is filled in after the G-buffer
    frag_data[4] = vec4(0.0);
#endif
    frag_data[2] = encodeNormal(vec3(0.0, 0.0, 1.0), 0.0, GBUFFER_FLAG_HAS_ATMOS);
#else
    vec4 norm = texture(normalMap,   vary_texcoord0.xy);
    vec4 spec = texture(specularMap, vary_texcoord0.xy);

    frag_data[0] = vec4(col.rgb, 0.0);
    frag_data[1] = spec;
#if defined(HAS_MOTION) // tu-motion-v1 (in the block above): no motion of its own, the camera's is filled in after the G-buffer
    frag_data[4] = vec4(0.0);
#endif
    frag_data[2] = vec4(norm.xyz, GBUFFER_FLAG_HAS_ATMOS);
#endif
// </plus>

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0);
#endif
}
