/**
 * @file aoset.h
 * @brief Animation Overrider: a set of animations, its states and the
 *        inventory folder names they are stored under.
 *
 * Ported from Firestorm's aoset.h (see SOURCE.md).
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

#ifndef PLUS_AO_AOSET_H
#define PLUS_AO_AOSET_H

#include "llframetimer.h"
#include "lluuid.h"

#include <array>
#include <string>
#include <vector>

namespace plus::ao
{
    // Keep number and order in sync with the table in aoset.cpp. The names are
    // the folder names of Firestorm's #AO layout (and of ZHAO notecards).
    enum EState : S32
    {
        Standing = 0,
        Walking,
        Running,
        Sitting,
        SittingOnGround,
        Crouching,
        CrouchWalking,
        Landing,
        SoftLanding,
        StandingUp,
        Falling,
        FlyingDown,
        FlyingUp,
        Flying,
        FlyingSlow,
        Hovering,
        Jumping,
        PreJumping,
        TurningRight,
        TurningLeft,
        Typing,
        Floating,
        SwimmingForward,
        SwimmingUp,
        SwimmingDown,
        AlwaysPlay, // plays whenever the AO is on, whatever the avatar does (FS "Always")
        STATE_COUNT
    };

    // The state's folder name, e.g. "Standing".
    const std::string& stateName(S32 state);
    // The state a folder or notecard name refers to ("Standing", "Stand.1", ...), or -1.
    S32 stateByName(const std::string& name);

    struct AOAnimation
    {
        std::string mName;          // a group: its name
        LLUUID      mAssetID;       // what is played; null until the linked item is known
        LLUUID      mOriginalID;    // the animation item the link points to
        LLUUID      mLinkID;        // the link in the state folder; null until the server made it
        S32         mSortOrder = 0; // the link's description (a group: the ":OR" of its folder name)
        U32         mLinkRequest = 0; // the request making the link (AOStore); 0: not asked for yet

        // A group plays its members together. It is a subfolder of the state
        // folder; its members' links are in it. A group has no asset or link of its own.
        bool                     mIsGroup = false;
        std::vector<AOAnimation> mMembers;
        LLUUID                   mFolderID;                // null until the server made it
        bool                     mFolderRequested = false;
        bool                     mFolderDirty = false;     // name or order not yet written
        U32                      mKey = 0;                 // names a group while requests are on their way (newKey())
    };

    // A number that is never given twice (groups and tracks).
    U32 newKey();

    // The assets a list entry plays: its own, or those of the members of a group
    // (those whose asset is known).
    void stepAssets(AOAnimation& step, std::vector<LLUUID>& assets);

    // An independent animation list of a state, played next to the main list
    // while the state is played; it cycles by itself (FS: AOTrack).
    // Folder: "<name>:TR[:CTn.nn][:CY][:RN]" below the state folder.
    struct AOTrack
    {
        U32                      mKey = newKey();
        std::string              mName;
        LLUUID                   mFolderID;
        bool                     mFolderRequested = false;
        bool                     mCycle = false;
        bool                     mRandom = false;
        F32                      mCycleTime = 0.f;
        bool                     mDirty = false; // flags not yet written to the folder name
        std::vector<AOAnimation> mAnimations;    // singles only
        U32                      mCurrentAnimation = 0;
        LLUUID                   mPlayingID;
        LLFrameTimer             mTimer;

        std::string folderName() const;
        // Reads a track folder name; false if it is no track (no ":TR").
        static bool parseFolderName(const std::string& folder_name, std::string& name, bool& cycle, bool& random, F32& cycle_time);
    };

    // The order of a state's tracks: by name, numbers as numbers ("Track 2" before "Track 10"), then by folder. The
    // inventory gives the folders in no set order; reading and adding both keep this one, so a reload does not move
    // them (research/275 L098).
    bool trackBefore(const AOTrack& a, const AOTrack& b);

    // A group's folder name: "<name>:OR<place in the list>".
    std::string groupFolderName(const std::string& name, S32 order);
    // Reads one; false if there is no name. order: -1 without the ":OR" part.
    bool parseGroupFolderName(const std::string& folder_name, std::string& name, S32& order);
    // Whether name can be a group or track name (no ':' or '|', no control characters, trimmed).
    bool isValidFolderPart(const std::string& name, S32 max_length = 0);

    struct AOState
    {
        S32                      mIndex = 0; // EState
        LLUUID                   mFolderID;  // null while the state has no folder
        bool                     mFolderRequested = false;
        bool                     mCycle = false;
        bool                     mRandom = false;
        F32                      mCycleTime = 0.f;
        bool                     mDirty = false; // flags not yet written to the folder name
        std::vector<AOAnimation> mAnimations;    // the main list: single animations and groups
        std::vector<AOTrack>     mTracks;
        U32                      mCurrentAnimation = 0;
        // The assets the overrider started for this state (FS: mCurrentAnimationIDs): one, or the
        // members of the group being played. The first is the one override() returns.
        std::vector<LLUUID>      mPlaying;
        LLFrameTimer             mStepTimer; // AlwaysPlay only: when its main list goes on

        const std::string& getName() const { return stateName(mIndex); }
        const LLUUID&      getRemapID() const;
        bool               empty() const { return mAnimations.empty(); }
        // Something to be stored in the state folder: animations or tracks.
        bool               hasContent() const { return !mAnimations.empty() || !mTracks.empty(); }
        bool               isPlaying() const { return !mPlaying.empty(); }
        LLUUID             lead() const { return mPlaying.empty() ? LLUUID::null : mPlaying.front(); }

        // The next sort order for an animation added at the end.
        S32 nextSortOrder() const;
        // A group or track name nobody has in this state yet ("Group 1", "Track 1", ...).
        std::string nextGroupName() const;
        std::string nextTrackName() const;
        AOTrack*    findTrack(U32 key);
        AOAnimation* findGroup(U32 key);

        // "Standing:CT10.00:CY:RN"
        std::string folderName() const;
        // Reads the flags of a state folder name; false if the name names no state.
        static bool parseFolderName(const std::string& folder_name, S32& state, bool& cycle, bool& random, F32& cycle_time);
        // A cycle time read from a folder name or an imported file, within the
        // floater's spinner (0-999 seconds; not a number: 0).
        static F32 clampCycleTime(F32 seconds);
    };

    class AOSet
    {
    public:
        explicit AOSet(const std::string& name);

        AOSet(const AOSet&) = delete;
        AOSet& operator=(const AOSet&) = delete;

        // Never reused while the viewer runs; the window refers to sets by it.
        U32 getSerial() const { return mSerial; }

        const LLUUID& getFolderID() const { return mFolderID; }
        void          setFolderID(const LLUUID& id) { mFolderID = id; }
        bool          isFolderRequested() const { return mFolderRequested; }
        void          setFolderRequested(bool requested) { mFolderRequested = requested; }

        const std::string& getName() const { return mName; }
        void               setName(const std::string& name) { mName = name; }

        bool getSitOverride() const { return mSitOverride; }
        void setSitOverride(bool override_sit) { mSitOverride = override_sit; }
        bool getSmart() const { return mSmart; }
        void setSmart(bool smart) { mSmart = smart; }
        bool getMouselookStandDisable() const { return mMouselookStandDisable; }
        void setMouselookStandDisable(bool disable) { mMouselookStandDisable = disable; }

        // Name or flags not yet written to the folder name.
        bool isDirty() const { return mDirty; }
        void setDirty(bool dirty) { mDirty = dirty; }

        // The Linden motion this set currently overrides (not typing).
        const LLUUID& getMotion() const { return mMotion; }
        void          setMotion(const LLUUID& motion) { mMotion = motion; }

        AOState&       getState(S32 index) { return mStates[index]; }
        const AOState& getState(S32 index) const { return mStates[index]; }
        AOState*       getStateByRemapID(const LLUUID& id);

        // The animations to play when the state starts, advancing the cycle
        // (FS: AOSet::getAnimationsForState()): the assets of one entry of the
        // main list, those of the members for a group. Empty if the state has none,
        // or their assets are not known yet.
        void pickAnimation(AOState& state, std::vector<LLUUID>& assets) const;
        // The same for a track (null: nothing).
        static LLUUID pickTrackAnimation(AOTrack& track);

        // The cycle timer of the state being played (FS: the set's LLEventTimer).
        void startTimer(F32 period);
        void stopTimer() { mTimerRunning = false; }
        void resetTimer() { mTimer.reset(); }
        // True once per period while running.
        bool timerFired();

        // "Name:SO:SM:DM:**"
        std::string folderName(bool is_default) const;
        // Reads a set folder name; false if it has no name part.
        static bool parseFolderName(const std::string& folder_name, std::string& name, bool& sit_override, bool& smart,
                                    bool& mouselook_stand_disable, bool& is_default);
        // Whether name can be a set name: not empty, no ':' or '|' (the folder name's
        // separators), no control characters, and short enough for the flags.
        static bool isValidName(const std::string& name);

    private:
        const U32   mSerial;
        LLUUID      mFolderID;
        bool        mFolderRequested = false;
        std::string mName;
        bool        mSitOverride = false;
        bool        mSmart = false;
        bool        mMouselookStandDisable = false;
        bool        mDirty = false;
        LLUUID      mMotion;

        LLFrameTimer mTimer;
        F32          mTimerPeriod = 0.f;
        bool         mTimerRunning = false;

        std::array<AOState, STATE_COUNT> mStates;
    };

    // Finds the asset of an animation whose link was read before the linked
    // item arrived. Returns the (possibly still null) asset id.
    const LLUUID& resolveAsset(AOAnimation& anim);
}

#endif // PLUS_AO_AOSET_H
