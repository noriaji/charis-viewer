/**
 * @file rmlnumbermath.h
 * @brief The number box's plain number functions with no viewer or RmlUi types (research/387 3.5), so a stand-alone
 *        test (rmlnumbermath_test.cpp) can try them.
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

#ifndef PLUS_RMLNUMBERMATH_H
#define PLUS_RMLNUMBERMATH_H

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace plus::rmlui
{
    // One character of a number box's text as the box reads it: full-width ASCII (U+FF01 to U+FF5E, what a Japanese IME
    // types) made half-width, the ideographic space a space, the minus sign U+2212 a "-".
    inline char32_t foldNumberChar(char32_t c)
    {
        if (c >= 0xFF01 && c <= 0xFF5E)
        {
            return c - 0xFEE0;
        }
        if (c == 0x3000)
        {
            return U' ';
        }
        if (c == 0x2212)
        {
            return U'-';
        }
        return c;
    }

    // A plain decimal number ("12", "-0.5", ".5"; no exponent, no sign "+", no "inf" or "nan"), the whole text.
    inline bool parsePlainNumber(const std::string& text, double& out)
    {
        if (text.empty())
        {
            return false;
        }
        const char* first = text.data();
        const char* last = first + text.size();
        double value = 0.0;
        const std::from_chars_result result = std::from_chars(first, last, value, std::chars_format::fixed);
        if (result.ec != std::errc() || result.ptr != last || !std::isfinite(value))
        {
            return false;
        }
        out = value;
        return true;
    }

    // The number with decimals digits after the point ("-0" is "0").
    inline std::string formatNumber(double value, int decimals)
    {
        if (value == 0.0)
        {
            value = 0.0;    // not "-0"
        }
        char buffer[64];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, decimals);
        if (result.ec != std::errc())
        {
            return std::string("0");
        }
        std::string text(buffer, result.ptr);
        if (text.size() > 1 && text[0] == '-' && text.find_first_not_of("0.", 1) == std::string::npos)
        {
            text.erase(0, 1);   // a small negative number shown with few digits ("-0", "-0.0") is a zero
        }
        return text;
    }

    // A value rounded to decimals digits.
    inline double roundTo(double value, int decimals)
    {
        const double scale = std::pow(10.0, decimals);
        return std::round(value * scale) / scale;
    }
}

#endif // PLUS_RMLNUMBERMATH_H
