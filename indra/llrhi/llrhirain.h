/**
 * @file llrhirain.h
 * @brief Rain (falling streaks, splashes) and the overhead map that keeps it
 *        off what lies under a roof: what the viewer hands the backend.
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

#ifndef LL_LLRHIRAIN_H
#define LL_LLRHIRAIN_H

#include "stdtypes.h"

// Research/120 sections 7 and 8 (stages 2 and 3 of the rain).
//
// The overhead map (RainOverhead): the viewer (plus/rain) draws the static
// world around the camera from straight above, orthographic, into a depth
// map (through the official shadow caster path, so nothing here records
// it). A point is under cover where the map holds a surface well above it.
// The rain pass and the wet look (llrhiwetlook.h) both read it.
//
// The rain pass (RainStreaks), one fullscreen pass on the scene's HDR target
// after the exposure is measured and before tone mapping, laid over it
// (premultiplied: a drop adds its light and hides a little of what lies
// behind it), in the scene's units (its exposed brightness divided by the
// exposure):
//   - streaks: drops fixed in the world (they fall and drift with the wind,
//     never with the camera), each drawn as its path over a short shutter
//     and projected like any other world line; the pixel's ray finds them
//     where it crosses world-fixed vertical sheets of drops (two crossed
//     families of sheets, sparser and with larger cells band by band away
//     from the camera); a drop is drawn where nothing in the scene is nearer
//     and the overhead map has no roof above it;
//   - splashes: on surfaces facing up near the camera, in world cells (they
//     stay put), a ring on the surface and a crown of droplets above it,
//     where the rain reaches the surface;
//   - a faint haze of the far rain.
// Projection values are GL's (column-major, eye space looking down -z).
namespace LLRHI
{
    // The overhead map as a pass reads it. The pass's points are in its own
    // space (eye space for the wet look, world axes with the camera at the
    // origin for the rain); to_map takes them to the map's clip space
    // (orthographic: x, y across the map, z from its top (-1) to its bottom (1)).
    struct RainOverhead
    {
        U32 depth_texture = 0;      // 0: no map (everything is open to the sky)
        F32 to_map[16] = {};
        F32 cover_bias = 0.f;       // clip z: a surface covers a point only when this much above it
        F32 edge = 0.f;           // clip x, y: the map fades to open sky over this much before its edge
        F32 normal_offset = 0.f;    // m: a surface point is looked up this far out along its normal
        F32 rain_shift[2] = {};     // clip x, y per metre of height: where slanted rain comes from (wind)
    };

    // Umbrellas that shield the rain (an attachment of the viewer's own avatar, found by name):
    // a shallow dome, its top centre in the pass's space (camera-relative world, m).
    constexpr U32 RAIN_UMBRELLAS = 2;
    struct RainUmbrella
    {
        F32 top[3] = {};
        F32 radius = 0.f;           // m; 0: none
    };

    // Bands of distance from the camera, each twice as far as the one before.
    constexpr U32 RAIN_BANDS = 5;
    // RainStreaks::time wraps at this (s): the splash cells' rate times it is whole.
    constexpr F32 RAIN_TIME_PERIOD = 1000.f;
    // The splashes' world cell (m); divides LLRHI::WET_LOOK_WORLD_PERIOD (4096 m),
    // the period RainStreaks::camera_world wraps at.
    constexpr F32 RAIN_SPLASH_CELL = 0.25f;
    constexpr F32 RAIN_WORLD_PERIOD = 4096.f;

    // One band of drops. The drops live in "rain space": the world's x, y
    // plus RainStreaks::shear times the world height, and the world height
    // plus how far the rain has fallen. A drop falling with the wind stays at
    // one point of it, so its cell alone fixes where it is. A band's drops lie
    // on vertical sheets every plane_spacing cells along x (one family) and
    // along y (the other), at most one in each cell of a sheet. Snow has a
    // third family (fix-round82): level sheets every plane_spacing cells up
    // rain space z, their cells cell_width square, for rays looking steeply
    // down (seen from above, the vertical sheets showed as a lattice).
    struct RainBand
    {
        F32 cell_width = 0.f;       // m (rain space x and y)
        F32 cell_height = 0.f;      // m (rain space z): at least the longest streak
        F32 probability = 0.f;      // 0..1: a cell holds a drop
        F32 brightness = 0.f;       // more drops than cells: fewer, brighter ones (energy kept)
        U32 plane_spacing = 1;      // cells from one sheet to the next
        U32 base[3] = {};           // the cell the camera is in (rain space, integers wrapped to 32 bits)
        F32 camera[3] = {};         // m: where the camera is within that cell (0..cell size)
        U32 plane_offset[2] = {};   // cells from base to the first sheet along x, y (0..plane_spacing-1)
        // The level sheets (snow): rain space z counted in cell_width cells.
        U32 level_base = 0;         // the camera's cell up z (wrapped to 32 bits)
        F32 level_camera = 0.f;     // m: where the camera is within it (0..cell_width)
        U32 level_offset = 0;       // cells from level_base to the first level sheet (0..plane_spacing-1)
    };

    struct RainStreaks
    {
        U32 depth_texture = 0;      // the scene depth (the bound target's own)
        U32 exposure_texture = 0;   // the measured exposure (1x1)
        F32 exposure_scale = 1.f;   // RenderExposure, as the tone mapping clamps it; 0: none (1, the map not read: "no post")
        F32 inverse_projection[16] = {};
        F32 eye_to_world[16] = {};  // eye space -> world axes, the camera at the origin (rotation only)
        RainOverhead overhead;      // to_map from that camera-relative space
        F32 shear[2] = {};          // m sideways per m of fall (world x, y): the wind over the fall speed
        F32 streak_length = 0.f;    // m: a drop's fall over the shutter (in rain space, straight up from it)
        F32 camera_world[2] = {};   // m: the camera's world x, y wrapped at RAIN_WORLD_PERIOD (splashes)
        F32 time = 0.f;             // s, wrapped at RAIN_TIME_PERIOD
        F32 drop_light[3] = {};     // exposed units: what a drop shows (its surroundings' light, the sky's tint)
        F32 drop_opacity = 0.f;     // 0..1: a streak's strength where it covers a whole pixel
        F32 drop_hide = 0.f;        // 0..1: the share of that strength that hides what lies behind
        F32 ambient[3] = {};        // the sky's light, scene units (linear): a dark sky dims the drops
        F32 glint_color[3] = {};    // scene units: the sun (moon) seen through drops in front of it
        F32 sun_direction[3] = {};  // world, towards the sun (moon), unit length
        F32 drop_width = 0.f;       // m
        F32 defocus = 0.f;          // m: the lens's blur, added to every drop's width
        RainUmbrella umbrellas[RAIN_UMBRELLAS];     // rain does not fall through them, splash crowns on top, drips off the rim
        F32 umbrella_rim_drop = 0.f;    // m: the rim hangs this far below the top
        F32 umbrella_drip = 0.f;        // 0..1: the share of the rim's columns that drip (0: none)
        F32 eave_density = 0.f;     // 0..1: the share of a roof edge's columns that drip; 0: no drips (needs the overhead map)
        F32 eave_step = 0.f;        // m: a step in the map's height this big is a roof's edge
        F32 eave_range = 0.f;       // m: drips are drawn no farther
        F32 sway = 0.f;             // m: snow: each flake flutters this far (sideways, own phase); 0: rain
        // Snow (fix-round79): a flake is a soft round dot, not a streak. 0 for rain (its streaks as they were).
        F32 flake_stretch = 0.f;    // snow: a flake's motion blur at most this share of its drawn width
        F32 flake_near_fade = 0.f;  // m: snow: flakes nearer than this fade out (none at half of it)
        F32 glint_limit = 0.f;      // exposed: the sun (moon) through a drop at most this; 0: the shader's own (rain)
        F32 pixel_angle = 0.f;      // rad: one pixel's height seen from the camera
        F32 first_band = 0.f;       // m: band i covers first_band * 2^i to twice that
        F32 band_overlap = 0.f;     // octaves: neighbouring bands cross-fade over twice this
        RainBand bands[RAIN_BANDS];
        U32 band_count = 0;         // 0: no streaks
        F32 splash_cell_rate = 0.f; // splashes per RAIN_SPLASH_CELL square per s (times RAIN_TIME_PERIOD whole); 0: none
        F32 splash_range = 0.f;     // m: none farther
        F32 splash_brightness = 0.f;
        F32 splash_scale = 1.f;     // heavy rain: the splashes' size (ring width, crown height and droplets) this many times; 1 as they were
        F32 veil = 0.f;             // 0..1: the far rain's haze at its thickest
        F32 veil_distance = 0.f;    // m: how deep the haze builds up
        // The depth's stencil tells avatars from the rest this frame (LLRHI::STENCIL_AVATAR,
        // llrhiskinsss.h): their pixels get the splashes' crowns but no rings (fix-round77).
        bool avatars_marked = false;
        U32 width = 0;              // the bound target's size
        U32 height = 0;
    };

    // On unless LLRHI_RAIN=0 (a switch for tests: the rain pass is never
    // recorded, and the viewer draws no overhead map). The viewer's setting
    // (PlusRain, off by default) decides whether it runs.
    extern bool sRain;

    // Set by the Vulkan backend. Records the pass into the bound draw
    // framebuffer (the scene's HDR target); false when it could not.
    extern bool (*sRainStreaks)(const RainStreaks& pass);
}

#endif // LL_LLRHIRAIN_H
