/**
 * @file lookfx.h
 * @brief Screen effects of the look pass (film grain, vignette, colour
 *        fringes, halation, dust, scanlines): which are on and how strongly.
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

#ifndef PLUS_LOOK_LOOKFX_H
#define PLUS_LOOK_LOOKFX_H

#include "llrhilook.h"

#include <string>
#include <vector>

namespace plus::look
{
    // A named set of effect amounts (each 0..1), picked with PlusFxPreset.
    struct FxPreset
    {
        const char* id;         // the PlusFxPreset value
        const char* name_en;
        const char* name_ja;
        LLRHI::LookFx amounts;  // time unused
    };

    // The presets in the order a list should show them.
    const std::vector<FxPreset>& fxPresets();

    // A painted style (research/142), picked with PlusPaintStyle.
    struct PaintStyle
    {
        const char* id;         // the PlusPaintStyle value
        const char* name_en;
        const char* name_ja;
        U32 style;              // LLRHI::LookFx::PaintStyle
    };

    const std::vector<PaintStyle>& paintStyles();

    // Chooses a screen effect preset (PlusFxPreset; id one of fxPresets(), or empty for the sliders alone).
    // A known preset also puts its clarity and texture on their sliders (PlusFxClarity, PlusFxTexture); moved
    // afterwards, they add to the preset (research/275 L219). Writing PlusFxPreset directly (an import,
    // settings given back, Debug Settings) leaves the sliders as they are.
    // No UI calls it yet: a UI that picks a preset, when one is made, goes through this. The six presets now
    // have clarity and texture 0, so a call puts both sliders at 0.
    void chooseFxPreset(const std::string& id);

    // The effects to draw now, from PlusFxPreset (or, empty, the PlusFx*
    // sliders; clarity, texture and the soft glow (PlusFxSoftGlow*) are the sliders' either way) scaled by PlusFxStrength, and the painted style (PlusPaint*);
    // each part none when its own *SnapshotOnly is set and no snapshot is
    // being taken; and the miniature (PlusMiniature*, research/236, which
    // adds a light vignette). True when any effect is on. The time stays the one of a
    // snapshot's first call until it ends. Main thread.
    bool currentFx(LLRHI::LookFx& fx);
}

#endif // PLUS_LOOK_LOOKFX_H
