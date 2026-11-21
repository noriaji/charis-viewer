/**
 * @file rmlnumber.cpp
 * @brief The number box's text: what the XUI spin control reads and writes, as plain functions (research/387 3.5).
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

#include "llviewerprecompiledheaders.h"

#include "rmlnumber.h"

#include "llcalc.h"
#include "llstring.h"

#include <algorithm>
#include <cmath>

namespace plus::rmlui
{
    namespace
    {
        constexpr size_t MAX_TEXT_CHARS = 64;   // a longer pasted text is no number
        constexpr double NUMBER_LIMIT = 1.0e9;  // the end of any range, and the range of a box that gives none

        double rangeAttribute(const Rml::Element* spin, const char* name, double fallback)
        {
            double value = 0.0;
            if (spin->HasAttribute(name) && parsePlainNumber(normalizeNumberText(spin->GetAttribute<Rml::String>(name, "")), value))
            {
                return std::clamp(value, -NUMBER_LIMIT, NUMBER_LIMIT);
            }
            return fallback;
        }
    }

    // A text as the number box reads it: full-width ASCII (U+FF01 to U+FF5E, what a Japanese IME types) and the
    // ideographic space made half-width, the minus sign U+2212 made "-", spaces cut off the ends. Empty when the
    // text is too long to be a number.
    std::string normalizeNumberText(const std::string& utf8)
    {
        LLWString wide = utf8str_to_wstring(utf8);
        if (wide.size() > MAX_TEXT_CHARS)
        {
            return std::string();
        }
        for (llwchar& c : wide)
        {
            c = (llwchar)foldNumberChar((char32_t)c);
        }
        LLWStringUtil::trim(wide);
        return wstring_to_utf8str(wide);
    }

    // A number, or an expression the way the XUI's number box reads it (LLCalc, upper case: 10*3, 2^3, PI).
    bool evaluateNumber(const std::string& text, double& out)
    {
        if (parsePlainNumber(text, out))
        {
            return true;
        }
        std::string expression = text;
        LLStringUtil::toUpper(expression);
        F32 result = 0.f;
        if (expression.empty() || !LLCalc::getInstance()->evalString(expression, result) || !std::isfinite(result))
        {
            return false;
        }
        out = (double)result;
        return true;
    }

    // min, max, step and decimals of a box (the markup's; a value that is no number, or no sense, is the default).
    SpinRange spinRange(const Rml::Element* spin)
    {
        SpinRange range;
        range.min = rangeAttribute(spin, "min", range.min);
        range.max = rangeAttribute(spin, "max", range.max);
        if (range.min > range.max)
        {
            std::swap(range.min, range.max);
        }
        range.step = rangeAttribute(spin, "step", range.step);
        if (range.step <= 0.0)
        {
            range.step = 1.0;
        }
        range.decimals = (int)std::clamp(rangeAttribute(spin, "decimals", 0.0), 0.0, 6.0);
        return range;
    }
}
