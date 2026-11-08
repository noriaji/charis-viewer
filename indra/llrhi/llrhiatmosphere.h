/**
 * @file llrhiatmosphere.h
 * @brief Atmosphere effects for photography (ground fog, fireflies): what the viewer hands the backend.
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

#ifndef LL_LLRHIATMOSPHERE_H
#define LL_LLRHIATMOSPHERE_H

#include "stdtypes.h"

// research/148 (plus/atmosphere). Projection values are GL's (column-major,
// eye space looking down -z).
namespace LLRHI
{
    // Ground fog: air that is thickest at a reference level (the ground) and
    // thins out exponentially with height. One fullscreen pass on the scene's
    // HDR target, drawn before the alpha (so the alpha blends over the fog as
    // over the rest of the scene), laid over it: the scene behind each pixel
    // is dimmed by the fog's transmittance along the pixel's ray (to the
    // scene depth, or FogGround::max_distance for the sky) and the fog's own
    // light is added. The optical depth along a ray is integrated in closed
    // form (one sample per pixel). The light is in the scene's units.
    struct GroundFog
    {
        U32 depth_texture = 0;      // the scene depth (the bound target's own)
        F32 inverse_projection[16] = {};
        F32 eye_to_world[16] = {};  // eye space -> world axes (rotation only)
        F32 density = 0.f;          // extinction per metre at the reference level and below it
        F32 inverse_height = 0.f;   // 1/m: the density falls by e over this height
        F32 camera_height = 0.f;    // m, the camera above the reference level
        F32 max_distance = 0.f;     // m: no ray is followed farther (the sky counts as this far)
        F32 ambient[3] = {};        // the fog's light from the sky (scene units)
        F32 sun[3] = {};            // the sun's (moon's) light scattered by the fog, times the phase function below
        F32 sun_direction[3] = {};  // world, towards the light, unit length
        F32 anisotropy = 0.f;       // Henyey-Greenstein g: > 0 scatters forwards (bright towards the light)
        U32 width = 0;              // the bound target's size
        U32 height = 0;
    };

    // Fireflies: points of light drifting in a layer of air above the ground,
    // fixed in the world (they stay where they are when the camera turns or
    // moves). One fullscreen pass on the scene's HDR target after the exposure
    // is measured and before the glare (so that the glare and the cross filter
    // pick the bright ones up), added to it, in the scene's units (the pass's
    // exposed brightness divided by the exposure).
    //
    // The layer is cut into square cells (cell_size); a cell holds at most one
    // firefly (probability), which wanders around the middle of its cell on a
    // slow Lissajous path and blinks, its glow always inside its cell. A pixel
    // walks its ray through the cells the ray crosses within the layer and
    // range (a digital differential analyser), nearer than the scene.
    struct Fireflies
    {
        U32 depth_texture = 0;      // the scene depth (the bound target's own)
        U32 exposure_texture = 0;   // the measured exposure (1x1)
        F32 exposure_scale = 1.f;   // RenderExposure, as the tone mapping clamps it; 0: none (1, the map not read: "no post")
        F32 inverse_projection[16] = {};
        F32 eye_to_world[16] = {};  // eye space -> world axes (rotation only)
        F32 cell_size = 0.f;        // m
        U32 cell_base[2] = {};      // the cell the camera is in (world cell indices wrapped to 32 bits)
        F32 camera_in_cell[2] = {}; // m: where the camera is within that cell (0..cell_size)
        F32 layer_bottom = 0.f;     // m above the camera (negative: below it)
        F32 layer_top = 0.f;
        F32 range = 0.f;            // m: none farther
        F32 probability = 0.f;      // 0..1: a cell holds a firefly
        F32 time = 0.f;             // s, wrapped at FIREFLY_TIME_PERIOD
        F32 core_radius = 0.f;      // m: the bright point
        F32 glow_radius = 0.f;      // m: the soft glow around it (its standard deviation)
        F32 glow_share = 0.f;       // the glow's brightness as a share of the point's
        F32 color[3] = {};          // exposed units: the point's peak
        F32 pixel_angle = 0.f;      // rad: one pixel's height seen from the camera
        U32 width = 0;              // the bound target's size
        U32 height = 0;
    };

    // The fireflies' time wraps at this (s): their paths and blinks repeat
    // every FIREFLY_TIME_PERIOD / k seconds for a whole k.
    constexpr F32 FIREFLY_TIME_PERIOD = 64.f;

    // On unless LLRHI_ATMOSPHERE=0 (a switch for tests: no pass of
    // plus/atmosphere is ever recorded). The viewer's settings (all off by
    // default) decide whether they run.
    extern bool sAtmosphere;

    // Set by the Vulkan backend. Records the pass into the bound draw
    // framebuffer (the scene's HDR target); false when it could not.
    extern bool (*sGroundFog)(const GroundFog& pass);
    extern bool (*sFireflies)(const Fireflies& pass);
}

#endif // LL_LLRHIATMOSPHERE_H
