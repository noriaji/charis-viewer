/**
 * @file skyscreen.h
 * @brief How the scene takes the sky's colour to the screen, for the sky's shaders that undo it (research/176 section 9).
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

#ifndef PLUS_SKYSCREEN_H
#define PLUS_SKYSCREEN_H

#include "stdtypes.h"

class LLGLSLShader;
class LLSettingsSky;

// What the sky (the WindLight dome, the stars, the clouds) written to the G buffer goes
// through before the screen: softenLightF.glsl takes it as sRGB and scales it by
// sky_hdr_scale; unless "no post" (plus::isNoPost), the tone mapping multiplies it by
// RenderExposure and the exposure map and puts it through the curve, mixed with the
// linear light by the sky's tonemap_mix (tonemapUtilF.glsl). The star field (starsF.glsl)
// and, at night, the clouds (cloudsF.glsl, PlusNightCloudLift) undo this with
// plusSkyLight() so that they show at the brightness they mean whatever the sky's HDR
// scale and the exposure.
namespace plus::skyscreen
{
    struct Screen
    {
        F32 exposure = 0.f;     // RenderExposure (0.5 .. 4), or 0 under "no post": no exposure, no curve
        F32 hdr_scale = 1.f;    // sky_hdr_scale
        F32 tonemap_mix = 0.f;  // the curve's share (the rest is the linear light)
        F32 aces = 0.f;         // 1 when the curve is ACES Hill (RenderTonemapType 1), else 0
    };

    // For the sky in hand (null: the "no post" values). Main thread.
    Screen current(const LLSettingsSky* sky);

    // Sets the plus_sky_* uniforms of the bound shader and binds the exposure map to its
    // exposureMap. Returns the texture unit taken (-1: none), for unbind().
    S32 bind(LLGLSLShader& shader, const Screen& screen);
    void unbind(S32 channel);
}

#endif // PLUS_SKYSCREEN_H
