/**
 * @file aoset.cpp
 * @brief Animation Overrider: a set of animations, its states and the
 *        inventory folder names they are stored under.
 *
 * Ported from Firestorm's aoset.cpp (see SOURCE.md).
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

#include "aoset.h"

#include "llanimationstates.h"
#include "lldbstrings.h"
#include "llinventorymodel.h"
#include "llrand.h"
#include "llstring.h"
#include "llviewerinventory.h"

#include <algorithm>
#include <cmath>

namespace
{
    struct StateDef
    {
        std::string              mName;       // the folder name
        std::vector<std::string> mAllNames;   // mName and the ZHAO names
        LLUUID                   mRemapID;    // the Linden motion it overrides
    };

    // Built on first use: the ANIM_AGENT_* ids are initialized in another library.
    const std::array<StateDef, plus::ao::STATE_COUNT>& stateDefs()
    {
        static const std::array<StateDef, plus::ao::STATE_COUNT> sDefs = []
        {
            // Folder name first, alternate (ZHAO) names following, separated by |.
            // Keep number and order in sync with plus::ao::EState.
            const char* names[plus::ao::STATE_COUNT] =
            {
                "Standing|Stand.1|Stand.2|Stand.3",
                "Walking|Walk.N",
                "Running",
                "Sitting|Sit.N",
                "Sitting On Ground|Sit.G",
                "Crouching|Crouch",
                "Crouch Walking|Walk.C",
                "Landing|Land.N",
                "Soft Landing",
                "Standing Up|Stand.U",
                "Falling",
                "Flying Down|Hover.D",
                "Flying Up|Hover.U",
                "Flying|Fly.N",
                "Flying Slow",
                "Hovering|Hover.N",
                "Jumping|Jump.N",
                "Pre Jumping|Jump.P",
                "Turning Right|Turn.R",
                "Turning Left|Turn.L",
                "Typing",
                "Floating|Swim.H",
                "Swimming Forward|Swim.N",
                "Swimming Up|Swim.U",
                "Swimming Down|Swim.D",
                "Always"
            };

            // The swimming states share the flying motions; the engine maps them
            // below water (AOEngine::getStateForMotion()).
            const LLUUID remap[plus::ao::STATE_COUNT] =
            {
                ANIM_AGENT_STAND,
                ANIM_AGENT_WALK,
                ANIM_AGENT_RUN,
                ANIM_AGENT_SIT,
                ANIM_AGENT_SIT_GROUND_CONSTRAINED,
                ANIM_AGENT_CROUCH,
                ANIM_AGENT_CROUCHWALK,
                ANIM_AGENT_LAND,
                ANIM_AGENT_MEDIUM_LAND,
                ANIM_AGENT_STANDUP,
                ANIM_AGENT_FALLDOWN,
                ANIM_AGENT_HOVER_DOWN,
                ANIM_AGENT_HOVER_UP,
                ANIM_AGENT_FLY,
                ANIM_AGENT_FLYSLOW,
                ANIM_AGENT_HOVER,
                ANIM_AGENT_JUMP,
                ANIM_AGENT_PRE_JUMP,
                ANIM_AGENT_TURNRIGHT,
                ANIM_AGENT_TURNLEFT,
                ANIM_AGENT_TYPE,
                ANIM_AGENT_HOVER,
                ANIM_AGENT_FLY,
                ANIM_AGENT_HOVER_UP,
                ANIM_AGENT_HOVER_DOWN,
                LLUUID::null // "Always" overrides no motion; motions never map to it
            };

            std::array<StateDef, plus::ao::STATE_COUNT> defs;
            for (S32 i = 0; i < plus::ao::STATE_COUNT; ++i)
            {
                LLStringUtil::getTokens(names[i], defs[i].mAllNames, "|");
                defs[i].mName    = defs[i].mAllNames.front();
                defs[i].mRemapID = remap[i];
            }
            return defs;
        }();
        return sDefs;
    }

    // The longest flag suffix a set folder name gets (":SO:SM:DM:**").
    constexpr S32 SET_FLAGS_MAX_LENGTH = 12;

    U32 nextSerial()
    {
        static U32 sSerial = 0;
        return ++sSerial;
    }
}

namespace plus::ao
{
    const std::string& stateName(S32 state)
    {
        return stateDefs()[state].mName;
    }

    S32 stateByName(const std::string& name)
    {
        const auto& defs = stateDefs();
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            for (const std::string& candidate : defs[i].mAllNames)
            {
                if (candidate == name)
                {
                    return i;
                }
            }
        }
        return -1;
    }

    U32 newKey()
    {
        static U32 sKey = 0;
        return ++sKey;
    }

    bool trackBefore(const AOTrack& a, const AOTrack& b)
    {
        const S32 by_name = LLStringUtil::compareDict(a.mName, b.mName);
        return by_name != 0 ? by_name < 0 : a.mFolderID < b.mFolderID;
    }

    bool isValidFolderPart(const std::string& name, S32 max_length)
    {
        if (max_length <= 0)
        {
            max_length = DB_INV_ITEM_NAME_STR_LEN - SET_FLAGS_MAX_LENGTH;
        }
        if (name.empty() || (S32)name.size() > max_length)
        {
            return false;
        }
        for (unsigned char c : name)
        {
            if (c == ':' || c == '|' || c < 0x20 || c == 0x7f)
            {
                return false;
            }
        }
        std::string trimmed = name;
        LLStringUtil::trim(trimmed);
        return trimmed == name;
    }

    std::string groupFolderName(const std::string& name, S32 order)
    {
        return name + llformat(":OR%d", order);
    }

    bool parseGroupFolderName(const std::string& folder_name, std::string& name, S32& order)
    {
        std::vector<std::string> params;
        LLStringUtil::getTokens(folder_name, params, ":");
        if (params.empty() || params[0].empty())
        {
            return false;
        }
        name  = params[0];
        order = -1;
        for (size_t i = 1; i < params.size(); ++i)
        {
            if (params[i].compare(0, 2, "OR") == 0)
            {
                if (!LLStringUtil::convertToS32(params[i].substr(2), order))
                {
                    order = -1;
                }
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // AOTrack

    std::string AOTrack::folderName() const
    {
        std::string name = mName + ":TR";
        if (mCycleTime > 0.f)
        {
            name += llformat(":CT%.2f", mCycleTime);
        }
        if (mCycle)
        {
            name += ":CY";
        }
        if (mRandom)
        {
            name += ":RN";
        }
        return name;
    }

    // static
    bool AOTrack::parseFolderName(const std::string& folder_name, std::string& name, bool& cycle, bool& random, F32& cycle_time)
    {
        std::vector<std::string> params;
        LLStringUtil::getTokens(folder_name, params, ":");
        if (params.size() < 2 || params[0].empty())
        {
            return false;
        }

        bool is_track = false;
        cycle         = false;
        random        = false;
        cycle_time    = 0.f;
        for (size_t i = 1; i < params.size(); ++i)
        {
            const std::string& param = params[i];
            if (param == "TR")
            {
                is_track = true;
            }
            else if (param == "CY")
            {
                cycle = true;
            }
            else if (param == "RN")
            {
                random = true;
            }
            else if (param.compare(0, 2, "CT") == 0)
            {
                LLStringUtil::convertToF32(param.substr(2), cycle_time);
                cycle_time = AOState::clampCycleTime(cycle_time);
            }
        }
        name = params[0];
        return is_track;
    }

    const LLUUID& resolveAsset(AOAnimation& anim)
    {
        if (anim.mAssetID.isNull())
        {
            // The original, not the link: a link's own asset field holds the
            // linked item's id until that item is loaded.
            if (const LLViewerInventoryItem* item = gInventory.getItem(anim.mOriginalID); item && !item->getIsLinkType())
            {
                anim.mAssetID = item->getAssetUUID();
            }
        }
        return anim.mAssetID;
    }

    void stepAssets(AOAnimation& step, std::vector<LLUUID>& assets)
    {
        if (!step.mIsGroup)
        {
            if (const LLUUID& asset = resolveAsset(step); asset.notNull())
            {
                assets.push_back(asset);
            }
            return;
        }
        for (AOAnimation& member : step.mMembers)
        {
            if (const LLUUID& asset = resolveAsset(member); asset.notNull())
            {
                assets.push_back(asset);
            }
        }
    }

    // ------------------------------------------------------------------------
    // AOState

    const LLUUID& AOState::getRemapID() const
    {
        return stateDefs()[mIndex].mRemapID;
    }

    S32 AOState::nextSortOrder() const
    {
        S32 next = 0;
        for (const AOAnimation& anim : mAnimations)
        {
            next = llmax(next, anim.mSortOrder + 1);
        }
        return next;
    }

    std::string AOState::nextGroupName() const
    {
        for (S32 number = 1;; ++number)
        {
            const std::string candidate = llformat("Group %d", number);
            const bool        taken     = std::any_of(mAnimations.begin(), mAnimations.end(), [&candidate](const AOAnimation& anim)
                                                      { return anim.mIsGroup && anim.mName == candidate; });
            if (!taken)
            {
                return candidate;
            }
        }
    }

    std::string AOState::nextTrackName() const
    {
        for (S32 number = 1;; ++number)
        {
            const std::string candidate = llformat("Track %d", number);
            const bool        taken     = std::any_of(mTracks.begin(), mTracks.end(), [&candidate](const AOTrack& track)
                                                      { return track.mName == candidate; });
            if (!taken)
            {
                return candidate;
            }
        }
    }

    AOTrack* AOState::findTrack(U32 key)
    {
        for (AOTrack& track : mTracks)
        {
            if (track.mKey == key)
            {
                return &track;
            }
        }
        return nullptr;
    }

    AOAnimation* AOState::findGroup(U32 key)
    {
        for (AOAnimation& anim : mAnimations)
        {
            if (anim.mIsGroup && anim.mKey == key)
            {
                return &anim;
            }
        }
        return nullptr;
    }

    std::string AOState::folderName() const
    {
        std::string name = getName();
        if (mCycleTime > 0.f)
        {
            name += llformat(":CT%.2f", mCycleTime);
        }
        if (mCycle)
        {
            name += ":CY";
        }
        if (mRandom)
        {
            name += ":RN";
        }
        return name;
    }

    // static
    F32 AOState::clampCycleTime(F32 seconds)
    {
        return std::isnan(seconds) ? 0.f : llclamp(seconds, 0.f, 999.f);
    }

    // static
    bool AOState::parseFolderName(const std::string& folder_name, S32& state, bool& cycle, bool& random, F32& cycle_time)
    {
        std::vector<std::string> params;
        LLStringUtil::getTokens(folder_name, params, ":");
        if (params.empty())
        {
            return false;
        }

        state = stateByName(params[0]);
        if (state < 0)
        {
            return false;
        }

        cycle      = false;
        random     = false;
        cycle_time = 0.f;
        for (size_t i = 1; i < params.size(); ++i)
        {
            const std::string& param = params[i];
            if (param == "CY")
            {
                cycle = true;
            }
            else if (param == "RN")
            {
                random = true;
            }
            else if (param.compare(0, 2, "CT") == 0)
            {
                LLStringUtil::convertToF32(param.substr(2), cycle_time);
                cycle_time = clampCycleTime(cycle_time);
            }
            else
            {
                LL_WARNS("PlusAO") << "Unknown option " << param << " in state folder " << folder_name << LL_ENDL;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // AOSet

    AOSet::AOSet(const std::string& name) :
        mSerial(nextSerial()),
        mName(name)
    {
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            mStates[i].mIndex = i;
        }
    }

    AOState* AOSet::getStateByRemapID(const LLUUID& id)
    {
        if (id.isNull())
        {
            return nullptr; // "Always" has a null remap id and overrides no motion
        }
        const LLUUID& remap_id = (id == ANIM_AGENT_SIT_GROUND) ? ANIM_AGENT_SIT_GROUND_CONSTRAINED : id;
        for (AOState& state : mStates)
        {
            if (state.getRemapID() == remap_id)
            {
                return &state; // the first match: the swimming states come last
            }
        }
        return nullptr;
    }

    void AOSet::pickAnimation(AOState& state, std::vector<LLUUID>& assets) const
    {
        const size_t count = state.mAnimations.size();
        if (count == 0)
        {
            return;
        }

        if (state.mCycle)
        {
            if (state.mRandom)
            {
                state.mCurrentAnimation = llmin((U32)(ll_frand() * count), (U32)count - 1);
            }
            else
            {
                ++state.mCurrentAnimation;
            }
        }
        if (state.mCurrentAnimation >= count)
        {
            state.mCurrentAnimation = 0;
        }

        stepAssets(state.mAnimations[state.mCurrentAnimation], assets);
    }

    // static
    LLUUID AOSet::pickTrackAnimation(AOTrack& track)
    {
        const size_t count = track.mAnimations.size();
        if (count == 0)
        {
            return LLUUID::null;
        }

        if (track.mCycle)
        {
            if (track.mRandom)
            {
                track.mCurrentAnimation = llmin((U32)(ll_frand() * count), (U32)count - 1);
            }
            else
            {
                ++track.mCurrentAnimation;
            }
        }
        if (track.mCurrentAnimation >= count)
        {
            track.mCurrentAnimation = 0;
        }
        return resolveAsset(track.mAnimations[track.mCurrentAnimation]);
    }

    void AOSet::startTimer(F32 period)
    {
        mTimerPeriod  = period;
        mTimerRunning = true;
        mTimer.reset();
    }

    bool AOSet::timerFired()
    {
        if (!mTimerRunning || mTimer.getElapsedTimeF32() < mTimerPeriod)
        {
            return false;
        }
        mTimer.reset();
        return true;
    }

    std::string AOSet::folderName(bool is_default) const
    {
        std::string name = mName;
        if (mSitOverride)
        {
            name += ":SO";
        }
        if (mSmart)
        {
            name += ":SM";
        }
        if (mMouselookStandDisable)
        {
            name += ":DM";
        }
        if (is_default)
        {
            name += ":**";
        }
        return name;
    }

    // static
    bool AOSet::parseFolderName(const std::string& folder_name, std::string& name, bool& sit_override, bool& smart,
                                bool& mouselook_stand_disable, bool& is_default)
    {
        std::vector<std::string> params;
        LLStringUtil::getTokens(folder_name, params, ":");
        if (params.empty() || params[0].empty())
        {
            return false;
        }

        name                    = params[0];
        sit_override            = false;
        smart                   = false;
        mouselook_stand_disable = false;
        is_default              = false;
        for (size_t i = 1; i < params.size(); ++i)
        {
            const std::string& param = params[i];
            if (param == "SO")
            {
                sit_override = true;
            }
            else if (param == "SM")
            {
                smart = true;
            }
            else if (param == "DM")
            {
                mouselook_stand_disable = true;
            }
            else if (param == "**")
            {
                is_default = true;
            }
            else
            {
                LL_WARNS("PlusAO") << "Unknown option " << param << " in set folder " << folder_name << LL_ENDL;
            }
        }
        return true;
    }

    // static
    bool AOSet::isValidName(const std::string& name)
    {
        if (name.empty() || (S32)name.size() > DB_INV_ITEM_NAME_STR_LEN - SET_FLAGS_MAX_LENGTH)
        {
            return false;
        }
        for (unsigned char c : name)
        {
            if (c == ':' || c == '|' || c < 0x20 || c == 0x7f)
            {
                return false;
            }
        }
        // Folder names are trimmed by the inventory; a name must survive that.
        std::string trimmed = name;
        LLStringUtil::trim(trimmed);
        return trimmed == name;
    }
}
