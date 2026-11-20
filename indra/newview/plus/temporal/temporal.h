/**
 * @file temporal.h
 * @brief What the time-based passes share (research/373): the projection's jitter,
 *        the last frame's camera and when a history is dropped.
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

#ifndef PLUS_TEMPORAL_TEMPORAL_H
#define PLUS_TEMPORAL_TEMPORAL_H

#include "stdtypes.h"

#include <glm/glm.hpp>

// What plus/taa now, and the temporal upscalers (PlusUpscale 3 and 4, research/373)
// later, have in common. Moved out of plus/taa without changing what it does.
//
// - The jitter (research/185 section 13): while a time-based pass drew the last
//   frame, the main view's scene is drawn with the projection moved by a fraction
//   of a pixel (Halton 2,3), a different one each frame, round a period of points
//   the pass names (TAA: LLRHI::TAA_JITTER_ROUND; a temporal upscaler: longer with
//   the scale). The pass takes the jitter out again.
// - FrameHistory: the last drawn frame's camera (view and unmoved projection) and
//   when it may no longer be used (a gap in frames, another size, a jump).
namespace plus::temporal
{
    // The last frame a pass drew (its size and the camera that drew it, the
    // projection without the jitter). A pass keeps one for each history of its own.
    class FrameHistory
    {
    public:
        // A frame is kept, and it is not older than a few frames (idle skipping is
        // allowed; more is a login, a minimized window, or the pass having been off).
        bool withinGap() const;
        // withinGap(), and it was kept at some size: what the next frame's scene will be mixed with.
        bool ahead() const;
        // withinGap() at this size, with the camera not further from the kept one than a
        // jump (a teleport, the agent frame moving to another region) allows.
        bool usable(U32 width, U32 height, const glm::mat4& view) const;
        // This frame was drawn (gFrameCount) at this size with this camera.
        void commit(U32 width, U32 height, const glm::mat4& view, const glm::mat4& projection);
        // Nothing is kept (the size and the camera stay, as they were).
        void drop() { mHas = false; }
        // The eye space of a frame drawn with view to the kept frame's clip space (its projection without the
        // jitter): what a pass brings a point to the last frame with. Made in double, back to float once
        // (research/246: the views hold the agent frame's offset, up to 4096 m high).
        glm::mat4 reprojectFrom(const glm::mat4& view) const;

        bool has() const { return mHas; }
        U32 width() const { return mWidth; }
        U32 height() const { return mHeight; }
        const glm::mat4& view() const { return mView; }
        const glm::mat4& projection() const { return mProjection; }

    private:
        bool mHas = false;
        U32 mFrame = 0;                 // gFrameCount of it
        U32 mWidth = 0;
        U32 mHeight = 0;
        glm::mat4 mView = glm::mat4(1.f);
        glm::mat4 mProjection = glm::mat4(1.f);   // without the jitter
    };

    // The points of a jitter's round for a picture drawn at 1/ratio of its output
    // size on each side (ratio >= 1): 8 at 1, longer as the picture gets smaller
    // (about FSR's 8 x ratio^2, rounded; FSR's own count may round down), at most 32.
    // ratio comes from the real output/drawn sizes; what is handed to a backend as
    // its phase count is always jitterPeriod(), never a count made again from the scale.
    U32 jitterPeriodForRatio(F32 ratio);

    // Pixels the main view's scene may be moved by this frame (the jitter's
    // largest, 0 without it): the zero coverage cull counts the pixel centres that much further out.
    F32 jitterMargin();

    // The main view's scene of this frame was drawn jittered (JitterScope).
    bool jitteredThisFrame();

    // The pixels this frame's main-view scene (and its depth) was moved by (0, 0 when not jittered).
    glm::vec2 depthJitter();

    // Of this frame's jittered scene (jitteredThisFrame()): the point in its round (1..jitterPeriod()),
    // the round's length, and the projection before it was moved.
    U32 jitterIndex();
    U32 jitterPeriod();
    const glm::mat4& unmovedProjection();

    // The <plus> hook in display() (llviewerdisplay.cpp) around the main view's
    // G-buffer, lighting and alpha: moves the projection (the loaded one and
    // get_current_projection) by this frame's jitter when it is wanted, and puts
    // both back at restore() or at the end of the scope.
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

#endif // PLUS_TEMPORAL_TEMPORAL_H
