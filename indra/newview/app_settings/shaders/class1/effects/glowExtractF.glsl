/**
 * @file glowExtractF.glsl
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

uniform sampler2D diffuseMap;
#if HAS_NOISE
uniform sampler2D glowNoiseMap;
uniform vec2 screen_res;
#endif
uniform float minLuminance;
uniform float maxExtractAlpha;
uniform vec3 lumWeights;
uniform vec3 warmthWeights;
uniform float warmthAmount;
// <plus> plus/taa (research/240): the picture's pixels a glow texel covers (x, y); 0: one tap
uniform vec2 plus_glow_footprint;
// </plus>

in vec2 vary_texcoord0;

void main()
{
    // <plus> plus/taa: with the scene jittered, the average over the footprint, so that a thin glowing surface that moves
    // by half a pixel is not hit and missed by turns. The bilinear taps are at most a pixel apart (up to 8 a side), so that
    // every pixel of the footprint weighs about the same (within a few percent up to 4K wide, research/240). The colour is
    // weighted by the glow (alpha), as the additive blend below weighs it: the glowing pixels' colour, not their neighbours'.
    vec4 col;
    if (plus_glow_footprint.x > 0.0)
    {
        vec2 texel = 1.0 / vec2(textureSize(diffuseMap, 0));
        ivec2 taps = clamp(ivec2(ceil(plus_glow_footprint)), ivec2(1), ivec2(8));
        vec4 sum = vec4(0.0);
        vec3 plain = vec3(0.0);
        for (int y = 0; y < taps.y; ++y)
        {
            for (int x = 0; x < taps.x; ++x)
            {
                vec2 offset = ((vec2(x, y) + 0.5) / vec2(taps) - 0.5) * plus_glow_footprint;
                vec4 c = texture(diffuseMap, vary_texcoord0.xy + offset * texel);
                sum += vec4(c.rgb * c.a, c.a);
                plain += c.rgb;
            }
        }
        float count = float(taps.x * taps.y);
        col.a = sum.a / count;
        col.rgb = sum.a > 1e-5 ? sum.rgb / sum.a : plain / count;
    }
    else
    {
        col = texture(diffuseMap, vary_texcoord0.xy);
    }
    // </plus> (was: vec4 col = texture(diffuseMap, vary_texcoord0.xy);)
    /// CALCULATING LUMINANCE (Using NTSC lum weights)
    /// http://en.wikipedia.org/wiki/Luma_%28video%29
    float lum = smoothstep(minLuminance, minLuminance+1.0, dot(col.rgb, lumWeights ) );
    float warmth = smoothstep(minLuminance, minLuminance+1.0, max(col.r * warmthWeights.r, max(col.g * warmthWeights.g, col.b * warmthWeights.b)) );

#if HAS_NOISE
    float TRUE_NOISE_RES = 128; // See mTrueNoiseMap
    // *NOTE: Usually this is vary_fragcoord not vary_texcoord0, but glow extraction is in screen space
    vec3 glow_noise = texture(glowNoiseMap, vary_texcoord0.xy * (screen_res / TRUE_NOISE_RES)).xyz;
    // Dithering. Reduces banding effects in the reduced precision glow buffer.
    float NOISE_DEPTH = 64.0;
    col.rgb += glow_noise / NOISE_DEPTH;
    col.rgb = max(col.rgb, vec3(0));
#endif
    frag_color.rgb = col.rgb;
    frag_color.a = max(col.a, mix(lum, warmth, warmthAmount) * maxExtractAlpha);

}
