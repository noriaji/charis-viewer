/**
 * @file invcustomtabs.cpp
 * @brief Named filter tabs in the inventory window (research/314).
 *
 * Ported from Firestorm's fsinventorycustomtabs.{h,cpp}: "User-defined inventory
 * filter tabs" (FIRE-35598) and hiding / showing them (FIRE-36831). The idea of a
 * tabbed inventory comes from the Catznip viewer. What was changed for this viewer
 * is in ORIGIN.md next to this file.
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

#include "llviewerprecompiledheaders.h"

#include "invcustomtabs.h"

#include "llbutton.h"
#include "llfiltereditor.h"
#include "llfolderviewitem.h"
#include "llinventoryfilter.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llmenugl.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llpanelmaininventory.h"
#include "llpanelmarketplaceinbox.h"
#include "llpermissionsflags.h"
#include "llsdparam.h"
#include "llsdutil.h"
#include "llsidepanelinventory.h"
#include "llstring.h"
#include "lltabcontainer.h"
#include "lltrans.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"

namespace
{
    // Firestorm's names for what can come from Firestorm or go into translations: the setting
    // (its tabs come over as they are), the strings and the notifications.
    constexpr char SETTING_KEY[]         = "FSInventoryCustomTabs";
    constexpr char DEFAULT_NAME_KEY[]    = "FSInventoryCustomTabDefaultName";
    constexpr char SHOW_HIDDEN_TIP_KEY[] = "FSInventoryCustomTabShowHidden";
    constexpr char RENAME_NOTIFY[]       = "FSInventoryCustomTabRename";
    constexpr char CLOSE_NOTIFY[]        = "FSInventoryCustomTabClose";
    constexpr char TABHIDDEN_KEY[]       = "tabhidden";
    // Ours: names inside the window and the menu file.
    constexpr char MENU_FILE[]              = "menu_inventory_custom_tab.xml";
    constexpr char ADD_TAB_PANEL_NAME[]     = "PlusInvCustomTabAdd";
    constexpr char SHOW_HIDDEN_PANEL_NAME[] = "PlusInvCustomTabShowHidden";
    constexpr char HIDDEN_MENU_NAME[]       = "plus_inv_hidden_tabs_menu";
    constexpr char CUSTOM_PANEL_FORMAT[]    = "PlusInvCustomTab_%d";
    constexpr char CLOSE_BUTTON_PREFIX[]    = "plus_inv_tab_close_";
    constexpr char TAB_BUTTON_PREFIX[]      = "htab_"; // LLTabContainer's name for a tab's button
    constexpr char ADD_TAB_LABEL[]          = "+";
    constexpr char FALLBACK_LABEL[]         = "Custom";
    constexpr char CLOSE_ICON_NAME[]        = "Icon_Close_Foreground";
    constexpr char CLOSE_ICON_PRESS[]       = "Icon_Close_Press";
    constexpr char SHOW_HIDDEN_ICON_NAME[]  = "Arrow_Down";
    constexpr S32  CLOSE_ICON_SIZE      = 16;
    constexpr S32  CLOSE_ICON_MARGIN    = 4;
    constexpr S32  CLOSE_BUTTON_RESERVE = CLOSE_ICON_SIZE + CLOSE_ICON_MARGIN + 2;
    constexpr S32  BUTTON_TAB_MIN_WIDTH = 18;
    constexpr S32  BUTTON_TAB_H_PAD     = 2;
    constexpr size_t MAX_TABS           = 64;
    constexpr size_t MAX_NAME_LENGTH    = 128;
    constexpr S32  MAX_FILTER_CHARS     = 300; // the search box's max_length_chars (panel_main_inventory.xml)
    // A saved "hours ago" above this (a hand-edited file) would overflow the filter's
    // hours * 3600 seconds; 100 years is more than the finder can set.
    constexpr U32  MAX_HOURS_AGO        = 24 * 365 * 100;

    std::set<PlusInvCustomTabs*>& allInstances()
    {
        static std::set<PlusInvCustomTabs*> s;
        return s;
    }

    // An enum value from the settings file: an integer within [lo, hi], else not taken.
    bool readEnum(const LLSD& value, S32 lo, S32 hi, S32& out)
    {
        if (!value.isInteger())
        {
            return false;
        }
        const S32 v = value.asInteger();
        if (v < lo || v > hi)
        {
            return false;
        }
        out = v;
        return true;
    }

    void cancelNotification(LLNotificationPtr& notification)
    {
        if (notification)
        {
            LLNotifications::instance().cancel(notification);
            notification.reset();
        }
    }

    // Sets a flag for the scope and puts back what it was on every way out.
    class FlagScope
    {
    public:
        explicit FlagScope(bool& flag) : mFlag(flag), mWas(flag) { mFlag = true; }
        ~FlagScope() { mFlag = mWas; }
        FlagScope(const FlagScope&) = delete;
        FlagScope& operator=(const FlagScope&) = delete;
    private:
        bool& mFlag;
        bool  mWas;
    };

    // Two saved entries make the same tab: the name, the words and the filter (not
    // whether it is hidden).
    bool sameTab(const LLSD& a, const LLSD& b)
    {
        return a["name"].asString() == b["name"].asString()
            && a["filter"].asString() == b["filter"].asString()
            && llsd_equals(a["states"], b["states"]);
    }

    // doLoad() makes a tab of each map entry in order, up to MAX_TABS: the entry of the
    // tab_index-th tab, and the tab of an entry (-1 when there is none).
    S32 entryOfTab(const LLSD& array, S32 tab_index)
    {
        if (!array.isArray() || tab_index < 0 || tab_index >= static_cast<S32>(MAX_TABS))
        {
            return -1;
        }
        S32 tabs = 0;
        for (S32 i = 0; i < static_cast<S32>(array.size()); ++i)
        {
            if (array[i].isMap() && tabs++ == tab_index)
            {
                return i;
            }
        }
        return -1;
    }

    S32 tabOfEntry(const LLSD& array, S32 entry_index)
    {
        if (!array.isArray() || entry_index < 0 || entry_index >= static_cast<S32>(array.size()) || !array[entry_index].isMap())
        {
            return -1;
        }
        S32 tabs = 0;
        for (S32 i = 0; i < entry_index; ++i)
        {
            if (array[i].isMap())
            {
                ++tabs;
            }
        }
        return tabs < static_cast<S32>(MAX_TABS) ? tabs : -1;
    }

    // The words of a saved entry as the search box would take them: no emoji, at most
    // its 300 characters.
    std::string savedWords(const LLSD& entry)
    {
        std::string words = entry["filter"].asString();
        utf8str_remove_emojis(words);
        return utf8str_symbol_truncate(words, MAX_FILTER_CHARS);
    }

    template <typename T> void killMenu(LLHandle<T>& handle)
    {
        if (T* menu = handle.get())
        {
            menu->die();
            handle.markDead();
        }
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PlusInvCustomTabs::PlusInvCustomTabs(LLPanelMainInventory* parent)
    : mParent(parent)
{
    if (!mParent)
    {
        return;
    }

    mTabs = mParent->mFilterTabs;
    if (!mTabs)
    {
        return;
    }

    if (gMenuHolder)
    {
        if (auto* menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>(MENU_FILE, gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance()))
        {
            mMenuHandle = menu->getHandle();
        }
    }

    installButtonTabs();
    allInstances().insert(this);
}

PlusInvCustomTabs::~PlusInvCustomTabs()
{
    allInstances().erase(this);
    mSettingConnection.disconnect();
    cancelNotification(mRenameNotification);
    cancelNotification(mCloseNotification);
    mValidateConnection.disconnect();
    if (mTabs && !mPanels.empty())
    {
        save();
    }
    killMenu(mMenuHandle);
    killMenu(mHiddenMenuHandle);
}

// ---------------------------------------------------------------------------
// Hooks called from LLPanelMainInventory
// ---------------------------------------------------------------------------

void PlusInvCustomTabs::registerCommitCallbacks(LLPanelMainInventory* parent)
{
    if (!parent)
    {
        return;
    }
    auto& reg = parent->getCommitCallbackRegistrar();
    reg.add("PlusInvCustomTab.Rename", [parent](LLUICtrl*, const LLSD&) { if (auto* self = instanceFor(parent)) self->onRenameClicked(); });
    reg.add("PlusInvCustomTab.Clone",  [parent](LLUICtrl*, const LLSD&) { if (auto* self = instanceFor(parent)) self->onCloneClicked();  });
    reg.add("PlusInvCustomTab.Close",  [parent](LLUICtrl*, const LLSD&) { if (auto* self = instanceFor(parent)) self->onCloseClicked();  });
    reg.add("PlusInvCustomTab.Hide",   [parent](LLUICtrl*, const LLSD&) { if (auto* self = instanceFor(parent)) self->onHideClicked();   });
}

void PlusInvCustomTabs::install(LLPanelMainInventory* parent, LLFilterEditor* filter_editor)
{
    if (!parent)
    {
        return;
    }
    parent->mCustomTabs = std::make_unique<PlusInvCustomTabs>(parent);
    parent->mCustomTabs->load();
    parent->mCustomTabs->attachToFilterEditor(filter_editor);
}

bool PlusInvCustomTabs::activePanelIsCustom(const LLPanelMainInventory* parent)
{
    const auto* self = instanceFor(parent);
    return self && self->isCustomPanel(parent->getActivePanel());
}

bool PlusInvCustomTabs::redirectButtonTabSelection(LLPanelMainInventory* parent)
{
    auto* self = instanceFor(parent);
    if (!self || !parent->mFilterTabs)
    {
        return false;
    }
    const LLPanel* current = parent->mFilterTabs->getCurrentPanel();
    if (!self->isAddTab(current) && !self->isShowHiddenTab(current))
    {
        return false;
    }
    self->leaveButtonTab();
    return true;
}

void PlusInvCustomTabs::noteActivePanel(LLPanelMainInventory* parent, LLInventoryPanel* panel)
{
    if (auto* self = instanceFor(parent))
    {
        self->noteActive(panel);
    }
}

void PlusInvCustomTabs::notifyFilterStateChanged(LLPanelMainInventory* parent)
{
    if (auto* self = instanceFor(parent))
    {
        self->notifyActiveFilterStateChanged();
    }
}

S32 PlusInvCustomTabs::captureFilterGeneration(const LLPanelMainInventory* parent)
{
    if (!parent)
    {
        return 0;
    }
    const auto* active = parent->getActivePanel();
    return active ? active->getFilter().getCurrentGeneration() : 0;
}

void PlusInvCustomTabs::notifyIfFilterChanged(LLPanelMainInventory* parent, S32 prev_generation)
{
    auto* self = instanceFor(parent);
    if (!self)
    {
        return;
    }
    const auto* active = parent->getActivePanel();
    if (active && active->getFilter().getCurrentGeneration() != prev_generation)
    {
        self->notifyActiveFilterStateChanged();
    }
}

bool PlusInvCustomTabs::handleRightMouseDown(LLPanelMainInventory* parent, S32 x, S32 y)
{
    auto* self = instanceFor(parent);
    return self && self->onRightMouseDown(x, y);
}

bool PlusInvCustomTabs::handleMouseDown(LLPanelMainInventory* parent, S32 x, S32 y)
{
    auto* self = instanceFor(parent);
    return self && self->onMouseDown(x, y);
}

bool PlusInvCustomTabs::handleDoubleClick(LLPanelMainInventory* parent, S32 x, S32 y)
{
    auto* self = instanceFor(parent);
    return self && self->onDoubleClick(x, y);
}

bool PlusInvCustomTabs::handleDragAndDrop(LLPanelMainInventory* parent, S32 x, S32 y)
{
    auto* self = instanceFor(parent);
    return self && self->onDragAndDrop(x, y);
}

void PlusInvCustomTabs::onParentDraw(LLPanelMainInventory* parent)
{
    if (auto* self = instanceFor(parent))
    {
        self->ensureRootOpenForActivePanel();
    }
}

bool PlusInvCustomTabs::findTabLabel(const LLPanelMainInventory* parent, const LLInventoryFilter* filter, std::string& label)
{
    const auto* self = instanceFor(parent);
    if (!self || !filter)
    {
        return false;
    }
    for (const LLInventoryPanel* panel : self->mPanels)
    {
        if (&panel->getFilter() == filter)
        {
            label = panel->getLabel();
            return true;
        }
    }
    return false;
}

void PlusInvCustomTabs::syncReceivedItemsOnTabChange(LLPanelMainInventory* parent)
{
    auto* self = instanceFor(parent);
    if (self && !self->mLoading)
    {
        self->syncReceivedItems();
    }
}

// Received Items (the inbox under the inventory) is searched with the words of the tab
// shown: a custom tab's own, else the official tabs' shared ones. Compared with the words
// it really has (a copy taken earlier can be wrong: Find Links changes the shared words
// without it, a gallery search returns early, the inbox list can be made later): calling
// its onFilterEdit() with the same words again would close its folders and scroll it.
void PlusInvCustomTabs::syncReceivedItems()
{
    if (!mParent || !mParent->mInboxPanel || !mParent->mActivePanel || !mParent->mParentSidepanel)
    {
        return;
    }
    LLInventoryPanel* inbox = mParent->mParentSidepanel->getInboxPanel();
    if (!inbox)
    {
        return;
    }
    const std::string& words = isCustomPanel(mParent->mActivePanel) ? mParent->mActivePanel->getFilter().getFilterSubStringOrig()
                                                                      : mParent->mFilterSubString;
    if (words != inbox->getFilter().getFilterSubStringOrig())
    {
        mParent->mInboxPanel->onFilterEdit(words);
    }
}

PlusInvCustomTabs* PlusInvCustomTabs::instanceFor(LLPanelMainInventory* parent)
{
    return parent ? parent->mCustomTabs.get() : nullptr;
}

const PlusInvCustomTabs* PlusInvCustomTabs::instanceFor(const LLPanelMainInventory* parent)
{
    return parent ? parent->mCustomTabs.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Mouse and drag over the tab strip
// ---------------------------------------------------------------------------

bool PlusInvCustomTabs::onRightMouseDown(S32 x, S32 y)
{
    auto* menu = static_cast<LLMenuGL*>(mMenuHandle.get());
    if (!menu)
    {
        return false;
    }

    S32 local_x = 0;
    auto* inv_panel = hitTestCustomTab(x, y, local_x);
    if (!inv_panel)
    {
        return false;
    }

    mContextPanel = inv_panel;
    menu->buildDrawLabels();
    menu->updateParent(LLMenuGL::sMenuContainer);
    LLMenuGL::showPopup(mParent, menu, x, y);
    return true;
}

// "+" and the hidden-tabs arrow act on the press, before the tab container sees it.
bool PlusInvCustomTabs::pressButtonTab(S32 x, S32 y)
{
    auto* button_tab = hitTestButtonTab(x, y);
    if (!button_tab)
    {
        return false;
    }
    if (isAddTab(button_tab))
    {
        onAddClicked();
    }
    else
    {
        onShowHiddenClicked();
    }
    return true;
}

// The custom tab whose close icon (the right end of its button) is under the point, else null.
LLInventoryPanel* PlusInvCustomTabs::hitTestCloseIcon(S32 x, S32 y) const
{
    S32 local_x = 0;
    auto* inv_panel = hitTestCustomTab(x, y, local_x);
    if (!inv_panel)
    {
        return nullptr;
    }
    auto* tab_btn = findTabButton(inv_panel);
    if (!tab_btn)
    {
        return nullptr;
    }
    const S32 btn_local_x = local_x - tab_btn->getRect().mLeft;
    return btn_local_x < tab_btn->getRect().getWidth() - CLOSE_BUTTON_RESERVE ? nullptr : inv_panel;
}

bool PlusInvCustomTabs::onMouseDown(S32 x, S32 y)
{
    // A double click is only taken right after a press of ours that closed a tab (below).
    mSwallowDoubleClick = false;

    if (pressButtonTab(x, y))
    {
        return true;
    }
    if (auto* inv_panel = hitTestCloseIcon(x, y))
    {
        // With "do not ask again" the tab is gone at once, and another tab (or "+")
        // moved under the mouse: the second press of a double click must not act on it.
        // When the question is asked instead, its window takes the second press (and
        // one reaching us would land on the same close icon, which does nothing).
        onClosePressed(inv_panel);
        mSwallowDoubleClick = !mCloseNotification;
        return true;
    }
    return false;
}

// The second press of a double click comes here instead of onMouseDown(). On "+" and
// the arrow it acts like a press (on "+" it rarely hits "+" again: the new tab moved
// "+" to the right, so the second press usually selects the new tab, one tab in all).
// On a close icon, or right after a press that closed a tab, it does nothing.
bool PlusInvCustomTabs::onDoubleClick(S32 x, S32 y)
{
    if (mSwallowDoubleClick)
    {
        mSwallowDoubleClick = false;
        return true;
    }
    return pressButtonTab(x, y) || hitTestCloseIcon(x, y) != nullptr;
}

bool PlusInvCustomTabs::onDragAndDrop(S32 x, S32 y)
{
    // Firestorm's FIRE-36910: dragging over "+" (here also the arrow) would select
    // it; nothing can be dropped there.
    return hitTestButtonTab(x, y) != nullptr;
}

// The tab whose (visible) button is under the point of the parent, else null.
// Null while the tab strip is not shown (the single folder view hides it, and the
// press, the right click or the drop belongs to what is shown there).
LLPanel* PlusInvCustomTabs::tabAt(S32 x, S32 y, S32* tab_local_x) const
{
    if (!mTabs || !mParent || !mTabs->isInVisibleChain())
    {
        return nullptr;
    }
    S32 local_x = 0;
    S32 local_y = 0;
    if (!mParent->localPointToOtherView(x, y, &local_x, &local_y, mTabs))
    {
        return nullptr;
    }
    const S32 idx = mTabs->getTabContainedAtPoint(local_x, local_y);
    if (idx < 0)
    {
        return nullptr;
    }
    if (tab_local_x)
    {
        *tab_local_x = local_x;
    }
    return mTabs->getPanelByIndex(idx);
}

LLInventoryPanel* PlusInvCustomTabs::hitTestCustomTab(S32 x, S32 y, S32& tab_local_x) const
{
    tab_local_x = 0;
    if (mPanels.empty())
    {
        return nullptr;
    }
    auto* inv_panel = dynamic_cast<LLInventoryPanel*>(tabAt(x, y, &tab_local_x));
    return isCustomPanel(inv_panel) ? inv_panel : nullptr;
}

// "+" or the hidden-tabs arrow under the point, else null.
LLInventoryPanel* PlusInvCustomTabs::hitTestButtonTab(S32 x, S32 y) const
{
    if (!mAddTabPanel)
    {
        return nullptr;
    }
    const LLPanel* panel = tabAt(x, y, nullptr);
    if (isAddTab(panel))
    {
        return mAddTabPanel;
    }
    if (isShowHiddenTab(panel))
    {
        return mShowHiddenTabPanel;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Following the window
// ---------------------------------------------------------------------------

// The window shows another tab: the custom tab left gets its label from its words
// and is saved.
void PlusInvCustomTabs::noteActive(LLInventoryPanel* panel)
{
    if (mLastActivePanel && mLastActivePanel != panel && isCustomPanel(mLastActivePanel))
    {
        updateAutoLabel(mLastActivePanel);
        save();
    }
    if (panel && panel != mAddTabPanel && panel != mShowHiddenTabPanel && mTabs && mTabs->getIndexForPanel(panel) >= 0)
    {
        mLastActivePanel = panel;
    }
}

void PlusInvCustomTabs::notifyActiveFilterStateChanged()
{
    if (!mParent || !isCustomPanel(mParent->getActivePanel()))
    {
        return;
    }
    save();
}

void PlusInvCustomTabs::ensureRootOpenForActivePanel()
{
    if (mPendingRootOpen.empty() || !mParent)
    {
        return;
    }
    auto* active = mParent->getActivePanel();
    if (!active)
    {
        return;
    }
    const auto it = mPendingRootOpen.find(active);
    if (it == mPendingRootOpen.end())
    {
        return;
    }
    if (auto* folder = dynamic_cast<LLFolderViewFolder*>(active->getItemByID(gInventory.getRootFolderID())))
    {
        folder->setOpen(true);
        active->getFilter().setModified(LLInventoryFilter::FILTER_RESTART);
        mPendingRootOpen.erase(it);
    }
}

void PlusInvCustomTabs::attachToFilterEditor(LLFilterEditor* filter_editor)
{
    if (!filter_editor)
    {
        return;
    }
    auto* inner = filter_editor->findChild<LLLineEditor>("filter edit box");
    if (!inner)
    {
        return;
    }
    auto handle = getHandle();
    inner->setFocusLostCallback([handle](LLFocusableElement*)
    {
        if (auto* self = handle.get())
        {
            self->onFilterFocusLost();
        }
    });
}

// Labels follow the words only when the search box is left (Firestorm's 763c20fbf7: not at every key).
void PlusInvCustomTabs::onFilterFocusLost()
{
    if (mParent)
    {
        auto* active = mParent->getActivePanel();
        if (isCustomPanel(active))
        {
            updateAutoLabel(active);
        }
    }
    save();
}

// ---------------------------------------------------------------------------
// The button tabs ("+" and the hidden-tabs arrow)
// ---------------------------------------------------------------------------

// At the end of the tabs: the arrow (hidden until a tab is hidden), then "+".
void PlusInvCustomTabs::installButtonTabs()
{
    if (!mTabs || mAddTabPanel)
    {
        return;
    }

    mShowHiddenTabPanel = addButtonTab(SHOW_HIDDEN_PANEL_NAME, LLStringUtil::null);
    if (auto* btn = findTabButton(mShowHiddenTabPanel))
    {
        btn->setImageOverlay(std::string(SHOW_HIDDEN_ICON_NAME));
        std::string tooltip;
        if (LLTrans::findString(tooltip, SHOW_HIDDEN_TIP_KEY) && !tooltip.empty())
        {
            btn->setToolTip(tooltip);
        }
    }
    if (mShowHiddenTabPanel)
    {
        mTabs->setTabButtonVisible(mShowHiddenTabPanel, false);
    }

    mAddTabPanel = addButtonTab(ADD_TAB_PANEL_NAME, ADD_TAB_LABEL);

    // A press acts through onMouseDown() (no click callback on the buttons: the second
    // press of a double click reaches the button and would act again). The tab container
    // may not select a button tab by any way (a double click, the arrow keys, a drag
    // hovering): its panel would show and build an inventory view nobody sees.
    mValidateConnection = mTabs->setValidateCallback([](LLUICtrl*, const LLSD& tab_name)
    {
        const std::string name = tab_name.asString();
        return name != ADD_TAB_PANEL_NAME && name != SHOW_HIDDEN_PANEL_NAME;
    });
}

// A narrow tab at the end whose panel is never shown.
LLInventoryPanel* PlusInvCustomTabs::addButtonTab(const char* name, const std::string& label)
{
    LLInventoryPanel::Params params;
    params.name(name);
    params.sort_order_setting(LLInventoryPanel::INHERIT_SORT_ORDER);
    params.preinitialize_views(false);
    params.follows.flags(FOLLOWS_ALL);

    auto* panel = LLUICtrlFactory::create<LLInventoryPanel>(params);
    if (!panel)
    {
        return nullptr;
    }

    mTabs->addTabPanel(LLTabContainer::TabPanelParams()
        .panel(panel)
        .label(label)
        .select_tab(false)
        .insert_at(LLTabContainer::END));

    mTabs->setTabButtonWidth(panel, llmax(mTabs->getMinTabWidth() / 2, BUTTON_TAB_MIN_WIDTH));
    if (auto* btn = findTabButton(panel))
    {
        btn->setLeftHPad(BUTTON_TAB_H_PAD);
        btn->setRightHPad(BUTTON_TAB_H_PAD);
    }
    return panel;
}

// A button tab became the current one anyway (the validate callback should not let
// it): back to the last real tab, else the first tab.
void PlusInvCustomTabs::leaveButtonTab()
{
    if (!mTabs)
    {
        return;
    }
    if (mLastActivePanel && mTabs->getIndexForPanel(mLastActivePanel) >= 0)
    {
        mTabs->selectTabPanel(mLastActivePanel);
    }
    else
    {
        mTabs->selectFirstTab();
    }
}

void PlusInvCustomTabs::onAddClicked()
{
    if (addTab(std::string(), std::string(), true))
    {
        save();
    }
}

void PlusInvCustomTabs::onShowHiddenClicked()
{
    if (!mTabs || mHidden.empty())
    {
        return;
    }
    killMenu(mHiddenMenuHandle);
    LLContextMenu* menu = buildHiddenTabsMenu();
    LLButton* btn = findTabButton(mShowHiddenTabPanel);
    if (!menu || !btn)
    {
        LL_INFOS("InvCustomTabs") << "Hidden tabs menu: " << mHidden.size() << " hidden, menu " << (menu ? "made" : "not made")
                                  << ", button " << (btn ? "found" : "not found") << LL_ENDL;
        return;
    }
    // An LLContextMenu is made visible by show() only: its setVisible(true) does nothing, so
    // LLMenuGL::showPopup() alone (which calls setVisible(true)) leaves it hidden. show() takes
    // screen coordinates; showPopup() then puts it under the button (and in a detached window,
    // plus/detach) as the shortcut bar's menus do.
    S32 screen_x = 0;
    S32 screen_y = 0;
    btn->localPointToScreen(0, 0, &screen_x, &screen_y);
    menu->show(screen_x, screen_y, btn);
    LLMenuGL::showPopup(btn, menu, 0, 0);
    // One line per press (counts only): no line = the press did not get here; 0 items = an
    // empty list; visible no = it was not shown.
    LL_INFOS("InvCustomTabs") << "Hidden tabs menu: " << mHidden.size() << " hidden, " << menu->getItemCount() << " items, visible "
                              << (menu->getVisible() ? "yes" : "no") << ", button found" << LL_ENDL;
}

// One item per hidden tab, in tab order; an item shows its tab again.
LLContextMenu* PlusInvCustomTabs::buildHiddenTabsMenu()
{
    LLContextMenu::Params menu_params;
    menu_params.name(HIDDEN_MENU_NAME);
    menu_params.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);
    if (!menu)
    {
        return nullptr;
    }
    LLMenuGL::sMenuContainer->addChild(menu);
    mHiddenMenuHandle = menu->getHandle();

    auto handle = getHandle();
    const S32 count = mTabs->getTabCount();
    for (S32 i = 0; i < count; ++i)
    {
        auto* inv_panel = dynamic_cast<LLInventoryPanel*>(mTabs->getPanelByIndex(i));
        if (!inv_panel || !mHidden.count(inv_panel))
        {
            continue;
        }

        std::string label = computeDisplayLabel(inv_panel);
        if (label.empty())
        {
            label = getDefaultTabName();
        }

        const std::string panel_name = inv_panel->getName();
        LLMenuItemCallGL::Params item_params;
        item_params.name(panel_name);
        item_params.label(label);
        item_params.on_click.function([handle, panel_name](LLUICtrl*, const LLSD&)
        {
            if (auto* self = handle.get())
            {
                if (self->mTabs)
                {
                    self->showTab(dynamic_cast<LLInventoryPanel*>(self->mTabs->getPanelByName(panel_name)));
                }
            }
        });
        if (auto* item = LLUICtrlFactory::create<LLMenuItemCallGL>(item_params))
        {
            menu->addChild(item);
        }
    }
    return menu;
}

void PlusInvCustomTabs::updateShowHiddenTabVisibility()
{
    if (mTabs && mShowHiddenTabPanel)
    {
        mTabs->setTabButtonVisible(mShowHiddenTabPanel, !mHidden.empty());
    }
}

// ---------------------------------------------------------------------------
// The custom tabs
// ---------------------------------------------------------------------------

LLInventoryPanel* PlusInvCustomTabs::addTab(const std::string& explicit_name, const std::string& filter, bool select)
{
    if (!mTabs || !mParent)
    {
        return nullptr;
    }

    if (mPanels.size() >= MAX_TABS)
    {
        LL_WARNS() << "Refusing to add a custom inventory tab: " << MAX_TABS << " already" << LL_ENDL;
        return nullptr;
    }

    static S32 sCounter = 0;
    const auto internal_name = llformat(CUSTOM_PANEL_FORMAT, ++sCounter);

    LLInventoryPanel::Params params;
    params.name(internal_name);
    params.sort_order_setting(LLInventoryPanel::INHERIT_SORT_ORDER);
    params.show_item_link_overlays(true);
    params.preinitialize_views(false);
    // No double_click_override (as Firestorm): MultiModeDoubleClickFolder would switch to the single
    // folder view with a custom tab shown; the custom tab code assumes a custom tab is never the
    // active panel there (research/314).
    params.follows.flags(FOLLOWS_ALL);

    auto* panel = LLUICtrlFactory::create<LLInventoryPanel>(params);
    if (!panel)
    {
        return nullptr;
    }

    panel->setSortOrder(gSavedSettings.getU32(LLInventoryPanel::DEFAULT_SORT_ORDER));
    panel->getFilter().markDefault();
    panel->setSelectCallback(boost::bind(&LLPanelMainInventory::onSelectionChange, mParent, panel, _1, _2));
    mPanels.insert(panel);
    mPendingRootOpen.insert(panel);

    // A name is only ever saved for a tab renamed by hand (Firestorm's format too), so a
    // name given here keeps the tab from being named after its words.
    if (!explicit_name.empty())
    {
        mExplicitlyNamed.insert(panel);
    }
    if (!filter.empty())
    {
        panel->setFilterSubString(filter);
    }

    const std::string display_label = explicit_name.empty() ? labelForWords(filter) : explicit_name;

    // Added at the end, then moved before the button tabs; selected afterwards (select_tab
    // selects the last tab, which would be "+" until the move).
    mTabs->addTabPanel(LLTabContainer::TabPanelParams()
        .panel(panel)
        .label(display_label)
        .select_tab(false)
        .insert_at(LLTabContainer::END));
    LLPanel* anchor = mShowHiddenTabPanel ? static_cast<LLPanel*>(mShowHiddenTabPanel) : static_cast<LLPanel*>(mAddTabPanel);
    if (anchor)
    {
        const S32 anchor_idx = mTabs->getIndexForPanel(anchor);
        if (anchor_idx >= 0)
        {
            mTabs->moveTabPanel(panel, anchor_idx);
        }
    }

    decorateCustomTabButton(panel);
    if (auto* tab_btn = findTabButton(panel))
    {
        tab_btn->setToolTip(display_label);
    }
    if (const S32 idx = mTabs->getIndexForPanel(panel); idx >= 0)
    {
        mTabs->setTabPadding(idx, CLOSE_BUTTON_RESERVE);
    }
    if (select)
    {
        mTabs->selectTabPanel(panel);
    }
    return panel;
}

// The close icon inside the tab's button (the press itself is taken by onMouseDown()).
void PlusInvCustomTabs::decorateCustomTabButton(LLInventoryPanel* panel)
{
    if (!mTabs || !panel)
    {
        return;
    }
    auto* btn = findTabButton(panel);
    if (!btn)
    {
        return;
    }

    const S32 btn_w = btn->getRect().getWidth();
    const S32 btn_h = btn->getRect().getHeight();
    if (btn_w < CLOSE_BUTTON_RESERVE || btn_h < CLOSE_ICON_SIZE)
    {
        return;
    }

    btn->setRightHPad(CLOSE_BUTTON_RESERVE);
    // A long name ends in "...": the tab container gives that to every tab button each time it
    // selects a tab, but a tab made after the last selection (a load) waits for the next one.
    btn->setUseEllipses(true);

    LLButton::Params close_params;
    close_params.name(CLOSE_BUTTON_PREFIX + panel->getName());
    close_params.rect(LLRect(btn_w - CLOSE_ICON_MARGIN - CLOSE_ICON_SIZE, (btn_h + CLOSE_ICON_SIZE) / 2, btn_w - CLOSE_ICON_MARGIN, (btn_h - CLOSE_ICON_SIZE) / 2));
    close_params.follows.flags(FOLLOWS_RIGHT);
    close_params.image_unselected.name(CLOSE_ICON_NAME);
    close_params.image_selected.name(CLOSE_ICON_PRESS);
    close_params.image_hover_unselected.name(CLOSE_ICON_PRESS);
    close_params.image_pressed.name(CLOSE_ICON_PRESS);
    close_params.tab_stop(false);
    close_params.label(LLStringUtil::null);
    // Only drawn: the press on it is taken by onMouseDown() (hitTestCloseIcon()), and a
    // button of its own would act again on a double click's second press.
    close_params.mouse_opaque(false);

    if (auto* close_btn = LLUICtrlFactory::create<LLButton>(close_params))
    {
        btn->addChild(close_btn);
    }
}

// Asks about mContextPanel; the notification names the tab by its inner panel name,
// so an answer after the tab is gone does nothing.
void PlusInvCustomTabs::askAboutContextTab(const char* notification_name, LLNotificationPtr& pending, answer_fn_t on_answer)
{
    cancelNotification(pending);

    LLSD args;
    args["NAME"] = mContextPanel->getLabel();
    LLSD payload;
    payload["panel_name"] = mContextPanel->getName();

    auto handle = getHandle();
    LLNotificationPtr notification = LLNotificationsUtil::add(notification_name, args, payload,
        [handle, on_answer](const LLSD& notification, const LLSD& response)
        {
            if (auto* self = handle.get())
            {
                (self->*on_answer)(notification, response);
            }
        });
    // With "do not ask again" the answer came already, inside add().
    if (notification && !notification->isRespondedTo())
    {
        pending = notification;
    }
}

LLInventoryPanel* PlusInvCustomTabs::customTabNamedIn(const LLSD& notification) const
{
    if (!mTabs)
    {
        return nullptr;
    }
    auto* panel = dynamic_cast<LLInventoryPanel*>(mTabs->getPanelByName(notification["payload"]["panel_name"].asString()));
    return isCustomPanel(panel) ? panel : nullptr;
}

void PlusInvCustomTabs::onRenameClicked()
{
    if (isCustomPanel(mContextPanel))
    {
        askAboutContextTab(RENAME_NOTIFY, mRenameNotification, &PlusInvCustomTabs::onRenameConfirmed);
    }
}

void PlusInvCustomTabs::onRenameConfirmed(const LLSD& notification, const LLSD& response)
{
    mRenameNotification.reset();

    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        return;
    }

    const std::string new_name = sanitizeTabName(response["new_name"].asString());
    if (new_name.empty())
    {
        return;
    }

    auto* panel = customTabNamedIn(notification);
    if (!panel)
    {
        return;
    }

    mExplicitlyNamed.insert(panel);
    applyTabLabel(panel, new_name);
    save();
}

void PlusInvCustomTabs::onCloneClicked()
{
    if (!isCustomPanel(mContextPanel))
    {
        return;
    }
    auto& src_filter = mContextPanel->getFilter();
    const std::string& filter = src_filter.getFilterSubStringOrig();
    const std::string explicit_name = mExplicitlyNamed.count(mContextPanel) ? mContextPanel->getLabel() : std::string();
    // Made unselected and selected once its filter is copied: the save the selection may
    // make (the tab left) then already has the clone as it is, and the save below finds
    // nothing changed (one write, so the other windows build their tabs again once).
    auto* new_panel = addTab(explicit_name, filter, false);
    if (!new_panel)
    {
        return;
    }
    LLInventoryFilter::Params filter_params;
    src_filter.toParams(filter_params);
    // toParams() gives the upper-cased words; the clone keeps them as typed.
    filter_params.substring = filter;
    if (filter_params.validateBlock(false))
    {
        auto& new_filter = new_panel->getFilter();
        new_filter.fromParams(filter_params);
        new_filter.setShowFolderState(src_filter.getShowFolderState());
        new_filter.setFilterCreator(src_filter.getFilterCreatorType());
        new_filter.setFilterLinks(src_filter.getFilterLinks());
        new_filter.setSearchType(src_filter.getSearchType());
    }
    mTabs->selectTabPanel(new_panel);
    save();
}

void PlusInvCustomTabs::onClosePressed(LLInventoryPanel* panel)
{
    if (!isCustomPanel(panel))
    {
        return;
    }
    mContextPanel = panel;
    onCloseClicked();
}

void PlusInvCustomTabs::onCloseClicked()
{
    if (mTabs && isCustomPanel(mContextPanel))
    {
        askAboutContextTab(CLOSE_NOTIFY, mCloseNotification, &PlusInvCustomTabs::onCloseConfirmed);
    }
}

void PlusInvCustomTabs::onCloseConfirmed(const LLSD& notification, const LLSD& response)
{
    mCloseNotification.reset();

    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        return;
    }

    auto* to_remove = customTabNamedIn(notification);
    if (!to_remove)
    {
        return;
    }

    if (mContextPanel == to_remove)
    {
        mContextPanel = nullptr;
    }
    if (mLastActivePanel == to_remove)
    {
        mLastActivePanel = nullptr;
    }

    if (mTabs->getCurrentPanel() == to_remove)
    {
        selectNearestVisibleTab(to_remove);
    }

    mPanels.erase(to_remove);
    mExplicitlyNamed.erase(to_remove);
    mPendingRootOpen.erase(to_remove);
    mHidden.erase(to_remove);
    mTabs->removeTabPanel(to_remove);
    to_remove->die();
    updateShowHiddenTabVisibility();
    save();
}

void PlusInvCustomTabs::onHideClicked()
{
    if (isCustomPanel(mContextPanel))
    {
        hideTab(mContextPanel);
    }
}

void PlusInvCustomTabs::hideTab(LLInventoryPanel* panel)
{
    if (!mTabs || !isCustomPanel(panel) || mHidden.count(panel))
    {
        return;
    }
    const bool was_active = (mTabs->getCurrentPanel() == panel);
    mHidden.insert(panel);
    mTabs->setTabButtonVisible(panel, false);
    if (mContextPanel == panel)
    {
        mContextPanel = nullptr;
    }
    if (mLastActivePanel == panel)
    {
        mLastActivePanel = nullptr;
    }
    if (was_active)
    {
        selectNearestVisibleTab(panel);
    }
    updateShowHiddenTabVisibility();
    save();
}

void PlusInvCustomTabs::showTab(LLInventoryPanel* panel)
{
    if (!mTabs || !isCustomPanel(panel) || !mHidden.count(panel))
    {
        return;
    }
    mHidden.erase(panel);
    mTabs->setTabButtonVisible(panel, true);
    updateShowHiddenTabVisibility();
    mTabs->selectTabPanel(panel);
    save();
}

// The nearest visible real tab before skip_panel, else after it, else the first tab.
void PlusInvCustomTabs::selectNearestVisibleTab(LLInventoryPanel* skip_panel)
{
    if (!mTabs)
    {
        return;
    }

    const S32 skip_idx = mTabs->getIndexForPanel(skip_panel);
    const S32 count = mTabs->getTabCount();
    const auto selectable = [this](LLPanel* p) -> bool
    {
        return p && !isAddTab(p) && !isShowHiddenTab(p) && mTabs->getTabVisibility(p);
    };

    for (S32 i = skip_idx - 1; i >= 0; --i)
    {
        if (selectable(mTabs->getPanelByIndex(i)))
        {
            mTabs->selectTab(i);
            return;
        }
    }
    for (S32 i = skip_idx + 1; i < count; ++i)
    {
        if (selectable(mTabs->getPanelByIndex(i)))
        {
            mTabs->selectTab(i);
            return;
        }
    }
    mTabs->selectFirstTab();
}

// Every custom tab goes (before a load from the setting).
void PlusInvCustomTabs::clearCustomTabs()
{
    if (!mTabs)
    {
        return;
    }

    mLoadedOk = false;
    mContextPanel = nullptr;
    mLastActivePanel = nullptr;

    auto* current = mTabs->getCurrentPanel();
    if (current && (isAddTab(current) || isShowHiddenTab(current) || isCustomTab(current)))
    {
        mTabs->selectFirstTab();
    }

    auto panels_copy = mPanels;
    mPanels.clear();
    mExplicitlyNamed.clear();
    mPendingRootOpen.clear();
    mHidden.clear();
    for (auto* panel : panels_copy)
    {
        mTabs->removeTabPanel(panel);
        panel->die();
    }

    // Selecting the first tab above may have noted a tab again.
    mLastActivePanel = nullptr;
    mContextPanel = nullptr;
    updateShowHiddenTabVisibility();
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

void PlusInvCustomTabs::applyTabLabel(LLInventoryPanel* panel, const std::string& label)
{
    if (!panel || !mTabs)
    {
        return;
    }
    panel->setLabel(label);
    if (const S32 idx = mTabs->getIndexForPanel(panel); idx >= 0)
    {
        mTabs->setPanelTitle(idx, label);
    }
    if (auto* btn = findTabButton(panel))
    {
        btn->setToolTip(label);
    }
    if (mParent && mParent->getActivePanel() == panel)
    {
        mParent->refreshFinderFromFilter();
    }
}

// A tab not renamed by hand is named after its words (or "Custom").
void PlusInvCustomTabs::updateAutoLabel(LLInventoryPanel* panel)
{
    if (!panel || mExplicitlyNamed.count(panel))
    {
        return;
    }
    const std::string new_label = computeDisplayLabel(panel);
    if (panel->getLabel() != new_label)
    {
        applyTabLabel(panel, new_label);
    }
}

std::string PlusInvCustomTabs::computeDisplayLabel(LLInventoryPanel* panel) const
{
    if (!panel)
    {
        return getDefaultTabName();
    }
    if (mExplicitlyNamed.count(panel))
    {
        return panel->getLabel();
    }
    return labelForWords(panel->getFilter().getFilterSubStringOrig());
}

// The label and tool tip of a tab named after its words: the words without control
// characters and direction marks (the rename's checks), else "Custom".
std::string PlusInvCustomTabs::labelForWords(const std::string& words)
{
    std::string label = sanitizeTabName(words);
    return label.empty() ? getDefaultTabName() : label;
}

std::string PlusInvCustomTabs::getDefaultTabName()
{
    std::string name;
    if (LLTrans::findString(name, DEFAULT_NAME_KEY) && !name.empty())
    {
        return name;
    }
    return FALLBACK_LABEL;
}

// Without control characters, the soft hyphen, zero-width and direction marks
// (U+200B..U+200F, U+2028..U+202E, U+2066..U+2069) and the BOM; trimmed, at most
// MAX_NAME_LENGTH bytes (whole UTF-8 characters).
std::string PlusInvCustomTabs::sanitizeTabName(std::string name)
{
    std::string out;
    out.reserve(name.size());

    for (size_t i = 0; i < name.size(); )
    {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F)
        {
            ++i;
            continue;
        }
        if (c == 0xC2 && i + 1 < name.size() && static_cast<unsigned char>(name[i + 1]) == 0xAD)
        {
            i += 2; // U+00AD
            continue;
        }
        if (c == 0xE2 && i + 2 < name.size())
        {
            const auto c1 = static_cast<unsigned char>(name[i + 1]);
            const auto c2 = static_cast<unsigned char>(name[i + 2]);
            if (c1 == 0x80 && ((c2 >= 0x8B && c2 <= 0x8F) // U+200B ... U+200F
                || (c2 >= 0xA8 && c2 <= 0xA9) // U+2028 ... U+2029
                || (c2 >= 0xAA && c2 <= 0xAE))) // U+202A ... U+202E
            {
                i += 3;
                continue;
            }
            if (c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9) // U+2066 ... U+2069
            {
                i += 3;
                continue;
            }
        }
        if (c == 0xEF && i + 2 < name.size() && static_cast<unsigned char>(name[i + 1]) == 0xBB && static_cast<unsigned char>(name[i + 2]) == 0xBF)
        {
            i += 3; // U+FEFF (BOM)
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }

    LLStringUtil::trim(out);
    if (out.size() > MAX_NAME_LENGTH)
    {
        out = utf8str_truncate(out, static_cast<S32>(MAX_NAME_LENGTH));
    }

    return out;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// The other windows save first, so this one starts from what they show.
void PlusInvCustomTabs::load()
{
    if (mLoading)
    {
        return;
    }
    FlagScope loading(mLoading);

    for (auto* other : allInstances())
    {
        if (other != this)
        {
            other->save();
        }
    }

    doLoad();
    subscribeToSetting();
}

void PlusInvCustomTabs::doLoad()
{
    mLoadedOk = false;

    const LLSD tabs = gSavedPerAccountSettings.getLLSD(SETTING_KEY);
    if (!tabs.isArray())
    {
        updateShowHiddenTabVisibility();
        mLoadedOk = true;
        return;
    }

    size_t attempted = 0;
    size_t added = 0;
    for (auto it = tabs.beginArray(); it != tabs.endArray(); ++it)
    {
        if (mPanels.size() >= MAX_TABS)
        {
            // At most MAX_TABS tabs: the entries after them are not loaded, and the next save leaves them out.
            LL_WARNS() << "Custom inventory tabs: only the first " << MAX_TABS << " saved tabs are loaded; the others go at the next save" << LL_ENDL;
            break;
        }
        const LLSD& entry = *it;
        if (!entry.isMap())
        {
            continue;
        }

        // The settings file is read as data: the name gets the rename's checks, the words
        // what the search box allows (no emoji, at most its 300 characters).
        const std::string explicit_name = sanitizeTabName(entry["name"].asString());

        const std::string filter = savedWords(entry);

        ++attempted;
        if (auto* new_panel = addTab(explicit_name, filter, false))
        {
            ++added;
            if (entry.has("states") && entry["states"].isMap())
            {
                applySavedStates(new_panel, entry["states"], filter);
            }

            if (entry.has(TABHIDDEN_KEY) && entry[TABHIDDEN_KEY].asBoolean())
            {
                mHidden.insert(new_panel);
                mTabs->setTabButtonVisible(new_panel, false);
            }
        }
    }

    updateShowHiddenTabVisibility();
    mLoadedOk = (added == attempted);
    if (!mLoadedOk)
    {
        LL_WARNS() << "Custom inventory tabs: made " << added << " of " << attempted << " saved tabs; not saved until a load makes all" << LL_ENDL;
    }
}

// The saved filter of one tab. Every value is bounded here: the per-account
// settings file can be edited by hand or come from another viewer.
void PlusInvCustomTabs::applySavedStates(LLInventoryPanel* panel, const LLSD& states, const std::string& filter)
{
    auto& new_filter = panel->getFilter();

    // Start from the tab's own values: a field the file lacks keeps it (the official
    // fromParams() sets every field, and a missing "filter_types" would otherwise
    // become FILTERTYPE_OBJECT, i.e. textures only).
    LLInventoryFilter::Params filter_params;
    new_filter.toParams(filter_params);
    LLParamSDParser().readSD(states, filter_params, true);

    auto& ops = filter_params.filter_ops;
    if (ops.hours_ago() > MAX_HOURS_AGO)
    {
        ops.hours_ago = MAX_HOURS_AGO;
    }
    if (ops.date_search_direction() > LLInventoryFilter::FILTERDATEDIRECTION_OLDER)
    {
        ops.date_search_direction = (U32)LLInventoryFilter::FILTERDATEDIRECTION_NEWER;
    }
    ops.permissions = ops.permissions() & PERM_ALL;
    // The words are saved apart ("filter", as typed); "states" has none.
    filter_params.substring = filter;
    if (filter_params.validateBlock(false))
    {
        new_filter.fromParams(filter_params);
    }

    // Enums the param parser does not read: saved as integers, taken only within their range.
    S32 value = 0;
    const LLSD& saved_ops = states["filter_ops"];
    if (saved_ops.isMap())
    {
        if (readEnum(saved_ops["show_folder_state"], LLInventoryFilter::SHOW_ALL_FOLDERS, LLInventoryFilter::SHOW_NO_FOLDERS, value))
        {
            new_filter.setShowFolderState(static_cast<LLInventoryFilter::EFolderShow>(value));
        }
        if (readEnum(saved_ops["creator_type"], LLInventoryFilter::FILTERCREATOR_ALL, LLInventoryFilter::FILTERCREATOR_OTHERS, value))
        {
            new_filter.setFilterCreator(static_cast<LLInventoryFilter::EFilterCreatorType>(value));
        }
        // "Only links" (a Firestorm tab can have it) is read as "include links": nothing here
        // keeps that mode (the official setFilterSubString() leaves it for the backup made by
        // setFindAllLinksMode(), the defaults after a load, so the first word typed would clear
        // the whole filter and save that).
        if (readEnum(saved_ops["links"], LLInventoryFilter::FILTERLINK_INCLUDE_LINKS, LLInventoryFilter::FILTERLINK_ONLY_LINKS, value))
        {
            if (value == LLInventoryFilter::FILTERLINK_ONLY_LINKS)
            {
                value = LLInventoryFilter::FILTERLINK_INCLUDE_LINKS;
            }
            new_filter.setFilterLinks(static_cast<U64>(value));
        }
    }
    if (readEnum(states["search_type"], LLInventoryFilter::SEARCHTYPE_NAME, LLInventoryFilter::SEARCHTYPE_UUID, value))
    {
        new_filter.setSearchType(static_cast<LLInventoryFilter::ESearchType>(value));
    }
}

// Written only when it differs from the setting (the setting's signal makes the
// other windows load again).
void PlusInvCustomTabs::save()
{
    if (!mTabs || mLoading || !mLoadedOk)
    {
        return;
    }

    LLSD array = LLSD::emptyArray();
    const S32 count = mTabs->getTabCount();
    for (S32 i = 0; i < count; ++i)
    {
        auto* inv_panel = dynamic_cast<LLInventoryPanel*>(mTabs->getPanelByIndex(i));
        if (isCustomPanel(inv_panel))
        {
            array.append(savedEntry(inv_panel));
        }
    }

    if (isUnchangedSetting(array))
    {
        return;
    }
    FlagScope saving(mSaving);
    gSavedPerAccountSettings.setLLSD(SETTING_KEY, array);
}

// Firestorm's format: name (empty unless renamed by hand), filter (the words as typed),
// states (the filter's params without the words, plus the enums as integers), tabhidden.
LLSD PlusInvCustomTabs::savedEntry(LLInventoryPanel* panel) const
{
    LLSD entry;
    entry["name"] = mExplicitlyNamed.count(panel) ? panel->getLabel() : std::string();
    auto& filter = panel->getFilter();
    entry["filter"] = filter.getFilterSubStringOrig();

    LLInventoryFilter::Params filter_params;
    filter.toParams(filter_params);
    if (filter_params.validateBlock(false))
    {
        LLSD states_sd;
        LLParamSDParser().writeSD(states_sd, filter_params);
        if (states_sd.isMap() && states_sd.has("substring"))
        {
            states_sd.erase("substring");
        }
        states_sd["filter_ops"]["show_folder_state"] = static_cast<S32>(filter.getShowFolderState());
        states_sd["filter_ops"]["creator_type"] = static_cast<S32>(filter.getFilterCreatorType());
        states_sd["filter_ops"]["links"] = static_cast<S32>(filter.getFilterLinks());
        states_sd["search_type"] = static_cast<S32>(filter.getSearchType());
        entry["states"] = states_sd;
    }

    entry[TABHIDDEN_KEY] = static_cast<bool>(mHidden.count(panel));
    return entry;
}

bool PlusInvCustomTabs::isUnchangedSetting(const LLSD& array) const
{
    const LLSD current = gSavedPerAccountSettings.getLLSD(SETTING_KEY);
    if (!current.isArray())
    {
        return array.size() == 0;
    }
    if (current.size() != array.size())
    {
        return false;
    }
    for (LLSD::Integer i = 0; i < static_cast<LLSD::Integer>(array.size()); ++i)
    {
        if (!sameTab(current[i], array[i]) || current[i][TABHIDDEN_KEY].asBoolean() != array[i][TABHIDDEN_KEY].asBoolean())
        {
            return false;
        }
    }
    return true;
}

void PlusInvCustomTabs::subscribeToSetting()
{
    if (mSettingConnection.connected())
    {
        return;
    }
    if (auto control = gSavedPerAccountSettings.getControl(SETTING_KEY))
    {
        // (control, new value, previous value)
        mSettingConnection = control->getSignal()->connect(boost::bind(&PlusInvCustomTabs::onSettingChangedExternally, this, _2, _3));
    }
}

// Another window saved, or the per-account settings were read again.
// With as many entries as before (a rename, new words or filters, hiding: tabs cannot
// be reordered, so each entry is still the tab at its place), the changed tabs are
// brought up to date where they are: no tab is made again or selected, so the open
// folders, the selection and the scrolling stay. Otherwise (a tab added or closed)
// the tabs are made again, and the custom tab shown is selected again: its previous
// entry, nearest to its old place first (two unnamed clones are the same entry); when
// it is not there, nothing is selected. In both, words typed into a tab but not saved
// yet (they differ from its previous entry) stay. Received Items is brought in line
// once at the end when a custom tab was or is shown (the tab selections meanwhile leave
// it alone: mLoading).
void PlusInvCustomTabs::onSettingChangedExternally(const LLSD& value, const LLSD& previous)
{
    if (mSaving || mLoading)
    {
        return;
    }
    cancelNotification(mRenameNotification);
    cancelNotification(mCloseNotification);

    // As on a tab change: Received Items is only touched when a custom tab was or is shown
    // (a window showing an official tab keeps what it has).
    const bool was_custom = isCustomPanel(mParent ? mParent->getActivePanel() : nullptr);
    if (!updateTabsInPlace(value, previous))
    {
        rebuildTabs(value, previous);
    }
    if (was_custom || isCustomPanel(mParent ? mParent->getActivePanel() : nullptr))
    {
        syncReceivedItems();
    }
}

// The rebuild of onSettingChangedExternally() (a tab added or closed).
void PlusInvCustomTabs::rebuildTabs(const LLSD& value, const LLSD& previous)
{
    S32 shown_tab = -1;
    S32 shown_entry_index = -1;
    LLSD shown_entry;
    std::string unsaved_words;
    bool has_unsaved_words = false;
    if (LLInventoryPanel* active = mParent ? mParent->getActivePanel() : nullptr; isCustomPanel(active))
    {
        shown_tab = customTabIndex(active);
        const S32 entry = entryOfTab(previous, shown_tab);
        if (entry >= 0)
        {
            shown_entry_index = entry;
            shown_entry = previous[entry];
            const std::string& words = active->getFilter().getFilterSubStringOrig();
            if (shown_entry["filter"].asString() != words)
            {
                unsaved_words = words;
                has_unsaved_words = true;
            }
        }
    }

    FlagScope loading(mLoading);
    clearCustomTabs();
    doLoad();

    if (shown_entry.isUndefined() || !value.isArray())
    {
        return;
    }
    // Nearest to the old place first: old place, one before, one after, ... (the old
    // place may lie past the end of the new value).
    S32 found = -1;
    const S32 size = static_cast<S32>(value.size());
    for (S32 d = 0; found < 0 && d <= std::max(size, shown_entry_index); ++d)
    {
        for (const S32 i : { shown_entry_index - d, shown_entry_index + d })
        {
            if (found < 0 && i >= 0 && i < size && value[i].isMap() && sameTab(value[i], shown_entry))
            {
                found = i;
            }
        }
    }

    LLInventoryPanel* panel = customTabAt(tabOfEntry(value, found));
    if (panel && !mHidden.count(panel))
    {
        if (has_unsaved_words)
        {
            panel->setFilterSubString(unsaved_words);
        }
        mTabs->selectTabPanel(panel);
    }
}

// The in-place update of onSettingChangedExternally(): false (nothing done) unless
// both values hold the same number of map entries, one per custom tab of this window.
bool PlusInvCustomTabs::updateTabsInPlace(const LLSD& value, const LLSD& previous)
{
    if (!mTabs || !mLoadedOk || !value.isArray() || !previous.isArray() || value.size() != previous.size()
        || value.size() > MAX_TABS || value.size() != mPanels.size())
    {
        return false;
    }
    const S32 size = static_cast<S32>(value.size());
    for (S32 i = 0; i < size; ++i)
    {
        if (!value[i].isMap() || !previous[i].isMap())
        {
            return false;
        }
    }

    FlagScope loading(mLoading);
    for (S32 i = 0; i < size; ++i)
    {
        if (llsd_equals(value[i], previous[i]))
        {
            continue;
        }
        LLInventoryPanel* panel = customTabAt(i);
        if (!panel)
        {
            continue;
        }
        const LLSD& entry = value[i];

        const bool shown = mParent && mParent->getActivePanel() == panel;

        // The words, unless this window has typed others not saved yet. On the tab shown
        // they go through the window's search (the custom tab's way: Received Items
        // follows, the official tabs' shared words and folder state are not touched, and
        // nothing is saved: onFilterEdit() does not save, and mLoading would stop it).
        const std::string words = savedWords(entry);
        const std::string& current_words = panel->getFilter().getFilterSubStringOrig();
        if (words != current_words && current_words == savedWords(previous[i]))
        {
            if (shown)
            {
                mParent->onFilterEdit(words);
            }
            else
            {
                panel->setFilterSubString(words);
            }
        }
        if (!llsd_equals(entry["states"], previous[i]["states"]) && entry["states"].isMap())
        {
            applySavedStates(panel, entry["states"], panel->getFilter().getFilterSubStringOrig());
            if (shown)
            {
                // An open finder writes what it shows into the tab's filter every frame:
                // it must show the new filter, or it would write the old one back (and
                // save it, and with a finder open in the other window too, every frame).
                // Its first write after this still changes the filter (it sets the upper
                // 32 bits of the types, which are saved as U32), so save() is called once,
                // finds the same entries and writes nothing.
                mParent->refreshFinderFromFilter();
                // The window's search type box shows the tab's (it is set only when a tab is
                // selected). Setting the box's value commits nothing, so nothing is saved.
                mParent->updateSearchTypeCombo();
                // And the filter text the item count line shows (made again only when a tab is
                // selected, the filters are reset or the finder draws; it only reads the filter).
                mParent->setFilterTextFromFilter();
            }
        }

        const std::string name = sanitizeTabName(entry["name"].asString());
        if (!name.empty())
        {
            mExplicitlyNamed.insert(panel);
            if (panel->getLabel() != name)
            {
                applyTabLabel(panel, name);
            }
        }
        else
        {
            mExplicitlyNamed.erase(panel);
            updateAutoLabel(panel);
        }

        const bool hidden = entry[TABHIDDEN_KEY].asBoolean();
        if (hidden && !mHidden.count(panel))
        {
            hideTab(panel); // selects another tab if it was shown; its save waits (mLoading)
        }
        else if (!hidden && mHidden.count(panel))
        {
            mHidden.erase(panel);
            mTabs->setTabButtonVisible(panel, true);
        }
    }
    updateShowHiddenTabVisibility();
    return true;
}

// The place of a custom tab among the custom tabs (hidden ones too), in tab order:
// its place in the saved array. -1 if it is not one.
S32 PlusInvCustomTabs::customTabIndex(const LLInventoryPanel* panel) const
{
    if (!mTabs || !isCustomPanel(panel))
    {
        return -1;
    }
    S32 index = 0;
    const S32 count = mTabs->getTabCount();
    for (S32 i = 0; i < count; ++i)
    {
        auto* inv_panel = dynamic_cast<LLInventoryPanel*>(mTabs->getPanelByIndex(i));
        if (inv_panel == panel)
        {
            return index;
        }
        if (isCustomPanel(inv_panel))
        {
            ++index;
        }
    }
    return -1;
}

LLInventoryPanel* PlusInvCustomTabs::customTabAt(S32 index) const
{
    if (!mTabs || index < 0)
    {
        return nullptr;
    }
    const S32 count = mTabs->getTabCount();
    for (S32 i = 0; i < count; ++i)
    {
        auto* inv_panel = dynamic_cast<LLInventoryPanel*>(mTabs->getPanelByIndex(i));
        if (isCustomPanel(inv_panel) && index-- == 0)
        {
            return inv_panel;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool PlusInvCustomTabs::isCustomTab(const LLPanel* panel) const
{
    return isCustomPanel(dynamic_cast<const LLInventoryPanel*>(panel));
}

// A pointer lookup only (the draw hooks call it every frame).
bool PlusInvCustomTabs::isCustomPanel(const LLInventoryPanel* panel) const
{
    return panel && !mPanels.empty() && mPanels.count(const_cast<LLInventoryPanel*>(panel));
}

bool PlusInvCustomTabs::isAddTab(const LLPanel* panel) const
{
    return panel && panel == static_cast<const LLPanel*>(mAddTabPanel);
}

bool PlusInvCustomTabs::isShowHiddenTab(const LLPanel* panel) const
{
    return panel && panel == static_cast<const LLPanel*>(mShowHiddenTabPanel);
}

LLButton* PlusInvCustomTabs::findTabButton(const LLPanel* panel) const
{
    if (!mTabs || !panel)
    {
        return nullptr;
    }
    return mTabs->findChild<LLButton>(TAB_BUTTON_PREFIX + panel->getName());
}
