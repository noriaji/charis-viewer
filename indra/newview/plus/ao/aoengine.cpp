/**
 * @file aoengine.cpp
 * @brief Animation Overrider: the engine that replaces the agent's Linden
 *        motions with the animations of the current set, and the in-memory
 *        sets the window edits.
 *
 * Ported from Firestorm's aoengine.cpp (see SOURCE.md). The overrider
 * (override(), enable(), cycle(), the sit, mouselook and water handling) is
 * Firestorm's, with null checks added; the editing functions are new: they
 * change the sets in memory and leave the inventory to AOStore.
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

#include "aoengine.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llanimationstates.h"
#include "llinventorymodel.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "llxorcipher.h"
#include "plus/invguard/invguard.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace
{
    constexpr char SETTING_ENABLED[]       = "PlusAOEnabled";      // per account (FS: UseAO)
    constexpr char SETTING_STANDS[]        = "PlusAOStands";       // per account (FS: UseAOStands)
    constexpr char SETTING_CURRENT_STATE[] = "PlusAOCurrentState"; // per account (FS: FSCurrentAOState)

    // Firestorm's key for the animation ids remembered in the settings.
    const LLUUID ENCRYPTION_MAGIC_ID("4b552ff5-fd63-408c-8288-cd09429852ba");

    constexpr F32 SIT_CANCEL_PERIOD = 0.1f;
    constexpr S32 SIT_CANCEL_TICKS  = 10;

    // How often the "Always" animations are looked for in the animations the region signals.
    constexpr F32 ALWAYS_CHECK_PERIOD = 3.f;

    // A track's folder name is its name and ":TR:CT999.00:CY:RN" (18 characters), within the 63 an inventory name has.
    constexpr S32 TRACK_NAME_MAX_LENGTH = 45;

    LLUUID xorAnimationID(const LLUUID& id)
    {
        LLUUID result;
        LLXORCipher cipher(ENCRYPTION_MAGIC_ID.mData, UUID_BYTES);
        cipher.encrypt(id.mData, UUID_BYTES, result.mData, UUID_BYTES);
        return result;
    }

    void stopLocally(const LLUUID& animation)
    {
        if (isAgentAvatarValid())
        {
            gAgentAvatarp->LLCharacter::stopMotion(animation);
        }
    }

    // Transient motions: the viewer needs the Linden animation to know the
    // agent's state, so the override plays next to it.
    bool isTransientMotion(const LLUUID& motion)
    {
        return motion == ANIM_AGENT_SIT_GROUND ||
               motion == ANIM_AGENT_SIT_GROUND_CONSTRAINED ||
               motion == ANIM_AGENT_PRE_JUMP ||
               motion == ANIM_AGENT_STANDUP ||
               motion == ANIM_AGENT_LAND ||
               motion == ANIM_AGENT_MEDIUM_LAND;
    }

    // An animation for the list from an inventory item (an animation or a link to one).
    plus::ao::AOAnimation makeAnimation(const LLViewerInventoryItem& item, S32 sort_order)
    {
        plus::ao::AOAnimation anim;
        anim.mName       = item.getName();
        anim.mOriginalID = item.getLinkedUUID();
        anim.mSortOrder  = sort_order;
        // A link's own asset field holds the linked item's id until that item is loaded.
        if (const LLViewerInventoryItem* original = item.getIsLinkType() ? item.getLinkedItem() : &item)
        {
            anim.mAssetID = original->getAssetUUID();
        }
        return anim;
    }

    // The asset a list entry is known by: its own, or its first member's (a group).
    LLUUID stepLeadAsset(const plus::ao::AOAnimation& step)
    {
        if (!step.mIsGroup)
        {
            return step.mAssetID;
        }
        return step.mMembers.empty() ? LLUUID::null : step.mMembers.front().mAssetID;
    }

    // What names an entry in the inventory: its link, or its folder (a group).
    const LLUUID& stepIdentity(const plus::ao::AOAnimation& step)
    {
        return step.mIsGroup ? step.mFolderID : step.mLinkID;
    }

    // A copy of an entry for a new place in the inventory: nothing of the old one's links or folders.
    plus::ao::AOAnimation copyForNewLinks(const plus::ao::AOAnimation& source)
    {
        plus::ao::AOAnimation copy = source;
        copy.mLinkID.setNull();
        copy.mLinkRequest     = 0;
        copy.mFolderID.setNull();
        copy.mFolderRequested = false;
        copy.mFolderDirty     = false;
        if (copy.mIsGroup)
        {
            copy.mKey = plus::ao::newKey();
            for (plus::ao::AOAnimation& member : copy.mMembers)
            {
                member = copyForNewLinks(member);
            }
        }
        return copy;
    }

    bool isSittingOnObject()
    {
        if (!isAgentAvatarValid())
        {
            return false;
        }
        const LLViewerObject* agent_root = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getRoot());
        return agent_root && agent_root->getID() != gAgentID;
    }
}

namespace plus::ao
{
    AOEngine::AOEngine() :
        mStore(*this),
        mLastMotion(ANIM_AGENT_STAND),
        mLastOverriddenMotion(ANIM_AGENT_STAND)
    {
        if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SETTING_ENABLED))
        {
            mUseAOConnection = control->getCommitSignal()->connect(
                [this](LLControlVariable*, const LLSD&, const LLSD&) { onToggleAOControl(); });
        }
        if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SETTING_STANDS))
        {
            mUseAOStandsConnection = control->getCommitSignal()->connect(
                [this](LLControlVariable*, const LLSD&, const LLSD&) { onToggleAOStandsControl(); });
        }
        mRegionChangeConnection = gAgent.addRegionChangedCallback([this]() { onRegionChange(); });
    }

    AOEngine::~AOEngine() = default;

    // ------------------------------------------------------------------------
    // Start-up and settings

    void AOEngine::init()
    {
        const bool do_enable        = gSavedPerAccountSettings.getBOOL(SETTING_ENABLED);
        const bool do_enable_stands = gSavedPerAccountSettings.getBOOL(SETTING_STANDS);
        if (do_enable)
        {
            // enableStands() calls enable(), but mEnabled must be set first.
            mEnabled = true;
            // Enabling the AO always enables stands to start with.
            enableStands(true);
        }
        else
        {
            enableStands(do_enable_stands);
            enable(false);
        }
    }

    void AOEngine::onLoaded()
    {
        mEnabled = gSavedPerAccountSettings.getBOOL(SETTING_ENABLED);
        LL_INFOS("PlusAO") << "Loaded " << mSets.size() << " set(s); current: "
                           << (mCurrentSet ? mCurrentSet->getName() : std::string("none")) << LL_ENDL;
        notifySetsChanged();
        notifyStatusChanged();
        enable(mEnabled);
    }

    void AOEngine::onToggleAOControl()
    {
        enable(gSavedPerAccountSettings.getBOOL(SETTING_ENABLED));
        if (mEnabled)
        {
            // Enabling the AO always enables stands to start with.
            gSavedPerAccountSettings.setBOOL(SETTING_STANDS, true);
        }
    }

    void AOEngine::onToggleAOStandsControl()
    {
        enableStands(gSavedPerAccountSettings.getBOOL(SETTING_STANDS));
    }

    void AOEngine::onQuitting()
    {
        rememberCurrentState();
        mStore.saveNow(); // nothing before the first read (guarded by mLoaded itself)
    }

    void AOEngine::rememberCurrentState()
    {
        // Firestorm's destructor: remember the set and what each state played.
        // Quitting before the AO folder was read (or with no current set) must
        // keep last session's state, not overwrite it with an empty map.
        if (!mStore.isLoaded() || !mCurrentSet)
        {
            return;
        }
        LLSD current_state = LLSD::emptyMap();
        {
            current_state["CurrentSet"] = mCurrentSet->getName();
            LLSD animations = LLSD::emptyMap();
            for (S32 i = 0; i < STATE_COUNT; ++i)
            {
                const AOState& state = mCurrentSet->getState(i);
                if (state.mCurrentAnimation < state.mAnimations.size())
                {
                    const LLUUID asset = stepLeadAsset(state.mAnimations[state.mCurrentAnimation]);
                    if (asset.notNull())
                    {
                        animations[state.getName()] = xorAnimationID(asset);
                    }
                }
            }
            current_state["CurrentStateAnimations"] = animations;
        }
        if (gSavedPerAccountSettings.controlExists(SETTING_CURRENT_STATE))
        {
            gSavedPerAccountSettings.setLLSD(SETTING_CURRENT_STATE, current_state);
        }
    }

    void AOEngine::onIdle()
    {
        mStore.onIdle();

        if (!isAgentAvatarValid())
        {
            return;
        }

        // Firestorm starts the overrider when the agent's avatar is first fully
        // visible (LLVOAvatar::idleUpdateLoadingEffect()).
        if (!mInitialized && gAgentAvatarp->isFullyLoaded())
        {
            mInitialized = true;
            init();
        }

        // Firestorm calls these from LLAgentCamera's mode changes and
        // LLVOAvatar::idleUpdateBelowWater(); both return at once when nothing changed.
        inMouselook(gAgentCamera.cameraMouselook());
        checkBelowWater(gAgentAvatarp->mBelowWater);

        if (mSitCancelTicks > 0 && mSitCancelTimer.getElapsedTimeF32() >= SIT_CANCEL_PERIOD)
        {
            mSitCancelTimer.reset();
            --mSitCancelTicks;
            checkSitCancel();
        }

        if (mCurrentSet && mCurrentSet->timerFired())
        {
            cycleTimeout();
        }

        // Only while a state with tracks or "Always" plays: a few comparisons.
        if (mEnabled && mActiveTrackState)
        {
            tickTracks(*mActiveTrackState);
        }
        if (mEnabled && mActiveAlwaysState)
        {
            tickAlways();
        }
    }

    // ------------------------------------------------------------------------
    // The overrider (Firestorm)

    void AOEngine::stopAllStandVariants()
    {
        LL_DEBUGS("PlusAO") << "stopping all STAND variants." << LL_ENDL;
        for (const LLUUID& id : { ANIM_AGENT_STAND_1, ANIM_AGENT_STAND_2, ANIM_AGENT_STAND_3, ANIM_AGENT_STAND_4 })
        {
            gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
            stopLocally(id);
        }
    }

    void AOEngine::stopAllSitVariants()
    {
        LL_DEBUGS("PlusAO") << "stopping all SIT variants." << LL_ENDL;
        for (const LLUUID& id : { ANIM_AGENT_SIT_FEMALE, ANIM_AGENT_SIT_GENERIC })
        {
            gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
            stopLocally(id);
        }

        // Scripted seats that use ground_sit as animation need special treatment.
        if (isSittingOnObject())
        {
            LL_DEBUGS("PlusAO") << "Not stopping ground sit animations while sitting on a prim." << LL_ENDL;
            return;
        }

        for (const LLUUID& id : { ANIM_AGENT_SIT_GROUND, ANIM_AGENT_SIT_GROUND_CONSTRAINED })
        {
            gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
            stopLocally(id);
        }
    }

    void AOEngine::setLastMotion(const LLUUID& motion)
    {
        if (motion != ANIM_AGENT_TYPE)
        {
            mLastMotion = motion;
        }
    }

    void AOEngine::setLastOverriddenMotion(const LLUUID& motion)
    {
        if (motion != ANIM_AGENT_TYPE)
        {
            mLastOverriddenMotion = motion;
        }
    }

    bool AOEngine::foreignAnimations() const
    {
        // Checking foreign animations only makes sense when smart sit is enabled.
        if (!mCurrentSet || !mCurrentSet->getSmart() || !isAgentAvatarValid())
        {
            return false;
        }

        // The seat the avatar is sitting on.
        const LLViewerObject* agent_root = dynamic_cast<LLViewerObject*>(gAgentAvatarp->getRoot());
        if (!agent_root)
        {
            return false;
        }

        const LLUUID& seat = agent_root->getID();
        if (seat == gAgentID)
        {
            LL_DEBUGS("PlusAO") << "Not checking for foreign animation when not sitting." << LL_ENDL;
            return false;
        }

        for (const auto& [source_id, animation_id] : gAgentAvatarp->mAnimationSources)
        {
            // Skip animations run by the avatar itself.
            if (source_id == gAgentID)
            {
                continue;
            }

            // Proceed if the source is not an attachment.
            const LLViewerObject* source = gObjectList.findObject(source_id);
            if (source && !source->isAttachment())
            {
                // Report back if the animation's root prim is the seat.
                const LLViewerObject* source_root = dynamic_cast<LLViewerObject*>(source->getRoot());
                if (source_root && source_root->getID() == seat)
                {
                    LL_DEBUGS("PlusAO") << "foreign animation " << animation_id << " found on seat." << LL_ENDL;
                    return true;
                }
            }
        }
        return false;
    }

    // Maps a motion to its underwater state, or null if not applicable.
    AOState* AOEngine::mapSwimming(const LLUUID& motion) const
    {
        S32 state;
        if (motion == ANIM_AGENT_HOVER)
        {
            state = Floating;
        }
        else if (motion == ANIM_AGENT_FLY)
        {
            state = SwimmingForward;
        }
        else if (motion == ANIM_AGENT_HOVER_UP)
        {
            state = SwimmingUp;
        }
        else if (motion == ANIM_AGENT_HOVER_DOWN)
        {
            state = SwimmingDown;
        }
        else
        {
            return nullptr;
        }
        return &mCurrentSet->getState(state);
    }

    // Switches between swimming and flying on the way in and out of Linden water.
    void AOEngine::checkBelowWater(bool check_underwater)
    {
        // There was no transition: nothing to do.
        if (mUnderWater == check_underwater)
        {
            return;
        }

        // Only applies to motions that change below water, with animations in their state.
        const AOState* mapped = (mEnabled && mCurrentSet) ? mapSwimming(mLastMotion) : nullptr;
        if (!mapped || mapped->empty())
        {
            mUnderWater = check_underwater;
            return;
        }

        // The animation to stop.
        LLUUID id = override(mLastMotion, false);
        if (id.isNull())
        {
            // No animation in the overrider for this state: the Linden motion.
            id = mLastMotion;
        }
        gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);

        if (!mUnderWater)
        {
            // Remember which animation was stopped on the way below water, to catch
            // its stop request later in the overrider: this keeps the overrider from
            // triggering itself when the region sends the stop for the Linden motion.
            mTransitionId = id;
        }

        mUnderWater = check_underwater;

        // The animation to start.
        id = override(mLastMotion, true);
        if (id.isNull())
        {
            id = mLastMotion;
        }
        gAgent.sendAnimationRequest(id, ANIM_REQUEST_START);
    }

    // The state for the motion, mapping flying to swimming below water.
    AOState* AOEngine::getStateForMotion(const LLUUID& motion) const
    {
        if (!mCurrentSet)
        {
            return nullptr;
        }

        AOState* default_state = mCurrentSet->getStateByRemapID(motion);
        if (!mUnderWater)
        {
            return default_state;
        }

        // Below water the swimming state, if it has animations to play.
        AOState* mapped = mapSwimming(motion);
        if (!mapped || mapped->empty())
        {
            return default_state;
        }
        return mapped;
    }

    void AOEngine::enableStands(bool enable_stands)
    {
        mEnabledStands = enable_stands;
        // Let enable() decide whether animations change, without changing the
        // enabled flag itself.
        enable(mEnabled);
    }

    void AOEngine::enable(bool enable)
    {
        LL_DEBUGS("PlusAO") << "using " << gAnimLibrary.animationName(mLastMotion) << " enable " << enable << LL_ENDL;
        mEnabled = enable;

        if (!mCurrentSet || !isAgentAvatarValid())
        {
            LL_DEBUGS("PlusAO") << "enable(" << enable << ") without animation set loaded." << LL_ENDL;
            return;
        }

        if (mEnabled)
        {
            startAlwaysAnimations();

            AOState* state = getStateForMotion(mLastMotion);
            if (!state || state->empty())
            {
                return;
            }

            LL_DEBUGS("PlusAO") << "Enabling animation state " << state->getName() << LL_ENDL;

            // Do not stop underlying sit animations when re-enabling the AO.
            if (mLastOverriddenMotion != ANIM_AGENT_SIT_GROUND &&
                mLastOverriddenMotion != ANIM_AGENT_SIT_GROUND_CONSTRAINED &&
                mLastOverriddenMotion != ANIM_AGENT_SIT)
            {
                gAgent.sendAnimationRequest(mLastOverriddenMotion, ANIM_REQUEST_STOP);
            }

            const LLUUID animation = override(mLastMotion, true);
            if (animation.isNull())
            {
                return;
            }

            if (mLastMotion == ANIM_AGENT_STAND)
            {
                if (!mEnabledStands)
                {
                    LL_DEBUGS("PlusAO") << "Last motion was a STAND, but disabled for stands, ignoring." << LL_ENDL;
                    return;
                }
                stopAllStandVariants();
            }
            else if (mLastMotion == ANIM_AGENT_WALK)
            {
                for (const LLUUID& id : { ANIM_AGENT_WALK_NEW, ANIM_AGENT_FEMALE_WALK, ANIM_AGENT_FEMALE_WALK_NEW })
                {
                    gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
                    stopLocally(id);
                }
            }
            else if (mLastMotion == ANIM_AGENT_RUN)
            {
                for (const LLUUID& id : { ANIM_AGENT_RUN_NEW, ANIM_AGENT_FEMALE_RUN_NEW })
                {
                    gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
                    stopLocally(id);
                }
            }
            else if (mLastMotion == ANIM_AGENT_SIT)
            {
                stopAllSitVariants();
                gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_START);
            }
            else
            {
                LL_DEBUGS("PlusAO") << "Unhandled last motion id " << gAnimLibrary.animationName(mLastMotion) << LL_ENDL;
            }

            gAgent.sendAnimationRequest(animation, ANIM_REQUEST_START);
            notifyAnimationChanged();

            // Ignore this motion once in the overrider, so stopping the Linden
            // motion does not stop the override animation.
            mIgnoreMotionStopOnce = mLastMotion;
        }
        else
        {
            if (mLastOverriddenMotion == ANIM_AGENT_SIT)
            {
                // Remove the sit cycle cover-up.
                gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_STOP);
            }

            // Stop all overriders, catch leftovers.
            stopActiveTracks();
            stopAlwaysAnimations();
            for (S32 i = 0; i < STATE_COUNT; ++i)
            {
                AOState& state = mCurrentSet->getState(i);
                if (state.isPlaying())
                {
                    LL_DEBUGS("PlusAO") << "Stopping leftover animation from state " << state.getName() << LL_ENDL;
                    stopAnimations(state);
                }
            }

            // Restore the Linden animation if applicable.
            if (mLastOverriddenMotion != ANIM_AGENT_SIT || !foreignAnimations())
            {
                gAgent.sendAnimationRequest(mLastMotion, ANIM_REQUEST_START);
            }

            mCurrentSet->stopTimer();
            notifyAnimationChanged();
        }
    }

    void AOEngine::setStateCycleTimer(const AOState& state)
    {
        LL_DEBUGS("PlusAO") << "Setting cycle timeout for state " << state.getName() << " of " << state.mCycleTime << LL_ENDL;
        if (state.mCycleTime > 0.f)
        {
            mCurrentSet->startTimer(state.mCycleTime);
        }
    }

    LLUUID AOEngine::override(const LLUUID& motion, bool start)
    {
        LL_DEBUGS("PlusAO") << "override(" << gAnimLibrary.animationName(motion) << "," << start << ")" << LL_ENDL;

        if (!mEnabled)
        {
            if (start && mCurrentSet)
            {
                if (const AOState* state = mCurrentSet->getStateByRemapID(motion))
                {
                    setLastMotion(motion);
                    if (!state->empty())
                    {
                        setLastOverriddenMotion(motion);
                    }
                }
            }
            return LLUUID::null;
        }

        if (mSets.empty() || !mCurrentSet || !isAgentAvatarValid())
        {
            return LLUUID::null;
        }

        // Ignore stopping this motion once, so the Linden animation can be stopped
        // without killing the overrider when logging in or re-enabling.
        if (!start && motion == mIgnoreMotionStopOnce)
        {
            LL_DEBUGS("PlusAO") << "Not stop-overriding motion " << gAnimLibrary.animationName(motion) << " within same state." << LL_ENDL;
            mIgnoreMotionStopOnce.setNull();

            // When stopping a sit motion, stop the cycle point cover-up animation.
            if (motion == ANIM_AGENT_SIT)
            {
                gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_STOP);
            }
            return LLUUID::null;
        }

        // The state for the motion, with swimming below water.
        AOState* state = getStateForMotion(motion);
        if (!state)
        {
            LL_DEBUGS("PlusAO") << "No current AO state for motion " << gAnimLibrary.animationName(motion) << LL_ENDL;
            return LLUUID::null;
        }

        // Clean up stray animations as an additional safety measure, unless the
        // motion is typing, which plays at the same time as other motions.
        if (motion != ANIM_AGENT_TYPE)
        {
            constexpr S32 cleanup_states[] =
            {
                Standing, Walking, Running, Sitting, SittingOnGround, Crouching, CrouchWalking,
                Falling, FlyingDown, FlyingUp, Flying, FlyingSlow, Hovering, Jumping,
                TurningRight, TurningLeft, Floating, SwimmingForward, SwimmingUp, SwimmingDown
            };
            for (S32 index : cleanup_states)
            {
                AOState& other = mCurrentSet->getState(index);
                if (&other != state && other.isPlaying())
                {
                    LL_WARNS("PlusAO") << "cleaning up animation in state " << other.getName() << LL_ENDL;
                    // Stop the leftover locally and in the region for everyone.
                    stopAnimations(other);
                }
            }
        }

        LLUUID animation;

        mCurrentSet->stopTimer();
        std::vector<LLUUID> assets;
        if (start)
        {
            // The tracks of the state that ends (not for typing, which plays next to another state).
            if (motion != ANIM_AGENT_TYPE)
            {
                stopActiveTracks();
            }

            setLastMotion(motion);

            // No stand override in mouselook if the set says so.
            if (mCurrentSet->getMouselookStandDisable() && motion == ANIM_AGENT_STAND && mInMouselook)
            {
                return LLUUID::null;
            }

            // Don't override standing and turning if stands are off.
            if (!mEnabledStands && (motion == ANIM_AGENT_STAND || motion == ANIM_AGENT_TURNRIGHT || motion == ANIM_AGENT_TURNLEFT))
            {
                return LLUUID::null;
            }

            // No sit override unless the set wants it.
            if (!mCurrentSet->getSitOverride() && motion == ANIM_AGENT_SIT)
            {
                return LLUUID::null;
            }

            // Scripted seats that use ground_sit as animation need special treatment.
            if ((motion == ANIM_AGENT_SIT_GROUND || motion == ANIM_AGENT_SIT_GROUND_CONSTRAINED) && isSittingOnObject())
            {
                LL_DEBUGS("PlusAO") << "Ground sit animation playing but sitting on a prim - disabling overrider." << LL_ENDL;
                return LLUUID::null;
            }

            if (!state->empty())
            {
                setLastOverriddenMotion(motion);
            }

            // Typing is not remembered as the set's motion.
            if (motion != ANIM_AGENT_TYPE)
            {
                mCurrentSet->setMotion(motion);
            }

            mCurrentSet->pickAnimation(*state, assets);
            animation = assets.empty() ? LLUUID::null : assets.front();

            if (state->isPlaying())
            {
                LL_DEBUGS("PlusAO") << "Previous animation for state " << gAnimLibrary.animationName(motion)
                                    << " was not stopped, but we were asked to start a new one. Killing old animation." << LL_ENDL;
                stopAnimations(*state);
            }

            state->mPlaying = assets;
            LL_DEBUGS("PlusAO") << "overriding " << gAnimLibrary.animationName(motion) << " with " << animation
                                << " in state " << state->getName() << " of set " << mCurrentSet->getName() << LL_ENDL;
            notifyAnimationChanged();

            setStateCycleTimer(*state);

            if (motion == ANIM_AGENT_SIT)
            {
                // ANIM_AGENT_SIT_GENERIC, so there is no overrider loop with ANIM_AGENT_SIT,
                // while still having a base sitting pose to cover up cycle points.
                gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_START);
                if (mCurrentSet->getSmart())
                {
                    mSitCancelTicks = SIT_CANCEL_TICKS;
                    mSitCancelTimer.reset();
                }
            }
            else if (isTransientMotion(motion))
            {
                for (const LLUUID& asset : assets)
                {
                    gAgent.sendAnimationRequest(asset, ANIM_REQUEST_START);
                }
                return LLUUID::null;
            }

            // The other members of a group; the first is returned for the caller to start.
            for (size_t n = 1; n < assets.size(); ++n)
            {
                gAgent.sendAnimationRequest(assets[n], ANIM_REQUEST_START);
            }
            if (motion != ANIM_AGENT_TYPE)
            {
                startStateTracks(*state);
            }
        }
        else
        {
            // A remembered transition motion from or to swimming must not be stopped
            // by the overrider, or the transition would be cancelled right after it started.
            if (motion == mTransitionId)
            {
                // Let the Linden animation system take over.
                mTransitionId.setNull();
                return LLUUID::null;
            }

            // Make sure there is no stray transition id left behind.
            mTransitionId.setNull();

            animation = state->lead();
            stopExtras(*state); // the caller stops the first
            notifyAnimationChanged();

            // Typing: return the stored animation, reset the state timer, remember nothing else.
            if (motion == ANIM_AGENT_TYPE)
            {
                if (const AOState* previous_state = getStateForMotion(mLastMotion))
                {
                    setStateCycleTimer(*previous_state);
                }
                return animation;
            }

            // Stop the underlying Linden motion in case it's still running;
            // frequently happens with sits.
            if (motion == ANIM_AGENT_SIT)
            {
                stopAllSitVariants();
            }

            if (motion != mCurrentSet->getMotion())
            {
                LL_WARNS("PlusAO") << "trying to stop-override motion " << gAnimLibrary.animationName(motion)
                                   << " but the current set has motion " << gAnimLibrary.animationName(mCurrentSet->getMotion()) << LL_ENDL;
                return animation;
            }

            mCurrentSet->setMotion(LLUUID::null);
            stopActiveTracks();

            // Transient animations: make sure our own animation is stopped.
            if (isTransientMotion(motion))
            {
                if (animation.notNull())
                {
                    gAgent.sendAnimationRequest(animation, ANIM_REQUEST_STOP);
                    stopLocally(animation);
                }
                setStateCycleTimer(*state);
                return LLUUID::null;
            }
        }

        return animation;
    }

    void AOEngine::checkSitCancel()
    {
        if (!mCurrentSet || !foreignAnimations())
        {
            return;
        }

        AOState& state = mCurrentSet->getState(Sitting);
        if (state.isPlaying())
        {
            LL_DEBUGS("PlusAO") << "Stopping sit animation due to foreign animations running" << LL_ENDL;
            stopAnimations(state);
            // Remove the cycle point cover-up.
            gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_STOP);
            mSitCancelTicks = 0;
            mCurrentSet->stopTimer();
            // Tracks of a sit that backs off make no sense.
            stopActiveTracks();
            notifyAnimationChanged();
        }
    }

    void AOEngine::cycleTimeout()
    {
        if (mEnabled)
        {
            cycle(CycleAny);
        }
    }

    void AOEngine::cycle(ECycleMode cycle_mode, bool reset_timer)
    {
        if (!mEnabled || !mCurrentSet || !isAgentAvatarValid())
        {
            return;
        }

        // Do not cycle while sitting with sit override off, or standing in
        // mouselook with mouselook stands disabled.
        if (mLastMotion == ANIM_AGENT_SIT && !mCurrentSet->getSitOverride())
        {
            return;
        }
        if (mLastMotion == ANIM_AGENT_STAND && mCurrentSet->getMouselookStandDisable() && mInMouselook)
        {
            return;
        }

        AOState* state = getStateForMotion(mLastMotion);
        if (!state || state->empty())
        {
            return;
        }

        // Timed cycling only when the state cycles; the buttons always work.
        if (!state->mCycle && cycle_mode == CycleAny)
        {
            return;
        }

        std::vector<LLUUID> assets;
        if (cycle_mode == CycleAny)
        {
            mCurrentSet->pickAnimation(*state, assets);
        }
        else
        {
            const U32 count = (U32)state->mAnimations.size();
            if (state->mCurrentAnimation >= count)
            {
                state->mCurrentAnimation = 0;
            }
            if (cycle_mode == CyclePrevious)
            {
                state->mCurrentAnimation = (state->mCurrentAnimation == 0) ? count - 1 : state->mCurrentAnimation - 1;
            }
            else
            {
                state->mCurrentAnimation = (state->mCurrentAnimation + 1) % count;
            }
            stepAssets(state->mAnimations[state->mCurrentAnimation], assets);
        }

        // A group without members, or an animation whose asset is not known yet: nothing to switch to.
        if (assets.empty())
        {
            return;
        }

        // Nothing to do if the animations did not change.
        if (!swapPlaying(*state, assets))
        {
            return;
        }

        if (reset_timer)
        {
            mCurrentSet->resetTimer();
        }
        notifyAnimationChanged();
    }

    void AOEngine::playAnimation(AOSet* set, S32 state_index, size_t index, S32 member)
    {
        if (!mEnabled || !set || !isAgentAvatarValid())
        {
            return;
        }

        // Activate the set if necessary.
        if (set != mCurrentSet)
        {
            selectSet(set);
        }

        // Only the state being played.
        AOState& state = set->getState(state_index);
        if (!isStateActive(state) || index >= state.mAnimations.size())
        {
            return;
        }

        if (state_index != AlwaysPlay)
        {
            if (mLastMotion == ANIM_AGENT_SIT && !mCurrentSet->getSitOverride())
            {
                return;
            }
            if (mLastMotion == ANIM_AGENT_STAND && mCurrentSet->getMouselookStandDisable() && mInMouselook)
            {
                return;
            }
        }

        AOAnimation&        step = state.mAnimations[index];
        std::vector<LLUUID> assets;
        if (member >= 0 && step.mIsGroup)
        {
            if ((size_t)member >= step.mMembers.size())
            {
                return;
            }
            if (const LLUUID& asset = resolveAsset(step.mMembers[member]); asset.notNull())
            {
                assets.push_back(asset);
            }
        }
        else
        {
            stepAssets(step, assets);
        }
        if (assets.empty())
        {
            return;
        }

        state.mCurrentAnimation = (U32)index;
        if (!swapPlaying(state, assets))
        {
            return;
        }

        if (state_index == AlwaysPlay)
        {
            state.mStepTimer.reset();
        }
        else
        {
            mCurrentSet->resetTimer();
        }
        notifyAnimationChanged();
    }

    void AOEngine::playTrackAnimation(AOSet* set, S32 state_index, S32 track_index, size_t index)
    {
        if (!mEnabled || !set || set != mCurrentSet || !isAgentAvatarValid())
        {
            return;
        }

        AOState& state = set->getState(state_index);
        if (track_index < 0 || (size_t)track_index >= state.mTracks.size() || (&state != mActiveTrackState && &state != mActiveAlwaysState))
        {
            return;
        }

        AOTrack& track = state.mTracks[track_index];
        if (index >= track.mAnimations.size())
        {
            return;
        }

        track.mCurrentAnimation = (U32)index;
        swapTrackAnimation(track, resolveAsset(track.mAnimations[index]));
        track.mTimer.reset();
    }

    const AOState* AOEngine::getCurrentState() const
    {
        return getStateForMotion(mLastMotion);
    }

    bool AOEngine::isStateActive(const AOState& state) const
    {
        if (!mEnabled || !mCurrentSet)
        {
            return false;
        }
        if (state.mIndex == AlwaysPlay)
        {
            return &state == mActiveAlwaysState;
        }
        return state.isPlaying() && &state == getStateForMotion(mLastMotion);
    }

    bool AOEngine::isPlaying(const AOSet* set, S32 state_index, size_t index) const
    {
        if (!set || set != mCurrentSet)
        {
            return false;
        }
        const AOState& state = set->getState(state_index);
        return state.isPlaying() && index == state.mCurrentAnimation && index < state.mAnimations.size() &&
               stepLeadAsset(state.mAnimations[index]) == state.lead();
    }

    bool AOEngine::isTrackPlaying(const AOSet* set, S32 state_index, S32 track_index, size_t index) const
    {
        if (!set || set != mCurrentSet || track_index < 0)
        {
            return false;
        }
        const AOState& state = set->getState(state_index);
        if (&state != mActiveTrackState && &state != mActiveAlwaysState)
        {
            return false;
        }
        if ((size_t)track_index >= state.mTracks.size())
        {
            return false;
        }
        const AOTrack& track = state.mTracks[track_index];
        return track.mPlayingID.notNull() && index == track.mCurrentAnimation && index < track.mAnimations.size() &&
               track.mAnimations[index].mAssetID == track.mPlayingID;
    }

    void AOEngine::stopSetAnimations(AOSet& set)
    {
        bool stopped = false;
        if (&set == mCurrentSet)
        {
            stopActiveTracks();
            stopAlwaysAnimations();
        }
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            AOState& state = set.getState(i);
            if (state.isPlaying())
            {
                stopAnimations(state);
                stopped = true;
            }
        }
        if (stopped && mLastOverriddenMotion == ANIM_AGENT_SIT)
        {
            // Remove the sit cycle cover-up.
            gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_STOP);
        }
        set.stopTimer();
        set.setMotion(LLUUID::null);
    }

    void AOEngine::startCurrentSet()
    {
        if (!mEnabled || !isAgentAvatarValid())
        {
            return;
        }

        LL_DEBUGS("PlusAO") << "enabling with motion " << gAnimLibrary.animationName(mLastMotion) << LL_ENDL;
        startAlwaysAnimations();
        const LLUUID animation = mCurrentSet ? override(mLastMotion, true) : LLUUID::null;
        if (animation.notNull())
        {
            gAgent.sendAnimationRequest(animation, ANIM_REQUEST_START);
        }
        else if (!isTransientMotion(mLastMotion) && (mLastOverriddenMotion != ANIM_AGENT_SIT || !foreignAnimations()))
        {
            // Nothing in the new set for this motion: the Linden animation again
            // (transient ones have been started by override() already).
            gAgent.sendAnimationRequest(mLastMotion, ANIM_REQUEST_START);
        }
        notifyAnimationChanged();
    }

    void AOEngine::selectSet(AOSet* set)
    {
        if (set == mCurrentSet)
        {
            return;
        }

        if (mEnabled && mCurrentSet)
        {
            stopSetAnimations(*mCurrentSet);
        }

        mCurrentSet = set;
        startCurrentSet();
    }

    void AOEngine::inMouselook(bool mouselook)
    {
        if (mInMouselook == mouselook)
        {
            return;
        }

        mInMouselook = mouselook;

        if (!mCurrentSet || !mCurrentSet->getMouselookStandDisable() || !mEnabled || mLastMotion != ANIM_AGENT_STAND)
        {
            return;
        }

        if (mouselook)
        {
            AOState& state = mCurrentSet->getState(Standing);
            if (state.isPlaying())
            {
                stopAnimations(state);
                stopActiveTracks();
                notifyAnimationChanged();
            }
            gAgent.sendAnimationRequest(ANIM_AGENT_STAND, ANIM_REQUEST_START);
        }
        else
        {
            stopAllStandVariants();
            const LLUUID animation = override(ANIM_AGENT_STAND, true);
            if (animation.notNull())
            {
                gAgent.sendAnimationRequest(animation, ANIM_REQUEST_START);
            }
        }
    }

    void AOEngine::onRegionChange()
    {
        if (!mEnabled || !mCurrentSet)
        {
            return;
        }

        // Sitting needs special attention.
        if (mLastMotion == ANIM_AGENT_SIT)
        {
            // Nothing to do without sit override, if the last overridden motion was
            // no sit (sit override on, but no sit animations yet), or if no AO
            // animation plays (e.g. smart sit cancel).
            if (!mCurrentSet->getSitOverride() || mLastOverriddenMotion != ANIM_AGENT_SIT ||
                !mCurrentSet->getState(Sitting).isPlaying())
            {
                reassertAlways(false);
                return;
            }
        }

        // The "Always" animations do not depend on the motion (FS: FIRE-36811).
        reassertAlways(false);

        // Restart the current animation on region crossing.
        gAgent.sendAnimationRequest(mLastMotion, ANIM_REQUEST_START);
    }

    // ------------------------------------------------------------------------
    // Sets

    AOEngine::set_ptr_t AOEngine::findSetPtr(const AOSet* set) const
    {
        for (const set_ptr_t& candidate : mSets)
        {
            if (candidate.get() == set)
            {
                return candidate;
            }
        }
        return nullptr;
    }

    AOSet* AOEngine::findSet(U32 serial) const
    {
        for (const set_ptr_t& set : mSets)
        {
            if (set->getSerial() == serial)
            {
                return set.get();
            }
        }
        return nullptr;
    }

    AOSet* AOEngine::findSetByName(const std::string& name) const
    {
        for (const set_ptr_t& set : mSets)
        {
            if (set->getName() == name)
            {
                return set.get();
            }
        }
        return nullptr;
    }

    std::string AOEngine::uniqueSetName(const std::string& name) const
    {
        // The names once, then the free number: with N sets of one name, asking findSetByName()
        // for "name 2", "name 3", ... in turn took N * N steps (and N * N * N for N imports).
        std::unordered_set<std::string> taken;
        taken.reserve(mSets.size());
        for (const set_ptr_t& set : mSets)
        {
            taken.insert(set->getName());
        }
        if (!taken.count(name))
        {
            return name;
        }
        for (S32 n = 2;; ++n) // at most mSets.size() + 1 tries
        {
            const std::string candidate = llformat("%s %d", name.c_str(), n);
            if (!taken.count(candidate))
            {
                return candidate;
            }
        }
    }

    void AOEngine::chooseCurrentSet()
    {
        if (mCurrentSet || mSets.empty())
        {
            return;
        }

        if (mDefaultSet)
        {
            mCurrentSet = mDefaultSet;
            return;
        }

        // No default: the set of the last session, else the first.
        if (gSavedPerAccountSettings.controlExists(SETTING_CURRENT_STATE))
        {
            const LLSD current_state = gSavedPerAccountSettings.getLLSD(SETTING_CURRENT_STATE);
            if (current_state.has("CurrentSet"))
            {
                if (AOSet* set = findSetByName(current_state["CurrentSet"].asString()))
                {
                    mCurrentSet = set;
                    return;
                }
            }
        }
        mCurrentSet = mSets.front().get();
    }

    void AOEngine::restoreSavedAnimations(AOSet& set)
    {
        if (!gSavedPerAccountSettings.controlExists(SETTING_CURRENT_STATE))
        {
            return;
        }

        const LLSD current_state = gSavedPerAccountSettings.getLLSD(SETTING_CURRENT_STATE);
        if (!current_state.has("CurrentSet") || current_state["CurrentSet"].asString() != set.getName() ||
            !current_state.has("CurrentStateAnimations"))
        {
            return;
        }

        const LLSD& animations = current_state["CurrentStateAnimations"];
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            AOState& state = set.getState(i);
            if (!animations.has(state.getName()))
            {
                continue;
            }
            const LLUUID asset = xorAnimationID(animations[state.getName()].asUUID());
            for (size_t index = 0; index < state.mAnimations.size(); ++index)
            {
                if (stepLeadAsset(state.mAnimations[index]) == asset)
                {
                    state.mCurrentAnimation = (U32)index;
                    break;
                }
            }
        }
    }

    void AOEngine::collectInventoryIDs(std::unordered_set<LLUUID>& ids) const
    {
        for (const set_ptr_t& set : mSets)
        {
            if (set->getFolderID().notNull())
            {
                ids.insert(set->getFolderID());
            }
            for (S32 i = 0; i < STATE_COUNT; ++i)
            {
                const AOState& state = set->getState(i);
                if (state.mFolderID.notNull())
                {
                    ids.insert(state.mFolderID);
                }
                std::function<void(const AOAnimation&)> collect = [&ids, &collect](const AOAnimation& anim)
                {
                    if (anim.mLinkID.notNull())
                    {
                        ids.insert(anim.mLinkID);
                    }
                    if (anim.mFolderID.notNull())
                    {
                        ids.insert(anim.mFolderID);
                    }
                    for (const AOAnimation& member : anim.mMembers)
                    {
                        collect(member);
                    }
                };
                for (const AOAnimation& anim : state.mAnimations)
                {
                    collect(anim);
                }
                for (const AOTrack& track : state.mTracks)
                {
                    if (track.mFolderID.notNull())
                    {
                        ids.insert(track.mFolderID);
                    }
                    for (const AOAnimation& anim : track.mAnimations)
                    {
                        collect(anim);
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // What a state plays

    void AOEngine::stopAnimations(AOState& state)
    {
        for (const LLUUID& id : state.mPlaying)
        {
            gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
            stopLocally(id);
        }
        state.mPlaying.clear();
    }

    void AOEngine::stopExtras(AOState& state)
    {
        for (size_t n = 1; n < state.mPlaying.size(); ++n)
        {
            gAgent.sendAnimationRequest(state.mPlaying[n], ANIM_REQUEST_STOP);
            stopLocally(state.mPlaying[n]);
        }
        state.mPlaying.clear();
    }

    bool AOEngine::swapPlaying(AOState& state, const std::vector<LLUUID>& assets)
    {
        if (assets == state.mPlaying)
        {
            return false;
        }

        const std::vector<LLUUID> old = state.mPlaying;
        state.mPlaying                = assets;
        for (const LLUUID& id : assets)
        {
            if (std::find(old.begin(), old.end(), id) == old.end())
            {
                gAgent.sendAnimationRequest(id, ANIM_REQUEST_START);
            }
        }
        for (const LLUUID& id : old)
        {
            if (std::find(assets.begin(), assets.end(), id) == assets.end())
            {
                gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
                stopLocally(id);
            }
        }
        return true;
    }

    void AOEngine::syncPlaying(AOSet& set, AOState& state)
    {
        if (&set != mCurrentSet || !mEnabled)
        {
            return;
        }

        if (state.mIndex == AlwaysPlay)
        {
            if (&state != mActiveAlwaysState)
            {
                startAlwaysAnimations();
                return;
            }
            if (!state.hasContent())
            {
                stopAlwaysAnimations();
                return;
            }
            std::vector<LLUUID> assets;
            if (!state.mAnimations.empty())
            {
                if (state.mCurrentAnimation >= state.mAnimations.size())
                {
                    state.mCurrentAnimation = 0;
                }
                stepAssets(state.mAnimations[state.mCurrentAnimation], assets);
            }
            if (swapPlaying(state, assets))
            {
                state.mStepTimer.reset();
            }
            refreshTracks(state);
            notifyAnimationChanged();
            return;
        }

        if (!state.isPlaying())
        {
            return;
        }

        if (!state.empty())
        {
            // The entry the list has at the current place takes over.
            if (state.mCurrentAnimation >= state.mAnimations.size())
            {
                state.mCurrentAnimation = 0;
            }
            std::vector<LLUUID> assets;
            stepAssets(state.mAnimations[state.mCurrentAnimation], assets);
            // An entry without assets (an emptied group): what plays stays.
            if (!assets.empty() && swapPlaying(state, assets))
            {
                set.resetTimer();
            }
            refreshTracks(state);
        }
        else if (state.mIndex != Typing)
        {
            // Nothing left in the state: the Linden animation again.
            swapPlaying(state, {});
            stopActiveTracks();
            if (state.mIndex == Sitting)
            {
                gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_STOP);
            }
            if (mLastOverriddenMotion != ANIM_AGENT_SIT || !foreignAnimations())
            {
                gAgent.sendAnimationRequest(mLastMotion, ANIM_REQUEST_START);
            }
            set.stopTimer();
        }
        else
        {
            swapPlaying(state, {});
        }
        notifyAnimationChanged();
    }

    // ------------------------------------------------------------------------
    // Tracks and "Always"

    void AOEngine::swapTrackAnimation(AOTrack& track, const LLUUID& animation)
    {
        if (animation == track.mPlayingID)
        {
            return;
        }

        const LLUUID old = track.mPlayingID;
        track.mPlayingID = animation;
        if (animation.notNull())
        {
            gAgent.sendAnimationRequest(animation, ANIM_REQUEST_START);
        }
        if (old.notNull())
        {
            gAgent.sendAnimationRequest(old, ANIM_REQUEST_STOP);
            stopLocally(old);
        }
        notifyAnimationChanged();
    }

    void AOEngine::advanceTrack(AOTrack& track)
    {
        track.mTimer.reset();
        swapTrackAnimation(track, AOSet::pickTrackAnimation(track));
    }

    void AOEngine::tickTracks(AOState& state)
    {
        for (AOTrack& track : state.mTracks)
        {
            if (track.mCycle && track.mCycleTime > 0.f && track.mAnimations.size() > 1 &&
                track.mTimer.getElapsedTimeF32() >= track.mCycleTime)
            {
                advanceTrack(track);
            }
        }
    }

    void AOEngine::refreshTracks(AOState& state)
    {
        if (&state != mActiveTrackState && &state != mActiveAlwaysState)
        {
            return;
        }

        for (AOTrack& track : state.mTracks)
        {
            LLUUID desired;
            if (!track.mAnimations.empty())
            {
                if (track.mCurrentAnimation >= track.mAnimations.size())
                {
                    track.mCurrentAnimation = 0;
                }
                desired = resolveAsset(track.mAnimations[track.mCurrentAnimation]);
            }
            if (desired != track.mPlayingID)
            {
                swapTrackAnimation(track, desired);
                track.mTimer.reset();
            }
        }
    }

    void AOEngine::startStateTracks(AOState& state)
    {
        stopActiveTracks();
        if (!mEnabled)
        {
            return;
        }

        // Also without tracks: tracks added meanwhile start at once (refreshTracks()).
        mActiveTrackState = &state;
        bool started      = false;
        for (AOTrack& track : state.mTracks)
        {
            if (track.mAnimations.empty())
            {
                continue;
            }
            track.mPlayingID = AOSet::pickTrackAnimation(track);
            if (track.mPlayingID.notNull())
            {
                gAgent.sendAnimationRequest(track.mPlayingID, ANIM_REQUEST_START);
                started = true;
            }
            track.mTimer.reset();
        }
        if (started)
        {
            notifyAnimationChanged();
        }
    }

    void AOEngine::stopActiveTracks()
    {
        if (!mActiveTrackState)
        {
            return;
        }

        bool stopped = false;
        for (AOTrack& track : mActiveTrackState->mTracks)
        {
            if (track.mPlayingID.notNull())
            {
                gAgent.sendAnimationRequest(track.mPlayingID, ANIM_REQUEST_STOP);
                stopLocally(track.mPlayingID);
                track.mPlayingID.setNull();
                stopped = true;
            }
        }
        mActiveTrackState = nullptr;
        if (stopped)
        {
            notifyAnimationChanged();
        }
    }

    void AOEngine::startAlwaysAnimations()
    {
        AOState* state = (mEnabled && mCurrentSet) ? &mCurrentSet->getState(AlwaysPlay) : nullptr;
        if (state && state == mActiveAlwaysState)
        {
            return;
        }

        stopAlwaysAnimations();
        if (!state || !state->hasContent() || !isAgentAvatarValid())
        {
            return;
        }

        std::vector<LLUUID> assets;
        mCurrentSet->pickAnimation(*state, assets);
        state->mPlaying = assets;
        for (const LLUUID& id : assets)
        {
            gAgent.sendAnimationRequest(id, ANIM_REQUEST_START);
        }
        for (AOTrack& track : state->mTracks)
        {
            if (track.mAnimations.empty())
            {
                continue;
            }
            track.mPlayingID = AOSet::pickTrackAnimation(track);
            if (track.mPlayingID.notNull())
            {
                gAgent.sendAnimationRequest(track.mPlayingID, ANIM_REQUEST_START);
            }
            track.mTimer.reset();
        }

        mActiveAlwaysState = state;
        state->mStepTimer.reset();
        mAlwaysCheckTimer.reset();
        notifyAnimationChanged();
    }

    void AOEngine::stopAlwaysAnimations()
    {
        if (!mActiveAlwaysState)
        {
            return;
        }

        AOState& state     = *mActiveAlwaysState;
        mActiveAlwaysState = nullptr;
        stopAnimations(state);
        for (AOTrack& track : state.mTracks)
        {
            if (track.mPlayingID.notNull())
            {
                gAgent.sendAnimationRequest(track.mPlayingID, ANIM_REQUEST_STOP);
                stopLocally(track.mPlayingID);
                track.mPlayingID.setNull();
            }
        }
        notifyAnimationChanged();
    }

    void AOEngine::tickAlways()
    {
        AOState& state = *mActiveAlwaysState;
        tickTracks(state);

        if (state.mCycle && state.mCycleTime > 0.f && state.mAnimations.size() > 1 &&
            state.mStepTimer.getElapsedTimeF32() >= state.mCycleTime)
        {
            state.mStepTimer.reset();
            std::vector<LLUUID> assets;
            mCurrentSet->pickAnimation(state, assets);
            if (swapPlaying(state, assets))
            {
                notifyAnimationChanged();
            }
        }

        // The region may have lost them (a teleport, a seat that plays its own animations).
        if (mAlwaysCheckTimer.getElapsedTimeF32() >= ALWAYS_CHECK_PERIOD)
        {
            mAlwaysCheckTimer.reset();
            reassertAlways(true);
        }
    }

    void AOEngine::reassertAlways(bool only_missing)
    {
        if (!mEnabled || !mActiveAlwaysState || !isAgentAvatarValid())
        {
            return;
        }

        auto needs_start = [only_missing](const LLUUID& id)
        {
            if (id.isNull())
            {
                return false;
            }
            return !only_missing || gAgentAvatarp->mSignaledAnimations.find(id) == gAgentAvatarp->mSignaledAnimations.end();
        };
        for (const LLUUID& id : mActiveAlwaysState->mPlaying)
        {
            if (needs_start(id))
            {
                gAgent.sendAnimationRequest(id, ANIM_REQUEST_START);
            }
        }
        for (const AOTrack& track : mActiveAlwaysState->mTracks)
        {
            if (needs_start(track.mPlayingID))
            {
                gAgent.sendAnimationRequest(track.mPlayingID, ANIM_REQUEST_START);
            }
        }
    }

    // ------------------------------------------------------------------------
    // From AOStore

    void AOEngine::afterListEdit(AOSet& set, AOState& state)
    {
        syncPlaying(set, state);
        refreshTracks(state);
        notifyStateChanged(set, state.mIndex);
    }

    void AOEngine::replaceAnimations(AOSet& set, AOState& state, std::vector<AOAnimation>&& animations)
    {
        // Keep the place in the list on the same entry.
        LLUUID current;
        if (state.mCurrentAnimation < state.mAnimations.size())
        {
            current = stepIdentity(state.mAnimations[state.mCurrentAnimation]);
        }

        state.mAnimations       = std::move(animations);
        state.mCurrentAnimation = 0;
        for (size_t index = 0; index < state.mAnimations.size(); ++index)
        {
            if (current.notNull() && stepIdentity(state.mAnimations[index]) == current)
            {
                state.mCurrentAnimation = (U32)index;
                break;
            }
        }

        syncPlaying(set, state);
        notifyStateChanged(set, state.mIndex);
    }

    void AOEngine::replaceTracks(AOSet& set, AOState& state, std::vector<AOTrack>&& tracks)
    {
        // A track that is still there keeps what it plays, its key and its place in the list.
        for (AOTrack& fresh : tracks)
        {
            if (fresh.mFolderID.isNull())
            {
                continue;
            }
            for (AOTrack& old : state.mTracks)
            {
                if (old.mFolderID != fresh.mFolderID)
                {
                    continue;
                }
                fresh.mKey       = old.mKey;
                fresh.mPlayingID = old.mPlayingID;
                fresh.mTimer     = old.mTimer;
                if (old.mCurrentAnimation < old.mAnimations.size())
                {
                    const LLUUID& link = old.mAnimations[old.mCurrentAnimation].mLinkID;
                    for (size_t index = 0; index < fresh.mAnimations.size(); ++index)
                    {
                        if (link.notNull() && fresh.mAnimations[index].mLinkID == link)
                        {
                            fresh.mCurrentAnimation = (U32)index;
                            break;
                        }
                    }
                }
                old.mPlayingID.setNull(); // taken over
                break;
            }
        }
        // The tracks that went stop.
        for (AOTrack& old : state.mTracks)
        {
            if (old.mPlayingID.notNull())
            {
                gAgent.sendAnimationRequest(old.mPlayingID, ANIM_REQUEST_STOP);
                stopLocally(old.mPlayingID);
            }
        }

        state.mTracks = std::move(tracks);
        refreshTracks(state);
        if (state.mIndex == AlwaysPlay)
        {
            syncPlaying(set, state);
        }
        notifyStateChanged(set, state.mIndex);
    }

    void AOEngine::replaceSets(set_list_t&& sets, AOSet* default_set, bool start)
    {
        const bool had_current  = mCurrentSet != nullptr;
        const bool current_kept = had_current && std::any_of(sets.begin(), sets.end(),
                                                             [this](const set_ptr_t& set) { return set.get() == mCurrentSet; });
        if (had_current && !current_kept)
        {
            stopSetAnimations(*mCurrentSet);
            mCurrentSet = nullptr;
        }

        mSets       = std::move(sets);
        mDefaultSet = default_set;

        if (!mCurrentSet)
        {
            chooseCurrentSet();
            // With no set before and none now, the Linden motions play already.
            if (start && (mCurrentSet || had_current))
            {
                startCurrentSet();
            }
        }
        notifySetsChanged();
    }

    void AOEngine::dropSet(const AOSet* set)
    {
        set_list_t sets;
        for (const set_ptr_t& candidate : mSets)
        {
            if (candidate.get() != set)
            {
                sets.push_back(candidate);
            }
        }
        replaceSets(std::move(sets), mDefaultSet == set ? nullptr : mDefaultSet);
    }

    // ------------------------------------------------------------------------
    // Editing

    AOSet* AOEngine::addSet(const std::string& name)
    {
        if (!isLoaded() || !AOSet::isValidName(name) || findSetByName(name))
        {
            return nullptr;
        }

        LocalTiming timing("add set \"" + name + "\"");
        set_ptr_t set = std::make_shared<AOSet>(name);
        mSets.push_back(set);
        mStore.flush(set);

        if (!mCurrentSet)
        {
            mCurrentSet = set.get();
        }
        notifySetsChanged();
        return set.get();
    }

    AOSet* AOEngine::addSet(const NewSet& description)
    {
        if (!isLoaded() || !AOSet::isValidName(description.mName) || findSetByName(description.mName))
        {
            return nullptr;
        }

        LocalTiming timing("create set \"" + description.mName + "\"");
        set_ptr_t set = std::make_shared<AOSet>(description.mName);
        set->setSitOverride(description.mSitOverride);
        set->setSmart(description.mSmart);
        set->setMouselookStandDisable(description.mMouselookStandDisable);

        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            const NewSet::State& source = description.mStates[i];
            AOState&             state  = set->getState(i);
            state.mCycle     = source.mCycle;
            state.mRandom    = source.mRandom;
            state.mCycleTime = source.mCycleTime;
            for (const NewSet::Step& step : source.mSteps)
            {
                std::vector<AOAnimation> members;
                for (const LLPointer<LLViewerInventoryItem>& item : step.mItems)
                {
                    if (item.notNull() && item->getInventoryType() == LLInventoryType::IT_ANIMATION)
                    {
                        members.push_back(makeAnimation(*item, (S32)members.size()));
                    }
                }
                if (members.empty())
                {
                    continue;
                }
                if (!step.mIsGroup || members.size() == 1)
                {
                    for (AOAnimation& member : members)
                    {
                        member.mSortOrder = state.nextSortOrder();
                        state.mAnimations.push_back(std::move(member));
                    }
                    continue;
                }
                AOAnimation group;
                group.mIsGroup   = true;
                group.mKey       = newKey();
                group.mName      = isValidFolderPart(step.mName) ? step.mName : state.nextGroupName();
                group.mSortOrder = state.nextSortOrder();
                group.mMembers   = std::move(members);
                state.mAnimations.push_back(std::move(group));
            }
            for (const NewSet::Track& source_track : source.mTracks)
            {
                AOTrack track;
                track.mName = isValidFolderPart(source_track.mName, TRACK_NAME_MAX_LENGTH) ? source_track.mName : state.nextTrackName();
                if (std::any_of(state.mTracks.begin(), state.mTracks.end(), [&track](const AOTrack& other) { return other.mName == track.mName; }))
                {
                    track.mName = state.nextTrackName();
                }
                track.mCycle     = source_track.mCycle;
                track.mRandom    = source_track.mRandom;
                track.mCycleTime = AOState::clampCycleTime(source_track.mCycleTime);
                for (const LLPointer<LLViewerInventoryItem>& item : source_track.mItems)
                {
                    if (item.notNull() && item->getInventoryType() == LLInventoryType::IT_ANIMATION)
                    {
                        track.mAnimations.push_back(makeAnimation(*item, (S32)track.mAnimations.size()));
                    }
                }
                state.mTracks.push_back(std::move(track));
            }
            // In the order a reading gives (renamed tracks may not be): the next reading keeps them (research/275 L098)
            std::stable_sort(state.mTracks.begin(), state.mTracks.end(), trackBefore);
        }

        mSets.push_back(set);
        mStore.flush(set);

        if (!mCurrentSet)
        {
            mCurrentSet = set.get();
            startCurrentSet();
        }
        notifySetsChanged();
        return set.get();
    }

    void AOEngine::removeSet(AOSet* set)
    {
        set_ptr_t ptr = findSetPtr(set);
        if (!ptr)
        {
            return;
        }

        LocalTiming timing("remove set \"" + set->getName() + "\"");
        const LLUUID folder = set->getFolderID();
        // A protected folder, or one holding one (a protected state folder), or one an RLV
        // folder lock keeps, stays, and with it the set: it would come back at the next login
        // (plus/invguard: the guard tells the user).
        if (folder.notNull() && plus::invguard::refusesRemoval(folder))
        {
            return;
        }
        // A folder holding other items than animation links stays this time, and the set
        // with it (removeFolder() moves them out and tells the user to remove it again).
        if (folder.notNull() && !mStore.removeFolder(folder, "remove set \"" + ptr->getName() + "\""))
        {
            return;
        }
        dropSet(set);
        // A folder still being created is removed when it arrives (the set is gone then).
    }

    bool AOEngine::renameSet(AOSet* set, const std::string& name)
    {
        if (!set || !AOSet::isValidName(name))
        {
            return false;
        }
        if (name == set->getName())
        {
            return true;
        }
        if (findSetByName(name))
        {
            return false;
        }

        set->setName(name);
        set->setDirty(true);
        mStore.scheduleSave();
        notifySetsChanged();
        return true;
    }

    void AOEngine::setDefaultSet(AOSet* set)
    {
        if (set == mDefaultSet)
        {
            return;
        }
        // Only the old and the new default change their folder names.
        if (mDefaultSet)
        {
            mDefaultSet->setDirty(true);
        }
        mDefaultSet = set;
        if (mDefaultSet)
        {
            mDefaultSet->setDirty(true);
        }
        mStore.scheduleSave();
        notifySetsChanged();
    }

    void AOEngine::setOverrideSits(AOSet* set, bool override_sit)
    {
        if (!set)
        {
            return;
        }
        set->setSitOverride(override_sit);
        set->setDirty(true);
        mStore.scheduleSave();
        notifySetsChanged();

        if (mCurrentSet != set || mLastMotion != ANIM_AGENT_SIT || !mEnabled)
        {
            return;
        }

        if (override_sit)
        {
            stopAllSitVariants();
            gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_START);
        }
        else
        {
            // Remove the sit cycle cover-up.
            gAgent.sendAnimationRequest(ANIM_AGENT_SIT_GENERIC, ANIM_REQUEST_STOP);

            AOState& state = mCurrentSet->getState(Sitting);
            if (state.isPlaying())
            {
                stopAnimations(state);
                stopActiveTracks();
                notifyAnimationChanged();
            }

            if (!foreignAnimations())
            {
                gAgent.sendAnimationRequest(ANIM_AGENT_SIT, ANIM_REQUEST_START);
            }
        }
    }

    void AOEngine::setSmart(AOSet* set, bool smart)
    {
        if (!set)
        {
            return;
        }
        set->setSmart(smart);
        set->setDirty(true);
        mStore.scheduleSave();
        notifySetsChanged();

        // Restart the sit cancel checks to fix sit overrides when the object we
        // are sitting on is playing its own animation.
        if (mEnabled && smart && isSittingOnObject())
        {
            mSitCancelTicks = SIT_CANCEL_TICKS;
            mSitCancelTimer.reset();
        }
    }

    void AOEngine::setDisableMouselookStands(AOSet* set, bool disabled)
    {
        if (!set)
        {
            return;
        }
        set->setMouselookStandDisable(disabled);
        set->setDirty(true);
        mStore.scheduleSave();
        notifySetsChanged();

        if (mCurrentSet != set || !mEnabled)
        {
            return;
        }

        // Make sure an update happens if needed.
        mInMouselook = !gAgentCamera.cameraMouselook();
        inMouselook(!mInMouselook);
    }

    void AOEngine::setCycle(AOSet* set, S32 state_index, bool cycle)
    {
        if (set)
        {
            AOState& state = set->getState(state_index);
            state.mCycle   = cycle;
            state.mDirty   = true;
            mStore.scheduleSave();
            if (!cycle)
            {
                // Without cycling the state plays its first entry, whatever the cycle had reached (FS).
                state.mCurrentAnimation = 0;
                syncPlaying(*set, state);
            }
        }
    }

    void AOEngine::setRandomize(AOSet* set, S32 state, bool randomize)
    {
        if (set)
        {
            set->getState(state).mRandom = randomize;
            set->getState(state).mDirty  = true;
            mStore.scheduleSave();
        }
    }

    void AOEngine::setCycleTime(AOSet* set, S32 state_index, F32 time)
    {
        if (set)
        {
            AOState& state  = set->getState(state_index);
            state.mCycleTime = time;
            state.mDirty     = true;
            mStore.scheduleSave();
            if (state_index == AlwaysPlay)
            {
                state.mStepTimer.reset();
            }
        }
    }

    // static
    std::vector<AOAnimation>* AOEngine::listFor(AOState& state, ListRef list)
    {
        if (list.mTrack >= 0)
        {
            return (size_t)list.mTrack < state.mTracks.size() ? &state.mTracks[list.mTrack].mAnimations : nullptr;
        }
        if (list.mGroup >= 0)
        {
            return ((size_t)list.mGroup < state.mAnimations.size() && state.mAnimations[list.mGroup].mIsGroup)
                       ? &state.mAnimations[list.mGroup].mMembers
                       : nullptr;
        }
        return &state.mAnimations;
    }

    void AOEngine::addAnimations(AOSet* set, S32 state_index, const std::vector<LLPointer<LLViewerInventoryItem>>& items, ListRef list)
    {
        set_ptr_t ptr = findSetPtr(set);
        if (!ptr || items.empty())
        {
            return;
        }

        AOState& state = set->getState(state_index);
        std::vector<AOAnimation>* entries = listFor(state, list);
        if (!entries)
        {
            return;
        }
        LocalTiming timing(llformat("add %d animation(s) to %s", (S32)items.size(), state.getName().c_str()));

        const bool was_empty = state.empty();
        S32        order     = 0;
        for (const AOAnimation& anim : *entries)
        {
            order = llmax(order, anim.mSortOrder + 1);
        }
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item.notNull() && item->getInventoryType() == LLInventoryType::IT_ANIMATION)
            {
                entries->push_back(makeAnimation(*item, order++));
            }
        }

        mStore.flush(ptr);

        if (list.mTrack >= 0 || list.mGroup >= 0)
        {
            afterListEdit(*set, state);
            return;
        }
        notifyStateChanged(*set, state_index);

        if (state_index == AlwaysPlay)
        {
            syncPlaying(*set, state);
        }
        // The state being played had nothing to play: start it now.
        else if (was_empty && !state.empty() && mEnabled && set == mCurrentSet && getStateForMotion(mLastMotion) == &state)
        {
            enable(true);
        }
    }

    void AOEngine::removeAnimations(AOSet* set, S32 state_index, std::vector<size_t> indices, ListRef list)
    {
        if (!findSetPtr(set) || indices.empty())
        {
            return;
        }

        AOState& state = set->getState(state_index);
        std::vector<AOAnimation>* entries = listFor(state, list);
        if (!entries)
        {
            return;
        }
        LocalTiming timing(llformat("remove %d animation(s) from %s", (S32)indices.size(), state.getName().c_str()));

        // The place of the cycle in the list, if the list has one.
        U32* current = nullptr;
        if (list.mTrack >= 0)
        {
            current = &state.mTracks[list.mTrack].mCurrentAnimation;
        }
        else if (list.mGroup < 0)
        {
            current = &state.mCurrentAnimation;
        }

        // From the back, so the indices stay valid.
        std::sort(indices.begin(), indices.end(), std::greater<size_t>());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        std::vector<LLUUID> links;
        for (size_t index : indices)
        {
            if (index >= entries->size())
            {
                continue;
            }
            const AOAnimation& entry = (*entries)[index];
            if (entry.mIsGroup)
            {
                // A protected folder, or one holding other items, stays and with it the group.
                if (entry.mFolderID.notNull() && !mStore.removeFolder(entry.mFolderID, "remove animations from " + state.getName()))
                {
                    continue;
                }
            }
            else if (entry.mLinkID.notNull())
            {
                links.push_back(entry.mLinkID);
            }
            // A link or folder still being made is removed when it arrives (nothing waits for it then).
            entries->erase(entries->begin() + index);
            if (current && index < *current)
            {
                --*current;
            }
        }
        if (current && *current >= entries->size())
        {
            *current = 0;
        }

        const std::string label = "remove animations from " + state.getName();
        // One request for the folder instead of one per link. The state's
        // flags go with it, as in Firestorm; they are written again with a new folder.
        // A protected or RLV-locked folder stays (plus/invguard): then the links go one by one
        // and the state keeps its folder, so animations added later go into it. Asked
        // without a message first: the animations do go, only the folder stays.
        if (!state.hasContent() && state.mFolderID.notNull() && !plus::invguard::blocksRemoval(state.mFolderID)
            && mStore.removeFolder(state.mFolderID, label))
        {
            state.mFolderID.setNull();
            state.mFolderRequested = false;
        }
        else
        {
            mStore.removeLinks(links, label);
        }

        if (list.mGroup >= 0)
        {
            normalizeGroup(*set, state, (size_t)list.mGroup);
        }
        afterListEdit(*set, state);
    }

    bool AOEngine::moveAnimation(AOSet* set, S32 state_index, size_t index, bool up, ListRef list)
    {
        if (!findSetPtr(set))
        {
            return false;
        }

        AOState& state = set->getState(state_index);
        std::vector<AOAnimation>* entries = listFor(state, list);
        if (!entries || index >= entries->size() || (up && index == 0) || (!up && index + 1 >= entries->size()))
        {
            return false;
        }

        LocalTiming timing("move an animation in " + state.getName());
        const size_t other = up ? index - 1 : index + 1;
        const size_t first = llmin(index, other);
        AOAnimation& a     = (*entries)[first];
        AOAnimation& b     = (*entries)[first + 1];

        // A link's number is its description; a group's is in its folder name.
        std::vector<std::pair<LLUUID, S32>> orders;
        bool                                save_names = false;
        auto note_order = [&orders, &save_names](AOAnimation& anim)
        {
            if (anim.mIsGroup)
            {
                anim.mFolderDirty = true;
                save_names        = true;
            }
            else if (anim.mLinkID.notNull())
            {
                orders.emplace_back(anim.mLinkID, anim.mSortOrder);
            }
        };

        if (a.mSortOrder < b.mSortOrder)
        {
            // The usual case: the two trade places, two entries change.
            std::swap(a.mSortOrder, b.mSortOrder);
            std::swap(a, b);
            note_order(a);
            note_order(b);
        }
        else
        {
            // Equal or unordered numbers (written by something else): number them all.
            std::swap(a, b);
            for (size_t i = 0; i < entries->size(); ++i)
            {
                AOAnimation& anim = (*entries)[i];
                if (anim.mSortOrder != (S32)i)
                {
                    anim.mSortOrder = (S32)i;
                    note_order(anim);
                }
            }
        }

        U32* current = nullptr;
        if (list.mTrack >= 0)
        {
            current = &state.mTracks[list.mTrack].mCurrentAnimation;
        }
        else if (list.mGroup < 0)
        {
            current = &state.mCurrentAnimation;
        }
        if (current)
        {
            if (*current == index)
            {
                *current = (U32)other;
            }
            else if (*current == other)
            {
                *current = (U32)index;
            }
        }

        // Links still being made get their number when they arrive (AOStore::onLinkCreated()).
        mStore.writeSortOrders(orders, "move an animation in " + state.getName());
        if (save_names)
        {
            mStore.scheduleSave();
        }
        notifyStateChanged(*set, state_index);
        return true;
    }

    void AOEngine::renumberMain(AOSet& set, AOState& state)
    {
        std::vector<std::pair<LLUUID, S32>> orders;
        bool                                save_names = false;
        for (size_t i = 0; i < state.mAnimations.size(); ++i)
        {
            AOAnimation& anim = state.mAnimations[i];
            if (anim.mSortOrder == (S32)i)
            {
                continue;
            }
            anim.mSortOrder = (S32)i;
            if (anim.mIsGroup)
            {
                anim.mFolderDirty = true;
                save_names        = true;
            }
            else if (anim.mLinkID.notNull())
            {
                orders.emplace_back(anim.mLinkID, anim.mSortOrder);
            }
        }
        mStore.writeSortOrders(orders, "number the animations of " + state.getName());
        if (save_names)
        {
            mStore.scheduleSave();
        }
    }

    void AOEngine::normalizeGroup(AOSet& set, AOState& state, size_t group_index)
    {
        if (group_index >= state.mAnimations.size() || !state.mAnimations[group_index].mIsGroup)
        {
            return;
        }

        AOAnimation& group = state.mAnimations[group_index];
        if (group.mMembers.size() >= 2)
        {
            return;
        }

        // The folder goes, and with it the link of the last member (which gets a new one in the state folder).
        if (group.mFolderID.notNull())
        {
            if (plus::invguard::refusesRemoval(group.mFolderID) || !mStore.removeFolder(group.mFolderID, "remove an emptied group"))
            {
                return;
            }
        }

        if (group.mMembers.empty())
        {
            state.mAnimations.erase(state.mAnimations.begin() + group_index);
            if (state.mCurrentAnimation > group_index)
            {
                --state.mCurrentAnimation;
            }
            if (state.mCurrentAnimation >= state.mAnimations.size())
            {
                state.mCurrentAnimation = 0;
            }
        }
        else
        {
            AOAnimation single = copyForNewLinks(group.mMembers.front());
            single.mSortOrder  = group.mSortOrder;
            state.mAnimations[group_index] = std::move(single);
        }
        mStore.flush(findSetPtr(&set));
    }

    void AOEngine::createGroup(AOSet* set, S32 state_index, size_t target_index, const std::vector<LLPointer<LLViewerInventoryItem>>& items)
    {
        set_ptr_t ptr = findSetPtr(set);
        if (!ptr || items.empty())
        {
            return;
        }

        AOState& state = set->getState(state_index);
        if (target_index >= state.mAnimations.size() || state.mAnimations[target_index].mIsGroup)
        {
            return;
        }
        LocalTiming timing("group animations in " + state.getName());

        AOAnimation group;
        group.mIsGroup   = true;
        group.mKey       = newKey();
        group.mName      = state.nextGroupName();
        group.mSortOrder = state.mAnimations[target_index].mSortOrder;

        AOAnimation first = copyForNewLinks(state.mAnimations[target_index]);
        first.mSortOrder  = 0;
        group.mMembers.push_back(std::move(first));
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item.notNull() && item->getInventoryType() == LLInventoryType::IT_ANIMATION)
            {
                group.mMembers.push_back(makeAnimation(*item, (S32)group.mMembers.size()));
            }
        }
        if (group.mMembers.size() < 2)
        {
            return;
        }

        const LLUUID old_link = state.mAnimations[target_index].mLinkID;
        state.mAnimations[target_index] = std::move(group);
        // The old link is replaced by the member's new one in the group's folder.
        if (old_link.notNull())
        {
            mStore.removeLinks({ old_link }, "group animations in " + state.getName());
        }
        mStore.flush(ptr);
        afterListEdit(*set, state);
    }

    bool AOEngine::createGroupFromFolder(AOSet* set, S32 state_index, const LLUUID& folder)
    {
        set_ptr_t ptr = findSetPtr(set);
        if (!ptr)
        {
            return false;
        }

        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        std::vector<LLPointer<LLViewerInventoryItem>> animations;
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (item->getInventoryType() == LLInventoryType::IT_ANIMATION)
                {
                    animations.push_back(item);
                }
            }
        }
        if (animations.empty())
        {
            return false;
        }
        std::stable_sort(animations.begin(), animations.end(),
                         [](const LLPointer<LLViewerInventoryItem>& a, const LLPointer<LLViewerInventoryItem>& b)
                         { return LLStringUtil::compareInsensitive(a->getName(), b->getName()) < 0; });

        AOState& state = set->getState(state_index);
        LocalTiming timing("group a folder in " + state.getName());

        std::string name;
        if (const LLViewerInventoryCategory* category = gInventory.getCategory(folder))
        {
            name = category->getName();
            LLStringUtil::trim(name);
        }

        if (animations.size() == 1)
        {
            addAnimations(set, state_index, animations);
            return true;
        }

        AOAnimation group;
        group.mIsGroup   = true;
        group.mKey       = newKey();
        group.mName      = isValidFolderPart(name) ? name : state.nextGroupName();
        group.mSortOrder = state.nextSortOrder();
        for (const LLPointer<LLViewerInventoryItem>& item : animations)
        {
            group.mMembers.push_back(makeAnimation(*item, (S32)group.mMembers.size()));
        }
        const bool was_empty = state.empty();
        state.mAnimations.push_back(std::move(group));

        mStore.flush(ptr);
        notifyStateChanged(*set, state_index);
        if (state_index == AlwaysPlay)
        {
            syncPlaying(*set, state);
        }
        else if (was_empty && mEnabled && set == mCurrentSet && getStateForMotion(mLastMotion) == &state)
        {
            enable(true);
        }
        return true;
    }

    void AOEngine::extractMember(AOSet* set, S32 state_index, size_t group_index, size_t member_index)
    {
        set_ptr_t ptr = findSetPtr(set);
        if (!ptr)
        {
            return;
        }

        AOState& state = set->getState(state_index);
        if (group_index >= state.mAnimations.size() || !state.mAnimations[group_index].mIsGroup ||
            member_index >= state.mAnimations[group_index].mMembers.size())
        {
            return;
        }
        LocalTiming timing("move an animation out of a group in " + state.getName());

        AOAnimation& group  = state.mAnimations[group_index];
        AOAnimation  single = copyForNewLinks(group.mMembers[member_index]);
        const LLUUID old_link = group.mMembers[member_index].mLinkID;
        group.mMembers.erase(group.mMembers.begin() + member_index);

        state.mAnimations.insert(state.mAnimations.begin() + group_index + 1, std::move(single));
        if (state.mCurrentAnimation > group_index)
        {
            ++state.mCurrentAnimation;
        }
        if (old_link.notNull())
        {
            mStore.removeLinks({ old_link }, "move an animation out of a group");
        }
        normalizeGroup(*set, state, group_index);
        renumberMain(*set, state);
        mStore.flush(ptr);
        afterListEdit(*set, state);
    }

    bool AOEngine::renameGroup(AOSet* set, S32 state_index, size_t group_index, const std::string& name)
    {
        if (!findSetPtr(set) || !isValidFolderPart(name))
        {
            return false;
        }

        AOState& state = set->getState(state_index);
        if (group_index >= state.mAnimations.size() || !state.mAnimations[group_index].mIsGroup)
        {
            return false;
        }

        AOAnimation& group = state.mAnimations[group_index];
        if (group.mName != name)
        {
            group.mName        = name;
            group.mFolderDirty = true;
            mStore.scheduleSave();
            notifyStateChanged(*set, state_index);
        }
        return true;
    }

    S32 AOEngine::addTrack(AOSet* set, S32 state_index)
    {
        set_ptr_t ptr = findSetPtr(set);
        if (!ptr || !isLoaded())
        {
            return -1;
        }

        AOState& state = set->getState(state_index);
        LocalTiming timing("add a track to " + state.getName());
        AOTrack track;
        track.mName = state.nextTrackName();
        // At its place in the order a reading gives (a freed number goes before the later ones: "Track 2" between 1
        // and 3), so the next reading keeps the tracks where they are (trackBefore(), research/275 L098)
        const auto at = std::upper_bound(state.mTracks.begin(), state.mTracks.end(), track, trackBefore);
        const S32  index = (S32)(at - state.mTracks.begin());
        state.mTracks.insert(at, std::move(track));

        mStore.flush(ptr);
        notifyStateChanged(*set, state_index);
        return index;
    }

    void AOEngine::removeTrack(AOSet* set, S32 state_index, S32 track_index)
    {
        if (!findSetPtr(set))
        {
            return;
        }

        AOState& state = set->getState(state_index);
        if (track_index < 0 || (size_t)track_index >= state.mTracks.size())
        {
            return;
        }
        LocalTiming timing("remove a track from " + state.getName());

        const LLUUID folder = state.mTracks[track_index].mFolderID;
        // A protected folder, or one holding other items, stays and with it the track.
        if (folder.notNull() && !mStore.removeFolder(folder, "remove a track from " + state.getName()))
        {
            return;
        }

        swapTrackAnimation(state.mTracks[track_index], LLUUID::null);
        state.mTracks.erase(state.mTracks.begin() + track_index);

        const std::string label = "remove a track from " + state.getName();
        if (!state.hasContent() && state.mFolderID.notNull() && !plus::invguard::blocksRemoval(state.mFolderID)
            && mStore.removeFolder(state.mFolderID, label))
        {
            state.mFolderID.setNull();
            state.mFolderRequested = false;
        }
        afterListEdit(*set, state);
    }

    void AOEngine::setTrackCycle(AOSet* set, S32 state_index, S32 track_index, bool cycle)
    {
        if (!set || track_index < 0 || (size_t)track_index >= set->getState(state_index).mTracks.size())
        {
            return;
        }
        AOState& state = set->getState(state_index);
        AOTrack& track = state.mTracks[track_index];
        track.mCycle   = cycle;
        track.mDirty   = true;
        mStore.scheduleSave();
        if (!cycle)
        {
            // Back to the first animation of the track (as the main list does).
            track.mCurrentAnimation = 0;
            refreshTracks(state);
        }
    }

    void AOEngine::setTrackRandomize(AOSet* set, S32 state_index, S32 track_index, bool randomize)
    {
        if (!set || track_index < 0 || (size_t)track_index >= set->getState(state_index).mTracks.size())
        {
            return;
        }
        AOTrack& track = set->getState(state_index).mTracks[track_index];
        track.mRandom  = randomize;
        track.mDirty   = true;
        mStore.scheduleSave();
    }

    void AOEngine::setTrackCycleTime(AOSet* set, S32 state_index, S32 track_index, F32 time)
    {
        if (!set || track_index < 0 || (size_t)track_index >= set->getState(state_index).mTracks.size())
        {
            return;
        }
        AOTrack& track  = set->getState(state_index).mTracks[track_index];
        track.mCycleTime = time;
        track.mDirty     = true;
        track.mTimer.reset();
        mStore.scheduleSave();
    }

    AOSet* AOEngine::cloneSet(const AOSet* source, const std::string& name)
    {
        if (!isLoaded() || !source || !AOSet::isValidName(name) || findSetByName(name))
        {
            return nullptr;
        }

        LocalTiming timing("clone set \"" + source->getName() + "\"");
        set_ptr_t set = std::make_shared<AOSet>(name);
        set->setSitOverride(source->getSitOverride());
        set->setSmart(source->getSmart());
        set->setMouselookStandDisable(source->getMouselookStandDisable());

        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            const AOState& from = source->getState(i);
            AOState&       to   = set->getState(i);
            to.mCycle     = from.mCycle;
            to.mRandom    = from.mRandom;
            to.mCycleTime = from.mCycleTime;
            for (const AOAnimation& anim : from.mAnimations)
            {
                to.mAnimations.push_back(copyForNewLinks(anim));
            }
            for (const AOTrack& track : from.mTracks)
            {
                AOTrack copy;
                copy.mName      = track.mName;
                copy.mCycle     = track.mCycle;
                copy.mRandom    = track.mRandom;
                copy.mCycleTime = track.mCycleTime;
                for (const AOAnimation& anim : track.mAnimations)
                {
                    copy.mAnimations.push_back(copyForNewLinks(anim));
                }
                to.mTracks.push_back(std::move(copy));
            }
        }

        mSets.push_back(set);
        mStore.flush(set);
        notifySetsChanged();
        return set.get();
    }

    void AOEngine::importFromFirestorm()
    {
        mStore.importFromFirestorm();
        notifyStatusChanged();
    }

    void AOEngine::reload()
    {
        mStore.reload();
        notifyStatusChanged();
    }
}
