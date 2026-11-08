// Stand-alone test of timeconvmath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX timeconv_test.cpp && timeconv_test.exe
#include "timeconvmath.h"

#include <cstdio>
#include <cstdlib>

using namespace plus::timeconv;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static Civil mk(int y, int mo, int d, int h, int mi)
{
    Civil c;
    c.year = y;
    c.month = mo;
    c.day = d;
    c.hour = h;
    c.minute = mi;
    return c;
}

static const Zone& Z(const char* id)
{
    for (const Zone& z : zones())
        if (std::string(z.id) == id) return z;
    std::printf("no zone %s\n", id);
    std::exit(2);
}

static Minutes utcOf(const char* id, const Civil& c, Note* n = nullptr) { return localToUtc(Z(id), c, n); }

int main()
{
    // calendar
    CHECK(daysFromCivil(1970, 1, 1) == 0);
    CHECK(weekdayOfDays(0) == 4); // Thursday
    CHECK(daysInMonth(2028, 2) == 29 && daysInMonth(2026, 2) == 28 && daysInMonth(2100, 2) == 28);
    CHECK(nthSunday(2026, 3, 2) == 8 && nthSunday(2026, 11, 1) == 1);
    CHECK(lastSunday(2026, 3) == 29 && lastSunday(2026, 10) == 25);
    {
        int y, m, d;
        civilFromDays(daysFromCivil(2028, 2, 29), y, m, d);
        CHECK(y == 2028 && m == 2 && d == 29);
    }

    // the example of the design: summer time in SL (PDT, UTC-7), Japan 16 hours ahead
    {
        const Minutes u = utcOf("sl", mk(2026, 10, 24, 7, 0));
        CHECK(utcToLocal(Z("utc"), u) == mk(2026, 10, 24, 14, 0));
        CHECK(utcToLocal(Z("jst"), u) == mk(2026, 10, 24, 23, 0));
        CHECK(isDst(Z("sl"), u));
    }
    // after 2026-11-01 02:00 SL is PST, 17 hours behind Japan; the day changes
    {
        const Minutes u = utcOf("sl", mk(2026, 11, 2, 7, 0));
        CHECK(utcToLocal(Z("utc"), u) == mk(2026, 11, 2, 15, 0));
        CHECK(utcToLocal(Z("jst"), u) == mk(2026, 11, 3, 0, 0));
        CHECK(!isDst(Z("sl"), u));
    }
    // Japan to SL, back a day
    {
        const Minutes u = utcOf("jst", mk(2026, 10, 24, 2, 0));
        CHECK(utcToLocal(Z("sl"), u) == mk(2026, 10, 23, 10, 0));
    }

    // US boundaries: 2026-03-08 01:59 PST, the next minute is 03:00 PDT
    {
        const Minutes u = utcOf("sl", mk(2026, 3, 8, 1, 59));
        CHECK(!isDst(Z("sl"), u) && isDst(Z("sl"), u + 1));
        CHECK(utcToLocal(Z("sl"), u + 1) == mk(2026, 3, 8, 3, 0));
        // the clock that does not exist
        Note n;
        const Minutes g = utcOf("sl", mk(2026, 3, 8, 2, 30), &n);
        CHECK(n == Note::Gap);
        CHECK(utcToLocal(Z("sl"), g) == mk(2026, 3, 8, 3, 30));
        // the end: 2026-11-01 01:59 PDT, next minute 01:00 PST
        const Minutes e = utcOf("sl", mk(2026, 11, 1, 1, 59), &n);
        CHECK(n == Note::Overlap && isDst(Z("sl"), e));
        CHECK(utcToLocal(Z("sl"), e + 1) == mk(2026, 11, 1, 1, 0) && !isDst(Z("sl"), e + 1));
        CHECK(utcToLocal(Z("sl"), e + 61) == mk(2026, 11, 1, 2, 0));
        // an ordinary clock has no note
        utcOf("sl", mk(2026, 6, 1, 12, 0), &n);
        CHECK(n == Note::None);
    }

    // the weeks the US and Europe differ: 2026-03-20 London is still GMT, SL already PDT
    {
        const Minutes u = utcOf("sl", mk(2026, 3, 20, 12, 0));
        CHECK(utcToLocal(Z("utc"), u) == mk(2026, 3, 20, 19, 0));
        CHECK(utcToLocal(Z("uk"), u) == mk(2026, 3, 20, 19, 0));
        CHECK(utcToLocal(Z("cet"), u) == mk(2026, 3, 20, 20, 0));
        const Minutes v = utcOf("sl", mk(2026, 4, 1, 12, 0));
        CHECK(utcToLocal(Z("uk"), v) == mk(2026, 4, 1, 20, 0));  // BST
        CHECK(utcToLocal(Z("cet"), v) == mk(2026, 4, 1, 21, 0)); // CEST
    }

    // southern summer time: Sydney in January AEDT (+11), in July AEST (+10); NZ +13 / +12
    {
        const Minutes jan = utcOf("utc", mk(2026, 1, 15, 0, 0));
        const Minutes jul = utcOf("utc", mk(2026, 7, 15, 0, 0));
        CHECK(utcToLocal(Z("aet"), jan) == mk(2026, 1, 15, 11, 0));
        CHECK(utcToLocal(Z("aet"), jul) == mk(2026, 7, 15, 10, 0));
        CHECK(utcToLocal(Z("nz"), jan) == mk(2026, 1, 15, 13, 0));
        CHECK(utcToLocal(Z("nz"), jul) == mk(2026, 7, 15, 12, 0));
        // Sydney: changes on 2026-10-04 and 2026-04-05
        CHECK(!isDst(Z("aet"), utcOf("utc", mk(2026, 10, 3, 15, 59))) && isDst(Z("aet"), utcOf("utc", mk(2026, 10, 3, 16, 0))));
        CHECK(isDst(Z("aet"), utcOf("utc", mk(2026, 4, 4, 15, 59))) && !isDst(Z("aet"), utcOf("utc", mk(2026, 4, 4, 16, 0))));
    }

    // zones with no summer time, and a fixed zone
    CHECK(utcToLocal(Z("india"), utcOf("utc", mk(2026, 1, 1, 0, 0))) == mk(2026, 1, 1, 5, 30));
    CHECK(utcToLocal(Z("hst"), utcOf("utc", mk(2026, 1, 1, 0, 0))) == mk(2025, 12, 31, 14, 0));
    {
        const Zone f = fixedZone("fixed", -210);
        CHECK(utcToLocal(f, 24 * 60) == mk(1970, 1, 1, 20, 30));
    }

    // year edges, leap day
    CHECK(utcToLocal(Z("jst"), utcOf("utc", mk(2027, 12, 31, 15, 0))) == mk(2028, 1, 1, 0, 0));
    CHECK(utcToLocal(Z("sl"), utcOf("jst", mk(2028, 3, 1, 3, 0))) == mk(2028, 2, 29, 10, 0));

    // round trip over a lot of moments (outside the skipped and doubled hours) in every zone
    {
        int bad = 0;
        for (const Zone& z : zones())
            for (Minutes u = 24 * 60; u < 130LL * 365 * 1440; u += 7919) // every 5.5 days, 1970-2100
            {
                const Civil c = utcToLocal(z, u);
                Note n;
                const Minutes back = localToUtc(z, c, &n);
                if (n == Note::None && back != u) ++bad;
                if (n == Note::Overlap && back != u && back != u - 60 && back != u + 60) ++bad;
            }
        CHECK(bad == 0);
    }

    // stepping
    CHECK(addWallMinutes(mk(2026, 10, 24, 23, 59), 1) == mk(2026, 10, 25, 0, 0));
    CHECK(addWallMinutes(mk(2026, 1, 1, 0, 0), -1) == mk(2025, 12, 31, 23, 59));
    CHECK(addMonths(mk(2026, 1, 31, 5, 0), 1) == mk(2026, 2, 28, 5, 0));
    CHECK(addMonths(mk(2026, 12, 15, 5, 0), 1) == mk(2027, 1, 15, 5, 0));
    CHECK(addMonths(mk(2026, 1, 15, 5, 0), -1) == mk(2025, 12, 15, 5, 0));
    CHECK(addMonths(mk(2026, 1, 15, 5, 0), -13) == mk(2024, 12, 15, 5, 0));

    // validity
    CHECK(validCivil(mk(2026, 2, 28, 23, 59)) && !validCivil(mk(2026, 2, 29, 0, 0)) && validCivil(mk(2028, 2, 29, 0, 0)));
    CHECK(!validCivil(mk(2026, 13, 1, 0, 0)) && !validCivil(mk(2026, 1, 1, 24, 0)) && !validCivil(mk(2026, 1, 1, 0, 60)));
    CHECK(!validCivil(mk(1969, 12, 31, 0, 0)) && !validCivil(mk(2101, 1, 1, 0, 0)));

    // text
    CHECK(offsetText(540) == "UTC+9" && offsetText(-420) == "UTC-7" && offsetText(330) == "UTC+5:30" && offsetText(0) == "UTC");
    CHECK(offsetText(-210) == "UTC-3:30" && offsetText(345) == "UTC+5:45");
    CHECK(civilText(mk(2026, 3, 5, 7, 4)) == "2026-03-05 07:04");
    CHECK(abbrText(Z("sl"), true) == "PDT" && abbrText(Z("sl"), false) == "PST" && abbrText(fixedZone("f", 60), false).empty());

    // paste
    {
        Parsed p;
        CHECK(parseDateTime("2026/10/24 19:30", p) && p.hasDate && p.hasTime && p.year == 2026 && p.month == 10 && p.day == 24 && p.hour == 19 && p.minute == 30);
        CHECK(parseDateTime("2026-10-24 7pm", p) && p.hour == 19 && p.minute == 0);
        CHECK(parseDateTime("2026-10-24 12am", p) && p.hour == 0);
        CHECK(parseDateTime("2026-10-24 12pm", p) && p.hour == 12);
        CHECK(parseDateTime("2026\xE5\xB9\xB4" "10\xE6\x9C\x88" "24\xE6\x97\xA5 19\xE6\x99\x82" "30\xE5\x88\x86", p) && p.hour == 19 && p.minute == 30 && p.month == 10);
        CHECK(parseDateTime("20261024 1930", p) && p.year == 2026 && p.day == 24 && p.hour == 19 && p.minute == 30);
        CHECK(parseDateTime("19:30", p) && !p.hasDate && p.hasTime && p.hour == 19 && p.minute == 30);
        CHECK(parseDateTime("7pm", p) && !p.hasDate && p.hour == 19 && p.ampm && p.numbers == 1);
        CHECK(parseDateTime("19", p) && !p.ampm && p.numbers == 1);
        CHECK(parseDateTime("2026/10/24 19:30", p) && p.numbers == 5);
        CHECK(parseDateTime("2026-10-24", p) && p.hasDate && !p.hasTime);
        CHECK(!parseDateTime("2026-13-24 10:00", p));
        CHECK(!parseDateTime("2026-02-30 10:00", p));
        CHECK(!parseDateTime("25:00", p));
        CHECK(!parseDateTime("hello", p));
        CHECK(!parseDateTime("", p));
    }

    if (failures == 0) std::printf("OK\n");
    return failures ? 1 : 0;
}
