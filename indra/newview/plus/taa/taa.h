/**
 * @file taa.h
 * @brief Temporal anti-aliasing (research/185 in the sl-viewer repository):
 *        the camera's motion, and a sub-pixel jitter of the projection.
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

#ifndef PLUS_TAA_TAA_H
#define PLUS_TAA_TAA_H

#include "stdtypes.h"

#include <glm/glm.hpp>

class LLRenderTarget;

// PlusTAA (off by default), the history's weight PlusTAABlend where the
// picture is still (less where it moves). The last frame's result is kept
// (two targets of the picture's size, taken turns: the picture and a memory
// of its flicker) and brought to this frame
// from the scene depth and the two frames' cameras; the colours around the
// pixel limit what of it is kept. Only the main view's picture is touched:
// snapshots, the probes' and the mirrors' views are left alone, and the
// history is dropped at a snapshot, a teleport (the camera jumping), a change
// of the picture's size or of the setting. Vulkan only (LLRHI::sTaaDraw);
// LLRHI_TAA=0 turns it off for tests.
//
// It follows the official FXAA/SMAA (research/185 section 13): they smooth
// the edges within the frame, TAA then evens them out over the frames.
//
// Jitter (research/185 section 13, PlusTAAJitter, on by default but only
// while TAA draws; PlusTAAJitterScale): while TAA drew the last frame, the
// main view's scene (its G-buffer, lighting and alpha: JitterScope) is drawn
// with the projection moved by a fraction of a pixel, a different one each
// frame (Halton 2,3, 8 points), so that the history gathers samples from
// across each pixel. TAA takes the jitter out again (its reprojection and the
// history it keeps are of the unmoved projection). The cull, the occlusion's
// and the shadows' cameras, the HUD and the UI are not moved.
//
// Flicker that comes round with the jitter (research/246, 250,
// PlusTAAThinGaps): a thing finer than a pixel (a gap or a bar narrower than
// one, a lattice seen at a slant, two faces in one plane) shows the whole 3x3
// one side of it in some of the jitter's frames and the other in others; the
// colours around the pixel then hold only this frame's side, and clipping the
// history to them each frame showed the jittered picture as it is (a flicker
// the picture without TAA does not have). Where the camera is still, the same
// surface stays nearest, the pixel's colour has been coming round with the
// jitter's round for a while and the clip has thrown much away round after
// round, its history is kept instead (the pass's second target remembers
// that; a fine pattern that never flickered is left alone). The pixel moves as
// the nearest surface of the 3x3 does.
namespace plus::taa
{
    // The <plus> hook in LLPipeline::renderFinalize(), after the FXAA/SMAA
    // pass: draws source into one of its own targets. True when it did
    // (source then names that picture and target the picture that was
    // source, a spare of its size). False: nothing changed. Off: returns at once.
    bool render(LLRenderTarget*& source, LLRenderTarget*& target);

    // With the screen's targets (LLPipeline::releaseScreenBuffers).
    void releaseTargets();

    // Pixels the main view's scene may be moved by this frame (the jitter's
    // largest, 0 without it): the zero coverage cull (LLPipeline::
    // rhiCoverageMainBegin) counts the pixel centres that much further out.
    F32 jitterMargin();

    // True while the main view is being drawn and TAA will mix it with its
    // history (on, drew a moment ago; not a snapshot, a probe's face or a
    // mirror): noise that changes every frame (the volumetric clouds' march)
    // is evened out then. False elsewhere, where it would show as flicker.
    bool smoothsMainView();

    // The main view's scene of this frame was drawn jittered (JitterScope):
    // what is made from it at a lower size (the glow) must look at the whole
    // of each of its pixels' footprint, or a thin bright surface would be hit
    // in one frame and missed in the next (research/240).
    bool jitteredThisFrame();

    // The pixels this frame's main-view scene (and its depth) was moved by
    // (0, 0 when not jittered). LLPipeline::renderFinalize()'s last pass
    // writes the depth the HUD and UI test against (the selection's outlines
    // and wireframes, the manipulators, the beacons: drawn after TAA and not
    // moved) at the unmoved pixel centres with it; the depth as it is moved by
    // up to half a pixel's slope each frame, and a line lying on a surface was
    // in front of it in one frame and behind it in the next (select-flicker).
    glm::vec2 depthJitter();

    // The <plus> hook in display() (llviewerdisplay.cpp) around the main
    // view's G-buffer, lighting and alpha: moves the projection (the loaded
    // one and get_current_projection) by this frame's jitter when it is
    // wanted, and puts both back at restore() or at the end of the scope.
    // for_snapshot: display()'s, nothing is moved then.
    class JitterScope
    {
    public:
        explicit JitterScope(bool for_snapshot);
        ~JitterScope() { restore(); }
        JitterScope(const JitterScope&) = delete;
        JitterScope& operator=(const JitterScope&) = delete;

        void restore();

    private:
        bool mApplied = false;
        glm::mat4 mCurrent = glm::mat4(1.f);    // get_current_projection() before
        glm::mat4 mLoaded = glm::mat4(1.f);     // gGL's projection matrix before
    };
}

#endif // PLUS_TAA_TAA_H
