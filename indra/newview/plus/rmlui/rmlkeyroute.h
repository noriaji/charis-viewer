/**
 * @file rmlkeyroute.h
 * @brief What a key does in a read-only RmlUi field (research/387 3.1), with no RmlUi types, so a stand-alone test
 *        (rmlkeyroute_test.cpp) can try it. indra_constants.h (KEY, MASK) is included before this.
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

#ifndef PLUS_RMLKEYROUTE_H
#define PLUS_RMLKEYROUTE_H

namespace plus::rmlui
{
    // What a key does in a read-only field (research/387 3.1): goes to RmlUi (selection, copy, Tab ...), scrolls the
    // field, or is not used (the viewer gets it: Esc and F1 to F12 too, the document takes none of them).
    enum class KeyRoute
    {
        Send,
        Scroll,
        Skip,
    };

    inline KeyRoute readOnlyKeyRoute(KEY key, MASK mask)
    {
        // Esc and the function keys are the viewer's (the floater closes, F12 ...): RmlUi never sees them, so the
        // document does not take them either.
        if (key == KEY_ESCAPE || (key >= KEY_F1 && key <= KEY_F12))
        {
            return KeyRoute::Skip;
        }
        // The modifiers alone and the focus's Tab go to RmlUi.
        if (key == KEY_SHIFT || key == KEY_CONTROL || key == KEY_ALT || key == KEY_TAB)
        {
            return KeyRoute::Send;
        }
        if (mask & MASK_ALT)
        {
            return KeyRoute::Skip;
        }
        const bool select_key = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_PAD_LEFT || key == KEY_PAD_RIGHT ||
                                key == KEY_UP || key == KEY_DOWN || key == KEY_PAD_UP || key == KEY_PAD_DOWN ||
                                key == KEY_HOME || key == KEY_END || key == KEY_PAD_HOME || key == KEY_PAD_END ||
                                key == KEY_PAGE_UP || key == KEY_PAGE_DOWN || key == KEY_PAD_PGUP || key == KEY_PAD_PGDN;
        if (mask & MASK_SHIFT)
        {
            // Shift and an arrow or a page key (with Ctrl: by words): the selection grows (RmlUi). Anything else with
            // Shift is not the field's.
            return select_key ? KeyRoute::Send : KeyRoute::Skip;
        }
        if (mask & MASK_CONTROL)
        {
            if (key == 'A' || key == 'C')
            {
                return KeyRoute::Send;
            }
        }
        switch (key)
        {
        case KEY_UP: case KEY_DOWN: case KEY_PAD_UP: case KEY_PAD_DOWN:
        case KEY_PAGE_UP: case KEY_PAGE_DOWN: case KEY_PAD_PGUP: case KEY_PAD_PGDN:
        case KEY_HOME: case KEY_END: case KEY_PAD_HOME: case KEY_PAD_END:
            return KeyRoute::Scroll;
        default:
            // the left and right arrows, characters, BackSpace, Delete, Enter, Ctrl+X / V / Z ...: not a read-only field's
            return KeyRoute::Skip;
        }
    }
}

#endif // PLUS_RMLKEYROUTE_H
