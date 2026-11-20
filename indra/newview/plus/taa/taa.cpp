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
#include "plus/temporal/temporal.h"
#include "presets/presets.h"

#include "llappviewer.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhitaa.h"
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
    constexpr char SETTING_THIN_GAPS[] = "PlusTAAThinGaps";

    // The jitter's points (Halton 2,3 from index 1, plus/temporal): with the history's weight
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

    struct State
    {
        LLRenderTarget out[2];          // the picture's size: this frame's result and the last frame's, taken turns
        plus::RetryWait retry;          // after one could not be made (VRAM full)
        U32 last = 0;                   // out[last] holds the last frame's result (when history.has())
        plus::temporal::FrameHistory history;   // has(): out[last] is a picture of that frame (its size and unmoved camera)
        U32 history_jitter_index = 0;   // the jitter's Halton index it was drawn with (1..JITTER_POINTS; 0: not jittered)
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
}

namespace plus::taa
{
    bool render(LLRenderTarget*& source, LLRenderTarget*& target)
    {
        State& s = state();
        if (!enabled() || !LLRHI::sTaa || !LLRHI::sTaaDraw)
        {
            if (s.history.has() || s.out[0].isComplete() || s.out[1].isComplete())
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
            s.history.drop();      // a snapshot is drawn once, in tiles, at another size
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
            s.history.drop();
            return false;
        }
        if (new_size)
        {
            LL_INFOS("Plus") << "TAA: targets made at " << width << "x" << height << " (the picture and its flicker memory)" << LL_ENDL;
        }

        const glm::mat4 view = get_last_modelview();    // the 3D scene's camera of this frame (renderGeomPostDeferred keeps it)
        // The projection without the jitter: the pixel centres are brought back and kept as the unmoved camera sees them.
        const bool jittered = plus::temporal::jitteredThisFrame();
        const glm::mat4 projection = jittered ? plus::temporal::unmovedProjection() : get_last_projection();
        const bool history = s.history.usable(width, height, view);

        LLRHI::TaaPass pass;
        pass.source_texture = source->getTexture();
        pass.history_texture = history ? s.out[s.last].getTexture(0) : 0;
        pass.history_memory_texture = history ? s.out[s.last].getTexture(1) : 0;
        pass.depth_texture = gPipeline.mRT->deferredScreen.getDepth();
        pass.width = width;
        pass.height = height;
        if (history)
        {
            // In double (research/246): in float the last view times the inverse of this one was off by up to 0.7 pixel
            // with the camera still.
            const glm::mat4 reproject = s.history.reprojectFrom(view);
            memcpy(pass.reproject, glm::value_ptr(reproject), sizeof(pass.reproject));
        }
        const glm::mat4 inverse_projection = glm::inverse(projection);
        memcpy(pass.inverse_projection, glm::value_ptr(inverse_projection), sizeof(pass.inverse_projection));
        pass.blend_still = blendSetting();
        pass.blend_moving = llmax(pass.blend_still - MOVING_BLEND_DROP, pass.blend_still * MOVING_BLEND_FLOOR);
        pass.flicker_keep = thinGapsSetting();
        pass.flicker_blend = pass.blend_still + (MAX_BLEND - pass.blend_still) * FLICKER_BLEND_TOWARDS_MAX;
        pass.jittered = jittered;
        const glm::vec2 jitter_pixels = plus::temporal::depthJitter();    // (0, 0) when not jittered
        pass.jitter_pixels[0] = jitter_pixels.x;
        pass.jitter_pixels[1] = jitter_pixels.y;
        // The flicker memory keeps a sign per point of the round: only a frame whose point follows the history's goes on with it.
        pass.jitter_point = jittered ? plus::temporal::jitterIndex() - 1 : 0;
        pass.jitter_round_goes_on = history && jittered && s.history_jitter_index != 0 &&
                                    s.history_jitter_index % JITTER_POINTS + 1 == plus::temporal::jitterIndex();

        const U32 write = history ? 1 - s.last : 0;
        LLRHI::gpuMark("taa");
        s.out[write].bindTarget();
        const bool drawn = LLRHI::sTaaDraw(pass);
        s.out[write].flush();
        if (!drawn)
        {
            s.history.drop();
            return false;       // the picture (after FXAA/SMAA) is left as it is
        }

        s.last = write;
        s.history.commit(width, height, view, projection);
        s.history_jitter_index = jittered ? plus::temporal::jitterIndex() : 0;

        target = source;                // the picture that came in is a spare of the same size
        source = &s.out[write];
        return true;
    }

    void releaseTargets()
    {
        State& s = state();
        releaseTarget(s.out[0]);
        releaseTarget(s.out[1]);
        s.history.drop();
    }

    bool historyAhead(U32& width, U32& height)
    {
        const State& s = state();
        if (!enabled() || !LLRHI::sTaa || !LLRHI::sTaaDraw || !s.history.ahead())
        {
            return false;
        }
        width = s.history.width();
        height = s.history.height();
        return true;
    }

    F32 jitterMargin()
    {
        return plus::temporal::jitterMargin();
    }

    bool jitteredThisFrame()
    {
        return plus::temporal::jitteredThisFrame();
    }

    glm::vec2 depthJitter()
    {
        return plus::temporal::depthJitter();
    }
}
