/**
 * @file motion.h
 * @brief The main view's motion vectors (research/373 section 2): the G-buffer's
 *        fifth attachment, the camera's motion filled in after the G-buffer,
 *        and a view of them (LLRHI_MOTION_VIEW).
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

#ifndef PLUS_TEMPORAL_MOTION_H
#define PLUS_TEMPORAL_MOTION_H

#include "stdtypes.h"

#include <map>
#include <string>

class LLRenderTarget;

// What the vectors hold and who writes them: llrhimotion.h. The G-buffer's
// programs write them with the define HAS_MOTION (llviewershadermgr.cpp). The
// main view's G-buffer has the attachment only under programs made so (a target
// with an attachment its programs leave unwritten is a validation warning); a
// G-buffer without it under such programs (a probe's face, a mirror, the avatar
// reflections, the impostors, a snapshot's, the main one after a failure) is
// bound with an empty fifth slot (LLRenderTarget::sPaddedDrawBuffers), so that
// its programs' fifth output has a place to be dropped. The hooks are in
// llviewerdisplay.cpp (the clear) and pipeline.cpp (<plus> tu-motion-v1): the
// screen targets' allocation and release, renderDeferredLighting (the camera's
// pass, after the wet look) and renderFinalize (the view). Off (nothing wants
// them): the programs as before, no attachment, no padding, and each of the
// hooks is one test of a flag.
namespace plus::temporal
{
    // Something wants the vectors: a temporal upscaler is chosen (none can be
    // yet: plus::upscale::temporalSelected) or LLRHI_MOTION_VIEW is set, the
    // backend draws them (not with LLRHI_TEMPORAL=0, nor on a device that binds
    // fewer than five colour attachments), and the G-buffer has the emissive
    // attachment before them (RenderEnableEmissiveBuffer).
    bool motionWanted();

    // LLViewerShaderMgr::loadBasicShaders, the global defines: HAS_MOTION when
    // motionWanted(). Noted as what the programs drawn with have (not in a
    // background reload's plan pass). Turned on or off later, the shaders are
    // made again (asked for at idle) and the screen targets with them.
    void addShaderDefines(std::map<std::string, std::string>& attribs);

    // allocateScreenBufferInternal, the main view's G-buffer just made with its
    // usual attachments: the motion attachment added when the programs have
    // HAS_MOTION, not while a snapshot is drawn (asked for again after it when
    // the window's G-buffer had it; not after a failure). A failure (VRAM)
    // leaves the G-buffer as it is (bound with the empty fifth slot) and the
    // vectors off until the screen targets are made again (not asked for again
    // by this).
    void addMotionAttachment(LLRenderTarget& gbuffer);

    // display(), right after the main view's G-buffer was cleared (bound): its
    // motion attachment to 0 (the official clear's magenta would leave w 1).
    // renderMotion draws only on a frame whose clear went through.
    void clearMotion();

    // releaseScreenBuffers: the attachment goes with the G-buffer, and so does
    // the last frame's camera.
    void releaseMotion();

    // renderDeferredLighting, the G-buffer drawn: the camera's motion where
    // nothing wrote its own (the main view only; not in a snapshot).
    void renderMotion();

    // renderFinalize, the finished picture just presented into the bound target
    // (the HUD and the UI come after): the vectors laid over it while
    // LLRHI_MOTION_VIEW is set and they were made this frame. Not over the
    // post-processing's own targets: one may be a time-based pass's history
    // (plus/taa's), which would then mix the view into the next frames.
    void viewMotion();

    // The G-buffer's motion attachment (GL name) when this frame's camera pass
    // drew it, 0 otherwise (for the temporal upscalers, tu-fsr3).
    U32 motionTexture();
}

#endif // PLUS_TEMPORAL_MOTION_H
