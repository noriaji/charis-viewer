/**
 * @file class1\deferred\moonF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, 2020 Linden Research, Inc.
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

out vec4 frag_data[4];

uniform vec4 color;
uniform vec3 moon_dir;
uniform float moon_brightness;
uniform sampler2D diffuseMap;

in vec2 vary_texcoord0;

void main()
{
    // Restore Pre-EEP alpha fade moon near horizon
    float fade = 1.0;
    if( moon_dir.z > 0 )
        fade = clamp( moon_dir.z*moon_dir.z*4.0, 0.0, 1.0 );

    vec4 c      = texture(diffuseMap, vary_texcoord0.xy);

    // SL-14113 Don't write to depth; prevent moon's quad from hiding stars which should be visible
    // Moon texture has transparent pixels <0x55,0x55,0x55,0x00>
    if (c.a <= 2./255.) // 0.00784
    {
        discard;
    }

    // <plus> research/moon-quad-cuts-milkyway-2026-10-26: the moon is drawn after the stars and the
    // Milky Way without writing depth (lldrawpoolwlsky.cpp, renderMoonDeferred), so its alpha is
    // what covers them. Some moon textures (EEP skies) have a dark opaque background round the
    // disc: a dark texel is taken as background, it covers nothing and keeps only the little
    // light it has (as if premultiplied), so the square around the moon shows the stars. A
    // straight-alpha moon (the default one, transparent outside) is not changed: its texels are
    // bright where they are opaque.
    float bright = max(c.r, max(c.g, c.b));
    float body   = smoothstep(0.03, 0.10, bright); // 0 = dark background, 1 = the moon's face
    float cover  = c.a * body;
    if (cover <= 2./255.)
    {
        discard;
    }
    // the light the texel gives (c.rgb * c.a, straight alpha) over its cover, so that the
    // blend (src * cover + dst * (1 - cover)) adds exactly that light
    c.rgb *= moon_brightness * c.a / cover;
    c.a    = cover * fade;
    // </plus>

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0.0);
    frag_data[2] = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(c.rgb, c.a);
#else
    frag_data[0] = vec4(c.rgb, c.a);
#endif

    // Added and commented out for a ground truth.  Do not uncomment - Geenz
    //gl_FragDepth = 0.999985f;
}

