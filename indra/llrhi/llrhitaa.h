/**
 * @file llrhitaa.h
 * @brief Temporal anti-aliasing (research/185): what the viewer hands the
 *        backend to draw one frame of it.
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

#ifndef LL_LLRHITAA_H
#define LL_LLRHITAA_H

#include "stdtypes.h"

// research/185: the camera's motion, and the projection's sub-pixel jitter.
// The viewer (plus/taa) draws this one fullscreen pass in
// LLPipeline::renderFinalize() after the FXAA/SMAA pass, into the GL
// framebuffer it has bound (of the size of the picture). The picture is tone
// mapped (0..1), as it comes out of the anti-aliasing.
namespace LLRHI
{
    // The jitter's points (plus/taa): a still scene shows the same picture again after this many frames. The flicker's
    // memory (research/250) keeps one sign per point.
    constexpr U32 TAA_JITTER_ROUND = 8;

    // The pass writes two colour attachments: the result (RGBA16F; its alpha is the running mean the flicker's
    // signs are taken against) and the flicker's memory (R32F, an integer). The next frame reads both as the history.
    struct TaaPass
    {
        U32 source_texture = 0;     // GL name of this frame's picture
        U32 history_texture = 0;    // the last frame's result (0: none, the picture is written as it is)
        U32 history_memory_texture = 0;     // the last frame's flicker memory (with history_texture)
        U32 depth_texture = 0;      // GL name of the scene depth (the size of the picture)
        U32 width = 0;              // the size of the picture and of the target the pass is called for
        U32 height = 0;             // (the bound one must have it, or nothing is drawn)
        // Column-major (as GL and glm lay out a mat4): this frame's eye space
        // to the last frame's clip space (last projection * last view *
        // inverse of this view), and this frame's clip space to its eye space.
        // Both without the jitter.
        F32 reproject[16] = {};
        F32 inverse_projection[16] = {};
        F32 blend_still = 0.9f;     // the history's weight where nothing moved
        F32 blend_moving = 0.55f;   // and where the picture moved by MOTION_FULL_PIXELS or more (plus/taa sets it: max(still - drop, still x floor))
        // The scene was drawn with the projection jittered: each pixel of the
        // picture (and of the depth) shows what the unmoved camera sees at
        // the pixel's centre minus jitter_pixels. The pass then makes this
        // frame's colour at the centre from the 3x3 around it. False: the
        // pixel itself, as without the jitter.
        bool jittered = false;
        F32 jitter_pixels[2] = {};
        // The jitter's point of this frame in its round (0..TAA_JITTER_ROUND-1),
        // and whether it is the one after the history's (false: a frame
        // between was drawn without the pass or without the jitter, and the
        // flicker memory's signs belong to other points: it begins again).
        U32 jitter_point = 0;
        bool jitter_round_goes_on = false;
        // research/250: where the camera is still and the same surface is
        // nearest, a colour that comes round with the jitter (a gap or a bar
        // narrower than a pixel, a fine lattice, two faces in one plane: in
        // some frames the whole 3x3 shows one side only) and whose history
        // the clip has cut by much round after round keeps this much (0..1)
        // of its history instead of having it clipped, and the history's
        // weight there rises to at least flicker_blend.
        F32 flicker_keep = 0.f;
        F32 flicker_blend = 0.f;
    };

    // On unless LLRHI_TAA=0 (a switch for tests).
    extern bool sTaa;

    // Set by the Vulkan backend. Records the pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sTaaDraw)(const TaaPass& pass);
}

#endif // LL_LLRHITAA_H
