/**
 * @file nightclouds.cpp
 * @brief The night clouds under an auto-adjusted classic sky (research/176 section 10).
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

#include "nightclouds.h"

#include "llsettingssky.h"
#include "llviewercontrol.h"
#include "plus/plus.h"

namespace plus::nightclouds
{
    namespace
    {
        // The sun's height (the z of its direction) where the lift starts and is full: about
        // 1 and 8 degrees below the horizon, so the sunset and the twilight stay as they are.
        constexpr F32 SUN_Z_START = -0.02f;
        constexpr F32 SUN_Z_FULL = -0.14f;
    }

    F32 lift(const LLSettingsSky* sky)
    {
        static LLCachedControl<F32> setting(gSavedSettings, "PlusNightCloudLift", 0.75f);
        const F32 strength = llfinite((F32)setting) ? llclamp((F32)setting, 0.f, 1.f) : 0.f;
        if (strength <= 0.f || !sky)
        {
            return 0.f;
        }
        // Only a classic sky that the auto-adjust puts through the tone mapping: an HDR sky and
        // a classic one shown as it is ("no post") keep the official clouds.
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        if (!should_auto_adjust || !sky->canAutoAdjust() || plus::isNoPost(sky))
        {
            return 0.f;
        }
        const F32 t = llclamp((SUN_Z_START - sky->getSunDirection().mV[VZ]) / (SUN_Z_START - SUN_Z_FULL), 0.f, 1.f);
        return strength * t * t * (3.f - 2.f * t);
    }
}
