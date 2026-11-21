/**
 * @file timeconvshared.h
 * @brief What the two time converter windows share: the XUI one (timeconvfloater.cpp, frozen) and the
 *        RmlUi one (timeconvrml.cpp): setting names, the zone of a row (the clock of this PC, a
 *        named place, UTC+N) and the text copy. Moved here unchanged from timeconvfloater.cpp.
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

#ifndef PLUS_TIMECONVSHARED_H
#define PLUS_TIMECONVSHARED_H

#include "timeconvmath.h"

#include "llclipboard.h"
#include "llstring.h"

#include <cstdlib>
#include <ctime>
#include <string>

namespace plus::timeconv
{
    inline constexpr char SETTING_ZONE2[] = "PlusTimeConvZone2";
    inline constexpr char SETTING_ZONE3[] = "PlusTimeConvZone3";
    inline constexpr char SETTING_ROWS[]  = "PlusTimeConvRows";
    inline constexpr char SETTING_TICK[]  = "PlusTimeConvTick";

    inline constexpr int MAX_ROWS = 4;
    enum Field { F_YEAR, F_MONTH, F_DAY, F_HOUR, F_MINUTE, F_COUNT };
    inline constexpr const char* FIELD_NAMES[F_COUNT] = { "y", "mo", "d", "h", "mi" };

    // UTC-12 .. UTC+14 in whole hours, for a place that has no name of its own.
    inline constexpr int FIXED_HOURS_MIN = -12, FIXED_HOURS_MAX = 14;

    // ---- the zone of a row ---------------------------------------------------------------

    struct RowZone
    {
        bool pc = false;               // the clock of this PC (its offset is asked of the OS)
        Zone zone = fixedZone("utc", 0);
    };

    // The offset of this PC from UTC at a moment, in minutes (summer time of the PC included).
    inline int pcOffsetAt(Minutes utc)
    {
        const std::time_t t = static_cast<std::time_t>(utc * 60);
        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &t) != 0)
        {
            return 0;
        }
#else
        if (!localtime_r(&t, &local))
        {
            return 0;
        }
#endif
        Civil c;
        c.year   = local.tm_year + 1900;
        c.month  = local.tm_mon + 1;
        c.day    = local.tm_mday;
        c.hour   = local.tm_hour;
        c.minute = local.tm_min;
        return static_cast<int>(wallMinutes(c) - utc);
    }

    inline Civil toLocal(const RowZone& rz, Minutes utc)
    {
        return rz.pc ? civilFromWallMinutes(utc + pcOffsetAt(utc)) : utcToLocal(rz.zone, utc);
    }

    inline Minutes toUtc(const RowZone& rz, const Civil& c, Note* note)
    {
        if (note)
        {
            *note = Note::None;
        }
        if (!rz.pc)
        {
            return localToUtc(rz.zone, c, note);
        }
        // The PC's offset depends on the moment: ask at the guess, then again at the answer.
        const Minutes w = wallMinutes(c);
        const Minutes guess = w - pcOffsetAt(w);
        return w - pcOffsetAt(guess);
    }

    inline int offsetAt(const RowZone& rz, Minutes utc)
    {
        return rz.pc ? pcOffsetAt(utc) : offsetMinutes(rz.zone, utc);
    }

    // "PDT", "JST", "UTC+9" : what the clock is called, for the copied text.
    inline std::string zoneName(const RowZone& rz, Minutes utc)
    {
        if (!rz.pc)
        {
            const std::string abbr = abbrText(rz.zone, isDst(rz.zone, utc));
            if (!abbr.empty())
            {
                return abbr;
            }
        }
        return offsetText(offsetAt(rz, utc));
    }

    // The id of a combo item: "pc", a name of zones() or "u<minutes>" (UTC+N).
    inline bool resolveZone(const std::string& id, RowZone& out)
    {
        out = RowZone();
        if (id == "pc")
        {
            out.pc = true;
            return true;
        }
        if (id.size() > 1 && id[0] == 'u')
        {
            const int minutes = std::atoi(id.c_str() + 1);
            if (minutes < FIXED_HOURS_MIN * 60 || minutes > FIXED_HOURS_MAX * 60)
            {
                return false;
            }
            out.zone = fixedZone("fixed", minutes);
            return true;
        }
        for (const Zone& z : zones())
        {
            if (id == z.id)
            {
                out.zone = z;
                return true;
            }
        }
        return false;
    }

    inline void copyText(const std::string& text)
    {
        const LLWString w = utf8str_to_wstring(text);
        LLClipboard::instance().copyToClipboard(w, 0, static_cast<S32>(w.length()));
    }
}

#endif // PLUS_TIMECONVSHARED_H
