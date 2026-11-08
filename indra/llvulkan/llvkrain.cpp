/**
 * @file llvkrain.cpp
 * @brief Rain (streaks, splashes) recorded directly in Vulkan (research/120
 *        sections 7, 8 and 12).
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

#include "llvkrain.h"

#include "llrhirain.h"
#include "llrhiskinsss.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// The overhead map's lookup (shared with llvkwetlook.cpp)

const char* const LLVKRain::OVERHEAD_GLSL = R"GLSL(
// How open to the sky the point at p (the overhead map's clip space) is: 1
// open, 0 under a roof. A texel covers the point when it holds a surface
// more than info.y (clip z) above it; the four texels around it are blended
// so a roof's edge is soft. Past the map's edge, above its top or with no
// map nothing covers anything; below its bottom a point is taken at the
// bottom (a cleared texel, depth 1, covers nothing).
float overheadOpen(vec3 p, vec4 info, vec2 map_depth_to_ndc)
{
    if (info.x < 0.5 || p.z < -1.0)
    {
        return 1.0;
    }
    vec2 beyond = (abs(p.xy) - (1.0 - info.w)) / max(info.w, 1e-4);
    float edge = clamp(max(beyond.x, beyond.y), 0.0, 1.0);
    if (edge >= 1.0)
    {
        return 1.0;
    }
    float z = min(p.z, 1.0) - info.y;
    ivec2 size = textureSize(overhead_map, 0);
    vec2 texel = (p.xy * 0.5 + 0.5) * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(texel));
    vec2 f = texel - vec2(base);
    vec4 covered;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 t = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), size - 1);
        float stored = texelFetch(overhead_map, t, 0).r * map_depth_to_ndc.x + map_depth_to_ndc.y;
        covered[i] = stored < z ? 1.0 : 0.0;
    }
    float cover = mix(mix(covered.x, covered.y, f.x), mix(covered.z, covered.w, f.x), f.y);
    return mix(1.0 - cover, 1.0, edge);
}

// Slanted rain reaches a point from upwind: its roof is looked for that far,
// as for one EAVE_HEIGHT above it, but at most MAX_EAVE_REACH m off (the map
// holds roofs, not walls: unbounded, a strong wind's look went past a roof's
// edge to the open sky and the rain fell indoors); and where the point itself
// is under cover, the rain from upwind comes in only by COVERED_REACH_SHARE
// (the windward eaves get a little, the room behind them none). Shared by the
// rain (llvkrain.cpp) and the wet look (llvkwetlook.cpp). (fix-round67 2)
const float EAVE_HEIGHT = 2.0;          // m
const float MAX_EAVE_REACH = 0.5;       // m
const float COVERED_REACH_SHARE = 0.25;

// The upwind shift in the map's clip xy. shift_per_metre: clip per metre of
// height, towards where the rain comes from; clip_per_metre: the map's clip
// units per metre across.
vec2 upwindShift(vec2 shift_per_metre, float clip_per_metre)
{
    vec2 shift = shift_per_metre * EAVE_HEIGHT;
    float shift_length = length(shift);
    float max_shift = clip_per_metre * MAX_EAVE_REACH;
    return shift_length > max_shift ? shift * (max_shift / shift_length) : shift;
}

// How much of the rain reaches a point open to the sky by `here` whose
// upwind point (upwindShift away) is open by `upwind`.
float windOpen(float here, float upwind)
{
    return max(min(here, upwind), COVERED_REACH_SHARE * upwind);
}
)GLSL";

namespace
{
    // ------------------------------------------------------------------
    // Shader

    const char* const RAIN_HEAD = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform RainStreaks
{
    mat4 inverse_projection;    // clip -> eye
    mat4 eye_to_world;          // eye -> world axes, the camera at the origin
    mat4 to_map;                // that space -> the overhead map's clip space
    vec4 map_info;              // x: 1 with a map, y: cover bias (clip z), w: edge fade (clip)
    vec4 map_info2;             // xy: the map's depth to ndc, z: normal offset (m)
    vec4 map_shift;             // xy: clip per metre of height, towards where the rain comes from
    vec4 rain;                  // xy: shear (m sideways per m of fall), z: streak length (m), w: pixel angle (rad)
    vec4 world;                 // xy: the camera's world x, y (wrapped), z: time (s), w: splashes per cell per s
    vec4 drop_light;            // rgb: what a drop shows under a bright sky (exposed), a: a streak's opacity
    vec4 ambient;               // rgb: the sky's light (scene units), a: the share of the opacity that hides
    vec4 glint;                 // rgb: the sun seen through a drop in front of it (scene units), a: its limit (exposed; 0: GLINT_LIMIT)
    vec4 sun;                   // xyz: towards the sun (world), w: the splashes' size (heavy rain; 1 as they were)
    vec4 streak;                // x: drop width (m), y: defocus (m), z: first band (m), w: band overlap (octaves)
    vec4 splash;                // x: range (m), y: brightness, z: veil, w: veil distance (m)
    vec4 exposure;              // x: RenderExposure (0: no exposure, the map not read), y: band count
    vec4 snow;                  // x: snow flakes' sway (m; 0: rain), y: 1: the splashes draw no ring (the avatars' pixels), z: a flake's stretch (0: rain's streaks), w: the flakes' near fade (m)
    vec4 umbrella[2];           // xyz: a canopy's top centre (camera-relative world, m), w: its radius (m; 0: none)
    vec4 umbrella_info;         // x: how far the rim hangs below the top (m), y: share of the rim's columns that drip
    vec4 eave;                  // x: share of a roof edge's columns that drip (0: no drips), y: smallest step of a roof's edge (m), z: range (m)
    vec4 band_cell[5];          // x: cell width (m), y: cell height (m), z: probability, w: brightness
    vec4 band_camera[5];        // xyz: the camera within its cell (m), w: within its cell up z counted in cell widths (snow's level sheets)
    uvec4 band_base[5];         // xyz: the camera's cell (rain space), w: its cell up z counted in cell widths
    uvec4 band_planes[5];       // x, y: cells from the base to the first sheet along x, y; z: cells between sheets; w: from base.w to the first level sheet
    vec2 depth_to_ndc;          // ndc z = depth * x + y
    ivec2 size;                 // the target's size
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D exposure_map;
layout(set = 0, binding = 3) uniform sampler2D overhead_map;

layout(location = 0) out vec4 frag_color;
)GLSL";

    const char* const RAIN_BODY = R"GLSL(
// Umbrellas (u.umbrella[i]: xyz the canopy's top centre, camera-relative
// world; w its radius, 0: none): a shallow dome, its rim u.umbrella_info.x
// lower than its top. The rain does not fall through it.
const int MAX_UMBRELLAS = 2;

// The canopy's height over the point offset d (horizontally) from its centre.
float canopyHeight(vec4 umbrella, vec2 d)
{
    float r = min(length(d) / umbrella.w, 1.0);
    return umbrella.z - u.umbrella_info.x * r * r;
}

// Whether the rain at p (camera-relative world) is shielded: the path it fell
// along, back up and upwind (rise_shift: sideways metres per metre up) passes
// through a canopy above it.
bool umbrellaShields(vec3 p, vec2 rise_shift)
{
    for (int i = 0; i < MAX_UMBRELLAS; ++i)
    {
        vec4 umbrella = u.umbrella[i];
        float rise = umbrella.z - p.z;
        if (umbrella.w <= 0.0 || rise <= 0.0)
        {
            continue;
        }
        vec2 d = p.xy + rise_shift * rise - umbrella.xy;
        if (length(d) < umbrella.w && p.z < canopyHeight(umbrella, d))
        {
            return true;
        }
    }
    return false;
}

// Whether p (a point of the scene) lies on a canopy's top.
bool onUmbrella(vec3 p)
{
    for (int i = 0; i < MAX_UMBRELLAS; ++i)
    {
        vec4 umbrella = u.umbrella[i];
        vec2 d = p.xy - umbrella.xy;
        if (umbrella.w > 0.0 && length(d) < umbrella.w * 1.05 && abs(p.z - canopyHeight(umbrella, d)) < 0.15)
        {
            return true;
        }
    }
    return false;
}
)GLSL"
    R"GLSL(
const int MAX_BANDS = 5;

// The drops' light: the sun shows through drops in front of it in a lobe this
// tight; the drops reach their full light from a sky this bright (exposed)
// and keep at least this share of it in the dark; nothing brighter than this
// (exposed) comes through a drop from the sun.
const float GLINT_POWER = 12.0;
const float AMBIENT_REFERENCE = 0.5;
const float MIN_LIT = 0.1;
const float GLINT_LIMIT = 2.0;

// Drops (plus/rain rain.cpp sizes the cells for them: LENGTH_SPREAD,
// SIZE_MAX): a streak's length varies by this share either way (the same fall
// speed for all, so not much); a drop is SIZE_MIN to SIZE_MAX times the width
// (most of them small) and as much brighter; a streak fades out over its top
// part (from this share of its length).
const float LENGTH_SPREAD = 0.15;
const float SIZE_MIN = 0.5;
const float SIZE_MAX = 1.6;
const float TAIL_FADE_START = 0.55;

// A family of sheets counts fully while the ray is within 30 degrees of
// facing them, not at all past 60 degrees (then the other family does): the
// share of the ray's horizontal part across them, cos 30 / (cos 30 + sin 30).
const float FAMILY_FULL = 0.634;
const float FAMILY_NONE = 0.366;
// Rays this close to the rain's own direction (the sine of the angle between
// them) see the streaks end on: faint dots, faded out towards END_ON_NONE
// (also the horizontal part of the ray in rain space below which nothing is
// tried).
const float END_ON_NONE = 0.01;
const float END_ON_FULL = 0.05;
// Around a crossing at most this many cells are tried: sheets per band
// (plus/rain spaces the sheets so a ray crosses at most about 3.6, wind or
// not), cells across a sheet, cells up it (more only for rays near the
// rain's direction and very wide views, where the drops are faint).
const int MAX_CROSSINGS = 4;
const int MAX_ACROSS = 6;
const int MAX_UP = 6;

// Splashes (llrhirain.h RAIN_SPLASH_CELL, RAIN_WORLD_PERIOD): on surfaces
// facing up at least this much; each lasts SPLASH_LIFE; a ring spreads on the
// surface, a crown of droplets rises and falls around it. On avatars (the
// pass drawn on their stencil mark with u.snow.y 1) and on an umbrella's
// canopy only the crown: a ring is still water's, not a body's or a cloth's
// (fix-round77).
const float SPLASH_CELL = 0.25;         // m
const float WORLD_PERIOD = 4096.0;      // m
const float SPLASH_FACING = 0.7;
const float CANOPY_FACING = 0.15;      // a canopy's top, down to its rim
const float SPLASH_LIFE = 0.3;          // s
const float RING_SPEED = 0.45;          // m/s
const float RING_WIDTH = 0.008;         // m
const float RING_SHARE = 0.5;
const float CROWN_SPREAD = 0.25;        // m/s
const float CROWN_UP = 1.1;             // m/s: the droplets' speed upwards
const float GRAVITY = 9.8;
const float DROPLET = 0.005;            // m
const float DROPLETS = 9.0;             // around the crown
const float CROWN_HEIGHTS[3] = float[3](0.015, 0.035, 0.055);   // m: where the ray is tried above the surface
const float MAX_CROWN_REACH = 1.0;      // m along the ray (grazing views)
// Heavy rain's large drops (u.sun.w, plus/rain HEAVY_*; 1 as they were): the
// ring is that many times as wide, the crown's droplets that many times as
// large and its height (and where the ray is tried above the surface) that
// many times, its spread sqrt of it; the ring spreads no faster (it stays in
// its cell). The peaks stay as they were: only the area grows.
float splashScale()
{
    return max(u.sun.w, 1.0);
}

// The far rain's haze starts this far off; the sky counts as this far.
const float VEIL_START = 15.0;          // m
const float VEIL_SKY_DISTANCE = 250.0;  // m

// Snow (u.snow.x > 0): each flake flutters sideways around its place in the
// cells, by its own phase, at about SWAY_RATE_A / SWAY_RATE_B rad/s (whole
// turns in the time period, 1000 s, so the wrap does not show).
const float SWAY_RATE_A = 6.2831853 * 333.0 / 1000.0;
const float SWAY_RATE_B = 6.2831853 * 271.0 / 1000.0;

uint hash(uvec2 q, uint seed)
{
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

uint hash3(uvec3 q, uint seed)
{
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u ^ q.z * 0xcb1ab31fu ^ seed * 0x165667b1u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float random(uvec2 q, uint seed)
{
    return float(hash(q, seed) >> 8) * (1.0 / 16777216.0);
}

vec3 eyePosition(ivec2 pixel)
{
    ivec2 p = clamp(pixel, ivec2(0), u.size - 1);
    float depth = texelFetch(scene_depth, p, 0).r;
    vec2 ndc_xy = (vec2(p) + 0.5) / vec2(u.size) * 2.0 - 1.0;
    vec4 eye = u.inverse_projection * vec4(ndc_xy, depth * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
    return eye.xyz / eye.w;
}

// The surface's normal from the depth (water and alpha have none in the
// G-buffer), from the neighbours on the nearer side (not across an edge),
// facing the camera. On the screen's border the neighbour inside it is used
// (the one outside is clamped back to the pixel itself: no difference).
vec3 geometryNormal(ivec2 pixel, vec3 pos)
{
    vec3 left = eyePosition(pixel - ivec2(1, 0));
    vec3 right = eyePosition(pixel + ivec2(1, 0));
    vec3 below = eyePosition(pixel - ivec2(0, 1));
    vec3 above = eyePosition(pixel + ivec2(0, 1));
    bool use_right = pixel.x == 0 || (pixel.x + 1 < u.size.x && abs(right.z - pos.z) < abs(pos.z - left.z));
    bool use_above = pixel.y == 0 || (pixel.y + 1 < u.size.y && abs(above.z - pos.z) < abs(pos.z - below.z));
    vec3 dx = use_right ? right - pos : pos - left;
    vec3 dy = use_above ? above - pos : pos - below;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len <= 0.0)
    {
        return vec3(0.0, 0.0, 1.0);
    }
    n /= len;
    return dot(n, pos) > 0.0 ? -n : n;
}

// How open to the sky a point is (camera-relative world): at the point, and
// upwind as for a roof EAVE_HEIGHT above it, at most MAX_EAVE_REACH off
// (OVERHEAD_GLSL windOpen, as the wet look takes it). In the open both are 1,
// as before; indoors the look upwind no longer reaches the sky past the roof's
// edge in a strong wind. (fix-round67 2)
float pointOpen(vec3 pos)
{
    if (u.map_info.x < 0.5)
    {
        return 1.0;     // no map: open everywhere
    }
    vec4 m = u.to_map * vec4(pos, 1.0);
    float here = overheadOpen(m.xyz, u.map_info, u.map_info2.xy);
    // The map is the same scale across x and y: clip per metre along world x.
    float clip_per_metre = length((u.to_map * vec4(1.0, 0.0, 0.0, 0.0)).xy);
    vec2 shift = upwindShift(u.map_shift.xy, clip_per_metre);
    if (length(shift) <= clip_per_metre * MAX_EAVE_REACH * 0.05)
    {
        return here;    // calm: straight down
    }
    float upwind = overheadOpen(vec3(m.xy + shift, m.z), u.map_info, u.map_info2.xy);
    return windOpen(here, upwind);
}

// How open to the sky a point of a surface is (camera-relative world, its
// normal): looked up a little out along the normal (a wall beside a roof's
// edge takes the rain, one under the eaves does not) and upwind.
float surfaceOpen(vec3 pos, vec3 n)
{
    return pointOpen(pos + n * u.map_info2.z);
}

// A band's share of a drop t metres away: the bands double in distance and
// cross-fade over twice the overlap (in octaves), so their shares add up to 1.
float bandWeight(float t, int band)
{
    float o = u.streak.w;
    float x = log2(max(t, 1e-4) / u.streak.z);
    float b = float(band);
    return smoothstep(b - o, b + o, x) - smoothstep(b + 1.0 - o, b + 1.0 + o, x);
}

// The ray and the rain's direction, per pixel: the streak's line runs along
// fall = (-shear, 1) (up and upwind from a drop: where it was), unnormalised
// so that its parameter is the height above the drop; fall_dot = dot(dir,
// fall), fall_len2 = dot(fall, fall), across2 = |dir x fall|^2 (0 when the
// ray runs along the rain).
struct RainRay
{
    vec3 dir;
    vec3 fall;
    float fall_dot;
    float fall_len2;
    float across2;
};
)GLSL"
    // (Another literal: MSVC takes at most 16 KB in one.)
    R"GLSL(
// Snow (u.snow.z > 0; fix-round79): a flake is a soft round dot, not a streak.
// Real snow falls slowly (about a metre a second), so over the shutter it
// moves less than its own size: drawn as the rain's streak, its fall showed
// as a line three to eight times as long as wide, a meteor's head and tail.
// Here its fall up the streak's line (len, from the head) is kept to at most
// u.snow.z of its drawn width (wide: round, or a little longer down its
// fall), and its light is brightest at the centre and fades smoothly to the
// edge (across: from the ray to the line, h: the closest point up it, both as
// streakAt has them). Blurred wider than the flake by the lens it is as much
// fainter (as a streak); once a pixel is wider than that, by the pixel's
// area (a far flake is a small faint dot, not a bright pixel); flakes nearer
// than u.snow.w fade out, none from half of it (the nearest were large
// blurred blobs across the view).
float flakeShape(float across, float h, float len, float width, float wide, float size, float t, RainRay ray)
{
    // The distance to a point up the line grows from across by sqrt(across2) per unit of its parameter.
    float reach = min(len, u.snow.z * wide * inversesqrt(ray.fall_len2));
    float beyond = h - clamp(h, 0.0, reach);
    float radius = 0.5 * wide;
    float q = (across * across + ray.across2 * beyond * beyond) / (radius * radius);
    if (q >= 1.0)
    {
        return 0.0;
    }
    float soft = (1.0 - q) * (1.0 - q);
    float lens = width + u.streak.y;
    float spread = lens / wide;
    float near_fade = smoothstep(0.5 * u.snow.w, u.snow.w, t);
    return soft * (width / lens) * spread * spread * size * near_fade;
}

// The light of one drop of a band. rel: its head (where it is now) from the
// camera in rain space (the band's cells; x and y as the world's); r: its
// random size. In the world its streak (where it was over the shutter) is
// the line from the head along ray.fall for a height of its length: drawn
// where the ray passes within half its width (measured in the world, at
// right angles to both), nearer than the scene, not under a roof; at least
// a pixel wide, then as much fainter.
float streakAt(vec3 rel, float r, RainRay ray, float scene_dist, int band)
{
    vec3 head_pos = vec3(rel.xy - u.rain.xy * rel.z, rel.z);     // camera-relative world
    if (u.snow.x > 0.0)
    {
        float phase = r * 6.2831853 * 17.0;
        float amplitude = u.snow.x * (0.5 + 0.5 * fract(r * 7.3));
        head_pos.xy += amplitude * vec2(sin(u.world.z * SWAY_RATE_A + phase), cos(u.world.z * SWAY_RATE_B + phase * 1.7));
    }
    if (umbrellaShields(head_pos, -u.rain.xy))
    {
        return 0.0;
    }
    float dd = dot(ray.dir, head_pos);
    float de = dot(ray.fall, head_pos);
    // The closest points: t along the ray (m), h up the streak's line.
    float t = (ray.fall_len2 * dd - ray.fall_dot * de) / ray.across2;
    float h = (ray.fall_dot * dd - de) / ray.across2;
    if (t <= 0.0 || t >= scene_dist)
    {
        return 0.0;
    }
    float size = mix(SIZE_MIN, SIZE_MAX, r * r);
    float width = u.streak.x * size;
    float wide = max(width + u.streak.y, t * u.rain.w);
    float across = length(ray.dir * t - head_pos - ray.fall * h);
    if (across >= 0.5 * wide)
    {
        return 0.0;
    }
    float len = u.rain.z * (1.0 + LENGTH_SPREAD * (2.0 * fract(r * 13.7) - 1.0));
    float shape;
    if (u.snow.z > 0.0)
    {
        shape = flakeShape(across, h, len, width, wide, size, t, ray);
    }
    else
    {
        // Up the line the pixel reaches this far either way: seen end on, far;
        // then the streak is a faint dot.
        float blur = 0.5 * wide / sqrt(ray.across2);
        float head = smoothstep(-blur, blur, h);
        float tail = 1.0 - smoothstep(TAIL_FADE_START * len - blur, len + blur, h);
        float along = head * tail * clamp((len + wide) / (2.0 * blur + wide), 0.0, 1.0);
        shape = (1.0 - across / (0.5 * wide)) * (width / wide) * along * size;
    }
    if (shape <= 0.0)
    {
        return 0.0;
    }
    return shape * bandWeight(t, band) * u.band_cell[band].w * pointOpen(ray.dir * t);
}

// The cells floor(lo / size) to floor(hi / size), at most count of them: when
// there are more, the count nearest centre (the crossing), so that neither
// side of it is the one cut off.
ivec2 cellRange(float lo, float hi, float centre, float size, int count)
{
    int a = int(floor(lo / size));
    int b = int(floor(hi / size));
    if (b - a >= count)
    {
        a = clamp(int(floor(centre / size - 0.5 * float(count) + 0.5)), a, b - count + 1);
        b = a + count - 1;
    }
    return ivec2(a, b);
}

// The streaks of one band's sheets of one family along the ray (rain-space
// direction v) for t in [ta, tb]: the sheets stand across rain-space x, or,
// swapped, across y (the same code with x and y exchanged). Where the ray
// crosses a sheet, the cells of the sheet around the crossing that a drop
// the ray passes could be in are tried.
float sheetDrops(int band, bool swapped, vec3 v, float ta, float tb, float scene_dist, RainRay ray)
{
    vec4 cell = u.band_cell[band];
    vec3 o = u.band_camera[band].xyz;
    uvec3 base = u.band_base[band].xyz;
    uvec3 planes = u.band_planes[band].xyz;
    uint seed = 211u + uint(band) * 13u;
    if (swapped)
    {
        o = o.yxz;
        v = v.yxz;
        base = base.yxz;
        planes = planes.yxz;
        seed = 307u + uint(band) * 13u;
    }
    if (abs(v.x) < 1e-5)
    {
        return 0.0;
    }
    float q = cell.x;
    float hz = cell.y;
    int spacing_cells = int(max(planes.z, 1u));
    float spacing = float(spacing_cells) * q;
    float first = float(planes.x) * q;
    float vh_len = length(v.xy);
    float facing = abs(v.x) / vh_len;       // the sine of the angle between the ray and the sheets
    // Rain space stretches distances across the rain by up to this much
    // (slanted rain seen from the side): a drop within half its width in
    // the world can be that much farther from the ray in rain space.
    float stretch = sqrt(ray.across2) / vh_len;
    float longest = u.rain.z * (1.0 + LENGTH_SPREAD);
    float widest = u.streak.x * SIZE_MAX + u.streak.y + 2.0 * u.snow.x;

    float xa = o.x + v.x * ta;
    float xb = o.x + v.x * tb;
    int k0 = int(ceil((min(xa, xb) - first) / spacing));
    int k1 = min(int(floor((max(xa, xb) - first) / spacing)), k0 + MAX_CROSSINGS - 1);
    float drops = 0.0;
    for (int k = k0; k <= k1; ++k)
    {
        float sheet = first + float(k) * spacing;
        float th = (sheet - o.x) / v.x;
        vec3 h = o + v * th;
        // A drop the ray passes within its width is this far from the
        // crossing across the sheet, and up or down it.
        float wide = max(widest, th * u.rain.w);
        float reach = 0.5 * wide * stretch / facing;
        float rise = abs(v.z) * reach / vh_len + 0.5 * wide / sqrt(ray.across2);
        // At most MAX_ACROSS by MAX_UP cells, centred on the crossing (up the
        // sheet: on the middle of where a streak through it can have its head).
        ivec2 across = cellRange(h.y - reach, h.y + reach, h.y, q, MAX_ACROSS);
        ivec2 up = cellRange(h.z - rise - longest, h.z + rise, h.z - 0.5 * longest, hz, MAX_UP);
        int c0 = across.x;
        int c1 = across.y;
        int z0 = up.x;
        int z1 = up.y;
        uint sheet_id = base.x + uint(int(planes.x) + k * spacing_cells);
        for (int c = c0; c <= c1; ++c)
        {
            for (int zc = z0; zc <= z1; ++zc)
            {
                uint bits = hash3(uvec3(sheet_id, base.y + uint(c), base.z + uint(zc)), seed);
                if (float(bits & 0xffu) >= cell.z * 256.0)
                {
                    continue;   // an empty cell
                }
                vec3 d = vec3(sheet,
                              (float(c) + 0.1 + 0.8 * float((bits >> 8) & 0xffu) * (1.0 / 255.0)) * q,
                              (float(zc) + float((bits >> 16) & 0xffu) * (1.0 / 256.0)) * hz);
                vec3 rel = d - o;
                drops += streakAt(swapped ? rel.yxz : rel, float(bits >> 24) * (1.0 / 255.0), ray, scene_dist, band);
            }
        }
    }
    return drops;
}
)GLSL"
    // (Another literal: MSVC takes at most 16 KB in one.)
    R"GLSL(
// Snow's level sheets (fix-round82). Seen from above, the flakes on the
// vertical sheets lay in a lattice: a ray looking steeply down runs
// nearly along the sheets and crosses few of them (in a band right below the
// camera often none), and the nearest sheet's flakes all crowd into a thin
// line. So the snow has a third family of sheets lying level in rain space
// (falling with the flakes), every planes.z cells up z counted in cell widths
// (u.band_base.w, u.band_camera.w, u.band_planes.w), their cells cell_width
// square, a flake anywhere in its cell. A level cell is cell_width /
// cell_height of a sheet's cell, so it holds a flake that much less often:
// as many flakes in a cubic metre, as bright. The level family counts fully
// for rays at least LEVEL_FULL steep (rain space: the sine of the angle below
// or above the level), not at all below LEVEL_NONE (there the vertical ones
// as before); a ray crosses the families' sheets as often at about 45
// degrees. The rain keeps its two families (u.snow.z 0).
const float LEVEL_NONE = 0.6;
const float LEVEL_FULL = 0.8;
// Across a level sheet at most this many cells along x and along y are tried
// around a crossing (a flake's reach there: its drawn half width, its
// stretch and its sway, sideways both ways at once, over the ray's
// steepness).
const int MAX_LEVEL_CELLS = 6;
const float SWAY_REACH = 1.42;

// The level family's share for a ray (rain-space direction v): 0 for rain.
float levelWeight(vec3 v)
{
    if (u.snow.z <= 0.0)
    {
        return 0.0;
    }
    return smoothstep(LEVEL_NONE, LEVEL_FULL, abs(v.z) / length(v));
}

// The flakes of one band's level sheets along the ray (rain-space direction
// v) for t in [ta, tb], as sheetDrops finds the vertical sheets' drops.
float levelDrops(int band, vec3 v, float ta, float tb, float scene_dist, RainRay ray)
{
    if (abs(v.z) < 1e-5)
    {
        return 0.0;
    }
    vec4 cell = u.band_cell[band];
    float q = cell.x;
    vec3 o = vec3(u.band_camera[band].xy, u.band_camera[band].w);
    uvec3 base = uvec3(u.band_base[band].xy, u.band_base[band].w);
    uint first_cells = u.band_planes[band].w;
    int spacing_cells = int(max(u.band_planes[band].z, 1u));
    float spacing = float(spacing_cells) * q;
    float first = float(first_cells) * q;
    float probability = min(cell.z * q / cell.y, 1.0);
    uint seed = 401u + uint(band) * 13u;
    // The sheets are level in the world too: a flake the ray passes within
    // its reach is at most reach / steepness from the crossing.
    float steep = max(abs(ray.dir.z), 0.1);
    float widest = u.streak.x * SIZE_MAX + u.streak.y;

    float za = o.z + v.z * ta;
    float zb = o.z + v.z * tb;
    int k0 = int(ceil((min(za, zb) - first) / spacing));
    int k1 = min(int(floor((max(za, zb) - first) / spacing)), k0 + MAX_CROSSINGS - 1);
    float drops = 0.0;
    for (int k = k0; k <= k1; ++k)
    {
        float sheet = first + float(k) * spacing;
        float th = (sheet - o.z) / v.z;
        vec3 h = o + v * th;
        float wide = max(widest, th * u.rain.w);
        float reach = ((0.5 + u.snow.z) * wide + SWAY_REACH * u.snow.x) / steep;
        ivec2 xs = cellRange(h.x - reach, h.x + reach, h.x, q, MAX_LEVEL_CELLS);
        ivec2 ys = cellRange(h.y - reach, h.y + reach, h.y, q, MAX_LEVEL_CELLS);
        uint sheet_id = base.z + uint(int(first_cells) + k * spacing_cells);
        for (int cx = xs.x; cx <= xs.y; ++cx)
        {
            for (int cy = ys.x; cy <= ys.y; ++cy)
            {
                uint bits = hash3(uvec3(base.x + uint(cx), base.y + uint(cy), sheet_id), seed);
                if (float(bits & 0xffu) >= probability * 256.0)
                {
                    continue;   // an empty cell
                }
                vec3 d = vec3((float(cx) + float((bits >> 8) & 0xffu) * (1.0 / 256.0)) * q,
                              (float(cy) + float((bits >> 16) & 0xffu) * (1.0 / 256.0)) * q,
                              sheet);
                drops += streakAt(d - o, float(bits >> 24) * (1.0 / 255.0), ray, scene_dist, band);
            }
        }
    }
    return drops;
}
)GLSL"
    // (Another literal: MSVC takes at most 16 KB in one.)
    R"GLSL(
// Eaves: water drips off the edge of a roof (the overhead map). The edge is
// where the map's height steps by at least u.eave.y between two neighbouring
// EAVE_CELL cells (a roof against the ground or a lower roof; the map holds
// the topmost surface, so the roof is the higher one). A pixel's ray is walked
// cell by cell across the map (horizontally, camera-relative world, the grid
// fixed in the world); where it crosses the face between two cells that step,
// it meets the edge's curtain: columns EAVE_COLUMN apart along the face, some
// of which drip. A drip hangs at the edge, then falls from it in free fall,
// drawn as its path over the shutter.
const float EAVE_CELL = 0.25;           // m (divides WORLD_PERIOD)
const float EAVE_COLUMN = 0.125;        // m (divides WORLD_PERIOD)
const int EAVE_STEPS = 64;
const float EAVE_BEAD = 0.006;          // m: a drop's width
const float EAVE_HANG_SHARE = 0.45;     // of a drip's cycle spent hanging at the edge
const float EAVE_MAX_FALL = 12.0;       // m: drips are not followed farther down
const float EAVE_SHUTTER = 0.035;       // s
const float EAVE_BRIGHTNESS = 1.5;
const float EAVE_FADE_FROM = 0.6;       // of the range: where drips start to fade out

// The map's height (camera-relative world z) over the cell, or -1e4 where
// the map holds nothing (a cleared texel); inside says whether the cell is in
// the map at all.
float eaveHeight(ivec2 cell, out bool inside)
{
    vec2 rel = (vec2(cell) + 0.5) * EAVE_CELL - u.world.xy;
    vec4 m = u.to_map * vec4(rel, 0.0, 1.0);
    inside = abs(m.x) < 1.0 && abs(m.y) < 1.0;
    if (!inside)
    {
        return -1e4;
    }
    ivec2 size = textureSize(overhead_map, 0);
    ivec2 texel = clamp(ivec2((m.xy * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1);
    float depth = texelFetch(overhead_map, texel, 0).r;
    if (depth >= 1.0)
    {
        return -1e4;
    }
    float clip_z = depth * u.map_info2.x + u.map_info2.y;
    return (clip_z - u.to_map[3][2]) / u.to_map[2][2];     // (the map looks straight down: clip z depends on world z only)
}

// The light of a drip's bead hanging at height top_z, falling from it (its
// random bits, which also choose its rate and phase), seen t metres along the
// ray at height z (camera-relative), lateral metres from the ray; none once it
// has landed (below floor_z) or farther than range (faded from EAVE_FADE_FROM
// of it).
float beadLight(uint bits, float t, float lateral, float z, float top_z, float floor_z, float range)
{
    // Three rates, each a whole number of cycles in the time period (1000 s).
    float rate = 1.0;
    switch (int((bits >> 16) & 3u) % 3)
    {
        case 0: rate = 0.8; break;
        case 2: rate = 1.25; break;
    }
    float phase = fract(u.world.z * rate + float((bits >> 18) & 0xffu) * (1.0 / 256.0));
    float fall_time = max(phase - EAVE_HANG_SHARE, 0.0) / rate;
    float speed = GRAVITY * fall_time;
    float grow = phase < EAVE_HANG_SHARE ? mix(0.4, 1.0, phase / EAVE_HANG_SHARE) : 1.0;
    float bead_z = top_z - 0.012 - 0.5 * GRAVITY * fall_time * fall_time;
    if (bead_z < floor_z)
    {
        return 0.0;
    }
    float wide = max(EAVE_BEAD * grow, t * u.rain.w);
    if (lateral >= 0.5 * wide)
    {
        return 0.0;
    }
    float length_up = max(speed * EAVE_SHUTTER, EAVE_BEAD);
    float vertical = smoothstep(bead_z - 0.5 * wide, bead_z, z) * (1.0 - smoothstep(bead_z + length_up, bead_z + length_up + 0.5 * wide, z));
    float fade = 1.0 - smoothstep(EAVE_FADE_FROM * range, range, t);
    return (1.0 - lateral / (0.5 * wide)) * (EAVE_BEAD * grow / wide) * vertical * fade * EAVE_BRIGHTNESS;
}

// The light of the drip, if any, where the ray crosses the edge's curtain:
// the face between two cells (axis 0: across x, 1: across y) on grid line
// `line`, t metres along the ray, `along` (m, world) along the face, at height
// z (camera-relative); roof_z the edge's height, low_z the lower side's;
// perp: how squarely the ray crosses the face.
float eaveDrip(int axis, int line, float t, float along, float z, float roof_z, float low_z, float perp)
{
    float column_at = floor(along / EAVE_COLUMN);
    uint id = uint(mod(column_at, WORLD_PERIOD / EAVE_COLUMN));
    uint bits = hash3(uvec3(id, uint(mod(float(line), WORLD_PERIOD / EAVE_CELL)), uint(axis)), 91u);
    if (float(bits & 0xffu) >= u.eave.x * 256.0)
    {
        return 0.0;
    }
    float centre = (column_at + 0.15 + 0.7 * float((bits >> 8) & 0xffu) * (1.0 / 255.0)) * EAVE_COLUMN;
    return beadLight(bits, t, abs(along - centre) * perp, z, roof_z, max(low_z, roof_z - EAVE_MAX_FALL), u.eave.z);
}

// The drips the ray meets before it reaches the scene (scene_dist).
float eaveDrops(vec3 dir, float scene_dist)
{
    if (u.eave.x <= 0.0 || u.map_info.x < 0.5 || length(dir.xy) < 1e-3)
    {
        return 0.0;
    }
    float t_end = min(scene_dist, u.eave.z);
    float horizontal = length(dir.xy);
    vec2 start = u.world.xy / EAVE_CELL;                // the camera, in cells
    vec2 step_cells = dir.xy / EAVE_CELL;               // cells per metre of ray
    ivec2 cell = ivec2(floor(start));
    ivec2 stepping = ivec2(sign(dir.xy));
    vec2 per_cell = 1.0 / max(abs(step_cells), vec2(1e-6));     // metres of ray per cell, per axis
    vec2 to_edge = vec2(stepping.x > 0 ? float(cell.x + 1) - start.x : start.x - float(cell.x),
                        stepping.y > 0 ? float(cell.y + 1) - start.y : start.y - float(cell.y));
    vec2 t_next = to_edge * per_cell;
    bool inside;
    float previous = eaveHeight(cell, inside);
    float light = 0.0;
    for (int i = 0; i < EAVE_STEPS && inside; ++i)
    {
        int axis = t_next.x < t_next.y ? 0 : 1;
        float t = axis == 0 ? t_next.x : t_next.y;
        if (t >= t_end)
        {
            break;
        }
        if (axis == 0)
        {
            cell.x += stepping.x;
            t_next.x += per_cell.x;
        }
        else
        {
            cell.y += stepping.y;
            t_next.y += per_cell.y;
        }
        float current = eaveHeight(cell, inside);
        // (Both sides need a surface: against an empty texel, water or a hole, no edge.)
        if (inside && min(current, previous) > -1000.0 && abs(current - previous) >= u.eave.y)
        {
            float roof_z = max(current, previous);
            float low_z = min(current, previous);
            float z = dir.z * t;
            if (z < roof_z && z > low_z)
            {
                int line = axis == 0 ? (stepping.x > 0 ? cell.x : cell.x + 1) : (stepping.y > 0 ? cell.y : cell.y + 1);
                float along = axis == 0 ? start.y * EAVE_CELL + dir.y * t : start.x * EAVE_CELL + dir.x * t;
                float perp = max(abs(axis == 0 ? dir.x : dir.y) / horizontal, 0.25);
                light += eaveDrip(axis, line, t, along, z, roof_z, low_z, perp);
            }
        }
        previous = current;
    }
    return light;
}

// The drips off a canopy's rim: UMBRELLA_COLUMNS columns around it, some of
// which drip, as the eaves do.
const int UMBRELLA_COLUMNS = 20;
const float UMBRELLA_RANGE = 12.0;      // m
const float UMBRELLA_MAX_FALL = 3.0;    // m

float umbrellaDrips(vec3 dir, float scene_dist)
{
    vec2 across = dir.xy;
    float horizontal = dot(across, across);
    if (u.umbrella_info.y <= 0.0 || horizontal < 1e-6)
    {
        return 0.0;
    }
    float light = 0.0;
    for (int i = 0; i < MAX_UMBRELLAS; ++i)
    {
        vec4 umbrella = u.umbrella[i];
        if (umbrella.w <= 0.0)
        {
            continue;
        }
        float t_centre = dot(umbrella.xy, across) / horizontal;
        // Only rays that pass near the canopy are worth the columns.
        if (t_centre <= 0.0 || t_centre - umbrella.w >= min(scene_dist, UMBRELLA_RANGE) ||
            length(umbrella.xy - across * t_centre) > umbrella.w + 0.1)
        {
            continue;
        }
        float rim_z = umbrella.z - u.umbrella_info.x;
        for (int k = 0; k < UMBRELLA_COLUMNS; ++k)
        {
            uint bits = hash3(uvec3(uint(k), uint(i), 5u), 97u);
            if (float(bits & 0xffu) >= u.umbrella_info.y * 256.0)
            {
                continue;
            }
            float angle = 6.2831853 * (float(k) + 0.6 * float((bits >> 8) & 0xffu) * (1.0 / 255.0)) / float(UMBRELLA_COLUMNS);
            vec2 column = umbrella.xy + umbrella.w * vec2(cos(angle), sin(angle));
            float t = dot(column, across) / horizontal;
            if (t <= 0.0 || t >= scene_dist)
            {
                continue;
            }
            // (The distance from a vertical line to a ray is the horizontal one, to the ray's projection.)
            light += beadLight(bits, t, length(column - across * t), dir.z * t, rim_z, rim_z - UMBRELLA_MAX_FALL, UMBRELLA_RANGE);
        }
    }
    return light;
}
)GLSL"
    R"GLSL(
// The splash in the cell (of one of two offset layers) around world xy w,
// if one is under way: its centre's offset from w (m) and its age (s).
bool splashNear(vec2 w, uint layer, out vec2 offset, out float age)
{
    vec2 q = w / SPLASH_CELL + vec2(0.37, 0.61) * float(layer);
    vec2 cell = floor(q);
    uvec2 id = uvec2(mod(cell, WORLD_PERIOD / SPLASH_CELL));
    float cycle = u.world.z * u.world.w + random(id, 40u + layer);
    age = fract(cycle) / u.world.w;
    if (age > SPLASH_LIFE)
    {
        return false;
    }
    uint event = uint(floor(cycle)) * 2u;
    vec2 centre = cell + 0.15 + 0.7 * vec2(random(id, 50u + event), random(id, 51u + event));
    offset = (q - centre) * SPLASH_CELL;
    return true;
}

// The crown's droplets at a point height h above the surface, offset d from
// the splash's centre, age into it.
float crownAt(vec2 d, float h, float age, float pixel, float scale)
{
    float fade = 1.0 - age / SPLASH_LIFE;
    float root = sqrt(scale);
    float radius = CROWN_SPREAD * root * age;
    float height = CROWN_UP * root * age - 0.5 * GRAVITY * age * age;   // (its peak: scale times as high)
    if (height <= 0.0)
    {
        return 0.0;
    }
    float droplet = DROPLET * scale;
    float size = max(droplet, pixel);
    float dist = length(d);
    float band = max(1.0 - abs(dist - radius) / size, 0.0) * max(1.0 - abs(h - height) / (size * 2.0), 0.0);
    // (atan(0, 0) is undefined: at the very centre, any angle.)
    float angle = dist > 1e-6 ? atan(d.y, d.x) : 0.0;
    float around = pow(0.5 + 0.5 * cos(DROPLETS * angle), 6.0);
    return band * around * fade * (droplet / size);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 ndc = (vec2(pixel) + 0.5) / vec2(u.size) * 2.0 - 1.0;
    vec4 near_eye = u.inverse_projection * vec4(ndc, -1.0, 1.0);
    mat3 to_world = mat3(u.eye_to_world);
    vec3 dir = normalize(to_world * (near_eye.xyz / near_eye.w));

    float depth = texelFetch(scene_depth, pixel, 0).r;
    bool sky = depth >= 1.0;
    vec3 scene_eye = vec3(0.0);
    float scene_dist = 1e9;
    if (!sky)
    {
        scene_eye = eyePosition(pixel);
        scene_dist = length(scene_eye);
    }
    float exposure = u.exposure.x > 0.0 ? max(texelFetch(exposure_map, ivec2(0), 0).r * u.exposure.x, 1e-4) : 1.0;

    // What a drop shows (exposed): its surroundings, dimmer under a dark sky;
    // and the sun through the drops in front of it.
    float sky_light = dot(u.ambient.rgb * exposure, vec3(0.2126, 0.7152, 0.0722));
    vec3 shown = u.drop_light.rgb * clamp(sky_light / AMBIENT_REFERENCE, MIN_LIT, 1.0);
    float glint_limit = u.glint.a > 0.0 ? u.glint.a : GLINT_LIMIT;     // (snow: lower, rain.cpp SNOW_GLINT_LIMIT)
    vec3 glint = min(u.glint.rgb * exposure, vec3(glint_limit)) * pow(max(dot(dir, u.sun.xyz), 0.0), GLINT_POWER);

    // Streaks: the ray in rain space (sheared by the wind), band by band.
    float drops = 0.0;
    vec3 v = vec3(dir.xy + u.rain.xy * dir.z, dir.z);
    float vh_len = length(v.xy);
    int bands = int(u.exposure.y);
    RainRay ray;
    ray.dir = dir;
    ray.fall = vec3(-u.rain.xy, 1.0);
    ray.fall_dot = dot(dir, ray.fall);
    ray.fall_len2 = dot(ray.fall, ray.fall);
    ray.across2 = max(ray.fall_len2 - ray.fall_dot * ray.fall_dot, 0.0);
    float end_on = smoothstep(END_ON_NONE, END_ON_FULL, sqrt(ray.across2 / ray.fall_len2));
    if (vh_len > END_ON_NONE && end_on > 0.0)
    {
        // (Snow: the level sheets take steep rays; rain: weight_level 0, the two as before.)
        float weight_level = levelWeight(v);
        float share_x = smoothstep(FAMILY_NONE, FAMILY_FULL, abs(v.x) / (abs(v.x) + abs(v.y)));
        float weight_x = (1.0 - weight_level) * share_x;
        float weight_y = (1.0 - weight_level) * (1.0 - share_x);
        for (int i = 0; i < MAX_BANDS; ++i)
        {
            if (i >= bands)
            {
                break;
            }
            float ta = u.streak.z * exp2(float(i) - u.streak.w);
            float tb = min(u.streak.z * exp2(float(i + 1) + u.streak.w), scene_dist);
            if (ta >= tb)
            {
                break;      // behind the scene, as every farther band
            }
            if (weight_x > 0.0)
            {
                drops += weight_x * sheetDrops(i, false, v, ta, tb, scene_dist, ray);
            }
            if (weight_y > 0.0)
            {
                drops += weight_y * sheetDrops(i, true, v, ta, tb, scene_dist, ray);
            }
            if (weight_level > 0.0)
            {
                drops += weight_level * levelDrops(i, v, ta, tb, scene_dist, ray);
            }
        }
        drops *= end_on;
    }
    drops += eaveDrops(dir, scene_dist) + umbrellaDrips(dir, scene_dist);

    // Splashes on surfaces facing up near the camera, and the far haze, where
    // the rain reaches.
    float splash = 0.0;
    float veil = 0.0;
    vec3 scene = to_world * scene_eye;
    bool near_surface = !sky && u.world.w > 0.0 && scene_dist < u.splash.x;
    vec3 n = near_surface ? to_world * geometryNormal(pixel, scene_eye) : vec3(0.0, 0.0, 1.0);
    // A canopy's top splashes too, however steep its rim; the overhead map knows nothing of it.
    bool canopy = near_surface && onUmbrella(scene);
    // (The overhead map has no attachments: under a canopy the ground looks open to it, so the canopy's own shield is asked.)
    bool splashing = near_surface && (canopy ? n.z > CANOPY_FACING : (n.z > SPLASH_FACING && !umbrellaShields(scene, -u.rain.xy)));
    float scene_open = 1.0;
    if (!sky && (splashing || u.splash.z > 0.0))
    {
        scene_open = canopy ? 1.0 : surfaceOpen(scene, n);
    }
    if (splashing && scene_open > 0.0)
    {
        float pixel_size = scene_dist * u.rain.w;
        float scale = splashScale();
        vec2 w = scene.xy + u.world.xy;
        vec2 offset;
        float age;
        // The ring on the surface (not on an avatar nor a canopy).
        bool rings = !canopy && u.snow.y < 0.5;
        for (uint layer = 0u; rings && layer < 2u; ++layer)
        {
            if (splashNear(w, layer, offset, age))
            {
                float ring_width = RING_WIDTH * scale;
                float width = max(ring_width, pixel_size);
                float ring = max(1.0 - abs(length(offset) - RING_SPEED * age) / width, 0.0);
                float fade = 1.0 - age / SPLASH_LIFE;
                splash += ring * fade * fade * RING_SHARE * (ring_width / width);
            }
        }
        // The crown: the ray a little above the surface, back towards the camera.
        if (dir.z < 0.0)
        {
            for (int k = 0; k < 3; ++k)
            {
                float h = CROWN_HEIGHTS[k] * scale;
                vec3 x = scene - dir * min(h / -dir.z, MAX_CROWN_REACH);
                float hx = x.z - scene.z;
                if (splashNear(x.xy + u.world.xy, 0u, offset, age))
                {
                    splash += crownAt(offset, hx, age, pixel_size, scale);
                }
            }
        }
        splash *= scene_open * (1.0 - smoothstep(0.6 * u.splash.x, u.splash.x, scene_dist)) * u.splash.y;
    }
    if (u.splash.z > 0.0)
    {
        float far_dist = sky ? VEIL_SKY_DISTANCE : scene_dist;
        veil = u.splash.z * (1.0 - exp(-max(far_dist - VEIL_START, 0.0) / max(u.splash.w, 1.0))) * scene_open;
    }

    // A streak takes the pixel towards what the drop shows, by its opacity
    // where it covers the pixel; only part of that hides what lies behind (a
    // drop is clear: it shows its surroundings, bright against the dark, about
    // as bright as the sky against the sky). The sun through it is added.
    float cover = clamp(drops, 0.0, 1.0);
    float opacity = u.drop_light.a;
    vec3 light = cover * (opacity * shown + glint) + splash * (shown + glint) + veil * shown;
    float alpha = clamp(cover * opacity * u.ambient.a + veil, 0.0, 1.0);
    frag_color = vec4(light / exposure, alpha);
}
)GLSL";

    // ------------------------------------------------------------------
    // The uniform block, as the shader's std140 block lays it out.

    struct Uniforms
    {
        F32 inverse_projection[16];
        F32 eye_to_world[16];
        F32 to_map[16];
        F32 map_info[4];
        F32 map_info2[4];
        F32 map_shift[4];
        F32 rain[4];
        F32 world[4];
        F32 drop_light[4];
        F32 ambient[4];
        F32 glint[4];
        F32 sun[4];
        F32 streak[4];
        F32 splash[4];
        F32 exposure[4];
        F32 snow[4];
        F32 umbrella[LLRHI::RAIN_UMBRELLAS][4];
        F32 umbrella_info[4];
        F32 eave[4];
        F32 band_cell[LLRHI::RAIN_BANDS][4];
        F32 band_camera[LLRHI::RAIN_BANDS][4];
        U32 band_base[LLRHI::RAIN_BANDS][4];
        U32 band_planes[LLRHI::RAIN_BANDS][4];
        F32 depth_to_ndc[2];
        S32 size[2];
    };
    static_assert(sizeof(Uniforms) == 800, "Uniforms must match the std140 block RainStreaks");
    static_assert(LLRHI::RAIN_BANDS == 5, "RAIN_HEAD declares five bands");
    static_assert(LLRHI::RAIN_SPLASH_CELL == 0.25f && LLRHI::RAIN_WORLD_PERIOD == 4096.f, "RAIN_BODY's SPLASH_CELL and WORLD_PERIOD");

    constexpr U32 RAIN_IMAGES = 3;      // the scene depth, the exposure, the overhead map

    // The two halves of the pass while avatars are marked (fix-round77): the
    // rest as it was, the avatars without the splashes' rings.
    constexpr LLVKFullscreen::StencilTest NOT_AVATARS = { true, 0, LLRHI::STENCIL_AVATAR };
    constexpr LLVKFullscreen::StencilTest ON_AVATARS = { true, LLRHI::STENCIL_AVATAR, LLRHI::STENCIL_AVATAR };

    // ------------------------------------------------------------------
    // The renderer

    class Renderer
    {
    public:
        bool streaks(const LLRHI::RainStreaks& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();
        bool splitReady();
        static const char* fragment();

        bool mTried = false;
        bool mReady = false;
        LLVKFullscreen::Pass mRain;
        // Made the first time avatars are marked while it rains.
        bool mTriedSplit = false;
        bool mSplitReady = false;
        LLVKFullscreen::Pass mRainNotAvatars;
        LLVKFullscreen::Pass mRainOnAvatars;
        bool mWarnedTarget = false;
        bool mLogged = false;
        // The textures the pass samples: kept so that no frame allocates them anew.
        std::vector<LLVKTracker::GLName> mSampled;
    };

    Renderer sRenderer;

    const char* Renderer::fragment()
    {
        static const std::string fragment = std::string(RAIN_HEAD) + LLVKRain::OVERHEAD_GLSL + RAIN_BODY;
        return fragment.c_str();
    }

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mReady = mRain.init("rain", fragment(), RAIN_IMAGES, LLVKFullscreen::Blend::PremultipliedOver);
            if (!mReady)
            {
                LL_WARNS("VKRain") << "The rain pass could not be set up; it stays off" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::splitReady()
    {
        if (!mTriedSplit)
        {
            mTriedSplit = true;
            mSplitReady = mRainNotAvatars.init("rain (not avatars)", fragment(), RAIN_IMAGES, LLVKFullscreen::Blend::PremultipliedOver,
                                               LLVKFullscreen::WRITE_RGBA, NOT_AVATARS) &&
                          mRainOnAvatars.init("rain (avatars)", fragment(), RAIN_IMAGES, LLVKFullscreen::Blend::PremultipliedOver,
                                              LLVKFullscreen::WRITE_RGBA, ON_AVATARS);
            if (!mSplitReady)
            {
                LL_WARNS("VKRain") << "The rain's avatar passes could not be set up; the splashes' rings stay on avatars" << LL_ENDL;
            }
        }
        return mSplitReady;
    }

    // One vec4 of the block.
    void row(F32 out[4], F32 x, F32 y, F32 z, F32 w)
    {
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    bool Renderer::streaks(const LLRHI::RainStreaks& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!ready() || !pass.depth_texture || !pass.exposure_texture || !pass.width || !pass.height)
        {
            return false;
        }
        mSampled.assign({ pass.depth_texture, pass.exposure_texture });
        if (pass.overhead.depth_texture)
        {
            mSampled.push_back(pass.overhead.depth_texture);
        }
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mSampled, target);
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
                LL_WARNS("VKRain") << "Rain: the bound target is " << target.width << "x" << target.height << ", not the " << pass.width
                                   << "x" << pass.height << " it was called for; not drawn" << LL_ENDL;
            }
            return false;
        }

        Uniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.eye_to_world, pass.eye_to_world, sizeof(uniforms.eye_to_world));
        const LLRHI::RainOverhead& map = pass.overhead;
        memcpy(uniforms.to_map, map.to_map, sizeof(uniforms.to_map));
        F32 depth_to_ndc[2];
        LLVKFullscreen::depthToNDC(depth_to_ndc);
        row(uniforms.map_info, map.depth_texture ? 1.f : 0.f, llmax(map.cover_bias, 0.f), 0.f, llmax(map.edge, 1e-4f));
        row(uniforms.map_info2, depth_to_ndc[0], depth_to_ndc[1], llmax(map.normal_offset, 0.f), 0.f);
        row(uniforms.map_shift, map.rain_shift[0], map.rain_shift[1], 0.f, 0.f);
        row(uniforms.rain, pass.shear[0], pass.shear[1], llmax(pass.streak_length, 1e-3f), llmax(pass.pixel_angle, 0.f));
        row(uniforms.world, pass.camera_world[0], pass.camera_world[1], pass.time, llmax(pass.splash_cell_rate, 0.f));
        row(uniforms.drop_light, pass.drop_light[0], pass.drop_light[1], pass.drop_light[2], llclamp(pass.drop_opacity, 0.f, 1.f));
        row(uniforms.ambient, pass.ambient[0], pass.ambient[1], pass.ambient[2], llclamp(pass.drop_hide, 0.f, 1.f));
        row(uniforms.glint, pass.glint_color[0], pass.glint_color[1], pass.glint_color[2], llmax(pass.glint_limit, 0.f));
        row(uniforms.sun, pass.sun_direction[0], pass.sun_direction[1], pass.sun_direction[2],
            pass.splash_scale > 1.f ? llmin(pass.splash_scale, 4.f) : 1.f);    // (not a number: 1)
        // The bands' cross-fade must have some width (the shader's smoothsteps need e0 < e1).
        row(uniforms.streak, llmax(pass.drop_width, 1e-4f), llmax(pass.defocus, 0.f), llmax(pass.first_band, 0.05f),
            llclamp(pass.band_overlap, 0.05f, 0.5f));
        row(uniforms.splash, llmax(pass.splash_range, 0.f), llmax(pass.splash_brightness, 0.f), llclamp(pass.veil, 0.f, 1.f),
            llmax(pass.veil_distance, 1.f));
        U32 bands = 0;
        for (U32 i = 0; i < llmin(pass.band_count, LLRHI::RAIN_BANDS); ++i)
        {
            const LLRHI::RainBand& band = pass.bands[i];
            // A band must have cells to find (the shader divides by their size and counts in whole cells);
            // the first one without ends the bands.
            if (band.cell_width <= 0.f || band.cell_height <= 0.f || band.plane_spacing < 1)
            {
                break;
            }
            row(uniforms.band_cell[i], band.cell_width, band.cell_height, llclamp(band.probability, 0.f, 1.f), llmax(band.brightness, 0.f));
            // (w: the level sheets', the snow's; fix-round82.)
            row(uniforms.band_camera[i], band.camera[0], band.camera[1], band.camera[2], llclamp(band.level_camera, 0.f, band.cell_width));
            uniforms.band_base[i][0] = band.base[0];
            uniforms.band_base[i][1] = band.base[1];
            uniforms.band_base[i][2] = band.base[2];
            uniforms.band_base[i][3] = band.level_base;
            uniforms.band_planes[i][0] = band.plane_offset[0] % band.plane_spacing;
            uniforms.band_planes[i][1] = band.plane_offset[1] % band.plane_spacing;
            uniforms.band_planes[i][2] = band.plane_spacing;
            uniforms.band_planes[i][3] = band.level_offset % band.plane_spacing;
            ++bands;
        }
        row(uniforms.exposure, pass.exposure_scale, (F32)bands, 0.f, 0.f);
        // z: the flakes' stretch (0: rain's streaks); w: their near fade, kept above 0 (the shader's smoothstep needs e0 < e1).
        row(uniforms.snow, llclamp(pass.sway, 0.f, 0.5f), 0.f, llclamp(pass.flake_stretch, 0.f, 4.f), llclamp(pass.flake_near_fade, 0.1f, 10.f));
        for (U32 i = 0; i < LLRHI::RAIN_UMBRELLAS; ++i)
        {
            const LLRHI::RainUmbrella& umbrella = pass.umbrellas[i];
            row(uniforms.umbrella[i], umbrella.top[0], umbrella.top[1], umbrella.top[2], llmax(umbrella.radius, 0.f));
        }
        row(uniforms.umbrella_info, llmax(pass.umbrella_rim_drop, 0.f), llclamp(pass.umbrella_drip, 0.f, 1.f), 0.f, 0.f);
        row(uniforms.eave, map.depth_texture ? llclamp(pass.eave_density, 0.f, 1.f) : 0.f, llmax(pass.eave_step, 0.1f),
            llmax(pass.eave_range, 1.f), 0.f);
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)target.width;
        uniforms.size[1] = (S32)target.height;

        // The scene depth is the target's own (shared): attached read-only and sampled in that layout.
        LLVKFullscreen::Image images[RAIN_IMAGES];
        images[0] = { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1].texture = pass.exposure_texture;
        images[2].texture = map.depth_texture;      // 0: a white stand-in (map_info.x 0: never read)
        // While the stencil tells avatars from the rest (fix-round77): the rest
        // as before, then the avatars with no rings (snow.y 1); the two
        // stencil tests cover every pixel once. The stencil is the attached
        // depth's: it must be the scene depth itself (attached read-only as it
        // is sampled). Without the mark (or should the first half not be
        // recorded) the one pass over everything.
        bool drawn = false;
        if (pass.avatars_marked && target.stencil && target.depth_read_only && splitReady())
        {
            drawn = mRainNotAvatars.draw(cmd, target, &uniforms, sizeof(uniforms), images, RAIN_IMAGES);
            if (drawn)
            {
                uniforms.snow[1] = 1.f;
                // (Not recorded: the avatars go without rain this frame; never drawn twice.)
                mRainOnAvatars.draw(cmd, target, &uniforms, sizeof(uniforms), images, RAIN_IMAGES);
            }
        }
        if (!drawn)
        {
            uniforms.snow[1] = 0.f;
            drawn = mRain.draw(cmd, target, &uniforms, sizeof(uniforms), images, RAIN_IMAGES);
        }
        LLVKTracker::endPass();
        if (drawn && !mLogged)
        {
            mLogged = true;
            LL_INFOS("VKRain") << "Rain: " << target.width << "x" << target.height << ", " << bands << " bands, "
                               << (map.depth_texture ? "with" : "without") << " the overhead map, colour format "
                               << (U32)target.color_formats[0] << LL_ENDL;
        }
        return drawn;
    }

    bool record_streaks(const LLRHI::RainStreaks& pass)
    {
        return sRenderer.streaks(pass);
    }
}

void LLVKRain::install()
{
    if (!LLRHI::sRain)
    {
        LL_INFOS("VKRain") << "Rain off (LLRHI_RAIN=0)" << LL_ENDL;
        return;
    }
    LLRHI::sRainStreaks = &record_streaks;
    LLVKShader::addBakeStep("rain", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
