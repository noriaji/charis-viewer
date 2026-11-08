/**
 * @file aoengine.h
 * @brief Animation Overrider: the engine that replaces the agent's Linden
 *        motions with the animations of the current set, and the in-memory
 *        sets the window edits.
 *
 * Ported from Firestorm's aoengine.h (see SOURCE.md). The overriding itself
 * is Firestorm's; what differs is how sets are edited: every change is made
 * in memory at once and AOStore writes it to the inventory in the
 * background, instead of Firestorm's reload of the whole AO after each one
 * (research/ao-port.md in the sl-viewer repository).
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

#ifndef PLUS_AO_AOENGINE_H
#define PLUS_AO_AOENGINE_H

#include "aoset.h"
#include "aostore.h"

#include "llframetimer.h"
#include "llpointer.h"
#include "llsingleton.h"

#include <boost/signals2.hpp>
#include <memory>
#include <unordered_set>
#include <vector>

class LLViewerInventoryItem;

namespace plus::ao
{
    // A set to create in one go (file and notecard import): the animations are
    // inventory items of the agent, already looked up.
    struct NewSet
    {
        typedef std::vector<LLPointer<LLViewerInventoryItem>> items_t;

        // An entry of a state's main list: one animation, or a group (mName).
        struct Step
        {
            bool        mIsGroup = false;
            std::string mName;
            items_t     mItems;
        };

        struct Track
        {
            std::string mName;
            bool        mCycle = false;
            bool        mRandom = false;
            F32         mCycleTime = 0.f;
            items_t     mItems;
        };

        struct State
        {
            bool               mCycle = false;
            bool               mRandom = false;
            F32                mCycleTime = 0.f;
            std::vector<Step>  mSteps;
            std::vector<Track> mTracks;
        };

        std::string                       mName;
        bool                              mSitOverride = false;
        bool                              mSmart = false;
        bool                              mMouselookStandDisable = false;
        std::array<State, STATE_COUNT>    mStates;
    };

    // Which list of a state an edit is about: the main list (both -1), the members of
    // the group at index mGroup of the main list, or the track at index mTrack.
    struct ListRef
    {
        S32 mTrack = -1;
        S32 mGroup = -1;
    };

    class AOEngine : public LLSingleton<AOEngine>
    {
        LLSINGLETON(AOEngine);
        ~AOEngine();

        friend class AOStore;

    public:
        enum ECycleMode
        {
            CycleAny,
            CycleNext,
            CyclePrevious
        };

        typedef std::shared_ptr<AOSet> set_ptr_t;
        typedef std::vector<set_ptr_t> set_list_t;

        // --- Called by plus/ao/ao.cpp ---------------------------------------

        // LLVOAvatar::startMotion()/stopMotion() of the agent: the animation
        // replacing motion, or null to let the Linden motion play.
        LLUUID override(const LLUUID& motion, bool start);
        // Every frame: loading and saving, the timers, mouselook and water.
        void onIdle();
        // The app is quitting: remember what plays, write unsaved names.
        void onQuitting();
        // Only the settings part of onQuitting(): the current set and what each
        // state plays into PlusAOCurrentState (nothing before the AO was read).
        // Also for an end that skips "quitting" (plus/vkcheck: the GPU lost).
        void rememberCurrentState();

        // --- The overrider (Firestorm) --------------------------------------

        void enable(bool enable);
        void enableStands(bool enable_stands);
        bool isEnabled() const { return mEnabled; }

        void cycle(ECycleMode cycle_mode, bool reset_timer = false);
        // Plays entry index of the state's main list now, if it is the state being played;
        // member >= 0: only that member of the group (to look at it alone).
        void playAnimation(AOSet* set, S32 state, size_t index, S32 member = -1);
        // Plays animation index of a track now (the track's cycle goes on from there).
        void playTrackAnimation(AOSet* set, S32 state, S32 track, size_t index);

        void   selectSet(AOSet* set);
        AOSet* getCurrentSet() const { return mCurrentSet; }
        // The state of the current set the agent is in, or null.
        const AOState* getCurrentState() const;
        // Whether entry index of the state's main list is the one the overrider plays.
        bool isPlaying(const AOSet* set, S32 state, size_t index) const;
        bool isTrackPlaying(const AOSet* set, S32 state, S32 track, size_t index) const;

        // --- Sets ------------------------------------------------------------

        const set_list_t& getSets() const { return mSets; }
        AOSet*            findSet(U32 serial) const;
        AOSet*            findSetByName(const std::string& name) const;
        AOSet*            getDefaultSet() const { return mDefaultSet; }
        // name, or "name 2", "name 3", ... if a set is called name already.
        std::string       uniqueSetName(const std::string& name) const;

        // The inventory has been read; sets can be edited.
        bool isLoaded() const { return mStore.isLoaded(); }
        // Inventory requests still on their way (shown in the window).
        S32  pendingRequests() const { return mStore.pendingRequests(); }
        const AOStore& getStore() const { return mStore; }

        // --- Editing: memory first, the inventory follows ----------------------
        // All of these change what the window shows and what plays at once;
        // AOStore sends the inventory changes as one batch per call.

        AOSet* addSet(const std::string& name);
        AOSet* addSet(const NewSet& description);
        // A new set with the animations, groups, tracks and flags of set (not its default mark).
        AOSet* cloneSet(const AOSet* set, const std::string& name);
        void   removeSet(AOSet* set);
        // False if the name is not valid or another set has it.
        bool   renameSet(AOSet* set, const std::string& name);

        void setDefaultSet(AOSet* set);
        void setOverrideSits(AOSet* set, bool override_sit);
        void setSmart(AOSet* set, bool smart);
        void setDisableMouselookStands(AOSet* set, bool disabled);
        void setCycle(AOSet* set, S32 state, bool cycle);
        void setRandomize(AOSet* set, S32 state, bool randomize);
        void setCycleTime(AOSet* set, S32 state, F32 time);

        // Items may be animations or links to them; all go into one request.
        void addAnimations(AOSet* set, S32 state, const std::vector<LLPointer<LLViewerInventoryItem>>& items, ListRef list = ListRef());
        void removeAnimations(AOSet* set, S32 state, std::vector<size_t> indices, ListRef list = ListRef());
        // Swaps animation index with the one above (up) or below; false at the end.
        bool moveAnimation(AOSet* set, S32 state, size_t index, bool up, ListRef list = ListRef());

        // Groups: entries of the main list that play their members together.
        // The single animation at target_index and items become a group.
        void createGroup(AOSet* set, S32 state, size_t target_index, const std::vector<LLPointer<LLViewerInventoryItem>>& items);
        // The animations of an inventory folder become a group; false if it has none.
        bool createGroupFromFolder(AOSet* set, S32 state, const LLUUID& folder);
        // A member leaves its group and becomes an entry of the main list after it.
        void extractMember(AOSet* set, S32 state, size_t group_index, size_t member_index);
        bool renameGroup(AOSet* set, S32 state, size_t group_index, const std::string& name);

        // Tracks: further lists of a state, played next to the main list.
        // Returns the index of the new track, or -1.
        S32  addTrack(AOSet* set, S32 state);
        void removeTrack(AOSet* set, S32 state, S32 track);
        void setTrackCycle(AOSet* set, S32 state, S32 track, bool cycle);
        void setTrackRandomize(AOSet* set, S32 state, S32 track, bool randomize);
        void setTrackCycleTime(AOSet* set, S32 state, S32 track, F32 time);

        // Reads the #AO folder again (the window's Reload button). Keeps the
        // overrider running; only what changed in the inventory changes.
        void reload();
        // Copies the sets of Firestorm's #AO folder into ours (the window's button).
        void importFromFirestorm();

        // --- Signals for the window ------------------------------------------

        typedef boost::signals2::signal<void()> signal_t;
        typedef boost::signals2::signal<void(U32 set_serial, S32 state)> state_signal_t;

        // The list of sets, their names or flags changed (or loading finished).
        boost::signals2::connection setSetsChangedCallback(const signal_t::slot_type& cb) { return mSetsChangedSignal.connect(cb); }
        // The animations of one state changed.
        boost::signals2::connection setStateChangedCallback(const state_signal_t::slot_type& cb) { return mStateChangedSignal.connect(cb); }
        // What the overrider plays changed.
        boost::signals2::connection setAnimationChangedCallback(const signal_t::slot_type& cb) { return mAnimationChangedSignal.connect(cb); }
        // Loading, or the number of pending requests, changed.
        boost::signals2::connection setStatusChangedCallback(const signal_t::slot_type& cb) { return mStatusChangedSignal.connect(cb); }

    private:
        void init();
        void onLoaded();

        void onToggleAOControl();
        void onToggleAOStandsControl();
        void onRegionChange();

        void setLastMotion(const LLUUID& motion);
        void setLastOverriddenMotion(const LLUUID& motion);
        void setStateCycleTimer(const AOState& state);
        void cycleTimeout();
        void checkSitCancel();
        void checkBelowWater(bool check_underwater);
        void inMouselook(bool mouselook);

        // Stops every animation the state plays (sent to the region and stopped here).
        void stopAnimations(AOState& state);
        // Clears the state's animations and stops all but the first (the caller of override() stops that).
        void stopExtras(AOState& state);
        // Starts the animations of assets that do not play yet and stops those of the state that are not
        // in it. False if that changed nothing.
        bool swapPlaying(AOState& state, const std::vector<LLUUID>& assets);
        // Whether the engine is playing the state now: the state of the motion, or the active "Always".
        bool isStateActive(const AOState& state) const;
        // After the entries of a state changed: the playing follows the list.
        void syncPlaying(AOSet& set, AOState& state);

        // Tracks of the state being played, and the "Always" state with its own tracks.
        void startStateTracks(AOState& state);
        void stopActiveTracks();
        void startAlwaysAnimations();
        void stopAlwaysAnimations();
        void refreshTracks(AOState& state);
        void swapTrackAnimation(AOTrack& track, const LLUUID& animation);
        void advanceTrack(AOTrack& track);
        void tickTracks(AOState& state);
        void tickAlways();
        void reassertAlways(bool only_missing);

        // The list an edit is about, or null if the indices do not name one.
        static std::vector<AOAnimation>* listFor(AOState& state, ListRef list);
        // Numbers the entries of the main list 0, 1, 2, ... and writes what changed.
        void renumberMain(AOSet& set, AOState& state);
        // A group with less than two members goes back to a single animation (or is gone).
        void normalizeGroup(AOSet& set, AOState& state, size_t group_index);

        void stopAllStandVariants();
        void stopAllSitVariants();
        // Stops everything the set plays (removing or leaving the current set).
        void stopSetAnimations(AOSet& set);
        // After the current set changed: lets the new set take over the motion.
        void startCurrentSet();

        bool     foreignAnimations() const;
        AOState* mapSwimming(const LLUUID& motion) const;
        AOState* getStateForMotion(const LLUUID& motion) const;

        set_ptr_t findSetPtr(const AOSet* set) const;
        void      chooseCurrentSet();

        // From AOStore after it read the inventory.
        void replaceAnimations(AOSet& set, AOState& state, std::vector<AOAnimation>&& animations);
        void replaceTracks(AOSet& set, AOState& state, std::vector<AOTrack>&& tracks);
        // The entries of a group or track list that could not be linked are gone (AOStore::dropUnlinked()).
        void afterListEdit(AOSet& set, AOState& state);
        // If the current set is gone (or there was none), another takes over;
        // start: and plays at once (not while loading, onLoaded() does that).
        void replaceSets(set_list_t&& sets, AOSet* default_set, bool start = true);
        // A set whose folder could not be created is dropped again.
        void dropSet(const AOSet* set);
        void restoreSavedAnimations(AOSet& set);
        // The folders and links of all sets (gathered once for a whole inventory change,
        // instead of going through every animation for each id that went).
        void collectInventoryIDs(std::unordered_set<LLUUID>& ids) const;

        void notifySetsChanged() { mSetsChangedSignal(); }
        void notifyStateChanged(const AOSet& set, S32 state) { mStateChangedSignal(set.getSerial(), state); }
        void notifyAnimationChanged() { mAnimationChangedSignal(); }
        void notifyStatusChanged() { mStatusChangedSignal(); }

        AOStore mStore;

        signal_t       mSetsChangedSignal;
        state_signal_t mStateChangedSignal;
        signal_t       mAnimationChangedSignal;
        signal_t       mStatusChangedSignal;

        bool mInitialized = false; // init() ran (the agent's avatar is fully loaded)
        bool mEnabled = false;
        bool mEnabledStands = false;
        bool mInMouselook = false;
        bool mUnderWater = false;

        LLUUID mLastMotion;
        LLUUID mLastOverriddenMotion;
        LLUUID mTransitionId;
        // This motion is ignored once in the overrider when stopping; fixes a case
        // where the AO doesn't correctly start up on login or when enabled manually.
        LLUUID mIgnoreMotionStopOnce;

        set_list_t mSets;
        AOSet*     mCurrentSet = nullptr;
        AOSet*     mDefaultSet = nullptr;

        // The tracks of the state being played (valid while the set exists: stopSetAnimations() ends them),
        // and the "Always" state with its tracks while it plays.
        AOState*     mActiveTrackState = nullptr;
        AOState*     mActiveAlwaysState = nullptr;
        LLFrameTimer mAlwaysCheckTimer;

        // Smart sit: checks ten times every 0.1 s whether the seat plays its own animation.
        LLFrameTimer mSitCancelTimer;
        S32          mSitCancelTicks = 0;

        boost::signals2::scoped_connection mUseAOConnection;
        boost::signals2::scoped_connection mUseAOStandsConnection;
        boost::signals2::scoped_connection mRegionChangeConnection;
    };
}

#endif // PLUS_AO_AOENGINE_H
