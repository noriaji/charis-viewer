/**
 * @file slwxtag.cpp
 * @brief The [slwx ...] tag of a parcel's description: reading, writing,
 *        and the check that they agree (research/229 sections 2, 7 and 9).
 *
 * The description is somebody else's text: every loop here is bounded by
 * DESCRIPTION_LIMIT bytes and MAX_ITEMS items, every number goes through
 * std::from_chars (no locale, no exceptions) and is clamped, and a value that
 * is not a finite number drops its item.
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

#ifndef PLUS_SLWX_STANDALONE
#include "llviewerprecompiledheaders.h"
#endif

#include "slwxtag.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

namespace plus::slwx::tag
{
    namespace
    {
        constexpr char OPEN[] = "[slwx";
        constexpr size_t OPEN_LENGTH = 5;
        // Below this a value counts as the default when writing.
        constexpr double EPS = 0.0005;

        bool isSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        char lower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }

        bool opensAt(const std::string& text, size_t at, size_t limit)
        {
            if (at + OPEN_LENGTH >= limit)
            {
                return false;
            }
            for (size_t k = 0; k < OPEN_LENGTH; ++k)
            {
                if (lower(text[at + k]) != OPEN[k])
                {
                    return false;
                }
            }
            const char next = text[at + OPEN_LENGTH];
            return isSpace(next) || next == ']';
        }

        bool equalsNoCase(std::string_view a, const char* b)
        {
            size_t k = 0;
            for (; k < a.size() && b[k]; ++k)
            {
                if (lower(a[k]) != b[k])
                {
                    return false;
                }
            }
            return k == a.size() && !b[k];
        }

        bool toDouble(std::string_view s, double& v)
        {
            if (s.size() > 1 && s[0] == '+' && s[1] != '-')
            {
                s.remove_prefix(1);
            }
            if (s.empty())
            {
                return false;
            }
            double x = 0.0;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), x);
            if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(x))
            {
                return false;
            }
            v = x;
            return true;
        }

        float clampTo(double v, double lo, double hi)
        {
            return (float)std::min(std::max(v, lo), hi);
        }

        // Decimal or 0x hex: the low 32 bits of the number (it may be longer).
        bool toSeed(std::string_view s, uint32_t& v)
        {
            uint32_t base = 10;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            {
                base = 16;
                s.remove_prefix(2);
            }
            if (s.empty())
            {
                return false;
            }
            uint32_t x = 0;
            for (const char c : s)
            {
                uint32_t digit = 99;
                if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
                if (digit >= base)
                {
                    return false;
                }
                x = x * base + digit;
            }
            v = x;
            return true;
        }

        bool toSigned(std::string_view s, int64_t& v)
        {
            if (s.empty())
            {
                return false;
            }
            int64_t x = 0;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), x);
            if (r.ec != std::errc() || r.ptr != s.data() + s.size())
            {
                return false;
            }
            v = x;
            return true;
        }

        // Sets one key; false when the value is broken. Unknown keys are not an error.
        bool setKey(const std::string& key, std::string_view value, Params& p)
        {
            double d = 0.0;
            if (key == "wind")
            {
                const size_t at = value.find('@');
                double dir = p.wind_from;
                double speed = 0.0;
                if (at == std::string_view::npos)
                {
                    if (!toDouble(value, speed))
                    {
                        return false;
                    }
                }
                else if (!toDouble(value.substr(0, at), dir) || !toDouble(value.substr(at + 1), speed))
                {
                    return false;
                }
                dir = std::fmod(dir, 360.0);
                p.wind_from = (float)(dir < 0.0 ? dir + 360.0 : dir);
                p.wind_speed = clampTo(speed, 0.0, WIND_SPEED_MAX);
                return true;
            }
            if (key == "seed")
            {
                uint32_t u = 0;
                if (!toSeed(value, u))
                {
                    return false;
                }
                p.has_seed = true;
                p.seed = u;
                return true;
            }
            if (key == "since")
            {
                int64_t s = 0;
                if (!toSigned(value, s))
                {
                    return false;
                }
                p.has_since = true;
                p.since = s;
                return true;
            }
            if (!toDouble(value, d))
            {
                // A broken value only counts for a key that is known.
                static const char* const KNOWN[] = { "rain", "snow", "rainmm", "snowcm", "pvar", "var", "gust", "wdirvar",
                                                     "storm", "fog", "cloud", "trans", "depth", "rate" };
                for (const char* k : KNOWN)
                {
                    if (key == k)
                    {
                        return false;
                    }
                }
                return true;
            }
            if (key == "rain") p.rain = clampTo(d, 0.0, 1.0);
            else if (key == "snow") p.snow = clampTo(d, 0.0, 1.0);
            else if (key == "rainmm") { p.has_rainmm = true; p.rainmm = clampTo(d, 0.0, RAIN_MM_MAX); }
            else if (key == "snowcm") { p.has_snowcm = true; p.snowcm = clampTo(d, 0.0, SNOW_CM_MAX); }
            else if (key == "pvar") p.pvar = clampTo(d, 0.0, 1.0);
            else if (key == "var") p.var = clampTo(d, 0.0, 1.0);
            else if (key == "gust") p.gust = clampTo(d, 0.0, 1.0);
            else if (key == "wdirvar") p.wdirvar = clampTo(d, 0.0, 180.0);
            else if (key == "storm") p.storm = clampTo(d, 0.0, 1.0);
            else if (key == "fog") { p.has_fog = true; p.fog = clampTo(d, 0.0, 1.0); }
            else if (key == "cloud") { p.has_cloud = true; p.cloud = clampTo(d, 0.0, 1.0); }
            else if (key == "trans") p.trans = clampTo(d, 0.0, TRANS_MAX);
            else if (key == "depth") { p.has_depth = true; p.depth = clampTo(d, 0.0, DEPTH_MAX); }
            else if (key == "rate") p.rate = clampTo(d, -RATE_MAX, RATE_MAX);
            return true;
        }

        // A number as short text: fixed, at most `precision` decimals, no trailing zeros.
        void appendNumber(std::string& out, double v, int precision)
        {
            char buf[48];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, precision);
            if (r.ec != std::errc())
            {
                out += '0';
                return;
            }
            size_t n = (size_t)(r.ptr - buf);
            if (std::find(buf, buf + n, '.') != buf + n)
            {
                while (n > 0 && buf[n - 1] == '0')
                {
                    --n;
                }
                if (n > 0 && buf[n - 1] == '.')
                {
                    --n;
                }
            }
            if (n == 0 || (n == 1 && buf[0] == '-'))
            {
                out += '0';
                return;
            }
            out.append(buf, n);
        }

        void appendKey(std::string& out, const char* key, double v, int precision)
        {
            out += ' ';
            out += key;
            out += '=';
            appendNumber(out, v, precision);
        }

        bool differs(double a, double b)
        {
            return std::fabs(a - b) > EPS;
        }

        // Piecewise scale through (0, 0), (1/3, a), (2/3, b), (1, c).
        float scaleUp(float x, float a, float b, float c)
        {
            x = std::min(std::max(x, 0.f), 1.f);
            const float third = 1.f / 3.f;
            if (x <= third) return a * (x / third);
            if (x <= 2.f * third) return a + (b - a) * ((x - third) / third);
            return b + (c - b) * ((x - 2.f * third) / third);
        }

        float scaleDown(float v, float a, float b, float c)
        {
            const float third = 1.f / 3.f;
            if (!(v > 0.f)) return 0.f;
            if (v <= a) return third * (v / a);
            if (v <= b) return third + third * ((v - a) / (b - a));
            if (v <= c) return 2.f * third + third * ((v - b) / (c - b));
            return 1.f;
        }
    }

    float rainToMm(float rain) { return scaleUp(rain, 2.5f, 10.f, 50.f); }
    float mmToRain(float mm) { return scaleDown(mm, 2.5f, 10.f, 50.f); }
    float snowToCm(float snow) { return scaleUp(snow, 1.f, 4.f, 20.f); }
    float cmToSnow(float cm) { return scaleDown(cm, 1.f, 4.f, 20.f); }

    int rainLevelOfMm(float mm)
    {
        if (!(mm > 0.001f)) return 0;
        return mm < 2.5f ? 1 : (mm < 10.f ? 2 : 3);
    }

    int snowLevelOfCm(float cm)
    {
        if (!(cm > 0.001f)) return 0;
        return cm < 1.f ? 1 : (cm < 4.f ? 2 : 3);
    }

    float rainMm(const Params& p) { return p.has_rainmm ? p.rainmm : rainToMm(p.rain); }
    float snowCm(const Params& p) { return p.has_snowcm ? p.snowcm : snowToCm(p.snow); }

    bool find(const std::string& description, size_t& begin, size_t& end)
    {
        const size_t limit = std::min(description.size(), DESCRIPTION_LIMIT);
        for (size_t at = 0; at + OPEN_LENGTH < limit; ++at)
        {
            if (description[at] != '[' || !opensAt(description, at, limit))
            {
                continue;
            }
            for (size_t close = at + OPEN_LENGTH; close < limit; ++close)
            {
                if (description[close] == ']')
                {
                    begin = at;
                    end = close + 1;
                    return true;
                }
            }
            // No "]" before the limit: not a tag; a later "[slwx" cannot close either.
            return false;
        }
        return false;
    }

    bool hasUnclosedOpen(const std::string& description)
    {
        const size_t limit = std::min(description.size(), DESCRIPTION_LIMIT);
        for (size_t at = 0; at + OPEN_LENGTH < limit; ++at)
        {
            if (description[at] == '[' && opensAt(description, at, limit))
            {
                return description.find(']', at + OPEN_LENGTH) >= limit;
            }
        }
        return false;
    }

    ParseResult parse(const std::string& description)
    {
        ParseResult result;
        size_t begin = 0;
        size_t end = 0;
        if (!find(description, begin, end))
        {
            return result;
        }
        result.found = true;
        const std::string_view body(description.data() + begin + OPEN_LENGTH, end - begin - OPEN_LENGTH - 1);
        size_t at = 0;
        while (at < body.size())
        {
            while (at < body.size() && isSpace(body[at]))
            {
                ++at;
            }
            const size_t start = at;
            while (at < body.size() && !isSpace(body[at]))
            {
                ++at;
            }
            if (at == start)
            {
                break;
            }
            const std::string_view item = body.substr(start, at - start);
            const size_t eq = item.find('=');
            if (eq == std::string_view::npos)
            {
                if (equalsNoCase(item, "off") || equalsNoCase(item, "clear"))
                {
                    result.off = true;
                }
                continue;   // another bare word: unknown, ignored
            }
            std::string key(item.substr(0, eq));
            for (char& c : key)
            {
                c = lower(c);
            }
            if (!setKey(key, item.substr(eq + 1), result.params))
            {
                ++result.bad_items;
            }
        }
        return result;
    }

    std::string write(const Params& p)
    {
        std::string out = "[slwx";
        const float rain = p.has_rainmm ? mmToRain(p.rainmm) : p.rain;
        const float snow = p.has_snowcm ? cmToSnow(p.snowcm) : p.snow;
        if (rain > EPS || p.has_rainmm)
        {
            appendKey(out, "rain", rain, 3);
            appendKey(out, "rainmm", rainMm(p), 2);
        }
        if (snow > EPS || p.has_snowcm)
        {
            appendKey(out, "snow", snow, 3);
            appendKey(out, "snowcm", snowCm(p), 2);
        }
        if (differs(p.pvar, PVAR_DEFAULT)) appendKey(out, "pvar", p.pvar, 3);
        if (differs(p.wind_from, WIND_DIR_DEFAULT) || differs(p.wind_speed, WIND_SPEED_DEFAULT))
        {
            out += " wind=";
            appendNumber(out, p.wind_from, 1);
            out += '@';
            appendNumber(out, p.wind_speed, 2);
        }
        if (differs(p.var, VAR_DEFAULT)) appendKey(out, "var", p.var, 3);
        if (differs(p.gust, GUST_DEFAULT)) appendKey(out, "gust", p.gust, 3);
        if (differs(p.wdirvar, 0.0)) appendKey(out, "wdirvar", p.wdirvar, 1);
        if (differs(p.storm, 0.0)) appendKey(out, "storm", p.storm, 3);
        if (p.has_fog) appendKey(out, "fog", p.fog, 3);
        if (p.has_cloud) appendKey(out, "cloud", p.cloud, 3);
        if (differs(p.trans, TRANS_DEFAULT)) appendKey(out, "trans", p.trans, 1);
        if (p.has_seed)
        {
            char buf[24];
            const auto r = std::to_chars(buf, buf + sizeof(buf), (unsigned long long)p.seed, 16);
            out += " seed=0x";
            out.append(buf, (size_t)(r.ptr - buf));
        }
        if (p.has_depth) appendKey(out, "depth", p.depth, 2);
        if (differs(p.rate, 0.0)) appendKey(out, "rate", p.rate, 2);
        if (p.has_since)
        {
            out += " since=";
            out += std::to_string((long long)p.since);
        }
        out += ']';
        return out;
    }

    std::string writeOff()
    {
        return "[slwx off]";
    }

    bool replaceIn(std::string& description, const std::string& tag)
    {
        std::string result;
        size_t begin = 0;
        size_t end = 0;
        if (find(description, begin, end))
        {
            if (tag.empty())
            {
                // Take the tag out with one of the spaces around it.
                if (begin > 0 && isSpace(description[begin - 1]))
                {
                    --begin;
                }
                else if (end < description.size() && isSpace(description[end]))
                {
                    ++end;
                }
                result = description.substr(0, begin) + description.substr(end);
            }
            else
            {
                result = description.substr(0, begin) + tag + description.substr(end);
            }
        }
        else if (tag.empty())
        {
            return true;    // nothing to take out
        }
        else if (hasUnclosedOpen(description))
        {
            return false;   // the "[slwx" without its "]" would swallow the new tag
        }
        else
        {
            result = description;
            if (!result.empty() && !isSpace(result.back()))
            {
                result += ' ';
            }
            result += tag;
        }
        if (result.size() > DESCRIPTION_LIMIT)
        {
            return false;
        }
        description = result;
        return true;
    }

    bool selftest(std::string& report)
    {
        int step = 0;
        auto fail = [&](const char* what) { report = std::string("slwxtag selftest #") + std::to_string(step) + ": " + what; return false; };
        auto approx = [](double a, double b) { return std::fabs(a - b) < 0.011; };
#define CHECK(cond) do { ++step; if (!(cond)) { return fail(#cond); } } while (0)

        {   // Slipstream's own example, inside other text.
            const ParseResult r = parse("Welcome! [slwx rain=0.7 wind=225@8 var=0.5 gust=0.6 storm=0.4 seed=0x5eed] enjoy");
            CHECK(r.found && !r.off && r.bad_items == 0);
            CHECK(approx(r.params.rain, 0.7) && approx(r.params.wind_from, 225) && approx(r.params.wind_speed, 8));
            CHECK(approx(r.params.var, 0.5) && approx(r.params.gust, 0.6) && approx(r.params.storm, 0.4));
            CHECK(r.params.has_seed && r.params.seed == 0x5eed);
            CHECK(approx(r.params.pvar, PVAR_DEFAULT) && approx(r.params.trans, TRANS_DEFAULT) && !r.params.has_rainmm);
        }
        {   // The shortest tag; the empty tag.
            const ParseResult r = parse("[slwx rain=.6]");
            CHECK(r.found && approx(r.params.rain, 0.6) && r.bad_items == 0);
            const ParseResult e = parse("[slwx]");
            CHECK(e.found && !e.off && approx(e.params.rain, 0));
        }
        {   // A broken item is dropped and the rest is used; an unknown key is ignored.
            const ParseResult r = parse("[slwx rain=abc wind=1@2 gust=0.9 future=7 snow=nan storm=]");
            CHECK(r.found && r.bad_items == 3);
            CHECK(approx(r.params.gust, 0.9) && approx(r.params.wind_from, 1) && approx(r.params.wind_speed, 2) && approx(r.params.rain, 0));
        }
        {   // Out of range: clamped.
            const ParseResult r = parse("[slwx rain=5 snow=-2 wind=500@99 trans=-4 rainmm=1e9 snowcm=400 seed=0x1ffffffff rate=-1e3 depth=99]");
            CHECK(approx(r.params.rain, 1) && approx(r.params.snow, 0) && approx(r.params.wind_from, 140) && approx(r.params.wind_speed, 40));
            CHECK(approx(r.params.trans, 0) && approx(r.params.rainmm, 200) && approx(r.params.snowcm, 360));
            CHECK(r.params.seed == 0xffffffffu && approx(r.params.rate, -100) && approx(r.params.depth, 10));
            CHECK(approx(parse("[slwx wind=-90@3 rain=+0.5]").params.wind_from, 270) && approx(parse("[slwx rain=+0.5]").params.rain, 0.5));
            CHECK(parse("[slwx seed=123456789012345678901234567890]").params.has_seed);
            const ParseResult w = parse("[slwx wind=7]");
            CHECK(approx(w.params.wind_speed, 7) && approx(w.params.wind_from, WIND_DIR_DEFAULT));
        }
        {   // off and clear win over the other keys.
            CHECK(parse("[slwx rain=1 off]").off);
            CHECK(parse("[slwx CLEAR]").off);
            CHECK(!parse("[slwx rain=1]").off);
        }
        {   // Past 255 bytes nothing is read; the tag must end inside.
            const std::string pad(250, 'x');
            CHECK(!parse(pad + "[slwx rain=1]").found);
            CHECK(parse(pad.substr(0, 200) + "[slwx rain=1]").found);
            CHECK(!parse("[slwx rain=1").found);
            CHECK(hasUnclosedOpen("hi [slwx rain=1") && !hasUnclosedOpen("hi [slwx rain=1]") && !hasUnclosedOpen("none"));
            std::string broken = "hi [slwx rain=1";
            CHECK(!replaceIn(broken, "[slwx snow=1]") && broken == "hi [slwx rain=1" && replaceIn(broken, ""));
            CHECK(!parse("[slwxrain=1]").found);
        }
        {   // Upper case; several tags (the first only).
            const ParseResult r = parse("[SLWX Rain=0.5 WIND=90@3]");
            CHECK(r.found && approx(r.params.rain, 0.5) && approx(r.params.wind_from, 90));
            const ParseResult m = parse("[slwx rain=0.2] and [slwx rain=0.9]");
            CHECK(approx(m.params.rain, 0.2));
        }
        {   // More items than the bound: the rest is not read, nothing breaks.
            std::string many = "[slwx";
            for (int k = 0; k < 100; ++k) many += " a";
            many += " rain=1]";
            CHECK(parse(many).found && approx(parse(many).params.rain, 1));
        }
        {   // Writing: the default is the empty tag; both rain and rainmm are written.
            Params d;
            CHECK(write(d) == "[slwx]");
            CHECK(writeOff() == "[slwx off]");
            Params p;
            p.rain = 0.7f;
            p.wind_from = 225.f;
            p.wind_speed = 8.f;
            p.gust = 0.6f;
            p.storm = 0.4f;
            p.has_seed = true;
            p.seed = 0x5eed;
            const std::string text = write(p);
            CHECK(text.find("rain=0.7") != std::string::npos && text.find("rainmm=") != std::string::npos);
            CHECK(text.find("snow") == std::string::npos && text.find("pvar") == std::string::npos);
            const ParseResult back = parse(text);
            CHECK(back.found && back.bad_items == 0);
            CHECK(approx(back.params.rain, 0.7) && approx(back.params.wind_speed, 8) && approx(back.params.gust, 0.6));
            CHECK(approx(back.params.storm, 0.4) && back.params.seed == 0x5eed && back.params.has_rainmm);
            CHECK(approx(rainMm(back.params), rainToMm(0.7f)));
            // Everything set: written then read back.
            Params q;
            q.rain = 0.2f; q.snow = 0.5f; q.pvar = 0.8f; q.wind_from = 10.5f; q.wind_speed = 12.25f;
            q.var = 0.1f; q.gust = 0.9f; q.wdirvar = 45.f; q.storm = 1.f; q.has_fog = true; q.fog = 0.3f;
            q.has_cloud = true; q.cloud = 0.6f; q.trans = 120.f; q.has_seed = true; q.seed = 4000000000u;
            q.has_depth = true; q.depth = 1.5f; q.rate = -2.f; q.has_since = true; q.since = 1760000000;
            const ParseResult b2 = parse(write(q));
            CHECK(b2.found && b2.bad_items == 0);
            const Params& g = b2.params;
            CHECK(approx(g.rain, 0.2) && approx(g.snow, 0.5) && approx(g.pvar, 0.8) && approx(g.wind_from, 10.5) && approx(g.wind_speed, 12.25));
            CHECK(approx(g.var, 0.1) && approx(g.gust, 0.9) && approx(g.wdirvar, 45) && approx(g.storm, 1) && g.has_fog && approx(g.fog, 0.3));
            CHECK(g.has_cloud && approx(g.cloud, 0.6) && approx(g.trans, 120) && g.seed == 4000000000u);
            CHECK(g.has_depth && approx(g.depth, 1.5) && approx(g.rate, -2) && g.has_since && g.since == 1760000000);
        }
        {   // The scale and the levels.
            CHECK(approx(mmToRain(rainToMm(0.5f)), 0.5) && approx(cmToSnow(snowToCm(0.9f)), 0.9));
            CHECK(rainLevelOfMm(0) == 0 && rainLevelOfMm(1) == 1 && rainLevelOfMm(2.5f) == 2 && rainLevelOfMm(10) == 3);
            CHECK(snowLevelOfCm(0) == 0 && snowLevelOfCm(0.5f) == 1 && snowLevelOfCm(1) == 2 && snowLevelOfCm(4) == 3);
            Params p;
            p.rain = 0.2f;
            CHECK(rainLevelOfMm(rainMm(p)) == 1);
            p.rain = 0.5f;
            CHECK(rainLevelOfMm(rainMm(p)) == 2);
            p.has_rainmm = true;
            p.rainmm = 30.f;
            CHECK(rainLevelOfMm(rainMm(p)) == 3);
        }
        {   // Replacing and taking out.
            std::string d = "A fine garden.";
            CHECK(replaceIn(d, "[slwx rain=1]") && d == "A fine garden. [slwx rain=1]");
            CHECK(replaceIn(d, "[slwx snow=1]") && d == "A fine garden. [slwx snow=1]");
            CHECK(replaceIn(d, "") && d == "A fine garden.");
            CHECK(replaceIn(d, "") && d == "A fine garden.");
            std::string e = "[slwx rain=1] tail";
            CHECK(replaceIn(e, "") && e == "tail");
            std::string big(250, 'x');
            CHECK(!replaceIn(big, "[slwx rain=1]") && big == std::string(250, 'x'));
        }
#undef CHECK
        report.clear();
        return true;
    }
}
