/**
 * @file objectfx_palette.h
 * @brief The colours the object appear / disappear effects choose from (plus/materialize,
 *        plus/derez) and the value of their palette settings that takes the custom colour.
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

// research/256 6b, research/257: PlusMaterializePalette and PlusDerezPalette are 0 cyan, 1 orange,
// 2 magenta, 3 green, PALETTE_CUSTOM the effect's own colour setting (PlusMaterializeColor,
// PlusDerezColor; the swatch of plus/objectfx). One table for the three (research/275 L065), so the
// effects and the UI cannot drift apart. The combo boxes' items (floater_plus_prefs_graphics_advanced.xml)
// follow the same order.

#ifndef PLUS_OBJECTFX_OBJECTFX_PALETTE_H
#define PLUS_OBJECTFX_OBJECTFX_PALETTE_H

#include "llmath.h"
#include "stdtypes.h"
#include "v3color.h"

namespace plus::objectfx
{
    constexpr S32 PALETTE_SIZE = 4;
    constexpr S32 PALETTE_CUSTOM = PALETTE_SIZE;    // the palette settings' value for the custom colour

    // sRGB, 0 - 1.
    inline const LLColor3 PALETTE[PALETTE_SIZE] =
    {
        LLColor3(25.f / 255.f, 230.f / 255.f, 1.f),             // #19E6FF electric cyan
        LLColor3(1.f, 138.f / 255.f, 30.f / 255.f),             // #FF8A1E orange
        LLColor3(1.f, 43.f / 255.f, 214.f / 255.f),             // #FF2BD6 magenta
        LLColor3(60.f / 255.f, 1.f, 122.f / 255.f),             // #3CFF7A green
    };

    // The colour a palette setting's value picks (sRGB, 0 - 1): PALETTE[index] below PALETTE_CUSTOM,
    // else the effect's custom colour, each channel held to 0 - 1 (NaN as 0); black, or next to it in
    // linear light, would glow as nothing, so the first colour then. The caller brings index into
    // 0 - PALETTE_CUSTOM.
    inline LLColor3 paletteColour(S32 index, const LLColor3& custom)
    {
        if (index < PALETTE_CUSTOM)
        {
            return PALETTE[llmax(index, 0)];
        }
        LLColor3 colour = custom;
        for (U32 i = 0; i < 3; ++i)
        {
            colour.mV[i] = llfinite(colour.mV[i]) ? llclamp(colour.mV[i], 0.f, 1.f) : 0.f;
        }
        if (sRGBtoLinear(llmax(colour.mV[VRED], colour.mV[VGREEN], colour.mV[VBLUE])) < 1.e-3f)
        {
            return PALETTE[0];
        }
        return colour;
    }
}

#endif // PLUS_OBJECTFX_OBJECTFX_PALETTE_H
