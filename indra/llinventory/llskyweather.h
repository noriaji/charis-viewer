/**
 * @file llskyweather.h
 * @brief Weather values stored inside a sky setting (research/371).
 *
 * <plus> sky-weather: new file (Charis). The data is pure, with no rendering dependency
 * (llskyweather.cpp); marking the weather of a sky or a day cycle is in llskyweathermark.cpp.
 * The sky setting LLSD carries one optional map "weather"; keys use the same
 * names and ranges as the slwx parcel tags. Reading never fails: values are
 * clamped (wind direction wraps round), NaN / wrong types / unknown keys are dropped.
 * setWeather() clamps the same way, so a saved value reads back unchanged.
 * </plus>
 */
#ifndef LL_LLSKYWEATHER_H
#define LL_LLSKYWEATHER_H

#include "stdtypes.h"
#include "llsd.h"

#include <memory>

class LLSettingsBase;

struct LLSkyWeather
{
    F32 rain = 0.f;         // 0..1
    F32 snow = 0.f;         // 0..1
    F32 wind = 3.f;         // 0..40 m/s
    F32 windDir = 225.f;    // 0..360 degrees, the direction the wind comes from (0 = north)
    F32 windVar = 0.3f;     // 0..1
    F32 gust = 0.5f;        // 0..1
    F32 windDirVar = 0.f;   // 0..180 degrees
    F32 pvar = 0.35f;       // 0..1
    F32 storm = 0.f;        // 0..1 (key reserved, not used yet)
    F32 groundFog = 0.f;    // 0..1
    F32 wet = 0.f;          // 0..1
    F32 snowCover = 0.f;    // 0..1
    F32 rainbow = 0.f;      // 0..1

    // Other people's: the weather came with the land's environment (the parcel's, the region's or a
    // script's) and was marked when it arrived. It is saved with the weather (key "land"), is kept
    // by every copy and blend (strictly between two weathers one of which is marked, it is marked), and is
    // dropped only when a user writes a new weather (setWeather with a value that has it false).
    // The viewer uses a marked weather only when the user allows other people's weather.
    bool fromLand = false;

    // Upper limit of keys in the incoming map; a bigger map is treated as malformed (defaults).
    static const S32 MAX_KEYS = 32;

    // Returns a copy with every value in range (the wind direction wraps round 360, the rest is clamped).
    LLSkyWeather clamped() const;
    // Clamps to the ranges, drops NaN / wrong types / unknown keys. Never fails.
    static LLSkyWeather fromLLSD(const LLSD& data);
    // Writes only the keys that differ from the defaults (an all-default value gives an empty map).
    LLSD toLLSD() const;
    // t = 0 gives a, t = 1 gives b. windDir takes the shortest way round.
    static LLSkyWeather lerp(const LLSkyWeather& a, const LLSkyWeather& b, F32 t);

    // Removes "weather" from every sky frame of a day cycle LLSD (the copy the caller owns;
    // LLSD is copy-on-write, so the original setting is not touched). Used before a day
    // cycle goes to the server, which may refuse keys it does not know.
    static void stripFromDayCycle(LLSD& day);

    // Marks the weather of a sky, or of every sky keyframe of a day cycle (the sky tracks), as
    // other people's (fromLand = true). Already marked ones and ones with no weather are left alone.
    // The viewer calls it when the land's (parcel, region, script) environment arrives.
    static void markFromLand(const std::shared_ptr<LLSettingsBase>& settings);
    // The same for a sky as LLSD (a script's sky): the weather is normalised (read and written
    // back, so that a map of too many keys cannot lose the mark) and marked. No weather map: untouched.
    static void markSkyLLSDFromLand(LLSD& sky);
    // Whether a sky, or any sky keyframe of a day cycle, has a weather marked fromLand.
    static bool isMarkedFromLand(const std::shared_ptr<LLSettingsBase>& settings);
    // Whether a sky, or any sky keyframe of a day cycle, has a weather at all (marked or not).
    static bool hasAnyWeather(const std::shared_ptr<LLSettingsBase>& settings);

    bool operator==(const LLSkyWeather& other) const;
    bool operator!=(const LLSkyWeather& other) const { return !(*this == other); }
};

#endif // LL_LLSKYWEATHER_H
