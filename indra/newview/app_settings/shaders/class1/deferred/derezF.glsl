/**
 * @file derezF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

// A deleted object's ghost crumbling (plus/derez, research/257), into the G-buffer as a PBR surface
// (so its glow goes into the emissive target). Before its cell starts, the surface lights up as the
// front comes near, the cuts between the cells the most; after, the cell is a block shrinking towards
// its centre (what is outside is discarded) that fades out by dithering. The normal is the face's own,
// from the derivatives (the vertex buffers of fullbright faces have none, and the blocks look cut).

out vec4 frag_data[4];

uniform sampler2D diffuseMap;       // the face's texture (sRGB)
uniform vec4 derez_color;           // rgb: the face's colour (linear), a: its alpha
uniform vec4 derez_face;            // x: 0 opaque, 1 blended (dithered), 2 masked; y: the mask's cutoff;
                                    // z: fullbright (the colour is its light)
uniform vec4 derez_emissive;        // rgb: the face's own light (a PBR material's emissive factor, linear)
uniform vec4 derez_glow;            // rgb: the glow (linear, the scene's units), w: the share of it put
                                    // into the colour when there is no emissive target

in vec3 vary_position;
in vec3 vary_world;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 c);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

// The cells' clock (derezClock.glsl, shared with derezV.glsl)
float derezHash(ivec3 c, uint salt);
float derezCellSize();
float derezTime();
vec3 derezInCells(vec3 p);
ivec3 derezCell(vec3 p);
vec3 derezCellCentre(ivec3 c);
float derezCellStart(ivec3 c, vec3 centre);
float derezFlight(float start);

const float DEREZ_EDGE = 0.06;      // the surface lights up this long before its cell starts

// An ordered 4x4 threshold (0-1) for the dithered alpha and fade-out.
float derezDither(vec2 frag)
{
    const float matrix[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                       3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 i = ivec2(mod(floor(frag), 4.0));
    return (matrix[i.y * 4 + i.x] + 0.5) / 16.0;
}

void main()
{
    // the derivatives and the texture first, in uniform control flow
    vec3 n = cross(dFdx(vary_position), dFdy(vary_position));
    float n_length = length(n);
    n = n_length > 1e-12 ? n / n_length : vec3(0.0, 0.0, 1.0);
    if (dot(n, vary_position) > 0.0)
    {
        n = -n;     // towards the eye
    }
    vec4 tex = texture(diffuseMap, vary_texcoord0);

    // the mask is cut on the texture's alpha times the face's (as pbropaqueF.glsl and the legacy
    // alpha mask shaders: their vertex colour's alpha)
    float coverage = tex.a * derez_color.a;
    float alpha = derez_face.x > 1.5 ? step(derez_face.y, coverage)
                : derez_face.x > 0.5 ? coverage
                : 1.0;

    ivec3 cell = derezCell(vary_world);
    vec3 centre = derezCellCentre(cell);
    float start = derezCellStart(cell, centre);
    float glow;
    if (derezTime() >= start)
    {
        // a block: shrinks towards the cell's centre, then fades out
        float flight = derezFlight(start);
        vec3 d = abs(vary_world - centre) / (0.5 * derezCellSize());
        if (max(d.x, max(d.y, d.z)) > 1.0 - 0.8 * flight)
        {
            discard;
        }
        alpha *= 1.0 - smoothstep(0.6, 1.0, flight);
        glow = 1.0 - 0.7 * flight;
    }
    else
    {
        // the front coming: the surface lights up, the cuts between the cells first
        float near = smoothstep(start - DEREZ_EDGE, start, derezTime());
        vec3 f = abs(fract(derezInCells(vary_world)) - 0.5) * 2.0;
        float cut = smoothstep(0.82, 1.0, max(f.x, max(f.y, f.z)));
        glow = near * (0.25 + 0.75 * cut);
    }
    if (alpha < derezDither(gl_FragCoord.xy))
    {
        discard;
    }

    vec3 base = srgb_to_linear(tex.rgb) * derez_color.rgb;
    float fullbright = derez_face.z;
    vec3 emissive = derez_glow.rgb * glow + derez_emissive.rgb + base * fullbright;
    vec3 col = base * (1.0 - fullbright);
#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(emissive, 0.0);
#else
    col = min(col + emissive * derez_glow.w, vec3(1.0));
#endif
    frag_data[0] = vec4(col, 0.0);
    frag_data[1] = vec4(1.0, 0.55, 0.0, 0.0);   // occlusion, roughness, metal
    frag_data[2] = encodeNormal(n, 0.0, GBUFFER_FLAG_HAS_PBR);
}
