/**
 * @file aostore.cpp
 * @brief Animation Overrider: keeps the sets in memory and the #AO folder of
 *        the inventory in step, in Firestorm's layout.
 *
 * Reading the folder follows Firestorm's AOEngine::update() and
 * reloadStateAnimations(); writing is new (see aostore.h and
 * research/ao-port.md in the sl-viewer repository).
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Zi Ree @ Second Life
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

#include "aostore.h"

#include "aoengine.h"

#include "llagent.h"
#include "llaisapi.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "plus/invguard/invguard.h"

#include <algorithm>

namespace
{
    constexpr char ROOT_FOLDER_NAME[]    = "#Charis";    // ours: no folder of another viewer is made in the inventory
    constexpr char FS_ROOT_FOLDER_NAME[] = "#Firestorm"; // Firestorm's ROOT_FIRESTORM_FOLDER (only read, to take its sets over)
    constexpr char AO_FOLDER_NAME[]      = "#AO";        // Firestorm's ROOT_AO_FOLDER
    constexpr char SETTING_OFFER[]       = "PlusAOFirestormOffer"; // ask about taking over Firestorm's sets (PerAccount)

    constexpr F32 POLL_PERIOD  = 0.5f; // loading steps
    constexpr F32 SAVE_DELAY   = 1.f;  // names written this long after the last change
    constexpr F32 VERIFY_DELAY = 1.f;  // the folder read again this long after another change

    bool logTiming()
    {
        static LLCachedControl<bool> log_timing(gSavedSettings, "PlusAOLogTiming", false);
        return log_timing;
    }

    // Owned by gInventory (LLInventoryModel::cleanupInventory() deletes it).
    class AOInventoryObserver : public LLInventoryObserver
    {
    public:
        void changed(U32 mask) override
        {
            if (plus::ao::AOStore* store = plus::ao::AOStore::live())
            {
                store->onInventoryChanged();
            }
        }
    };

    bool isAnimationLink(const LLViewerInventoryItem* item)
    {
        return item && item->getIsLinkType() && item->getInventoryType() == LLInventoryType::IT_ANIMATION;
    }

    // The list of links of a state's main list (key 0), of a group or of a track, and the folder they go to.
    std::vector<plus::ao::AOAnimation>* containerList(plus::ao::AOState& state, U32 key, LLUUID& folder)
    {
        if (key == 0)
        {
            folder = state.mFolderID;
            return &state.mAnimations;
        }
        if (plus::ao::AOAnimation* group = state.findGroup(key))
        {
            folder = group->mFolderID;
            return &group->mMembers;
        }
        if (plus::ao::AOTrack* track = state.findTrack(key))
        {
            folder = track->mFolderID;
            return &track->mAnimations;
        }
        return nullptr;
    }

    // The animation links directly in a folder, numbered by their descriptions (-1: none).
    std::vector<plus::ao::AOAnimation> readLinks(const LLUUID& folder)
    {
        std::vector<plus::ao::AOAnimation> result;
        LLInventoryModel::cat_array_t*     unused = nullptr;
        LLInventoryModel::item_array_t*    links  = nullptr;
        gInventory.getDirectDescendentsOf(folder, unused, links);
        if (!links)
        {
            return result;
        }
        for (const LLPointer<LLViewerInventoryItem>& link : *links)
        {
            if (!isAnimationLink(link))
            {
                continue;
            }
            plus::ao::AOAnimation anim;
            anim.mName       = link->getName();
            anim.mLinkID     = link->getUUID();
            anim.mOriginalID = link->getLinkedUUID();
            if (link->getLinkedItem())
            {
                anim.mAssetID = link->getAssetUUID();
            }
            if (!LLStringUtil::convertToS32(link->getActualDescription(), anim.mSortOrder))
            {
                anim.mSortOrder = -1; // numbered by numberList()
            }
            result.push_back(std::move(anim));
        }
        return result;
    }

    // Entries without a number go to the end, in the order the inventory has them
    // (Firestorm appends them too), and get one: a link's description is written
    // (orders), a group's folder is renamed (names_dirty). Then the list is sorted.
    void numberList(std::vector<plus::ao::AOAnimation>& list, std::vector<std::pair<LLUUID, S32>>& orders, bool& names_dirty)
    {
        S32 next = 0;
        for (const plus::ao::AOAnimation& anim : list)
        {
            next = llmax(next, anim.mSortOrder + 1);
        }
        for (plus::ao::AOAnimation& anim : list)
        {
            if (anim.mSortOrder >= 0)
            {
                continue;
            }
            anim.mSortOrder = next++;
            if (anim.mIsGroup)
            {
                anim.mFolderDirty = true;
                names_dirty       = true;
            }
            else
            {
                orders.emplace_back(anim.mLinkID, anim.mSortOrder);
            }
        }
        std::stable_sort(list.begin(), list.end(),
                         [](const plus::ao::AOAnimation& a, const plus::ao::AOAnimation& b) { return a.mSortOrder < b.mSortOrder; });
    }

    bool sameList(const std::vector<plus::ao::AOAnimation>& a, const std::vector<plus::ao::AOAnimation>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const plus::ao::AOAnimation& x, const plus::ao::AOAnimation& y)
                          {
                              if (x.mIsGroup != y.mIsGroup)
                              {
                                  return false;
                              }
                              return x.mIsGroup ? x.mFolderID == y.mFolderID && sameList(x.mMembers, y.mMembers) : x.mLinkID == y.mLinkID;
                          });
    }

    bool sameTracks(const std::vector<plus::ao::AOTrack>& a, const std::vector<plus::ao::AOTrack>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const plus::ao::AOTrack& x, const plus::ao::AOTrack& y)
                          { return x.mFolderID == y.mFolderID && sameList(x.mAnimations, y.mAnimations); });
    }

    // Same entries in the same order: takes over names and numbers. True if a name changed.
    bool adoptList(std::vector<plus::ao::AOAnimation>& dst, const std::vector<plus::ao::AOAnimation>& src)
    {
        bool renamed = false;
        for (size_t n = 0; n < src.size() && n < dst.size(); ++n)
        {
            plus::ao::AOAnimation& anim = dst[n];
            if (anim.mIsGroup && anim.mFolderDirty)
            {
                // Our own name and number are about to be written; they win.
                renamed = adoptList(anim.mMembers, src[n].mMembers) || renamed;
                continue;
            }
            renamed         = renamed || anim.mName != src[n].mName;
            anim.mName      = src[n].mName;
            anim.mSortOrder = src[n].mSortOrder;
            if (anim.mAssetID.isNull())
            {
                anim.mAssetID = src[n].mAssetID;
            }
            if (anim.mIsGroup)
            {
                renamed = adoptList(anim.mMembers, src[n].mMembers) || renamed;
            }
        }
        return renamed;
    }

    // One request for several folders in the same parent; on_created is called
    // for each folder made, or once with null if the request failed.
    void createFolders(const LLUUID& parent, const std::vector<std::string>& names, const plus::ao::batch_ptr_t& batch,
                       std::function<void(const LLUUID&)> on_created)
    {
        LLSD body;
        body["categories"] = LLSD::emptyArray();
        for (const std::string& name : names)
        {
            LLPointer<LLViewerInventoryCategory> cat = new LLViewerInventoryCategory(LLUUID::null, parent, LLFolderType::FT_NONE, name, gAgent.getID());
            body["categories"].append(cat->asAISCreateCatLLSD());
        }

        batch->addRequest();
        AISAPI::CreateInventory(parent, body, [batch, on_created](const LLUUID& id)
        {
            if (id.isNull())
            {
                batch->addFailure();
            }
            on_created(id);
        });
    }
}

namespace plus::ao
{
    const char* rootFolderName()
    {
        return ROOT_FOLDER_NAME;
    }

    // ------------------------------------------------------------------------
    // Timing

    ServerBatch::ServerBatch(const std::string& label, done_func_t on_done) :
        mLabel(label),
        mOnDone(std::move(on_done))
    {
    }

    ServerBatch::~ServerBatch()
    {
        // The last callback is being released inside the inventory service's
        // coroutine; finish on the next idle instead.
        if (LLApp::isExiting())
        {
            return;
        }

        const F64         ms       = mTimer.getElapsedTimeF64() * 1000.0;
        const S32         requests = mRequests;
        const S32         failures = mFailures;
        const std::string label    = mLabel;
        done_func_t       on_done  = std::move(mOnDone);
        doOnIdleOneTime([=]()
        {
            if (logTiming())
            {
                LL_INFOS("PlusAO") << label << ": server done after " << llformat("%.0f", ms) << " ms, " << requests
                                   << " request(s)" << (failures ? llformat(", %d failed", failures) : std::string()) << LL_ENDL;
            }
            if (AOStore* store = AOStore::live())
            {
                if (on_done)
                {
                    on_done(failures);
                }
                store->onBatchDone();
            }
        });
    }

    LocalTiming::LocalTiming(const std::string& label) :
        mLabel(label)
    {
    }

    LocalTiming::~LocalTiming()
    {
        if (logTiming())
        {
            LL_INFOS("PlusAO") << mLabel << ": " << llformat("%.2f", mTimer.getElapsedTimeF64() * 1000.0) << " ms in the viewer" << LL_ENDL;
        }
    }

    // ------------------------------------------------------------------------
    // AOStore

    AOStore::AOStore(AOEngine& engine) :
        mEngine(engine)
    {
    }

    // static
    AOStore* AOStore::live()
    {
        return AOEngine::instanceExists() ? &AOEngine::instance().mStore : nullptr;
    }

    batch_ptr_t AOStore::newBatch(const std::string& label, ServerBatch::done_func_t on_done)
    {
        ++mPending;
        mEngine.notifyStatusChanged();
        return std::make_shared<ServerBatch>(label, std::move(on_done));
    }

    void AOStore::onBatchDone()
    {
        mPending = llmax(0, mPending - 1);
        mEngine.notifyStatusChanged();
    }

    void AOStore::onIdle()
    {
        if (mPhase != PHASE_IDLE && mPollTimer.getElapsedTimeF32() >= POLL_PERIOD)
        {
            mPollTimer.reset();
            stepLoading();
        }

        if (mOfferDue && mPhase == PHASE_IDLE && mPollTimer.getElapsedTimeF32() >= POLL_PERIOD)
        {
            mPollTimer.reset();
            stepOffer();
        }

        if (mSaveDue && mSaveTimer.getElapsedTimeF32() >= SAVE_DELAY)
        {
            saveNow();
        }

        // Only once our own requests are answered: until then the inventory
        // legitimately differs from the sets in memory.
        if (mVerifyDue && mPhase == PHASE_IDLE && mPending == 0 && !mSaveDue && mVerifyTimer.getElapsedTimeF32() >= VERIFY_DELAY)
        {
            mVerifyDue = false;
            sync(SYNC_VERIFY);
        }
    }

    // ------------------------------------------------------------------------
    // Reading

    bool AOStore::findChildFolder(const LLUUID& parent, const std::string& name, LLUUID& found)
    {
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (cat->getName() == name)
                {
                    found = cat->getUUID();
                    return true;
                }
            }
        }

        // Not there: certain only once the parent's contents are all known.
        if (gInventory.isCategoryComplete(parent))
        {
            found.setNull();
            return true;
        }
        gInventory.fetchDescendentsOf(parent);
        return false;
    }

    bool AOStore::isFetched(const LLUUID& folder, S32 depth)
    {
        if (!gInventory.getCategory(folder))
        {
            // Gone: nothing to wait for (validateFolders() looks for the folders again).
            return true;
        }
        if (!gInventory.isCategoryComplete(folder))
        {
            gInventory.fetchDescendentsOf(folder);
            return false;
        }
        if (depth <= 1)
        {
            return true;
        }

        bool fetched = true;
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                // No early return: every incomplete folder starts fetching now.
                fetched = isFetched(cat->getUUID(), depth - 1) && fetched;
            }
        }
        return fetched;
    }

    bool AOStore::validateFolders()
    {
        const LLUUID trash  = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        auto         usable = [&trash](const LLUUID& id)
        {
            return id.isNull() || (gInventory.getCategory(id) && (trash.isNull() || !gInventory.isObjectDescendentOf(id, trash)));
        };
        if (usable(mRootFolder) && usable(mAOFolder))
        {
            return true;
        }

        LL_INFOS("PlusAO") << "The AO folder is gone from the inventory; looking for it again" << LL_ENDL;
        mRootFolder.setNull();
        mAOFolder.setNull();
        if (mPhase != PHASE_WAIT_INVENTORY)
        {
            mPhase = PHASE_FIND_FOLDERS;
        }
        mEngine.notifyStatusChanged();
        return false;
    }

    void AOStore::stepLoading()
    {
        if (mPhase == PHASE_FIND_FOLDERS || mPhase == PHASE_FETCH)
        {
            validateFolders();
        }
        switch (mPhase)
        {
            case PHASE_WAIT_INVENTORY:
                if (!gInventory.isInventoryUsable() || gInventory.getRootFolderID().isNull())
                {
                    return;
                }
                if (!mObserverAdded)
                {
                    gInventory.addObserver(new AOInventoryObserver());
                    mObserverAdded = true;
                }
                mPhase = PHASE_FIND_FOLDERS;
                [[fallthrough]];

            case PHASE_FIND_FOLDERS:
            {
                LLUUID found;
                if (mRootFolder.isNull())
                {
                    if (!findChildFolder(gInventory.getRootFolderID(), ROOT_FOLDER_NAME, found))
                    {
                        return;
                    }
                    mRootFolder = found;
                }
                if (mRootFolder.notNull() && mAOFolder.isNull())
                {
                    if (!findChildFolder(mRootFolder, AO_FOLDER_NAME, found))
                    {
                        return;
                    }
                    mAOFolder = found;
                }
                // No folder yet: no sets. It is made with the first set (ensureAOFolder()).
                mPhase = PHASE_FETCH;
                [[fallthrough]];
            }

            case PHASE_FETCH:
                // Set folder -> state folders -> group and track folders -> links.
                if ((mAOFolder.notNull() && !isFetched(mAOFolder, 4)) || mPending > 0)
                {
                    return;
                }
                mPhase = PHASE_IDLE;
                sync(mLoaded ? SYNC_RELOAD : SYNC_LOGIN);
                return;

            case PHASE_RELOAD_FETCH:
            case PHASE_IDLE:
                return;
        }
    }

    void AOStore::reload()
    {
        if (!mLoaded || mPhase != PHASE_IDLE || mImporting)
        {
            return;
        }

        if (!validateFolders())
        {
            return; // looking for the folders again is the reload
        }

        LL_INFOS("PlusAO") << "Reloading the AO folder" << LL_ENDL;
        if (mAOFolder.isNull() || !AISAPI::isAvailable())
        {
            // Look for the folders again (another viewer may have made them).
            mRootFolder.setNull();
            mAOFolder.setNull();
            mPhase = PHASE_FIND_FOLDERS;
            return;
        }

        // One recursive fetch for everything below #AO, then the same read as at login.
        mPhase = PHASE_RELOAD_FETCH;
        batch_ptr_t batch = newBatch("reload the AO folder", [](S32)
        {
            // Not when the folders went away meanwhile (validateFolders(): looking for them again).
            AOStore* store = AOStore::live();
            if (store && store->mPhase == PHASE_RELOAD_FETCH)
            {
                store->mPhase = PHASE_FETCH;
            }
        });
        batch->addRequest();
        AISAPI::FetchCategoryChildren(mAOFolder, AISAPI::INVENTORY, true, [batch](const LLUUID&) {});
    }

    bool AOStore::isAORelated(const LLUUID& id, std::optional<std::unordered_set<LLUUID>>& owned_ids) const
    {
        if (id == mAOFolder)
        {
            return true;
        }
        if (gInventory.getObject(id))
        {
            return gInventory.isObjectDescendentOf(id, mAOFolder);
        }
        // Gone from the inventory: one of ours? The ids of the sets are gathered once per
        // change, on the first id that went.
        if (!owned_ids.has_value())
        {
            owned_ids.emplace();
            mEngine.collectInventoryIDs(*owned_ids);
        }
        return owned_ids->count(id) > 0;
    }

    void AOStore::onInventoryChanged()
    {
        if (!mLoaded || mAOFolder.isNull() || mVerifyDue)
        {
            return;
        }

        std::optional<std::unordered_set<LLUUID>> owned_ids;
        for (const LLUUID& id : gInventory.getChangedIDs())
        {
            if (isAORelated(id, owned_ids))
            {
                mVerifyDue = true;
                mVerifyTimer.reset();
                return;
            }
        }
    }

    bool AOStore::moveForeignItems(const LLUUID& folder)
    {
        bool moved = false;

        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        if (cats)
        {
            // A copy: the recursion does not change the folders, but stay safe.
            const LLInventoryModel::cat_array_t subfolders = *cats;
            for (const LLPointer<LLViewerInventoryCategory>& cat : subfolders)
            {
                moved = moveForeignItems(cat->getUUID()) || moved;
            }
        }
        if (items)
        {
            const LLUUID lost_and_found = gInventory.findCategoryUUIDForType(LLFolderType::FT_LOST_AND_FOUND);
            if (lost_and_found.isNull())
            {
                // Nowhere to put them: they stay (a parent of null would lose them).
                if (holdsForeignItems(folder))
                {
                    LL_WARNS("PlusAO") << "No Lost and Found folder; items that are not animation links stay in " << folder << LL_ENDL;
                }
                return moved;
            }
            const LLInventoryModel::item_array_t contents = *items;
            for (const LLPointer<LLViewerInventoryItem>& item : contents)
            {
                if (!isAnimationLink(item))
                {
                    LL_INFOS("PlusAO") << item->getName() << " is not an animation link; moved to Lost and Found" << LL_ENDL;
                    gInventory.changeItemParent(item, lost_and_found, false);
                    moved = true;
                }
            }
        }
        return moved;
    }

    bool AOStore::holdsForeignItems(const LLUUID& folder) const
    {
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (!isAnimationLink(item))
                {
                    return true;
                }
            }
        }
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (holdsForeignItems(cat->getUUID()))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void AOStore::readStates(AOSet& set, bool is_new)
    {
        struct Fresh
        {
            std::vector<AOAnimation> mSteps;
            std::vector<AOTrack>     mTracks;
        };
        std::array<Fresh, STATE_COUNT>   fresh;
        std::array<LLUUID, STATE_COUNT>  folders;
        std::array<bool, STATE_COUNT>    flags_changed{}; // CY, RN, CT taken over from the folder name

        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(set.getFolderID(), cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                S32  index      = -1;
                bool cycle      = false;
                bool random     = false;
                F32  cycle_time = 0.f;
                if (!AOState::parseFolderName(cat->getName(), index, cycle, random, cycle_time))
                {
                    LL_WARNS("PlusAO") << "Unknown state folder " << cat->getName() << " in set " << set.getName() << LL_ENDL;
                    continue;
                }

                AOState& state = set.getState(index);
                if (folders[index].isNull())
                {
                    folders[index] = cat->getUUID();
                    if (!state.mDirty)
                    {
                        flags_changed[index] = state.mCycle != cycle || state.mRandom != random || state.mCycleTime != cycle_time;
                        state.mCycle     = cycle;
                        state.mRandom    = random;
                        state.mCycleTime = cycle_time;
                    }
                }

                // A second folder of the same state adds its links (new links go to the first).
                for (AOAnimation& anim : readLinks(cat->getUUID()))
                {
                    fresh[index].mSteps.push_back(std::move(anim));
                }

                // Below the state folder: tracks, and every other folder is a group.
                LLInventoryModel::cat_array_t*  sub_cats  = nullptr;
                LLInventoryModel::item_array_t* sub_items = nullptr;
                gInventory.getDirectDescendentsOf(cat->getUUID(), sub_cats, sub_items);
                if (!sub_cats)
                {
                    continue;
                }
                for (const LLPointer<LLViewerInventoryCategory>& sub : *sub_cats)
                {
                    std::string name;
                    S32         order = -1;
                    if (AOTrack::parseFolderName(sub->getName(), name, cycle, random, cycle_time))
                    {
                        AOTrack track;
                        track.mName       = name;
                        track.mFolderID   = sub->getUUID();
                        track.mCycle      = cycle;
                        track.mRandom     = random;
                        track.mCycleTime  = cycle_time;
                        track.mAnimations = readLinks(sub->getUUID());
                        fresh[index].mTracks.push_back(std::move(track));
                    }
                    else if (parseGroupFolderName(sub->getName(), name, order))
                    {
                        AOAnimation group;
                        group.mIsGroup   = true;
                        group.mKey       = newKey();
                        group.mName      = name;
                        group.mSortOrder = order;
                        group.mFolderID  = sub->getUUID();
                        group.mMembers   = readLinks(sub->getUUID());
                        if (group.mMembers.empty())
                        {
                            continue; // a folder with no animation is no group (Firestorm skips it too)
                        }
                        fresh[index].mSteps.push_back(std::move(group));
                    }
                    else
                    {
                        LL_WARNS("PlusAO") << "Unknown folder " << sub->getName() << " in state " << state.getName() << LL_ENDL;
                    }
                }
            }
        }

        std::vector<std::pair<LLUUID, S32>> orders;
        bool                                names_dirty = false;
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            AOState&                  state      = set.getState(i);
            std::vector<AOAnimation>& animations = fresh[i].mSteps;
            std::vector<AOTrack>&     tracks     = fresh[i].mTracks;

            state.mFolderID = folders[i];
            state.mFolderRequested = false;

            // In trackBefore() order: the inventory's order of the folders could change from one reading to the next
            std::stable_sort(tracks.begin(), tracks.end(), trackBefore);

            numberList(animations, orders, names_dirty);
            for (AOAnimation& anim : animations)
            {
                if (anim.mIsGroup)
                {
                    numberList(anim.mMembers, orders, names_dirty);
                    // A group with a single member plays as that animation; a group that
                    // is empty is no use. Both stay as they are in the inventory.
                }
            }
            for (AOTrack& track : tracks)
            {
                numberList(track.mAnimations, orders, names_dirty);
            }

            if (is_new)
            {
                state.mAnimations = std::move(animations);
                state.mTracks     = std::move(tracks);
                continue;
            }

            if (sameList(state.mAnimations, animations))
            {
                // Same entries in the same order: take over names and numbers only.
                const bool renamed = adoptList(state.mAnimations, animations);
                if (flags_changed[i] || renamed)
                {
                    mEngine.notifyStateChanged(set, i);
                }
            }
            else
            {
                mEngine.replaceAnimations(set, state, std::move(animations)); // tells the window itself
            }

            if (sameTracks(state.mTracks, tracks))
            {
                bool changed = false;
                for (size_t n = 0; n < tracks.size(); ++n)
                {
                    AOTrack& track = state.mTracks[n];
                    if (!track.mDirty)
                    {
                        changed = changed || track.mName != tracks[n].mName || track.mCycle != tracks[n].mCycle ||
                                  track.mRandom != tracks[n].mRandom || track.mCycleTime != tracks[n].mCycleTime;
                        track.mName      = tracks[n].mName;
                        track.mCycle     = tracks[n].mCycle;
                        track.mRandom    = tracks[n].mRandom;
                        track.mCycleTime = tracks[n].mCycleTime;
                    }
                    changed = adoptList(track.mAnimations, tracks[n].mAnimations) || changed;
                }
                if (changed)
                {
                    mEngine.notifyStateChanged(set, i);
                }
            }
            else
            {
                mEngine.replaceTracks(set, state, std::move(tracks));
            }
        }

        if (!orders.empty())
        {
            writeSortOrders(orders, "number links of set \"" + set.getName() + "\"");
        }
        if (names_dirty)
        {
            scheduleSave(); // the groups' folders get their numbers
        }
    }

    void AOStore::sync(ESyncReason reason)
    {
        if (!validateFolders())
        {
            return; // stepLoading() reads again once the folders are looked for
        }

        static const char* reason_names[] = { "login", "reload", "verify" };
        LocalTiming timing(std::string("read the AO folder (") + reason_names[reason] + ")");

        if (reason != SYNC_VERIFY && mAOFolder.notNull() && moveForeignItems(mAOFolder))
        {
            notifyUser("ao_foreign_items", LLSD());
        }

        AOEngine::set_list_t sets;
        AOSet*               default_set = nullptr;
        bool                 changed     = false;

        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        if (mAOFolder.notNull())
        {
            gInventory.getDirectDescendentsOf(mAOFolder, cats, items);
        }
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                std::string name;
                bool        sit_override = false, smart = false, mouselook_stand_disable = false, is_default = false;
                if (!AOSet::parseFolderName(cat->getName(), name, sit_override, smart, mouselook_stand_disable, is_default))
                {
                    LL_WARNS("PlusAO") << "Unexpected folder in the AO folder: " << cat->getName() << LL_ENDL;
                    continue;
                }

                AOEngine::set_ptr_t set;
                for (const AOEngine::set_ptr_t& existing : mEngine.mSets)
                {
                    if (existing->getFolderID() == cat->getUUID())
                    {
                        set = existing;
                        break;
                    }
                }

                const bool is_new = !set;
                if (is_new)
                {
                    set = std::make_shared<AOSet>(name);
                    set->setFolderID(cat->getUUID());
                    changed = true;
                }

                if (set->isDirty())
                {
                    // Our own name and flags are about to be written; they win.
                    if (mEngine.mDefaultSet == set.get() && !default_set)
                    {
                        default_set = set.get();
                    }
                }
                else
                {
                    changed = changed || set->getName() != name || set->getSitOverride() != sit_override ||
                              set->getSmart() != smart || set->getMouselookStandDisable() != mouselook_stand_disable;
                    set->setName(name);
                    set->setSitOverride(sit_override);
                    set->setSmart(smart);
                    set->setMouselookStandDisable(mouselook_stand_disable);
                    if (is_default && !default_set)
                    {
                        default_set = set.get();
                    }
                }

                readStates(*set, is_new);
                sets.push_back(set);
            }
        }

        // Sets whose folder is still being made stay (none while verifying:
        // that waits until every request is answered).
        for (const AOEngine::set_ptr_t& set : mEngine.mSets)
        {
            if (set->getFolderID().isNull())
            {
                sets.push_back(set);
                if (mEngine.mDefaultSet == set.get() && !default_set)
                {
                    default_set = set.get();
                }
            }
        }

        changed = changed || sets.size() != mEngine.mSets.size() || default_set != mEngine.mDefaultSet;

        if (reason == SYNC_LOGIN)
        {
            for (const AOEngine::set_ptr_t& set : sets)
            {
                mEngine.restoreSavedAnimations(*set);
            }
            const bool no_sets = sets.empty();
            mEngine.replaceSets(std::move(sets), default_set, false);
            mLoaded = true;
            mEngine.onLoaded();
            mOfferDue = no_sets && gSavedPerAccountSettings.getBOOL(SETTING_OFFER);
            return;
        }

        if (changed)
        {
            mEngine.replaceSets(std::move(sets), default_set);
        }

        // Changes made while the Reload button's read was going on.
        const AOEngine::set_list_t current = mEngine.mSets;
        for (const AOEngine::set_ptr_t& set : current)
        {
            flush(set);
        }
        mEngine.notifyStatusChanged();
    }

    // ------------------------------------------------------------------------
    // Writing

    void AOStore::ensureAOFolder()
    {
        // While the folders are being looked for, sync() flushes the sets afterwards.
        if (mAOFolderRequested || mPhase != PHASE_IDLE || !AISAPI::isAvailable())
        {
            return;
        }
        mAOFolderRequested = true;

        auto on_ao_folder = [](const LLUUID& id)
        {
            AOStore* store = AOStore::live();
            if (!store)
            {
                return;
            }
            store->mAOFolderRequested = false;
            std::function<void()> waiter;
            waiter.swap(store->mOnAOFolder);
            if (id.isNull())
            {
                store->mImporting = false;
                notifyUser("ao_folder_failed", LLSD(), true);
                return;
            }
            store->mAOFolder = id;
            // Now the sets waiting for it.
            const AOEngine::set_list_t sets = store->mEngine.mSets;
            for (const AOEngine::set_ptr_t& set : sets)
            {
                store->flush(set);
            }
            if (waiter)
            {
                waiter();
            }
        };

        batch_ptr_t batch = newBatch("create the AO folder");
        if (mRootFolder.notNull())
        {
            createFolders(mRootFolder, { AO_FOLDER_NAME }, batch, on_ao_folder);
            return;
        }

        createFolders(gInventory.getRootFolderID(), { ROOT_FOLDER_NAME }, batch, [batch, on_ao_folder](const LLUUID& id)
        {
            AOStore* store = AOStore::live();
            if (!store)
            {
                return;
            }
            if (id.isNull())
            {
                on_ao_folder(id);
                return;
            }
            store->mRootFolder = id;
            createFolders(id, { AO_FOLDER_NAME }, batch, on_ao_folder);
        });
    }

    void AOStore::flush(const std::shared_ptr<AOSet>& set)
    {
        if (!mLoaded || !set || !validateFolders())
        {
            return; // after looking for the folders again, sync() flushes every set
        }
        if (mAOFolder.isNull())
        {
            ensureAOFolder();
            return;
        }
        if (set->getFolderID().isNull())
        {
            if (!set->isFolderRequested())
            {
                requestSetFolder(set);
            }
            return;
        }

        // All missing state folders of the set in one request.
        std::vector<S32> need_folder;
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            const AOState& state = set->getState(i);
            if (state.mFolderID.isNull() && !state.mFolderRequested && state.hasContent())
            {
                need_folder.push_back(i);
            }
        }
        if (!need_folder.empty())
        {
            requestStateFolders(set, need_folder);
        }

        // The folders of groups and tracks (one request each), and the new links: one request per folder.
        auto request_links = [this, &set](AOState& state, U32 key, const std::vector<AOAnimation>& list)
        {
            std::vector<size_t> indices;
            for (size_t n = 0; n < list.size(); ++n)
            {
                const AOAnimation& anim = list[n];
                if (!anim.mIsGroup && anim.mLinkID.isNull() && !anim.mLinkRequest)
                {
                    indices.push_back(n);
                }
            }
            if (!indices.empty())
            {
                requestLinks(set, state, key, indices);
            }
        };

        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            AOState& state = set->getState(i);
            if (state.mFolderID.isNull())
            {
                continue;
            }
            request_links(state, 0, state.mAnimations);
            for (AOAnimation& group : state.mAnimations)
            {
                if (!group.mIsGroup)
                {
                    continue;
                }
                if (group.mFolderID.isNull())
                {
                    if (!group.mFolderRequested)
                    {
                        requestContainerFolder(set, state, group.mKey);
                    }
                    continue;
                }
                request_links(state, group.mKey, group.mMembers);
            }
            for (AOTrack& track : state.mTracks)
            {
                if (track.mFolderID.isNull())
                {
                    if (!track.mFolderRequested)
                    {
                        requestContainerFolder(set, state, track.mKey);
                    }
                    continue;
                }
                request_links(state, track.mKey, track.mAnimations);
            }
        }
    }

    void AOStore::requestContainerFolder(const std::shared_ptr<AOSet>& set, AOState& state, U32 key)
    {
        std::string name;
        std::string label;
        if (AOAnimation* group = state.findGroup(key))
        {
            group->mFolderRequested = true;
            group->mFolderDirty     = false; // the folder gets the name and number as they are now
            name                    = groupFolderName(group->mName, group->mSortOrder);
            label                   = "create the folder of group \"" + group->mName + "\"";
        }
        else if (AOTrack* track = state.findTrack(key))
        {
            track->mFolderRequested = true;
            track->mDirty           = false;
            name                    = track->folderName();
            label                   = "create the folder of track \"" + track->mName + "\"";
        }
        else
        {
            return;
        }

        const std::weak_ptr<AOSet> weak  = set;
        const S32                  index = state.mIndex;
        batch_ptr_t batch = newBatch(label, [weak, index, key](S32)
        {
            if (AOStore* store = AOStore::live())
            {
                store->onContainerFolderDone(weak, index, key);
            }
        });
        createFolders(state.mFolderID, { name }, batch, [weak, index, key](const LLUUID& id)
        {
            AOStore* store = AOStore::live();
            if (store && id.notNull())
            {
                store->onContainerFolderCreated(weak, index, key, id);
            }
        });
    }

    void AOStore::onContainerFolderCreated(const std::weak_ptr<AOSet>& weak_set, S32 state_index, U32 key, const LLUUID& folder_id)
    {
        const std::shared_ptr<AOSet> set = weak_set.lock();
        if (!set)
        {
            return; // the set's folder is being removed, and this one with it
        }

        AOState& state = set->getState(state_index);
        if (AOAnimation* group = state.findGroup(key); group && group->mFolderID.isNull())
        {
            group->mFolderID        = folder_id;
            group->mFolderRequested = false;
            if (group->mFolderDirty)
            {
                scheduleSave(); // renamed or moved while the server made it
            }
        }
        else if (AOTrack* track = state.findTrack(key); track && track->mFolderID.isNull())
        {
            track->mFolderID        = folder_id;
            track->mFolderRequested = false;
            if (track->mDirty)
            {
                scheduleSave();
            }
        }
        else
        {
            // Removed while the server made it.
            removeFolder(folder_id, "remove the folder of a removed group or track");
            return;
        }
        flush(set);
    }

    void AOStore::onContainerFolderDone(const std::weak_ptr<AOSet>& weak_set, S32 state_index, U32 key)
    {
        const std::shared_ptr<AOSet> set = weak_set.lock();
        if (!set)
        {
            return;
        }

        AOState&    state = set->getState(state_index);
        std::string failed;
        if (AOAnimation* group = state.findGroup(key); group && group->mFolderID.isNull() && group->mFolderRequested)
        {
            failed = group->mName;
            const size_t at = (size_t)(group - state.mAnimations.data());
            state.mAnimations.erase(state.mAnimations.begin() + at);
            if (state.mCurrentAnimation > at)
            {
                --state.mCurrentAnimation;
            }
            if (state.mCurrentAnimation >= state.mAnimations.size())
            {
                state.mCurrentAnimation = 0;
            }
        }
        else if (AOTrack* track = state.findTrack(key); track && track->mFolderID.isNull() && track->mFolderRequested)
        {
            failed = track->mName;
            mEngine.swapTrackAnimation(*track, LLUUID::null); // nothing would stop it later
            state.mTracks.erase(state.mTracks.begin() + (track - state.mTracks.data()));
        }
        if (failed.empty())
        {
            return;
        }

        LL_WARNS("PlusAO") << "Could not make the folder of " << failed << " in " << state.getName() << LL_ENDL;
        LLSD args;
        args["STATE"] = aoText(llformat("state_%d", state.mIndex), LLSD());
        args["NAMES"] = failed;
        notifyUser("ao_link_failed", args, true);
        mEngine.afterListEdit(*set, state);
    }

    void AOStore::requestSetFolder(const std::shared_ptr<AOSet>& set)
    {
        set->setFolderRequested(true);
        set->setDirty(false); // the folder gets the name and flags as they are now

        const std::string          label = "create set folder \"" + set->getName() + "\"";
        const std::weak_ptr<AOSet> weak  = set;
        batch_ptr_t batch = newBatch(label, [weak](S32)
        {
            AOStore*                     store = AOStore::live();
            const std::shared_ptr<AOSet> set   = weak.lock();
            if (store && set && set->getFolderID().isNull())
            {
                LLSD args;
                args["NAME"] = set->getName();
                notifyUser("ao_set_failed", args, true);
                store->mEngine.dropSet(set.get());
            }
        });

        createFolders(mAOFolder, { set->folderName(mEngine.mDefaultSet == set.get()) }, batch, [weak](const LLUUID& id)
        {
            AOStore* store = AOStore::live();
            if (!store || id.isNull())
            {
                return;
            }
            const std::shared_ptr<AOSet> set = weak.lock();
            if (!set)
            {
                // Removed while the server made it.
                store->removeFolder(id, "remove the folder of a removed set");
                return;
            }
            set->setFolderID(id);
            set->setFolderRequested(false);
            if (set->isDirty())
            {
                store->scheduleSave(); // renamed or flags changed while the server made it
            }
            store->flush(set);
        });
    }

    void AOStore::requestStateFolders(const std::shared_ptr<AOSet>& set, const std::vector<S32>& states, bool may_split)
    {
        std::vector<std::string> names;
        for (S32 index : states)
        {
            AOState& state = set->getState(index);
            state.mFolderRequested = true;
            state.mDirty           = false; // the folder gets the flags as they are now
            names.push_back(state.folderName());
        }

        const std::weak_ptr<AOSet> weak = set;
        batch_ptr_t batch = newBatch(llformat("create %d state folder(s) in \"%s\"", (S32)states.size(), set->getName().c_str()),
                                     [weak, states, may_split](S32)
                                     {
                                         if (AOStore* store = AOStore::live())
                                         {
                                             store->onStateFoldersDone(weak, states, may_split);
                                         }
                                     });

        createFolders(set->getFolderID(), names, batch, [weak](const LLUUID& id)
        {
            AOStore* store = AOStore::live();
            if (store && id.notNull())
            {
                store->onStateFolderCreated(weak, id);
            }
        });
    }

    void AOStore::onStateFolderCreated(const std::weak_ptr<AOSet>& weak_set, const LLUUID& folder_id)
    {
        const std::shared_ptr<AOSet> set = weak_set.lock();
        if (!set)
        {
            return; // the set's folder is being removed, and this one with it
        }

        const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        S32  index      = -1;
        bool cycle      = false;
        bool random     = false;
        F32  cycle_time = 0.f;
        if (!cat || !AOState::parseFolderName(cat->getName(), index, cycle, random, cycle_time))
        {
            LL_WARNS("PlusAO") << "Created state folder " << folder_id << " is not known; reading the AO folder again" << LL_ENDL;
            mVerifyDue = true;
            mVerifyTimer.reset();
            return;
        }

        AOState& state = set->getState(index);
        if (!state.mFolderRequested || state.mFolderID.notNull() || !state.hasContent())
        {
            // Not wanted any more (the state was emptied meanwhile).
            state.mFolderRequested = false;
            removeFolder(folder_id, "remove an unused state folder");
            return;
        }

        state.mFolderID        = folder_id;
        state.mFolderRequested = false;
        if (state.mDirty)
        {
            scheduleSave(); // flags changed while the server made it
        }
        flush(set);
    }

    void AOStore::onStateFoldersDone(const std::weak_ptr<AOSet>& weak_set, const std::vector<S32>& states, bool may_split)
    {
        const std::shared_ptr<AOSet> set = weak_set.lock();
        if (!set)
        {
            return;
        }

        std::vector<S32> failed;
        for (S32 index : states)
        {
            AOState& state = set->getState(index);
            if (state.mFolderID.isNull() && state.mFolderRequested)
            {
                state.mFolderRequested = false;
                failed.push_back(index);
            }
        }

        // None of several made: in case the service wants one folder per request.
        if (may_split && failed.size() > 1 && failed.size() == states.size())
        {
            LL_WARNS("PlusAO") << "Creating " << failed.size() << " state folders in one request failed; one request per folder" << LL_ENDL;
            for (S32 index : failed)
            {
                requestStateFolders(set, { index }, false);
            }
            return;
        }

        for (S32 index : failed)
        {
            AOState& state = set->getState(index);
            dropUnlinked(*set, state, 0, 0);
            if (!state.mTracks.empty())
            {
                mEngine.replaceTracks(*set, state, {});
            }
        }
    }

    void AOStore::requestLinks(const std::shared_ptr<AOSet>& set, AOState& state, U32 key, const std::vector<size_t>& indices)
    {
        LLUUID                    folder;
        std::vector<AOAnimation>* list = containerList(state, key, folder);
        if (!list)
        {
            return;
        }

        static U32 sLastRequest = 0;
        const U32  request      = ++sLastRequest;

        LLSD links = LLSD::emptyArray();
        for (size_t index : indices)
        {
            AOAnimation& anim = (*list)[index];
            anim.mLinkRequest = request;

            // As link_inventory_array() builds it, with the place in the list as
            // the description, so no second request is needed to number it.
            LLSD link;
            link["linked_id"] = anim.mOriginalID;
            link["type"]      = (S8)LLAssetType::AT_LINK;
            link["inv_type"]  = (S8)LLInventoryType::IT_ANIMATION;
            link["name"]      = anim.mName;
            link["desc"]      = llformat("%d", anim.mSortOrder);
            links.append(link);
        }

        LLSD body;
        body["links"] = links;

        const std::weak_ptr<AOSet> weak  = set;
        const S32                  index = state.mIndex;
        batch_ptr_t batch = newBatch(llformat("link %d animation(s) in %s", (S32)indices.size(), state.getName().c_str()),
                                     [weak, index, key, request](S32)
                                     {
                                         if (AOStore* store = AOStore::live())
                                         {
                                             store->onLinksDone(weak, index, key, request);
                                         }
                                     });
        batch->addRequest();
        AISAPI::CreateInventory(folder, body, [weak, index, key, batch](const LLUUID& id)
        {
            if (id.isNull())
            {
                batch->addFailure();
                return;
            }
            if (AOStore* store = AOStore::live())
            {
                store->onLinkCreated(weak, index, key, id);
            }
        });
    }

    void AOStore::onLinkCreated(const std::weak_ptr<AOSet>& weak_set, S32 state_index, U32 key, const LLUUID& link_id)
    {
        const std::shared_ptr<AOSet> set  = weak_set.lock();
        const LLViewerInventoryItem* link = gInventory.getItem(link_id);
        if (!set || !link)
        {
            return; // the set's folder is being removed, and the link with it
        }

        AOState&                  state = set->getState(state_index);
        LLUUID                    folder;
        std::vector<AOAnimation>* list = containerList(state, key, folder);
        if (!list)
        {
            // The group or track went while the server made the link.
            removeLinks({ link_id }, "remove the link of a removed animation");
            return;
        }
        if (link->getParentUUID() != folder)
        {
            return; // made in a folder that is being removed
        }

        for (AOAnimation& anim : *list)
        {
            if (!anim.mIsGroup && anim.mLinkID.isNull() && anim.mLinkRequest && anim.mOriginalID == link->getLinkedUUID())
            {
                anim.mLinkID = link_id;
                resolveAsset(anim);

                // Moved while the server made it: the number it was sent with is old.
                S32 sort_order = -1;
                LLStringUtil::convertToS32(link->getActualDescription(), sort_order);
                if (sort_order != anim.mSortOrder)
                {
                    writeSortOrders({ { link_id, anim.mSortOrder } }, "number a new link in " + state.getName());
                }
                return;
            }
        }

        // Nothing waits for it: removed from the list while the server made it.
        removeLinks({ link_id }, "remove the link of a removed animation");
    }

    void AOStore::onLinksDone(const std::weak_ptr<AOSet>& weak_set, S32 state_index, U32 key, U32 request)
    {
        if (const std::shared_ptr<AOSet> set = weak_set.lock())
        {
            if (!dropUnlinked(*set, set->getState(state_index), key, request))
            {
                // The window shows animations without a link in italics.
                mEngine.notifyStateChanged(*set, state_index);
            }
        }
    }

    bool AOStore::dropUnlinked(AOSet& set, AOState& state, U32 key, U32 request)
    {
        LLUUID                    folder;
        std::vector<AOAnimation>* list = containerList(state, key, folder);
        if (!list)
        {
            return false;
        }

        std::vector<AOAnimation> kept;
        std::string              failed;
        for (const AOAnimation& anim : *list)
        {
            // request 0: the folder could not be made, so nothing without a link stays.
            const bool lost = anim.mLinkID.isNull() && (request ? anim.mLinkRequest == request : true);
            if (lost)
            {
                failed += (failed.empty() ? "" : ", ") + anim.mName;
            }
            else
            {
                kept.push_back(anim);
            }
        }
        if (failed.empty())
        {
            return false;
        }

        LL_WARNS("PlusAO") << "Could not link in " << state.getName() << ": " << failed << LL_ENDL;
        LLSD args;
        args["STATE"] = aoText(llformat("state_%d", state.mIndex), LLSD());
        args["NAMES"] = failed;
        notifyUser("ao_link_failed", args, true);
        if (key == 0)
        {
            mEngine.replaceAnimations(set, state, std::move(kept));
            return true;
        }

        *list = std::move(kept);
        // A group that lost members: back to a single animation, or gone.
        for (size_t n = 0; n < state.mAnimations.size(); ++n)
        {
            if (state.mAnimations[n].mIsGroup && state.mAnimations[n].mKey == key)
            {
                mEngine.normalizeGroup(set, state, n);
                break;
            }
        }
        mEngine.afterListEdit(set, state);
        return true;
    }

    void AOStore::removeLinks(const std::vector<LLUUID>& links, const std::string& label)
    {
        if (links.empty())
        {
            return;
        }
        // The inventory service removes one item per request; they all go now,
        // and nothing waits for them.
        batch_ptr_t batch = newBatch(label);
        for (const LLUUID& id : links)
        {
            batch->addRequest();
            AISAPI::RemoveItem(id, [batch](const LLUUID&) {});
        }
    }

    bool AOStore::removeFolder(const LLUUID& folder, const std::string& label)
    {
        if (folder.isNull())
        {
            return true;
        }
        // A protected inventory folder, or one holding one, or one an RLV folder lock keeps, stays
        // (plus/invguard, whose guard tells the user).
        if (plus::invguard::refusesRemoval(folder))
        {
            return false;
        }
        // Only animation links are removed for good. Other items go to Lost and Found, but the
        // folder stays this time: the move is a message the server may handle after a removal
        // sent now, and the items would go with the folder. Removed again later, it holds none.
        if (holdsForeignItems(folder))
        {
            notifyUser(moveForeignItems(folder) ? "ao_folder_kept_moved" : "ao_folder_kept_foreign", LLSD(), true);
            return false;
        }

        batch_ptr_t batch = newBatch(label);
        batch->addRequest();
        AISAPI::RemoveCategory(folder, [batch](const LLUUID&) {});
        return true;
    }

    void AOStore::writeSortOrders(const std::vector<std::pair<LLUUID, S32>>& orders, const std::string& label)
    {
        if (orders.empty())
        {
            return;
        }
        batch_ptr_t batch = newBatch(label);
        for (const auto& [link_id, sort_order] : orders)
        {
            batch->addRequest();
            AISAPI::UpdateItem(link_id, LLSD().with("desc", llformat("%d", sort_order)), [batch](const LLUUID&) {});
        }
    }

    void AOStore::saveNow()
    {
        mSaveDue = false;
        if (!mLoaded)
        {
            return;
        }

        batch_ptr_t batch;
        auto rename = [this, &batch](const LLUUID& folder, const std::string& name)
        {
            if (!batch)
            {
                batch = newBatch("save set and state names");
            }
            batch->addRequest();
            AISAPI::UpdateCategory(folder, LLSD().with("name", name), [batch](const LLUUID&) {});
        };

        for (const AOEngine::set_ptr_t& set : mEngine.mSets)
        {
            // Without a folder the flags are written when it is made.
            if (set->isDirty() && set->getFolderID().notNull())
            {
                rename(set->getFolderID(), set->folderName(mEngine.mDefaultSet == set.get()));
                set->setDirty(false);
            }
            for (S32 i = 0; i < STATE_COUNT; ++i)
            {
                AOState& state = set->getState(i);
                if (state.mDirty && state.mFolderID.notNull())
                {
                    rename(state.mFolderID, state.folderName());
                    state.mDirty = false;
                }
                for (AOAnimation& group : state.mAnimations)
                {
                    if (group.mIsGroup && group.mFolderDirty && group.mFolderID.notNull())
                    {
                        rename(group.mFolderID, groupFolderName(group.mName, group.mSortOrder));
                        group.mFolderDirty = false;
                    }
                }
                for (AOTrack& track : state.mTracks)
                {
                    if (track.mDirty && track.mFolderID.notNull())
                    {
                        rename(track.mFolderID, track.folderName());
                        track.mDirty = false;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // Taking over Firestorm's sets

    bool AOStore::findFirestormAO(LLUUID& found)
    {
        found.setNull();
        LLUUID fs_root;
        if (!findChildFolder(gInventory.getRootFolderID(), FS_ROOT_FOLDER_NAME, fs_root))
        {
            return false;
        }
        if (fs_root.isNull())
        {
            return true;
        }
        LLUUID fs_ao;
        if (!findChildFolder(fs_root, AO_FOLDER_NAME, fs_ao))
        {
            return false;
        }
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        if (fs_ao.notNull() && (trash.isNull() || !gInventory.isObjectDescendentOf(fs_ao, trash)))
        {
            found = fs_ao;
        }
        return true;
    }

    void AOStore::stepOffer()
    {
        LLUUID fs_ao;
        if (!findFirestormAO(fs_ao))
        {
            return; // asked again at the next poll
        }
        if (fs_ao.notNull() && !gInventory.isCategoryComplete(fs_ao))
        {
            gInventory.fetchDescendentsOf(fs_ao);
            return; // the list of its sets is not complete yet: asked again at the next poll
        }
        mOfferDue = false;
        if (fs_ao.isNull() || !mEngine.getSets().empty())
        {
            return;
        }

        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(fs_ao, cats, items);
        if (!cats || cats->empty())
        {
            return; // no set known in it; the window's button is there for later
        }

        LLNotificationsUtil::add("PlusAOFirestormOffer", LLSD(), LLSD(),
                                 [](const LLSD& notification, const LLSD& response)
                                 {
                                     switch (LLNotificationsUtil::getSelectedOption(notification, response))
                                     {
                                         case 0:
                                             if (AOStore* store = AOStore::live())
                                             {
                                                 store->importFromFirestorm();
                                             }
                                             break;
                                         case 2:
                                             gSavedPerAccountSettings.setBOOL(SETTING_OFFER, false); // "Do not ask again"
                                             break;
                                         default:
                                             break;
                                     }
                                 });
    }

    void AOStore::importFromFirestorm()
    {
        if (!mLoaded || mPhase != PHASE_IDLE || mImporting || mAOFolderRequested || !AISAPI::isAvailable())
        {
            notifyUser("ao_fs_busy", LLSD());
            return;
        }

        LLUUID fs_ao;
        if (!findFirestormAO(fs_ao))
        {
            notifyUser("ao_fs_busy", LLSD()); // the inventory is still loading
            return;
        }
        if (fs_ao.isNull())
        {
            notifyUser("ao_fs_none", LLSD());
            return;
        }

        mImporting = true;
        // Everything below the folder must be known before it can be copied.
        batch_ptr_t batch = newBatch("fetch Firestorm's AO folder", [fs_ao](S32 failures)
        {
            AOStore* store = AOStore::live();
            if (!store)
            {
                return;
            }
            if (failures > 0)
            {
                store->mImporting = false;
                notifyUser("ao_fs_failed", LLSD(), true);
                return;
            }
            store->copyFirestormSets(fs_ao);
        });
        batch->addRequest();
        AISAPI::FetchCategoryChildren(fs_ao, AISAPI::INVENTORY, true, [batch](const LLUUID& id)
        {
            if (id.isNull())
            {
                batch->addFailure();
            }
        });
    }

    void AOStore::copyFirestormSets(const LLUUID& fs_ao)
    {
        if (!validateFolders() || !gInventory.getCategory(fs_ao))
        {
            mImporting = false;
            notifyUser("ao_fs_failed", LLSD(), true);
            return;
        }
        if (mAOFolder.isNull())
        {
            // Make #Charis/#AO first, then come back here.
            mOnAOFolder = [fs_ao]()
            {
                if (AOStore* store = AOStore::live())
                {
                    store->copyFirestormSets(fs_ao);
                }
            };
            ensureAOFolder();
            if (!mAOFolderRequested)
            {
                // It did not start (the folders are being looked for, or the service is gone).
                mOnAOFolder = nullptr;
                mImporting  = false;
                notifyUser("ao_fs_busy", LLSD());
            }
            return;
        }

        // Taken from the whole fetch of the folder: a set with links missing would count as
        // "already here" at the next import and could only be mended by hand.
        if (!isFetched(fs_ao, 4))
        {
            mImporting = false;
            notifyUser("ao_fs_failed", LLSD(), true);
            return;
        }

        // The set names we have: a set of the same name is not copied again.
        auto base_name = [](const std::string& folder_name) { return folder_name.substr(0, folder_name.find(':')); };
        std::unordered_set<std::string> have;
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(mAOFolder, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                have.insert(base_name(cat->getName()));
            }
        }

        std::vector<std::string> names;
        const auto sources = std::make_shared<std::multimap<std::string, LLUUID>>();
        S32        skipped = 0;
        gInventory.getDirectDescendentsOf(fs_ao, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (have.count(base_name(cat->getName())))
                {
                    ++skipped;
                    continue;
                }
                names.push_back(cat->getName());
                sources->emplace(cat->getName(), cat->getUUID());
            }
        }

        const S32 count = (S32)names.size();
        if (names.empty())
        {
            mImporting = false;
            if (skipped == 0)
            {
                notifyUser("ao_fs_none", LLSD()); // the folder holds no set
                return;
            }
            notifyUser("ao_fs_nothing_new", LLSD().with("SKIPPED", skipped));
            return;
        }

        batch_ptr_t batch = newBatch("take over Firestorm's AO sets", [count, skipped](S32 failures)
        {
            AOStore* store = AOStore::live();
            if (!store)
            {
                return;
            }
            store->mImporting = false;
            // What came in is read once these requests are answered.
            store->mVerifyDue = true;
            store->mVerifyTimer.reset();
            const LLSD args = LLSD().with("COUNT", count).with("SKIPPED", skipped);
            notifyUser(failures > 0 ? "ao_fs_partial" : "ao_fs_done", args, failures > 0);
        });
        copyFolders(mAOFolder, names, sources, batch);
    }

    // The folders of one level, then what is in each of them.
    void AOStore::copyFolders(const LLUUID& target, const std::vector<std::string>& names,
                              const std::shared_ptr<std::multimap<std::string, LLUUID>>& sources, const batch_ptr_t& batch)
    {
        createFolders(target, names, batch, [sources, batch](const LLUUID& id)
        {
            const LLViewerInventoryCategory* cat = id.notNull() ? gInventory.getCategory(id) : nullptr;
            AOStore*                         store = AOStore::live();
            if (!cat || !store)
            {
                return;
            }
            const auto it = sources->find(cat->getName());
            if (it != sources->end())
            {
                const LLUUID source = it->second;
                sources->erase(it);
                store->copyContents(source, id, batch);
            }
        });
    }

    void AOStore::copyContents(const LLUUID& source, const LLUUID& target, const batch_ptr_t& batch)
    {
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(source, cats, items);

        // The links as they are, with the description that holds the place in the list.
        LLSD links = LLSD::emptyArray();
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (!item->getIsLinkType() || item->getInventoryType() != LLInventoryType::IT_ANIMATION)
                {
                    continue; // only animation links belong in an AO folder
                }
                LLSD link;
                link["linked_id"] = item->getLinkedUUID();
                link["type"]      = (S8)LLAssetType::AT_LINK;
                link["inv_type"]  = (S8)LLInventoryType::IT_ANIMATION;
                link["name"]      = item->getName();
                link["desc"]      = item->getDescription();
                links.append(link);
            }
        }
        if (links.size() > 0)
        {
            LLSD body;
            body["links"] = links;
            batch->addRequest();
            AISAPI::CreateInventory(target, body, [batch](const LLUUID& id)
            {
                if (id.isNull())
                {
                    batch->addFailure();
                }
            });
        }

        if (!cats || cats->empty())
        {
            return;
        }
        std::vector<std::string> names;
        const auto sources = std::make_shared<std::multimap<std::string, LLUUID>>();
        for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
        {
            names.push_back(cat->getName());
            sources->emplace(cat->getName(), cat->getUUID());
        }
        copyFolders(target, names, sources, batch);
    }
}
