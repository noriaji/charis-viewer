/**
 * @file probeStaticCopyF.glsl
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

// LLRHI: copies a reflection probe face's cached static G-buffer into the
// face's G-buffer (LLPipeline::probeStaticDraw, research/44 P5): the pixels the
// cached batches covered, with their depth. Every G-buffer fragment of those
// batches writes a non-zero flag in the normal's alpha; the cache was cleared
// to 0, so 0 means nothing was drawn there. Texel for texel, no filtering, so
// the values (and the depth) come back as they were written.

out vec4 frag_data[4];

in vec2 tc;

uniform sampler2D diffuseRect;
uniform sampler2D specularRect;
uniform sampler2D normalMap;
#if defined(HAS_EMISSIVE)
uniform sampler2D emissiveRect;
#endif
uniform sampler2D depthMap;

void main()
{
    ivec2 size = textureSize(normalMap, 0);
    ivec2 texel = clamp(ivec2(tc * vec2(size)), ivec2(0), size - ivec2(1));
    vec4 norm = texelFetch(normalMap, texel, 0);
    if (norm.a == 0.0)
    {
        discard;
    }
    frag_data[0] = texelFetch(diffuseRect, texel, 0);
    frag_data[1] = texelFetch(specularRect, texel, 0);
    frag_data[2] = norm;
#if defined(HAS_EMISSIVE)
    frag_data[3] = texelFetch(emissiveRect, texel, 0);
#endif
    gl_FragDepth = texelFetch(depthMap, texel, 0).r;
}
