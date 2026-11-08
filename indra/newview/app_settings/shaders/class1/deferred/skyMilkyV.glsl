/**
 * @file skyMilkyV.glsl
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

// The baked Milky Way (plus/starfield/skymilky.h, research/224): a box round the camera, turned
// with the stars (the same matrix); each pixel takes its direction from the box's position, in
// the star field's frame (J2000 for the real sky), and skyMilkyF.glsl finds it in the picture.

uniform mat4 modelview_projection_matrix;

in vec3 position;

out vec3 vary_dir;

void main()
{
    vec4 pos = modelview_projection_matrix * vec4(position, 1.0);

    // on the far plane, behind everything, as the stars are
    pos.z = pos.w;

    gl_Position = pos;
    vary_dir = position;
}
