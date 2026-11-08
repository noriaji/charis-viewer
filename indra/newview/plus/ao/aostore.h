/**
 * @file aostore.h
 * @brief Animation Overrider: keeps the sets in memory and the #AO folder of
 *        the inventory in step, in Firestorm's layout.
 *
 * Firestorm reloads the whole AO after each change. Here the engine changes
 * its sets in memory first and the store writes the change to the inventory
 * in the background, as few requests as the inventory service allows (one
 * per folder for new links, one for all new state folders of a set). The
 * inventory is only read again at login, from the Reload button, and when
 * something else changed the #AO folder (another viewer, the inventory
 * window): then only what differs is taken over.
 *
 * Layout (Firestorm's, below our own #Charis folder so that no folder of another viewer is
 * made in a user's inventory; the sets of Firestorm's #Firestorm/#AO are taken over on request):
 *   #Charis/#AO/<set name>[:SO][:SM][:DM][:**]/<state name>[:CTn.nn][:CY][:RN]/<links>
 *   ... /<state>/<group name>:OR<place in the list>/<links>   (a group: its members play together)
 *   ... /<state>/<track name>:TR[:CTn.nn][:CY][:RN]/<links>   (a track: a further list, played next to the main one)
 * A link's description is its place in the list (any increasing numbers).
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

#ifndef PLUS_AO_AOSTORE_H
#define PLUS_AO_AOSTORE_H

#include "aoset.h"

#include "llframetimer.h"
#include "lltimer.h"
#include "lluuid.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class LLSD;

namespace plus::ao
{
    class AOEngine;

    // Name of our folder in the inventory root ("#Charis"), where the AO sets live.
    // The inventory's folder icon (plus/invfoldericons) picks its own icon by it.
    const char* rootFolderName();

    // Shows a message of the AO window's strings (floater_plus_ao.xml) as a tip,
    // or as an alert that stays until it is closed (aofloater.cpp). The text values of args
    // go in through plus::safeText() (they may be someone else's names); note, a text of
    // ours (not passed through it), is added on a line of its own.
    void notifyUser(const std::string& string_name, const LLSD& args, bool alert = false, const std::string& note = std::string());
    // One of those strings, with [ARGS] filled in.
    std::string aoText(const std::string& string_name, const LLSD& args);

    // The inventory requests of one editing step. It is finished when the last
    // request has answered (the shared_ptr held by their callbacks is released);
    // then it logs how long the server took and runs its completion on the
    // next idle with the number of failed requests.
    class ServerBatch
    {
    public:
        typedef std::function<void(S32 failures)> done_func_t;

        ServerBatch(const std::string& label, done_func_t on_done);
        ~ServerBatch();

        void addRequest() { ++mRequests; }
        void addFailure() { ++mFailures; }

    private:
        std::string mLabel;
        LLTimer     mTimer;
        S32         mRequests = 0;
        S32         mFailures = 0;
        done_func_t mOnDone;
    };

    typedef std::shared_ptr<ServerBatch> batch_ptr_t;

    // Logs how long an editing step took in the viewer itself (PlusAOLogTiming).
    class LocalTiming
    {
    public:
        explicit LocalTiming(const std::string& label);
        ~LocalTiming();

    private:
        std::string mLabel;
        LLTimer     mTimer;
    };

    class AOStore
    {
        friend class ServerBatch; // onBatchDone()

    public:
        explicit AOStore(AOEngine& engine);

        bool          isLoaded() const { return mLoaded; }
        // Loading at login, or the Reload button's read, is going on.
        bool          isBusy() const { return mPhase != PHASE_IDLE; }
        // The Firestorm import is under way (the window's Reload and import buttons wait).
        bool          isImporting() const { return mImporting; }
        S32           pendingRequests() const { return mPending; }
        const LLUUID& getAOFolder() const { return mAOFolder; }

        // Every frame (from AOEngine::onIdle()).
        void onIdle();

        // Creates whatever the set lacks in the inventory: the #AO folder, the
        // set's folder, its state folders, links for new animations.
        void flush(const std::shared_ptr<AOSet>& set);
        void removeLinks(const std::vector<LLUUID>& links, const std::string& label);
        // False if the folder stays: protected, or holding a protected folder
        // (plus/protectedfolders, which tells the user), or holding anything that is
        // not an animation link (moved to Lost and Found now, the folder left for a
        // later removal; the user is told).
        bool removeFolder(const LLUUID& folder, const std::string& label);
        void writeSortOrders(const std::vector<std::pair<LLUUID, S32>>& orders, const std::string& label);
        // Writes changed set and state names a moment later (the cycle time
        // spinner sends many changes in a row).
        void scheduleSave() { mSaveDue = true; mSaveTimer.reset(); }
        void saveNow();

        // The window's Reload button: fetch the #AO folder again, then take over what differs.
        void reload();

        // Firestorm's #AO folder (#Firestorm/#AO). False while the inventory does not tell yet;
        // found is null when there is none (or it is in the trash).
        bool findFirestormAO(LLUUID& found);
        // Copies the sets of Firestorm's #AO folder into ours: the folders and links, with the
        // places in the lists; a set whose name we have already is left out. Tells the user
        // what was done.
        void importFromFirestorm();

        // From the inventory observer: something below #AO changed.
        void onInventoryChanged();

        // From a request's callback: the store, unless the engine is gone (quitting).
        static AOStore* live();

    private:
        enum EPhase
        {
            PHASE_WAIT_INVENTORY, // the inventory is not usable yet
            PHASE_FIND_FOLDERS,   // looking for #Charis/#AO
            PHASE_FETCH,          // waiting until the #AO folder's contents are fetched
            PHASE_RELOAD_FETCH,   // the Reload button's fetch is on its way
            PHASE_IDLE
        };

        enum ESyncReason
        {
            SYNC_LOGIN,  // the first read: pick the current set, restore what played
            SYNC_RELOAD, // the Reload button
            SYNC_VERIFY  // something else changed the folder
        };

        void stepLoading();
        // Finds a direct subfolder by name (null if there is none). Returns false
        // while the parent is not fetched far enough to tell.
        bool findChildFolder(const LLUUID& parent, const std::string& name, LLUUID& found);
        bool isFetched(const LLUUID& folder, S32 depth);
        // False when #Charis or #AO went away during the session (purged, or in the trash: another
        // viewer, the inventory window). Then their ids are forgotten and the folders are looked for
        // again (PHASE_FIND_FOLDERS; the read after it drops the sets that went with them).
        bool validateFolders();
        // owned_ids: the ids of the sets, gathered on first need and kept for the rest of one change.
        bool isAORelated(const LLUUID& id, std::optional<std::unordered_set<LLUUID>>& owned_ids) const;
        void sync(ESyncReason reason);
        void readStates(AOSet& set, bool is_new);
        bool moveForeignItems(const LLUUID& folder);
        // An item that is not an animation link in the folder or one under it.
        bool holdsForeignItems(const LLUUID& folder) const;

        void ensureAOFolder();
        // The import's steps: fetch Firestorm's folder, make ours if need be, copy the sets.
        void copyFirestormSets(const LLUUID& fs_ao);
        void copyFolders(const LLUUID& target, const std::vector<std::string>& names,
                         const std::shared_ptr<std::multimap<std::string, LLUUID>>& sources, const batch_ptr_t& batch);
        void copyContents(const LLUUID& source, const LLUUID& target, const batch_ptr_t& batch);
        // After login, when we have no set: offers the sets of Firestorm's folder, if it has any.
        void stepOffer();
        void requestSetFolder(const std::shared_ptr<AOSet>& set);
        // All in one request; may_split: if that fails as a whole, one request per folder.
        void requestStateFolders(const std::shared_ptr<AOSet>& set, const std::vector<S32>& states, bool may_split = true);
        // key: 0 for the state's main list, else the key of the group or track whose list it is.
        void requestLinks(const std::shared_ptr<AOSet>& set, AOState& state, U32 key, const std::vector<size_t>& indices);
        // The folder of a group or track (below the state folder).
        void requestContainerFolder(const std::shared_ptr<AOSet>& set, AOState& state, U32 key);
        void onContainerFolderCreated(const std::weak_ptr<AOSet>& weak_set, S32 state, U32 key, const LLUUID& folder_id);
        void onContainerFolderDone(const std::weak_ptr<AOSet>& weak_set, S32 state, U32 key);
        void onStateFolderCreated(const std::weak_ptr<AOSet>& weak_set, const LLUUID& folder_id);
        void onStateFoldersDone(const std::weak_ptr<AOSet>& weak_set, const std::vector<S32>& states, bool may_split);
        void onLinkCreated(const std::weak_ptr<AOSet>& weak_set, S32 state, U32 key, const LLUUID& link_id);
        void onLinksDone(const std::weak_ptr<AOSet>& weak_set, S32 state, U32 key, U32 request);
        // Animations whose link (of the request; 0: any, the folder failed)
        // could not be made leave the list again. False if there were none.
        bool dropUnlinked(AOSet& set, AOState& state, U32 key, U32 request);

        batch_ptr_t newBatch(const std::string& label, ServerBatch::done_func_t on_done = {});
        void        onBatchDone();

        AOEngine& mEngine;

        EPhase       mPhase = PHASE_WAIT_INVENTORY;
        bool         mLoaded = false; // the first read is done; stays true
        LLFrameTimer mPollTimer;      // the loading steps run twice a second
        LLUUID       mRootFolder;     // #Charis
        LLUUID       mAOFolder;       // #Charis/#AO; null until it exists
        bool         mAOFolderRequested = false;
        bool         mObserverAdded = false;
        bool         mImporting = false;   // the Firestorm import is under way
        bool         mOfferDue = false;    // looking for Firestorm's folder to offer its sets
        std::function<void()> mOnAOFolder; // run once the #AO folder exists (the import waits for it)

        S32          mPending = 0;  // batches not finished yet

        bool         mSaveDue = false;
        LLFrameTimer mSaveTimer;

        bool         mVerifyDue = false;
        LLFrameTimer mVerifyTimer;
    };
}

#endif // PLUS_AO_AOSTORE_H
