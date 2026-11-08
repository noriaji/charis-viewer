/**
 * @file skyscreen.cpp
 * @brief How the scene takes the sky's colour to the screen (research/176 section 9).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "skyscreen.h"

#include "llglslshader.h"
#include "llshadermgr.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "plus/plus.h"

namespace plus::skyscreen
{
    namespace
    {
        LLStaticHashedString sExposure("plus_sky_exposure");
        LLStaticHashedString sHdrScale("plus_sky_hdr_scale");
        LLStaticHashedString sTonemapMix("plus_sky_tonemap_mix");
        LLStaticHashedString sAces("plus_sky_aces");

        constexpr U32 TONEMAP_ACES_HILL = 1;    // tonemapUtilF.glsl's toneMap(): case 1

        F32 finiteOr(F32 value, F32 fallback)
        {
            return llfinite(value) ? value : fallback;
        }
    }

    Screen current(const LLSettingsSky* sky)
    {
        Screen screen;
        if (!sky)
        {
            return screen;
        }
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        static LLCachedControl<F32> auto_adjust_hdr_scale(gSavedSettings, "RenderSkyAutoAdjustHDRScale", 2.f);
        static LLCachedControl<U32> tonemap_type(gSavedSettings, "RenderTonemapType", 0U);

        // sky_hdr_scale: the same branches as LLSettingsVOSky::applySpecial outside the
        // irradiance pass (keep the two in step)
        F32 hdr_scale = 1.f;
        if (sky->getReflectionProbeAmbiance() != 0.f)
        {
            hdr_scale = sqrtf(llmax(sky->getGamma(), 0.f)) * 2.f;
        }
        else if (sky->canAutoAdjust() && should_auto_adjust)
        {
            hdr_scale = auto_adjust_hdr_scale;
        }
        hdr_scale = finiteOr(hdr_scale, 1.f);
        screen.hdr_scale = hdr_scale > 1e-3f ? hdr_scale : 1.f;

        // the tone mapping: LLPipeline::tonemap (none under "no post")
        screen.exposure = plus::exposureScaleForPasses();
        if (screen.exposure > 0.f)
        {
            screen.tonemap_mix = llclamp(finiteOr(sky->getTonemapMix(should_auto_adjust), 0.f), 0.f, 1.f);
            screen.aces = ((U32)tonemap_type == TONEMAP_ACES_HILL) ? 1.f : 0.f;
        }
        return screen;
    }

    S32 bind(LLGLSLShader& shader, const Screen& screen)
    {
        shader.uniform1f(sExposure, screen.exposure);
        shader.uniform1f(sHdrScale, screen.hdr_scale);
        shader.uniform1f(sTonemapMix, screen.tonemap_mix);
        shader.uniform1f(sAces, screen.aces);
        // Bound whether or not it is read, so the shader's sampler is never left unbound; the
        // exposure map is last frame's, as the fullbright shaders read it.
        const S32 channel = shader.enableTexture(LLShaderMgr::EXPOSURE_MAP);
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
        }
        return channel;
    }

    void unbind(S32 channel)
    {
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
        }
    }
}
