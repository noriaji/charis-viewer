/**
 * @file llskyweathermark.cpp
 * @brief Marking the weather of a sky or a day cycle as other people's (research/371).
 *
 * <plus> sky-weather: new file (Charis). Apart from llskyweather.cpp, which is pure data, because it
 * reaches into the sky and day cycle classes. </plus>
 */
#include "linden_common.h"

#include "llskyweather.h"

#include "llsettingsdaycycle.h"
#include "llsettingssky.h"

namespace
{
    // Calls `fn` on every sky of `settings`: the sky itself, or the sky keyframes of a day cycle.
    template <typename Fn>
    void forEachSky(const std::shared_ptr<LLSettingsBase>& settings, Fn fn)
    {
        if (!settings)
        {
            return;
        }
        if (const LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(settings))
        {
            fn(sky);
        }
        else if (const LLSettingsDay::ptr_t day = std::dynamic_pointer_cast<LLSettingsDay>(settings))
        {
            // every sky track (the water track has none)
            for (U32 track = LLSettingsDay::TRACK_GROUND_LEVEL; track < LLSettingsDay::TRACK_MAX; ++track)
            {
                for (const auto& frame : day->getCycleTrackConst(track))
                {
                    if (const LLSettingsSky::ptr_t keyframe = std::dynamic_pointer_cast<LLSettingsSky>(frame.second))
                    {
                        fn(keyframe);
                    }
                }
            }
        }
    }
}

void LLSkyWeather::markFromLand(const std::shared_ptr<LLSettingsBase>& settings)
{
    forEachSky(settings, [](const LLSettingsSky::ptr_t& sky)
    {
        if (sky->hasWeather() && !sky->getWeather().fromLand)
        {
            LLSkyWeather w = sky->getWeather();
            w.fromLand = true;
            sky->setWeather(w);
        }
    });
}

void LLSkyWeather::markSkyLLSDFromLand(LLSD& sky)
{
    if (sky.isMap() && sky.has("weather") && sky["weather"].isMap())
    {
        LLSkyWeather w = fromLLSD(sky["weather"]);
        w.fromLand = true;
        sky["weather"] = w.toLLSD();
    }
}

bool LLSkyWeather::isMarkedFromLand(const std::shared_ptr<LLSettingsBase>& settings)
{
    bool marked = false;
    forEachSky(settings, [&marked](const LLSettingsSky::ptr_t& sky)
    {
        marked = marked || (sky->hasWeather() && sky->getWeather().fromLand);
    });
    return marked;
}

bool LLSkyWeather::hasAnyWeather(const std::shared_ptr<LLSettingsBase>& settings)
{
    bool any = false;
    forEachSky(settings, [&any](const LLSettingsSky::ptr_t& sky)
    {
        any = any || sky->hasWeather();
    });
    return any;
}
