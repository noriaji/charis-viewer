/**
 * @file nightclouds.h
 * @brief The night clouds under an auto-adjusted classic sky: kept from the tone curve's crushing (research/176 section 10).
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

#ifndef PLUS_NIGHTCLOUDS_H
#define PLUS_NIGHTCLOUDS_H

#include "stdtypes.h"

class LLSettingsSky;

// A classic (WindLight) sky lit the new way by RenderSkyAutoAdjustLegacy (the photo tools'
// "auto-adjust legacy skies") goes through the tone mapping, whose ACES curve crushes the dark:
// at night the clouds, a dark grey a little above the sky, come out black. cloudsF.glsl lifts
// them back towards the brightness the same sky shows them at without the auto-adjust (it
// undoes the curve with plus/skyscreen). Only the clouds' pixels change: the rest of the scene
// keeps the tone mapping as it is. Nothing changes by day, at sunset, under an HDR (EEP) sky
// or with the auto-adjust off.
namespace plus::nightclouds
{
    // PlusNightCloudLift (0 .. 1) times how far into the night it is (0 until the sun is
    // about 1 degree below the horizon, 1 from about 8 degrees), for an auto-adjusted classic
    // sky put through the tone mapping; else 0. Cached; returns at once when off. Main thread.
    F32 lift(const LLSettingsSky* sky);
}

#endif // PLUS_NIGHTCLOUDS_H
