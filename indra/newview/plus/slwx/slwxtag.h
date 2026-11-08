/**
 * @file slwxtag.h
 * @brief The [slwx ...] tag of a parcel's description (the same format as
 *        Slipstream's, research/229 section 2): text <-> values. Plain
 *        functions on plain values; nothing here knows the viewer.
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

#ifndef PLUS_SLWX_SLWXTAG_H
#define PLUS_SLWX_SLWXTAG_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace plus::slwx::tag
{
    // The parcel description's limit (bytes); only this much is read.
    constexpr size_t DESCRIPTION_LIMIT = 255;

    // The keys' ranges and defaults (research/229 section 2).
    constexpr float RAIN_MM_MAX = 200.f;       // mm/h
    constexpr float SNOW_CM_MAX = 360.f;       // cm/h
    constexpr float WIND_SPEED_MAX = 40.f;     // m/s
    constexpr float WIND_DIR_DEFAULT = 225.f;
    constexpr float WIND_SPEED_DEFAULT = 3.f;
    constexpr float PVAR_DEFAULT = 0.35f;
    constexpr float VAR_DEFAULT = 0.3f;
    constexpr float GUST_DEFAULT = 0.5f;
    constexpr float TRANS_DEFAULT = 30.f;      // s
    constexpr float TRANS_MAX = 600.f;
    constexpr float DEPTH_MAX = 10.f;          // m
    constexpr float RATE_MAX = 100.f;          // m/h

    // What a tag says. A key that was not written has its default (and, where
    // the default depends on another key, a has_* flag that is false).
    struct Params
    {
        float rain = 0.f;              // 0..1
        float snow = 0.f;              // 0..1
        bool  has_rainmm = false;
        float rainmm = 0.f;            // 0..200 mm/h, ahead of rain when written
        bool  has_snowcm = false;
        float snowcm = 0.f;            // 0..360 cm/h, ahead of snow when written
        float pvar = PVAR_DEFAULT;     // 0..1
        float wind_from = WIND_DIR_DEFAULT;    // degrees, 0 north, the way it blows from
        float wind_speed = WIND_SPEED_DEFAULT; // m/s
        float var = VAR_DEFAULT;       // 0..1
        float gust = GUST_DEFAULT;     // 0..1
        float wdirvar = 0.f;           // 0..180 degrees
        float storm = 0.f;             // 0..1
        bool  has_fog = false;
        float fog = 0.f;               // 0..1
        bool  has_cloud = false;
        float cloud = 0.f;             // 0..1
        float trans = TRANS_DEFAULT;   // 0..600 s
        bool  has_seed = false;
        uint32_t seed = 0;
        bool  has_depth = false;
        float depth = 0.f;             // 0..10 m
        float rate = 0.f;              // -100..100 m/h
        bool  has_since = false;
        int64_t since = 0;             // UTC Unix seconds
    };

    struct ParseResult
    {
        bool found = false;            // a [slwx ...] tag is there
        bool off = false;              // the bare "off" / "clear": no weather (the tag is there, the rest is not used)
        int  bad_items = 0;            // items dropped for being broken (not the unknown keys)
        Params params;
    };

    // The tag's place in the text: [begin, end) with end after the "]". False
    // when there is none. Only the first DESCRIPTION_LIMIT bytes are looked at.
    bool find(const std::string& description, size_t& begin, size_t& end);

    // A "[slwx" that has no "]" after it (within the limit): the text is broken,
    // and a tag added after it would be swallowed by it. replaceIn() refuses to
    // add a tag then (false); the caller may tell the owner to mend the description.
    bool hasUnclosedOpen(const std::string& description);

    // Reads the first tag of a description. Bounded: at most DESCRIPTION_LIMIT
    // bytes are looked at.
    ParseResult parse(const std::string& description);

    // The tag as text, with only the keys that differ from the defaults
    // (rain and snow are written together with rainmm and snowcm). params.off
    // is not a field: "[slwx off]" is writeOff().
    std::string write(const Params& params);
    std::string writeOff();

    // The description with its tag (the first) replaced by `tag`, or `tag`
    // added at the end when there is none (a space between). `tag` empty:
    // the tag is taken out. False (description untouched) when the result
    // would be longer than DESCRIPTION_LIMIT.
    bool replaceIn(std::string& description, const std::string& tag);

    // Rain 0..1 and the rate in mm/h are two views of one scale: [0, 1/3] is
    // [0, 2.5], [1/3, 2/3] is [2.5, 10], [2/3, 1] is [10, 50] (snow: cm/h,
    // [0, 1], [1, 4], [4, 20]). What the tag says by `rain` alone is read
    // through it, and the writer sets rainmm from rain through it.
    float rainToMm(float rain);
    float mmToRain(float mm);
    float snowToCm(float snow);
    float cmToSnow(float cm);

    // The level of a fall (0 none, 1 light, 2 medium, 3 heavy) from its rate.
    int rainLevelOfMm(float mm);
    int snowLevelOfCm(float cm);

    // The rate the tag asks for (rainmm if written, else rain through the scale).
    float rainMm(const Params& params);
    float snowCm(const Params& params);

    // Checks the above with Slipstream's examples, the shortest tag, broken
    // items, out-of-range values, off, 255+ bytes, upper case, several tags,
    // and writing then reading back. Returns true when all hold; on false,
    // `report` names the first that did not. Debug builds run it at start.
    bool selftest(std::string& report);
}

#endif // PLUS_SLWX_SLWXTAG_H
