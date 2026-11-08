/**
 * @file starsF.glsl
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

out vec4 frag_data[4];

in vec4 vertex_color;
in vec2 vary_texcoord0;
in vec2 screenpos;
in vec4 vary_star; // <plus> plus/starfield </plus>

uniform sampler2D diffuseMap;
uniform float blend_factor;
uniform float custom_alpha;
uniform float time;
// <plus> plus/starfield (research/176)
uniform float star_field;   // 0: the official look
uniform float star_nopost;  // 1: the picture is only clamped to 1 (no tone mapping)
uniform float star_gain;    // PlusSkyStarGain, for the stars (the patches have it in vary_star.x)
uniform vec2 star_line;     // plus/realstars: the constellation lines' brightness, width in pixels (starsV.glsl)
// plus/skyscreen: what the scene does to the sky's colour before the screen (skyscreen.h)
uniform float plus_sky_exposure;    // RenderExposure, or 0 under "no post": no exposure, no curve
uniform float plus_sky_hdr_scale;   // sky_hdr_scale
uniform float plus_sky_tonemap_mix; // the curve's share
uniform float plus_sky_aces;        // 1: the curve is ACES Hill
uniform sampler2D exposureMap;
// research/288: the constellation pictures, a layer each (one grey channel: the drawing on black)
uniform sampler2DArray plus_star_art_map;
// </plus>

float twinkle(){
    float d = fract(screenpos.x + screenpos.y);
    return abs(d);
}

// See:
// ALM off: class1/environment/starsF.glsl
// ALM on : class1/deferred/starsF.glsl
// <plus> plus/starfield: a star as a bright Gaussian core with a faint wide halo, a Milky Way
// patch as a soft round spot; vary_star.x is the light at the centre (for a patch already
// scaled by the gain), .y the core's sigma in pixels, .z 1 for a patch, .w (a star) the factor
// on its brightness shown: its narrower core's and its twinkle's.
// linear light to sRGB, for light that may be above 1 (linear_to_srgb() clamps)
vec3 starEncode(vec3 cl)
{
    vec3 low_range = cl * 12.92;
    vec3 high_range = 1.055 * pow(max(cl, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high_range, low_range, vec3(lessThan(cl, vec3(0.0031308))));
}

// The star's brightness on the screen (linear, 0 .. STAR_SHOWN_MAX) from its light v
// (research/176 section 9). The light is compressed alone, so the magnitudes stay apart
// whatever the sky: brightness = STAR_TOP * v / (v + STAR_KNEE), times sqrt(star_brightness's
// factor) and the gain, then a soft knee above STAR_SHOWN_KNEE. Magnitude 1 is about 9 times
// magnitude 6 at any star_brightness (13 times with the ranks below).
const float STAR_KNEE = 9.5;
const float STAR_TOP = 1.18;
const float STAR_SHOWN_KNEE = 0.8;
const float STAR_SHOWN_MAX = 0.95;

// The ranks (2026-10-25, research/176 section 12): the compression all but evened the bright
// stars with the many faint ones, and the constellations could not be told without their lines.
// On top of it, the stars of magnitude 1 and brighter are shown 1.3 times, those of magnitude 6
// 0.78 times, smoothly between on the log of the light (about 1 at magnitude 3.5): magnitude 1
// is now about 13 times magnitude 6, and the bright stars come out of the field. v at magnitude
// 6 is 0.535 (log2 -0.9), at magnitude 1 9.5 (log2 3.25).
const float STAR_RANK_FAINT = 0.78;
const float STAR_RANK_BRIGHT = 1.3;
// The faintest (2026-10-26, fix-r4: the faintest stars darker): the stars of
// magnitude 6.5 (log2 v -1.3) and fainter about 0.55 times more, magnitude 6 (log2 v -0.9) about
// 0.69, 5.5 0.87, and from magnitude 5 (log2 v 0) on as they were (0.83 of log2 v a magnitude). fix-r2's
// pixel by pixel undoing of the curve gave the faint stars about 1.2 times their light on the
// screen; this takes that back too (0.55 x 1.2: about two thirds of what they were before it).
// Their core's width is not touched (sigma's floor in starsV.glsl), so they do not shimmer.
const float STAR_FAINTEST = 0.55;

float starRank(float v)
{
    float l = log2(max(v, 1e-6));
    return mix(STAR_RANK_FAINT, STAR_RANK_BRIGHT, smoothstep(-0.9, 3.25, l))
         * mix(STAR_FAINTEST, 1.0, smoothstep(-1.4, 0.0, l));
}

float starShown(float v, float factor)
{
    float d = STAR_TOP * v / (v + STAR_KNEE) * starRank(v) * sqrt(factor) * star_gain;
    float over = STAR_SHOWN_MAX - STAR_SHOWN_KNEE;
    return d < STAR_SHOWN_KNEE ? d : STAR_SHOWN_KNEE + over * (1.0 - exp(-(d - STAR_SHOWN_KNEE) / over));
}

// plus/skyscreen (keep in step with cloudsF.glsl's copy): the light to write for the linear
// brightness d (0 .. 0.95) to come out on the screen. The scene takes the sky as sRGB and
// scales it by sky_hdr_scale (softenLightF.glsl) and, unless "no post", by the exposure, then
// puts it through the tone curve mixed with the linear light (tonemapUtilF.glsl). All of it is
// undone here: ACES Hill (RRTAndODTFit, the default curve; its toe crushes the dark and its
// shoulder whitens the bright) by Newton's method on the mixed curve; another curve is taken
// as straight (they are near enough in the dark, where it matters).
float plusSkyLight(float d)
{
    float x = d;
    if (plus_sky_exposure > 0.0)
    {
        float m = plus_sky_tonemap_mix * plus_sky_aces;
        if (m > 0.0)
        {
            // the start: ACES alone (at or above the root for the mixed curve)
            float qa = 1.0 - 0.983729 * d;     // > 0: d <= 0.95
            float qb = 0.0245786 - 0.4329510 * d;
            float qc = 0.000090537 + 0.238081 * d;
            x = (-qb + sqrt(qb * qb + 4.0 * qa * qc)) / (2.0 * qa);
            for (int i = 0; i < 4; ++i)
            {
                float na = x * (x + 0.0245786) - 0.000090537;
                float nb = x * (0.983729 * x + 0.4329510) + 0.238081;
                float f = (1.0 - m) * x + m * na / nb - d;
                float slope = (1.0 - m) + m * ((2.0 * x + 0.0245786) * nb - na * (2.0 * 0.983729 * x + 0.4329510)) / (nb * nb);
                x = max(x - f / max(slope, 1e-3), 0.0);
            }
        }
        x /= max(plus_sky_exposure * texture(exposureMap, vec2(0.5, 0.5)).r, 1e-4);
    }
    return x / max(plus_sky_hdr_scale, 1e-3);
}

// The light to write for a star of the brightness d.
float starLight(float d)
{
    float x = plusSkyLight(d);
#if !defined(HAS_EMISSIVE)
    x = min(x, 1.0);    // the colour buffer is clamped to 1
#endif
    return x;
}

vec3 newStar()
{
    float r2 = dot(vary_texcoord0, vary_texcoord0);
    float factor = smoothstep(0.0, 0.9, custom_alpha);  // the sky's star_brightness
    vec3 lin;   // linear light
    if (vary_star.z > 2.5)
    {
        // A constellation picture (plus/realstars, research/288): the picture's grey (each one's
        // brightest 0.38 .. 0.84, as Stellarium drew it) times vary_star.x, its brightness on the screen (linear) at white,
        // as faint as the sky's stars are and gone under the horizon, as the lines (starsV.glsl:
        // screenpos.x / screenpos.y is the height above the horizon, sine). Tinted as the lines.
        // A pixel too faint to show on 8 bits is left black, the curve not undone there (most
        // of a picture is black).
        float grey = texture(plus_star_art_map, vec3(vary_texcoord0, vary_star.w)).r;
        float above = smoothstep(-0.0087, 0.0, screenpos.x / max(screenpos.y, 1e-6));
        float d = vary_star.x * grey * sqrt(factor) * above;
        lin = vertex_color.rgb * (d < 1e-4 ? 0.0 : starLight(min(d, STAR_SHOWN_MAX)));
    }
    else if (vary_star.z > 1.5)
    {
        // A constellation line (plus/realstars, research/177 §2.6): vary_texcoord0.x is the
        // distance from its middle in pixels, vary_star.x its brightness on the screen (linear),
        // vary_star.y the height above the horizon (sine), where it ends. Soft edges a pixel
        // wide; a line narrower than a pixel is as much fainter.
        float width = star_line.y;
        float cover = clamp(0.5 * width + 0.5 - abs(vary_texcoord0.x), 0.0, 1.0) * min(width, 1.0);
        float above = smoothstep(-0.0087, 0.0, vary_star.y);
        float d = vary_star.x * sqrt(factor) * cover * above;
        lin = vertex_color.rgb * starLight(min(d, STAR_SHOWN_MAX));
    }
    else if (vary_star.z < 0.5)
    {
        // A star: its brightness is set at the centre, before the shape, so the profile stays
        // a Gaussian and its size has an upper bound whatever the sky's star_brightness, the
        // gain and the exposure (2026-10-24: rolling each pixel off made the bright stars big
        // round dots; capping the centre made them all the same white).
        // vary_star.w: the narrower core's keeping of the light times the twinkle (starsV.glsl)
        float shown = min(starShown(vary_star.x, factor) * vary_star.w, STAR_SHOWN_MAX);
        float s2 = vary_star.y * vary_star.y;
        // The quad's half width (starsV.glsl: 6 sigma) and a small halo at twice its sigma:
        // 2 % of the core, up to 6 % for the stars brighter than about magnitude 3.5 (from
        // log2 v 1.2, fully at magnitude 0, log2 v 4.1): they look a little larger, as the
        // centre is near the top of what is shown (research/176 section 12).
        float ext = 6.0 * vary_star.y;
        float halo = 0.02 + 0.04 * smoothstep(1.2, 4.1, log2(max(vary_star.x, 1e-6)));
        float shape = exp(-r2 / (2.0 * s2)) + halo * exp(-r2 / (8.0 * s2));
        shape *= 1.0 - smoothstep(0.6 * ext, ext, sqrt(r2));
        // The shape is the brightness shown (after the scene's curve), each pixel undone on
        // its own (2026-10-26, research star-brightness-drift): undoing the centre's alone and
        // scaling the light by the shape let the curve's toe crush the faint stars' outer
        // pixels, so a faint star dimmed and brightened as the sky's turning moved it across
        // a pixel (one pixel: all its light, four: crushed). The centre is as before; below a
        // shown 1e-4 nothing comes out on 8 bits, the curve is not undone there. The halo
        // takes the centre of a star shown at the top a little past STAR_SHOWN_MAX (up to 1.007),
        // past where plusSkyLight's undoing holds (d <= 0.95; ACES's goes to infinity at 1.0165,
        // about 7 times the light at 1.007): capped there as the lines are (fix-r5).
        float d = shown * shape;
        lin = vertex_color.rgb * (d < 1e-4 ? 0.0 : starLight(min(d, STAR_SHOWN_MAX)));
    }
    else
    {
        // A Milky Way patch: soft already. The scene takes what is written to the G buffer as
        // sRGB (softenLightF.glsl: srgb_to_linear()ed, scaled by sky_hdr_scale and clamped to
        // 11.2): its pixels are rolled off softly below 5 so the light keeps its tint, ...
        float edge = 1.0 - smoothstep(0.7, 1.0, sqrt(r2));
        float boost = 4.0 * star_nopost + (1.0 - star_nopost);
        lin = vertex_color.rgb * (vary_star.x * exp(-4.0 * r2) * edge * factor * boost);
        lin = 5.0 * (vec3(1.0) - exp(-lin / 5.0));
        // ... and without tone mapping the picture is clamped to 1 (so is the colour buffer
        // when there is no emissive one): rolled off below 1.
        float below_one = star_nopost;
#if !defined(HAS_EMISSIVE)
        below_one = 1.0;
#endif
        lin = mix(lin, vec3(1.0) - exp(-lin * 0.5), below_one);
    }

    return starEncode(lin);
}
// </plus>

void main()
{
    // <plus> plus/starfield
    if (star_field > 0.5)
    {
        vec4 star = vec4(newStar(), 1.0);
        frag_data[1] = vec4(0.0f);
        frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
        frag_data[0] = vec4(0);
        frag_data[3] = star;
#else
        frag_data[0] = star;
#endif
        return;
    }
    // </plus>

    // camera above water: class1\deferred\starsF.glsl
    // camera below water: class1\environment\starsF.glsl
    vec4 col_a = texture(diffuseMap, vary_texcoord0.xy);
    vec4 col_b = texture(diffuseMap, vary_texcoord0.xy);
    vec4 col = mix(col_b, col_a, blend_factor);
    col.rgb *= vertex_color.rgb;

    float factor = smoothstep(0.0f, 0.9f, custom_alpha);

    col.a = (col.a * factor) * 32.0f;
    col.a *= twinkle();

    frag_data[1] = vec4(0.0f);
    frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = col;
#else
    frag_data[0] = col;
#endif
}

