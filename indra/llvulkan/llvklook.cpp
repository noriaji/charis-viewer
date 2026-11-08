/**
 * @file llvklook.cpp
 * @brief Looks (colour grading with a LUT) and screen effects (film grain,
 *        vignette, colour fringes, halation, dust, scanlines, the miniature's
 *        tilt-shift blur, clarity, texture and the soft glow) recorded directly in Vulkan
 *        (research/102, 236, 249, 286).
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

#include "linden_common.h"

#include "llvklook.h"

#include "llrhilook.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cmath>
#include <mutex>
#include <string>

namespace
{
    // The grade: each pixel of the source looked up in the strip and mixed
    // with itself by the strength, then the screen effects. The LUT part is
    // the same sampling as the CPU reference in tools/looks/lut_test.cpp of
    // the sl-viewer repository (keep them alike). Two variants of one source:
    // HAS_LUT (a look is selected; the strip is the second image) and
    // without it (effects only; the source is the only image).
    const char* const GRADE_FRAGMENT = R"GLSL(
layout(std140, set = 0, binding = 0) uniform Look
{
    vec4 domain_min;            // xyz: the colour at the table's first entries
    vec4 domain_scale;          // xyz: 1 / (domain max - domain min)
    float lut_size;             // N: entries along each colour axis
    float strength;             // 0: the source as it is, 1: the table's colours
    int debug_mode;             // LLRHI_LOOK_DEBUG (llrhilook.h); 0: the grade
    float padding;
    vec2 target_size;           // the bound target's size in pixels
    float fx_time;              // seconds (wrapped): seeds the moving noise
    float fx_taps;              // the taps of clarity's disc (more in snapshots, LookFx::clarity_taps)
    vec4 fx_a;                  // x: grain, y: vignette, z: chromatic, w: halation
    vec4 fx_b;                  // x: dust, y: scanlines, z: clarity, w: texture
    vec4 fx_c;                  // x: paint style (LookFx::PaintStyle), y: paint amount
    vec4 fx_d;                  // research/286. x: soft glow amount, y: its size, z: its lift, w: its taps
    vec4 image;                 // the whole picture (a tile's, LookGrade::image_*): xy where the
                                // target's first pixel lies in it, zw its size (the target's when whole)
    vec4 miniature;             // research/236. x: the band's centre line, its distance from the picture's centre
                                // across the band, -1..1 from one side of the picture to the other that way
                                // (level: -1 bottom .. 1 top), y: half its height
                                // (the picture's height spans -1..1), z: the largest blur radius in pixels of
                                // a 1080 high picture (0: no blur), w: the saturation and contrast added (0..1)
    vec4 miniature_normal;      // xy: the band's normal (the band slants by the tilt, anticlockwise),
                                // z: the most taps of the blur's disc (more in snapshots, LookFx::miniature_max_taps)
} u;

layout(set = 0, binding = 1) uniform sampler2D source;
#ifdef HAS_LUT
layout(set = 0, binding = 2) uniform sampler2D lut;     // N tiles of N x N: tile blue, x red, y green
#endif

layout(location = 0) out vec4 frag_color;

#ifdef HAS_LUT
// The table's colour for c: the sampler blends red and green inside a tile,
// two reads blend the tiles on either side of blue.
vec3 lookUp(vec3 c)
{
    float n = u.lut_size;
    vec3 cell = clamp((c - u.domain_min.xyz) * u.domain_scale.xyz, 0.0, 1.0) * (n - 1.0);
    float blue = min(floor(cell.b), n - 2.0);
    float blend = cell.b - blue;
    // Red and green at texel centres, from the first to the last of the tile:
    // the filter never reaches into the next tile.
    vec2 inside = cell.rg + 0.5;
    vec2 strip_size = vec2(n * n, n);
    vec3 lower = textureLod(lut, vec2(blue * n + inside.x, inside.y) / strip_size, 0.0).rgb;
    vec3 upper = textureLod(lut, vec2((blue + 1.0) * n + inside.x, inside.y) / strip_size, 0.0).rgb;
    return mix(lower, upper, blend);
}
#endif

// A hash of an integer cell and a seed (PCG output function, M. O'Neill),
// 0..1. Used for noise that holds still inside a cell and changes with the seed.
uint pcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rnd(uvec2 cell, uint seed)
{
    return float(pcg(cell.x + pcg(cell.y + pcg(seed)))) * (1.0 / 4294967295.0);
}

const vec3 LUMA = vec3(0.299, 0.587, 0.114);

// ---- Painted styles (research/142) ----

vec3 colorAt(vec2 uv)
{
    return textureLod(source, uv, 0.0).rgb;
}

float lumaAt(vec2 uv)
{
    return dot(colorAt(uv), LUMA);
}

// Kuwahara filter: the four quadrants round the pixel (each read on a sparse
// 4 x 4 grid, spacing radius / 3 pixels); the mean of the quietest one wins.
// Flat areas become flat paint, edges stay sharp.
vec3 kuwahara(vec2 uv, float radius)
{
    vec2 step_uv = vec2(radius / 3.0) / u.target_size;
    vec3 best_mean = vec3(0.0);
    float best_variance = 1e9;
    for (int q = 0; q < 4; ++q)
    {
        vec2 direction = vec2((q & 1) == 0 ? -1.0 : 1.0, (q & 2) == 0 ? -1.0 : 1.0);
        vec3 sum = vec3(0.0);
        float luma_sum = 0.0;
        float luma_square_sum = 0.0;
        for (int j = 0; j < 4; ++j)
        {
            for (int i = 0; i < 4; ++i)
            {
                vec3 s = colorAt(uv + direction * vec2(float(i), float(j)) * step_uv);
                float l = dot(s, LUMA);
                sum += s;
                luma_sum += l;
                luma_square_sum += l * l;
            }
        }
        float mean_luma = luma_sum / 16.0;
        float variance = luma_square_sum / 16.0 - mean_luma * mean_luma;
        if (variance < best_variance)
        {
            best_variance = variance;
            best_mean = sum / 16.0;
        }
    }
    return best_mean;
}

// Sobel gradient of the brightness, eight taps width_px apart.
float edgeAt(vec2 uv, float width_px)
{
    vec2 t = vec2(width_px) / u.target_size;
    float a = lumaAt(uv + vec2(-t.x, -t.y));
    float b = lumaAt(uv + vec2(0.0, -t.y));
    float c = lumaAt(uv + vec2(t.x, -t.y));
    float d = lumaAt(uv + vec2(-t.x, 0.0));
    float f = lumaAt(uv + vec2(t.x, 0.0));
    float g = lumaAt(uv + vec2(-t.x, t.y));
    float h = lumaAt(uv + vec2(0.0, t.y));
    float i = lumaAt(uv + vec2(t.x, t.y));
    float gx = (c + 2.0 * f + i) - (a + 2.0 * d + g);
    float gy = (g + 2.0 * h + i) - (a + 2.0 * b + c);
    return length(vec2(gx, gy));
}

// Brightness l rounded to n levels; softness 0..0.5 rounds the steps off.
float poster(float l, float n, float softness)
{
    float x = l * n;
    return (floor(x) + smoothstep(0.5 - softness, 0.5 + softness, fract(x))) / n;
}

// Brightness moved towards target by amount, the colour kept (ratio limited).
vec3 reLight(vec3 c, float target, float amount)
{
    float l = dot(c, LUMA);
    float ratio = clamp((target + 0.02) / (l + 0.02), 0.6, 1.6);
    return c * mix(1.0, ratio, amount);
}

// Value noise 0..1 on a grid of one unit (fixed to the screen, not to time).
float valueNoise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    uvec2 i = uvec2(ivec2(cell) + 4096);
    float a = rnd(i, 21u);
    float b = rnd(i + uvec2(1u, 0u), 21u);
    float c = rnd(i + uvec2(0u, 1u), 21u);
    float d = rnd(i + uvec2(1u, 1u), 21u);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// uv: where the target is read; px: the pixel in the whole picture (the
// paper, canvas and hatching hold still across a snapshot's tiles).
vec3 paintImage(vec2 uv, vec2 px, int style, float scale)
{
    if (style == 1)            // cartoon: flat colour in a few steps, ink outlines
    {
        vec3 c = kuwahara(uv, 3.0 * scale);
        float l = dot(c, LUMA);
        c = reLight(c, poster(l, 5.0, 0.05), 1.0);
        c = mix(vec3(dot(c, LUMA)), c, 1.15);
        float line = smoothstep(0.10, 0.30, edgeAt(uv, 1.5 * scale));
        return mix(c, vec3(0.04), line * 0.9);
    }
    if (style == 2)             // watercolour: bleeding wash, pooled edges, paper
    {
        vec2 grid = px / (40.0 * scale);
        vec2 wobble = vec2(valueNoise(grid), valueNoise(grid + 17.0)) - 0.5;
        vec2 at = uv + wobble * (4.0 * scale) / u.target_size;
        vec3 c = kuwahara(at, 5.0 * scale);
        c = reLight(c, poster(dot(c, LUMA), 6.0, 0.35), 0.5);
        c *= 1.0 - 0.35 * smoothstep(0.05, 0.25, edgeAt(at, 2.0 * scale));
        float paper = 0.5 * valueNoise(px / (3.0 * scale)) + 0.5 * valueNoise(px / (11.0 * scale));
        c *= 0.96 + 0.08 * paper;
        c = mix(c, vec3(1.0), 0.12);
        return mix(vec3(dot(c, LUMA)), c, 0.9);
    }
    if (style == 3)             // sketch: pencil outline and hatching on paper
    {
        float l = lumaAt(uv);
        float line = smoothstep(0.06, 0.22, edgeAt(uv, 1.2 * scale));
        float t1 = fract((px.x + px.y) / (5.0 * scale));
        float t2 = fract((px.x - px.y) / (5.0 * scale));
        float w1 = clamp((0.65 - l) * 1.6, 0.0, 1.0);
        float w2 = clamp((0.35 - l) * 2.5, 0.0, 1.0);
        float dark1 = (1.0 - smoothstep(w1 * 0.5, w1 * 0.5 + 0.08, t1)) * step(0.01, w1);
        float dark2 = (1.0 - smoothstep(w2 * 0.5, w2 * 0.5 + 0.08, t2)) * step(0.01, w2);
        float grain = 0.5 * valueNoise(px / (2.0 * scale)) + 0.5 * valueNoise(px / (9.0 * scale));
        vec3 paper = vec3(0.95, 0.93, 0.88) * (0.96 + 0.07 * grain);
        return paper * (1.0 - 0.55 * dark1) * (1.0 - 0.55 * dark2) * (1.0 - 0.85 * line);
    }
    // oil: broad strokes, a little more colour and contrast, canvas weave
    vec3 c = kuwahara(uv, 6.0 * scale);
    c = mix(vec3(dot(c, LUMA)), c, 1.1);
    c = (c - 0.5) * 1.1 + 0.5;
    float weave = sin(px.x * 2.0 / scale) * sin(px.y * 2.0 / scale);
    return c * (1.0 + 0.03 * weave);
}
)GLSL"
    // (three literals: one may hold at most about 16,000 characters, MSVC C2026)
    R"GLSL(
// ---- Miniature (tilt-shift, research/236) ----

// From the band's edge to the strongest blur, in the units where the picture's height spans -1..1.
const float MINIATURE_RAMP = 0.5;
const float MINIATURE_SATURATION = 0.6;    // saturation x (1 + this) at full amount
const float MINIATURE_CONTRAST = 0.2;      // contrast x (1 + this) at full amount

// Interleaved gradient noise (J. Jimenez), 0..1: fixed to the picture's pixel,
// so a still picture does not flicker and a snapshot's tiles share it.
float gradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The blur radius in pixels at image_place (a share of the whole picture), 0 inside the band.
float miniatureRadius(vec2 image_place, float aspect, float scale)
{
    vec2 q = vec2((image_place.x - 0.5) * 2.0 * aspect, image_place.y * 2.0 - 1.0);
    // Measured across the band, so the band's place moves it across itself at any slant; the place's
    // -1..1 spans the picture across the band (its half width that way), so at any slant it reaches
    // the picture's corners (level, as before: its height).
    vec2 n = u.miniature_normal.xy;
    float distance_from_line = abs(dot(q, n) - u.miniature.x * (abs(n.x) * aspect + abs(n.y)));
    float t = clamp((distance_from_line - u.miniature.y) / MINIATURE_RAMP, 0.0, 1.0);
    return u.miniature.z * scale * t * t * (3.0 - 2.0 * t);
}

// The golden angle in radians: from one tap of a spiral to the next.
const float GOLDEN_ANGLE = 2.39996323;

// Tap i of a disc of taps round a pixel, in pixels: the golden-angle spiral (even over the
// disc, each tap on a radius of its own, so what it misses scatters instead of drawing
// copies), turned by turn.
vec2 discTap(int i, int taps, float radius, float turn)
{
    float angle = float(i) * GOLDEN_ANGLE + turn;
    return vec2(cos(angle), sin(angle)) * radius * sqrt((float(i) + 0.5) / float(taps));
}

// A disc of taps round uv, radius in pixels: the golden-angle spiral (an even
// disc, as a lens blurs), turned per pixel so the gaps between taps scatter
// instead of drawing copies. More taps for a larger disc, up to the most the
// viewer allows (more while a snapshot is taken).
vec3 miniatureBlur(vec2 uv, vec2 pixel, float radius)
{
    int taps = int(clamp(8.0 + 0.25 * radius * radius, 8.0, max(u.miniature_normal.z, 8.0)));
    float turn = gradientNoise(pixel) * 6.2831853;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < taps; ++i)
    {
        sum += textureLod(source, uv + discTap(i, taps, radius, turn) / u.target_size, 0.0).rgb;
    }
    return sum / float(taps);
}

// ---- Taps round a pixel: halation, clarity and texture (research/249 2.1) ----

// Tap i of two rings of eight round a pixel, in pixels: i < 8 on the inner ring,
// the rest on the outer one turned half a step; the whole turned by turn (radians).
vec2 ringTap(int i, float inner, float outer, float turn)
{
    int ring = i / 8;
    float angle = (float(i - 8 * ring) + 0.5 * float(ring)) * 0.78539816 + turn;
    return vec2(cos(angle), sin(angle)) * (ring == 0 ? inner : outer);
}

// Radii in pixels of a 1080 high picture: halation's two rings; clarity's disc (2 % of the
// height) and texture's one ring of eight (0.3 %).
const float HALATION_INNER = 9.0;
const float HALATION_OUTER = 20.0;
const float CLARITY_RADIUS = 21.6;
const float TEXTURE_RADIUS = 3.2;
const int TEXTURE_TAPS = 8;
// How far the brightness moves per unit of its difference from the mean, at full amount.
const float CLARITY_GAIN = 1.5;
const float TEXTURE_GAIN = 2.0;
// The most one pixel's brightness moves: the change rounds off towards it (tanh), so strong
// edges get no bright or dark rims.
const float LOCAL_CONTRAST_LIMIT = 0.15;
// Taps whose brightness lies this far from the centre's count about a third (bilateral):
// across a strong edge the far side hardly counts, so the edge is not lifted into a rim.
const float EDGE_STOP = 0.15;

// The source's mean brightness round uv, each tap weighted by how near its brightness is to
// centre's (the centre itself counts 1, so the weights never add up to 0). disc: clarity's
// disc of taps taps; else texture's ring of eight. Neither turns: the same taps for every
// pixel, so no pattern is fixed to the screen to crawl as the camera moves.
float bilateralMeanLuma(vec2 uv, float centre, float radius, int taps, bool disc)
{
    float sum = centre;
    float weight = 1.0;
    for (int i = 0; i < taps; ++i)
    {
        vec2 offset = disc ? discTap(i, taps, radius, 0.0) : ringTap(i, radius, radius, 0.0);
        float l = lumaAt(uv + offset / u.target_size);
        float d = (l - centre) / EDGE_STOP;
        float w = exp(-d * d);
        sum += w * l;
        weight += w;
    }
    return sum / weight;
}

// 1 inside the target, falling to 0 within reach pixels of the edges a snapshot's tile
// shares with its neighbours: the taps cannot read across them, so the tiles meet without
// a seam. The whole picture's own edges are not faded.
float innerEdgeFade(float reach)
{
    vec2 p = gl_FragCoord.xy;
    vec2 low = mix(vec2(1e9), p, greaterThan(u.image.xy, vec2(0.5)));
    vec2 high = mix(vec2(1e9), u.target_size - p, lessThan(u.image.xy + u.target_size, u.image.zw - 0.5));
    vec2 nearest = min(low, high);
    return clamp(min(nearest.x, nearest.y) / max(reach, 1.0), 0.0, 1.0);
}

// Clarity and texture: the brightness of c moved by the source's own difference from the
// mean round the pixel (an unsharp mask of the brightness, so the mean over an area stays),
// the hue kept. Weighted to the midtones: little in the dark (the noise of night scenes)
// and in the highlights. keep 0..1: how much of the picture is still the source's own.
// clarity_taps: the taps of clarity's disc (more while a snapshot is taken).
vec3 localContrast(vec3 c, float source_luma, vec2 uv, float scale, float clarity_amount, float texture_amount,
                   int clarity_taps, float keep)
{
    float l = dot(c, LUMA);
    float weight = smoothstep(0.03, 0.3, l) * (1.0 - smoothstep(0.8, 1.0, l)) * keep;
    if (weight <= 0.0)
    {
        return c;   // nothing would move: no reads
    }
    float detail = 0.0;
    if (clarity_amount > 0.0)
    {
        float mean = bilateralMeanLuma(uv, source_luma, CLARITY_RADIUS * scale, clarity_taps, true);
        detail += clarity_amount * CLARITY_GAIN * (source_luma - mean);
    }
    if (texture_amount > 0.0)
    {
        float mean = bilateralMeanLuma(uv, source_luma, TEXTURE_RADIUS * scale, TEXTURE_TAPS, false);
        detail += texture_amount * TEXTURE_GAIN * (source_luma - mean);
    }
    float delta = LOCAL_CONTRAST_LIMIT * tanh(detail * weight / LOCAL_CONTRAST_LIMIT);
    return reLight(c, l + delta, 1.0);
}
)GLSL"
    // (a third literal: the second is near MSVC's limit, C2026)
    R"GLSL(
// ---- Soft glow (research/286) ----

// The sigma of the blur in pixels of a 1080 high picture, at the size slider's two ends.
const float SOFT_GLOW_SIGMA_MIN = 6.5;
const float SOFT_GLOW_SIGMA_MAX = 32.0;
// The widest blur a tile of a snapshot drawn in pieces may have, in pixels: its taps cannot read across the
// tile's edge, so the glow fades out over three sigma from it (a wide blur would leave a bright-less cross).
const float SOFT_GLOW_TILE_SIGMA = 24.0;
// The share of the blur laid over the picture at amount 1, and the share of it added as light at lift 1.
const float SOFT_GLOW_OPACITY = 0.6;
const float SOFT_GLOW_LIGHT = 0.5;

// The picture blurred by a Gaussian of sigma pixels: taps equally weighted, spread by the Gaussian's
// own distribution (r = sigma * sqrt(-2 ln(1 - share))) on a golden-angle spiral (the direction turned
// by a rotation from one tap to the next: the cosine and sine once per pixel), the whole turned by a
// noise that differs from pixel to pixel (the steps between the taps show as grain, not as rings). The
// noise sits still on the screen: this pass comes after the anti-aliasing, which does not smooth it.
vec3 softGlowBlur(vec2 uv, vec2 pixel, float sigma, int taps)
{
    float turn = gradientNoise(pixel) * 6.2831853;
    vec2 direction = vec2(cos(turn), sin(turn));
    const vec2 step_turn = vec2(cos(GOLDEN_ANGLE), sin(GOLDEN_ANGLE));
    float per_tap = 1.0 / float(taps);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < taps; ++i)
    {
        float r = sigma * sqrt(-2.0 * log(1.0 - (float(i) + 0.5) * per_tap));
        sum += textureLod(source, uv + direction * r / u.target_size, 0.0).rgb;
        direction = vec2(direction.x * step_turn.x - direction.y * step_turn.y,
                         direction.x * step_turn.y + direction.y * step_turn.x);
    }
    return sum / float(taps);
}

void main()
{
    vec4 color = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    vec2 place = gl_FragCoord.xy / u.target_size;
    if (u.debug_mode == 1)
    {
        frag_color = color;                                         // reads and writes only
        return;
    }
    if (u.debug_mode == 2)
    {
        frag_color = vec4(0.0, 1.0, 0.0, 1.0);                      // the target only
        return;
    }
    if (u.debug_mode == 3)
    {
#ifdef HAS_LUT
        frag_color = vec4(textureLod(lut, place, 0.0).rgb, 1.0);    // the LUT strip itself
#else
        frag_color = color;
#endif
        return;
    }
    if (u.debug_mode == 4)
    {
        frag_color = vec4(place, 0.0, 1.0);                         // the target's size and orientation
        return;
    }

    // Places and sizes are the whole picture's (u.image): a snapshot drawn in
    // tiles gets one vignette, not one per tile. Reads stay the target's.
    vec2 image_size = u.image.zw;
    vec2 pixel = gl_FragCoord.xy + u.image.xy;      // in the whole picture
    vec2 image_place = pixel / image_size;
    vec2 to_target = image_size / u.target_size;    // a share of the picture as one of the target
    float aspect = image_size.x / image_size.y;

    // Effect sizes are for a 1080 pixel high image; larger ones (snapshots) scale up.
    float scale = image_size.y / 1080.0;
    float grain = u.fx_a.x;
    float vignette = u.fx_a.y;
    float chromatic = u.fx_a.z;
    float halation = u.fx_a.w;
    float dust = u.fx_b.x;
    float scanlines = u.fx_b.y;
    float clarity = u.fx_b.z;
    float fine_detail = u.fx_b.w;   // texture (a name GLSL keeps for its function)
    int paint_style = int(u.fx_c.x + 0.5);
    float paint = u.fx_c.y;

    // Where the colour is read: scanlines wave the picture sideways and now
    // and then tear a band of rows; colour fringes read the three channels
    // at different distances from the centre.
    vec2 read_at = place;
    if (scanlines > 0.0)
    {
        float band = floor(pixel.y / (8.0 * scale));
        uint tick = uint(u.fx_time * 30.0);
        float shift = 0.0006 * sin(pixel.y / scale * 0.05 + u.fx_time * 2.0);
        if (rnd(uvec2(uint(band), 7u), tick) > 0.99)
        {
            shift += (rnd(uvec2(uint(band), 11u), tick) - 0.5) * 0.02;
        }
        read_at.x += shift * scanlines * to_target.x;
    }
    vec3 c = color.rgb;
    if (chromatic > 0.0 || scanlines > 0.0)
    {
        vec2 read_in_image = (read_at * u.target_size + u.image.xy) / image_size;
        vec2 away = (read_in_image - 0.5) * (chromatic * 0.03) * to_target;
        c = vec3(textureLod(source, read_at + away, 0.0).r,
                 textureLod(source, read_at, 0.0).g,
                 textureLod(source, read_at - away, 0.0).b);
    }

    // Miniature: outside the band the picture is blurred, more the farther
    // away, round where the colour was read (the colour fringes give way to it).
    float blurred = 0.0;    // how much of the picture the blur replaced (clarity gives way to it)
    if (u.miniature.z > 0.0)
    {
        float radius = miniatureRadius(image_place, aspect, scale);
        if (radius > 0.5)
        {
            blurred = clamp(radius - 0.5, 0.0, 1.0);
            c = mix(c, miniatureBlur(read_at, pixel, radius), blurred);
        }
    }

    // Painted style: replaces the picture, and the LUT grades the painting.
    float painted = 0.0;
    if (paint_style > 0 && paint > 0.0)
    {
        painted = paint;
        c = mix(c, paintImage(place, pixel, paint_style, scale), paint);
    }

#ifdef HAS_LUT
    c = mix(c, lookUp(c), u.strength);
#endif

    // Clarity and texture: local contrast over the look's colours, where the picture is still the source's own.
    if (clarity > 0.0 || fine_detail > 0.0)
    {
        float reach = (clarity > 0.0 ? CLARITY_RADIUS : TEXTURE_RADIUS) * scale;
        float keep = (1.0 - blurred) * (1.0 - painted) * innerEdgeFade(reach);
        // Round where the colour was read (scanlines move it), so the detail goes with the picture.
        float read_luma = scanlines > 0.0 ? dot(textureLod(source, read_at, 0.0).rgb, LUMA) : dot(color.rgb, LUMA);
        c = localContrast(c, read_luma, read_at, scale, clarity, fine_detail, int(u.fx_taps), keep);
    }

    // Miniature: a toy's brighter colours and a little more contrast, over the look's colours.
    if (u.miniature.w > 0.0)
    {
        c = mix(vec3(dot(c, LUMA)), c, 1.0 + MINIATURE_SATURATION * u.miniature.w);
        c = max((c - 0.5) * (1.0 + MINIATURE_CONTRAST * u.miniature.w) + 0.5, 0.0);
    }

    // Soft glow: the blurred picture (in the look's colours) over the picture, and a little of it
    // added as light. Faded out towards the inner edges of a snapshot's tiles (the taps cannot read across).
    if (u.fx_d.x > 0.0)
    {
        float sigma = mix(SOFT_GLOW_SIGMA_MIN, SOFT_GLOW_SIGMA_MAX, u.fx_d.y) * scale;
        if (any(greaterThan(abs(u.image.zw - u.target_size), vec2(0.5))))
        {
            sigma = min(sigma, SOFT_GLOW_TILE_SIGMA);   // a tile of a snapshot drawn in pieces
        }
        // Where a painted style has replaced the picture the blur (of the source) does not belong.
        float amount = u.fx_d.x * (1.0 - painted) * innerEdgeFade(sigma * 3.0);
        if (amount > 0.0)
        {
            vec3 glow = softGlowBlur(read_at, pixel, sigma, int(u.fx_d.w));
            // One bad pixel (NaN, inf) in the source would spoil a whole disc: no glow there.
            if (!any(isnan(glow)) && !any(isinf(glow)))
            {
#ifdef HAS_LUT
                glow = mix(glow, lookUp(glow), u.strength);
#endif
                c = mix(c, glow, amount * SOFT_GLOW_OPACITY);
                c += (1.0 - min(c, vec3(1.0))) * glow * (amount * u.fx_d.z * SOFT_GLOW_LIGHT);
            }
        }
    }

    // Halation: the bright parts of the picture, spread over two rings of
    // taps and laid back in red-orange, as light scatters in the back of film.
    if (halation > 0.0)
    {
        float glow = 0.0;
        for (int i = 0; i < 16; ++i)
        {
            vec2 at = place + ringTap(i, HALATION_INNER * scale, HALATION_OUTER * scale, 0.0) / u.target_size;
            glow += dot(max(textureLod(source, at, 0.0).rgb - 0.65, 0.0), LUMA);
        }
        c += (glow / 16.0) * vec3(1.0, 0.38, 0.16) * (halation * 4.0);
    }

    // Vignette: 0 at the centre, 1 in the corners (the aspect ratio kept).
    if (vignette > 0.0)
    {
        vec2 from_centre = (image_place - 0.5) * vec2(aspect, 1.0);
        float corner = length(from_centre) / (0.5 * length(vec2(aspect, 1.0)));
        c *= 1.0 - vignette * smoothstep(0.3, 1.0, corner);
    }

    // Scanlines: dark rows every few pixels.
    if (scanlines > 0.0)
    {
        float row = 0.5 + 0.5 * cos(pixel.y * 6.2831853 / max(3.0 * scale, 2.0));
        c *= (1.0 - scanlines * 0.35 * row) * (1.0 + 0.15 * scanlines);
    }

    // Grain: monochrome, strongest in the midtones, new every 1/24 s.
    if (grain > 0.0)
    {
        float cell_size = max(floor(scale * 1.5 + 0.5), 1.0);
        uvec2 cell = uvec2(floor(pixel / cell_size));
        uint seed = uint(u.fx_time * 24.0);
        float noise = rnd(cell, seed) + rnd(cell, seed + 977u) - 1.0;   // -1..1, peaked at 0
        float luma = dot(c, LUMA);
        c += noise * grain * 0.22 * (1.0 - 0.5 * abs(luma * 2.0 - 1.0));
    }

    // Dust: a few specks (dark mostly, some bright) and now and then one
    // long vertical scratch, new every 1/8 s.
    if (dust > 0.0)
    {
        uint seed = uint(u.fx_time * 8.0);
        float cell_size = 96.0 * scale;
        vec2 cell_at = floor(pixel / cell_size);
        uvec2 cell = uvec2(cell_at);
        if (rnd(cell, seed + 5000u) < dust * 0.04)
        {
            vec2 centre = (cell_at + vec2(rnd(cell, seed + 1u), rnd(cell, seed + 2u))) * cell_size;
            float radius = (1.0 + 2.5 * rnd(cell, seed + 3u)) * scale;
            float spot = 1.0 - smoothstep(radius * 0.6, radius, length(pixel - centre));
            float tone = rnd(cell, seed + 4u) < 0.7 ? 0.03 : 0.93;
            c = mix(c, vec3(tone), spot * 0.75);
        }
        if (rnd(uvec2(4u, 4u), seed) < dust * 0.5)
        {
            float x = rnd(uvec2(3u, 3u), seed) * image_size.x;
            float from_top = image_place.y - rnd(uvec2(5u, 5u), seed);
            float length_of = 0.15 + 0.25 * rnd(uvec2(6u, 6u), seed);
            if (abs(pixel.x - x) < 0.8 * scale && from_top > 0.0 && from_top < length_of)
            {
                c = mix(c, vec3(0.92), 0.35);
            }
        }
    }

    frag_color = vec4(c, color.a);
}
)GLSL";

    // The uniform block Look, as std140 lays it out.
    struct Uniforms
    {
        F32 domain_min[4];
        F32 domain_scale[4];
        F32 lut_size;
        F32 strength;
        S32 debug_mode;
        F32 padding;
        F32 target_size[2];
        F32 fx_time;
        F32 fx_taps;
        F32 fx_a[4];
        F32 fx_b[4];
        F32 fx_c[4];
        F32 fx_d[4];
        F32 image[4];
        F32 miniature[4];
        F32 miniature_normal[4];
    };
    static_assert(sizeof(Uniforms) == 176, "Uniforms must match the std140 block Look");

    // The miniature's largest blur radius, in pixels of a 1080 high picture (at LookFx::miniature_blur 1).
    constexpr F32 MINIATURE_RADIUS = 12.f;
    // The band's height may span the picture (a share of 1) and its slant a quarter turn either way.
    constexpr F32 MINIATURE_MAX_WIDTH = 1.f;
    constexpr F32 MINIATURE_MAX_TILT = 90.f;
    constexpr F32 RADIANS_PER_DEGREE = 3.14159265f / 180.f;
    // The blur's disc takes 8 taps at least; the viewer's limit is kept to this many at most.
    constexpr U32 MINIATURE_MIN_TAPS = 8;
    constexpr U32 MINIATURE_MOST_TAPS = 64;
    // Clarity's disc: as few and as many taps as the viewer may ask for.
    constexpr U32 CLARITY_MIN_TAPS = 8;
    constexpr U32 CLARITY_MOST_TAPS = 64;
    // The soft glow's blur: as few and as many taps as the viewer may ask for.
    constexpr U32 SOFT_GLOW_MIN_TAPS = 8;
    constexpr U32 SOFT_GLOW_MOST_TAPS = 96;

    // The smallest table the shader can blend (two tiles for blue).
    constexpr U32 MIN_LUT_SIZE = 2;
    // LLRHI_LOOK_DEBUG 5: nothing is recorded (llrhilook.h).
    constexpr U32 DEBUG_SKIP = 5;

    class Renderer
    {
    public:
        bool grade(const LLRHI::LookGrade& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { const bool effects = ready(false); return ready(true) && effects; }

    private:
        bool ready(bool with_lut);

        // [0]: effects only, [1]: with a LUT.
        bool mTried[2] = { false, false };
        bool mReady[2] = { false, false };
        std::string mSource[2];
        LLVKFullscreen::Pass mGrade[2];
        U32 mLoggedSize = 0;        // the LUT size last logged (one line per LUT size)
        bool mLoggedFx = false;     // the effects-only pass logged
        bool mWarnedTarget = false; // a bound target of another size (warned once)
    };

    Renderer sRenderer;

    bool Renderer::ready(bool with_lut)
    {
        const U32 v = with_lut ? 1 : 0;
        if (!mTried[v])
        {
            mTried[v] = true;
            mSource[v] = std::string("#version 450\n") + (with_lut ? "#define HAS_LUT 1\n" : "") + GRADE_FRAGMENT;
            mReady[v] = mGrade[v].init(with_lut ? "look grade" : "look effects", mSource[v].c_str(), with_lut ? 2 : 1);
            if (!mReady[v])
            {
                LL_WARNS("VKLook") << "The " << (with_lut ? "look" : "effects") << " pass could not be set up; it stays off" << LL_ENDL;
            }
        }
        return mReady[v];
    }

    bool Renderer::grade(const LLRHI::LookGrade& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        const bool with_lut = pass.lut_texture && pass.lut_size >= MIN_LUT_SIZE;
        if (!pass.source_texture || (!with_lut && !pass.fx.any()) || !ready(with_lut))
        {
            return false;
        }
        Uniforms uniforms = {};
        if (with_lut)
        {
            for (U32 c = 0; c < 3; ++c)
            {
                const F32 span = pass.domain_max[c] - pass.domain_min[c];
                if (!(span > 0.f))
                {
                    return false;
                }
                uniforms.domain_min[c] = pass.domain_min[c];
                uniforms.domain_scale[c] = 1.f / span;
            }
            uniforms.lut_size = (F32)pass.lut_size;
            uniforms.strength = llclamp(pass.strength, 0.f, 1.f);
        }
        uniforms.debug_mode = (S32)LLRHI::sLookDebug;
        uniforms.target_size[0] = (F32)pass.width;
        uniforms.target_size[1] = (F32)pass.height;
        // The whole picture: the target itself unless it is a snapshot's tile.
        const bool tile = pass.image_size[0] > 0.f && pass.image_size[1] > 0.f;
        uniforms.image[0] = tile ? pass.image_offset[0] : 0.f;
        uniforms.image[1] = tile ? pass.image_offset[1] : 0.f;
        uniforms.image[2] = tile ? pass.image_size[0] : (F32)pass.width;
        uniforms.image[3] = tile ? pass.image_size[1] : (F32)pass.height;
        uniforms.fx_time = pass.fx.time;
        uniforms.fx_a[0] = llclamp(pass.fx.grain, 0.f, 1.f);
        uniforms.fx_a[1] = llclamp(pass.fx.vignette, 0.f, 1.f);
        uniforms.fx_a[2] = llclamp(pass.fx.chromatic, 0.f, 1.f);
        uniforms.fx_a[3] = llclamp(pass.fx.halation, 0.f, 1.f);
        uniforms.fx_b[0] = llclamp(pass.fx.dust, 0.f, 1.f);
        uniforms.fx_b[1] = llclamp(pass.fx.scanlines, 0.f, 1.f);
        uniforms.fx_b[2] = llclamp(pass.fx.clarity, 0.f, 1.f);
        uniforms.fx_b[3] = llclamp(pass.fx.texture, 0.f, 1.f);
        uniforms.fx_taps = (F32)llclamp(pass.fx.clarity_taps, CLARITY_MIN_TAPS, CLARITY_MOST_TAPS);
        if (pass.fx.painting())
        {
            uniforms.fx_c[0] = (F32)pass.fx.paint_style;
            uniforms.fx_c[1] = llclamp(pass.fx.paint, 0.f, 1.f);
        }
        if (pass.fx.softGlow())
        {
            uniforms.fx_d[0] = llclamp(pass.fx.soft_glow, 0.f, 1.f);
            uniforms.fx_d[1] = llclamp(pass.fx.soft_glow_size, 0.f, 1.f);
            uniforms.fx_d[2] = llclamp(pass.fx.soft_glow_lift, 0.f, 1.f);
            uniforms.fx_d[3] = (F32)llclamp(pass.fx.soft_glow_taps, SOFT_GLOW_MIN_TAPS, SOFT_GLOW_MOST_TAPS);
        }
        if (pass.fx.miniature())
        {
            // The band's half height in the shader's units (the picture's height spans -1..1) is the share itself.
            uniforms.miniature[0] = llclamp(pass.fx.miniature_band, -1.f, 1.f);
            uniforms.miniature[1] = llclamp(pass.fx.miniature_width, 0.f, MINIATURE_MAX_WIDTH);
            uniforms.miniature[2] = llclamp(pass.fx.miniature_blur, 0.f, 1.f) * MINIATURE_RADIUS;
            uniforms.miniature[3] = llclamp(pass.fx.miniature_saturation, 0.f, 1.f);
            const F32 tilt = llclamp(pass.fx.miniature_tilt, -MINIATURE_MAX_TILT, MINIATURE_MAX_TILT) * RADIANS_PER_DEGREE;
            uniforms.miniature_normal[0] = -std::sin(tilt);
            uniforms.miniature_normal[1] = std::cos(tilt);
            uniforms.miniature_normal[2] = (F32)llclamp(pass.fx.miniature_max_taps, MINIATURE_MIN_TAPS, MINIATURE_MOST_TAPS);
        }
        if (LLRHI::sLookDebug == DEBUG_SKIP)
        {
            return true;    // the caller swaps: the screen shows what the target held
        }

        LLVKMirror::Target target;
        VkCommandBuffer cmd = with_lut ? LLVKTracker::beginPass({ pass.source_texture, pass.lut_texture }, target)
                                       : LLVKTracker::beginPass({ pass.source_texture }, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != pass.width || target.height != pass.height)
        {
            LLVKTracker::endPass();
            if (!mWarnedTarget)
            {
                mWarnedTarget = true;
                LL_WARNS("VKLook") << "Look: the bound target is " << target.width << "x" << target.height << ", not the "
                                   << pass.width << "x" << pass.height << " it was called for; not drawn" << LL_ENDL;
            }
            return false;
        }
        const LLVKFullscreen::Image images[2] = { { pass.source_texture }, { pass.lut_texture } };
        const bool drawn = mGrade[with_lut ? 1 : 0].draw(cmd, target, &uniforms, sizeof(uniforms), images, with_lut ? 2 : 1);
        LLVKTracker::endPass();
        if (drawn && with_lut && mLoggedSize != pass.lut_size)
        {
            mLoggedSize = pass.lut_size;
            LL_INFOS("VKLook") << "Look: " << pass.lut_size << "^3 LUT on " << target.width << "x" << target.height << ", target format "
                               << (U32)target.color_formats[0] << ", depth format " << (U32)target.depth_format
                               << (LLRHI::sLookDebug ? ", LLRHI_LOOK_DEBUG " + std::to_string(LLRHI::sLookDebug) : std::string()) << LL_ENDL;
        }
        if (drawn && !with_lut && !mLoggedFx)
        {
            mLoggedFx = true;
            LL_INFOS("VKLook") << "Look: effects only on " << target.width << "x" << target.height << ", target format "
                               << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool record_grade(const LLRHI::LookGrade& pass)
    {
        return sRenderer.grade(pass);
    }
}

void LLVKLook::install()
{
    if (!LLRHI::sLook)
    {
        LL_INFOS("VKLook") << "Looks off (LLRHI_LOOK=0)" << LL_ENDL;
        return;
    }
    if (LLRHI::sLookDebug)
    {
        LL_INFOS("VKLook") << "Looks drawn for finding faults: LLRHI_LOOK_DEBUG " << LLRHI::sLookDebug << LL_ENDL;
    }
    LLRHI::sLookGrade = &record_grade;
    LLVKShader::addBakeStep("look", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
