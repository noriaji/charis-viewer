/**
 * @file skyweather.cpp
 * @brief The weather the current sky carries as the rain asks for it
 *        (research/371 section 3.4).
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

#include "plus.h"

#include "skyweather.h"
#include "skyweathercore.h"

#include "plus/slwx/slwxsync.h"
#include "plus/slwx/slwxtag.h"

#include "llenvironment.h"
#include "llframetimer.h"
#include "llsettingssky.h"
#include "llskyweather.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace plus::skyweather
{
    namespace
    {
        constexpr char SETTING_ON[] = "PlusSkyWeather";
        constexpr char SETTING_RAIN[] = "PlusRain";
        constexpr char SETTING_SNOW[] = "PlusSnow";
        // The gusts of a sky's weather: one seed for all, so that those who use the
        // same sky (or the same day cycle at the same UTC second) see the same gusts.
        constexpr uint32_t SEED = 0x5EEDC10Du;

        U64 sFrame = ~(U64)0;           // the frame the answer below is for
        bool sAnswer = false;
        plus::slwx::Effective sEffective = {};

        bool compute()
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, true);
            static LLCachedControl<S32> own_rain(gSavedSettings, SETTING_RAIN, 0);
            static LLCachedControl<S32> own_snow(gSavedSettings, SETTING_SNOW, 0);
            if (!on)
            {
                return false;
            }
            const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
            core::Inputs in;
            in.settingOn = true;
            in.skyHasWeather = sky && sky->hasWeather();
            in.weatherFromLand = in.skyHasWeather && sky->getWeather().fromLand;
            in.ownRain = own_rain;
            in.ownSnow = own_snow;
            in.allowParcel = plus::slwx::allowParcel();
            if (!core::applies(in))
            {
                return false;
            }
            convert(sky->getWeather(), plus::slwx::sync::now(), sEffective);
            return true;
        }

        bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

#define SKYWEATHER_CHECK(cond, what) \
        do { if (!(cond)) { report = what; return false; } } while (0)

        void init()
        {
            // Setting: app_settings/settings_plus.xml (PlusSkyWeather); the switch is on the
            // photo tools' Sky & Weather tab (app_settings/plus/phototools/catalog.xml).
            // plus/slwx asks effective() before the parcel's tag.
#ifdef _DEBUG
            std::string report;
            if (!selftest(report))
            {
                LL_WARNS("SLWX") << report << LL_ENDL;
            }
#endif
        }

        plus::FeatureRegistration sRegistration({ "skyweather", &init, {} });
    }

    void convert(const LLSkyWeather& w, double utc, plus::slwx::Effective& out)
    {
        namespace tag = plus::slwx::tag;
        // The tag's Params carry the wind; the rest of it is not used here.
        tag::Params p;
        p.wind_from = w.windDir;
        p.wind_speed = w.wind;
        p.var = w.windVar;
        p.gust = w.gust;
        p.wdirvar = w.windDirVar;
        const plus::slwx::sync::Wind wind = plus::slwx::sync::wind(p, SEED, utc);

        const float mm = tag::rainToMm(w.rain);
        const float cm = tag::snowToCm(w.snow);
        out.fromOutside = true;
        out.rainLevel = tag::rainLevelOfMm(mm);
        out.rainMm = mm;
        out.snowLevel = tag::snowLevelOfCm(cm);
        out.windSpeed = wind.speed;
        out.windFromDeg = wind.from_deg;
        out.windBaseSpeed = std::min(std::max(w.wind, 0.f), tag::WIND_SPEED_MAX);
        out.storm = std::min(std::max(w.storm, 0.f), 1.f);
    }

    void markFromLand(const LLSettingsBase::ptr_t& settings)
    {
        LLSkyWeather::markFromLand(settings);
    }

    void markSkyLLSDFromLand(LLSD& sky)
    {
        LLSkyWeather::markSkyLLSDFromLand(sky);
    }

    bool hasAnyWeather(const LLSettingsBase::ptr_t& settings)
    {
        return LLSkyWeather::hasAnyWeather(settings);
    }

    bool effective(plus::slwx::Effective& out)
    {
        const U64 frame = LLFrameTimer::getFrameCount();
        if (frame != sFrame)
        {
            sFrame = frame;
            sAnswer = compute();
        }
        if (sAnswer)
        {
            out = sEffective;
        }
        return sAnswer;
    }

    bool selftest(std::string& report)
    {
        namespace tag = plus::slwx::tag;
        // The values the weather tab's import (research/371 3.5) writes come back as their levels.
        const float of_level[4] = { 0.f, 0.15f, 0.4f, 0.8f };
        for (int level = 0; level < 4; ++level)
        {
            LLSkyWeather w;
            w.rain = of_level[level];
            w.snow = of_level[level];
            plus::slwx::Effective e = {};
            convert(w, 1.0e9, e);
            SKYWEATHER_CHECK(e.rainLevel == level, "skyweather: rain level of the import value");
            SKYWEATHER_CHECK(e.snowLevel == level, "skyweather: snow level of the import value");
        }
        {
            LLSkyWeather w;
            w.rain = 0.f;
            plus::slwx::Effective e = {};
            convert(w, 1.0e9, e);
            SKYWEATHER_CHECK(e.rainLevel == 0 && e.snowLevel == 0 && approx(e.rainMm, 0.f), "skyweather: no rain");
            w.rain = 1.f;
            convert(w, 1.0e9, e);
            SKYWEATHER_CHECK(e.rainLevel == 3 && approx(e.rainMm, 50.f, 0.01f), "skyweather: full rain is 50 mm/h");
            w.rain = 0.001f;
            convert(w, 1.0e9, e);
            SKYWEATHER_CHECK(e.rainLevel == 1, "skyweather: a little rain is a drizzle");
        }
        for (float x = 0.f; x <= 1.f; x += 0.05f)
        {
            SKYWEATHER_CHECK(approx(tag::mmToRain(tag::rainToMm(x)), x), "skyweather: rain scale round trip");
            SKYWEATHER_CHECK(approx(tag::cmToSnow(tag::snowToCm(x)), x), "skyweather: snow scale round trip");
        }
        {
            // No variation, no gusts: steady, from where it was set.
            LLSkyWeather w;
            w.wind = 8.f;
            w.windDir = 90.f;
            w.windVar = 0.f;
            w.gust = 0.f;
            w.windDirVar = 0.f;
            plus::slwx::Effective e = {};
            convert(w, 1.0e9, e);
            SKYWEATHER_CHECK(approx(e.windSpeed, 8.f) && approx(e.windFromDeg, 90.f) && approx(e.windBaseSpeed, 8.f), "skyweather: steady wind");
            // Gusty: in range, the same for the same second, the base speed unchanged.
            w.windVar = 1.f;
            w.gust = 1.f;
            w.windDirVar = 180.f;
            plus::slwx::Effective a = {}, b = {};
            convert(w, 1.0e9 + 37.0, a);
            convert(w, 1.0e9 + 37.0, b);
            SKYWEATHER_CHECK(a.windSpeed == b.windSpeed && a.windFromDeg == b.windFromDeg, "skyweather: the same second, the same gust");
            SKYWEATHER_CHECK(a.windSpeed >= 0.f && a.windSpeed <= tag::WIND_SPEED_MAX && a.windFromDeg >= 0.f && a.windFromDeg < 360.001f,
                             "skyweather: the gusts stay in range");
            SKYWEATHER_CHECK(approx(a.windBaseSpeed, 8.f), "skyweather: the base speed has no gusts");
        }
        return true;
    }
}
