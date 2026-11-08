/**
 * @file photodetail_layout.h
 * @brief The sizes of the detailed photo screen in one table: the frame around the world
 *        (photoscreen.cpp, frameMargins()) and the parts of the panels (photodetail_*.cpp,
 *        photodetail_hud.cpp). UI pixels at a window 1080 high unless said otherwise.
 *
 * Each size says, where research/221 (the design after the target picture
 * research/photoscreen-design-target-2026-10-30.webp) gives one, the size of the picture at 1080 as
 * "221:". Those are not in use yet: the branches of research/221 (ps-left, ps-center, ps-right,
 * ps-bottom) change the values here together with the panels that need them.
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_LAYOUT_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_LAYOUT_H

#include "stdtypes.h"

namespace plus::photoscreen::sizes
{
    // --- the frame around the world (photoscreen.cpp, frameMargins(); research/218 "UI の固定寸法") ---------
    // Other window heights scale these (FRAME_SCALE_MIN..MAX). Under NARROW_WIDTH the panels at the sides fold
    // (only the rail stays on the left).
    inline constexpr S32 RAIL_W        = 123;   // 221: 123 (the left rail; ps-left)
    inline constexpr S32 LEFT_PANEL_W  = 296;   // 221: 280 (the left panel) and its gap of 8 at each side (ps-left)
    inline constexpr S32 RIGHT_PANEL_W = 414;   // 221: 406 (the right panel). ps-right: the column with its 8 px gaps, the boxes 398 (the picture's 337 x 1.18)
    inline constexpr S32 BOTTOM_H      = 250;   // 221: 250 (the preset strip; ps-bottom)
    inline constexpr S32 TOP_H         = 8;     // the gap over the world (221: the chips go inside the world, 12 from its top left)
    inline constexpr S32 NARROW_WIDTH  = 1280;  // window width, not scaled
    inline constexpr S32 MIN_WORLD     = 64;    // not scaled
    inline constexpr F32 FRAME_SCALE_MIN = 1.f; // the panels are not made narrower than the widgets in them
    inline constexpr F32 FRAME_SCALE_MAX = 2.f;

    // --- the preset strip (photodetail_bottom.cpp) -------------------------------------------------------------
    // The panel's XML (panel_plus_photoscreen_bottom.xml) is BOTTOM_H less the gaps above and below (234): the tabs
    // 6..42, the cards 59..199, the "+" card 59..191 at the right.
    inline constexpr S32 CARD_W   = 108;        // 221: picture 108 x 90, the name in 2 lines under it
    inline constexpr S32 CARD_GAP = 12;         // 221: 12
    inline constexpr S32 CARD_H   = 140;        // the picture (psstyle PRESET_PICTURE_HEIGHT 90) and the two lines under it
    inline constexpr S32 STRIP_STEP  = CARD_W + CARD_GAP;
    inline constexpr S32 STRIP_EDGE  = 24;      // 221: the fade at an end of the strip while cards are out of view there
    inline constexpr S32 STRIP_ARROW = 22;      // the round arrow on that fade
    inline constexpr S32 THUMB_W  = 192;        // the width of the small picture saved with a preset (pixels of the file)

    // --- over the world (photodetail_center.cpp, photodetail_hud.cpp, panel_plus_photoscreen_{top,hud}.xml; research/221 8.-12.) -
    // The XML places the chips, the read-out, the histogram's frame and the icon boxes at these sizes; the code places
    // the panels on the world's rectangle and keeps the chat between the read-out and the chips.
    inline constexpr S32 WORLD_INSET  = 12;     // the chips, the histogram and the icon boxes from the world's edges (221 8., 9., 12.)
    inline constexpr S32 CHIP_ROW_H   = 40;     // the chips' height (221 8.: 34 at 912)
    inline constexpr S32 HIST_W       = 195;    // the histogram's frame at the top right (221 9.); its data: the branch ps-histogram
    inline constexpr S32 HIST_H       = 100;    // its height (panel_plus_photoscreen_hud.xml)
    inline constexpr S32 CHAT_W      = 380;     // the chat at most this wide (and 45 % of the world)
    inline constexpr S32 CHAT_INPUT_BOTTOM = 56;    // the chat input's box over the read-out (221 11.: the numbers end 42 over the bottom)
    inline constexpr S32 CHAT_INPUT_H      = 30;
    inline constexpr S32 CHAT_BOTTOM = CHAT_INPUT_BOTTOM + CHAT_INPUT_H + 6;  // the chat lines over the chat input
    inline constexpr S32 CHAT_TOP_ROOM = WORLD_INSET + CHIP_ROW_H + 12;       // the chat lines stay under the chips
    inline constexpr S32 CHAT_MAX_H  = 160;
    inline constexpr S32 CHAT_BADGE_GAP = 8;    // between the chat input and the flycam's line at its right
    inline constexpr S32 CHAT_PAD    = 6;       // around the chat lines' box
    inline constexpr S32 FRAME_W     = 128;     // the focus frame (221 10.: 108 x 105 at 912)
    inline constexpr S32 FRAME_H     = 124;
    inline constexpr F32 CORNER_SHARE = 0.3f;   // a corner's arms: this share of the frame's side
    inline constexpr S32 CROSS       = 12;      // arms of the cross in the middle (24 across)
    inline constexpr S32 FRAME_TICK  = 4;       // the small marks at the middle of the frame's sides
    inline constexpr S32 FRAME_LABEL_GAP = 6;   // the label under the frame
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_LAYOUT_H
