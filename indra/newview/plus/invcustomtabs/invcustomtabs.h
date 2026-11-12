/**
 * @file invcustomtabs.h
 * @brief Named filter tabs in the inventory window (research/314).
 *
 * Ported from Firestorm's fsinventorycustomtabs.{h,cpp}: "User-defined inventory
 * filter tabs" (FIRE-35598) and hiding / showing them (FIRE-36831). The idea of a
 * tabbed inventory comes from the Catznip viewer.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * https://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef PLUS_INVCUSTOMTABS_H
#define PLUS_INVCUSTOMTABS_H

#include "llhandle.h"
#include "llnotificationptr.h"
#include "llsd.h"

#include <boost/signals2/connection.hpp>
#include <set>
#include <string>
#include <unordered_set>

class LLButton;
class LLContextMenu;
class LLFilterEditor;
class LLInventoryFilter;
class LLInventoryPanel;
class LLPanel;
class LLPanelMainInventory;
class LLTabContainer;
class LLView;

// One per inventory window (LLPanelMainInventory owns it). Custom tabs are
// LLInventoryPanels inserted before two button tabs at the end of the window's
// tab container: the hidden-tabs arrow (shown while a tab is hidden) and "+".
// The button tabs are LLInventoryPanels too (the window casts whatever tab is
// current to one), but they are never shown: a press on them is taken before
// the tab container sees it, and a selection by other means goes back to a
// real tab. A custom tab keeps its own search words; the official tabs keep
// sharing theirs (LLPanelMainInventory::mFilterSubString).
// Saved per account in FSInventoryCustomTabs; every inventory window follows
// a change another one saves. Everything runs on the UI thread.
class PlusInvCustomTabs : public LLHandleProvider<PlusInvCustomTabs>
{
    LOG_CLASS(PlusInvCustomTabs);

public:
    explicit PlusInvCustomTabs(LLPanelMainInventory* parent);
    ~PlusInvCustomTabs();

    // Hooks from LLPanelMainInventory. Each one does nothing for a window
    // without an instance (before postBuild() made it).
    static void registerCommitCallbacks(LLPanelMainInventory* parent);
    static void install(LLPanelMainInventory* parent, LLFilterEditor* filter_editor);
    // A pointer lookup: the window's draw() asks every frame.
    static bool activePanelIsCustom(const LLPanelMainInventory* parent);
    // True when the current tab is a button tab: another tab was selected instead.
    static bool redirectButtonTabSelection(LLPanelMainInventory* parent);
    static void noteActivePanel(LLPanelMainInventory* parent, LLInventoryPanel* panel);
    static void notifyFilterStateChanged(LLPanelMainInventory* parent);
    static S32  captureFilterGeneration(const LLPanelMainInventory* parent);
    static void notifyIfFilterChanged(LLPanelMainInventory* parent, S32 prev_generation);
    static bool handleRightMouseDown(LLPanelMainInventory* parent, S32 x, S32 y);
    static bool handleMouseDown(LLPanelMainInventory* parent, S32 x, S32 y);
    static bool handleDoubleClick(LLPanelMainInventory* parent, S32 x, S32 y);
    static bool handleDragAndDrop(LLPanelMainInventory* parent, S32 x, S32 y);
    static void onParentDraw(LLPanelMainInventory* parent);
    // After a tab change that enters or leaves a custom tab; not while tabs are being loaded.
    static void syncReceivedItemsOnTabChange(LLPanelMainInventory* parent);
    // The label of the custom tab that filters with this filter (the finder's title).
    static bool findTabLabel(const LLPanelMainInventory* parent, const LLInventoryFilter* filter, std::string& label);

private:
    typedef void (PlusInvCustomTabs::*answer_fn_t)(const LLSD& notification, const LLSD& response);

    static PlusInvCustomTabs* instanceFor(LLPanelMainInventory* parent);
    static const PlusInvCustomTabs* instanceFor(const LLPanelMainInventory* parent);

    // Mouse and drag over the tab strip
    bool onRightMouseDown(S32 x, S32 y);
    bool onMouseDown(S32 x, S32 y);
    bool pressButtonTab(S32 x, S32 y);
    bool onDoubleClick(S32 x, S32 y);
    LLInventoryPanel* hitTestCloseIcon(S32 x, S32 y) const;
    bool onDragAndDrop(S32 x, S32 y);
    LLPanel* tabAt(S32 x, S32 y, S32* tab_local_x) const;
    LLInventoryPanel* hitTestCustomTab(S32 x, S32 y, S32& tab_local_x) const;
    LLInventoryPanel* hitTestButtonTab(S32 x, S32 y) const;

    // Following the window
    void noteActive(LLInventoryPanel* panel);
    void notifyActiveFilterStateChanged();
    void ensureRootOpenForActivePanel();
    void attachToFilterEditor(LLFilterEditor* filter_editor);
    void onFilterFocusLost();

    // The button tabs
    void installButtonTabs();
    LLInventoryPanel* addButtonTab(const char* name, const std::string& label);
    void leaveButtonTab();
    void onAddClicked();
    void onShowHiddenClicked();
    LLContextMenu* buildHiddenTabsMenu();
    void updateShowHiddenTabVisibility();

    // The custom tabs
    LLInventoryPanel* addTab(const std::string& explicit_name, const std::string& filter, bool select);
    void decorateCustomTabButton(LLInventoryPanel* panel);
    void onRenameClicked();
    void onRenameConfirmed(const LLSD& notification, const LLSD& response);
    void onCloneClicked();
    void onClosePressed(LLInventoryPanel* panel);
    void onCloseClicked();
    void onCloseConfirmed(const LLSD& notification, const LLSD& response);
    void onHideClicked();
    void hideTab(LLInventoryPanel* panel);
    void showTab(LLInventoryPanel* panel);
    void selectNearestVisibleTab(LLInventoryPanel* skip_panel);
    void clearCustomTabs();
    // Asks about the context tab; the answer goes to on_answer while this lives.
    void askAboutContextTab(const char* notification_name, LLNotificationPtr& pending, answer_fn_t on_answer);
    LLInventoryPanel* customTabNamedIn(const LLSD& notification) const;

    // Labels
    void applyTabLabel(LLInventoryPanel* panel, const std::string& label);
    void updateAutoLabel(LLInventoryPanel* panel);
    std::string computeDisplayLabel(LLInventoryPanel* panel) const;
    static std::string labelForWords(const std::string& words);
    static std::string getDefaultTabName();
    static std::string sanitizeTabName(std::string name);

    // Persistence
    void load();
    void doLoad();
    void applySavedStates(LLInventoryPanel* panel, const LLSD& states, const std::string& filter);
    void save();
    LLSD savedEntry(LLInventoryPanel* panel) const;
    bool isUnchangedSetting(const LLSD& array) const;
    void subscribeToSetting();
    void onSettingChangedExternally(const LLSD& value, const LLSD& previous);
    void rebuildTabs(const LLSD& value, const LLSD& previous);
    void syncReceivedItems();
    bool updateTabsInPlace(const LLSD& value, const LLSD& previous);
    S32 customTabIndex(const LLInventoryPanel* panel) const;
    LLInventoryPanel* customTabAt(S32 index) const;

    bool isCustomTab(const LLPanel* panel) const;
    bool isCustomPanel(const LLInventoryPanel* panel) const;
    bool isAddTab(const LLPanel* panel) const;
    bool isShowHiddenTab(const LLPanel* panel) const;
    LLButton* findTabButton(const LLPanel* panel) const;

    LLPanelMainInventory* mParent{ nullptr };
    LLTabContainer* mTabs{ nullptr };
    LLHandle<LLView> mMenuHandle;                           // the custom tabs' right-click menu
    LLHandle<LLContextMenu> mHiddenMenuHandle;              // the arrow's list, made again at each press
    std::set<LLInventoryPanel*> mPanels;                    // the custom tabs
    std::unordered_set<LLInventoryPanel*> mExplicitlyNamed; // renamed by hand (else named after their words)
    std::unordered_set<LLInventoryPanel*> mPendingRootOpen; // open My Inventory when first shown
    std::unordered_set<LLInventoryPanel*> mHidden;
    LLInventoryPanel* mContextPanel{ nullptr };             // the tab the right-click menu is for
    LLInventoryPanel* mAddTabPanel{ nullptr };
    LLInventoryPanel* mShowHiddenTabPanel{ nullptr };
    LLInventoryPanel* mLastActivePanel{ nullptr };          // the last real tab shown
    LLNotificationPtr mRenameNotification;
    LLNotificationPtr mCloseNotification;
    boost::signals2::connection mSettingConnection;
    boost::signals2::connection mValidateConnection;        // the tab container may not select a button tab
    bool mSaving{ false };   // our own write: the setting's signal is not a change from elsewhere
    bool mLoading{ false };  // building tabs from the setting: nothing is saved meanwhile
    bool mLoadedOk{ false }; // the last load made every saved tab it read (the first MAX_TABS): saving would not lose one
    bool mSwallowDoubleClick{ false }; // the last press closed a tab: its double click does nothing (cleared at the next press)
};

#endif // PLUS_INVCUSTOMTABS_H
