/**
 * @file rmltextmath.h
 * @brief The text of a read-only log field as RmlUi keeps it, with no viewer or RmlUi types (research/387 3.1), so a
 *        stand-alone test (rmltextmath_test.cpp) can try it.
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

#ifndef PLUS_RMLTEXTMATH_H
#define PLUS_RMLTEXTMATH_H

#include <cstddef>
#include <string>

namespace plus::rmlui
{
    // A multi-line field takes no '\r' and no '\t' in its value: RmlUi erases both from whatever is put in
    // (WidgetTextInputMultiLine::SanitizeValue) and sends a change event, so a text that has them is never equal to
    // the field's value, however often it is compared. The text for the field has them gone first.
    inline bool isPlainLogText(const std::string& text, size_t max_bytes)
    {
        return text.size() <= max_bytes && text.find_first_of("\r\t") == std::string::npos;
    }

    // The text a log field shows for text: at most max_bytes bytes (the start of the text, or the end of it when
    // keep_end, cut at a character, so nothing half of a UTF-8 character is left), '\r' gone and '\t' a space.
    inline std::string logFieldText(const std::string& text, size_t max_bytes, bool keep_end)
    {
        const auto continuation = [&](size_t i) { return (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80; };
        size_t begin = 0;
        size_t end = text.size();
        if (end > max_bytes)
        {
            if (keep_end)
            {
                begin = end - max_bytes;
                while (begin < end && continuation(begin))
                {
                    ++begin;
                }
            }
            else
            {
                end = max_bytes;
                while (end > 0 && continuation(end))
                {
                    --end;
                }
            }
        }
        std::string out;
        out.reserve(end - begin);
        for (size_t i = begin; i < end; ++i)
        {
            const char c = text[i];
            if (c == '\r')
            {
                continue;
            }
            out += (c == '\t') ? ' ' : c;
        }
        return out;
    }
}

#endif // PLUS_RMLTEXTMATH_H
