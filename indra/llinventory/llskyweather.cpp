/**
 * @file llskyweather.cpp
 * @brief Weather values stored inside a sky setting (research/371).
 *
 * <plus> sky-weather: new file (Charis). </plus>
 */
#include "linden_common.h"

#include "llskyweather.h"

#include <cmath>

namespace
{
    // One row per stored key: name, member, lower and upper bound.
    struct Field
    {
        const char* name;
        F32 LLSkyWeather::* member;
        F32 lo;
        F32 hi;
    };

    const Field sFields[] =
    {
        { "rain",      &LLSkyWeather::rain,       0.f,   1.f },
        { "snow",      &LLSkyWeather::snow,       0.f,   1.f },
        { "wind",      &LLSkyWeather::wind,       0.f,  40.f },
        { "wind_dir",  &LLSkyWeather::windDir,    0.f, 360.f },
        { "wind_var",  &LLSkyWeather::windVar,    0.f,   1.f },
        { "gust",      &LLSkyWeather::gust,       0.f,   1.f },
        { "wdirvar",   &LLSkyWeather::windDirVar, 0.f, 180.f },
        { "pvar",      &LLSkyWeather::pvar,       0.f,   1.f },
        { "storm",     &LLSkyWeather::storm,      0.f,   1.f },
        { "ground_fog",&LLSkyWeather::groundFog,  0.f,   1.f },
        { "wet",       &LLSkyWeather::wet,        0.f,   1.f },
        { "snow_cover",&LLSkyWeather::snowCover,  0.f,   1.f },
        { "rainbow",   &LLSkyWeather::rainbow,    0.f,   1.f },
    };

    // Shortest-way blend of two angles in degrees; the result is in 0..360.
    F32 lerpAngle(F32 a, F32 b, F32 t)
    {
        F32 d = std::fmod(b - a, 360.f);
        if (d > 180.f)
        {
            d -= 360.f;
        }
        else if (d < -180.f)
        {
            d += 360.f;
        }
        F32 r = std::fmod(a + d * t, 360.f);
        if (r < 0.f)
        {
            r += 360.f;
        }
        // adding 360 to a tiny negative value can round up to 360 in F32
        return r >= 360.f ? 0.f : r;
    }

    // Clamps in double first so that huge values do not overflow F32. The wind direction wraps
    // round 360 (370 is 10, -30 is 330); every other value is clamped to its range.
    F32 clampValue(const Field& f, F64 d)
    {
        if (f.member == &LLSkyWeather::windDir)
        {
            d = std::fmod(d, 360.0);
            if (d < 0.0)
            {
                d += 360.0;
            }
            const F32 r = (F32)d;
            // a value just under 360 can round up to 360 in F32; 360 and 0 are the same direction
            return r >= 360.f ? 0.f : r;
        }
        return (F32)(d < f.lo ? f.lo : (d > f.hi ? f.hi : d));
    }
}

LLSkyWeather LLSkyWeather::fromLLSD(const LLSD& data)
{
    LLSkyWeather result;
    if (!data.isMap() || data.size() > MAX_KEYS)
    {
        return result;
    }
    for (const Field& f : sFields)
    {
        if (!data.has(f.name))
        {
            continue;
        }
        const LLSD& v = data[f.name];
        if (v.type() != LLSD::TypeReal && v.type() != LLSD::TypeInteger)
        {
            continue;
        }
        const F64 d = v.asReal();
        if (!std::isfinite(d))
        {
            continue;
        }
        result.*(f.member) = clampValue(f, d);
    }
    if (data.has("land") && data["land"].type() == LLSD::TypeBoolean)
    {
        result.fromLand = data["land"].asBoolean();
    }
    return result;
}

LLSkyWeather LLSkyWeather::clamped() const
{
    LLSkyWeather result = *this;
    for (const Field& f : sFields)
    {
        const F64 d = this->*(f.member);
        // NaN / infinity: back to the default
        result.*(f.member) = std::isfinite(d) ? clampValue(f, d) : LLSkyWeather().*(f.member);
    }
    return result;
}

LLSD LLSkyWeather::toLLSD() const
{
    LLSD result = LLSD::emptyMap();
    const LLSkyWeather dflt;
    for (const Field& f : sFields)
    {
        if (this->*(f.member) != dflt.*(f.member))
        {
            result[f.name] = (LLSD::Real)(this->*(f.member));
        }
    }
    if (fromLand)
    {
        result["land"] = true;
    }
    return result;
}

LLSkyWeather LLSkyWeather::lerp(const LLSkyWeather& a, const LLSkyWeather& b, F32 t)
{
    LLSkyWeather result;
    for (const Field& f : sFields)
    {
        const F32 x = a.*(f.member);
        const F32 y = b.*(f.member);
        result.*(f.member) = x + (y - x) * t;
    }
    result.windDir = lerpAngle(a.windDir, b.windDir, t);
    // in between, part of it is other people's: all of it is; the ends are the ends
    result.fromLand = (t <= 0.f) ? a.fromLand : ((t >= 1.f) ? b.fromLand : (a.fromLand || b.fromLand));
    return result;
}

void LLSkyWeather::stripFromDayCycle(LLSD& day)
{
    if (!day.isMap() || !day.has("frames") || !day["frames"].isMap())
    {
        return;
    }
    LLSD& frames = day["frames"];
    for (LLSD::map_iterator it = frames.beginMap(); it != frames.endMap(); ++it)
    {
        if (it->second.isMap() && it->second.has("weather"))
        {
            it->second.erase("weather");
        }
    }
}

bool LLSkyWeather::operator==(const LLSkyWeather& other) const
{
    if (fromLand != other.fromLand)
    {
        return false;
    }
    for (const Field& f : sFields)
    {
        if (this->*(f.member) != other.*(f.member))
        {
            return false;
        }
    }
    return true;
}
