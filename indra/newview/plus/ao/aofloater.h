/**
 * @file aofloater.h
 * @brief Animation Overrider: the window (floater_plus_ao.xml).
 *
 * Ported from Firestorm's ao.h (FloaterAO, see SOURCE.md). The window shows
 * the engine's sets as they are in memory, so every change shows at once;
 * there is no "reloading" cover any more. Several animations dropped at once
 * are added as one batch.
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

#ifndef PLUS_AO_AOFLOATER_H
#define PLUS_AO_AOFLOATER_H

#include "llfloater.h"
#include "lluuid.h"

#include <boost/signals2.hpp>
#include <set>
#include <vector>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLContextMenu;
class LLFloaterReg;
class LLMenuButton;
class LLMenuItemCallGL;
class LLPanel;
class LLScrollListItem;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;

namespace plus::ao
{
    class AOSet;
    struct AOAnimation;
    struct AOState;
    struct ListRef;

    class AOFloater : public LLFloater
    {
        friend class ::LLFloaterReg;

    public:
        static constexpr const char* NAME = "plus_ao";

        bool postBuild() override;

        bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                               EAcceptance* accept, std::string& tooltip_msg) override;

    private:
        explicit AOFloater(const LLSD& key);
        ~AOFloater() override;

        AOSet* selectedSet() const;

        // From the engine's signals.
        void refreshSets();
        void refreshSetParameters();
        void refreshStates();
        void refreshAnimations();
        void refreshPlaying();
        void refreshStatus();
        void onStateChanged(U32 set_serial, S32 state);

        void onSelectSet();
        void onSelectSetSmall();
        void renameSet();
        void onSelectState();
        void onChangeAnimationSelection();
        void onClickActivate();
        void buildAddMenu();
        void onAddMenuShow();
        void onAddSet(bool clone);
        void onClickRemove();
        void onClickReload();
        void onCheckDefault();
        void onCheckOverrideSits(LLCheckBoxCtrl* source);
        void onCheckSmart();
        void onCheckDisableStands();
        void onClickMoveUp();
        void onClickMoveDown();
        void onClickTrash();
        void onCheckCycle();
        void onCheckRandomize();
        void onChangeCycleTime();
        void onClickPrevious();
        void onClickNext();
        void onDoubleClick();
        void onClickExport(bool all);
        void onClickImport();
        void onClickFsImport();
        void onClickMore();
        void onClickLess();

        // One row of the animation list (the list's value is the row's place in mRows):
        // 's' an animation of the main list, 'g' a group, 'm' a member of a group, 't' an animation of the track.
        struct Row
        {
            char mKind = 's';
            S32  mIndex = 0;   // the place in the main list (the group for 'm'), or in the track
            S32  mMember = -1; // 'm': the place in the group
        };
        const Row* rowOf(const LLScrollListItem* item) const;
        // The entry the row shows in the selected state (its track for 't'), or null when the row is out of the lists.
        const AOAnimation* animOf(const Row& row) const;
        // What a row stands for when a context menu or the rename dialog is opened on it: the set, the state, the track,
        // the row and the entry it showed. Their actions are taken only while the row still shows that entry: the list
        // is rebuilt meanwhile by edits and by the inventory coming in, and the row's places then name another one.
        struct RowTarget
        {
            U32         mSerial = 0;
            S32         mState  = 0;
            S32         mTrack  = 0;
            Row         mRow;
            LLUUID      mItem;      // an animation's item (the link's original: the link is null until the server made it)
            U32         mKey = 0;   // a group's key (its folder too is null until the server made it)
            std::string mName;
        };
        RowTarget targetOf(const Row& row) const;
        bool      stillShows(const RowTarget& target) const;
        void addRow(const std::string& icon, const std::string& name, const Row& row, bool italic_hint);
        void selectRow(const Row& row);
        // Which list of the state an edit of the row is about.
        ListRef listOf(const Row& row) const;
        // The selected track (null on the main list).
        const AOState* selectedState() const;
        bool           inTrackView() const;
        void rebuildTrackSelector();
        void updateCycleValues();
        void onSelectTrackTab();
        void onClickAddTrack();
        void onClickRename();
        void renameGroup(const Row& row);
        void renameGroupCallback(const LLSD& notification, const LLSD& response);
        void onAnimationListRightClick(S32 x, S32 y);
        void showContextMenu(S32 x, S32 y);
        void toggleGroup(S32 index);
        void deleteRows(const std::vector<Row>& rows);

        void moveSelected(bool up);
        void updateCycleParameters();
        void enableControls();

        // Drops of several animations arrive one by one in the same frame; they
        // are added together on the next idle (to the list or group under the cursor, or grouped
        // with the animation under it: mDropMerge).
        void queueDrop(const LLUUID& item_id, const Row* target);
        void flushDrops();
        // An inventory folder dropped on the list.
        void dropFolder(const LLUUID& folder, const Row* target);

        U32  mSelectedSerial = 0; // the set being edited (AOSet::getSerial())
        S32  mSelectedState  = 0; // EState
        bool mMore           = false; // postBuild() picks the interface; false so onClickLess() there does not save the XML rect as the full size

        std::vector<LLUUID> mDropItems;
        U32                 mDropSerial = 0;
        S32                 mDropState  = 0;
        S32                 mDropTrack  = -1;
        S32                 mDropGroup  = -1;
        S32                 mDropMerge  = -1;
        bool                mDropScheduled = false;

        std::vector<Row>  mRows;
        S32               mCurrentTrack = 0; // 0: the main list; N: track N-1
        // The track shown when the list was last made (key 0: the main list), so that it is found again when the
        // state's tracks change (a reading puts them in order, gives them new keys: then by the name). research/275 L098
        U32               mCurrentTrackKey = 0;
        std::string       mCurrentTrackName;
        std::set<LLUUID>  mCollapsed;        // folders of the groups whose members are hidden
        LLHandle<LLContextMenu> mContextMenuHandle;
        LLMenuItemCallGL* mCloneSetItem = nullptr;
        LLComboBox*       mTrackSelector = nullptr;
        LLButton*         mTrackAddButton = nullptr;
        LLButton*         mRenameButton = nullptr;

        LLPanel* mMainPanel  = nullptr;
        LLPanel* mSmallPanel = nullptr;

        // Full interface
        LLCheckBoxCtrl*   mEnabledCheckBox = nullptr;
        LLComboBox*       mSetSelector = nullptr;
        LLButton*         mActivateButton = nullptr;
        LLMenuButton*     mAddButton = nullptr;
        LLButton*         mRemoveButton = nullptr;
        LLCheckBoxCtrl*   mDefaultCheckBox = nullptr;
        LLCheckBoxCtrl*   mOverrideSitsCheckBox = nullptr;
        LLCheckBoxCtrl*   mSmartCheckBox = nullptr;
        LLCheckBoxCtrl*   mDisableMouselookCheckBox = nullptr;
        LLComboBox*       mStateSelector = nullptr;
        LLScrollListCtrl* mAnimationList = nullptr;
        LLButton*         mMoveUpButton = nullptr;
        LLButton*         mMoveDownButton = nullptr;
        LLButton*         mTrashButton = nullptr;
        LLCheckBoxCtrl*   mCycleCheckBox = nullptr;
        LLCheckBoxCtrl*   mRandomizeCheckBox = nullptr;
        LLTextBox*        mCycleTimeLabel = nullptr;
        LLSpinCtrl*       mCycleTimeSpinner = nullptr;
        LLButton*         mImportButton = nullptr;
        LLButton*         mExportButton = nullptr;
        LLButton*         mExportAllButton = nullptr;
        LLButton*         mReloadButton = nullptr;
        LLButton*         mFsImportButton = nullptr;
        LLButton*         mPreviousButton = nullptr;
        LLButton*         mNextButton = nullptr;
        LLTextBox*        mStatusText = nullptr;

        // Small interface
        LLCheckBoxCtrl* mEnabledCheckBoxSmall = nullptr;
        LLComboBox*     mSetSelectorSmall = nullptr;
        LLButton*       mPreviousButtonSmall = nullptr;
        LLButton*       mNextButtonSmall = nullptr;
        LLCheckBoxCtrl* mOverrideSitsCheckBoxSmall = nullptr;

        boost::signals2::scoped_connection mSetsConnection;
        boost::signals2::scoped_connection mStateConnection;
        boost::signals2::scoped_connection mAnimationConnection;
        boost::signals2::scoped_connection mStatusConnection;
    };
}

#endif // PLUS_AO_AOFLOATER_H
