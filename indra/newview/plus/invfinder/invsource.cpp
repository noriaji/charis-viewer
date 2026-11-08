/**
 * @file invsource.cpp
 * @brief Keeps an invindex Index in step with the viewer's inventory (gInventory).
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

#include "llviewerprecompiledheaders.h"

#include "invsource.h"

#include "llassettype.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llfoldertype.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "llpermissions.h"
#include "llviewerinventory.h"
#include "llwearabletype.h"

#include "llagent.h"
#include "plus/rlv/rlvhooks.h"

#include <chrono>
#include <unordered_set>

namespace plus::invfinder
{
    namespace
    {
        // More changes than this at once (a big fetch, a mass move) are cheaper to rebuild.
        constexpr std::size_t kRebuildThreshold = 8000;
        constexpr std::size_t kMaxUnfetchedExamples = 5;
        constexpr int kMaxDepth = 64;

        std::string idText(const LLUUID& id)
        {
            return id.isNull() ? std::string() : id.asString();
        }

        double msSince(std::chrono::steady_clock::time_point t0)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }

        // The area a folder (and so everything in it) belongs to, from its own
        // type and the folders above it.
        Area areaOfFolder(const LLUUID& folderId, const LLUUID& libraryId)
        {
            LLUUID id = folderId;
            for (int depth = 0; depth < kMaxDepth && id.notNull(); ++depth)
            {
                if (id == libraryId)
                    return AREA_LIBRARY;
                const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
                if (!cat)
                    break;
                switch (cat->getPreferredType())
                {
                    case LLFolderType::FT_TRASH:
                        return AREA_TRASH;
                    case LLFolderType::FT_CURRENT_OUTFIT:
                        return AREA_OUTFIT;
                    default:
                        break;
                }
                id = cat->getParentUUID();
            }
            return AREA_MINE;
        }

        // A folder's own area given the area it sits in.
        Area areaOfChild(const LLViewerInventoryCategory* cat, Area inherited)
        {
            switch (cat->getPreferredType())
            {
                case LLFolderType::FT_TRASH:
                    return AREA_TRASH;
                case LLFolderType::FT_CURRENT_OUTFIT:
                    return AREA_OUTFIT;
                default:
                    return inherited;
            }
        }

        Entry folderEntry(const LLViewerInventoryCategory* cat, Area area)
        {
            Entry e;
            e.id = idText(cat->getUUID());
            e.parent = idText(cat->getParentUUID());
            e.is_folder = true;
            e.name = cat->getName();
            e.area = area;
            return e;
        }

        Entry itemEntry(const LLViewerInventoryItem* item, Area area)
        {
            Entry e;
            e.id = idText(item->getUUID());
            e.parent = idText(item->getParentUUID());
            e.name = item->getName();
            e.desc = item->getDescription();
            e.is_link = item->getIsLinkType();
            if (const char* type = LLAssetType::lookup(item->getType()))
                e.type = type;
            if (item->isWearableType())
                e.subtype = LLWearableType::instance().getTypeName(item->getWearableType());
            e.creator = idText(item->getCreatorUUID());
            e.acquired = static_cast<std::int64_t>(item->getCreationDate());
            const U32 owner = item->getPermissions().getMaskOwner();
            e.perms = static_cast<std::uint8_t>(((owner & ::PERM_COPY) ? PERM_COPY : 0)
                                                | ((owner & ::PERM_MODIFY) ? PERM_MODIFY : 0)
                                                | ((owner & ::PERM_TRANSFER) ? PERM_TRANSFER : 0));
            e.area = area;
            return e;
        }

        // Puts the direct contents of folder `catId` (and, below it, everything
        // else) into the index with the areas they have given `area` for the folder.
        void addSubtree(Index& index, const LLUUID& rootId, Area rootArea)
        {
            struct Todo
            {
                LLUUID id;
                Area area;
            };
            std::vector<Todo> stack{{rootId, rootArea}};
            std::unordered_set<LLUUID> seen;
            while (!stack.empty())
            {
                const Todo todo = stack.back();
                stack.pop_back();
                if (!seen.insert(todo.id).second)
                    continue;
                LLInventoryModel::cat_array_t* cats = nullptr;
                LLInventoryModel::item_array_t* items = nullptr;
                gInventory.getDirectDescendentsOf(todo.id, cats, items);
                if (cats)
                {
                    for (const auto& cat : *cats)
                    {
                        const Area area = areaOfChild(cat.get(), todo.area);
                        index.upsert(folderEntry(cat.get(), area));
                        stack.push_back({cat->getUUID(), area});
                    }
                }
                if (items)
                {
                    for (const auto& item : *items)
                        index.upsert(itemEntry(item.get(), todo.area));
                }
            }
        }
    }

    struct InvSource::Pending
    {
        std::unordered_set<LLUUID> ids;
    };

    class InvSource::Observer : public LLInventoryObserver
    {
    public:
        explicit Observer(InvSource& owner) : mOwner(owner) {}
        void changed(U32 mask) override { mOwner.onChanged(mask); }

    private:
        InvSource& mOwner;
    };

    InvSource::InvSource() : mPending(std::make_unique<Pending>()) {}

    InvSource::~InvSource()
    {
        stop();
    }

    InvSource& InvSource::instance()
    {
        static InvSource sInstance;
        return sInstance;
    }

    void InvSource::start()
    {
        if (mObserver)
            return;
        mObserver = std::make_unique<Observer>(*this);
        gInventory.addObserver(mObserver.get());
        mNeedRebuild = true;
    }

    void InvSource::stop()
    {
        if (!mObserver)
            return;
        gInventory.removeObserver(mObserver.get());
        mObserver.reset();
        mPending->ids.clear();
        mIndex.clear();
        mNeedRebuild = true;
    }

    void InvSource::onChanged(unsigned mask)
    {
        constexpr unsigned kInteresting = LLInventoryObserver::ADD | LLInventoryObserver::REMOVE | LLInventoryObserver::LABEL
                                          | LLInventoryObserver::STRUCTURE | LLInventoryObserver::INTERNAL;
        if (!(mask & kInteresting) || mNeedRebuild)
            return;
        for (const LLUUID& id : gInventory.getChangedIDs())
            mPending->ids.insert(id);
        for (const LLUUID& id : gInventory.getAddedIDs())
            mPending->ids.insert(id);
        if (mPending->ids.size() > kRebuildThreshold)
        {
            mPending->ids.clear();
            mNeedRebuild = true;
        }
    }

    bool InvSource::sync()
    {
        mRebuiltLastSync = false;
        if (!gInventory.isInventoryUsable())
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string root = idText(gInventory.getRootFolderID());
        const std::string library = idText(gInventory.getLibraryRootFolderID());
        if (root.empty())
            return false;
        if (mNeedRebuild || root != mRootId || library != mLibraryId)
        {
            mRootId = root;
            mLibraryId = library;
            rebuildAll();
            mRebuiltLastSync = true;
        }
        else if (!mPending->ids.empty())
        {
            applyPending();
        }
        mLastSyncMs = msSince(t0);
        return true;
    }

    void InvSource::rebuildAll()
    {
        const auto t0 = std::chrono::steady_clock::now();
        mIndex.clear();
        mPending->ids.clear();

        const LLUUID rootId = gInventory.getRootFolderID();
        if (const LLViewerInventoryCategory* root = gInventory.getCategory(rootId))
        {
            Entry e = folderEntry(root, AREA_MINE);
            e.parent.clear(); // a root: its own name is left out of paths
            mIndex.upsert(std::move(e));
            addSubtree(mIndex, rootId, AREA_MINE);
        }
        const LLUUID libraryId = gInventory.getLibraryRootFolderID();
        if (libraryId.notNull())
        {
            if (const LLViewerInventoryCategory* library = gInventory.getCategory(libraryId))
            {
                Entry e = folderEntry(library, AREA_LIBRARY);
                e.parent.clear();
                mIndex.upsert(std::move(e));
                addSubtree(mIndex, libraryId, AREA_LIBRARY);
            }
        }
        mNeedRebuild = false;
        mLastRebuildMs = msSince(t0);
        LL_INFOS("PlusInvFinder") << "Indexed " << mIndex.size() << " inventory entries (" << mIndex.folderCount()
                                  << " folders) in " << mLastRebuildMs << " ms" << LL_ENDL;
    }

    void InvSource::applyPending()
    {
        const LLUUID libraryId = gInventory.getLibraryRootFolderID();
        const std::unordered_set<LLUUID> ids = std::move(mPending->ids);
        mPending->ids.clear();
        for (const LLUUID& id : ids)
        {
            const std::string text = idText(id);
            if (const LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                mIndex.upsert(itemEntry(item, areaOfFolder(item->getParentUUID(), libraryId)));
            }
            else if (const LLViewerInventoryCategory* cat = gInventory.getCategory(id))
            {
                const bool isRoot = cat->getParentUUID().isNull();
                const Area area = isRoot ? (id == libraryId ? AREA_LIBRARY : AREA_MINE)
                                         : areaOfChild(cat, areaOfFolder(cat->getParentUUID(), libraryId));
                const Entry* old = mIndex.find(text);
                const bool areaChanged = old && old->area != area;
                const bool isNew = old == nullptr;
                Entry e = folderEntry(cat, area);
                mIndex.upsert(std::move(e));
                // A folder moved into or out of the trash, or a folder we had not seen with
                // contents already in the model: its contents' areas need another look.
                if (areaChanged || isNew)
                    addSubtree(mIndex, id, area);
            }
            else
            {
                mIndex.remove(text);
            }
        }
    }

    InvSource::Status InvSource::status()
    {
        Status s;
        s.usable = sync();
        s.rebuiltLastSync = mRebuiltLastSync;
        s.entries = mIndex.size();
        s.folders = mIndex.folderCount();
        s.generation = mIndex.generation();
        s.lastRebuildMs = mLastRebuildMs;
        s.lastSyncMs = mLastSyncMs;
        s.everythingFetched = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
        s.fetchBusy = !LLInventoryModelBackgroundFetch::instance().isBulkFetchProcessingComplete();
        if (!s.usable)
            return s;

        std::vector<LLUUID> stack{gInventory.getRootFolderID()};
        std::unordered_set<LLUUID> seen;
        while (!stack.empty())
        {
            const LLUUID id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second)
                continue;
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(id, cats, items);
            if (!cats)
                continue;
            for (const auto& cat : *cats)
            {
                if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
                {
                    ++s.unfetchedFolders;
                    if (s.unfetchedExamples.size() < kMaxUnfetchedExamples)
                    {
                        std::string path = mIndex.folderPath(idText(cat->getUUID()));
                        s.unfetchedExamples.push_back(path.empty() ? cat->getName() : path);
                    }
                }
                stack.push_back(cat->getUUID());
            }
        }
        return s;
    }

    std::string InvSource::creatorName(const std::string& id, bool* known) const
    {
        if (known)
            *known = false;
        LLUUID uuid;
        if (id.empty() || !uuid.set(id, false))
            return id;
        // @shownames: the anonym where the item properties would show it (research/248 E36). Every item here is the
        // agent's own, so this is canShowItemCreator() for an item owned by the agent. The anonym is "known": a search by
        // a creator's real name does not find that creator's items.
        if (plus::rlv::enabled() && !plus::rlv::canShowCreatorName(uuid, gAgentID))
        {
            if (known)
                *known = true;
            return plus::rlv::anonymizedName(uuid);
        }
        LLAvatarName name;
        if (LLAvatarNameCache::get(uuid, &name))
        {
            if (known)
                *known = true;
            return name.getUserName();
        }
        // Not cached: ask for it. The next query will find it.
        LLAvatarNameCache::get(uuid, [](const LLUUID&, const LLAvatarName&) {});
        return id;
    }
}
