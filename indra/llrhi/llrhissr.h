/**
 * @file llrhissr.h
 * @brief Screen space reflections in passes of their own (a port of the
 *        official project/graphics-care-package's): what the viewer hands
 *        the backend.
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

#ifndef LL_LLRHISSR_H
#define LL_LLRHISSR_H

#include "stdtypes.h"

// Research/129 (sl-viewer). The official viewer traces screen space
// reflections inside the lighting shaders (screenSpaceReflUtil.glsl, a linear
// march, several rays a pixel every frame). Linden Lab's feature branch
// project/graphics-care-package moves the opaque surfaces' reflections into
// passes of their own before the lighting; the viewer (plus/ssr) ports them.
// Each frame, between the G-buffer and the lighting, on the main view only:
//   1. SsrHiZ: from the scene copy's depth (LLPipeline::mSceneMap: the last
//      frame, lit, copied before tone mapping), a pyramid of the nearest
//      depth (levels 1.. at half, quarter... size; level 0 is that depth).
//      The backend keeps it (no GL texture); a pass with no depth releases it.
//   2. SsrTrace: into the bound target (the SSR buffer, RGBA16F, at the
//      G-buffer's size or smaller), per pixel of a glossy surface: rays along
//      the reflection (GGX-jittered by the roughness) traced through the
//      pyramid in the scene copy's screen; where one hits, the scene copy's
//      colour. Stored as GCP does: rgb the colour compressed by
//      c / (1 + luma) times the hit's weight, a the weight (0..1).
//   3. SsrAvatarMask (with temporal accumulation): the bound target (R8, the
//      G-buffer's size, sharing its depth) gets 1 where the depth's stencil
//      carries LLRHI::STENCIL_AVATAR (llrhiskinsss.h), 0 elsewhere.
//   4. SsrResolve (temporal accumulation): into the bound target, this
//      frame's trace (one ray a pixel) and the last frame's result moved with
//      the camera, clipped to this frame's 3x3 neighbourhood. None of the
//      history where the scene copy's depth shows the surface was hidden a
//      frame ago, less of it on avatars (the camera's motion is not theirs).
//      With the trace's hit distance (research/294 S1, PlusSSRReflectionMotion)
//      the history is taken where the reflection was a frame ago (its mirror
//      image moved with the camera) rather than the surface, clipped to the
//      5x5's spread, and the 3x3's mean stands in for history that was lost
//      where the pixel's one ray is noisy (from where the ray's jitter,
//      about twice the GGX alpha, roughness squared, passes a pixel's
//      angle: not on near mirrors).
//   5. SsrFilter: into the bound target (what the lighting reads), the trace
//      or the resolve blurred by each pixel's roughness: a pyramid of the source (the
//      backend's, dual-filter downsampling), read at the level the roughness
//      asks for, and faded out towards the trace's roughness limit.
// Matrices are GL's (column-major, eye space looking down -z). Stored depth
// values go to NDC z as the backend's depth range does (LLVKFullscreen).
namespace LLRHI
{
    // SsrResolve::clip_sigma: the default (the resolve's before research/294) and the range it is kept in.
    constexpr F32 SSR_CLIP_SIGMA_DEFAULT = 1.25f;
    constexpr F32 SSR_CLIP_SIGMA_MIN = 0.5f;
    constexpr F32 SSR_CLIP_SIGMA_MAX = 4.f;

    // The camera and the G-buffer, which the trace, the resolve and the filter read.
    struct SsrFrame
    {
        F32 projection[16] = {};                // this frame: eye -> clip
        F32 inverse_projection[16] = {};        // this frame: clip -> eye
        F32 prev_projection[16] = {};           // the scene copy's frame: eye -> clip
        F32 inverse_prev_projection[16] = {};   // and back
        F32 inverse_modelview_delta[16] = {};   // this frame's eye space -> the scene copy's
        U32 depth_texture = 0;                  // GL name of this frame's G-buffer depth
        U32 specular_texture = 0;               // G-buffer 1: PBR occlusion, roughness, metallic; legacy specular and gloss
        U32 normal_texture = 0;                 // G-buffer 2: encoded normal, environment intensity, the G-buffer flag
        U32 width = 0;                          // the G-buffer's size
        U32 height = 0;
    };

    struct SsrHiZ
    {
        U32 scene_depth_texture = 0;    // the scene copy's depth; 0: release the pyramids (SSR off)
        U32 width = 0;                  // its size
        U32 height = 0;
    };

    struct SsrTrace
    {
        SsrFrame frame;
        U32 scene_color_texture = 0;    // the scene copy (the last frame, lit, HDR)
        U32 scene_depth_texture = 0;    // its depth: the pyramid's level 0 (SsrHiZ's, this frame)
        U32 scene_width = 0;            // their size
        U32 scene_height = 0;
        U32 width = 0;                  // the bound target's size
        U32 height = 0;
        U32 iterations = 0;             // pyramid steps a ray takes at most
        U32 rays = 1;                   // rays a pixel
        F32 noise = 0.f;                // the noise's phase: the frame with temporal accumulation, else 0
        F32 max_thickness = 0.f;        // m: how deep a surface is taken to be behind its depth
        F32 depth_bias = 0.f;           // the ray's start pulled back along the normal, times the view depth
        F32 max_depth = 0.f;            // m: no reflections traced from or hit beyond this view depth
        F32 max_roughness = 0.f;        // no reflections traced on rougher surfaces
        // Your avatar's reflection in the floor (plus/avatarrefl) takes your
        // avatar on its plane: there the rays' hits on your avatar (in its
        // box) are dropped, by how much the reflection takes the pixel, so
        // you are not shown twice (research/210 A2). self_params.x 0: off.
        F32 self_box[16] = {};          // the scene copy's eye space -> your avatar's box (inside: -1..1 on each axis)
        F32 self_plane[4] = {};         // the plane in this frame's eye space: xyz normal, w distance
        F32 self_params[4] = {};        // x the reflection's weight (0: off), y the plane's thickness (m), z the least normal match
        // The bound target has a second attachment (R16F): per pixel, the hits'
        // mean distance (m) from the surface along the ray, weighted as the
        // colour is (0 where nothing was hit), for SsrResolve's reflection
        // motion (research/294 S1). False: one attachment, as before.
        bool hit_distance = false;
    };

    struct SsrAvatarMask
    {
        U32 width = 0;                  // the bound target's size (the G-buffer's)
        U32 height = 0;
    };

    struct SsrResolve
    {
        SsrFrame frame;
        U32 trace_texture = 0;          // this frame's SsrTrace (the bound target's size)
        U32 history_texture = 0;        // the last frame's SsrResolve (the same size)
        U32 scene_depth_texture = 0;    // the scene copy's depth (what the last frame saw)
        U32 scene_width = 0;
        U32 scene_height = 0;
        U32 avatar_mask_texture = 0;    // SsrAvatarMask's target (the G-buffer's size), or 0: no avatar mark this frame
        U32 width = 0;                  // the bound target's size
        U32 height = 0;
        F32 history_blend = 0.f;        // the history's weight (0: none, the first frame or after a jump)
        F32 avatar_history = 1.f;       // on avatars, the history's weight times this
        F32 disocclusion = 0.05f;       // relative depth difference from which the history is dropped (all of it at twice this)
        // This frame's SsrTrace hit distance (its second attachment), or 0: the
        // history moves with the surface and is clipped to the 3x3's spread
        // times 1.25, as before research/294.
        U32 hit_distance_texture = 0;
        F32 clip_sigma = SSR_CLIP_SIGMA_DEFAULT;    // with hit_distance_texture: the clip box's half width, in the 5x5's standard deviations
    };

    struct SsrFilter
    {
        SsrFrame frame;
        U32 source_texture = 0;         // SsrResolve's target, or SsrTrace's (without temporal accumulation)
        U32 width = 0;                  // its size, the bound target's too
        U32 height = 0;
        F32 filter_scale = 0.f;         // the pyramid's taps, in source texels (0: no pyramid, sharp everywhere)
        F32 max_roughness = 0.f;        // SsrTrace's
    };

    // On unless LLRHI_SSR=0 (a switch for tests: the passes are then never
    // recorded and the official tracing stays). The viewer's settings
    // (RenderScreenSpaceReflections, off by default and on from the High
    // quality level up, and PlusSSR, on by default) decide whether they run.
    extern bool sSsr;

    // Set by the Vulkan backend when it sets its passes up (on their first
    // use): whether it can trace the hits' distance and resolve with the
    // reflection's motion (SsrTrace::hit_distance, SsrResolve::hit_distance_texture;
    // research/294). False until then and when those passes could not be made:
    // the passes run as before. Main thread.
    extern bool sSsrHitDistance;

    // Set by the Vulkan backend. Each records its pass (SsrHiZ: its passes,
    // into its own images; SsrFilter: its pyramid, then into the bound draw
    // framebuffer; the others: into the bound draw framebuffer); false when
    // it could not (SsrTrace and SsrFilter then draw nothing the lighting
    // could read).
    extern bool (*sSsrHiZ)(const SsrHiZ& pass);
    extern bool (*sSsrTrace)(const SsrTrace& pass);
    extern bool (*sSsrAvatarMask)(const SsrAvatarMask& pass);
    extern bool (*sSsrResolve)(const SsrResolve& pass);
    extern bool (*sSsrFilter)(const SsrFilter& pass);
}

#endif // LL_LLRHISSR_H
