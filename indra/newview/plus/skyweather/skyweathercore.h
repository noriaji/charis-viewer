/**
 * @file skyweathercore.h
 * @brief The decision of plus/skyweather with no viewer in them (header only, so that
 *        skyweathercore_test.cpp builds it stand-alone): the priority (research/371
 *        section 3.4).
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

#ifndef PLUS_SKYWEATHER_SKYWEATHERCORE_H
#define PLUS_SKYWEATHER_SKYWEATHERCORE_H

namespace plus::skyweather::core
{
    struct Inputs
    {
        bool settingOn = false;         // PlusSkyWeather
        bool skyHasWeather = false;
        bool weatherFromLand = false;   // the weather is marked as other people's (LLSkyWeather::fromLand)
        int  ownRain = 0;               // PlusRain
        int  ownSnow = 0;               // PlusSnow
        bool allowParcel = false;       // PlusWeatherAllowParcel
    };

    // The priority: whether the sky's weather is the one in effect. Which environment the
    // sky is in does not matter: the weather itself says whose it is, and the mark goes with it
    // through every copy, blend and save (the land's environment is marked when it arrives).
    inline bool applies(const Inputs& in)
    {
        if (!in.settingOn || !in.skyHasWeather)
        {
            return false;
        }
        if (in.ownRain != 0 || in.ownSnow != 0)
        {
            return false;   // the user's own weather wins
        }
        return !in.weatherFromLand || in.allowParcel;   // other people's weather only with the opt-in
    }
}

#endif // PLUS_SKYWEATHER_SKYWEATHERCORE_H
