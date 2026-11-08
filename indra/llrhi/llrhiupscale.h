/**
 * @file llrhiupscale.h
 * @brief Upscaling (research/165): what the viewer hands the backend to draw
 *        the main view's smaller picture at the window's size.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLRHIUPSCALE_H
#define LL_LLRHIUPSCALE_H

#include "stdtypes.h"

// The viewer (plus/upscale) draws the main view smaller (G-buffer to
// anti-aliasing) and, in LLPipeline::renderFinalize() after the
// anti-aliasing, has the backend draw it at the window's size in one or two
// fullscreen passes into the GL framebuffer it has bound. The picture is
// tone mapped and gamma corrected (0..1), as AMD FidelityFX Super Resolution 1
// wants it.
namespace LLRHI
{
    struct UpscalePass
    {
        enum Kind : U32
        {
            EASU = 0,       // FSR 1 edge adaptive spatial upsampling: source (smaller) to the target's size
            BILINEAR = 1,   // a plain bilinear stretch, as the official RenderResolutionDivisor (to compare)
            RCAS = 2,       // FSR 1 robust contrast adaptive sharpening: source and target of the same size
        };
        U32 kind = EASU;
        U32 source_texture = 0;     // GL name of the picture to read
        U32 source_width = 0;       // its size
        U32 source_height = 0;
        U32 width = 0;              // the size of the target the pass is called for
        U32 height = 0;             // (the bound one must have it, or nothing is drawn)
        F32 sharpness = 0.f;        // RCAS: 0 (none) .. 1 (the strongest FSR 1 makes)
    };

    // On unless LLRHI_UPSCALE=0 (a switch for tests; the cue file
    // upscale.toggle flips it while the viewer runs). Off: the main view is
    // drawn at the window's size whatever PlusUpscale says.
    extern bool sUpscale;

    // Set by the Vulkan backend. Records the pass into the bound draw
    // framebuffer; false when it could not (nothing recorded).
    extern bool (*sUpscaleDraw)(const UpscalePass& pass);
}

#endif // LL_LLRHIUPSCALE_H
