/**
 * @file skyMilkyF.glsl
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

// The baked Milky Way (plus/starfield/skymilky.h, research/224): the picture is an equirectangular
// RGBA16F of the whole sky in the star field's frame, linear light, its brightest about 1 (starfieldmath.h
// bake()); u = 0.5 + atan2(y, x) / 2 pi, v = 0.5 + asin(z) / pi, row 0 at z = -1.

out vec4 frag_data[4];

in vec3 vary_dir;

uniform sampler2D environmentMap;   // the picture
uniform float custom_alpha;         // the sky's star_brightness / 500
uniform vec3 star_zenith;           // the zenith in the picture's frame ((0, 0, 1) for the made-up sky)
uniform float star_real;            // 1: the real sky (the air dims and reddens what is low)
uniform float star_haze;            // 0.5 + 0.7 x the sky's haze: the air's dimming, as the stars' (starsV.glsl)
uniform float milky_shown;          // the brightest texel as it is to show on the screen (linear)
// plus/skyscreen: what the scene does to the sky's colour before the screen (skyscreen.h)
uniform float plus_sky_exposure;    // RenderExposure, or 0 under "no post": no exposure, no curve
uniform float plus_sky_hdr_scale;   // sky_hdr_scale
uniform float plus_sky_tonemap_mix; // the curve's share
uniform float plus_sky_aces;        // 1: the curve is ACES Hill
uniform sampler2D exposureMap;

// as starsF.glsl: the brightest the shown light can be, where the undoing of the curve holds
const float MILKY_SHOWN_MAX = 0.95;

// linear light to sRGB, for light that may be above 1 (linear_to_srgb() clamps)
vec3 milkyEncode(vec3 cl)
{
    vec3 low_range = cl * 12.92;
    vec3 high_range = 1.055 * pow(max(cl, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high_range, low_range, vec3(lessThan(cl, vec3(0.0031308))));
}

// The light to write for the linear brightness d (0 .. 0.95, a channel each) to come out on the
// screen: starsF.glsl's plusSkyLight() on three channels (keep in step with it and with
// cloudsF.glsl's copy). The scene takes the sky as sRGB and scales it by sky_hdr_scale and, unless
// "no post", by the exposure, then puts it through the tone curve mixed with the linear light; all
// of it is undone here (ACES Hill by Newton's method on the mixed curve, another curve as straight).
vec3 milkySkyLight(vec3 d)
{
    vec3 x = d;
    if (plus_sky_exposure > 0.0)
    {
        float m = plus_sky_tonemap_mix * plus_sky_aces;
        if (m > 0.0)
        {
            vec3 qa = vec3(1.0) - 0.983729 * d;
            vec3 qb = vec3(0.0245786) - 0.4329510 * d;
            vec3 qc = vec3(0.000090537) + 0.238081 * d;
            x = (-qb + sqrt(qb * qb + 4.0 * qa * qc)) / (2.0 * qa);
            for (int i = 0; i < 4; ++i)
            {
                vec3 na = x * (x + 0.0245786) - 0.000090537;
                vec3 nb = x * (0.983729 * x + 0.4329510) + 0.238081;
                vec3 f = (1.0 - m) * x + m * na / nb - d;
                vec3 slope = (1.0 - m) + m * ((2.0 * x + 0.0245786) * nb - na * (2.0 * 0.983729 * x + 0.4329510)) / (nb * nb);
                x = max(x - f / max(slope, vec3(1e-3)), vec3(0.0));
            }
        }
        x /= max(plus_sky_exposure * texture(exposureMap, vec2(0.5, 0.5)).r, 1e-4);
    }
    return x / max(plus_sky_hdr_scale, 1e-3);
}

// The light the air leaves of the Milky Way at the height (sine) h, and its reddening: the
// stars' own (starsV.glsl airLight() / airTint(): 0.2 magnitudes for each air mass past the
// first, Rozenberg's air mass, the blue dimmed the most), scaled by the haze.
vec3 milkyAir(float h)
{
    float u = max(h, 0.0);
    float airmass = 1.0 / (u + 0.025 * exp(-11.0 * u));
    float light = pow(10.0, -0.4 * 0.62 * 0.20 * star_haze * (airmass - 1.0));
    return light * exp(-(airmass - 1.0) * vec3(0.0, 0.03, 0.08) * star_haze);
}

void main()
{
    vec3 d = normalize(vary_dir);

    // the place in the picture. The longitude wraps: where atan() jumps (u from 1 to 0) the
    // derivatives would be huge and pick the smallest mip, a line down the sky; the other
    // measure of u (shifted half a turn) has its jump elsewhere: the smaller derivative of the two
    vec2 uv = vec2(0.5 + atan(d.y, d.x) * 0.15915494, 0.5 + asin(clamp(d.z, -1.0, 1.0)) * 0.31830989);
    vec2 uv_shift = vec2(fract(uv.x + 0.5) - 0.5, uv.y);
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    vec2 dx_shift = dFdx(uv_shift);
    vec2 dy_shift = dFdy(uv_shift);
    bool shifted = dot(dx_shift, dx_shift) + dot(dy_shift, dy_shift) < dot(dx, dx) + dot(dy, dy);
    vec3 picture = textureGrad(environmentMap, uv, shifted ? dx_shift : dx, shifted ? dy_shift : dy).rgb;

    // below the horizon nothing (the picture is the whole sky); the real sky's air dims and reddens what is low
    float height = dot(d, star_zenith);
    vec3 air = star_real > 0.5 ? milkyAir(height) : vec3(1.0);
    float above = smoothstep(-0.03, 0.03, height);
    float factor = smoothstep(0.0, 0.9, custom_alpha);      // the sky's star brightness, as the stars'

    vec3 shown = min(picture * air * (milky_shown * factor * above), vec3(MILKY_SHOWN_MAX));
    vec3 lin = milkySkyLight(shown);
    // Nothing shown is nothing written: the curve ACES Hill is fitted with is below zero near 0, so
    // its undoing does not give 0 for 0 (about 0.003 under the default curve) and the box, which
    // covers the whole sky, would add that to every pixel off the band, below the horizon and by
    // twilight, and the sky would step brighter as the stars' brightness (or the picture) arrives.
    // As starsF.glsl does for the stars (below a shown 1e-4 nothing comes out on 8 bits anyway).
    lin = mix(lin, vec3(0.0), vec3(lessThan(shown, vec3(1e-4))));
#if !defined(HAS_EMISSIVE)
    lin = min(lin, vec3(1.0));  // the colour buffer is clamped to 1
#endif
    vec4 light = vec4(milkyEncode(lin), 1.0);

    frag_data[1] = vec4(0.0f);
    frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = light;
#else
    frag_data[0] = light;
#endif
}
