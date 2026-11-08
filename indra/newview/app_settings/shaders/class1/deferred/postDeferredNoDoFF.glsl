/**
 * @file postDeferredNoDoFF.glsl
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

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;

uniform vec2 screen_res;
in vec2 vary_fragcoord;

// <plus> plus/taa (select-flicker): the scene's depth was drawn jittered by this much (its texels; 0: not). A texel
// shows the unjittered view at its centre minus this, so the unjittered depth at a place is the jittered one this
// much further on. The HUD and UI after this pass (the selection's outlines and wireframes, the manipulators, the
// beacons) are drawn unjittered and test against the depth written here.
uniform vec2 depth_unjitter;

// Two texels further apart than this in the distance (1 - depth goes as one over it) are not one surface.
const float UNJITTER_STEP = 0.05;

// The depth at tc as the unjittered view has it: the jittered depth read bilinearly where the sample shows that
// place (exact on a flat face, whose depth is affine on the screen; the read of the nearest texel moved by up to
// half a pixel's slope each frame, and lines lying on the surface were in front in one frame, behind in the next).
// Where the four texels are not one surface (an outline, a thing in front), the one nearest that place, as before:
// a depth between the two would be neither.
float unjitteredDepth(vec2 tc)
{
    ivec2 size = textureSize(depthMap, 0);
    vec2 at = tc * vec2(size) + depth_unjitter - 0.5;
    ivec2 lo = ivec2(floor(at));
    vec2 f = at - vec2(lo);
    ivec2 last = size - ivec2(1);
    ivec2 a = clamp(lo, ivec2(0), last);
    ivec2 b = clamp(lo + ivec2(1), ivec2(0), last);
    float d00 = texelFetch(depthMap, a, 0).r;
    float d10 = texelFetch(depthMap, ivec2(b.x, a.y), 0).r;
    float d01 = texelFetch(depthMap, ivec2(a.x, b.y), 0).r;
    float d11 = texelFetch(depthMap, b, 0).r;
    vec4 room = vec4(1.0) - vec4(d00, d10, d01, d11);
    float near_room = max(max(room.x, room.y), max(room.z, room.w));
    float far_room = min(min(room.x, room.y), min(room.z, room.w));
    if (near_room - far_room > UNJITTER_STEP * far_room)
    {
        return f.y < 0.5 ? (f.x < 0.5 ? d00 : d10) : (f.x < 0.5 ? d01 : d11);
    }
    // a + t * (b - a), not mix: equal texels give their value exactly, however a driver builds mix (the lines lie on
    // the surface at its very depth and pass LEQUAL with no room to spare)
    float lower = d00 + f.x * (d10 - d00);
    float upper = d01 + f.x * (d11 - d01);
    return clamp(lower + f.y * (upper - lower), 0.0, 1.0);
}
// </plus>

//=================================
// borrowed noise from:
//  <https://www.shadertoy.com/view/4dS3Wd>
//  By Morgan McGuire @morgan3d, http://graphicscodex.com
//
float hash(float n) { return fract(sin(n) * 1e4); }
float hash(vec2 p) { return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x)))); }

float noise(float x) {
    float i = floor(x);
    float f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), u);
}

float noise(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);

    // Four corners in 2D of a tile
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    // Simple 2D lerp using smoothstep envelope between the values.
    // return vec3(mix(mix(a, b, smoothstep(0.0, 1.0, f.x)),
    //          mix(c, d, smoothstep(0.0, 1.0, f.x)),
    //          smoothstep(0.0, 1.0, f.y)));

    // Same code, with the clamps in smoothstep and common subexpressions
    // optimized away.
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

//=============================

vec3 clampHDRRange(vec3 color);


void main()
{
    vec4 diff = texture(diffuseRect, vary_fragcoord.xy);

#ifdef HAS_NOISE
    vec2 tc = vary_fragcoord.xy*screen_res*4.0;
    vec3 seed = (diff.rgb+vec3(1.0))*vec3(tc.xy, tc.x+tc.y);
    vec3 nz = vec3(noise(seed.rg), noise(seed.gb), noise(seed.rb));
    diff.rgb += nz*0.003;
#endif

    diff.rgb = clampHDRRange(diff.rgb);
    frag_color = diff;

    // <plus> plus/taa (select-flicker): unjittered while the scene was jittered; as before otherwise
    gl_FragDepth = depth_unjitter == vec2(0.0) ? texture(depthMap, vary_fragcoord.xy).r : unjitteredDepth(vary_fragcoord.xy);
    // </plus>
}

