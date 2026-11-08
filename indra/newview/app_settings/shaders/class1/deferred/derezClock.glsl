/**
 * @file derezClock.glsl
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

// The cells' clock of a deleted object's ghost crumbling (plus/derez, research/257): where a place's
// cell is and when it starts and flies. One file for both stages (derezV.glsl moves the vertices with
// their cells, derezF.glsl cuts the blocks and lights the front), so that they cannot drift apart.

uniform vec4 derez_grid;            // xyz: the grid's corner (agent), w: a cell's size (m)
uniform vec4 derez_shape;           // x: the box's bottom (agent z), y: 1 / its height, z: the time (0-1)

const float DEREZ_SWEEP = 0.5;      // the front's way from the top to the bottom (of the time)
const float DEREZ_JITTER = 0.12;    // a cell's start, out of order by up to this
const float DEREZ_FLY = 0.38;       // a cell's flight

float derezHash(ivec3 c, uint salt)
{
    uvec3 v = uvec3(c) * uvec3(73856093u, 19349663u, 83492791u);
    uint h = (v.x ^ v.y ^ v.z ^ salt) * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return float(h & 0xffffffu) * (1.0 / 16777216.0);
}

// A cell's size (m).
float derezCellSize()
{
    return derez_grid.w;
}

// The time (0-1).
float derezTime()
{
    return derez_shape.z;
}

// p (the agent's frame) in cells from the grid's corner.
vec3 derezInCells(vec3 p)
{
    return (p - derez_grid.xyz) / derez_grid.w;
}

ivec3 derezCell(vec3 p)
{
    return ivec3(floor(derezInCells(p)));
}

vec3 derezCellCentre(ivec3 c)
{
    return derez_grid.xyz + (vec3(c) + 0.5) * derez_grid.w;
}

// The time (0-1) the cell starts to crumble.
float derezCellStart(ivec3 c, vec3 centre)
{
    float from_top = 1.0 - clamp((centre.z - derez_shape.x) * derez_shape.y, 0.0, 1.0);
    return from_top * DEREZ_SWEEP + derezHash(c, 0u) * DEREZ_JITTER;
}

// 0 until the cell starts, then 0-1 over its flight.
float derezFlight(float start)
{
    return clamp((derez_shape.z - start) / DEREZ_FLY, 0.0, 1.0);
}
