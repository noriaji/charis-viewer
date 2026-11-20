/**
 * @file llrhimotion.h
 * @brief The motion vectors of the main view's G-buffer (research/373 section 2):
 *        what the viewer hands the backend to fill in the camera's motion, and
 *        to show the vectors.
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

#ifndef LL_LLRHIMOTION_H
#define LL_LLRHIMOTION_H

#include "stdtypes.h"

// While a temporal pass wants them (plus/temporal: a temporal upscaler, PlusUpscale 3 and 4,
// or the view below), the main view's G-buffer (mRT->deferredScreen of mMainRT) has a fifth
// colour attachment, MOTION_ATTACHMENT: RGBA16F at the size the view is drawn at (none when it
// could not be made, nor on a G-buffer made while a snapshot is drawn; a snapshot the window
// holds keeps the window's, which is cleared as always, and only the camera's pass stops).
//   xy: where the surface seen at the pixel was in the last drawn frame, less where it is in
//       this one (UV units: the picture spans 0..1, y up as GL's; FSR's top-left origin wants
//       the y scale negative, tu-fsr3), both without the jitter;
//   z:  the composition mark, 0..1: how much a temporal pass should take this frame's colour
//       over its history there (tu-reactive writes it; 0 for now);
//   w:  1 where the thing drawn wrote its own motion into xy (tu-motion-skin, tu-motion-rigid),
//       0 where it did not (everything for now).
// It is cleared to (0, 0, 0, 0) right after the G-buffer's clear (plus::temporal::clearMotion).
// The G-buffer's programs write it while they have the define HAS_MOTION (frag_data[4]):
// (0, 0, 0, 0) for a thing that does not move by itself. A blended G-buffer draw (the stars, the
// moon, the old clouds, a highlight) leaves it as it was (its output's alpha is 0). While the
// programs have HAS_MOTION, every target made as a G-buffer (addDeferredAttachments) is bound
// with draw buffers up to the fifth (those it lacks GL_NONE, LLRenderTarget::sPaddedDrawBuffers):
// a probe's face, a mirror, the avatar reflections, the impostors, a snapshot's G-buffer, the main
// one when its attachment could not be made. Their fifth output is dropped.
//
// After the G-buffer is drawn (and the wet look wrote back into it), before the lighting, the
// viewer binds the G-buffer and asks for the camera's pass: where w is 0, xy becomes the
// camera's motion (from the depth: this frame's eye space brought to the last frame's clip
// space); at the far depth (MOTION_FAR_NDC_Z) it is the camera's turning only. z and w are left
// as they are.
namespace LLRHI
{
    // The attachment's index in the G-buffer (after the diffuse, specular, normal and emissive
    // ones: the G-buffer has it only with the emissive one, RenderEnableEmissiveBuffer).
    constexpr U32 MOTION_ATTACHMENT = 4;

    // From this NDC z on a depth is the far one (a direction, which moves with the camera's
    // turning only): the sky dome, the sun, the moon and the stars are drawn squashed to the far
    // clip at 0.99999 (LLGLSquashToFarClip), and the clear leaves 1. A surface this deep is near
    // the far clip (with a near clip of 0.1 m: the last 6 m of a draw distance of 256 m, the last
    // 95 m of 1024 m), so far off that the camera's moving adds no parallax that shows.
    constexpr F32 MOTION_FAR_NDC_Z = 0.99998f;

    // The camera's pass, on the bound G-buffer (it must have MOTION_ATTACHMENT + 1 colour
    // attachments of width x height, or nothing is drawn). Matrices column-major (as GL and glm
    // lay out a mat4).
    struct MotionCameraPass
    {
        U32 depth_texture = 0;          // GL name of the G-buffer's depth
        U32 width = 0;                  // the G-buffer's size
        U32 height = 0;
        // The clip space of the projection the G-buffer was drawn with (jittered) to its eye space.
        F32 inverse_projection[16] = {};
        // The eye space to this frame's clip space without the jitter.
        F32 projection[16] = {};
        // The eye space to the last frame's clip space without the jitter (last projection * last
        // view * inverse of this view). Read only with has_history.
        F32 reproject[16] = {};
        // False: no last frame to compare with (the first frame, a jump, a gap, another size): the
        // camera's motion is 0.
        bool has_history = false;
    };

    // LLRHI_MOTION_VIEW: the vectors drawn over the finished picture, to look at them.
    enum MotionView : U32
    {
        MOTION_VIEW_OFF = 0,
        MOTION_VIEW_VECTORS = 1,    // xy as colour: grey where nothing moves, red to the right, green up
        MOTION_VIEW_MARKS = 2,      // w in red (an own motion), z in blue (the composition mark)
        MOTION_VIEW_COUNT
    };

    // The view, on the bound target (the finished picture, of any size).
    struct MotionViewPass
    {
        U32 motion_texture = 0;     // GL name of the G-buffer's MOTION_ATTACHMENT
        U32 motion_width = 0;       // its size (the drawn size)
        U32 motion_height = 0;
        U32 mode = MOTION_VIEW_OFF;
    };

    // Motion vectors drawn ahead of the lighting while a temporal pass wants them.
    // On unless LLRHI_TEMPORAL=0 (a switch for tests; the safe mode sets it).
    extern bool sMotion;
    // LLRHI_MOTION_VIEW (MotionView, read once): the view's mode; anything else is off.
    // Not 0 also asks for the vectors (the G-buffer gets the attachment).
    extern U32 sMotionViewMode;

    // Set by the Vulkan backend (not on a device with fewer than five colour attachments
    // (MOTION_ATTACHMENT + 1) or fragment outputs, or without independentBlend). Record into
    // the bound draw framebuffer; false when nothing was recorded.
    // The bound G-buffer's MOTION_ATTACHMENT cleared to 0 (the G-buffer of width x height with
    // its fifth attachment, or nothing is cleared).
    extern bool (*sMotionClear)(U32 width, U32 height);
    extern bool (*sMotionCamera)(const MotionCameraPass& pass);
    extern bool (*sMotionViewDraw)(const MotionViewPass& pass);
}

#endif // LL_LLRHIMOTION_H
