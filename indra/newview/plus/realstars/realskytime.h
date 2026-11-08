/**
 * @file realskytime.h
 * @brief Dates for the real night sky (research/177 §5, PlusSkyStarsTime): the UTC text the
 *        setting keeps, and the civil date of the box the photo tools show. Plain C++, header only.
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

#ifndef PLUS_REALSTARS_REALSKYTIME_H
#define PLUS_REALSTARS_REALSKYTIME_H

// The setting keeps "YYYY-MM-DDTHH:MM:SSZ" (UTC; LLControl has no F64 for a Unix time). The
// photo tools show and take "YYYY-MM-DD HH:MM" in the PC's own time zone (the conversion to and
// from local time is the viewer's, realstars.cpp: it needs the C library's time zone).
// Years 1800 .. 2200: the precession and the Sun's formulas are good for centuries around 2000.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace plus
{
namespace realstars
{
namespace timeutil
{
    constexpr int YEAR_MIN = 1800;
    constexpr int YEAR_MAX = 2200;

    struct Civil
    {
        int year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    };

    // Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
    inline std::int64_t daysFromCivil(int y, int m, int d)
    {
        y -= m <= 2;
        const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
        const std::int64_t yoe = y - era * 400;
        const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    // The date of a day count since 1970-01-01 (civil_from_days).
    inline void civilFromDays(std::int64_t z, int& y, int& m, int& d)
    {
        z += 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const std::int64_t doe = z - era * 146097;
        const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const std::int64_t mp = (5 * doy + 2) / 153;
        d = (int)(doy - (153 * mp + 2) / 5 + 1);
        m = (int)(mp < 10 ? mp + 3 : mp - 9);
        y = (int)(yoe + era * 400 + (m <= 2));
    }

    inline int daysInMonth(int y, int m)
    {
        static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (m < 1 || m > 12)
            return 0;
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return days[m - 1] + (m == 2 && leap ? 1 : 0);
    }

    // A date that exists, in the years the formulas are for.
    inline bool valid(const Civil& c)
    {
        return c.year >= YEAR_MIN && c.year <= YEAR_MAX && c.month >= 1 && c.month <= 12 && c.day >= 1
            && c.day <= daysInMonth(c.year, c.month) && c.hour >= 0 && c.hour <= 23 && c.minute >= 0
            && c.minute <= 59 && c.second >= 0 && c.second <= 59;
    }

    // Seconds since 1970-01-01T00:00:00Z of a UTC date.
    inline double unixOfUtc(const Civil& c)
    {
        return (double)daysFromCivil(c.year, c.month, c.day) * 86400.0 + c.hour * 3600.0 + c.minute * 60.0 + c.second;
    }

    inline Civil utcOfUnix(double unixSeconds)
    {
        const std::int64_t s = (std::int64_t)std::floor(unixSeconds);
        std::int64_t days = s / 86400;
        std::int64_t rest = s % 86400;
        if (rest < 0)
        {
            rest += 86400;
            --days;
        }
        Civil c;
        civilFromDays(days, c.year, c.month, c.day);
        c.hour = (int)(rest / 3600);
        c.minute = (int)(rest % 3600 / 60);
        c.second = (int)(rest % 60);
        return c;
    }

    namespace detail
    {
        // Reads up to max digits at text[at]; at least one. Moves at past them.
        inline bool number(const std::string& text, size_t& at, int max, int& out)
        {
            int n = 0, value = 0;
            while (at < text.size() && n < max && text[at] >= '0' && text[at] <= '9')
            {
                value = value * 10 + (text[at] - '0');
                ++at;
                ++n;
            }
            out = value;
            return n > 0;
        }

        inline void spaces(const std::string& text, size_t& at)
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
                ++at;
        }

        inline bool oneOf(const std::string& text, size_t& at, const char* chars)
        {
            if (at >= text.size())
                return false;
            for (const char* c = chars; *c; ++c)
                if (text[at] == *c)
                {
                    ++at;
                    return true;
                }
            return false;
        }
    }

    // "YYYY-MM-DD HH:MM" as the photo tools take it (also "/" or "." between the date's parts,
    // "T" or spaces before the time, the seconds, or no time at all: 00:00). Leading and
    // trailing spaces are let be. false when it is not a date that exists.
    inline bool parseCivil(const std::string& text, Civil& out)
    {
        Civil c;
        size_t at = 0;
        detail::spaces(text, at);
        if (!detail::number(text, at, 4, c.year) || !detail::oneOf(text, at, "-/.")
            || !detail::number(text, at, 2, c.month) || !detail::oneOf(text, at, "-/.")
            || !detail::number(text, at, 2, c.day))
            return false;
        const size_t date_end = at;
        detail::spaces(text, at);
        if (at < text.size() && (text[at] == 'T' || text[at] == 't'))
            ++at;
        detail::spaces(text, at);
        if (at < text.size() && at > date_end)
        {
            if (!detail::number(text, at, 2, c.hour) || !detail::oneOf(text, at, ":")
                || !detail::number(text, at, 2, c.minute))
                return false;
            if (detail::oneOf(text, at, ":") && !detail::number(text, at, 2, c.second))
                return false;
        }
        detail::spaces(text, at);
        if (at != text.size() || !valid(c))
            return false;
        out = c;
        return true;
    }

    // The setting's text: "YYYY-MM-DDTHH:MM:SSZ" (the "Z" and the seconds may be left out on
    // reading). false for anything else, the empty text included ("not given").
    inline bool parseUtc(const std::string& text, double& unixSeconds)
    {
        std::string body = text;
        while (!body.empty() && (body.back() == ' ' || body.back() == 'Z' || body.back() == 'z'))
            body.pop_back();
        if (body.find('T') == std::string::npos && body.find('t') == std::string::npos)
            return false; // a date with no time is not what the viewer writes
        Civil c;
        if (!parseCivil(body, c))
            return false;
        unixSeconds = unixOfUtc(c);
        return true;
    }

    inline std::string formatUtc(const Civil& c)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", c.year, c.month, c.day, c.hour, c.minute, c.second);
        return text;
    }

    // The photo tools' text (no seconds).
    inline std::string formatCivil(const Civil& c)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", c.year, c.month, c.day, c.hour, c.minute);
        return text;
    }
} // namespace timeutil
} // namespace realstars
} // namespace plus

#endif // PLUS_REALSTARS_REALSKYTIME_H
