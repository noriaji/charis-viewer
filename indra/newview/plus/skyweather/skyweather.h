/**
 * @file skyweather.h
 * @brief The weather the current sky carries (LLSettingsSky::getWeather(),
 *        saved in a sky setting and in a day cycle's keyframes), as the
 *        slwx::Effective that plus/rain reads (research/371 section 3.4).
 *        plus/slwx asks it first; plus/rain is not changed.
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

#ifndef PLUS_SKYWEATHER_SKYWEATHER_H
#define PLUS_SKYWEATHER_SKYWEATHER_H

#include "plus/slwx/slwx.h"

#include "llsettingsbase.h"

#include <string>

struct LLSkyWeather;

namespace plus::skyweather
{
    // What the weather `w` makes of the rain, snow and wind at `utc` seconds.
    // Rain and snow go through the tag's scale (slwxtag rainToMm / snowToCm),
    // the wind and its gusts through slwxsync with a fixed seed (so that
    // everybody who uses the same sky sees the same gusts). Reads no setting.
    void convert(const LLSkyWeather& w, double utc, plus::slwx::Effective& out);

    // The weather that arrives with the land's environment (the parcel's, the region's, any
    // environment the server sends) and the scripts' (experiences) is other people's: it is marked
    // when it arrives (LLSkyWeather::fromLand), and the mark goes with the weather through every
    // copy, save and read back, and through the sky's own blends (LLSettingsSky::blend), so wherever
    // the sky ends up (the photo mode's sun, the personal environment window, an edit window,
    // ENV_LOCAL, a day cycle saved to the inventory) it is still known. It goes with the data, so it
    // is not kept across log-in by an asset ID (the viewer reads the author's asset back, with no
    // mark, and not the weather as it is now, which a user may have rewritten): a sky or day cycle
    // with any weather (hasAnyWeather(), marked or not) is saved whole instead, whatever its flag
    // says now, so that what comes back is what was there. One with no name (or the default name)
    // is not kept at all, and the land's environment comes back (as the official viewer does for
    // a setting that was never saved).
    // One thing does not carry it: a script's sky blended in as LLSD (LLSettingsInjected) keeps a
    // map key only when both sides have it. The source of that blend is the shared environment (the
    // parcel's or the region's), whose weather is always marked, so the mark stays; a change that
    // lets an unmarked weather be the source must mark it too. Main thread.
    void markFromLand(const LLSettingsBase::ptr_t& settings);
    void markSkyLLSDFromLand(LLSD& sky);
    bool hasAnyWeather(const LLSettingsBase::ptr_t& settings);

    // Every frame, main thread (the answer is kept for the frame). True, and
    // `out` written, when the current sky carries a weather, PlusSkyWeather is
    // on, the viewer's own rain and snow (PlusRain, PlusSnow) are both 0, and
    // the weather is the user's own, or the user allows other people's weather
    // (PlusWeatherAllowParcel: for a weather marked as the land's). A weather
    // that blends with the land's (a transition into or out of it) is marked
    // while it blends. False otherwise (`out` untouched): then the parcel's
    // tag and the settings stand.
    bool effective(plus::slwx::Effective& out);

    // The checks (the conversion's levels, scales and wind; the decisions are checked by skyweathercore_test.cpp).
    // Debug builds run it at start. False names the first failure in `report`.
    bool selftest(std::string& report);
}

#endif // PLUS_SKYWEATHER_SKYWEATHER_H
