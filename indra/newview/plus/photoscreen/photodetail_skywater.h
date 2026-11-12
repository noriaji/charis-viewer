/**
 * @file photodetail_skywater.h
 * @brief The sky and the water of the detailed photo screen's scene tab (research/233, ps-sky-water): a card
 *        for each showing the one in use, and one list under them, sky or water, with the tabs All,
 *        Favorites and Recent. The lists are the photo tools' (plus/photomode/photosky.h).
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_SKYWATER_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_SKYWATER_H

namespace plus::psui
{
    class Stack;
}

namespace plus::photoscreen::detail::skywater
{
    // Adds the two cards and, while it is open, the list (inside the scene section of the column). With the
    // keyboard on a card or a row (a click or Tab), Up / Down put on the row before / after; Esc closes the list.
    void fill(plus::psui::Stack& stack);
    // Every tick while the scene tab is shown: the cards' names, the list's rows (made again only when
    // what they show changed) and the row in use. Cheap when nothing changed.
    void refresh();
    // The column is made again: forget the widgets (they are deleted with it).
    void clear();
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_SKYWATER_H
