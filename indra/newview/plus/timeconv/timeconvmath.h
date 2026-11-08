/**
 * @file timeconvmath.h
 * @brief The arithmetic of the time converter: a moment is minutes since 1970-01-01 00:00 UTC,
 *        a zone turns it into the wall clock (year, month, day, hour, minute) and back.
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

// Header only and free of viewer headers, so timeconv_test.cpp can build it alone (research/173).
// The operating system's time zone data is not used: the summer-time rules of the zones in
// ZONES are written out here (Windows' own tables stop at 2037 and their names differ by language).

#ifndef PLUS_TIMECONV_TIMECONVMATH_H
#define PLUS_TIMECONV_TIMECONVMATH_H

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace plus::timeconv
{
    constexpr int YEAR_MIN = 1970;
    constexpr int YEAR_MAX = 2100;

    using Minutes = std::int64_t; // since 1970-01-01 00:00 UTC

    struct Civil // a wall clock
    {
        int year = 1970, month = 1, day = 1, hour = 0, minute = 0;
        bool operator==(const Civil& o) const
        {
            return year == o.year && month == o.month && day == o.day && hour == o.hour && minute == o.minute;
        }
    };

    enum class Rule { None, US, EU, AU, NZ };

    struct Zone
    {
        const char* id;        // also the strings zone_<id> of the window
        int         stdMin;    // offset from UTC without summer time, in minutes
        Rule        rule;
        const char* stdAbbr;   // "" : written as UTC+9
        const char* dstAbbr;
    };

    // Index 0 is SL's own time and 1 UTC: the first two rows of the window.
    inline const std::vector<Zone>& zones()
    {
        static const std::vector<Zone> ZONES = {
            { "sl",    -480, Rule::US,   "PST",  "PDT"  },
            { "utc",      0, Rule::None, "UTC",  "UTC"  },
            { "jst",    540, Rule::None, "JST",  "JST"  },
            { "kst",    540, Rule::None, "KST",  "KST"  },
            { "china",  480, Rule::None, "CST",  "CST"  },
            { "india",  330, Rule::None, "IST",  "IST"  },
            { "uk",       0, Rule::EU,   "GMT",  "BST"  },
            { "cet",     60, Rule::EU,   "CET",  "CEST" },
            { "et",    -300, Rule::US,   "EST",  "EDT"  },
            { "ct",    -360, Rule::US,   "CST",  "CDT"  },
            { "mt",    -420, Rule::US,   "MST",  "MDT"  },
            { "hst",   -600, Rule::None, "HST",  "HST"  },
            { "aet",    600, Rule::AU,   "AEST", "AEDT" },
            { "nz",     720, Rule::NZ,   "NZST", "NZDT" },
        };
        return ZONES;
    }

    constexpr int ZONE_SL = 0, ZONE_UTC = 1, ZONE_JST = 2;

    // A zone with no summer time and a given offset: "this PC" and UTC+N. Abbreviation empty.
    inline Zone fixedZone(const char* id, int offsetMin)
    {
        return Zone{ id, offsetMin, Rule::None, "", "" };
    }

    // ---- calendar ---------------------------------------------------------------------------

    inline bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

    inline int daysInMonth(int y, int m)
    {
        static const int D[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        return (m == 2 && isLeap(y)) ? 29 : D[m - 1];
    }

    // Days since 1970-01-01 of a calendar date (proleptic Gregorian).
    inline std::int64_t daysFromCivil(int y, int m, int d)
    {
        y -= m <= 2;
        const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int yoe = int(y - era * 400);
        const int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    inline void civilFromDays(std::int64_t z, int& y, int& m, int& d)
    {
        z += 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const int doe = int(z - era * 146097);
        const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const int mp  = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = int(yoe + era * 400) + (m <= 2);
    }

    // 0 = Sunday.
    inline int weekdayOfDays(std::int64_t days) { return int(((days + 4) % 7 + 7) % 7); }

    // Day of the month of the n-th Sunday (n from 1).
    inline int nthSunday(int y, int m, int n)
    {
        const int wd = weekdayOfDays(daysFromCivil(y, m, 1));
        return 1 + (7 - wd) % 7 + 7 * (n - 1);
    }

    inline int lastSunday(int y, int m)
    {
        const int dim = daysInMonth(y, m);
        return dim - weekdayOfDays(daysFromCivil(y, m, dim));
    }

    inline bool validCivil(const Civil& c)
    {
        if (c.year < YEAR_MIN || c.year > YEAR_MAX) return false;
        if (c.month < 1 || c.month > 12) return false;
        if (c.day < 1 || c.day > daysInMonth(c.year, c.month)) return false;
        return c.hour >= 0 && c.hour <= 23 && c.minute >= 0 && c.minute <= 59;
    }

    // A wall clock as minutes (no zone): the same scale as a moment, but local.
    inline Minutes wallMinutes(const Civil& c)
    {
        return daysFromCivil(c.year, c.month, c.day) * 1440 + c.hour * 60 + c.minute;
    }

    inline Civil civilFromWallMinutes(Minutes w)
    {
        std::int64_t days = w / 1440;
        int rem = int(w % 1440);
        if (rem < 0) { rem += 1440; --days; }
        Civil c;
        civilFromDays(days, c.year, c.month, c.day);
        c.hour = rem / 60;
        c.minute = rem % 60;
        return c;
    }

    // ---- summer time ------------------------------------------------------------------------

    // Is summer time on at this moment in this zone?
    inline bool isDst(const Zone& z, Minutes utc)
    {
        if (z.rule == Rule::None) return false;
        const int sm = z.stdMin; // standard offset
        const int y = civilFromWallMinutes(utc + sm).year; // the year of the local date
        Minutes start = 0, end = 0;
        bool southern = false;
        switch (z.rule)
        {
        case Rule::US:
            start = daysFromCivil(y, 3, nthSunday(y, 3, 2)) * 1440 + 120 - sm;
            end   = daysFromCivil(y, 11, nthSunday(y, 11, 1)) * 1440 + 120 - (sm + 60);
            break;
        case Rule::EU: // 01:00 UTC both ways
            start = daysFromCivil(y, 3, lastSunday(y, 3)) * 1440 + 60;
            end   = daysFromCivil(y, 10, lastSunday(y, 10)) * 1440 + 60;
            break;
        case Rule::AU: // 02:00 standard time, first Sunday of October to 03:00 summer time, first Sunday of April
            southern = true;
            start = daysFromCivil(y, 10, nthSunday(y, 10, 1)) * 1440 + 120 - sm;
            end   = daysFromCivil(y, 4, nthSunday(y, 4, 1)) * 1440 + 180 - (sm + 60);
            break;
        case Rule::NZ: // last Sunday of September to first Sunday of April
            southern = true;
            start = daysFromCivil(y, 9, lastSunday(y, 9)) * 1440 + 120 - sm;
            end   = daysFromCivil(y, 4, nthSunday(y, 4, 1)) * 1440 + 180 - (sm + 60);
            break;
        case Rule::None:
            break;
        }
        return southern ? (utc >= start || utc < end) : (utc >= start && utc < end);
    }

    inline int offsetMinutes(const Zone& z, Minutes utc) { return z.stdMin + (isDst(z, utc) ? 60 : 0); }

    inline Civil utcToLocal(const Zone& z, Minutes utc) { return civilFromWallMinutes(utc + offsetMinutes(z, utc)); }

    enum class Note { None, Gap, Overlap };

    // The moment of a wall clock in a zone. The clock that does not exist (the hour skipped when
    // summer time starts) is taken as an hour later (Note::Gap: 02:30 is 03:30); the clock that
    // comes twice is taken on the summer-time side, the first of the two (Note::Overlap).
    inline Minutes localToUtc(const Zone& z, const Civil& c, Note* note = nullptr)
    {
        const Minutes w = wallMinutes(c);
        const Minutes asStd = w - z.stdMin;        // read as standard time
        const Minutes asDst = w - z.stdMin - 60;   // read as summer time
        const bool stdOk = !isDst(z, asStd);
        const bool dstOk = isDst(z, asDst);
        if (note) *note = Note::None;
        if (stdOk && dstOk) { if (note) *note = Note::Overlap; return asDst; }
        if (dstOk) return asDst;
        if (stdOk) return asStd;
        if (note) *note = Note::Gap;
        return asStd; // 02:30 does not exist: as standard time it is the same moment as 03:30 summer time
    }

    // ---- stepping a field -------------------------------------------------------------------

    // Adds minutes to a wall clock (days, hours, minutes carry into each other).
    inline Civil addWallMinutes(const Civil& c, std::int64_t delta) { return civilFromWallMinutes(wallMinutes(c) + delta); }

    // Adds months; a day past the end of the month becomes its last day.
    inline Civil addMonths(const Civil& c, int delta)
    {
        Civil r = c;
        const int idx = c.year * 12 + (c.month - 1) + delta;
        r.year = idx / 12;
        r.month = idx % 12 + 1;
        const int dim = daysInMonth(r.year, r.month);
        if (r.day > dim) r.day = dim;
        return r;
    }

    // ---- text -------------------------------------------------------------------------------

    // "UTC+9", "UTC-7", "UTC+5:30"
    inline std::string offsetText(int offsetMin)
    {
        std::string s = "UTC";
        if (offsetMin == 0) return s;
        const int a = offsetMin < 0 ? -offsetMin : offsetMin;
        s += offsetMin < 0 ? '-' : '+';
        s += std::to_string(a / 60);
        if (a % 60)
        {
            s += ':';
            if (a % 60 < 10) s += '0';
            s += std::to_string(a % 60);
        }
        return s;
    }

    inline std::string abbrText(const Zone& z, bool dst)
    {
        const char* a = dst ? z.dstAbbr : z.stdAbbr;
        return (a && *a) ? std::string(a) : std::string();
    }

    inline std::string two(int v)
    {
        std::string s = std::to_string(v);
        return v < 10 ? "0" + s : s;
    }

    // "2026-10-24 23:00"
    inline std::string civilText(const Civil& c)
    {
        return std::to_string(c.year) + "-" + two(c.month) + "-" + two(c.day) + " " + two(c.hour) + ":" + two(c.minute);
    }

    // Reads a date and time pasted as text: "2026/10/24 19:30", "2026-10-24 7pm", "2026年10月24日 19時30分",
    // "20261024 1930", "19:30" (time only), "7pm". Numbers only are looked at; a number of four
    // digits, then two for the month and the day, make the date; hour and minute follow.
    struct Parsed
    {
        bool hasDate = false, hasTime = false;
        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
        int numbers = 0;     // how many numbers the text had
        bool ampm = false;   // had am / pm
    };

    inline bool parseDateTime(const std::string& text, Parsed& out)
    {
        struct Num { int value; int digits; };
        std::vector<Num> nums;
        int pm = -1; // 1 pm, 0 am
        for (size_t i = 0; i < text.size();)
        {
            const unsigned char ch = (unsigned char)text[i];
            if (std::isdigit(ch))
            {
                int v = 0, n = 0;
                while (i < text.size() && std::isdigit((unsigned char)text[i]) && n < 9)
                {
                    v = v * 10 + (text[i] - '0');
                    ++i;
                    ++n;
                }
                nums.push_back({ v, n });
            }
            else
            {
                const char l = (char)std::tolower(ch);
                if ((l == 'p' || l == 'a') && i + 1 < text.size() && std::tolower((unsigned char)text[i + 1]) == 'm' && !nums.empty())
                    pm = (l == 'p') ? 1 : 0;
                ++i;
            }
        }
        out = Parsed();
        out.numbers = int(nums.size());
        out.ampm = pm >= 0;
        size_t k = 0;
        if (!nums.empty() && nums[0].digits == 8) // 20261024
        {
            out.year = nums[0].value / 10000;
            out.month = nums[0].value / 100 % 100;
            out.day = nums[0].value % 100;
            out.hasDate = true;
            k = 1;
        }
        else if (nums.size() >= 3 && nums[0].digits == 4)
        {
            out.year = nums[0].value;
            out.month = nums[1].value;
            out.day = nums[2].value;
            out.hasDate = true;
            k = 3;
        }
        if (k < nums.size()) // the time
        {
            if (nums[k].digits >= 3 && nums[k].digits <= 4) // 1930
            {
                out.hour = nums[k].value / 100;
                out.minute = nums[k].value % 100;
                ++k;
            }
            else
            {
                out.hour = nums[k].value;
                ++k;
                if (k < nums.size() && nums[k].digits <= 2) out.minute = nums[k].value;
            }
            if (pm == 1 && out.hour >= 1 && out.hour <= 11) out.hour += 12;
            if (pm == 0 && out.hour == 12) out.hour = 0;
            out.hasTime = true;
        }
        if (!out.hasDate && !out.hasTime) return false;
        if (out.hasTime && (out.hour > 23 || out.minute > 59)) return false;
        if (out.hasDate)
        {
            Civil c;
            c.year = out.year;
            c.month = out.month;
            c.day = out.day;
            if (!validCivil(c)) return false;
        }
        return true;
    }
}

#endif // PLUS_TIMECONV_TIMECONVMATH_H
