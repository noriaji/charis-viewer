/**
 * @file derezV.glsl
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

// A deleted object's ghost crumbling (plus/derez, research/257). The object's box is cut into cells
// in the agent's frame; each cell starts to crumble at its own time (from the top down, a little out
// of order) and then flies: its vertices shrink towards the cell's centre, turn a little and rise.
// Only position and texcoord0 are read, which every face's vertex buffer has, so one pipeline serves
// every face (the vertex format is part of the pipeline's key). The normal is made in derezF.glsl.

uniform mat4 modelview_matrix;      // the camera's only (the face's own matrix is derez_model)
uniform mat4 projection_matrix;
uniform mat4 derez_model;           // the face's vertices to the agent's frame
uniform vec4 derez_face;            // w: how far the vertices move with their cells (0: a coarse mesh stays)
uniform vec4 derez_uv0;             // the base colour's texture transform (GLTF; identity for the others)
uniform vec4 derez_uv1;

in vec3 position;
in vec2 texcoord0;

out vec3 vary_position;             // eye space, moved
out vec3 vary_world;                // the agent's frame, where it was: its cell
out vec2 vary_texcoord0;

vec2 texture_transform(vec2 vertex_texcoord, vec4[2] khr_gltf_transform, mat4 sl_animation_transform);

// The cells' clock (derezClock.glsl, shared with derezF.glsl)
float derezHash(ivec3 c, uint salt);
float derezCellSize();
float derezTime();
vec3 derezInCells(vec3 p);
ivec3 derezCell(vec3 p);
vec3 derezCellCentre(ivec3 c);
float derezCellStart(ivec3 c, vec3 centre);
float derezFlight(float start);

vec3 derezTurn(vec3 v, vec3 axis, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

void main()
{
    vec3 world = (derez_model * vec4(position, 1.0)).xyz;
    vary_world = world;

    ivec3 cell = derezCell(world);
    vec3 centre = derezCellCentre(cell);
    float flight = derezFlight(derezCellStart(cell, centre));
    float e = flight * flight * derez_face.w;   // slow at first, then faster

    vec3 moved = world;
    if (e > 0.0)
    {
        vec3 h = vec3(derezHash(cell, 1u), derezHash(cell, 2u), derezHash(cell, 3u));
        vec3 axis = normalize(h - 0.5 + vec3(0.0, 0.0, 0.001));
        vec3 local = derezTurn((world - centre) * (1.0 - 0.6 * e), axis, (h.x - 0.5) * 2.4 * e);
        vec3 drift = vec3((h.y - 0.5) * 2.4, (h.z - 0.5) * 2.4, 3.0 + 3.0 * h.x) * (derezCellSize() * e);
        moved = centre + local + drift;
    }

    vec4 eye = modelview_matrix * vec4(moved, 1.0);
    vary_position = eye.xyz;
    gl_Position = projection_matrix * eye;

    vec4 uv_transform[2] = vec4[2](derez_uv0, derez_uv1);
    vary_texcoord0 = texture_transform(texcoord0, uv_transform, mat4(1.0));
}
