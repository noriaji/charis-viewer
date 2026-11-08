/**
 * @file objectSkinV.glsl
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

in vec4 weight4;

uniform mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT];

// <plus> hair sway (indra/newview/plus/hairsway/hairsway.h): a hair palette
// carries two more entries after its joints, at plusHairSway (0: none). A vertex
// moves by how far it hangs below the hairline plane: not at all above it (the
// hair on the scalp stays), by the middle offset from a middle depth down and by
// the tips' offset from a tips' depth down; the part into the head is cut.
in vec3 position;
uniform float plusHairSway;

vec3 plusHairSwayOffset(vec3 p)
{
    int s = clamp(int(plusHairSway + 0.5), 1, MAX_JOINTS_PER_MESH_OBJECT - 2);
    vec4 plane = matrixPalette[s][0];      // hairline: depth below it = plane.w - dot(plane.xyz, p)
    vec4 centre = matrixPalette[s][1];     // centre of the head, mark
    vec4 mid = matrixPalette[s][2];        // middle offset
    vec4 tip = matrixPalette[s + 1][0];    // tips' offset
    vec4 shape = matrixPalette[s + 1][1];  // depths: still, middle, tips; part of an inward offset kept
    if (centre.w != -98765.0 || !(shape.x < shape.y && shape.y < shape.z))
    {
        return vec3(0.0);
    }
    float d = plane.w - dot(plane.xyz, p);
    vec3 o = mid.xyz * smoothstep(shape.x, shape.y, d) + (tip.xyz - mid.xyz) * smoothstep(shape.y, shape.z, d);
    vec3 outward = p - centre.xyz;
    float len = length(outward);
    if (len > 0.0001)
    {
        outward /= len;
        o -= outward * (min(dot(o, outward), 0.0) * (1.0 - shape.w));
    }
    return o;
}
// </plus>

mat4 getObjectSkinnedTransform()
{
    int i;

    vec4 w = fract(weight4);
    vec4 index = floor(weight4);

    index = min(index, vec4(MAX_JOINTS_PER_MESH_OBJECT-1));
    index = max(index, vec4( 0.0));

    w *= 1.0/(w.x+w.y+w.z+w.w);

    int i1 = int(index.x);
    int i2 = int(index.y);
    int i3 = int(index.z);
    int i4 = int(index.w);

    mat3 mat = mat3(matrixPalette[i1])*w.x;
         mat += mat3(matrixPalette[i2])*w.y;
         mat += mat3(matrixPalette[i3])*w.z;
         mat += mat3(matrixPalette[i4])*w.w;

    vec3 trans = vec3(matrixPalette[i1][0].w,matrixPalette[i1][1].w,matrixPalette[i1][2].w)*w.x;
         trans += vec3(matrixPalette[i2][0].w,matrixPalette[i2][1].w,matrixPalette[i2][2].w)*w.y;
         trans += vec3(matrixPalette[i3][0].w,matrixPalette[i3][1].w,matrixPalette[i3][2].w)*w.z;
         trans += vec3(matrixPalette[i4][0].w,matrixPalette[i4][1].w,matrixPalette[i4][2].w)*w.w;

    mat4 ret;

    ret[0] = vec4(mat[0], 0);
    ret[1] = vec4(mat[1], 0);
    ret[2] = vec4(mat[2], 0);
    ret[3] = vec4(trans, 1.0);

    // <plus> hair sway: the skinned position moved (a translation for this vertex only)
    if (plusHairSway > 0.5)
    {
        ret[3].xyz += plusHairSwayOffset((ret * vec4(position, 1.0)).xyz);
    }
    // </plus>

    return ret;

#ifdef IS_AMD_CARD
   // If it's AMD make sure the GLSL compiler sees the arrays referenced once by static index. Otherwise it seems to optimise the storage awawy which leads to unfun crashes and artifacts.
   mat3x4 dummy1 = matrixPalette[0];
   mat3x4 dummy2 = matrixPalette[MAX_JOINTS_PER_MESH_OBJECT-1];
#endif

}

