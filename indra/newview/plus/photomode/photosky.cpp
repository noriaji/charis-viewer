/**
 * @file photosky.cpp
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

// Where they are kept, and why it differs from the other photo tools settings (which are shared by every
// account): an inventory item's id belongs to the account, so the favourites are in the account's own
// settings. The library's items have the same ids in every account, so those are copied to the shared
// settings as well, and an account that opens the lists takes the library favourites of the others in.
//
// The skies and the waters have lists of their own. Before 2026-10-21 both were kept in the sky's lists
// (each entry says whether it is a water); the waters found there are moved to the water's lists.

#include "llviewerprecompiledheaders.h"

#include "photosky.h"

#include <algorithm>

#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorysettings.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

namespace plus::photomode
{
    namespace
    {
        // The settings of one kind.
        struct ListNames
        {
            const char* favorites; // per account
            const char* recent;    // per account
            const char* library;   // shared (settings_plus.xml)
        };
        constexpr ListNames SKY_LISTS   = { "PlusPhotoSkyFavorites", "PlusPhotoSkyRecent", "PlusPhotoSkyLibraryFavorites" };
        constexpr ListNames WATER_LISTS = { "PlusPhotoWaterFavorites", "PlusPhotoWaterRecent", "PlusPhotoWaterLibraryFavorites" };

        const ListNames& lists(EnvKind kind)
        {
            return kind == EnvKind::Water ? WATER_LISTS : SKY_LISTS;
        }

        unsigned sGeneration = 1;

        LLSD readArray(LLControlGroup& group, const char* name)
        {
            if (!group.controlExists(name))
            {
                return LLSD::emptyArray();
            }
            LLSD data = group.getLLSD(name);
            return data.isArray() ? data : LLSD::emptyArray();
        }

        void writeArray(LLControlGroup& group, const char* name, const LLSD& data)
        {
            if (group.controlExists(name))
            {
                group.setLLSD(name, data);
            }
            ++sGeneration;
        }

        bool kindOf(const LLViewerInventoryItem* item, EnvKind& kind)
        {
            if (!item || item->getInventoryType() != LLInventoryType::IT_SETTINGS)
            {
                return false;
            }
            switch (item->getSettingsType())
            {
            case LLSettingsType::ST_SKY:
                kind = EnvKind::Sky;
                return true;
            case LLSettingsType::ST_WATER:
                kind = EnvKind::Water;
                return true;
            default:
                return false;
            }
        }

        LLSD toLLSD(const SkyEntry& entry)
        {
            LLSD data;
            data["item"]    = entry.item;
            data["asset"]   = entry.asset;
            data["name"]    = entry.name;
            data["water"]   = entry.water;
            data["library"] = entry.library;
            return data;
        }

        SkyEntry fromLLSD(const LLSD& data)
        {
            SkyEntry entry;
            entry.item    = data["item"].asUUID();
            entry.asset   = data["asset"].asUUID();
            entry.name    = data["name"].asString();
            entry.water   = data["water"].asBoolean();
            entry.library = data["library"].asBoolean();
            return entry;
        }

        // The item an id stands for: the one a link points to, else the id itself (also when it is not
        // loaded). The lists keep the items themselves; a link may be in a list kept before 2026-10-30.
        const LLUUID& baseItem(const LLUUID& id)
        {
            return gInventory.getLinkedItemID(id);
        }

        bool sameItem(const LLUUID& a, const LLUUID& b)
        {
            return a == b || baseItem(a) == baseItem(b);
        }

        bool contains(const LLSD& array, const LLUUID& item)
        {
            for (const LLSD& entry : llsd::inArray(array))
            {
                if (sameItem(entry["item"].asUUID(), item))
                {
                    return true;
                }
            }
            return false;
        }

        // An entry made from an inventory item, or from a link the item it points to (false when it is not
        // a sky or a water).
        bool makeEntry(const LLUUID& given, SkyEntry& entry, EnvKind& kind)
        {
            const LLUUID                 item_id = baseItem(given);
            const LLViewerInventoryItem* item    = gInventory.getItem(item_id);
            if (!kindOf(item, kind))
            {
                return false;
            }
            entry.item    = item_id;
            entry.asset   = item->getAssetUUID();
            entry.name    = item->getName();
            entry.water   = kind == EnvKind::Water;
            entry.library = gInventory.isObjectDescendentOf(item_id, gInventory.getLibraryRootFolderID());
            return true;
        }

        // Moves the waters of one of the sky's arrays to the water's (they were kept together before).
        // Nothing is written when there are none, which is every time after the first.
        void moveWaters(LLControlGroup& group, const char* from, const char* to, size_t limit)
        {
            const LLSD old_from = readArray(group, from);
            LLSD       kept     = LLSD::emptyArray();
            LLSD       target   = readArray(group, to);
            bool       moved    = false;
            for (const LLSD& entry : llsd::inArray(old_from))
            {
                if (!entry["water"].asBoolean())
                {
                    kept.append(entry);
                    continue;
                }
                moved = true;
                if (target.size() < (LLSD::Integer)limit && !contains(target, entry["item"].asUUID()))
                {
                    target.append(entry);
                }
            }
            if (moved && group.controlExists(to))
            {
                writeArray(group, to, target);
                writeArray(group, from, kept);
            }
        }

        void splitOldLists()
        {
            moveWaters(gSavedPerAccountSettings, SKY_LISTS.favorites, WATER_LISTS.favorites, MAX_SKY_FAVORITES);
            moveWaters(gSavedPerAccountSettings, SKY_LISTS.recent, WATER_LISTS.recent, MAX_SKY_RECENT);
            moveWaters(gSavedSettings, SKY_LISTS.library, WATER_LISTS.library, MAX_SKY_FAVORITES);
        }

        // Takes the item out of one of the arrays.
        void removeFrom(LLControlGroup& group, const char* name, const LLUUID& item)
        {
            LLSD kept  = LLSD::emptyArray();
            bool found = false;
            for (const LLSD& entry : llsd::inArray(readArray(group, name)))
            {
                if (sameItem(entry["item"].asUUID(), item))
                {
                    found = true;
                }
                else
                {
                    kept.append(entry);
                }
            }
            if (found)
            {
                writeArray(group, name, kept);
            }
        }

        // The entries of an array, as the inventory has them now. An item kept twice (itself and a link to
        // it, from a list kept before 2026-10-30) is shown once, where it comes first.
        std::vector<SkyEntry> described(const LLSD& array)
        {
            std::vector<SkyEntry> result;
            std::set<LLUUID>      seen;
            for (const LLSD& entry : llsd::inArray(array))
            {
                SkyEntry e = fromLLSD(entry);
                describe(e);
                if (seen.insert(e.item).second)
                {
                    result.push_back(std::move(e));
                }
            }
            return result;
        }
    }

    void declareSkySettings()
    {
        // Before the account's file is read (it fills these in), at start-up.
        struct Declared
        {
            const char* name;
            const char* comment;
        };
        const Declared declared[] = {
            { SKY_LISTS.favorites, "Photo tools: the favourite skies of this account (an array of maps: item, asset, name, water, library)." },
            { SKY_LISTS.recent, "Photo tools: the skies of this account used last, newest first (the same maps)." },
            { WATER_LISTS.favorites, "Photo tools: the favourite waters of this account (the same maps)." },
            { WATER_LISTS.recent, "Photo tools: the waters of this account used last, newest first (the same maps)." },
        };
        for (const Declared& setting : declared)
        {
            if (!gSavedPerAccountSettings.controlExists(setting.name))
            {
                gSavedPerAccountSettings.declareLLSD(setting.name, LLSD::emptyArray(), setting.comment);
            }
        }
    }

    unsigned favoritesGeneration()
    {
        return sGeneration;
    }

    bool environmentKind(const LLUUID& item, EnvKind& kind)
    {
        return kindOf(gInventory.getItem(item), kind);
    }

    std::vector<SkyEntry> favorites(EnvKind kind)
    {
        splitOldLists();
        const ListNames& names  = lists(kind);
        LLSD             own    = readArray(gSavedPerAccountSettings, names.favorites);
        const LLSD       shared = readArray(gSavedSettings, names.library);

        // the library favourites of other accounts
        bool adopted = false;
        for (const LLSD& entry : llsd::inArray(shared))
        {
            if (entry["water"].asBoolean() != (kind == EnvKind::Water))
            {
                continue; // an old entry the split could not move (it moves them to the other list)
            }
            if (own.size() < (LLSD::Integer)MAX_SKY_FAVORITES && !contains(own, entry["item"].asUUID()))
            {
                own.append(entry);
                adopted = true;
            }
        }
        if (adopted)
        {
            writeArray(gSavedPerAccountSettings, names.favorites, own);
        }
        return described(own);
    }

    bool isFavorite(const LLUUID& item)
    {
        return contains(readArray(gSavedPerAccountSettings, SKY_LISTS.favorites), item) ||
               contains(readArray(gSavedPerAccountSettings, WATER_LISTS.favorites), item);
    }

    std::set<LLUUID> favoriteItems()
    {
        std::set<LLUUID> items;
        for (const ListNames* names : { &SKY_LISTS, &WATER_LISTS })
        {
            for (const LLSD& entry : llsd::inArray(readArray(gSavedPerAccountSettings, names->favorites)))
            {
                const LLUUID item = entry["item"].asUUID();
                items.insert(item);
                items.insert(baseItem(item));   // a link kept by an older list stars the item too
            }
        }
        return items;
    }

    AddFavorite addFavorite(const LLUUID& item)
    {
        SkyEntry entry;
        EnvKind  kind = EnvKind::Sky;
        if (!makeEntry(item, entry, kind))
        {
            return AddFavorite::NotFound;
        }
        const ListNames& names = lists(kind);
        LLSD             own   = readArray(gSavedPerAccountSettings, names.favorites);
        if (contains(own, item))
        {
            return AddFavorite::Added;
        }
        if (own.size() >= (LLSD::Integer)MAX_SKY_FAVORITES)
        {
            return AddFavorite::Full;
        }
        own.append(toLLSD(entry));
        writeArray(gSavedPerAccountSettings, names.favorites, own);
        if (entry.library)
        {
            LLSD shared = readArray(gSavedSettings, names.library);
            if (!contains(shared, item))
            {
                shared.append(toLLSD(entry));
                writeArray(gSavedSettings, names.library, shared);
            }
        }
        return AddFavorite::Added;
    }

    void removeFavorite(const LLUUID& item)
    {
        for (const ListNames* names : { &SKY_LISTS, &WATER_LISTS })
        {
            removeFrom(gSavedPerAccountSettings, names->favorites, item);
            removeFrom(gSavedSettings, names->library, item);
        }
    }

    void moveFavorite(const LLUUID& item, int index)
    {
        for (const ListNames* names : { &SKY_LISTS, &WATER_LISTS })
        {
            const LLSD        own = readArray(gSavedPerAccountSettings, names->favorites);
            LLSD              moved;
            std::vector<LLSD> others;
            for (const LLSD& entry : llsd::inArray(own))
            {
                if (moved.isUndefined() && sameItem(entry["item"].asUUID(), item))
                {
                    moved = entry;
                }
                else
                {
                    others.push_back(entry);
                }
            }
            if (moved.isUndefined())
            {
                continue; // in the other list
            }
            index = llclamp(index, 0, (int)others.size());
            others.insert(others.begin() + index, moved);
            LLSD result = LLSD::emptyArray();
            for (const LLSD& entry : others)
            {
                result.append(entry);
            }
            writeArray(gSavedPerAccountSettings, names->favorites, result);
            return;
        }
    }

    void sortFavoritesByName(EnvKind kind)
    {
        std::vector<SkyEntry> entries = favorites(kind);
        // the names as the inventory has them now (favorites() fills them in), then the stored ones
        std::stable_sort(entries.begin(), entries.end(), [](const SkyEntry& a, const SkyEntry& b) {
            return LLStringUtil::compareDict(a.name, b.name) < 0;
        });
        LLSD result = LLSD::emptyArray();
        for (const SkyEntry& entry : entries)
        {
            result.append(toLLSD(entry));
        }
        writeArray(gSavedPerAccountSettings, lists(kind).favorites, result);
    }

    std::vector<SkyEntry> recents(EnvKind kind)
    {
        splitOldLists();
        return described(readArray(gSavedPerAccountSettings, lists(kind).recent));
    }

    void noteApplied(const LLUUID& item)
    {
        SkyEntry entry;
        EnvKind  kind = EnvKind::Sky;
        if (!makeEntry(item, entry, kind))
        {
            return;
        }
        const char* name   = lists(kind).recent;
        LLSD        recent = LLSD::emptyArray();
        recent.append(toLLSD(entry));
        for (const LLSD& old : llsd::inArray(readArray(gSavedPerAccountSettings, name)))
        {
            // the same sky as another item (the library's and its copy in the inventory: one row of the list,
            // either may be the one put on) is pushed out too; an unknown asset is never the same
            const bool same_sky = entry.asset.notNull() && old["asset"].asUUID() == entry.asset && old["name"].asString() == entry.name;
            if (!sameItem(old["item"].asUUID(), entry.item) && !same_sky && recent.size() < (LLSD::Integer)MAX_SKY_RECENT)
            {
                recent.append(old);
            }
        }
        writeArray(gSavedPerAccountSettings, name, recent);
    }

    size_t environmentItems(EnvKind kind, std::vector<SkyEntry>& out, size_t max)
    {
        out.clear();
        LLInventoryModel::cat_array_t  cats;
        LLInventoryModel::item_array_t items;
        LLIsType                       settings(LLAssetType::AT_SETTINGS);
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, settings);
        const LLUUID& library = gInventory.getLibraryRootFolderID();
        if (library.notNull())
        {
            gInventory.collectDescendentsIf(library, cats, items, LLInventoryModel::EXCLUDE_TRASH, settings);
        }
        const std::set<LLUUID> starred = favoriteItems();
        std::vector<std::string> lower;     // the sort key of each entry, by its index
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            EnvKind found = EnvKind::Sky;
            if (!item || item->getIsLinkType() || !kindOf(item.get(), found) || found != kind)
            {
                continue;
            }
            SkyEntry entry;
            entry.item    = item->getUUID();
            entry.asset   = item->getAssetUUID();
            entry.name    = item->getName();
            entry.water   = kind == EnvKind::Water;
            entry.library = library.notNull() && gInventory.isObjectDescendentOf(entry.item, library);
            entry.exists  = true;
            out.push_back(std::move(entry));
        }
        std::vector<size_t> order(out.size());
        lower.reserve(out.size());
        for (size_t i = 0; i < out.size(); ++i)
        {
            order[i] = i;
            std::string key = out[i].name;
            LLStringUtil::toLower(key);
            lower.push_back(std::move(key));
        }
        // By name; the same sky (asset and name) together, the one to keep first: a favourite, then one of
        // the inventory (the library's are the same in every account), then by item.
        auto keep_first = [&](size_t a, size_t b) {
            const bool fa = starred.count(out[a].item) > 0, fb = starred.count(out[b].item) > 0;
            if (fa != fb)
            {
                return fa;
            }
            if (out[a].library != out[b].library)
            {
                return !out[a].library;
            }
            return out[a].item < out[b].item;
        };
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (lower[a] != lower[b])
            {
                return lower[a] < lower[b];
            }
            if (out[a].name != out[b].name)
            {
                return out[a].name < out[b].name;
            }
            if (out[a].asset != out[b].asset)
            {
                return out[a].asset < out[b].asset;
            }
            return keep_first(a, b);
        });
        std::vector<SkyEntry> sorted;
        sorted.reserve(llmin(order.size(), max));
        size_t      entries = 0;    // counted on past max
        LLUUID      last_asset;
        std::string last_name;
        for (size_t i = 0; i < order.size(); ++i)
        {
            SkyEntry& entry = out[order[i]];
            // A copy of the one before (an asset unknown, as of an item not loaded yet, is never the same).
            const bool copy = entries > 0 && entry.asset.notNull() && entry.asset == last_asset && entry.name == last_name;
            if (!copy)
            {
                ++entries;
                last_asset = entry.asset;
                last_name  = entry.name;
            }
            if (entries > max)
            {
                continue;   // only counted
            }
            if (copy)
            {
                sorted.back().same.push_back(entry.item);
                continue;
            }
            sorted.push_back(std::move(entry));
        }
        out.swap(sorted);
        return entries;
    }

    void describe(SkyEntry& entry)
    {
        entry.item = baseItem(entry.item);  // a link of an older list: the item it points to
        const LLViewerInventoryItem* item = gInventory.getItem(entry.item);
        EnvKind                      kind = EnvKind::Sky;
        entry.exists = false;
        entry.folder.clear();
        if (!kindOf(item, kind))
        {
            return;
        }
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        if (trash.notNull() && gInventory.isObjectDescendentOf(entry.item, trash))
        {
            return; // in the trash counts as gone
        }
        entry.exists = true;
        entry.name   = item->getName();
        entry.asset  = item->getAssetUUID();
        if (const LLViewerInventoryCategory* parent = gInventory.getCategory(item->getParentUUID()))
        {
            entry.folder = parent->getName();
        }
    }
}
