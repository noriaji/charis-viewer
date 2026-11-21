/**
 * @file radarui.h
 * @brief What the radar window shows and does, apart from how it is shown (research/386).
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

// What the radar window shows and does, apart from how it is shown (shared by the XUI window, radarfloater.cpp, and the
// RmlUi window, radarrml.cpp, so that the two cannot differ): the columns, the text of a cell, the styles of a row, the
// sort keys, the filter, and the actions of the list's menu (the selection is the window's: it passes the ids).
// Main thread only.

#ifndef PLUS_RADARUI_H
#define PLUS_RADARUI_H

#include "radar.h"

#include <string>

#include "v4color.h"
#include "llstring.h"
#include "lluuid.h"

namespace plus::radar::ui
{
    struct ColumnDef
    {
        const char* name;   // also the strings col_<name> (label) and tip_<name> (tool tip)
        S32 width;
        U32 bit;            // PlusRadarColumns; 0: always shown
    };
    constexpr size_t COLUMN_COUNT = 7;
    inline constexpr ColumnDef COLUMNS[COLUMN_COUNT] = {
        { "name",    160, 0  },     // always shown
        { "region",  24,  1  },
        { "sitting", 24,  2  },
        { "age",     44,  4  },
        { "payment", 30,  8  },
        { "seen",    58,  16 },
        { "range",   56,  32 },
    };
    // The places in COLUMNS (a window loops over the columns by these, without comparing names).
    enum ColumnIndex : size_t { COL_NAME, COL_REGION, COL_SITTING, COL_AGE, COL_PAYMENT, COL_SEEN, COL_RANGE };

    constexpr bool sameName(const char* a, const char* b)
    {
        return *a == *b && (*a == '\0' || sameName(a + 1, b + 1));
    }
    // The enum and the table are one list: a column added to one and not to the other does not compile.
    static_assert(COL_RANGE + 1 == COLUMN_COUNT, "ColumnIndex and COLUMNS differ in length");
    static_assert(sameName(COLUMNS[COL_NAME].name, "name") && sameName(COLUMNS[COL_REGION].name, "region") &&
                  sameName(COLUMNS[COL_SITTING].name, "sitting") && sameName(COLUMNS[COL_AGE].name, "age") &&
                  sameName(COLUMNS[COL_PAYMENT].name, "payment") && sameName(COLUMNS[COL_SEEN].name, "seen") &&
                  sameName(COLUMNS[COL_RANGE].name, "range"), "ColumnIndex and COLUMNS differ in order");

    // The words of the marks in the cells (the window's <string>s).
    struct Marks
    {
        std::string parcel, region, sitting, age_hidden, age_unknown;
    };

    std::string cellText(const Entry& entry, size_t column, const Marks& marks);
    std::string cellText(const Entry& entry, const std::string& column, const Marks& marks);   // by the column's name
    // The sort keys of the cells: the first number of a text (">" is last), the seconds of "h:mm:ss".
    F64 sortNumber(const std::string& text);
    S32 sortSeconds(const std::string& text);
    // RLVa @shownames: the avatar's name may not be shown.
    bool nameHidden(const LLUUID& id);

    struct Style
    {
        bool bold = false;
        bool italic = false;
        bool coloured = false;      // colour is set (otherwise the list's own text colour)
        LLColor4 colour;
    };
    // The name's: friends bold (not under @shownames), the blocked italic, the contact set's colour.
    Style nameStyle(const Entry& entry);
    // The range's: red within say range, yellow within shout range, bold when the avatar is drawn.
    Style rangeStyle(const Entry& entry);
    LLColor4 listTextColour();

    // The name contains the (lower case) filter.
    bool matchesFilter(const Entry& entry, const std::string& lower_filter);
    // [TOTAL] [REGION] [CHAT] of the counter's string.
    void counterArgs(const Stats& stats, LLStringUtil::format_map_t& args);

    // What an action tells the user in a notice (the window's string no_focus / no_position).
    enum class Notice { None, NoFocus, NoPosition };
    // The camera in front of the avatar when it is drawn.
    Notice zoomToId(const LLUUID& id);
    // Whether a menu item (menu_plus_radar.xml's Plus.Radar.Enable parameter) is enabled for these avatars.
    bool menuEnabled(const std::string& what, const uuid_vec_t& ids);
    // A menu item's action (Plus.Radar.Action's parameter) except "select_all" (the window's).
    Notice runAction(const std::string& action, const uuid_vec_t& ids);
    // The names (or the ids) of the avatars, one to a line, on the clipboard.
    void copyToClipboard(const uuid_vec_t& ids, bool as_ids);
}

#endif // PLUS_RADARUI_H
