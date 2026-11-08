/**
 * @file llrhiwetlook.h
 * @brief Wet look (wet ground and puddles): what the viewer hands the
 *        backend.
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

#ifndef LL_LLRHIWETLOOK_H
#define LL_LLRHIWETLOOK_H

#include "stdtypes.h"

#include "llrhirain.h"

// Research/120 section 6 (stage 1 of the rain). Between the G-buffer pass
// and the lighting, the viewer (plus/wetlook) makes the G-buffer wet, so the
// official lighting (sun, local lights, reflection probes) lights the wet
// surfaces as it lights any other:
//   - the base colour darker and more saturated, by how porous the surface
//     is (rough surfaces soak up water, metals and glossy ones do not);
//   - the roughness lower (PBR), the specular colour and gloss higher
//     (legacy materials);
//   - puddles on flat ground (a low-frequency noise in world space): almost
//     no roughness, no metal, the normal flat with rain ripples on it.
// Surfaces facing up take all of it, walls a share, undersides none, and
// nothing under a roof (the overhead map, llrhirain.h; research/120 section 8).
// A shader cannot read the G-buffer while drawing into it, so it takes two
// passes, each into the GL framebuffer bound when it is called:
//   1. WetLookCompute: the bound target is a copy target with three colour
//      attachments in the G-buffer's first three formats (base colour,
//      specular / ORM, normal), sharing the main view's depth. The wet
//      G-buffer goes there; pixels that are not wet (the sky) are copied.
//   2. WetLookWriteBack: the bound target is the G-buffer itself (three
//      attachments, or four with the emissive one, which is left alone).
//      The copy target's base colour, specular and normal are written back,
//      except on the sky (left as it is).
// Both may test the avatar mark (STENCIL_AVATAR, llrhiskinsss.h) so that
// avatars stay dry or take less; a second compute pass on their bare skin
// (STENCIL_AVATAR_SKIN) lays no snow there.
// Projection values are GL's (column-major, eye space looking down -z).
namespace LLRHI
{
    // The world offset in WetLookCompute::eye_to_world is the global position
    // of the agent frame wrapped at this (m): the puddle noise repeats with it,
    // so it stays put across regions and keeps its precision.
    constexpr F32 WET_LOOK_WORLD_PERIOD = 4096.f;
    // WetLookCompute::time wraps at this (s): the ripples' rate times it is a
    // whole number of rings, so the wrap does not show.
    constexpr F32 WET_LOOK_TIME_PERIOD = 1000.f;
    // The ripples' rings per cell and second without rain (WetLookCompute::
    // ripple_rate; any rate used times WET_LOOK_TIME_PERIOD must be whole).
    constexpr F32 WET_LOOK_RIPPLE_RATE = 0.8f;
    // WetLookCompute::water_height without water: below everything.
    constexpr F32 WET_LOOK_NO_WATER = -1.0e6f;

    // Which pixels a pass draws on (the avatar mark in the main view's stencil).
    enum WetLookPixels : U32
    {
        WET_LOOK_EVERYWHERE = 0,        // no stencil test
        WET_LOOK_NOT_AVATARS = 1,       // where STENCIL_AVATAR is clear
        WET_LOOK_AVATARS = 2,           // where it is set
        WET_LOOK_AVATAR_SKIN = 3,       // where STENCIL_AVATAR_SKIN is set (compute only: drawn over an earlier pass)
    };

    struct WetLookCompute
    {
        U32 depth_texture = 0;          // the scene depth (the bound target's own)
        U32 diffuse_texture = 0;        // G-buffer 0: base colour (legacy: sRGB, alpha the fullbright mix; PBR: linear)
        U32 specular_texture = 0;       // G-buffer 1: legacy specular colour and gloss; PBR occlusion, roughness, metallic
        U32 normal_texture = 0;         // G-buffer 2: encoded normal, environment intensity, the G-buffer flag
        F32 inverse_projection[16] = {};
        F32 eye_to_world[16] = {};      // eye space -> world (agent, offset so the puddles stay put across regions)
        F32 up[3] = {};                 // world up in eye space, unit length
        F32 wetness = 0.f;              // 0..1: the water film on surfaces facing up
        F32 soaked = 0.f;               // 0..1: how much of the film is falling rain's (walls take more of it,
                                        // level surfaces get glossier, no dry patches between the puddles)
        F32 water_height = WET_LOOK_NO_WATER;   // world (agent) z of the water: nothing below it is made wet
        F32 darkening = 1.f;            // 0..1: the share of the darkening the film brings (less on avatars)
        F32 puddles = 0.f;              // 0..1: how much of the flat ground is under puddles
        F32 ripples = 0.f;              // the rain ripples' tilt of the puddles' normal (0: still water)
        F32 time = 0.f;                 // s, for the ripples, wrapped at WET_LOOK_TIME_PERIOD
        F32 ripple_rate = WET_LOOK_RIPPLE_RATE;     // rings per cell and second (more while it rains)
        F32 snow = 0.f;                 // 0..1: the snow cover on what is open to the sky (flat surfaces first, then slopes)
        RainOverhead overhead;          // to_map from eye space; no map: everything is open to the sky
        WetLookPixels pixels = WET_LOOK_EVERYWHERE;
    };

    struct WetLookWriteBack
    {
        U32 diffuse_texture = 0;        // WetLookCompute's target, attachment 0
        U32 specular_texture = 0;       // attachment 1
        U32 normal_texture = 0;         // attachment 2
        WetLookPixels pixels = WET_LOOK_EVERYWHERE;     // where the compute pass drew (not WET_LOOK_AVATARS alone)
    };

    // On unless LLRHI_WET_LOOK=0 (a switch for tests: the passes are then
    // never recorded). The viewer's setting (PlusWetLook, off by default)
    // decides whether they run.
    extern bool sWetLook;

    // Set by the Vulkan backend. Each records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sWetLookCompute)(const WetLookCompute& pass);
    extern bool (*sWetLookWriteBack)(const WetLookWriteBack& pass);
}

#endif // LL_LLRHIWETLOOK_H
