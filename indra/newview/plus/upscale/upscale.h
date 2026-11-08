/**
 * @file upscale.h
 * @brief Upscaling: the main view drawn smaller and brought to the window's
 *        size with AMD FidelityFX Super Resolution 1 (research/165 in the
 *        sl-viewer repository, stages 1 and 2).
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

#ifndef PLUS_UPSCALE_UPSCALE_H
#define PLUS_UPSCALE_UPSCALE_H

#include "stdtypes.h"

class LLRenderTarget;

// PlusUpscale (0 off, 1 FSR 1, 2 a bilinear stretch to compare), at
// PlusUpscaleScale of the window's size on each side, sharpened by RCAS at
// PlusUpscaleSharpness. Only the main view's screen targets (G-buffer to
// anti-aliasing) are made smaller: the shadow maps, the reflection probes and
// the mirrors keep their sizes, and the look, the final pass, the HUD, the
// name tags and the UI are drawn at the window's size. Snapshots and the
// photo mode (the photo screen, or the photo tools open and not minimized) are
// always at the full size. A change takes
// effect through LLPipeline's resize of the screen targets (one, not every
// frame). Vulkan only (LLRHI::sUpscaleDraw); LLRHI_UPSCALE=0 or the cue file
// upscale.toggle turns it off for tests.
namespace plus::upscale
{
    // In LLPipeline::allocateScreenBufferInternal(), in place of the official
    // RenderResolutionDivisor: the size the targets of a view of res_x x res_y
    // are made at. For the main view (main_view), while the upscale is wanted,
    // the smaller size (true returned, the divisor not used); otherwise the
    // official divisor's, as before (false).
    bool drawSize(U32 res_x, U32 res_y, bool main_view, U32 divisor, U32& out_x, U32& out_y);

    // Right after it for the main view: what is being made. The passes
    // (renderFinalize, the anti-aliasing) follow this, not the setting, until
    // the targets are made again.
    void noteMainTargets(bool upscaled, U32 drawn_x, U32 drawn_y);

    // LLPipeline::resizeScreenTexture(): true when targets drawn at
    // drawn_x x drawn_y are the ones wanted now for a window of res_x x res_y.
    bool drawnSizeMatches(U32 res_x, U32 res_y, U32 divisor, U32 drawn_x, U32 drawn_y);

    // The main view's targets are made smaller (the upscale draws this frame).
    bool active();

    // The anti-aliasing to use for RenderFSAAType (fsaa_type): at least FXAA
    // (1) while active, as FSR 1 wants an anti-aliased picture; the setting
    // itself is not changed (research/165 review 3).
    U32 effectiveAAType(U32 fsaa_type);

    // The official CAS is left out of the tone mapping: FSR 1's RCAS sharpens
    // instead (active with FSR 1, not the bilinear stretch).
    bool replacesCAS();

    // The <plus> hook in LLPipeline::renderFinalize(), after the
    // anti-aliasing and before the look: while active, draws source at the
    // window's size (EASU, then RCAS); source then names that picture and
    // target a spare of its size for the passes after it. Nothing changes
    // when inactive (returns at once) or when the backend could not draw (the
    // final pass then stretches the picture, as the official divisor does).
    void render(LLRenderTarget*& source, LLRenderTarget*& target);

    // LLPipeline::releaseScreenBuffers(): the window-sized targets go with the screen's.
    void releaseTargets();
}

#endif // PLUS_UPSCALE_UPSCALE_H
