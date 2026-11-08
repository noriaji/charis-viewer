/**
 * @file wetlook.h
 * @brief Wet look: wet ground and puddles, made in the G-buffer before the
 *        lighting (research/120-rain-idea.md, stage 1).
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

#ifndef PLUS_WETLOOK_H
#define PLUS_WETLOOK_H

// The hook returns at once while the feature is off (PlusWetLook 0, the
// default, and no rain wetting things: PlusRain 0 or PlusRainWetAll off, and
// what an earlier rain wetted has dried), with GL or LLRHI_WET_LOOK=0, and
// for reflection probe faces and mirrors. Main thread.
namespace plus::wetlook
{
    // For plus/rain: whether the wet look reads the overhead map now (its
    // passes run: the setting is on, it rains, or the rain's wetness has not
    // dried yet).
    bool wantsOverhead();

    // The <plus> hook in LLPipeline::renderDeferredLighting(), after the
    // G-buffer pass and before anything reads it (the sun's light map, the
    // lighting): the main view's G-buffer is made wet.
    void render();
}

#endif // PLUS_WETLOOK_H
