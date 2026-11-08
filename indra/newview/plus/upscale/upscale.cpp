/**
 * @file upscale.cpp
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"
#include "upscale.h"
#include "presets/presets.h"
#include "plus/photomode/phototoolsfloater.h"

#include "llcallbacklist.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhiupscale.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "pipeline.h"

#include <utility>

namespace
{
    constexpr char SETTING_MODE[] = "PlusUpscale";
    constexpr char SETTING_SCALE[] = "PlusUpscaleScale";
    constexpr char SETTING_SHARPNESS[] = "PlusUpscaleSharpness";

    enum Mode : U32
    {
        MODE_OFF = 0,
        MODE_FSR = 1,       // EASU, then RCAS
        MODE_BILINEAR = 2,  // the official divisor's stretch, to compare
        MODE_COUNT
    };

    constexpr F32 MIN_SCALE = 0.5f;         // FSR 1's "performance" (2x on each side)
    constexpr F32 FULL_SCALE = 0.995f;      // at or above: the full size (nothing upscaled)
    constexpr F32 DEFAULT_SCALE = 0.67f;
    constexpr F32 DEFAULT_SHARPNESS = 0.5f;
    // The smallest side drawn (research/165 review 4): a minimized or very
    // small window is drawn at its own size.
    constexpr U32 MIN_SIDE = 64;
    // How often the photo mode (plus::photomode::inPhotoMode) is looked at while upscaling is chosen.
    constexpr F64 PHOTO_MODE_PERIOD = 0.25;

    struct Targets
    {
        LLRenderTarget out[2];          // the window's size: the upscaled picture and a spare (RCAS, the look)
        plus::RetryWait retry;          // after one could not be made (VRAM full)
    };

    // Never destroyed: released with the screen's targets (LLPipeline::releaseScreenBuffers), before GL goes.
    Targets& targets()
    {
        static Targets* targets = new Targets();
        return *targets;
    }

    // What the main view's targets were last made at (noteMainTargets), and
    // the drawn size last asked for (a resize requested once per change of
    // the size wanted, also when what is made then differs).
    bool sActive = false;
    U32 sMadeX = 0;
    U32 sMadeY = 0;
    U32 sRequestedX = 0;
    U32 sRequestedY = 0;

    bool sPhotoMode = false;
    F64 sPhotoModeChecked = 0.0;

    U32 modeSetting()
    {
        static LLCachedControl<U32> mode(gSavedSettings, SETTING_MODE, MODE_OFF);
        const U32 value = mode();
        return value < MODE_COUNT ? value : MODE_OFF;
    }

    F32 scaleSetting()
    {
        static LLCachedControl<F32> scale(gSavedSettings, SETTING_SCALE, DEFAULT_SCALE);
        const F32 value = scale();
        return value >= MIN_SCALE ? llmin(value, 1.f) : MIN_SCALE;    // also a NaN: the smallest
    }

    F32 sharpnessSetting()
    {
        static LLCachedControl<F32> sharpness(gSavedSettings, SETTING_SHARPNESS, DEFAULT_SHARPNESS);
        const F32 value = sharpness();
        return value > 0.f ? llmin(value, 1.f) : 0.f;
    }

    // The main view is to be drawn smaller now: chosen, drawable, not for a
    // snapshot and not in the photo mode (the photo screen, or the photo tools
    // open and not minimized: what is seen there is what is taken).
    bool wanted()
    {
        return modeSetting() != MODE_OFF && LLRHI::sUpscale && LLRHI::sUpscaleDraw && scaleSetting() < FULL_SCALE &&
               !plus::presets::snapshotInProgress() && !sPhotoMode;
    }

    // One side drawn at scale: even (no odd pixel for the half-size passes),
    // at least MIN_SIDE, at most the side itself.
    U32 scaledSide(U32 side, F32 scale)
    {
        if (side < 2 * MIN_SIDE)
        {
            return side;
        }
        const U32 even = (U32)((F32)side * scale + 1.f) & ~1u;
        return llclamp(even, MIN_SIDE, side);
    }

    void releaseTarget(LLRenderTarget& target)
    {
        if (target.isComplete())
        {
            target.release();
        }
    }

    // The target at the window's size (sampled by the passes after it); false when it could not be made.
    bool sizeTarget(LLRenderTarget& target, U32 width, U32 height, U32 format)
    {
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        plus::RetryWait& retry = targets().retry;
        if (!retry.due())
        {
            return false;       // one could not be made a moment ago: not again every frame
        }
        if (!target.allocate(width, height, format))
        {
            target.release();   // a half-made target is not isComplete(): releaseTarget would skip it
            retry.failed();
            LL_WARNS_ONCE("Plus") << "Upscale: no " << width << "x" << height << " target; the picture is stretched as it is" << LL_ENDL;
            return false;
        }
        retry.succeeded();
        // Then unit 0 is left empty, not holding the new texture in the middle of the post-processing.
        target.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return true;
    }

    // Asks LLPipeline for the screen targets again when the drawn size wanted
    // for the main view is not the one made (nor already asked for).
    void requestIfChanged()
    {
        const LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
        if (rt != &gPipeline.mMainRT || !rt->width || !rt->height)
        {
            return;     // not made yet, or inside a probe's or a mirror's drawing
        }
        U32 x = 0, y = 0;
        plus::upscale::drawSize(rt->width, rt->height, true, LLPipeline::RenderResolutionDivisor, x, y);
        if (x == sMadeX && y == sMadeY)
        {
            sRequestedX = x;    // back to what is made: a later change asks again
            sRequestedY = y;
            return;
        }
        if (x == sRequestedX && y == sRequestedY)
        {
            return;             // asked for already
        }
        sRequestedX = x;
        sRequestedY = y;
        gPipeline.requestResizeScreenTexture();
    }

    void updatePhotoMode()
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (now - sPhotoModeChecked < PHOTO_MODE_PERIOD)
        {
            return;
        }
        sPhotoModeChecked = now;
        // The photo screen or the photo tools in use (minimized: not in use, upscaled again)
        const bool photo_mode = plus::photomode::inPhotoMode();
        if (photo_mode != sPhotoMode)
        {
            sPhotoMode = photo_mode;
            LL_INFOS("Plus") << "Upscale: the photo mode " << (photo_mode ? "on, the view drawn at the full size" : "off, the view upscaled again")
                             << LL_ENDL;
        }
    }

    // Each frame at idle. Off and not upscaled: nothing but the setting read.
    bool onIdle()
    {
        if (modeSetting() == MODE_OFF && !sActive)
        {
            sPhotoMode = false;
            return false;
        }
        updatePhotoMode();
        requestIfChanged();
        return false;   // repeat
    }

    // The outermost snapshot begins (in_progress) or ends: its frames are
    // drawn at the full size. Asked for before its first frame (the snapshot
    // runs inside one idle); a snapshot larger than the window makes its own
    // targets at its size and drops the request (plus_targets_made).
    void onSnapshot(bool /*in_progress*/)
    {
        requestIfChanged();
    }

    void init()
    {
        plus::presets::addSnapshotListener(&onSnapshot);
    }

    void onUIReady()
    {
        doOnIdleRepeating(&onIdle);
    }

    plus::FeatureRegistration sRegistration({ "upscale", &init, &onUIReady });
}

namespace plus::upscale
{
    bool drawSize(U32 res_x, U32 res_y, bool main_view, U32 divisor, U32& out_x, U32& out_y)
    {
        if (main_view && wanted())
        {
            const F32 scale = scaleSetting();
            const U32 x = scaledSide(res_x, scale);
            const U32 y = scaledSide(res_y, scale);
            if (x < res_x || y < res_y)
            {
                out_x = x;
                out_y = y;
                return true;
            }
        }
        // The official RenderResolutionDivisor, as allocateScreenBufferInternal had it.
        out_x = res_x;
        out_y = res_y;
        if (divisor > 1 && divisor < res_x && divisor < res_y)
        {
            out_x = res_x / divisor;
            out_y = res_y / divisor;
        }
        return false;
    }

    void noteMainTargets(bool upscaled, U32 drawn_x, U32 drawn_y)
    {
        if (upscaled != sActive || drawn_x != sMadeX || drawn_y != sMadeY)
        {
            const LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
            LL_INFOS("Plus") << "Upscale: the main view drawn at " << drawn_x << "x" << drawn_y << " for " << rt->width << "x" << rt->height
                             << (upscaled ? " (upscaled)" : "") << LL_ENDL;
        }
        // The size asked for is kept: when this is another (made smaller after a failure, doAllocateScreenBuffer),
        // it is not asked for again every frame.
        sActive = upscaled;
        sMadeX = drawn_x;
        sMadeY = drawn_y;
        if (!upscaled)
        {
            releaseTargets();
        }
    }

    bool drawnSizeMatches(U32 res_x, U32 res_y, U32 divisor, U32 drawn_x, U32 drawn_y)
    {
        U32 x = 0, y = 0;
        drawSize(res_x, res_y, true, divisor, x, y);
        return x == drawn_x && y == drawn_y;
    }

    bool active()
    {
        return sActive;
    }

    U32 effectiveAAType(U32 fsaa_type)
    {
        return sActive ? llmax(fsaa_type, 1u) : fsaa_type;
    }

    bool replacesCAS()
    {
        return sActive && modeSetting() == MODE_FSR && LLRHI::sUpscaleDraw;
    }

    void render(LLRenderTarget*& source, LLRenderTarget*& target)
    {
        if (!sActive)
        {
            return;
        }
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        const U32 width = rt.width;
        const U32 height = rt.height;
        if (!LLRHI::sUpscaleDraw || !source || !source->isComplete() || !width || !height ||
            (source->getWidth() == width && source->getHeight() == height))
        {
            return;
        }
        static LLCachedControl<bool> high_precision(gSavedSettings, "RenderHighPrecisionPostProcess", false);
        const U32 format = high_precision() ? GL_RGBA16F : GL_RGBA;
        Targets& t = targets();
        if (!sizeTarget(t.out[0], width, height, format) || !sizeTarget(t.out[1], width, height, format))
        {
            return;
        }

        LLRHI::gpuMark("upscale");
        const bool fsr = modeSetting() != MODE_BILINEAR;
        LLRHI::UpscalePass pass;
        pass.kind = fsr ? LLRHI::UpscalePass::EASU : LLRHI::UpscalePass::BILINEAR;
        pass.source_texture = source->getTexture();
        pass.source_width = source->getWidth();
        pass.source_height = source->getHeight();
        pass.width = width;
        pass.height = height;
        t.out[0].bindTarget();
        const bool drawn = LLRHI::sUpscaleDraw(pass);
        t.out[0].flush();
        if (!drawn)
        {
            return;     // the final pass stretches the picture as it is
        }
        LLRenderTarget* picture = &t.out[0];
        LLRenderTarget* spare = &t.out[1];

        const F32 sharpness = fsr ? sharpnessSetting() : 0.f;
        if (sharpness > 0.f)
        {
            pass.kind = LLRHI::UpscalePass::RCAS;
            pass.source_texture = t.out[0].getTexture();
            pass.source_width = width;
            pass.source_height = height;
            pass.sharpness = sharpness;
            t.out[1].bindTarget();
            const bool sharpened = LLRHI::sUpscaleDraw(pass);
            t.out[1].flush();
            if (sharpened)
            {
                std::swap(picture, spare);
            }
        }
        source = picture;
        target = spare;
    }

    void releaseTargets()
    {
        for (LLRenderTarget& target : targets().out)
        {
            releaseTarget(target);
        }
    }
}
