/**
 * @file aofloater.cpp
 * @brief Animation Overrider: the window (floater_plus_ao.xml).
 *
 * Ported from Firestorm's ao.cpp (FloaterAO, see SOURCE.md).
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "aofloater.h"

#include "aoengine.h"
#include "aoset.h"
#include "aotransfer.h"
#include "plus.h"

#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llmenubutton.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdutil.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltoggleablemenu.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewermenu.h"
#include "llviewermenufile.h"

#include <algorithm>
#include <boost/bind.hpp>
#include <functional>
#include <map>

namespace
{
    constexpr char SETTING_FULL_INTERFACE[] = "PlusAOFullInterface"; // per account (FS: UseFullAOInterface)
    constexpr char SETTING_FULL_RECT[]      = "PlusAOFullRect";      // per account (FS: floater_rect_animation_overrider_full)

    constexpr char ICON_STOPPED[]       = "Inv_Animation";
    constexpr char ICON_PLAYING[]       = "Play_Off";
    constexpr char ICON_GROUP[]         = "Inv_FolderClosed";
    constexpr char ICON_GROUP_PLAYING[] = "Inv_FolderOpen";

    plus::ao::AOEngine& engine()
    {
        return plus::ao::AOEngine::instance();
    }
}

namespace plus::ao
{
    std::string aoText(const std::string& string_name, const LLSD& args)
    {
        AOFloater* floater = LLFloaterReg::getTypedInstance<AOFloater>(AOFloater::NAME);
        if (!floater || !floater->hasString(string_name))
        {
            return string_name;
        }
        LLStringUtil::format_map_t map;
        for (const auto& [key, value] : llsd::inMap(args))
        {
            map["[" + key + "]"] = value.asString();
        }
        return floater->getString(string_name, map);
    }

    void notifyUser(const std::string& string_name, const LLSD& args, bool alert, const std::string& note)
    {
        // The names in args may be someone else's (a set or animation named in a notecard or
        // file, a folder given by someone): no links of theirs in the notice, whose text box
        // trusts its text. Numbers stay as they are.
        LLSD safe_args = LLSD::emptyMap();
        for (const auto& [key, value] : llsd::inMap(args))
        {
            safe_args[key] = value.isString() ? LLSD(plus::safeText(value.asString())) : value;
        }
        std::string message = aoText(string_name, safe_args);
        if (!note.empty())
        {
            message += "\n" + note;
        }
        LLNotificationsUtil::add(alert ? "GenericAlert" : "SystemMessageTip", LLSD().with("MESSAGE", message));
    }

    AOFloater::AOFloater(const LLSD& key) :
        LLFloater(key)
    {
    }

    AOFloater::~AOFloater()
    {
        if (LLContextMenu* menu = mContextMenuHandle.get())
        {
            menu->die();
        }
    }

    bool AOFloater::postBuild()
    {
        mMainPanel  = getChild<LLPanel>("animation_overrider_panel");
        mSmallPanel = getChild<LLPanel>("animation_overrider_panel_small");

        mEnabledCheckBox          = mMainPanel->getChild<LLCheckBoxCtrl>("ao_enabled");
        mSetSelector              = mMainPanel->getChild<LLComboBox>("ao_set_selection_combo");
        mActivateButton           = mMainPanel->getChild<LLButton>("ao_activate");
        mAddButton                = mMainPanel->getChild<LLMenuButton>("ao_add");
        mTrackSelector            = mMainPanel->getChild<LLComboBox>("ao_track_selection_combo");
        mTrackAddButton           = mMainPanel->getChild<LLButton>("ao_track_add");
        mRenameButton             = mMainPanel->getChild<LLButton>("ao_rename");
        mRemoveButton             = mMainPanel->getChild<LLButton>("ao_remove");
        mDefaultCheckBox          = mMainPanel->getChild<LLCheckBoxCtrl>("ao_default");
        mOverrideSitsCheckBox     = mMainPanel->getChild<LLCheckBoxCtrl>("ao_sit_override");
        mSmartCheckBox            = mMainPanel->getChild<LLCheckBoxCtrl>("ao_smart");
        mDisableMouselookCheckBox = mMainPanel->getChild<LLCheckBoxCtrl>("ao_disable_stands_in_mouselook");
        mStateSelector            = mMainPanel->getChild<LLComboBox>("ao_state_selection_combo");
        mAnimationList            = mMainPanel->getChild<LLScrollListCtrl>("ao_state_animation_list");
        mMoveUpButton             = mMainPanel->getChild<LLButton>("ao_move_up");
        mMoveDownButton           = mMainPanel->getChild<LLButton>("ao_move_down");
        mTrashButton              = mMainPanel->getChild<LLButton>("ao_trash");
        mCycleCheckBox            = mMainPanel->getChild<LLCheckBoxCtrl>("ao_cycle");
        mRandomizeCheckBox        = mMainPanel->getChild<LLCheckBoxCtrl>("ao_randomize");
        mCycleTimeLabel           = mMainPanel->getChild<LLTextBox>("ao_cycle_time_seconds_label");
        mCycleTimeSpinner         = mMainPanel->getChild<LLSpinCtrl>("ao_cycle_time");
        mImportButton             = mMainPanel->getChild<LLButton>("ao_import");
        mExportButton             = mMainPanel->getChild<LLButton>("ao_export");
        mExportAllButton          = mMainPanel->getChild<LLButton>("ao_export_all");
        mReloadButton             = mMainPanel->getChild<LLButton>("ao_reload");
        mFsImportButton           = mMainPanel->getChild<LLButton>("ao_fs_import");
        mPreviousButton           = mMainPanel->getChild<LLButton>("ao_previous");
        mNextButton               = mMainPanel->getChild<LLButton>("ao_next");
        mStatusText               = mMainPanel->getChild<LLTextBox>("ao_status");

        mEnabledCheckBoxSmall      = mSmallPanel->getChild<LLCheckBoxCtrl>("ao_enabled_small");
        mSetSelectorSmall          = mSmallPanel->getChild<LLComboBox>("ao_set_selection_combo_small");
        mPreviousButtonSmall       = mSmallPanel->getChild<LLButton>("ao_previous_small");
        mNextButtonSmall           = mSmallPanel->getChild<LLButton>("ao_next_small");
        mOverrideSitsCheckBoxSmall = mSmallPanel->getChild<LLCheckBoxCtrl>("ao_sit_override_small");

        mSetSelector->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectSet(); });
        mSetSelector->setFocusLostCallback([this](LLFocusableElement*) { onSelectSet(); });
        mActivateButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickActivate(); });
        buildAddMenu();
        mRemoveButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRemove(); });
        mDefaultCheckBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckDefault(); });
        mOverrideSitsCheckBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckOverrideSits(mOverrideSitsCheckBox); });
        mSmartCheckBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckSmart(); });
        mDisableMouselookCheckBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckDisableStands(); });

        mStateSelector->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectState(); });
        mAnimationList->setCommitOnSelectionChange(true);
        mAnimationList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChangeAnimationSelection(); });
        mAnimationList->setDoubleClickCallback([this]() { onDoubleClick(); });
        mAnimationList->setRightMouseDownCallback([this](LLUICtrl*, S32 x, S32 y, MASK) { onAnimationListRightClick(x, y); });
        mTrackSelector->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectTrackTab(); });
        mTrackAddButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickAddTrack(); });
        mRenameButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRename(); });
        mMoveUpButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickMoveUp(); });
        mMoveDownButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickMoveDown(); });
        mTrashButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickTrash(); });
        mCycleCheckBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckCycle(); });
        mRandomizeCheckBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckRandomize(); });
        mCycleTimeSpinner->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChangeCycleTime(); });

        mImportButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickImport(); });
        mExportButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickExport(false); });
        mExportAllButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickExport(true); });
        mReloadButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReload(); });
        mFsImportButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickFsImport(); });
        mPreviousButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickPrevious(); });
        mNextButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickNext(); });
        mMainPanel->getChild<LLButton>("ao_less")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickLess(); });

        mSetSelectorSmall->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectSetSmall(); });
        mPreviousButtonSmall->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickPrevious(); });
        mNextButtonSmall->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickNext(); });
        mOverrideSitsCheckBoxSmall->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCheckOverrideSits(mOverrideSitsCheckBoxSmall); });
        mSmallPanel->getChild<LLButton>("ao_more")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickMore(); });

        AOEngine& ao = engine();
        mSetsConnection      = ao.setSetsChangedCallback([this]() { refreshSets(); });
        mStateConnection     = ao.setStateChangedCallback([this](U32 serial, S32 state) { onStateChanged(serial, state); });
        mAnimationConnection = ao.setAnimationChangedCallback([this]() { refreshPlaying(); });
        mStatusConnection    = ao.setStatusChangedCallback([this]() { refreshStatus(); });

        refreshSets();

        if (gSavedPerAccountSettings.getBOOL(SETTING_FULL_INTERFACE))
        {
            onClickMore();
        }
        else
        {
            onClickLess();
        }
        return true;
    }

    AOSet* AOFloater::selectedSet() const
    {
        return engine().findSet(mSelectedSerial);
    }

    // ------------------------------------------------------------------------
    // Showing the engine's sets

    void AOFloater::refreshSets()
    {
        AOEngine& ao = engine();

        // Alphabetical, as in Firestorm.
        std::vector<AOSet*> sets;
        for (const AOEngine::set_ptr_t& set : ao.getSets())
        {
            sets.push_back(set.get());
        }
        std::sort(sets.begin(), sets.end(), [](const AOSet* a, const AOSet* b) { return a->getName() < b->getName(); });

        if (!ao.findSet(mSelectedSerial))
        {
            const AOSet* fallback = ao.getCurrentSet() ? ao.getCurrentSet() : (sets.empty() ? nullptr : sets.front());
            mSelectedSerial       = fallback ? fallback->getSerial() : 0;
        }

        for (LLComboBox* selector : { mSetSelector, mSetSelectorSmall })
        {
            selector->removeall();
            selector->clear();
            if (sets.empty())
            {
                selector->add(getString(ao.isLoaded() ? "ao_no_sets_loaded" : "ao_loading"), LLSD(0), ADD_BOTTOM, false);
                selector->selectFirstItem();
                continue;
            }
            for (const AOSet* set : sets)
            {
                selector->add(set->getName(), LLSD((S32)set->getSerial()));
            }
            selector->setValue(LLSD((S32)mSelectedSerial));
        }

        refreshSetParameters();
        refreshStates();
        refreshAnimations();
        refreshStatus();
    }

    void AOFloater::refreshSetParameters()
    {
        const AOSet* set = selectedSet();
        mOverrideSitsCheckBox->setValue(set && set->getSitOverride());
        mOverrideSitsCheckBoxSmall->setValue(set && set->getSitOverride());
        mSmartCheckBox->setValue(set && set->getSmart());
        mDisableMouselookCheckBox->setValue(set && set->getMouselookStandDisable());
        mDefaultCheckBox->setValue(set && set == engine().getDefaultSet());
    }

    void AOFloater::refreshStates()
    {
        const AOSet* set = selectedSet();

        mStateSelector->removeall();
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            std::string label = getString(llformat("state_%d", i));
            if (set && set->getState(i).hasContent())
            {
                label += llformat(" (%d)", (S32)set->getState(i).mAnimations.size());
            }
            mStateSelector->add(label, LLSD(i));
        }
        mStateSelector->setValue(LLSD(mSelectedState));

        rebuildTrackSelector();
        updateCycleValues();
    }

    const AOState* AOFloater::selectedState() const
    {
        const AOSet* set = selectedSet();
        return set ? &set->getState(mSelectedState) : nullptr;
    }

    bool AOFloater::inTrackView() const
    {
        const AOState* state = selectedState();
        return state && mCurrentTrack > 0 && (size_t)mCurrentTrack <= state->mTracks.size();
    }

    void AOFloater::rebuildTrackSelector()
    {
        const AOState* state = selectedState();
        const size_t   count = state ? state->mTracks.size() : 0;
        if ((size_t)mCurrentTrack > count)
        {
            mCurrentTrack = 0;
        }

        mTrackSelector->removeall();
        mTrackSelector->add(getString("ao_track_main"), LLSD(0));
        for (size_t n = 0; n < count; ++n)
        {
            mTrackSelector->add(state->mTracks[n].mName, LLSD((S32)n + 1));
        }
        mTrackSelector->setValue(LLSD(mCurrentTrack));
    }

    void AOFloater::updateCycleValues()
    {
        const AOState* state = selectedState();
        if (inTrackView())
        {
            const AOTrack& track = state->mTracks[mCurrentTrack - 1];
            mCycleCheckBox->setValue(track.mCycle);
            mRandomizeCheckBox->setValue(track.mRandom);
            mCycleTimeSpinner->setValue(track.mCycleTime);
            return;
        }
        mCycleCheckBox->setValue(state && state->mCycle);
        mRandomizeCheckBox->setValue(state && state->mRandom);
        mCycleTimeSpinner->setValue(state ? state->mCycleTime : 0.f);
    }

    const AOFloater::Row* AOFloater::rowOf(const LLScrollListItem* item) const
    {
        if (!item)
        {
            return nullptr;
        }
        const S32 id = item->getValue().asInteger();
        return (id >= 0 && (size_t)id < mRows.size()) ? &mRows[id] : nullptr;
    }

    const AOAnimation* AOFloater::animOf(const Row& row) const
    {
        const AOState* state = selectedState();
        if (!state || row.mIndex < 0)
        {
            return nullptr;
        }
        const size_t index = (size_t)row.mIndex;
        if (row.mKind == 't')
        {
            const S32 track = mCurrentTrack - 1;
            return (track >= 0 && (size_t)track < state->mTracks.size() && index < state->mTracks[track].mAnimations.size())
                       ? &state->mTracks[track].mAnimations[index] : nullptr;
        }
        if (index >= state->mAnimations.size())
        {
            return nullptr;
        }
        const AOAnimation& entry = state->mAnimations[index];
        if (row.mKind == 'm')
        {
            return (row.mMember >= 0 && (size_t)row.mMember < entry.mMembers.size()) ? &entry.mMembers[row.mMember] : nullptr;
        }
        return (row.mKind == 's' || row.mKind == 'g') ? &entry : nullptr;
    }

    namespace
    {
        // Whether the entry is the one a target was taken of: a group by its key, an animation by its item, both by
        // the name too.
        bool sameEntry(const AOAnimation& entry, const LLUUID& item, U32 key, const std::string& name)
        {
            return entry.mName == name && (entry.mIsGroup ? entry.mKey == key : entry.mOriginalID == item);
        }
    }

    AOFloater::RowTarget AOFloater::targetOf(const Row& row) const
    {
        RowTarget target;
        target.mSerial = mSelectedSerial;
        target.mState  = mSelectedState;
        target.mTrack  = mCurrentTrack;
        target.mRow    = row;
        if (const AOAnimation* entry = animOf(row))
        {
            target.mItem = entry->mIsGroup ? LLUUID::null : entry->mOriginalID;
            target.mKey  = entry->mIsGroup ? entry->mKey : 0;
            target.mName = entry->mName;
        }
        return target;
    }

    bool AOFloater::stillShows(const RowTarget& target) const
    {
        if (target.mSerial != mSelectedSerial || target.mState != mSelectedState || target.mTrack != mCurrentTrack)
        {
            return false;
        }
        const AOAnimation* entry = animOf(target.mRow);
        return entry && sameEntry(*entry, target.mItem, target.mKey, target.mName);
    }

    void AOFloater::addRow(const std::string& icon, const std::string& name, const Row& row, bool)
    {
        LLSD data;
        data["value"]                = (S32)mRows.size();
        data["columns"][0]["column"] = "icon";
        data["columns"][0]["type"]   = "icon";
        data["columns"][0]["value"]  = icon;
        data["columns"][1]["column"] = "animation_name";
        data["columns"][1]["type"]   = "text";
        data["columns"][1]["value"]  = name;
        mRows.push_back(row);
        mAnimationList->addElement(data);
    }

    void AOFloater::selectRow(const Row& row)
    {
        for (size_t id = 0; id < mRows.size(); ++id)
        {
            const Row& have = mRows[id];
            if (have.mKind == row.mKind && have.mIndex == row.mIndex && have.mMember == row.mMember)
            {
                mAnimationList->setSelectedByValue(LLSD((S32)id), true);
                return;
            }
        }
    }

    ListRef AOFloater::listOf(const Row& row) const
    {
        ListRef list;
        if (row.mKind == 't')
        {
            list.mTrack = mCurrentTrack - 1;
        }
        else if (row.mKind == 'm')
        {
            list.mGroup = row.mIndex;
        }
        return list;
    }

    void AOFloater::refreshAnimations()
    {
        mCurrentTrackKey = inTrackView() ? selectedState()->mTracks[mCurrentTrack - 1].mKey : 0;
        mCurrentTrackName = mCurrentTrackKey ? selectedState()->mTracks[mCurrentTrack - 1].mName : std::string();

        // Keep the selection and the scroll position while the same list changes.
        std::vector<Row> selected;
        for (const LLScrollListItem* item : mAnimationList->getAllSelected())
        {
            if (const Row* row = rowOf(item))
            {
                selected.push_back(*row);
            }
        }
        const S32 scroll = mAnimationList->getScrollPos();

        mAnimationList->deleteAllItems();
        mRows.clear();

        const AOSet* set = selectedSet();
        if (!set)
        {
            mAnimationList->setCommentText(getString("ao_no_sets_loaded"));
            onChangeAnimationSelection();
            return;
        }

        const AOState& state = set->getState(mSelectedState);
        if (inTrackView())
        {
            const AOTrack& track = state.mTracks[mCurrentTrack - 1];
            if (track.mAnimations.empty())
            {
                mAnimationList->setCommentText(getString("ao_no_track_animations"));
            }
            for (size_t index = 0; index < track.mAnimations.size(); ++index)
            {
                Row row;
                row.mKind  = 't';
                row.mIndex = (S32)index;
                addRow(ICON_STOPPED, track.mAnimations[index].mName, row, false);
            }
        }
        else
        {
            if (state.empty())
            {
                mAnimationList->setCommentText(getString("ao_drop_animations_here"));
            }
            for (size_t index = 0; index < state.mAnimations.size(); ++index)
            {
                const AOAnimation& anim = state.mAnimations[index];
                Row                row;
                row.mIndex = (S32)index;
                if (!anim.mIsGroup)
                {
                    row.mKind = 's';
                    addRow(ICON_STOPPED, anim.mName, row, false);
                    continue;
                }
                row.mKind = 'g';
                addRow(ICON_GROUP, anim.mName, row, false);
                if (anim.mFolderID.notNull() && mCollapsed.count(anim.mFolderID))
                {
                    continue;
                }
                for (size_t member = 0; member < anim.mMembers.size(); ++member)
                {
                    Row member_row;
                    member_row.mKind   = 'm';
                    member_row.mIndex  = (S32)index;
                    member_row.mMember = (S32)member;
                    addRow(ICON_STOPPED, "    " + anim.mMembers[member].mName, member_row, false);
                }
            }
        }

        for (const Row& row : selected)
        {
            selectRow(row);
        }
        mAnimationList->setScrollPos(scroll);

        refreshPlaying();
        onChangeAnimationSelection();
    }

    void AOFloater::refreshPlaying()
    {
        const AOSet* set = selectedSet();
        if (!set)
        {
            return;
        }

        const AOState& state = set->getState(mSelectedState);
        for (LLScrollListItem* item : mAnimationList->getAllData())
        {
            const Row* row = rowOf(item);
            if (!row)
            {
                continue;
            }

            bool playing = false;
            bool pending = false; // in italics until the inventory has the link or folder
            switch (row->mKind)
            {
                case 't':
                {
                    if (!inTrackView())
                    {
                        break;
                    }
                    const AOTrack& track = state.mTracks[mCurrentTrack - 1];
                    playing = engine().isTrackPlaying(set, mSelectedState, mCurrentTrack - 1, (size_t)row->mIndex);
                    pending = (size_t)row->mIndex < track.mAnimations.size() && track.mAnimations[row->mIndex].mLinkID.isNull();
                    break;
                }
                case 'm':
                {
                    if ((size_t)row->mIndex >= state.mAnimations.size())
                    {
                        break;
                    }
                    playing = engine().isPlaying(set, mSelectedState, (size_t)row->mIndex);
                    const AOAnimation& group = state.mAnimations[row->mIndex];
                    pending = (size_t)row->mMember < group.mMembers.size() && group.mMembers[row->mMember].mLinkID.isNull();
                    break;
                }
                default:
                {
                    playing = engine().isPlaying(set, mSelectedState, (size_t)row->mIndex);
                    if ((size_t)row->mIndex < state.mAnimations.size())
                    {
                        const AOAnimation& anim = state.mAnimations[row->mIndex];
                        pending = anim.mIsGroup ? anim.mFolderID.isNull() : anim.mLinkID.isNull();
                    }
                    break;
                }
            }

            if (LLScrollListIcon* icon = dynamic_cast<LLScrollListIcon*>(item->getColumn(0)))
            {
                if (row->mKind == 'g')
                {
                    icon->setValue(playing ? ICON_GROUP_PLAYING : ICON_GROUP);
                }
                else
                {
                    icon->setValue(playing ? ICON_PLAYING : ICON_STOPPED);
                }
            }
            if (LLScrollListText* text = dynamic_cast<LLScrollListText*>(item->getColumn(1)))
            {
                text->setFontStyle(playing ? LLFontGL::BOLD : (pending ? LLFontGL::ITALIC : LLFontGL::NORMAL));
            }
        }
    }

    void AOFloater::refreshStatus()
    {
        AOEngine& ao = engine();
        if (!ao.isLoaded())
        {
            mStatusText->setText(getString("ao_loading"));
        }
        else if (ao.getStore().isBusy())
        {
            mStatusText->setText(getString("ao_reloading"));
        }
        else if (const S32 pending = ao.pendingRequests(); pending > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", pending);
            mStatusText->setText(getString("ao_saving", args));
        }
        else
        {
            mStatusText->setText(LLStringUtil::null);
        }
        enableControls();
    }

    void AOFloater::onStateChanged(U32 set_serial, S32 state)
    {
        if (set_serial != mSelectedSerial)
        {
            return;
        }
        refreshStates(); // the counts
        if (state == mSelectedState)
        {
            // The track that was shown, at its place now (by its key, else by its name; gone: the main list)
            if (mCurrentTrackKey && selectedState())
            {
                const std::vector<AOTrack>& tracks = selectedState()->mTracks;
                auto it = std::find_if(tracks.begin(), tracks.end(), [this](const AOTrack& t) { return t.mKey == mCurrentTrackKey; });
                if (it == tracks.end())
                {
                    it = std::find_if(tracks.begin(), tracks.end(), [this](const AOTrack& t) { return t.mName == mCurrentTrackName; });
                }
                mCurrentTrack = it == tracks.end() ? 0 : (S32)(it - tracks.begin()) + 1;
            }
            rebuildTrackSelector();
            updateCycleValues();
            refreshAnimations();
        }
    }

    void AOFloater::enableControls()
    {
        AOEngine&  ao      = engine();
        const bool loaded  = ao.isLoaded();
        const bool has_set = loaded && selectedSet();

        for (LLUICtrl* ctrl : std::initializer_list<LLUICtrl*>{ mSetSelector, mSetSelectorSmall, mActivateButton, mRemoveButton,
                                                                 mDefaultCheckBox, mOverrideSitsCheckBox, mOverrideSitsCheckBoxSmall,
                                                                 mDisableMouselookCheckBox, mStateSelector, mAnimationList,
                                                                 mCycleCheckBox, mExportButton, mPreviousButton, mNextButton,
                                                                 mTrackSelector, mTrackAddButton,
                                                                 mPreviousButtonSmall, mNextButtonSmall })
        {
            ctrl->setEnabled(has_set);
        }
        mSmartCheckBox->setEnabled(has_set && mOverrideSitsCheckBox->getValue().asBoolean());
        mAddButton->setEnabled(loaded);
        mImportButton->setEnabled(loaded);
        mExportAllButton->setEnabled(loaded && !ao.getSets().empty());
        const bool store_busy = ao.getStore().isBusy() || ao.getStore().isImporting();
        mReloadButton->setEnabled(loaded && !store_busy);
        mFsImportButton->setEnabled(loaded && !store_busy);

        updateCycleParameters();
        onChangeAnimationSelection();
    }

    void AOFloater::updateCycleParameters()
    {
        const bool enabled = mCycleCheckBox->getEnabled() && mCycleCheckBox->getValue().asBoolean();
        mRandomizeCheckBox->setEnabled(enabled);
        mCycleTimeLabel->setEnabled(enabled);
        mCycleTimeSpinner->setEnabled(enabled);
    }

    // ------------------------------------------------------------------------
    // Sets

    void AOFloater::onSelectSet()
    {
        // Text typed into the box (no item picked): a new name for the set.
        if (mSetSelector->getSelectedItemLabel().empty())
        {
            renameSet();
            return;
        }

        const U32 serial = (U32)mSetSelector->getValue().asInteger();
        if (serial == mSelectedSerial || !engine().findSet(serial))
        {
            return;
        }

        mSelectedSerial = serial;
        mCurrentTrack   = 0;
        mSetSelectorSmall->setValue(LLSD((S32)serial));
        refreshSetParameters();
        refreshStates();
        refreshAnimations();
        enableControls();
    }

    void AOFloater::onSelectSetSmall()
    {
        const U32 serial = (U32)mSetSelectorSmall->getValue().asInteger();
        if (!engine().findSet(serial))
        {
            return;
        }

        mSelectedSerial = serial;
        mCurrentTrack   = 0;
        mSetSelector->setValue(LLSD((S32)serial));
        refreshSetParameters();
        refreshStates();
        refreshAnimations();
        enableControls();

        // The small selector switches the set at once.
        onClickActivate();
    }

    void AOFloater::renameSet()
    {
        AOSet* set = selectedSet();
        if (!set)
        {
            return;
        }

        std::string name = mSetSelector->getSimple();
        LLStringUtil::trim(name);

        const bool unchanged = name.empty() || name == set->getName();
        if (!unchanged)
        {
            LLSD args;
            args["NAME"] = name;
            if (!AOSet::isValidName(name))
            {
                notifyUser("ao_name_invalid", args, true);
            }
            else if (engine().findSetByName(name))
            {
                notifyUser("ao_name_taken", args, true);
            }
            else if (engine().renameSet(set, name))
            {
                return; // the list is rebuilt with the new name
            }
        }
        mSetSelector->setValue(LLSD((S32)set->getSerial()));
        mSetSelector->setLabel(set->getName());
    }

    void AOFloater::onClickActivate()
    {
        if (AOSet* set = selectedSet())
        {
            engine().selectSet(set);
        }
    }

    void AOFloater::buildAddMenu()
    {
        LLToggleableMenu::Params menu_params;
        menu_params.name("ao_add_menu");
        menu_params.visible(false);
        LLToggleableMenu* menu = LLUICtrlFactory::create<LLToggleableMenu>(menu_params);

        LLMenuItemCallGL::Params blank_params;
        blank_params.name("ao_add_blank");
        blank_params.label(getString("ao_add_blank_label"));
        blank_params.on_click.function([this](LLUICtrl*, const LLSD&) { onAddSet(false); });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(blank_params));

        LLMenuItemCallGL::Params clone_params;
        clone_params.name("ao_add_clone");
        clone_params.label(getString("ao_add_clone_label"));
        clone_params.on_click.function([this](LLUICtrl*, const LLSD&) { onAddSet(true); });
        mCloneSetItem = LLUICtrlFactory::create<LLMenuItemCallGL>(clone_params);
        menu->addChild(mCloneSetItem);

        mAddButton->setMenu(menu, LLMenuButton::MP_BOTTOM_RIGHT, true);
        mAddButton->setMouseDownCallback(boost::bind(&AOFloater::onAddMenuShow, this));
    }

    void AOFloater::onAddMenuShow()
    {
        if (mCloneSetItem)
        {
            mCloneSetItem->setEnabled(selectedSet() != nullptr);
        }
    }

    void AOFloater::onAddSet(bool clone)
    {
        AOEngine& ao  = engine();
        AOSet*    set = nullptr;
        if (clone)
        {
            const AOSet* source = selectedSet();
            if (!source)
            {
                return;
            }
            LLStringUtil::format_map_t args;
            args["[NAME]"] = source->getName();
            const std::string name = ao.uniqueSetName(getString("ao_clone_name", args));
            set                    = ao.cloneSet(source, name);
            if (!set)
            {
                notifyUser("ao_name_invalid", LLSD().with("NAME", name), true);
                return;
            }
        }
        else
        {
            set = ao.addSet(ao.uniqueSetName(getString("ao_new_set_name")));
        }
        if (set)
        {
            mSelectedSerial = set->getSerial();
            refreshSets();
            notifyUser("ao_new_set_hint", LLSD().with("NAME", set->getName()));
        }
    }

    void AOFloater::onClickRemove()
    {
        const AOSet* set = selectedSet();
        if (!set)
        {
            return;
        }

        LLStringUtil::format_map_t args;
        args["[NAME]"]   = plus::safeText(set->getName());   // a set may come from someone else
        const U32 serial = set->getSerial();
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", getString("ao_remove_confirm", args)), LLSD(),
                                 [serial](const LLSD& notification, const LLSD& response)
                                 {
                                     if (LLNotificationsUtil::getSelectedOption(notification, response) == 0 && AOEngine::instanceExists())
                                     {
                                         AOEngine::instance().removeSet(AOEngine::instance().findSet(serial));
                                     }
                                 });
    }

    void AOFloater::onClickFsImport()
    {
        engine().importFromFirestorm();
    }

    void AOFloater::onClickReload()
    {
        engine().reload();
    }

    void AOFloater::onCheckDefault()
    {
        if (AOSet* set = selectedSet())
        {
            if (mDefaultCheckBox->getValue().asBoolean())
            {
                engine().setDefaultSet(set);
            }
            else if (engine().getDefaultSet() == set)
            {
                engine().setDefaultSet(nullptr);
            }
        }
    }

    void AOFloater::onCheckOverrideSits(LLCheckBoxCtrl* source)
    {
        const bool override_sit = source->getValue().asBoolean();
        mOverrideSitsCheckBox->setValue(override_sit);
        mOverrideSitsCheckBoxSmall->setValue(override_sit);
        engine().setOverrideSits(selectedSet(), override_sit);
        enableControls();
    }

    void AOFloater::onCheckSmart()
    {
        engine().setSmart(selectedSet(), mSmartCheckBox->getValue().asBoolean());
    }

    void AOFloater::onCheckDisableStands()
    {
        engine().setDisableMouselookStands(selectedSet(), mDisableMouselookCheckBox->getValue().asBoolean());
    }

    // ------------------------------------------------------------------------
    // States and animations

    void AOFloater::onSelectState()
    {
        const S32 state = mStateSelector->getValue().asInteger();
        if (state < 0 || state >= STATE_COUNT || state == mSelectedState)
        {
            return;
        }
        mSelectedState = state;
        mCurrentTrack  = 0;
        mAnimationList->deselectAllItems(true);
        mAnimationList->setScrollPos(0);
        refreshStates();
        refreshAnimations();
        enableControls();
    }

    void AOFloater::onSelectTrackTab()
    {
        const S32 track = mTrackSelector->getValue().asInteger();
        if (track < 0 || track == mCurrentTrack)
        {
            return;
        }
        mCurrentTrack = track;
        mAnimationList->deselectAllItems(true);
        mAnimationList->setScrollPos(0);
        updateCycleValues();
        refreshAnimations();
        enableControls();
    }

    void AOFloater::onClickAddTrack()
    {
        AOSet* set = selectedSet();
        if (!set)
        {
            return;
        }
        const S32 track = engine().addTrack(set, mSelectedState);
        if (track >= 0)
        {
            mCurrentTrack = track + 1;
            mAnimationList->deselectAllItems(true);
            rebuildTrackSelector();
            updateCycleValues();
            refreshAnimations();
            enableControls();
        }
    }

    void AOFloater::onChangeAnimationSelection()
    {
        const std::vector<LLScrollListItem*> items = mAnimationList->getAllSelected();
        const size_t selected = items.size();
        const bool   enabled  = mAnimationList->getEnabled();
        mMoveUpButton->setEnabled(enabled && selected == 1);
        mMoveDownButton->setEnabled(enabled && selected == 1);
        mTrashButton->setEnabled(enabled && selected > 0);
        const Row* row = selected == 1 ? rowOf(items.front()) : nullptr;
        mRenameButton->setEnabled(enabled && row && row->mKind == 'g');
    }

    void AOFloater::moveSelected(bool up)
    {
        AOSet* set = selectedSet();
        if (!set || mAnimationList->getAllSelected().size() != 1)
        {
            return;
        }

        const Row* found = rowOf(mAnimationList->getFirstSelected());
        if (!found)
        {
            return;
        }
        const Row row = *found;

        // The list is rebuilt from the engine; the selection moves with the animation.
        const S32 index = row.mKind == 'm' ? row.mMember : row.mIndex;
        mAnimationList->deselectAllItems(true);
        Row after = row;
        if (engine().moveAnimation(set, mSelectedState, (size_t)index, up, listOf(row)))
        {
            (row.mKind == 'm' ? after.mMember : after.mIndex) = up ? index - 1 : index + 1;
        }
        selectRow(after);
        onChangeAnimationSelection();
    }

    void AOFloater::onClickMoveUp()
    {
        moveSelected(true);
    }

    void AOFloater::onClickMoveDown()
    {
        moveSelected(false);
    }

    void AOFloater::onClickTrash()
    {
        std::vector<Row> rows;
        for (const LLScrollListItem* item : mAnimationList->getAllSelected())
        {
            if (const Row* row = rowOf(item))
            {
                rows.push_back(*row);
            }
        }
        deleteRows(rows);
    }

    void AOFloater::deleteRows(const std::vector<Row>& rows)
    {
        AOSet* set = selectedSet();
        if (!set || rows.empty())
        {
            return;
        }
        mAnimationList->deselectAllItems(true);

        if (inTrackView())
        {
            std::vector<size_t> indices;
            for (const Row& row : rows)
            {
                if (row.mKind == 't')
                {
                    indices.push_back((size_t)row.mIndex);
                }
            }
            ListRef list;
            list.mTrack = mCurrentTrack - 1;
            engine().removeAnimations(set, mSelectedState, indices, list);
            return;
        }

        // Entry by entry from the end of the list: removing members can end a group, which
        // moves the entries after it, so the indices before it must still be right.
        struct Selection
        {
            bool                whole = false;
            std::vector<size_t> members;
        };
        std::map<S32, Selection, std::greater<S32>> by_entry;
        for (const Row& row : rows)
        {
            if (row.mKind == 'm')
            {
                by_entry[row.mIndex].members.push_back((size_t)row.mMember);
            }
            else if (row.mKind == 's' || row.mKind == 'g')
            {
                by_entry[row.mIndex].whole = true;
            }
        }
        for (const auto& [index, selection] : by_entry)
        {
            if (selection.whole)
            {
                engine().removeAnimations(set, mSelectedState, { (size_t)index });
            }
            else
            {
                ListRef list;
                list.mGroup = index;
                engine().removeAnimations(set, mSelectedState, selection.members, list);
            }
        }
    }

    void AOFloater::onCheckCycle()
    {
        if (inTrackView())
        {
            engine().setTrackCycle(selectedSet(), mSelectedState, mCurrentTrack - 1, mCycleCheckBox->getValue().asBoolean());
        }
        else
        {
            engine().setCycle(selectedSet(), mSelectedState, mCycleCheckBox->getValue().asBoolean());
        }
        updateCycleParameters();
    }

    void AOFloater::onCheckRandomize()
    {
        if (inTrackView())
        {
            engine().setTrackRandomize(selectedSet(), mSelectedState, mCurrentTrack - 1, mRandomizeCheckBox->getValue().asBoolean());
        }
        else
        {
            engine().setRandomize(selectedSet(), mSelectedState, mRandomizeCheckBox->getValue().asBoolean());
        }
    }

    void AOFloater::onChangeCycleTime()
    {
        if (inTrackView())
        {
            engine().setTrackCycleTime(selectedSet(), mSelectedState, mCurrentTrack - 1, mCycleTimeSpinner->getValueF32());
        }
        else
        {
            engine().setCycleTime(selectedSet(), mSelectedState, mCycleTimeSpinner->getValueF32());
        }
    }

    void AOFloater::onClickPrevious()
    {
        engine().cycle(AOEngine::CyclePrevious, true);
    }

    void AOFloater::onClickNext()
    {
        engine().cycle(AOEngine::CycleNext, true);
    }

    void AOFloater::onDoubleClick()
    {
        const Row* row = rowOf(mAnimationList->getFirstSelected());
        if (!row)
        {
            return;
        }
        if (row->mKind == 't')
        {
            engine().playTrackAnimation(selectedSet(), mSelectedState, mCurrentTrack - 1, (size_t)row->mIndex);
        }
        else
        {
            engine().playAnimation(selectedSet(), mSelectedState, (size_t)row->mIndex, row->mKind == 'm' ? row->mMember : -1);
        }
    }

    // ------------------------------------------------------------------------
    // Groups, the context menu

    void AOFloater::onClickRename()
    {
        if (const Row* row = rowOf(mAnimationList->getFirstSelected()))
        {
            renameGroup(*row);
        }
    }

    void AOFloater::renameGroup(const Row& row)
    {
        const AOState* state = selectedState();
        if (!state || row.mKind != 'g' || row.mIndex < 0 || (size_t)row.mIndex >= state->mAnimations.size())
        {
            return;
        }

        // The group by its place, and by what it is: the dialog stays open while the list may change (research/275 L097)
        const RowTarget target = targetOf(row);
        LLSD payload;
        payload["serial"] = (S32)mSelectedSerial;
        payload["state"]  = mSelectedState;
        payload["index"]  = row.mIndex;
        payload["key"]    = (S32)target.mKey;
        payload["name"]   = target.mName;
        LLHandle<LLFloater> handle = getHandle();
        LLNotificationsUtil::add("PlusAORenameGroup", LLSD().with("NAME", target.mName), payload,
                                 [handle](const LLSD& notification, const LLSD& response)
                                 {
                                     if (AOFloater* floater = static_cast<AOFloater*>(handle.get()))
                                     {
                                         floater->renameGroupCallback(notification, response);
                                     }
                                 });
    }

    void AOFloater::renameGroupCallback(const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
        {
            return;
        }
        std::string name = response["name"].asString();
        LLStringUtil::trim(name);
        const LLSD& payload = notification["payload"];
        AOSet*      set     = engine().findSet((U32)payload["serial"].asInteger());
        const S32   state   = payload["state"].asInteger();
        if (!set || name.empty() || state < 0 || state >= STATE_COUNT)
        {
            return;
        }
        // The group the dialog was opened on: at its place if it is still there, else wherever it went; gone, nothing
        const std::vector<AOAnimation>& entries = set->getState(state).mAnimations;
        const U32    key    = (U32)payload["key"].asInteger();
        const std::string old_name = payload["name"].asString();
        size_t index = (size_t)llmax(payload["index"].asInteger(), 0);
        if (index >= entries.size() || !entries[index].mIsGroup || !sameEntry(entries[index], LLUUID::null, key, old_name))
        {
            index = entries.size();
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (entries[i].mIsGroup && sameEntry(entries[i], LLUUID::null, key, old_name))
                {
                    index = i;
                    break;
                }
            }
            if (index == entries.size())
            {
                return;
            }
        }
        if (!engine().renameGroup(set, state, index, name))
        {
            notifyUser("ao_group_name_invalid", LLSD().with("NAME", name), true);
        }
    }

    void AOFloater::toggleGroup(S32 index)
    {
        const AOState* state = selectedState();
        if (!state || (size_t)index >= state->mAnimations.size() || !state->mAnimations[index].mIsGroup)
        {
            return;
        }
        const LLUUID& folder = state->mAnimations[index].mFolderID;
        if (folder.isNull())
        {
            return;
        }
        if (!mCollapsed.erase(folder))
        {
            mCollapsed.insert(folder);
        }
        refreshAnimations();
    }

    void AOFloater::onAnimationListRightClick(S32 x, S32 y)
    {
        if (!mMore)
        {
            return;
        }
        mAnimationList->selectItemAt(x, y, MASK_NONE);
        showContextMenu(x, y);
    }

    void AOFloater::showContextMenu(S32 x, S32 y)
    {
        const AOState* state = selectedState();
        const Row*     found = rowOf(mAnimationList->getFirstSelected());
        const bool     track_view = inTrackView();
        if (!state || (!found && !track_view))
        {
            return;
        }

        if (LLContextMenu* old_menu = mContextMenuHandle.get())
        {
            old_menu->die();
            mContextMenuHandle.markDead();
        }

        LLContextMenu::Params menu_params;
        menu_params.name("ao_row_context_menu");
        menu_params.visible(false);
        LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);

        LLHandle<LLFloater> handle = getHandle();
        auto add_item = [&](const std::string& name, const std::string& label, std::function<void(AOFloater&)> action, bool enabled = true)
        {
            LLMenuItemCallGL::Params item_params;
            item_params.name(name);
            item_params.label(label);
            item_params.enabled.set(enabled);
            item_params.on_click.function([handle, action](LLUICtrl*, const LLSD&)
                                          {
                                              if (AOFloater* floater = static_cast<AOFloater*>(handle.get()))
                                              {
                                                  action(*floater);
                                              }
                                          });
            menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(item_params));
        };

        if (found)
        {
            const Row row = *found;
            const AOAnimation* anim = animOf(row);
            // The row's places name another entry once the list was rebuilt (an edit, the inventory coming in): the
            // items act only while the row still shows the entry the menu was opened on (research/275 L097).
            const RowTarget target = targetOf(row);

            if (row.mKind == 'g' && anim)
            {
                const bool collapsed = anim->mFolderID.notNull() && mCollapsed.count(anim->mFolderID);
                add_item("ao_cm_expand", getString(collapsed ? "ao_cm_expand" : "ao_cm_collapse"),
                         [target](AOFloater& floater) { if (floater.stillShows(target)) floater.toggleGroup(target.mRow.mIndex); },
                         anim->mFolderID.notNull());
                add_item("ao_cm_rename", getString("ao_cm_rename_group"),
                         [target](AOFloater& floater) { if (floater.stillShows(target)) floater.renameGroup(target.mRow); });
            }
            else if (row.mKind == 'm')
            {
                add_item("ao_cm_move_out", getString("ao_cm_move_out"), [target](AOFloater& floater)
                         {
                             AOSet* set = engine().findSet(target.mSerial);
                             if (set && floater.stillShows(target))
                             {
                                 engine().extractMember(set, target.mState, (size_t)target.mRow.mIndex, (size_t)target.mRow.mMember);
                             }
                         });
            }

            // The original of an animation: only for one that has a link in the inventory.
            if (anim && row.mKind != 'g')
            {
                const LLUUID link = anim->mLinkID;
                const bool   can  = link.notNull() && gInventory.getItem(anim->mOriginalID);
                add_item("ao_cm_find_original", getString("ao_cm_find_original"), [link](AOFloater&) { show_item_original(link); }, can);
            }

            menu->addSeparator();
            add_item("ao_cm_delete", getString("ao_cm_delete"),
                     [target](AOFloater& floater) { if (floater.stillShows(target)) floater.deleteRows({ target.mRow }); });
        }

        if (track_view)
        {
            if (found)
            {
                menu->addSeparator();
            }
            // The track by its key: its place may have changed by the time the item is chosen (research/275 L097)
            const U32 serial      = mSelectedSerial;
            const S32 state_index = mSelectedState;
            const U32 track_key   = state->mTracks[mCurrentTrack - 1].mKey;
            add_item("ao_cm_remove_track", getString("ao_cm_remove_track"), [serial, state_index, track_key](AOFloater& floater)
                     {
                         AOSet* set = engine().findSet(serial);
                         if (!set || floater.mSelectedSerial != serial || floater.mSelectedState != state_index)
                         {
                             return;
                         }
                         const std::vector<AOTrack>& tracks = set->getState(state_index).mTracks;
                         const auto it = std::find_if(tracks.begin(), tracks.end(),
                                                      [track_key](const AOTrack& t) { return t.mKey == track_key; });
                         if (it != tracks.end())
                         {
                             const S32 track = (S32)(it - tracks.begin());
                             floater.mCurrentTrack = 0;
                             engine().removeTrack(set, state_index, track);
                             floater.rebuildTrackSelector();
                             floater.refreshAnimations();
                             floater.updateCycleValues();
                         }
                     });
        }

        mContextMenuHandle = menu->getHandle();
        gMenuHolder->addChild(menu);

        S32 screen_x, screen_y;
        mAnimationList->localPointToScreen(x, y, &screen_x, &screen_y);
        menu->show(screen_x, screen_y, mAnimationList);
    }

    // ------------------------------------------------------------------------
    // Files

    void AOFloater::onClickExport(bool all)
    {
        std::vector<U32> serials;
        std::string      name;
        if (all)
        {
            for (const AOEngine::set_ptr_t& set : engine().getSets())
            {
                serials.push_back(set->getSerial());
            }
            name = getString("ao_export_all_name");
        }
        else if (const AOSet* set = selectedSet())
        {
            serials.push_back(set->getSerial());
            name = set->getName();
        }
        if (serials.empty())
        {
            return;
        }

        LLFilePickerReplyThread::startPicker(
            [serials](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
            {
                if (files.empty() || !AOEngine::instanceExists())
                {
                    return;
                }
                std::vector<const AOSet*> sets;
                for (U32 serial : serials)
                {
                    if (const AOSet* set = AOEngine::instance().findSet(serial))
                    {
                        sets.push_back(set);
                    }
                }
                LLSD args;
                args["FILE"]  = files.front();
                args["COUNT"] = (S32)sets.size();
                const bool ok = !sets.empty() && exportSets(sets, files.front());
                notifyUser(ok ? "ao_export_done" : "ao_export_failed", args, !ok);
            },
            LLFilePicker::FFSAVE_XML, exportFileName(name));
    }

    void AOFloater::onClickImport()
    {
        LLFilePickerReplyThread::startPicker(
            [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
            {
                if (!files.empty() && AOEngine::instanceExists())
                {
                    importFile(files.front());
                }
            },
            LLFilePicker::FFLOAD_XML, false);
    }

    // ------------------------------------------------------------------------
    // Full and small interface (Firestorm)

    void AOFloater::onClickMore()
    {
        LLRect full_size = gSavedPerAccountSettings.getRect(SETTING_FULL_RECT);
        if (full_size.getHeight() < getMinHeight())
        {
            full_size.setOriginAndSize(full_size.mLeft, full_size.mBottom, full_size.getWidth(), getRect().getHeight());
        }
        if (full_size.getWidth() < getMinWidth())
        {
            full_size.setOriginAndSize(full_size.mLeft, full_size.mBottom, getRect().getWidth(), full_size.getHeight());
        }
        // A saved (or hand-edited) height taller than the window would put the
        // floater's bottom out of reach.
        if (gFloaterView && full_size.getHeight() > gFloaterView->getRect().getHeight())
        {
            full_size.setOriginAndSize(full_size.mLeft, full_size.mBottom, full_size.getWidth(),
                                       llmax(gFloaterView->getRect().getHeight(), getMinHeight()));
        }

        mMore = true;
        mSmallPanel->setVisible(false);
        mMainPanel->setVisible(true);
        setCanResize(true);
        gSavedPerAccountSettings.setBOOL(SETTING_FULL_INTERFACE, true);

        reshape(getRect().getWidth(), full_size.getHeight());
    }

    void AOFloater::onClickLess()
    {
        const LLRect full_size  = getRect();
        LLRect       small_size = mSmallPanel->getRect();
        small_size.setLeftTopAndSize(0, 0, small_size.getWidth(), small_size.getHeight() + getHeaderHeight());

        // Remember the full size for onClickMore().
        if (mMore)
        {
            gSavedPerAccountSettings.setRect(SETTING_FULL_RECT, full_size);
        }

        mMore = false;
        mSmallPanel->setVisible(true);
        mMainPanel->setVisible(false);
        setCanResize(false);
        gSavedPerAccountSettings.setBOOL(SETTING_FULL_INTERFACE, false);

        reshape(getRect().getWidth(), small_size.getHeight());
    }

    // ------------------------------------------------------------------------
    // Drag and drop

    bool AOFloater::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                      EAcceptance* accept, std::string& tooltip_msg)
    {
        // No drag and drop on the small interface.
        if (!mMore)
        {
            tooltip_msg = getString("ao_dnd_only_on_full_interface");
            *accept     = ACCEPT_NO;
            return true;
        }

        const LLInventoryItem* item = static_cast<const LLInventoryItem*>(cargo_data);
        if (!engine().isLoaded() || !item)
        {
            *accept = ACCEPT_NO;
            return true;
        }

        if (cargo_type == DAD_NOTECARD)
        {
            *accept = ACCEPT_YES_SINGLE;
            if (drop)
            {
                importNotecard(gInventory.getItem(item->getUUID()));
            }
        }
        else if (cargo_type == DAD_ANIMATION || cargo_type == DAD_CATEGORY)
        {
            if (!selectedSet())
            {
                *accept = ACCEPT_NO;
                return true;
            }
            *accept = cargo_type == DAD_CATEGORY ? ACCEPT_YES_SINGLE : ACCEPT_YES_MULTI;
            if (drop)
            {
                // The row under the cursor: the list finds it by selecting it.
                S32 list_x, list_y;
                const Row* target = nullptr;
                if (localPointToOtherView(x, y, &list_x, &list_y, mAnimationList) && mAnimationList->pointInView(list_x, list_y))
                {
                    mAnimationList->deselectAllItems(true);
                    if (mAnimationList->selectItemAt(list_x, list_y, MASK_NONE))
                    {
                        target = rowOf(mAnimationList->getFirstSelected());
                    }
                }
                if (cargo_type == DAD_CATEGORY)
                {
                    dropFolder(static_cast<const LLInventoryCategory*>(cargo_data)->getUUID(), target);
                }
                else
                {
                    queueDrop(item->getUUID(), target);
                }
            }
        }
        else
        {
            *accept = ACCEPT_NO;
        }
        return true;
    }

    void AOFloater::queueDrop(const LLUUID& item_id, const Row* target)
    {
        if (mDropItems.empty())
        {
            mDropSerial = mSelectedSerial;
            mDropState  = mSelectedState;
            mDropTrack  = inTrackView() ? mCurrentTrack - 1 : -1;
            mDropGroup  = -1;
            mDropMerge  = -1;
            if (mDropTrack < 0 && target)
            {
                // On a single animation: they play together as a group. On a group or one of its members: they join it.
                if (target->mKind == 's')
                {
                    mDropMerge = target->mIndex;
                }
                else
                {
                    mDropGroup = target->mIndex;
                }
            }
        }
        mDropItems.push_back(item_id);

        if (!mDropScheduled)
        {
            mDropScheduled = true;
            LLHandle<LLFloater> handle = getHandle();
            doOnIdleOneTime([handle]()
            {
                if (AOFloater* floater = static_cast<AOFloater*>(handle.get()))
                {
                    floater->flushDrops();
                }
            });
        }
    }

    void AOFloater::dropFolder(const LLUUID& folder, const Row*)
    {
        AOSet* set = selectedSet();
        if (!set)
        {
            return;
        }

        if (!inTrackView())
        {
            // The folder's animations play together as a group.
            if (!engine().createGroupFromFolder(set, mSelectedState, folder))
            {
                notifyUser("ao_folder_not_animations", LLSD());
            }
            return;
        }

        // On a track: the animations of the folder join its list.
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
            notifyUser("ao_folder_not_animations", LLSD());
            return;
        }
        std::stable_sort(animations.begin(), animations.end(),
                         [](const LLPointer<LLViewerInventoryItem>& a, const LLPointer<LLViewerInventoryItem>& b)
                         { return LLStringUtil::compareInsensitive(a->getName(), b->getName()) < 0; });
        ListRef list;
        list.mTrack = mCurrentTrack - 1;
        engine().addAnimations(set, mSelectedState, animations, list);
    }

    void AOFloater::flushDrops()
    {
        mDropScheduled = false;

        std::vector<LLPointer<LLViewerInventoryItem>> items;
        for (const LLUUID& id : mDropItems)
        {
            if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                items.push_back(item);
            }
        }
        mDropItems.clear();

        if (AOSet* set = engine().findSet(mDropSerial))
        {
            if (mDropMerge >= 0)
            {
                engine().createGroup(set, mDropState, (size_t)mDropMerge, items);
                return;
            }
            ListRef list;
            list.mTrack = mDropTrack;
            list.mGroup = mDropGroup;
            engine().addAnimations(set, mDropState, items, list);
        }
    }
}
