/**
 * @file atmosphere.h
 * @brief Atmosphere effects for photography: ground fog (and the light shafts
 *        in it), fireflies (research/148).
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

#ifndef PLUS_ATMOSPHERE_H
#define PLUS_ATMOSPHERE_H

#include "stdtypes.h"

#include <glm/mat4x4.hpp>

// Every hook returns at once while its effect is off (the default), with GL,
// or with LLRHI_ATMOSPHERE=0. Main thread.
namespace plus::atmosphere
{
    // The <plus> hook in LLDrawPoolAlpha::renderPostDeferred() for the main
    // view's above-water alpha, with mRT->screen bound: lays the ground fog
    // over the scene, so that the alpha blends over the fog as over the rest
    // of the scene behind it. renderFog() then draws none this frame.
    void renderFogBeforeAlpha();

    // The <plus> hook in LLPipeline::renderFinalize(), before the exposure is
    // measured: the fog, unless renderFogBeforeAlpha() drew it this frame.
    void renderFog();

    // The <plus> hook in LLPipeline::renderFinalize(), after the exposure is
    // measured and before the glare and the tone mapping: the fireflies added
    // to the scene's HDR target (mRT->screen). Drawn only with HDR on.
    void renderFireflies();

    // The ground fog as the light shafts (plus/volumetric) see it: the mist
    // that makes them show. Densities per metre at the fog's level.
    struct ShaftMist
    {
        F32 up_eye[3] = {};             // world up in eye space, unit length
        F32 camera_height = 0.f;        // m above the fog's level (negative: below)
        F32 inverse_height = 0.f;       // 1/m
        F32 scatter = 0.f;              // the scattering the shafts add at the level (the sun's light, shadowed)
        F32 extinction = 0.f;           // what the fog takes out of that light on its way to the eye
    };

    // Fills mist for the camera view modelview (agent -> eye) and returns
    // true while the ground fog is on and drawn (so it is off with GL, under
    // water, ...); false: the shafts get no mist. Main thread.
    bool shaftMist(const glm::mat4& modelview, ShaftMist& mist);
}

#endif // PLUS_ATMOSPHERE_H
