/**
 * @file photosky.h
 * @brief Photo tools: the favourite and the recently used skies and waters.
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

#ifndef PLUS_PHOTOMODE_PHOTOSKY_H
#define PLUS_PHOTOMODE_PHOTOSKY_H

#include <set>
#include <string>
#include <vector>

#include "lluuid.h"

namespace plus::photomode
{
    constexpr size_t MAX_SKY_FAVORITES = 100; // of each kind
    constexpr size_t MAX_SKY_RECENT    = 10;  // of each kind

    // The two lists of the Sky & Weather tab: the skies and the waters are chosen apart (EEP keeps
    // them as separate layers of the environment), so each has its own favourites and recent ones.
    enum class EnvKind
    {
        Sky,
        Water
    };

    // A sky or a water of the inventory, as the lists remember it.
    struct SkyEntry
    {
        LLUUID      item;       // the inventory item (the same after a move, new in another account)
        LLUUID      asset;
        std::string name;       // as last seen, to show when the item cannot be found
        bool        water = false;
        bool        library = false;

        // What the inventory says now (filled by describe()).
        bool        exists = false;
        std::string folder;

        // environmentItems() only: the other items of the same sky (the same asset and name, as a copy of
        // a library sky in the inventory), left out of its list for this one.
        std::vector<LLUUID> same;
    };

    // The kind of an inventory item (false when it is neither a sky nor a water).
    bool environmentKind(const LLUUID& item, EnvKind& kind);

    // The favourites of a kind, in their order. They are kept per account (an item's id is the account's
    // own; gSavedPerAccountSettings: PlusPhotoSkyFavorites, PlusPhotoWaterFavorites) and, for the library's
    // items, whose ids are the same in every account, also in the shared settings
    // (PlusPhotoSkyLibraryFavorites, PlusPhotoWaterLibraryFavorites): a library favourite of another
    // account shows up here too. Not removed when the inventory does not show the item (not loaded yet,
    // or deleted): the lists show it greyed with "not found" and let the user remove it.
    std::vector<SkyEntry> favorites(EnvKind kind);
    bool                  isFavorite(const LLUUID& item);   // in either list
    std::set<LLUUID>      favoriteItems();                  // the items of both lists (isFavorite() for many at once)
    // Added also when it was a favourite already. Full: there are MAX_SKY_FAVORITES of its kind already.
    // NotFound: the inventory does not have the item (deleted, not loaded yet) or it is not a sky or water.
    enum class AddFavorite { Added, Full, NotFound };
    AddFavorite           addFavorite(const LLUUID& item);
    void                  removeFavorite(const LLUUID& item);
    // Puts a favourite at a place of its list (0 first).
    void                  moveFavorite(const LLUUID& item, int index);
    void                  sortFavoritesByName(EnvKind kind);

    // The skies or waters used last, newest first, MAX_SKY_RECENT of them (per account). Fills itself:
    // every sky or water the tool applies is noted.
    std::vector<SkyEntry> recents(EnvKind kind);
    void                  noteApplied(const LLUUID& item);

    // Every sky (or water) of the inventory and the library, not in the trash and not a link, sorted by
    // name (in lower case, then as written, then by asset, then the one kept first), at most max of them,
    // into out (emptied first). Each entry is described (exists, name, asset; the folder is left empty).
    // Items of the same asset and name (a library sky and its copy in the inventory, two copies) are one
    // entry: a favourite one first, then one of the inventory, then the library's; the others are in its
    // "same". Walks the whole inventory: not every frame. Returns how many entries there are without max (more than
    // out has when some were cut).
    size_t environmentItems(EnvKind kind, std::vector<SkyEntry>& out, size_t max);

    // Fills in whether the item is in the inventory (not in the trash), its name there, its folder's name.
    // A link (kept by an older list) is described as, and becomes, the item it points to.
    void describe(SkyEntry& entry);

    // Counts up with every change of the lists, so windows know to look again.
    unsigned favoritesGeneration();

    // Declares the per-account settings (at start-up, before the account's file is read).
    void declareSkySettings();
}

#endif // PLUS_PHOTOMODE_PHOTOSKY_H
