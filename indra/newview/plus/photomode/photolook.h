/**
 * @file photolook.h
 * @brief Photo tools: the looks (colour grading LUTs of plus/look) as the
 *        tool lists them, with the favourite and the recently used ones.
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

#ifndef PLUS_PHOTOMODE_PHOTOLOOK_H
#define PLUS_PHOTOMODE_PHOTOLOOK_H

#include <string>
#include <vector>

namespace plus::photomode
{
    constexpr size_t MAX_LOOK_FAVORITES = 100;
    constexpr size_t MAX_LOOK_RECENT    = 10;

    // A look as the lists of the tool show it. The tool keeps no look of its own: the one in use is
    // PlusLook, the same setting the Charis menu's "Look" list sets, so both always show the same.
    struct LookChoice
    {
        std::string id;           // PlusLook's value; empty: no look
        std::string label;        // empty for "no look" and for a look that is not in the folders
        bool        user = false; // from the user's look folder
        bool        exists = true; // false: a favourite or recent look that is not in the folders (any more)
    };

    // "No look" first, then the shipped looks, then the user's (plus::look::entries()).
    std::vector<LookChoice> allLooks();
    // The favourites in their order (PlusLookFavorites), and the looks put on last, newest first
    // (PlusLookRecent). A look that is gone stays in them, with exists false.
    std::vector<LookChoice> favoriteLooks();
    std::vector<LookChoice> recentLooks();
    // The ids alone, as the two settings keep them (to know cheaply whether the lists changed).
    std::vector<std::string> favoriteLookIds();
    std::vector<std::string> recentLookIds();

    bool isFavoriteLook(const std::string& id);
    // False for "no look", or when there are MAX_LOOK_FAVORITES already.
    bool addFavoriteLook(const std::string& id);
    void removeFavoriteLook(const std::string& id);

    std::string currentLook();
    // Puts the look on (PlusLook) and notes it among the recent ones ("no look" is not noted).
    void applyLook(const std::string& id);
    // The id of the look before or after the one in use in the list (skipping looks that are gone), round;
    // the first or the last when the one in use is not in it. The current one when the list has nothing else.
    std::string stepLook(const std::vector<LookChoice>& list, int direction);
}

#endif // PLUS_PHOTOMODE_PHOTOLOOK_H
