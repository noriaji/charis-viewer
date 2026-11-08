/**
 * @file skyRainbowF.glsl
 * @brief The rainbow worked out per pixel (plus/rainbow, research/255), shared by the sky and the clouds.
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

// Linked into the WindLight sky (skyF.glsl, with and without HAS_HDRI) and the clouds (cloudsF.glsl, 2D and
// VOLUMETRIC) by llviewershadermgr.cpp; both add the same bow to what they write (plusRainbowOver), so that
// where the clouds cover the sky the bow stays in front of them (research/255 section 8 [high 1]).
//
// The bow: for each of 8 wavelengths, the primary (one reflection inside the drop) and the secondary (two)
// as Airy's theory gives them near Descartes' angle: Ai(-z)^2, z = S (theta_R - theta), S growing with the
// drops' radius as r^(2/3) (small drops: broad, white and further in; the fogbow comes out of the same
// sums). The supernumeraries follow Ai^2's asymptotic form; the sun's disc (0.53 degrees) smears them and
// widens the main lobe. Inside the primary and outside the secondary, geometric optics' brightening; none
// between them (Alexander's dark band). The wavelengths' CIE 1931 weights add up to white. Every constant
// is from research/255-rainbows-sim/rainbow_sim.py --glsl, which also checks the angles, the colours'
// order and the brightness on the three skies of research/149 (B) (results.md beside it).
//
// The light: what skyF.glsl's haze takes below the clouds (its sunlight: the sun's, by night the moon's
// x 0.7, through the air and dimmed by the cloud cover), decoded from sRGB; the bow is added to the sky's
// linear light and encoded again (the sky is written as sRGB in every look: softenLightF.glsl decodes
// it). No exposure: the sky is in its own units (research/255 [high 2]). Under "no post" a ceiling holds
// sky + bow under 1 on the screen, the bow's hue kept ([high 3]).

uniform vec4 plus_rainbow;          // x: strength (0: no bow: what is written is left as it is), y: the drops' radius (um), z: the supernumeraries' share (0 - 1), w: the secondary's share (0 - 1)
uniform vec4 plus_rainbow_twin;     // a second set of larger, flattened drops: x their radius (um, 0: none), y their bow's angle taken in at its top (rad), z their share
uniform vec4 plus_rainbow_look;     // rgb: the light's filter (sRGB, white but for the sunset kind), a: saturation (1: as worked out, 0: white)
uniform float plus_rainbow_ceiling; // "no post": the most a channel's linear light may reach (1 on the screen); 0: no ceiling

uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  blue_density;
uniform float haze_density;
uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;

vec3 srgb_to_linear(vec3 c);

// ---- research/255-rainbows-sim/rainbow_sim.py --glsl (8 wavelengths, 410 - 690 nm) ----
// Descartes' angle from the antisolar point (rad) of the primary and the secondary
const float PLUS_RB_THETA1[8] = float[8](0.711152, 0.718696, 0.724464, 0.729047,
                                         0.732803, 0.735960, 0.738667, 0.741028);
const float PLUS_RB_THETA2[8] = float[8](0.930200, 0.916558, 0.906138, 0.897867,
                                         0.891093, 0.885403, 0.880525, 0.876273);
// Airy's scale S of each, a share of the primary's at 550 nm
const float PLUS_RB_SCALE1[8] = float[8](1.234793, 1.153854, 1.085417, 1.026525,
                                         0.975150, 0.929822, 0.889456, 0.853220);
const float PLUS_RB_SCALE2[8] = float[8](0.690479, 0.645497, 0.607407, 0.574598,
                                         0.545953, 0.520664, 0.498133, 0.477901);
// their light (Fresnel's share, the ray bundle, sqrt(S)), a share of the primary's at 550 nm
const float PLUS_RB_AMP1[8] = float[8](1.148802, 1.096381, 1.053159, 1.016450,
                                       0.984609, 0.956525, 0.931446, 0.908815);
const float PLUS_RB_AMP2[8] = float[8](0.138106, 0.134303, 0.130885, 0.127786,
                                       0.124961, 0.122370, 0.119982, 0.117772);
// linear sRGB of each wavelength's 40 nm (CIE 1931), the 8 adding up to white
const vec3 PLUS_RB_WEIGHT[8] = vec3[8](vec3(0.020349, -0.022295, 0.172250),
                                       vec3(0.035334, -0.062022, 0.727007),
                                       vec3(-0.144429, 0.164808, 0.225384),
                                       vec3(-0.219118, 0.539555, -0.040575),
                                       vec3(0.321493, 0.395094, -0.059540),
                                       vec3(0.710255, 0.013310, -0.020837),
                                       vec3(0.258104, -0.028600, -0.003185),
                                       vec3(0.018012, 0.000151, -0.000504));
const float PLUS_RB_S_REF = 2.994203;       // the primary's S at 550 nm (1/rad) per um^(2/3) of the radius
const float PLUS_RB_Z1 = 1.018790;          // Ai(-z)^2's first maximum: where, how high
const float PLUS_RB_F1 = 0.286928;
const float PLUS_RB_DARK_P = 0.749227;      // its dark side: F1 exp(-P d^Q)
const float PLUS_RB_DARK_Q = 1.807158;
const float PLUS_RB_MEAN1 = 0.157680;       // the asymptotic form's mean at Z1: z^(-1/2) / 2 pi
const float PLUS_RB_SIN1 = 0.980125;        // and its swing: sin(4/3 z^(3/2))
const float PLUS_RB_KAPPA = 0.790000;       // the sun's disc widens the lobe: sqrt(1 + (kappa zr)^2)
const vec4 PLUS_RB_BRIGHT = vec4(-1.342042, -0.137716, -0.056722, -0.004344);   // ln(the primary's brightest luminance): a cubic in ln(r / 100 um)
const vec3 PLUS_RB_TAPER1 = vec3(0.600000, 0.349066, 4.0);   // away from the bow: floor, reach (rad), power
const vec3 PLUS_RB_TAPER2 = vec3(0.000000, 0.174533, 3.0);
const float PLUS_RB_SUN_RADIUS = 0.004651;  // the sun's (and the moon's) angular radius (rad)
const float PLUS_RB_LOOK_GAIN = 0.5;        // the bow's brightest point, times its light
const vec2 PLUS_RB_RADIUS = vec2(10.0, 2000.0); // the drops the model is fitted for (um)
// ---- end of the generated constants ----

const float PLUS_RB_THETA_MAX = 1.75;       // beyond (100 degrees from the antisolar point) the bow adds nothing
const float PLUS_RB_INV_2PI = 0.159154943;

// linear light to sRGB for light that may be above 1 (linear_to_srgb() clamps); the clouds' night lift
// (cloudsF.glsl, plus/nightclouds) takes it too
vec3 plusToSrgb(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(lessThan(c, vec3(0.0031308))));
}

// The light skyF.glsl's haze takes below the clouds towards view_dir: its sunlight (the sun's, by night
// the moon's x 0.7), through the air and dimmed by the cloud cover (sRGB, as skyF.glsl's).
vec3 plusRainbowLight(vec3 view_dir)
{
    vec3 light = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7;
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    float off_axis = 1.0 / max(1e-6, max(0.0, view_dir.y) + lightnorm.y);
    return light * exp(-light_atten * off_axis) * max(0.0, 1.0 - cloud_shadow);
}

// Ai(-z)^2 seen through the sun's disc (zr: its radius in z), the supernumeraries kept by keep_share (0:
// smeared out by a spread of drop sizes, 1: all of Airy's). The dark side (z <= Z1) is a fit of Ai^2; the
// lit side is the asymptotic form's mean and swing, the swing smeared by the disc, with the main lobe's
// fall kept whatever keep_share; both sides meet at the blurred lobe's top.
float plusRainbowAiry(float z, float zr, float keep_share)
{
    float wide = sqrt(1.0 + PLUS_RB_KAPPA * PLUS_RB_KAPPA * zr * zr);
    float peak = PLUS_RB_F1 / wide;
    if (z <= PLUS_RB_Z1)
    {
        return peak * exp(-PLUS_RB_DARK_P * pow((PLUS_RB_Z1 - z) / wide, PLUS_RB_DARK_Q));
    }
    float smear = -0.5 * zr * zr;
    float mean = inversesqrt(z) * PLUS_RB_INV_2PI;
    float swing = mean * sin((4.0 / 3.0) * z * sqrt(z));
    float lobe_off = (z - PLUS_RB_Z1) / wide;
    float lobe = exp(-lobe_off * lobe_off);
    float keep = exp(smear * z) * mix(keep_share, 1.0, lobe);
    float at_top = PLUS_RB_MEAN1 * (1.0 + exp(smear * PLUS_RB_Z1) * PLUS_RB_SIN1);
    return mean + keep * swing + (peak - at_top) * lobe;
}

// Away from the bow on its lit side (u > 0, rad): Airy's mean brought to geometric optics' (floor, reach, power).
float plusRainbowTaper(float u, vec3 taper)
{
    return taper.x + (1.0 - taper.x) / (1.0 + pow(max(u, 0.0) / taper.y, taper.z));
}

// One order's light over the 8 wavelengths, in linear sRGB for white light. theta: the angle from the
// antisolar point; s_ref: the primary's S at 550 nm for the drops; shrink: the primary's angle taken in (rad).
vec3 plusRainbowOrder(bool primary, float theta, float s_ref, float keep_share, float shrink)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; ++i)
    {
        float s = s_ref * (primary ? PLUS_RB_SCALE1[i] : PLUS_RB_SCALE2[i]);
        float u = primary ? (PLUS_RB_THETA1[i] - shrink - theta) : (theta - PLUS_RB_THETA2[i]);   // > 0: the lit side
        float light = plusRainbowAiry(s * u, s * PLUS_RB_SUN_RADIUS, keep_share)
                    * plusRainbowTaper(u, primary ? PLUS_RB_TAPER1 : PLUS_RB_TAPER2);
        sum += (primary ? PLUS_RB_AMP1[i] : PLUS_RB_AMP2[i]) * light * PLUS_RB_WEIGHT[i];
    }
    return sum;
}

// The primary's S at 550 nm for drops of radius r (um), and 1 / its brightest luminance (white light)
float plusRainbowScale(float r)
{
    return PLUS_RB_S_REF * pow(r, 2.0 / 3.0);
}

float plusRainbowNorm(float r)
{
    float x = log(r / 100.0);
    return exp(-(PLUS_RB_BRIGHT.x + x * (PLUS_RB_BRIGHT.y + x * (PLUS_RB_BRIGHT.z + x * PLUS_RB_BRIGHT.w))));
}

// The bow towards view_dir in linear sRGB for white light, its brightest point about 1 (0: none there).
vec3 plusRainbowPattern(vec3 view_dir)
{
    vec3 anti = -normalize(lightnorm);
    float cos_theta = clamp(dot(view_dir, anti), -1.0, 1.0);
    float theta = acos(cos_theta);
    if (theta > PLUS_RB_THETA_MAX)
    {
        return vec3(0.0);
    }
    float radius = clamp(plus_rainbow.y, PLUS_RB_RADIUS.x, PLUS_RB_RADIUS.y);
    float s_ref = plusRainbowScale(radius);
    float keep_share = clamp(plus_rainbow.z, 0.0, 1.0);
    vec3 rgb = plusRainbowOrder(true, theta, s_ref, keep_share, 0.0);
    if (plus_rainbow.w > 0.0)
    {
        rgb += plus_rainbow.w * plusRainbowOrder(false, theta, s_ref, keep_share, 0.0);
    }
    if (plus_rainbow_twin.x > 0.0)
    {
        // the twin: larger drops, flattened as they fall, whose bow is taken in towards its top (sin^2 of
        // the place on the bow, 0 at its feet, 1 at its top and its bottom); research/255 [low 18]
        vec3 across = view_dir - anti * cos_theta;
        vec3 up = vec3(0.0, 1.0, 0.0) - anti * anti.y;
        float lengths = dot(across, across) * dot(up, up);
        float sin_place = lengths > 1e-8 ? dot(across, up) * inversesqrt(lengths) : 0.0;
        float twin_radius = clamp(plus_rainbow_twin.x, PLUS_RB_RADIUS.x, PLUS_RB_RADIUS.y);
        rgb += plus_rainbow_twin.z * plusRainbowOrder(true, theta, plusRainbowScale(twin_radius), keep_share,
                                                      plus_rainbow_twin.y * sin_place * sin_place);
    }
    rgb *= plusRainbowNorm(radius);
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    return max(mix(vec3(luma), rgb, clamp(plus_rainbow_look.a, 0.0, 1.0)), vec3(0.0));
}

// shown: what the sky (or a cloud) writes (sRGB, after skyF.glsl's * 2); view_dir: its rel_pos_norm.
// Returns it with the bow added; as it is while plus_rainbow.x is 0 (PlusRainbow off, no light, a probe's
// irradiance pass, the HDRI sky) and where the bow adds nothing.
vec3 plusRainbowOver(vec3 shown, vec3 view_dir)
{
    if (plus_rainbow.x <= 0.0)
    {
        return shown;
    }
    vec3 add = (plus_rainbow.x * PLUS_RB_LOOK_GAIN) * plusRainbowPattern(view_dir)
             * srgb_to_linear(plusRainbowLight(view_dir) * plus_rainbow_look.rgb);
    float strongest = max(add.r, max(add.g, add.b));
    if (strongest <= 0.0)
    {
        return shown;
    }
    vec3 sky = srgb_to_linear(max(shown, vec3(0.0)));
    if (plus_rainbow_ceiling > 0.0)
    {
        // the largest share of the bow that keeps its channels under the ceiling, one for all three (its
        // hue kept); a channel where it adds under 2% of its strongest does not hold it back
        vec3 room = max(vec3(plus_rainbow_ceiling) - sky, vec3(0.0));
        vec3 fits = mix(vec3(1.0), room / max(add, vec3(1e-6)), vec3(greaterThan(add, vec3(0.02 * strongest))));
        add *= clamp(min(fits.r, min(fits.g, fits.b)), 0.0, 1.0);
    }
    return plusToSrgb(sky + add);
}
