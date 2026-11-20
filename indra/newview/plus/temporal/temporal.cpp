/**
 * @file temporal.cpp
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "temporal.h"
#include "plus/taa/taa.h"
#include "plus/upscale/upscale.h"

#include "llappviewer.h"
#include "presets/presets.h"
#include "llgraywire.h"
#include "llrender.h"
#include "llrhitaa.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    constexpr char SETTING_JITTER[] = "PlusTAAJitter";
    constexpr char SETTING_JITTER_SCALE[] = "PlusTAAJitterScale";

    // Frames the viewer did not draw in between (idle skipping) are allowed up to this many: the picture is
    // brought over by the two cameras however many frames lie between. More is a login or a minimized window.
    constexpr U32 MAX_FRAME_GAP = 8;
    // A camera that moved further than this between two drawn frames jumped (a teleport,
    // the agent frame moving to another region): no history. As plus/ssr.
    constexpr F32 JUMP_METRES = 10.f;

    // A round of the jitter's points is never longer than this.
    constexpr U32 MAX_JITTER_PERIOD = 32;

    struct JitterState
    {
        // The jitter (JitterScope): the frame whose scene was drawn moved
        // (U32_MAX: none yet), by how much, and the projection unmoved.
        U32 index = 0;                  // the last Halton index used (1..period)
        U32 period = LLRHI::TAA_JITTER_ROUND;   // the round's length of that frame
        U32 frame = U32_MAX;
        glm::vec2 pixels = glm::vec2(0.f);   // where the samples lie from the pixel centres
        glm::mat4 unmoved = glm::mat4(1.f);
    };

    // Never destroyed (as plus/taa's state).
    JitterState& jitter()
    {
        static JitterState* state = new JitterState();
        return *state;
    }

    // The pass that will mix this frame's main view with a history, and what it asks of the jitter.
    struct Consumer
    {
        U32 width = 0;      // the picture's pixels the jitter is counted in
        U32 height = 0;
        U32 period = LLRHI::TAA_JITTER_ROUND;
    };

    // Whether a time-based pass drew the main view a moment ago (so that it will mix this frame's picture with a history).
    bool consumerAhead(Consumer& out)
    {
        // Only the plain main view: not a snapshot, a probe's face or a mirror.
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT || plus::presets::snapshotInProgress())
        {
            return false;
        }
        if (plus::taa::historyAhead(out.width, out.height))
        {
            out.period = LLRHI::TAA_JITTER_ROUND;       // plus/taa's flicker memory (research/250) keeps one sign per point
            return true;
        }
        F32 ratio = 1.f;
        if (plus::upscale::temporalAhead(out.width, out.height, ratio))
        {
            out.period = plus::temporal::jitterPeriodForRatio(ratio);
            return true;
        }
        return false;
    }

    // The jitter's size, 0..1 of a pixel either way of the centre's half (0: none).
    F32 jitterScale()
    {
        static LLCachedControl<bool> jitter(gSavedSettings, SETTING_JITTER, true);
        static LLCachedControl<F32> scale(gSavedSettings, SETTING_JITTER_SCALE, 1.f);
        if (!jitter)
        {
            return 0.f;
        }
        const F32 value = scale;
        return value > 0.f ? llmin(value, 1.f) : 0.f;      // also a NaN: none
    }

    // The jitter's size for this frame's main view (0: not moved): a time-based pass is on and drew a
    // moment ago (it takes the jitter out again; a frame it does not draw would show it),
    // the jitter is wanted, and this is the plain main view (not a snapshot or its tiles). Nor while a texture not
    // loaded yet is drawn as a wireframe (GrayWire, select-flicker): lines thinner than a pixel, moved by under one,
    // came and went with the jitter; the pass mixes the unmoved picture meanwhile. research/289: wireframes covering
    // enough of the view only (near ones; PlusWireframeTaaNearOnly off: any, as before).
    F32 jitterWanted(Consumer& consumer)
    {
        const F32 scale = jitterScale();
        if (scale <= 0.f || !consumerAhead(consumer) || LLViewerCamera::getInstance()->getZoomFactor() != 1.f || GrayWire::drawnLately())
        {
            return 0.f;
        }
        return scale;
    }

    // The Halton sequence's index-th number in base (0..1).
    F32 halton(U32 index, U32 base)
    {
        F32 result = 0.f;
        F32 fraction = 1.f;
        while (index > 0)
        {
            fraction /= (F32)base;
            result += fraction * (F32)(index % base);
            index /= base;
        }
        return result;
    }

    // The clip space moved by ndc (x, y): x' = x + ndc.x * w, so the picture moves by ndc.
    glm::mat4 clipShift(const glm::vec2& ndc)
    {
        glm::mat4 shift(1.f);
        shift[3][0] = ndc.x;
        shift[3][1] = ndc.y;
        return shift;
    }

    // Loads a projection: gGL's (what the draws' shaders get) and get_current_projection (what the passes read).
    void loadProjection(const glm::mat4& current, const glm::mat4& loaded)
    {
        const LLRender::eMatrixMode mode = gGL.getMatrixMode();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.loadMatrix(glm::value_ptr(loaded));
        gGL.matrixMode(mode);
        set_current_projection(current);
    }

    // The camera's place in the world of a view matrix.
    glm::vec3 eyePosition(const glm::mat4& view)
    {
        return glm::vec3(glm::inverse(view)[3]);
    }
}

namespace plus::temporal
{
    bool FrameHistory::withinGap() const
    {
        return mHas && gFrameCount > mFrame && gFrameCount - mFrame <= MAX_FRAME_GAP;
    }

    bool FrameHistory::ahead() const
    {
        return withinGap() && mWidth && mHeight;
    }

    bool FrameHistory::usable(U32 width, U32 height, const glm::mat4& view) const
    {
        if (!withinGap() || mWidth != width || mHeight != height)
        {
            return false;       // a long gap is the login, a minimized window, or the setting having been off
        }
        const glm::vec3 moved = eyePosition(view) - eyePosition(mView);
        return std::isfinite(moved.x) && std::isfinite(moved.y) && std::isfinite(moved.z) && glm::length(moved) <= JUMP_METRES;
    }

    void FrameHistory::commit(U32 width, U32 height, const glm::mat4& view, const glm::mat4& projection)
    {
        mHas = true;
        mFrame = gFrameCount;
        mWidth = width;
        mHeight = height;
        mView = view;
        mProjection = projection;
    }

    glm::mat4 FrameHistory::reprojectFrom(const glm::mat4& view) const
    {
        return glm::mat4(glm::dmat4(mProjection) * glm::dmat4(mView) * glm::inverse(glm::dmat4(view)));
    }

    U32 jitterPeriodForRatio(F32 ratio)
    {
        const U32 base = LLRHI::TAA_JITTER_ROUND;
        if (!(ratio > 1.f))     // also a NaN
        {
            return base;
        }
        const F32 points = (F32)base * ratio * ratio + 0.5f;     // rounded
        return points >= (F32)MAX_JITTER_PERIOD ? MAX_JITTER_PERIOD : llmax(base, (U32)points);
    }

    F32 jitterMargin()
    {
        Consumer consumer;
        return 0.5f * jitterWanted(consumer);
    }

    bool jitteredThisFrame()
    {
        return jitter().frame == gFrameCount;
    }

    glm::vec2 depthJitter()
    {
        const JitterState& s = jitter();
        return s.frame == gFrameCount ? s.pixels : glm::vec2(0.f);
    }

    U32 jitterIndex()
    {
        return jitter().index;
    }

    U32 jitterPeriod()
    {
        return jitter().period;
    }

    const glm::mat4& unmovedProjection()
    {
        return jitter().unmoved;
    }

    JitterScope::JitterScope(bool for_snapshot)
    {
        JitterState& s = jitter();
        Consumer consumer;
        const F32 scale = for_snapshot ? 0.f : jitterWanted(consumer);
        if (scale <= 0.f)
        {
            // A scene drawn unmoved after a moved one in the same frame (a snapshot's): what reads its depth or
            // glow is told it is not jittered (select-flicker).
            s.frame = U32_MAX;
            return;
        }
        s.period = consumer.period;
        s.index = s.index % s.period + 1;
        const glm::vec2 pixels((halton(s.index, 2) - 0.5f) * scale, (halton(s.index, 3) - 0.5f) * scale);
        // The picture's pixels (the size the pass drew it at) in NDC, which spans 2.
        const glm::mat4 shift = clipShift(glm::vec2(2.f * pixels.x / (F32)consumer.width, 2.f * pixels.y / (F32)consumer.height));
        mCurrent = get_current_projection();
        mLoaded = gGL.getProjectionMatrix();
        loadProjection(shift * mCurrent, shift * mLoaded);
        mApplied = true;
        s.frame = gFrameCount;
        s.pixels = pixels;
        s.unmoved = mCurrent;
    }

    void JitterScope::restore()
    {
        if (!mApplied)
        {
            return;
        }
        mApplied = false;
        loadProjection(mCurrent, mLoaded);
    }
}
