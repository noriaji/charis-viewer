/**
 * @file llrhivolumetric.h
 * @brief Volumetric light (sun shafts): what the viewer hands the backend.
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

#ifndef LL_LLRHIVOLUMETRIC_H
#define LL_LLRHIVOLUMETRIC_H

#include "stdtypes.h"

// Research/84. The viewer (plus/volumetric) draws the light scattered by the
// air between the camera and the scene in two passes, each into the GL
// framebuffer it has bound:
//   1. march: a reduced-resolution target; per pixel, a ray from the eye to
//      the scene depth (at most to the end of the sun's last shadow cascade)
//      sums the light that reaches it past the shadow maps. RGB: scattered
//      light, A: the pixel's view depth (for the upsample).
//   2. composite: the scene's HDR target (before tone mapping); the march's
//      result, upsampled with the scene depth as the guide, is laid over it:
//      the target is dimmed by the transmittance of the air the march crossed
//      and the air's light (the march's, and the sky's ambient for the light
//      taken out) is added. The march sums its light so that it and the
//      transmittance never come to more than 1 (fix-r9).
// Matrices are column-major 4x4, eye space as GL's (looking down -z).
//
// The clouds' shadow in the air (research/253, with plus/volclouds' map; off while cloud_strength
// is 0, and the passes are then as above): the march multiplies the light that reaches each step by
// what the clouds above let through (the near air), and past its end, up to the clouds' base (a ray
// to the sky) or the scene, sums how much the sky's haze is lit more or less than the air around the
// camera (the far air: the sky already draws that haze, so nothing is added where the air is as lit
// as around the camera). Its channels are then shares: r the near air's light over the light's colour,
// g the far air's brighter share, b the share of the scene's light the far air's shadows take out; the
// composite gives the first the light's colour, and with the other two lights the haze more or less in
// the units the sky is drawn in (skyF.glsl's), taking the difference to the screen's light as the haze is
// taken there (made linear, times its scale): at a pixel of the sky the sky's haze in that direction
// (softenLightF.glsl), at a pixel of the scene the scene's own haze up to it (atmosphericsFuncs.glsl, as
// hazeF.glsl lays it over the scene).
namespace LLRHI
{
    // skyF.glsl holds the sky's colour (its units, before softenLightF.glsl makes it linear) under this.
    constexpr F32 SKY_COLOUR_MAX = 5.f;

    // The clouds' shadow map (plus::volclouds::AirShadow) as the march reads it; the places are from
    // the camera, east, north and up (m).
    struct VolumetricClouds
    {
        U32 map_texture = 0;            // GL name of the map (R8, 1 - exp(-optical depth) of the layer above
                                        // each place, sampled trilinear, with mips); 0: none
        F32 strength = 0.f;             // the light a cloud of map value 1 takes out (PlusCloudShadow times the
                                        // low light's fade); 0: no clouds' shadow in the air
        F32 axes[3][3] = {};            // east, north, up in eye space (unit)
        F32 inverse_width = 0.f;        // 1 / the map's width (1/m)
        F32 layer_base = 0.f;           // the layer's base above the camera (m): where a ray to the sky leaves the air
        F32 layer_middle = 0.f;         // the layer's middle above the camera (m): where the way to the light is cut
        F32 light_slope[2] = {};        // the way to the light: metres east and north per metre up
        F32 camera_place[2] = {};       // the map's uv where the way to the light from the camera crosses the layer's middle
        F32 reference_lod = 0.f;        // the mip whose texel spans the air around the camera (the far air's reference)
        F32 far_extinction = 0.f;       // the far air's extinction (1/m); 0: no far air (the near air only)
        U32 far_stages = 0;             // its stages, of equal light each
        // The sky's haze (skyF.glsl), luminance, for the share of a pixel's light the far air's sun
        // light is: its ambient share, its sun share in every direction, around the light (times the glow),
        // the sun's as much as the sky draws of it below the clouds (times 1 - cloud_shadow).
        F32 haze_shares[3] = {};
        F32 glow[3] = {};               // the sky's glow: x and z (the glow's size and power), the glow's factor (0: none)
        // The same for a ray to the scene, of the scene's own haze (atmosphericsFuncs.glsl: the light dimmed for
        // lightnorm.y, tmpAmbient), and hazeF.glsl's lightnorm for its glow (VolumetricCloudsLight::scene_lightnorm).
        F32 scene_shares[3] = {};
        F32 scene_lightnorm[3] = {};
    };

    // What the composite needs to give the march's shares their light. Used when the march before it
    // had the clouds' shadow in the air (the backend keeps that, so the two never disagree).
    struct VolumetricCloudsLight
    {
        F32 light_color[3] = {};        // VolumetricMarch::light_color: the near air's light
        F32 light_direction[3] = {};    // VolumetricMarch::light_direction
        // The sky's haze as skyF.glsl draws it (RGB, in its units), for the sky in each pixel's direction:
        F32 haze_sun[3] = {};           // its sun light in every direction, and around the light (times the glow),
        F32 haze_glow[3] = {};          // for a share of 1, before the air above the ray dims the light
        F32 haze_ambient[3] = {};       // its ambient light below the clouds (with the clouds'),
        F32 haze_above[3] = {};         // and above them
        F32 haze_drawn = 1.f;           // the share of the light the sky draws below the clouds (1 - cloud_shadow)
        F32 light_depth[3] = {};        // the light's optical depth to a ray (light_atten): dimmed by exp(-this / (up + light_up))
        F32 air_depth[3] = {};          // the air's optical depth straight up (its combined haze x density x max_y)
        F32 up[3] = {};                 // up in eye space (unit)
        F32 light_up = 0.f;             // the light's up (lightnorm's)
        F32 glow[3] = {};               // VolumetricClouds::glow
        F32 sky_scale = 1.f;            // the sky's colour (its units) to the screen's linear light: softenLightF.glsl's
                                        // sky_hdr_scale (times 1.1 under a classic sky), after srgb_to_linear
        F32 far_ceiling = 0.f;          // the brightest each channel of the haze may be lit to, in its units (at most
                                        // SKY_COLOUR_MAX; white under the "no post" tone mapping)
        // The scene's own haze (atmosphericsFuncs.glsl's calcAtmosphericVars, which hazeF.glsl lays over the scene),
        // for a pixel of the scene:
        F32 scene_extinction[3] = {};   // its extinction per metre (combined_haze x density_multiplier x distance_multiplier)
        F32 scene_scale = 1.f;          // its colour (the sky's units) to the screen's linear light: hazeF.glsl's
                                        // sky_hdr_scale, after srgb_to_linear (no 1.1 under a classic sky)
        F32 scene_lightnorm[3] = {};    // hazeF.glsl's lightnorm: LLEnvironment::getClampedLightNorm() as it is (Windlight's
                                        // order north, up, east; its glow dots it with the eye-space ray)
        F32 scene_ambient[3] = {};      // its ambient light below the clouds (2 x the weights x atmosphericsFuncs.glsl's
                                        // tmpAmbient, which has no max(0, ...) as skyF.glsl's has)
    };

    struct VolumetricMarch
    {
        U32 depth_texture = 0;          // GL name of the scene depth (full resolution)
        U32 depth_width = 0;            // its size in pixels
        U32 depth_height = 0;
        U32 scale = 1;                  // full-resolution pixels per march pixel, each way (the bound target's reduction)
        U32 shadow_textures[4] = {};    // GL names of the sun's shadow map depths, nearest cascade first
        F32 shadow_matrices[4][16] = {};// eye space -> shadow map texture space ([0, 1], z as stored by GL)
        F32 shadow_far[4] = {};         // each cascade's far end (view depth, m)
        F32 inverse_projection[16] = {};
        F32 light_direction[3] = {};    // towards the light, eye space, unit length
        F32 light_color[3] = {};        // linear RGB: the light at full scattering (the march sums the share
                                        // scattered), times the phase function's normalisation
        F32 extinction = 0.f;           // per metre, for the light on its way back to the eye; the base air
                                        // scatters all it takes out (its scattering is the same)
        F32 anisotropy = 0.f;           // Henyey-Greenstein g: > 0 scatters forwards (bright towards the light)
        U32 steps = 0;                  // samples along each ray
        F32 max_length = 0.f;           // m: no ray marches farther (0: up to the last cascade's end)
        // Mist (the ground fog of plus/atmosphere): the air is denser low down. At a sample its
        // extinction is the base's plus mist_extinction * p and its scattering the base's times
        // (1 + mist_scatter * p), p = exp(-mist_inverse_height * height above the level) (1 at
        // and below the level); never more than its extinction.
        F32 up_eye[3] = {};             // world up, eye space, unit length
        F32 mist_camera_height = 0.f;   // m above the level (negative: below)
        F32 mist_inverse_height = 0.f;  // 1/m; 0: no mist
        F32 mist_scatter = 0.f;         // extra scattering at the level, as a multiple of the base's
        F32 mist_extinction = 0.f;      // extra extinction at the level, per metre
        VolumetricClouds clouds;        // the clouds' shadow in the air (research/253); off while its strength is 0
    };

    struct VolumetricComposite
    {
        U32 scatter_texture = 0;        // GL name of the march's target
        U32 scatter_width = 0;
        U32 scatter_height = 0;
        U32 scale = 1;                  // the march's VolumetricMarch::scale
        U32 depth_texture = 0;          // GL name of the scene depth (the bound target's own depth)
        F32 depth_a = 0.f;              // the projection's [2][2] and [3][2] (column-major 10 and 14),
        F32 depth_b = 0.f;              // for the view depth of a depth value
        F32 march_far = 0.f;            // view depth where the march stops at the latest (the last
                                        // cascade's far end, VolumetricMarch::shadow_far[3]); 0: no edge fallback
        // The air the march crossed along each pixel's ray, as the march's rays: up to the scene,
        // the view depth shadow_end and the length max_length, whichever is nearest.
        F32 ray_scale[2] = {};          // 1 / the projection's [0][0] and [1][1]
        F32 ray_offset[2] = {};         // the projection's [2][0] and [2][1]: eye x / -z = (ndc x + offset) * scale
        F32 shadow_end = 0.f;           // VolumetricMarch::shadow_far[3] (view depth, m); 0: no air
        F32 max_length = 0.f;           // VolumetricMarch::max_length (m); 0: no limit
        F32 extinction = 0.f;           // VolumetricMarch::extinction (per metre)
        F32 air_ambient[3] = {};        // linear RGB: the sky's light the air scatters, in place of the light it takes out
        VolumetricCloudsLight clouds;   // with the clouds' shadow in the air (research/253)
    };

    // A blur of the march's light along one axis, at the march's size: the
    // texels of a 9-tap Gaussian whose view depth (alpha) is near the
    // centre's (so a near object's light does not run onto the far one
    // beside it). Smooths the march's dither and the shadow maps' texels.
    struct VolumetricBlur
    {
        U32 source_texture = 0;         // GL name of the light to blur (rgb light, a view depth)
        U32 width = 0;                  // its size (and the bound target's)
        U32 height = 0;
        S32 direction[2] = { 1, 0 };    // (1, 0) across, (0, 1) down
    };

    // On unless LLRHI_VOLUMETRIC=0 (a switch for tests: the passes are then
    // never recorded). The viewer's setting (PlusVolumetric, off by default)
    // decides whether they run.
    extern bool sVolumetric;

    // Set by the Vulkan backend. Each records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sVolumetricMarch)(const VolumetricMarch& pass);
    extern bool (*sVolumetricComposite)(const VolumetricComposite& pass);
    extern bool (*sVolumetricBlur)(const VolumetricBlur& pass);
}

#endif // LL_LLRHIVOLUMETRIC_H
