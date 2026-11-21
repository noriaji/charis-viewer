/**
 * @file rmlnumber.h
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

// No RmlUi state here (rmlparts.cpp's number box uses these), so each can be tried on its own.

#ifndef PLUS_RMLNUMBER_H
#define PLUS_RMLNUMBER_H

#include "rmlcommon.h"
#include "rmlnumbermath.h"     // parsePlainNumber, formatNumber, roundTo (no viewer types: tried stand-alone)

#include <string>

namespace plus::rmlui
{
    // A text as the number box reads it: full-width ASCII (U+FF01 to U+FF5E, what a Japanese IME types) and the
    // ideographic space made half-width, the minus sign U+2212 made "-", spaces cut off the ends. Empty when the text
    // is too long to be a number.
    std::string normalizeNumberText(const std::string& utf8);
    // A number, or an expression the way the XUI's number box reads it (LLCalc, upper case: 10*3, 2^3, PI).
    bool evaluateNumber(const std::string& text, double& out);

    // min, max, step and decimals of a number box (a .ps-spin element's attributes; one that is no number or makes no
    // sense, such as a step of 0, is the default).
    struct SpinRange
    {
        double min = 0.0;
        double max = 100.0;
        double step = 1.0;
        int decimals = 0;
    };
    SpinRange spinRange(const Rml::Element* spin);
}

#endif // PLUS_RMLNUMBER_H
