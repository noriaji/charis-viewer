/**
 * @file llrhigtao.h
 * @brief Ground-truth ambient occlusion (GTAO) and contact shadows: what the
 *        viewer hands the backend.
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

#ifndef LL_LLRHIGTAO_H
#define LL_LLRHIGTAO_H

#include "stdtypes.h"

// Research/92. The viewer (plus/gtao) writes into the deferred light map
// (LLPipeline's mRT->deferredLight: r the sun's shadow, g ambient occlusion)
// before the official blur softens it. Each pass draws into the GL
// framebuffer bound when it is called:
//   1. GtaoDepth: the scene depth as view depth (m), at 1/scale each way (R32F).
//   2. GtaoOcclusion: per pixel of that, the GTAO visibility (R) and the view
//      depth again (G), for the passes after it (RG16F).
//   3. GtaoDenoise: a depth-aware 4x4 average of that, which takes out the
//      4x4 noise pattern of the slice and step offsets (RG16F).
//   4. GtaoApply: the light map. The denoised visibility, upsampled with the
//      scene depth as the guide, replaces its g; nothing else is written.
//   5. ContactShadows: the light map. A short ray towards the light from each
//      pixel; where the depth buffer hides it, r is lowered (the smaller of the
//      two is kept); nothing else is written. Each 2x2 quad of pixels averages
//      its four rays (their start offsets interleave).
//   4+5. GtaoApplyContact: both of the above in one pass over the light map
//      (research/125 #9), for when the sun program left g at 1 (it drew
//      without the official SSAO): the smaller of each and the light map's is
//      kept, r and g only, which for g is the apply pass's value (it is <= 1).
//      The same pixels as the two passes, one read and write of the light map
//      fewer.
// Projection values are GL's (column-major, eye space looking down -z). A
// view depth d (> 0) comes from a stored depth value through its NDC z:
// d = depth_b / (ndc z + depth_a). The eye-space position of the point at
// NDC (x, y) and view depth d is (d * (x + [2][0]) / [0][0], d * (y + [2][1]) / [1][1], -d).
namespace LLRHI
{
    // The camera and the scene depth, which every pass needs.
    struct GtaoView
    {
        F32 depth_a = 0.f;              // the projection's [2][2]
        F32 depth_b = 0.f;              // and [3][2]
        F32 projection[4] = {};         // its [0][0], [1][1], [2][0], [2][1]
        U32 depth_texture = 0;          // GL name of the scene depth (full resolution)
        U32 width = 0;                  // its size in pixels (the light map's too)
        U32 height = 0;
    };

    struct GtaoDepth
    {
        GtaoView view;
        U32 scale = 1;                  // full-resolution pixels per target pixel, each way
    };

    struct GtaoOcclusion
    {
        GtaoView view;
        U32 view_depth_texture = 0;     // GtaoDepth's target
        U32 width = 0;                  // its size (the bound target's too)
        U32 height = 0;
        U32 scale = 1;                  // as GtaoDepth's
        F32 radius = 0.f;               // m: how far away a surface still occludes
        F32 max_screen_radius = 0.f;    // the radius on screen at most, in target pixels
        U32 slices = 0;                 // directions around each pixel
        U32 steps = 0;                  // samples on each side of a direction
    };

    struct GtaoDenoise
    {
        U32 occlusion_texture = 0;      // GtaoOcclusion's target
        U32 width = 0;
        U32 height = 0;
    };

    struct GtaoApply
    {
        GtaoView view;
        U32 occlusion_texture = 0;      // GtaoDenoise's target
        U32 width = 0;
        U32 height = 0;
        U32 scale = 1;                  // as GtaoDepth's
        F32 power = 1.f;                // visibility ^ power goes into the light map (> 1 darker)
    };

    struct ContactShadows
    {
        GtaoView view;
        F32 light_direction[3] = {};    // towards the light, eye space, unit length
        F32 length = 0.f;               // m: how far each ray goes
        F32 thickness = 0.f;            // m: how deep a surface is taken to be behind its depth
        F32 fade_start = 0.f;           // view depth (m) where contact shadows start to fade out
        F32 fade_end = 0.f;             // and where they are gone
        // Close up: a ray covers at most this many full-resolution pixels on
        // screen (its length in metres shrinks to fit), and contact shadows fade
        // out between these sizes of a metre on screen at the pixel's depth, in
        // screen heights (projection [1][1] / 2 / view depth).
        F32 max_screen_length = 0.f;
        F32 near_fade_start = 0.f;
        F32 near_fade_end = 0.f;
        U32 steps = 0;                  // samples along each ray at most
    };

    // On unless LLRHI_GTAO=0 (a switch for tests: the passes are then never
    // recorded). The viewer's settings (PlusGtao, PlusContactShadows, both
    // off by default) decide whether they run.
    extern bool sGtao;

    // Set by the Vulkan backend. Each records its pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sGtaoDepth)(const GtaoDepth& pass);
    extern bool (*sGtaoOcclusion)(const GtaoOcclusion& pass);
    extern bool (*sGtaoDenoise)(const GtaoDenoise& pass);
    extern bool (*sGtaoApply)(const GtaoApply& pass);
    extern bool (*sContactShadows)(const ContactShadows& pass);
    // apply.view and contact.view must be the same view. Only where the bound
    // light map's g is 1 everywhere (see 4+5 above).
    extern bool (*sGtaoApplyContact)(const GtaoApply& apply, const ContactShadows& contact);
}

#endif // LL_LLRHIGTAO_H
