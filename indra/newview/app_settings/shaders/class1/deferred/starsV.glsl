/**
 * @file starsV.glsl
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

uniform mat4 texture_matrix0;
uniform mat4 modelview_projection_matrix;
uniform float time;
// <plus> plus/starfield (research/176): star_field 0 is the official look
uniform float star_field;
uniform float star_glow;    // 1: drawing the Milky Way's patches, 0: the stars
uniform float star_peak;    // the light of the brightest star / patch, before tone mapping
uniform vec2 screen_res;    // the size of the target in pixels
// plus/realstars (research/177): the real sky, its stars in J2000 over the whole sphere
uniform vec3 star_zenith;   // the zenith in the buffer's frame ((0, 0, 1) for the made-up sky)
uniform float star_real;    // 1: the real sky (what is below the horizon is left out, the air dims it)
uniform vec2 star_line;     // the constellation lines (star_glow 2): x their brightness, y their width in pixels
uniform float star_art;     // research/288: the constellation pictures (star_glow 3): the brightness of a picture's white
// plus/starfield (research/176 §11): the twinkle. x its strength (PlusSkyStarsTwinkle, 0: none),
// y its clock: seconds, wrapped to STAR_TWINKLE_PERIOD (plus::starfield::TWINKLE_PERIOD)
uniform vec2 star_twinkle;
// research/224: the air's haze on the stars' dimming and reddening: 0.5 + 0.7 x the sky's haze (about 1 by default)
uniform float star_haze;
// </plus>

in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;

out vec4 vertex_color;
out vec2 vary_texcoord0;
out vec2 screenpos;
// <plus> x: light at the centre, y: the core's sigma in pixels, z: 1 for a Milky Way patch,
// w: a star's brightness multiplier for its narrower core (starsF.glsl).
// A constellation line: x its brightness, y the height above the horizon (sine), z 2.
// A constellation picture (research/288): x its brightness, z 3, w its layer; screenpos x the
// position along the zenith, y the position's length (their ratio: the height, sine).
out vec4 vary_star;

float starHash(vec3 d)
{
    return fract(sin(dot(d, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}
// </plus>

// <plus> plus/realstars: a vertex out of the picture (the clip leaves it out, so does its triangle)
const vec4 PLUS_CULLED = vec4(0.0, 0.0, 2.0, 1.0);

// The light the air leaves of a star at the height (sine) h, in the shown scale (the magnitudes
// times plus::starfield's 0.62), and its reddening: 0.2 magnitudes for each air mass past the
// first (Rozenberg's air mass, about 40 at the horizon), the blue dimmed the most.
float airLight(float h)
{
    float u = max(h, 0.0);
    float airmass = 1.0 / (u + 0.025 * exp(-11.0 * u));
    return pow(10.0, -0.4 * 0.62 * 0.20 * star_haze * (airmass - 1.0));
}

vec3 airTint(float h)
{
    float u = max(h, 0.0);
    float airmass = 1.0 / (u + 0.025 * exp(-11.0 * u));
    return exp(-(airmass - 1.0) * vec3(0.0, 0.03, 0.08) * star_haze);
}

// The twinkle (scintillation, research/176 §11) of a star of the light code (0 .. 1) at the height
// (sine) h in the direction d: the factor on its brightness on the screen, and in tint a slight
// shift of its colour near the horizon (the air splits the light: a red, then a blue flash).
// Three waves a star, each of its own rate and phase from the direction (so no two stars go
// together): a slow one, a middling one and a quick one, the quick one stronger low down. The
// rates are whole turns of the period, so the clock's wrap does not show. The bright stars twinkle
// the most (the faint ones half as much), and the low ones more than those overhead: there is more air.
const float STAR_TWINKLE_PERIOD = 1024.0;   // plus::starfield::TWINKLE_PERIOD

float twinkleOf(float code, float h, vec3 d, out vec3 tint)
{
    tint = vec3(1.0);
    if (star_twinkle.x <= 0.0)
    {
        return 1.0;
    }
    float low = 1.0 - clamp(h, 0.0, 1.0);
    float air = 0.3 + 0.7 * low * low;              // 0.3 overhead, 1 at the horizon
    // 2026-10-26 (fix-r4): 0.5 the faintest (was 0.12) and 1.2 times the strength (was 0.7).
    // The faint stars swung by about 3 % and the most by about 5 % in the middle of the sky:
    // too little to see once fix-r2 took away their flicker from where they fell in a pixel
    // (which had been seen as their twinkle). Now at the default 0.6, half way up the sky,
    // about 15 % the faint ones and 30 % the bright ones (the scintillation is about the same
    // share of the light for any star; the bright ones show it the more).
    float bright = 0.5 + 0.5 * sqrt(code);          // 0.5 the faintest, 1 the brightest
    float amp = min(star_twinkle.x * 1.2 * air * bright, 0.8);
    vec3 r = vec3(starHash(d), starHash(d.yzx * 1.7 + 0.3), starHash(d.zxy * 2.3 + 0.7));
    // about 0.3, 0.8 and 2 turns a second, each a quarter either way
    vec3 turns = floor(vec3(0.3, 0.8, 2.0) * STAR_TWINKLE_PERIOD * (0.75 + 0.5 * r)) / STAR_TWINKLE_PERIOD;
    vec3 wave = sin(6.2831853 * (fract(star_twinkle.y * turns) + r.zxy));
    vec3 weight = vec3(0.55, 0.30, 0.15 + 0.20 * low);
    float n = clamp(1.5 * dot(weight, wave) / (weight.x + weight.y + weight.z), -1.0, 1.0);
    // the colour: near the horizon only, a third of the way round between the channels
    // (on the amplitude as it was before fix-r4: the colour's swing is not made larger)
    float amp_tint = min(star_twinkle.x * 0.7 * air * (0.12 + 0.88 * sqrt(code)), 0.8);
    float chroma = min(amp_tint * low * low * low * low * 0.6, 0.35);
    // its rate whole turns of the period too (bugloop round 1: 1.3 times turns.y was not, and the
    // colour jumped at each wrap of the clock)
    float tint_turns = floor(turns.y * 1.3 * STAR_TWINKLE_PERIOD) / STAR_TWINKLE_PERIOD;
    float a = 6.2831853 * fract(star_twinkle.y * tint_turns + r.x);
    tint = 1.0 + chroma * vec3(sin(a), sin(a + 2.0943951), sin(a + 4.1887902));
    return 1.0 + amp * n;
}
// </plus>

void main()
{
    //transform vertex
    vec4 pos = modelview_projection_matrix * vec4(position, 1.0);


    // smash to far clip plane to
    // avoid rendering on top of moon (do NOT write to gl_FragDepth, it's slow)
    pos.z = pos.w;

    // <plus> plus/starfield
    if (star_field > 0.5)
    {
        screenpos = vec2(0.0);
        vertex_color = diffuse_color;
        if (star_glow > 2.5)
        {
            // A constellation picture (plus/realstars, research/288 section 3.2): a flat plate on
            // the plane through the picture's three reference stars (its corners times the sky's
            // radius), on the far plane as the stars are. texcoord0 is the place in the picture
            // (0 .. 1, rows from its top), diffuse_color.rgb its tint and diffuse_color.a its layer
            // in the picture array (the layer / 255, llvowlsky.cpp). The height above the horizon
            // goes to the fragment shader unnormalized (screenpos: the position along the zenith,
            // and its length): along the plate the first is exact (linear) and so is where it
            // crosses 0, while a height worked out at the corners alone would cut a large plate
            // at the wrong place.
            screenpos = vec2(dot(position, star_zenith), length(position));
            vary_texcoord0 = texcoord0;
            vary_star = vec4(star_art, 0.0, 3.0, floor(diffuse_color.a * 255.0 + 0.5));
            gl_Position = pos;
            return;
        }
        if (star_glow > 1.5)
        {
            // A constellation line (plus/realstars, research/177 §2.6): a quad spread from the
            // segment to star_line.y pixels in clip space, its ends drawn back a little from the
            // stars. This vertex sits at one end; texcoord0 is the other's right ascension and
            // declination (J2000, radians), diffuse_color.a the side it is on.
            vec3 e = normalize(position);
            vec3 o = vec3(cos(texcoord0.y) * cos(texcoord0.x), cos(texcoord0.y) * sin(texcoord0.x), sin(texcoord0.y));
            vec4 po = modelview_projection_matrix * vec4(o * length(position), 1.0);
            vec4 pe = pos;  // its x, y and w (z is on the far plane already)
            vec2 half_res = 0.5 * screen_res;
            vec2 se = pe.xy / max(pe.w, 1e-6) * half_res;
            vec2 so = po.xy / max(po.w, 1e-6) * half_res;
            float len = length(so - se);
            // an end behind the camera, or a segment seen end on: left out
            if (pe.w <= 1e-6 || po.w <= 1e-6 || len < 0.5)
            {
                gl_Position = PLUS_CULLED;
                vary_texcoord0 = vec2(0.0);
                vary_star = vec4(0.0);
                return;
            }
            vec2 along = (so - se) / len;
            vec2 across = vec2(-along.y, along.x);
            // diffuse_color.a (plus/realstars LINE_CORNERS): bit 0 the side as seen from this
            // end, bit 1 the end (b); seen from b the sides change places
            float code = floor(diffuse_color.a * 3.0 + 0.5);
            float side = mod(code, 2.0) > 0.5 ? 1.0 : -1.0;
            float from_a = code > 1.5 ? -1.0 : 1.0;
            float half_width = 0.5 * star_line.y + 1.0;     // one pixel more for the soft edge
            float back = min(3.5, 0.3 * len);               // clear of the star
            vec2 s = se + along * back + across * (side * half_width);
            gl_Position = vec4(s / half_res * pe.w, pe.w, pe.w);
            // the distance across, signed as seen from a at both ends (else it is 0 on a slanted
            // line in each triangle and the line shows as two)
            vary_texcoord0 = vec2(side * from_a * half_width, 0.0);
            vary_star = vec4(star_line.x, dot(e, star_zenith), 2.0, 0.0);
            return;
        }
        // diffuse_color.a is the light on a log scale (plus::starfield::LEVEL_STOPS = 12)
        float code = exp2((diffuse_color.a - 1.0) * 12.0);
        vec3 d = normalize(position);
        float height = dot(d, star_zenith);     // = d.z for the made-up sky
        if (star_real > 0.5)
        {
            // the real sky: below the horizon (half a degree under it) nothing; near it, dimmer and redder
            if (star_glow < 0.5 && height < -0.0087)
            {
                gl_Position = PLUS_CULLED;
                vary_texcoord0 = vec2(0.0);
                vary_star = vec4(0.0);
                return;
            }
            code *= star_glow < 0.5 ? airLight(height) : smoothstep(-0.03, 0.03, height);
            vertex_color.rgb *= star_glow < 0.5 ? airTint(height) : vec3(1.0);
        }
        vary_star = vec4(code * star_peak, 1.0, 1.0, 0.0);
        vary_texcoord0 = texcoord0;
        if (star_glow < 0.5)
        {
            // A star is a point: a quad of a few pixels, spread in clip space so it is
            // the same size on screen at any field of view.
            // The twinkle (2026-10-25: was on the light before the compression, v / (v + 9.5),
            // which all but flattened it for the bright stars; now on the brightness shown).
            vec3 tint;
            float tw = twinkleOf(code, height, d, tint);
            vertex_color.rgb *= tint;
            // The core's sigma in pixels: about 0.52 for the faint ones, 0.9 for the
            // brightest (2026-10-24: was 0.55 .. 1.4, then 0.63 .. 0.9; the faint ones still
            // looked 2 - 3 px wide). Not below about 0.5: a narrower core than the pixel
            // makes a point's brightest pixel swing with where it falls in the pixel (at 0.5,
            // 0.37 of the centre's at a corner; its sum stays within 3 %) and the faint stars
            // would shimmer as the camera moves. The quad ends at 6 sigma (starsF.glsl).
            float sigma = 0.48 + 0.42 * sqrt(code);
            // The narrower core keeps the light of the core it replaces (0.60 + 0.30 sqrt):
            // its centre is the brighter by the ratio of the areas.
            float wide = 0.60 + 0.30 * sqrt(code);
            float keep = (wide * wide) / (sigma * sigma);
            // research/224: the size follows the screen's height (a 4K screen has twice the pixels
            // a star is seen across at 1080p; 720p and below stay at 1: the core is never narrower
            // than half a pixel). Only sigma grows: the centre is as bright as at 1080p, so the star
            // looks the same on the screen (the light summed over its pixels grows with the area, as
            // the pixels are the smaller)
            float res_scale = clamp(screen_res.y / 1080.0, 1.0, 2.5);
            sigma *= res_scale;
            // At the twinkle's peaks a bright star also swells a little (its centre may be at
            // the top of what is shown already); never narrower than its own core.
            sigma *= 1.0 + 0.25 * sqrt(code) * max(tw - 1.0, 0.0);
            float ext = 6.0 * sigma;
            pos.xy += texcoord0 * ext * 2.0 / screen_res * pos.w;
            // the twinkle on the brightness shown (starsF.glsl: after starShown())
            vary_star = vec4(code * star_peak, sigma, 0.0, keep * tw);
            vary_texcoord0 = texcoord0 * ext;
        }
        gl_Position = pos;
        return;
    }
    // </plus>

    gl_Position = pos;

    float t = mod(time, 1.25f);
    screenpos = position.xy * vec2(t, t);
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
    vertex_color = diffuse_color;
    vary_star = vec4(0.0); // <plus> </plus>
}
