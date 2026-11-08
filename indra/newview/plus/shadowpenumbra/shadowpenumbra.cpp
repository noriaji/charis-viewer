/**
 * @file shadowpenumbra.cpp
 * @brief A penumbra that grows with the distance from the caster in the
 *        sun's shadows (research/187 in the sl-viewer repository).
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

#include "shadowpenumbra.h"
#include "presets/presets.h"

#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llviewercontrol.h"
#include "pipeline.h"

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    constexpr char SETTING_PENUMBRA[] = "PlusShadowPenumbra";
    constexpr char SETTING_STABILIZE[] = "PlusShadowStabilize";
    constexpr char SETTING_SHADOW_DETAIL[] = "RenderShadowDetail";

    constexpr F32 MAX_PENUMBRA = 8.f;
    // The widest penumbra, in metres: a tall caster's shadow is not smeared further than this.
    constexpr F32 MAX_WIDTH_METRES = 0.6f;
    // Taps of the blocker search and of the filter: the main view, and a snapshot.
    constexpr F32 BLOCKER_TAPS = 8.f;
    constexpr F32 FILTER_TAPS = 12.f;
    constexpr F32 SNAPSHOT_BLOCKER_TAPS = 24.f;
    constexpr F32 SNAPSHOT_FILTER_TAPS = 32.f;

    const LLStaticHashedString sPenumbra("plus_penumbra");
    const LLStaticHashedString sParams("plus_penumbra_params");

    // The penumbra to draw with now, 0 for the official read.
    F32 penumbraNow()
    {
        static LLCachedControl<F32> penumbra(gSavedSettings, SETTING_PENUMBRA, 0.f);
        static LLCachedControl<bool> stabilize(gSavedSettings, SETTING_STABILIZE, true);
        static LLCachedControl<S32> shadow_detail(gSavedSettings, SETTING_SHADOW_DETAIL, 0);
        if (gCubeSnapshot || !stabilize || shadow_detail < 1)
        {
            return 0.f;
        }
        const F32 value = penumbra;
        return value > 0.f ? llmin(value, MAX_PENUMBRA) : 0.f;   // also a NaN: none
    }
}

namespace plus::shadowpenumbra
{
    void bind(LLGLSLShader& shader)
    {
        const F32 penumbra = penumbraNow();
        shader.uniform1f(sPenumbra, penumbra);
        if (penumbra <= 0.f)
        {
            return;
        }
        const bool snapshot = plus::presets::snapshotInProgress();
        shader.uniform4f(sParams, 0.f, snapshot ? SNAPSHOT_BLOCKER_TAPS : BLOCKER_TAPS, snapshot ? SNAPSHOT_FILTER_TAPS : FILTER_TAPS,
                         MAX_WIDTH_METRES);
        for (U32 i = 0; i < 2; ++i)
        {
            LLRenderTarget* target = gPipeline.getSunShadowTarget(i);
            if (target)
            {
                shader.bindTexture(LLShaderMgr::PLUS_SHADOW_RAW0 + i, target, true);
            }
        }
    }

    void unbind(LLGLSLShader& shader)
    {
        if (penumbraNow() <= 0.f)
        {
            return;     // nothing was bound
        }
        for (U32 i = 0; i < 2; ++i)
        {
            shader.unbindTexture(LLShaderMgr::PLUS_SHADOW_RAW0 + i);
        }
    }
}
