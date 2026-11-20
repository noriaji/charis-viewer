/**
 * @file helper_json.h
 * @brief The small JSON reader and writer of the Charis Viewer helpers (charis-translate, charis-motion).
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

// Only what the helpers' one-line protocols need: strings, numbers, objects, arrays of numbers.
// Reader works on one line that is already in memory (the caller caps its length), walks it once and never
// recurses on what the input says: an unknown value is skipped by the grammar with its brackets kept in a string
// (skipStrict; skipValue is the translate helper's older, looser one), and the nested readers (object, array,
// numberRows) go only as deep as the caller's code does.
// Nothing here reads a locale or throws (std::string may throw bad_alloc, like everywhere).

#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace charis_helper
{
    inline void appendUtf8(std::string& out, unsigned cp);

    struct Reader
    {
        // A number longer than this (digits, sign, point and exponent) is refused: no real value needs it.
        static constexpr size_t MAX_NUMBER_CHARS = 40;

        const std::string& s;
        size_t             p = 0;

        explicit Reader(const std::string& str) : s(str) {}

        void ws()
        {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
            {
                ++p;
            }
        }

        // ws(), then true (and past it) when the next character is c.
        bool take(char c)
        {
            ws();
            if (p < s.size() && s[p] == c)
            {
                ++p;
                return true;
            }
            return false;
        }

        // ws(), then true when nothing but white space is left.
        bool atEnd()
        {
            ws();
            return p >= s.size();
        }

        bool hex4(unsigned& v)
        {
            if (p + 4 > s.size())
            {
                return false;
            }
            v = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char c = s[p + i];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= unsigned(c - '0');
                else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
                else return false;
            }
            p += 4;
            return true;
        }

        bool string(std::string& out)
        {
            out.clear();
            if (p >= s.size() || s[p] != '"')
            {
                return false;
            }
            ++p;
            while (p < s.size())
            {
                const char c = s[p++];
                if (c == '"')
                {
                    return true;
                }
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (p >= s.size())
                {
                    return false;
                }
                const char e = s[p++];
                switch (e)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '/': out += '/'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case 'u':
                {
                    unsigned cp = 0;
                    if (!hex4(cp))
                    {
                        return false;
                    }
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u')
                    {
                        p += 2;
                        unsigned lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            }
            return false;
        }

        // Skips a value loosely: a string is read, brackets are counted (a deep nesting costs no stack), anything
        // else runs to the next ',' or '}'. The shape inside is not checked ("nul", "[}" pass). Kept as it was
        // for the translate helper, whose answers must not change; new code uses skipStrict().
        bool skipValue()
        {
            ws();
            if (p >= s.size())
            {
                return false;
            }
            if (s[p] == '"')
            {
                std::string tmp;
                return string(tmp);
            }
            if (s[p] == '{' || s[p] == '[')
            {
                const char open = s[p], close = open == '{' ? '}' : ']';
                int depth = 0;
                while (p < s.size())
                {
                    if (s[p] == '"')
                    {
                        std::string tmp;
                        if (!string(tmp))
                        {
                            return false;
                        }
                        continue;
                    }
                    if (s[p] == open) ++depth;
                    if (s[p] == close && --depth == 0)
                    {
                        ++p;
                        return true;
                    }
                    ++p;
                }
                return false;
            }
            while (p < s.size() && s[p] != ',' && s[p] != '}')
            {
                ++p;
            }
            return true;
        }

        // Skips one value by the JSON grammar (objects, arrays, strings, numbers, true, false, null), nested at
        // most maxDepth deep. Iterative: the open brackets are kept in a small string, not on the call stack.
        // Where it differs from the grammar (RFC 8259), it is stricter: a number over MAX_NUMBER_CHARS or outside
        // a double (number()), a \u high surrogate not followed by a \u low one (string()), and nesting deeper
        // than maxDepth are refused. Raw control characters (U+0000..U+001F) in a string are refused here too, as
        // the grammar says; string() itself lets them through (the translate helper's reading, kept as it was).
        bool skipStrict(size_t maxDepth = 64)
        {
            std::string open;   // the closer of each open bracket, innermost last
            std::string tmp;
            auto        str = [&]() {
                const size_t from = p;
                if (!string(tmp))
                {
                    return false;
                }
                for (size_t i = from; i < p; ++i)
                {
                    if (static_cast<unsigned char>(s[i]) < 0x20)
                    {
                        return false;
                    }
                }
                return true;
            };
            for (;;)
            {
                // a value
                ws();
                if (p >= s.size())
                {
                    return false;
                }
                const char c = s[p];
                bool       done = true;   // false: a bracket was opened and its first item comes next
                if (c == '{' || c == '[')
                {
                    if (open.size() >= maxDepth)
                    {
                        return false;
                    }
                    ++p;
                    open += c == '{' ? '}' : ']';
                    if (take(open.back()))
                    {
                        open.pop_back();
                    }
                    else if (c == '{')
                    {
                        ws();
                        if (!str() || !take(':'))
                        {
                            return false;
                        }
                        done = false;
                    }
                    else
                    {
                        done = false;
                    }
                }
                else if (c == '"')
                {
                    if (!str())
                    {
                        return false;
                    }
                }
                else if (c == 't' || c == 'f' || c == 'n')
                {
                    const char* word = c == 't' ? "true" : c == 'f' ? "false" : "null";
                    const size_t n = c == 'f' ? 5 : 4;
                    if (s.compare(p, n, word) != 0)
                    {
                        return false;
                    }
                    p += n;
                    if (p < s.size() && ((s[p] >= 'a' && s[p] <= 'z') || (s[p] >= 'A' && s[p] <= 'Z') || (s[p] >= '0' && s[p] <= '9')))
                    {
                        return false;
                    }
                }
                else
                {
                    double v;
                    if (!number(v))
                    {
                        return false;
                    }
                }
                if (!done)
                {
                    continue;
                }
                // after a value: close brackets, or a ',' and the next item
                for (;;)
                {
                    if (open.empty())
                    {
                        return true;
                    }
                    if (take(open.back()))
                    {
                        open.pop_back();
                        continue;
                    }
                    if (!take(','))
                    {
                        return false;
                    }
                    if (open.back() == '}')
                    {
                        ws();
                        if (!str() || !take(':'))
                        {
                            return false;
                        }
                    }
                    break;
                }
            }
        }

        // A number by the JSON grammar only (no NaN, Infinity, hex, leading '+', leading zeros, bare '.'),
        // and stricter than it: at most MAX_NUMBER_CHARS long, finite as a double (1e999 is refused; a tiny one
        // becomes 0 or a subnormal), and followed by a delimiter (not a letter, digit, '.', '+' or '-').
        // The digits are copied out before strtod, so it never reads past them.
        bool number(double& v)
        {
            ws();
            const size_t start = p;
            size_t       q = p;
            auto digit = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
            if (q < s.size() && s[q] == '-')
            {
                ++q;
            }
            if (!digit(q))
            {
                return false;
            }
            if (s[q] == '0')
            {
                ++q;
            }
            else
            {
                while (digit(q))
                {
                    ++q;
                }
            }
            if (q < s.size() && s[q] == '.')
            {
                ++q;
                if (!digit(q))
                {
                    return false;
                }
                while (digit(q))
                {
                    ++q;
                }
            }
            if (q < s.size() && (s[q] == 'e' || s[q] == 'E'))
            {
                ++q;
                if (q < s.size() && (s[q] == '+' || s[q] == '-'))
                {
                    ++q;
                }
                if (!digit(q))
                {
                    return false;
                }
                while (digit(q))
                {
                    ++q;
                }
            }
            // the number must end here ("01", "0x10", "1.5.2", "2e5e" are not numbers)
            if (q < s.size() && ((s[q] >= '0' && s[q] <= '9') || (s[q] >= 'a' && s[q] <= 'z') || (s[q] >= 'A' && s[q] <= 'Z') ||
                                 s[q] == '.' || s[q] == '+' || s[q] == '-'))
            {
                return false;
            }
            if (q - start > MAX_NUMBER_CHARS)
            {
                return false;
            }
            char buf[MAX_NUMBER_CHARS + 1];
            s.copy(buf, q - start, start);
            buf[q - start] = '\0';
            char* end = nullptr;
            v = std::strtod(buf, &end);   // the helpers never call setlocale: the "C" locale's '.'
            if (end != buf + (q - start) || !std::isfinite(v))
            {
                return false;
            }
            p = q;
            return true;
        }

        // A number that is a whole number in [lo, hi] (1, 1.0 and 1e0 are all 1).
        bool integer(long long& v, long long lo, long long hi)
        {
            double d = 0.0;
            if (!number(d) || d != std::floor(d) || d < double(lo) || d > double(hi))
            {
                return false;
            }
            v = static_cast<long long>(d);
            return true;
        }

        // [n0, n1, ...] with exactly `count` numbers, into out[0..count).
        bool numberRow(double* out, size_t count)
        {
            if (!take('['))
            {
                return false;
            }
            for (size_t i = 0; i < count; ++i)
            {
                if ((i > 0 && !take(',')) || !number(out[i]))
                {
                    return false;
                }
            }
            return take(']');
        }

        // [[...], [...], ...]: rows of exactly `width` numbers each, at most maxRows of them (more is a failure,
        // found at the first extra row), appended to out row after row. rows = how many were read.
        bool numberRows(std::vector<double>& out, size_t width, size_t maxRows, size_t& rows)
        {
            rows = 0;
            if (!take('['))
            {
                return false;
            }
            if (take(']'))
            {
                return true;
            }
            for (;;)
            {
                if (rows == maxRows)
                {
                    return false;
                }
                out.resize(out.size() + width);
                if (!numberRow(out.data() + out.size() - width, width))
                {
                    return false;
                }
                ++rows;
                if (take(']'))
                {
                    return true;
                }
                if (!take(','))
                {
                    return false;
                }
            }
        }

        // { "key": value, ... }: onMember(key) reads the value (or skips it) and returns false to stop.
        // A missing ':' or ',', a key that is not a string and a line that ends early are failures.
        template <class OnMember>
        bool object(OnMember&& onMember)
        {
            if (!take('{'))
            {
                return false;
            }
            if (take('}'))
            {
                return true;
            }
            std::string key;
            for (;;)
            {
                ws();
                if (!string(key) || !take(':'))
                {
                    return false;
                }
                ws();
                if (!onMember(key))
                {
                    return false;
                }
                if (take('}'))
                {
                    return true;
                }
                if (!take(','))
                {
                    return false;
                }
            }
        }

        // [ value, ... ] with at most maxCount values: onElement(index) reads one. count = how many were read.
        template <class OnElement>
        bool array(OnElement&& onElement, size_t maxCount, size_t& count)
        {
            count = 0;
            if (!take('['))
            {
                return false;
            }
            if (take(']'))
            {
                return true;
            }
            for (;;)
            {
                if (count == maxCount)
                {
                    return false;
                }
                ws();
                if (!onElement(count))
                {
                    return false;
                }
                ++count;
                if (take(']'))
                {
                    return true;
                }
                if (!take(','))
                {
                    return false;
                }
            }
        }
    };

    inline void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80)
        {
            out += char(cp);
        }
        else if (cp < 0x800)
        {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        else
        {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    // A JSON string literal of s (the bytes are passed through; control characters are escaped).
    inline std::string quote(const std::string& s)
    {
        std::string out = "\"";
        for (const unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += char(c);
                }
            }
        }
        out += '"';
        return out;
    }

    // The shortest text that reads back as the same float (the helpers' answers are float32 values).
    // v must be finite (the caller checks; JSON has no NaN).
    inline void appendFloat(std::string& out, float v)
    {
        char buf[32];
        const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, r.ptr);
    }
}
