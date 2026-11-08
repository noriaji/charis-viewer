/**
 * @file class1/deferred/globalF.glsl
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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


 // Global helper functions included in every fragment shader
 // DO NOT declare sampler uniforms here as OS X doesn't compile
 // them out

uniform float mirror_flag;
uniform vec4 clipPlane;
uniform float clipSign;

void mirrorClip(vec3 pos)
{
    if (mirror_flag > 0)
    {
        if ((dot(pos.xyz, clipPlane.xyz) + clipPlane.w) < 0.0)
        {
                discard;
        }
    }
}

// <plus> plus/materialize (research/256): the band of light that turns a
// face from its loading wireframe into the surface. The uniforms are not
// declared here: this file is in every fragment program, and every uniform
// declared in it would sit in each program's default block (the Vulkan
// backend's depth-only path refuses a program with one it does not know).
// The shaders that draw the band declare them, set by name, all 0 unless a
// band is being drawn, and pass them in:
//   materialize_plane   (n.xyz, d) in eye space: s = dot(n, pos) + d is the
//                       height above the band's front (up positive)
//   materialize_params  x the mode (0 off, 1 the surface pass, 2 the line
//                       pass, 3 the waiting wireframe), y the band's half
//                       width (in units of s), z the glow's strength (the
//                       renderer caps it; in modes 2 and 3 the line's), w the
//                       ripple's seed (seconds)
//   materialize_color   rgb the edge's colour
// The surface pass drops what is above the front (s > 0) and lights the
// edge just below it (-y < s <= 0); the line pass drops the rest (s <= 0),
// so the two meet. The ripple moves s by at most MATERIALIZE_RIPPLE * y
// either way: the front reaches every fragment once it is that far past
// the top. The lines glow evenly, lit or not, so they do not go dark at
// night: the waiting wireframe (mode 3) drops nothing (no plane, no ripple),
// the line pass (mode 2) glows the same on what it keeps.

#define MATERIALIZE_SURFACE 1
#define MATERIALIZE_LINES   2
#define MATERIALIZE_WAIT    3
#define MATERIALIZE_RIPPLE  0.5

int materializeMode(vec4 params)
{
    return int(params.x + 0.5);
}

float materializeHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Value noise in [0, 1], smooth across cells.
float materializeNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float x00 = mix(materializeHash(i + vec3(0, 0, 0)), materializeHash(i + vec3(1, 0, 0)), f.x);
    float x10 = mix(materializeHash(i + vec3(0, 1, 0)), materializeHash(i + vec3(1, 1, 0)), f.x);
    float x01 = mix(materializeHash(i + vec3(0, 0, 1)), materializeHash(i + vec3(1, 0, 1)), f.x);
    float x11 = mix(materializeHash(i + vec3(0, 1, 1)), materializeHash(i + vec3(1, 1, 1)), f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}

// s at pos (eye space), with the ripple: cells of half a metre, drifting with the seed.
float materializeHeight(vec3 pos, vec4 plane, vec4 params)
{
    float s = dot(plane.xyz, pos) + plane.w;
    float ripple = materializeNoise(pos * 2.0 + vec3(0.0, 0.0, params.w * 1.5)) * 2.0 - 1.0;
    return s + ripple * MATERIALIZE_RIPPLE * params.y;
}

// Right after mirrorClip: drops the side of the band the pass does not draw.
// Modes 0 and 3 return at once (the waiting wireframe drops nothing).
void materializeClip(vec3 pos, vec4 plane, vec4 params)
{
    int mode = materializeMode(params);
    if (mode != MATERIALIZE_SURFACE && mode != MATERIALIZE_LINES)
    {
        return;
    }
    float s = materializeHeight(pos, plane, params);
    if (mode == MATERIALIZE_SURFACE ? s > 0.0 : s <= 0.0)
    {
        discard;
    }
}

// A line's glow (modes 2 and 3): the colour at the strength, on every
// fragment of the line; a (the fullbright amount without the emissive
// target) follows the strength up to 1, so a dim pulse does not paint the
// line black.
vec4 materializeLineGlow(vec4 params, vec3 color)
{
    float strength = max(params.z, 0.0);
    return vec4(color * strength, min(strength, 1.0));
}

// The glow the pass adds at pos: rgb linear light, a how far into the edge
// (1 at the front, 0 at y below it), the fullbright amount without the
// emissive target. The surface pass lights the edge, near the front most
// of the way to white (the band's hot core); the lines glow throughout
// (materializeLineGlow); mode 0 adds 0.
vec4 materializeEdge(vec3 pos, vec4 plane, vec4 params, vec3 color)
{
    int mode = materializeMode(params);
    if (mode == MATERIALIZE_LINES || mode == MATERIALIZE_WAIT)
    {
        return materializeLineGlow(params, color);
    }
    float half_width = params.y;
    if (mode != MATERIALIZE_SURFACE || half_width <= 0.0)
    {
        return vec4(0.0);
    }
    float t = clamp(1.0 + materializeHeight(pos, plane, params) / half_width, 0.0, 1.0);
    float glow = t * t;
    vec3 tint = mix(color, vec3(1.0), 0.7 * smoothstep(0.8, 1.0, t));
    return vec4(tint * (params.z * glow), glow);
}

// srgbF.glsl's linear_to_srgb, the same curve (softenLightF.glsl's
// srgb_to_linear undoes it exactly). Not that function itself: srgbF.glsl
// is not linked into every program that draws the band (the alpha mask and
// bump programs have none of the features that attach it).
vec3 materializeLinearToSrgb(vec3 cl)
{
    cl = clamp(cl, vec3(0), vec3(1));
    vec3 low_range  = cl * 12.92;
    vec3 high_range = 1.055 * pow(cl, vec3(0.41666)) - 0.055;
    return mix(high_range, low_range, lessThan(cl, vec3(0.0031308)));
}

// A legacy (sRGB) G-buffer without the emissive target: the edge as a
// fullbright colour in the albedo (alpha: the fullbright amount), capped
// at 1. albedo comes back as it was where the edge is 0.
vec4 materializeLegacyAlbedo(vec4 albedo, vec4 edge)
{
    if (edge.a <= 0.0)
    {
        return albedo;
    }
    vec3 glow = materializeLinearToSrgb(edge.rgb);
    return vec4(mix(albedo.rgb, glow, edge.a), max(albedo.a, edge.a));
}
// </plus>

vec4 encodeNormal(vec3 n, float env, float gbuffer_flag)
{
    float f = sqrt(8 * n.z + 8);
    return vec4(n.xy / f + 0.5, env, gbuffer_flag);
}

vec4 decodeNormal(vec4 norm)
{
    vec2 fenc = norm.xy*4-2;
    float f = dot(fenc,fenc);
    float g = sqrt(1-f/4);
    vec4 n;
    n.xy = fenc*g;
    n.z = 1-f/2;
    return n;
}
