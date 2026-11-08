/**
 * @file rain.h
 * @brief Rain: falling streaks and splashes, and the overhead depth map that
 *        keeps them (and the wet look) off what lies under a roof
 *        (research/120-rain-idea.md, stages 2 and 3).
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

#ifndef PLUS_RAIN_H
#define PLUS_RAIN_H

#include "llrhirain.h"

#include <glm/mat4x4.hpp>

class LLCamera;

// Both hooks return at once while their work is off: the rain (PlusRain 0,
// the default) and, for the overhead map, the wet look too (its passes not
// running: plus::wetlook::wantsOverhead); with GL, or LLRHI_RAIN=0. The
// rain counts as off while RenderHDREnabled is off (it is drawn only with
// HDR). Main thread.
namespace plus::rain
{
    // The <plus> hook in display() (llviewerdisplay.cpp), right after the sun's
    // shadows of the main view (not for snapshots): draws the overhead map
    // when it is wanted and the camera has moved (or a few seconds passed);
    // releases it when neither the rain nor the wet look is on.
    void renderOverhead(LLCamera& camera);

    // The <plus> hook in LLPipeline::renderFinalize(), after the exposure is
    // measured and before the glare and the tone mapping: the rain laid over
    // the scene's HDR target (mRT->screen).
    void renderStreaks();

    // For the wet look (plus/wetlook), a pass in eye space: the overhead map
    // with to_map from eye space (modelview: the camera's, agent -> eye).
    // No map (depth_texture 0) while there is none.
    LLRHI::RainOverhead overheadForEye(const glm::mat4& modelview);

    // For the wet look: the rain's level now (PlusRain: 1 drizzle, 2 rain,
    // 3 downpour), 0 while it does not rain or cannot be seen (off, GL,
    // LLRHI_RAIN=0, RenderHDREnabled off). While PlusRainWetAll is on, the
    // wet look wets what the rain falls on by it.
    S32 rainLevel();

    // For the wet look: the snow's level now (PlusSnow: 1 light, 2 snow,
    // 3 heavy), 0 while it does not snow or cannot be seen (as rainLevel()).
    // While it snows there is no rain: rainLevel() is 0.
    S32 snowLevel();

    // For the wet look: while it rains (and can be seen, as rainLevel()), the
    // puddles' ripples (their tilt, and rings per cell and second); false
    // while it does not.
    bool wetLookRipples(F32& tilt, F32& rate);
}

#endif // PLUS_RAIN_H
