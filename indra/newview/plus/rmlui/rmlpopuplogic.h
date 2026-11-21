// The parts of the select list that need no viewer and no RmlUi (rmlpopup.cpp uses them; rmlpopuplogic_test.cpp tests
// them stand-alone: cl /nologo /EHsc /std:c++17 /W4 /WX rmlpopuplogic_test.cpp && rmlpopuplogic_test.exe).
// Rows are anything with `bool selectable` (and `int option` for rowOfOption); a row index is 0 .. count-1, -1: none.

#ifndef PLUS_RMLPOPUPLOGIC_H
#define PLUS_RMLPOPUPLOGIC_H

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace plus::rmlui::popuplogic
{
    // A rectangle in UI units, y up (the viewer's LLRect: bottom < top).
    struct Box
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        int width() const { return right - left; }
        int height() const { return top - bottom; }
    };

    struct PlaceLimits
    {
        int max_width = 500;        // the widest list (MAX_COMBO_WIDTH)
        int area_margin = 50;       // a list is at most the area's height minus this
        int scrollbar_width = 0;    // added to the width when the list scrolls
    };

    // Where the list goes (llcombobox.cpp showList): below the anchor (the select) when it fits, else on the side with
    // more room (shrunk to it), moved into the area, never lower than the area's bottom. text_width: the widest text
    // with its paddings, without a scroll bar. min_height: the least a list is shown with (one row).
    inline Box placeList(const Box& area, const Box& anchor, int text_width, int content_height, int min_height,
                         const PlaceLimits& limits)
    {
        const int max_height = std::max(min_height, area.height() - limits.area_margin);
        int height = std::min(content_height, max_height);
        const int space_below = anchor.bottom - area.bottom;
        const int space_above = area.top - anchor.top;
        bool below = true;
        if (height > space_below)
        {
            below = space_below > space_above;
            height = std::min(height, below ? space_below : space_above);
        }
        const int lowest = std::min(min_height, content_height);
        height = std::clamp(height, lowest, std::max(lowest, std::min(content_height, area.height())));

        const bool scrolls = height < content_height;
        const int min_width = anchor.width();
        const int widest = std::max(min_width, limits.max_width);
        const int width = std::clamp(text_width + (scrolls ? limits.scrollbar_width : 0), min_width, widest);

        int left = anchor.left;
        if (left + width > area.right)
        {
            left = area.right - width;
        }
        left = std::max(left, area.left);
        int bottom = below ? anchor.bottom - height : anchor.top;
        if (bottom + height > area.top)
        {
            bottom = area.top - height;
        }
        bottom = std::max(bottom, area.bottom);
        Box out;
        out.left = left;
        out.right = left + width;
        out.bottom = bottom;
        out.top = bottom + height;
        return out;
    }

    // The row `step` rows from `from` (up: negative) among the ones that can be chosen; an end stops it. From -1 (or any
    // place outside the list) a step down starts at the first row, a step up at the last. Nothing to move to (no row
    // can be chosen, no rows, a step of 0): `from` if it is a row, else -1 -- never a number outside the list.
    template <class Rows>
    int stepRows(const Rows& rows, int from, int step)
    {
        const int count = (int)rows.size();
        const bool inside = from >= 0 && from < count;
        const int stay = inside ? from : -1;
        if (step == 0 || count == 0)
        {
            return stay;
        }
        const int direction = step > 0 ? 1 : -1;
        int row = inside ? from : (direction > 0 ? -1 : count);
        bool moved = false;
        for (int left = std::abs(step); left > 0; --left)
        {
            int next = row + direction;
            while (next >= 0 && next < count && !rows[next].selectable)
            {
                next += direction;
            }
            if (next < 0 || next >= count)
            {
                break;      // the end stops it
            }
            row = next;
            moved = true;
        }
        return moved ? row : stay;
    }

    template <class Rows>
    int rowOfOption(const Rows& rows, int option)
    {
        if (option < 0)
        {
            return -1;      // (no option: -1 is also the number the row that says options were left out has)
        }
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].option == option)
            {
                return (int)i;
            }
        }
        return -1;
    }

    // The type-ahead (llscrolllistctrl.cpp): labels are the rows' texts, lower case and trimmed. The first row that can
    // be chosen and starts with `target`; -1 if none.
    template <class Rows, class Str>
    int findByPrefix(const Rows& rows, const std::vector<Str>& labels, const Str& target)
    {
        const size_t count = std::min(rows.size(), labels.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (rows[i].selectable && labels[i].compare(0, target.size(), target) == 0)
            {
                return (int)i;
            }
        }
        return -1;
    }

    // The same letter again: the next row after the cursor (round the list once, the cursor's own row last) that can be
    // chosen and starts with it; -1 if none. A cursor outside the list: every row from the first.
    template <class Rows, class Str>
    int nextByFirstChar(const Rows& rows, const std::vector<Str>& labels, int cursor, typename Str::value_type letter)
    {
        const int count = (int)std::min(rows.size(), labels.size());
        auto matches = [&](int i) { return rows[i].selectable && !labels[i].empty() && labels[i][0] == letter; };
        if (cursor < 0 || cursor >= count)
        {
            for (int i = 0; i < count; ++i)
            {
                if (matches(i))
                {
                    return i;
                }
            }
            return -1;
        }
        for (int i = (cursor + 1) % count; i != cursor; i = (i + 1) % count)
        {
            if (matches(i))
            {
                return i;
            }
        }
        return -1;
    }
}

#endif // PLUS_RMLPOPUPLOGIC_H
