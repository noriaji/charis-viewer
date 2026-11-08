/**
 * @file fpscornermath.h
 * @brief FPS display: where the box goes, whether the status bar has room, and what number it shows
 *        (no viewer types, so fpscorner_test.cpp checks it on its own).
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

#ifndef PLUS_FPSCORNER_FPSCORNERMATH_H
#define PLUS_FPSCORNER_FPSCORNERMATH_H

namespace plus::fpscorner
{
    // The values of PlusFpsCornerPosition (the menu items' parameters). The four corners came first and
    // keep their values; the status bar was added after them and is the default.
    enum class Position : int
    {
        TopRight = 0,
        TopLeft = 1,
        BottomLeft = 2,
        BottomRight = 3,
        StatusBar = 4,  // in the status bar, right of the volume (speaker) button, as Firestorm has it
        Count
    };

    constexpr Position DEFAULT_POSITION = Position::StatusBar;

    // A setting out of range is the default.
    inline Position positionFrom(int value)
    {
        return (value < 0 || value >= (int)Position::Count) ? DEFAULT_POSITION : (Position)value;
    }

    inline bool isCorner(Position position) { return position != Position::StatusBar; }
    inline bool isTop(Position position)    { return position == Position::TopRight || position == Position::TopLeft; }
    inline bool isRight(Position position)  { return position == Position::TopRight || position == Position::BottomRight; }

    // A rectangle the way LLRect has it: y goes up, top > bottom.
    struct Box
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    // The box of width x height in the corner of area, margin away from its two edges.
    // False (and out untouched) when it does not fit or position is not a corner: nothing is drawn then.
    inline bool place(const Box& area, int width, int height, Position position, int margin, Box& out)
    {
        if (!isCorner(position) || width <= 0 || height <= 0 || area.right - area.left < width + 2 * margin ||
            area.top - area.bottom < height + 2 * margin)
        {
            return false;
        }
        Box box;
        if (isRight(position))
        {
            box.right = area.right - margin;
            box.left  = box.right - width;
        }
        else
        {
            box.left  = area.left + margin;
            box.right = box.left + width;
        }
        if (isTop(position))
        {
            box.top    = area.top - margin;
            box.bottom = box.top - height;
        }
        else
        {
            box.bottom = area.bottom + margin;
            box.top    = box.bottom + height;
        }
        out = box;
        return true;
    }

    // The status bar makes room for the number by moving its right-hand group (from the menu search box
    // to the volume button) left by shift. It has room when the group's left edge where the official
    // layout has it (official_left), moved by shift, stays pad away from the menus' right edge. The test
    // does not depend on whether the group is moved now, so the number does not come and go by itself.
    inline bool statusBarHasRoom(int official_left, int menu_right, int shift, int pad)
    {
        return official_left - shift >= menu_right + pad;
    }

    // The number shown for a rate: rounded, never below 0 (no data yet, a NaN) or above 9999.
    inline int shownFps(double per_second)
    {
        if (!(per_second > 0.0))
        {
            return 0;
        }
        if (per_second >= 9999.0)
        {
            return 9999;
        }
        return (int)(per_second + 0.5);
    }
}

#endif // PLUS_FPSCORNER_FPSCORNERMATH_H
