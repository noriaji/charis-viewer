/**
 * @file taa.cpp
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "taa.h"
#include "presets/presets.h"

#include "llappviewer.h"
#include "llgl.h"
#include "llgraywire.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
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
    constexpr char SETTING_ENABLED[] = "PlusTAA";
    constexpr char SETTING_BLEND[] = "PlusTAABlend";
    constexpr char SETTING_JITTER[] = "PlusTAAJitter";
    constexpr char SETTING_JITTER_SCALE[] = "PlusTAAJitterScale";
    constexpr char SETTING_THIN_GAPS[] = "PlusTAAThinGaps";

    // The jitter's points (Halton 2,3 from index 1): with the history's weight
    // near 0.9 (about ten frames) the picture holds a whole round of them. The
    // pass's flicker memory (research/250) keeps one sign per point.
    constexpr U32 JITTER_POINTS = LLRHI::TAA_JITTER_ROUND;

    constexpr F32 DEFAULT_BLEND = 0.9f;
    constexpr F32 MIN_BLEND = 0.5f;
    constexpr F32 MAX_BLEND = 0.98f;
    // The history's weight where a flicker that comes round with the jitter is kept, once it has settled
    // (research/250): this much of the way from PlusTAABlend to the most. 0.97 at the default 0.9 (about 33 frames,
    // four rounds, so the round's colours even out to under two 8-bit steps); less with a lighter TAA.
    constexpr F32 FLICKER_BLEND_TOWARDS_MAX = 0.875f;
    // The weight of the history where the picture moves: this much less than where it is still.
    constexpr F32 MOVING_BLEND_DROP = 0.35f;
    // ... but not below this share of it (a light TAA, PlusTAABlend near 0.5, must not become none while moving).
    // The pass takes up to 35% more off a history it had to clip far (llvktaa.cpp), so the least is 0.6 x 0.65 = 0.39 of still.
    constexpr F32 MOVING_BLEND_FLOOR = 0.6f;
    // Frames the viewer did not draw in between (idle skipping) are allowed up to this many: the picture is
    // brought over by the two cameras however many frames lie between. More is a login or a minimized window.
    constexpr U32 MAX_FRAME_GAP = 8;
    // A camera that moved further than this between two drawn frames jumped (a teleport,
    // the agent frame moving to another region): no history. As plus/ssr.
    constexpr F32 JUMP_METRES = 10.f;

    struct State
    {
        LLRenderTarget out[2];          // the picture's size: this frame's result and the last frame's, taken turns
        plus::RetryWait retry;          // after one could not be made (VRAM full)
        U32 last = 0;                   // out[last] holds the last frame's result (when has_history)
        bool has_history = false;       // out[last] is a picture of frame history_frame
        U32 history_frame = 0;          // gFrameCount of it
        U32 history_jitter_index = 0;   // the jitter's Halton index it was drawn with (1..JITTER_POINTS; 0: not jittered)
        U32 width = 0;                  // its size
        U32 height = 0;
        glm::mat4 last_view = glm::mat4(1.f);   // the camera of that frame
        glm::mat4 last_projection = glm::mat4(1.f);     // without the jitter

        // The jitter (JitterScope): the frame whose scene was drawn moved
        // (U32_MAX: none yet), by how much, and the projection unmoved.
        U32 jitter_index = 0;           // the last Halton index used (1..JITTER_POINTS)
        U32 jitter_frame = U32_MAX;
        glm::vec2 jitter_pixels = glm::vec2(0.f);   // where the samples lie from the pixel centres
        glm::mat4 jitter_unmoved = glm::mat4(1.f);
    };

    // Never destroyed: released with the screen's targets (LLPipeline::releaseScreenBuffers), before GL goes.
    State& state()
    {
        static State* state = new State();
        return *state;
    }

    // Not while the whole scene is drawn as a wireframe (the Develop menu's Wireframe, gUseWireframe): its lines are
    // thinner than a pixel's jitter, and TAA would mix them away (select-flicker).
    bool enabled()
    {
        static LLCachedControl<bool> enabled(gSavedSettings, SETTING_ENABLED, false);
        return enabled && !gUseWireframe;
    }

    F32 blendSetting()
    {
        static LLCachedControl<F32> blend(gSavedSettings, SETTING_BLEND, DEFAULT_BLEND);
        const F32 value = blend;
        return value >= MIN_BLEND ? llmin(value, MAX_BLEND) : MIN_BLEND;    // also a NaN: the smallest
    }

    // How much of the history is kept where a thin thing flickers with the jitter (research/246, 250), 0..1 (0: clipped
    // as any).
    F32 thinGapsSetting()
    {
        static LLCachedControl<F32> thin(gSavedSettings, SETTING_THIN_GAPS, 1.f);
        const F32 value = thin;
        return value > 0.f ? llmin(value, 1.f) : 0.f;      // also a NaN: none
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

    void releaseTarget(LLRenderTarget& target)
    {
        if (target.isComplete())
        {
            target.release();
        }
    }

    // The target at the picture's size: the result (half-float: with 8 bits the history, mixed in at 0.9 each frame,
    // would stop changing short of the picture), read with a bilinear filter (the pass takes five taps of it), and the
    // flicker's memory (research/250: an integer, read as it is). False when it could not be made.
    bool sizeTarget(State& s, LLRenderTarget& target, U32 width, U32 height)
    {
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        if (!s.retry.due())
        {
            return false;       // one could not be made a moment ago: not again every frame
        }
        releaseTarget(target);
        if (!target.allocate(width, height, GL_RGBA16F) || !target.addColorAttachment(GL_R32F))
        {
            target.release();   // a half-made target is not isComplete(): releaseTarget would skip it
            s.retry.failed();
            LL_WARNS_ONCE("Plus") << "TAA: no " << width << "x" << height << " target; the picture is left as it is" << LL_ENDL;
            return false;
        }
        s.retry.succeeded();
        target.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR);
        target.bindTexture(1, 0, LLTexUnit::TFO_POINT);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return true;
    }

    // The camera's place in the world of a view matrix.
    glm::vec3 eyePosition(const glm::mat4& view)
    {
        return glm::vec3(glm::inverse(view)[3]);
    }

    // Whether out[last] is the last frame's picture and this frame's camera is where it left off.
    bool historyUsable(const State& s, U32 width, U32 height, const glm::mat4& view)
    {
        if (!s.has_history || gFrameCount <= s.history_frame || gFrameCount - s.history_frame > MAX_FRAME_GAP || s.width != width ||
            s.height != height)
        {
            return false;       // a long gap is the login, a minimized window, or the setting having been off
        }
        const glm::vec3 moved = eyePosition(view) - eyePosition(s.last_view);
        return std::isfinite(moved.x) && std::isfinite(moved.y) && std::isfinite(moved.z) && glm::length(moved) <= JUMP_METRES;
    }

    // Whether what is drawn now is the plain main view and TAA, on, drew it a moment ago (so that it
    // will mix this frame's picture with a history): not a snapshot, a probe's face or a mirror.
    bool historyAhead(const State& s)
    {
        if (!enabled() || !LLRHI::sTaa || !LLRHI::sTaaDraw)
        {
            return false;
        }
        if (!s.has_history || !s.width || !s.height || gFrameCount <= s.history_frame || gFrameCount - s.history_frame > MAX_FRAME_GAP)
        {
            return false;
        }
        return !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT && !plus::presets::snapshotInProgress();
    }

    // The jitter's size for this frame's main view (0: not moved): TAA is on and drew a
    // moment ago (it takes the jitter out again; a frame it does not draw would show it),
    // the jitter is wanted, and this is the plain main view (not a snapshot or its tiles). Nor while a texture not
    // loaded yet is drawn as a wireframe (GrayWire, select-flicker): lines thinner than a pixel, moved by under one,
    // came and went with the jitter; TAA mixes the unmoved picture meanwhile. research/289: wireframes covering
    // enough of the view only (near ones; PlusWireframeTaaNearOnly off: any, as before).
    F32 jitterWanted(const State& s)
    {
        const F32 scale = jitterScale();
        if (scale <= 0.f || !historyAhead(s) || LLViewerCamera::getInstance()->getZoomFactor() != 1.f || GrayWire::drawnLately())
        {
            return 0.f;
        }
        return scale;
    }
}

namespace plus::taa
{
    bool render(LLRenderTarget*& source, LLRenderTarget*& target)
    {
        State& s = state();
        if (!enabled() || !LLRHI::sTaa || !LLRHI::sTaaDraw)
        {
            if (s.has_history || s.out[0].isComplete() || s.out[1].isComplete())
            {
                releaseTargets();   // off: nothing is kept
            }
            return false;
        }
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;       // a probe's face or a mirror: the main view's history is left as it is
        }
        if (plus::presets::snapshotInProgress())
        {
            s.has_history = false;      // a snapshot is drawn once, in tiles, at another size
            return false;
        }
        if (!source || !source->isComplete() || !gPipeline.mRT->deferredScreen.getDepth())
        {
            LL_WARNS_ONCE("Plus") << "TAA: no source or no depth texture; the picture is left as it is" << LL_ENDL;
            return false;
        }
        const U32 width = source->getWidth();
        const U32 height = source->getHeight();
        if (!width || !height)
        {
            return false;
        }

        const bool new_size = s.out[0].getWidth() != width || s.out[0].getHeight() != height;
        if (!sizeTarget(s, s.out[0], width, height) || !sizeTarget(s, s.out[1], width, height))
        {
            s.has_history = false;
            return false;
        }
        if (new_size)
        {
            LL_INFOS("Plus") << "TAA: targets made at " << width << "x" << height << " (the picture and its flicker memory)" << LL_ENDL;
        }

        const glm::mat4 view = get_last_modelview();    // the 3D scene's camera of this frame (renderGeomPostDeferred keeps it)
        // The projection without the jitter: the pixel centres are brought back and kept as the unmoved camera sees them.
        const bool jittered = s.jitter_frame == gFrameCount;
        const glm::mat4 projection = jittered ? s.jitter_unmoved : get_last_projection();
        const bool history = historyUsable(s, width, height, view);

        LLRHI::TaaPass pass;
        pass.source_texture = source->getTexture();
        pass.history_texture = history ? s.out[s.last].getTexture(0) : 0;
        pass.history_memory_texture = history ? s.out[s.last].getTexture(1) : 0;
        pass.depth_texture = gPipeline.mRT->deferredScreen.getDepth();
        pass.width = width;
        pass.height = height;
        if (history)
        {
            // In double, back to float once (research/246): the views hold the agent frame's offset (up to 4096 m high),
            // and in float the last view times the inverse of this one was off by up to 0.7 pixel with the camera still.
            const glm::mat4 reproject = glm::mat4(glm::dmat4(s.last_projection) * glm::dmat4(s.last_view) * glm::inverse(glm::dmat4(view)));
            memcpy(pass.reproject, glm::value_ptr(reproject), sizeof(pass.reproject));
        }
        const glm::mat4 inverse_projection = glm::inverse(projection);
        memcpy(pass.inverse_projection, glm::value_ptr(inverse_projection), sizeof(pass.inverse_projection));
        pass.blend_still = blendSetting();
        pass.blend_moving = llmax(pass.blend_still - MOVING_BLEND_DROP, pass.blend_still * MOVING_BLEND_FLOOR);
        pass.flicker_keep = thinGapsSetting();
        pass.flicker_blend = pass.blend_still + (MAX_BLEND - pass.blend_still) * FLICKER_BLEND_TOWARDS_MAX;
        pass.jittered = jittered;
        pass.jitter_pixels[0] = jittered ? s.jitter_pixels.x : 0.f;
        pass.jitter_pixels[1] = jittered ? s.jitter_pixels.y : 0.f;
        // The flicker memory keeps a sign per point of the round: only a frame whose point follows the history's goes on with it.
        pass.jitter_point = jittered ? s.jitter_index - 1 : 0;
        pass.jitter_round_goes_on = history && jittered && s.history_jitter_index != 0 &&
                                    s.history_jitter_index % JITTER_POINTS + 1 == s.jitter_index;

        const U32 write = history ? 1 - s.last : 0;
        LLRHI::gpuMark("taa");
        s.out[write].bindTarget();
        const bool drawn = LLRHI::sTaaDraw(pass);
        s.out[write].flush();
        if (!drawn)
        {
            s.has_history = false;
            return false;       // the picture (after FXAA/SMAA) is left as it is
        }

        s.last = write;
        s.has_history = true;
        s.history_frame = gFrameCount;
        s.history_jitter_index = jittered ? s.jitter_index : 0;
        s.width = width;
        s.height = height;
        s.last_view = view;
        s.last_projection = projection;

        target = source;                // the picture that came in is a spare of the same size
        source = &s.out[write];
        return true;
    }

    void releaseTargets()
    {
        State& s = state();
        releaseTarget(s.out[0]);
        releaseTarget(s.out[1]);
        s.has_history = false;
    }

    F32 jitterMargin()
    {
        return 0.5f * jitterWanted(state());
    }

    bool smoothsMainView()
    {
        return historyAhead(state());
    }

    bool jitteredThisFrame()
    {
        return state().jitter_frame == gFrameCount;
    }

    glm::vec2 depthJitter()
    {
        const State& s = state();
        return s.jitter_frame == gFrameCount ? s.jitter_pixels : glm::vec2(0.f);
    }

    JitterScope::JitterScope(bool for_snapshot)
    {
        State& s = state();
        const F32 scale = for_snapshot ? 0.f : jitterWanted(s);
        if (scale <= 0.f)
        {
            // A scene drawn unmoved after a moved one in the same frame (a snapshot's): what reads its depth or
            // glow is told it is not jittered (select-flicker).
            s.jitter_frame = U32_MAX;
            return;
        }
        s.jitter_index = s.jitter_index % JITTER_POINTS + 1;
        const glm::vec2 pixels((halton(s.jitter_index, 2) - 0.5f) * scale, (halton(s.jitter_index, 3) - 0.5f) * scale);
        // The picture's pixels (s.width x s.height, the size TAA drew it at) in NDC, which spans 2.
        const glm::mat4 shift = clipShift(glm::vec2(2.f * pixels.x / (F32)s.width, 2.f * pixels.y / (F32)s.height));
        mCurrent = get_current_projection();
        mLoaded = gGL.getProjectionMatrix();
        loadProjection(shift * mCurrent, shift * mLoaded);
        mApplied = true;
        s.jitter_frame = gFrameCount;
        s.jitter_pixels = pixels;
        s.jitter_unmoved = mCurrent;
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
