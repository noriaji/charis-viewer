/**
 * @file peopletab.cpp
 * @brief Contact sets in the People window and the places that open them (research/333)
 *
 * Ported from Firestorm (llpanelpeople.cpp [FS:CR] Contact sets, FIRE-17568, FIRE-21531, FIRE-31146,
 * FIRE-32401, FIRE-34809; llavataractions.cpp addToContactSet()/moveToContactSet()). See peopletab.h.
 *
 * This is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#include "llviewerprecompiledheaders.h"

#include "peopletab.h"

#include "contactsets.h"

#include "llagent.h"
#include "llavataractions.h"
#include "llavatarlist.h"
#include "llavatarlistitem.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llcombobox.h"
#include "llfiltereditor.h"
#include "llfloatersidepanelcontainer.h"
#include "llfloateravatarpicker.h"
#include "llinventory.h"
#include "llpanelpeoplemenus.h"
#include "llscrolllistctrl.h"
#include "llstyle.h"
#include "llviewercontrol.h"

namespace plus::contactsets
{
    namespace
    {
        // The most people the menus act on at once (Firestorm's MAX_SELECTIONS of llpanelpeople.cpp).
        constexpr size_t MAX_SELECTED = 20;

        // Online first, then by name as the other lists sort (LLAvatarItemNameComparator: an alias's quote
        // left out, Firestorm FIRE-36478) (Firestorm FIRE-21531, a set's "sort by online status").
        class OnlineThenNameComparator : public LLAvatarItemNameComparator
        {
        protected:
            bool doCompare(const LLAvatarListItem* item1, const LLAvatarListItem* item2) const override
            {
                const LLAvatarTracker& tracker = LLAvatarTracker::instance();
                const bool online1 = tracker.isBuddyOnline(item1->getAvatarId());
                const bool online2 = tracker.isBuddyOnline(item2->getAvatarId());
                if (online1 != online2)
                {
                    return online1;
                }
                return LLAvatarItemNameComparator::doCompare(item1, item2);
            }
        };

        const OnlineThenNameComparator ONLINE_THEN_NAME;

        LLPanelInjector<PeopleTab> sPeopleTabRegistration("plus_contact_sets_panel");
        LLDefaultChildRegistry::Register<ProfileSetsText> sProfileSetsTextRegistration("plus_contact_sets_text");

        // The avatar of a dragged person or calling card (null for other cargo).
        LLUUID draggedAvatar(EDragAndDropType cargo_type, void* cargo_data)
        {
            if (!cargo_data)
            {
                return LLUUID::null;
            }
            if (cargo_type == DAD_PERSON)
            {
                return *static_cast<const LLUUID*>(cargo_data);
            }
            if (cargo_type == DAD_CALLINGCARD)
            {
                return static_cast<const LLInventoryItem*>(cargo_data)->getCreatorUUID();
            }
            return LLUUID::null;
        }

        // A set of the user's (people can be added to it and taken out), not "All Sets" and the like.
        bool isUserSet(const std::string& set_name)
        {
            const LGGContactSets& sets = LGGContactSets::instance();
            return !set_name.empty() && !sets.isInternalSetName(set_name) && sets.isValidSet(set_name);
        }
    }

    // showAddToSet(), showMoveToSet(), namesShown() and the questions (ask*()): contactsets.cpp (cs-core).

    bool getTabSelection(const LLPanel* tab, uuid_vec_t& ids)
    {
        const PeopleTab* contact_sets_tab = dynamic_cast<const PeopleTab*>(tab);
        if (!contact_sets_tab)
        {
            return false;
        }
        contact_sets_tab->getSelectedUUIDs(ids);
        return true;
    }

    void showProfileSets(LLPanel* profile_panel, const LLUUID& avatar_id)
    {
        if (ProfileSetsText* sets_text = profile_panel ? profile_panel->findChild<ProfileSetsText>("plus_contact_sets_text") : nullptr)
        {
            sets_text->setAvatarId(avatar_id);
        }
    }

    //============================================================================ PeopleTab

    PeopleTab::PeopleTab()
    {
        // Before the XUI is read: the menus of the buttons are built from it with these.
        mCommitCallbackRegistrar.add("ContactSet.Action", [this](LLUICtrl*, const LLSD& param) { onAction(param); });
        mEnableCallbackRegistrar.add("ContactSet.Enable", [this](LLUICtrl*, const LLSD& param) { return onEnable(param); });
    }

    PeopleTab::~PeopleTab()
    {
        // Added in postBuild(); removing one that is not there does nothing (a failed build).
        LLAvatarTracker::instance().removeObserver(this);
        // mSetsChangedConnection and mFilterConnection disconnect themselves (scoped).
    }

    bool PeopleTab::postBuild()
    {
        mSetCombo = getChild<LLComboBox>("combo_sets");
        mSetCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { rebuildMemberList(); });

        mList = getChild<LLAvatarList>("contact_list");
        mList->setNoItemsCommentText(getString("empty_list"));
        mList->setNoItemsMsg(getString("empty_list"));
        mList->setNoFilteredItemsMsg(getString("no_filtered"));
        mList->setContextMenu(&LLPanelPeopleMenus::gPeopleContextMenu);
        // A set can hold people who are not friends (and so may be around): the names follow
        // @shownames as in the nearby list (no names, no context menu, no double click).
        mList->setRlvCheckShowNames(true);
        // The names in their set's colour (FSContactSetsColorizeFriends) and Firestorm's look of this list:
        // online in bold, offline not greyed (FS llpanelpeople.cpp, FIRE-32748, FIRE-31733).
        mList->setUseContactSetColors(true);
        mList->setUseContactSetListStyle(true);
        mList->setItemDoubleClickCallback([this](LLUICtrl* ctrl, S32, S32, MASK) { onListDoubleClicked(ctrl); });
        mList->setReturnCallback([this](LLUICtrl*, const LLSD&) { startIMWithSelection(); });

        mFilterConnection = getChild<LLFilterEditor>("contact_sets_filter_input")->setCommitCallback(
            [this](LLUICtrl*, const LLSD& value) { onFilterEdit(value.asString()); });

        mSetsChangedConnection = LGGContactSets::instance().setContactSetChangeCallback(
            [this](LGGContactSets::EContactSetUpdate type) { onSetsChanged(type); });

        LLAvatarTracker::instance().addObserver(this);

        return true;
    }

    void PeopleTab::onVisibilityChange(bool new_visibility)
    {
        LLPanel::onVisibilityChange(new_visibility);
        if (new_visibility)
        {
            rebuildIfStale();
        }
    }

    void PeopleTab::onSetsChanged(LGGContactSets::EContactSetUpdate type)
    {
        markStale(type == LGGContactSets::UPDATED_LISTS ? Stale::SETS : Stale::MEMBERS);
    }

    void PeopleTab::changed(U32 mask)
    {
        // The people of "No Sets" are the friends in no set: they change with the friends.
        if ((mask & (LLFriendObserver::ADD | LLFriendObserver::REMOVE)) && currentSet() == CS_SET_NO_SETS)
        {
            markStale(Stale::MEMBERS);
        }
        // Online first: a friend coming or going online moves in the list.
        if ((mask & LLFriendObserver::ONLINE) && mList && isInVisibleChain() && sortByOnlineStatus())
        {
            mList->sort();
        }
    }

    void PeopleTab::markStale(Stale stale)
    {
        if (stale > mStale)
        {
            mStale = stale;
        }
        // Hidden (another tab, the window closed): rebuilt when shown (onVisibilityChange).
        if (isInVisibleChain())
        {
            rebuildIfStale();
        }
    }

    void PeopleTab::rebuildIfStale()
    {
        if (mStale == Stale::NONE || !mSetCombo || !mList)
        {
            return;
        }
        const Stale stale = mStale;
        mStale = Stale::NONE;
        if (stale == Stale::SETS)
        {
            rebuildSetCombo();
        }
        rebuildMemberList();
    }

    void PeopleTab::rebuildSetCombo()
    {
        const std::string previous = currentSet();

        mSetCombo->clearRows();
        const LGGContactSets::string_vec_t sets = LGGContactSets::instance().getAllContactSets();
        for (const std::string& set_name : sets)
        {
            mSetCombo->add(set_name, LLSD(set_name));
        }
        if (!sets.empty())
        {
            mSetCombo->addSeparator(ADD_BOTTOM);
        }
        mSetCombo->add(getString("all_sets"), LLSD(CS_SET_ALL_SETS));
        mSetCombo->add(getString("no_sets"), LLSD(CS_SET_NO_SETS));
        mSetCombo->add(getString("pseudonyms"), LLSD(CS_SET_PSEUDONYM));

        // The set shown before stays shown; a removed or renamed one gives the first in the list.
        if (previous.empty() || !mSetCombo->setSelectedByValue(LLSD(previous), true))
        {
            mSetCombo->selectFirstItem();
        }
    }

    void PeopleTab::rebuildMemberList()
    {
        if (!mList)
        {
            return;
        }
        if (mStale == Stale::MEMBERS)
        {
            mStale = Stale::NONE; // a pick in the combo rebuilds the people too
        }

        const LGGContactSets& contact_sets = LGGContactSets::instance();
        const std::string set_name = currentSet();
        uuid_vec_t& ids = mList->getIDs();
        ids.clear();

        if (set_name == CS_SET_ALL_SETS)
        {
            ids = contact_sets.getFriendsInAnySet();
        }
        else if (set_name == CS_SET_NO_SETS)
        {
            // The friends who are in no set (Firestorm: "only show our buddies who aren't in a set").
            LLAvatarTracker::buddy_map_t buddies;
            LLAvatarTracker::instance().copyBuddyList(buddies);
            ids.reserve(buddies.size());
            for (const auto& buddy : buddies)
            {
                if (!contact_sets.isFriendInAnySet(buddy.first))
                {
                    ids.push_back(buddy.first);
                }
            }
        }
        else if (set_name == CS_SET_PSEUDONYM)
        {
            ids = contact_sets.getListOfPseudonymAvs();
        }
        else if (const LGGContactSets::ContactSet* set = contact_sets.getContactSet(set_name))
        {
            ids.assign(set->mFriends.begin(), set->mFriends.end());
        }

        applySorting();
        // An alias or a removed display name changes how a name reads: the items read it again.
        mList->handleDisplayNamesOptionChanged();
        mList->setDirty();
    }

    void PeopleTab::applySorting()
    {
        if (sortByOnlineStatus())
        {
            mList->setComparator(&ONLINE_THEN_NAME);
            mList->sort();
        }
        else
        {
            mList->sortByName();
        }
    }

    std::string PeopleTab::currentSet() const
    {
        return mSetCombo ? mSetCombo->getValue().asString() : std::string();
    }

    bool PeopleTab::currentSetIsUserSet() const
    {
        return isUserSet(currentSet());
    }

    bool PeopleTab::sortByOnlineStatus() const
    {
        return currentSetIsUserSet() && LGGContactSets::instance().getSortByOnlineStatusForSet(currentSet());
    }

    void PeopleTab::getSelectedUUIDs(uuid_vec_t& ids) const
    {
        if (mList)
        {
            mList->getSelectedUUIDs(ids);
        }
    }

    bool PeopleTab::usableSelection(uuid_vec_t& ids) const
    {
        getSelectedUUIDs(ids);
        return !ids.empty() && ids.size() <= MAX_SELECTED && namesShown(ids);
    }

    bool PeopleTab::onEnable(const LLSD& userdata)
    {
        const std::string item = userdata.asString();
        if (item == "has_mutable_set")
        {
            return currentSetIsUserSet();
        }

        uuid_vec_t ids;
        const bool usable = usableSelection(ids);
        if (item == "has_selection")
        {
            return usable;
        }
        if (item == "has_mutable_set_and_selection")
        {
            return usable && currentSetIsUserSet();
        }
        if (item == "has_single_selection")
        {
            return usable && ids.size() == 1;
        }
        if (item == "has_pseudonym")
        {
            return usable && LGGContactSets::instance().hasPseudonym(ids);
        }
        if (item == "has_display_name")
        {
            return usable && !LGGContactSets::instance().hasDisplayNameRemoved(ids);
        }
        return false;
    }

    void PeopleTab::onAction(const LLSD& userdata)
    {
        const std::string item = userdata.asString();
        if (!onSetAction(item))
        {
            onPeopleAction(item);
        }
    }

    bool PeopleTab::onSetAction(const std::string& item)
    {
        if (item == "add_set")
        {
            askAddSet();
            return true;
        }
        if (item != "remove_set" && item != "set_config" && item != "add_contact")
        {
            return false;
        }
        if (!currentSetIsUserSet())
        {
            return true;
        }

        const std::string set_name = currentSet();
        if (item == "remove_set")
        {
            askRemoveSet(set_name);
            return true;
        }

        LLFloater* root = gFloaterView->getParentFloater(this);
        LLFloater* opened = nullptr;
        if (item == "set_config")
        {
            opened = showSetConfig(set_name, this);
        }
        else
        {
            // The set by name, not this tab: the picker can outlive it.
            opened = LLFloaterAvatarPicker::show(
                [set_name](const uuid_vec_t& ids, const std::vector<LLAvatarName>&)
                {
                    if (!ids.empty() && LGGContactSets::instance().isValidSet(set_name))
                    {
                        LGGContactSets::instance().addToSet(ids, set_name);
                    }
                },
                true, true, true, root ? root->getName() : std::string());
        }
        if (root && opened)
        {
            root->addDependentFloater(opened);
        }
        return true;
    }

    void PeopleTab::onPeopleAction(const std::string& item)
    {
        uuid_vec_t ids;
        if (!usableSelection(ids))
        {
            return;
        }
        LGGContactSets& contact_sets = LGGContactSets::instance();
        const bool several = ids.size() > 1;

        if (item == "remove_contact" || item == "move_contact")
        {
            if (!currentSetIsUserSet())
            {
                return;
            }
            const std::string set_name = currentSet();
            if (item == "move_contact")
            {
                showMoveToSet(ids, set_name);
                return;
            }
            askRemoveFromSet(ids, set_name);
        }
        else if (item == "profile")
        {
            for (const LLUUID& id : ids)
            {
                LLAvatarActions::showProfile(id);
            }
        }
        else if (item == "im")
        {
            if (several)
            {
                LLAvatarActions::startConference(ids);
            }
            else
            {
                LLAvatarActions::startIM(ids.front());
            }
        }
        else if (item == "teleport")
        {
            LLAvatarActions::offerTeleport(ids);
        }
        else if (item == "set_pseudonym")
        {
            askSetPseudonym(ids);
        }
        else if (item == "remove_pseudonym")
        {
            for (const LLUUID& id : ids)
            {
                if (contact_sets.hasPseudonym(id))
                {
                    contact_sets.clearPseudonym(id);
                }
            }
        }
        else if (item == "remove_display_name")
        {
            for (const LLUUID& id : ids)
            {
                if (!contact_sets.hasDisplayNameRemoved(id))
                {
                    contact_sets.removeDisplayName(id);
                }
            }
        }
    }

    void PeopleTab::onFilterEdit(const std::string& filter)
    {
        std::string trimmed = filter;
        LLStringUtil::trimHead(trimmed);
        if (trimmed == mFilter || !mList)
        {
            return;
        }
        mFilter = trimmed;
        mList->setNameFilter(mFilter);
    }

    void PeopleTab::onListDoubleClicked(LLUICtrl* ctrl)
    {
        const LLAvatarListItem* item = dynamic_cast<const LLAvatarListItem*>(ctrl);
        if (!item || item->getAvatarId() == gAgentID || !namesShown(uuid_vec_t{ item->getAvatarId() }))
        {
            return;
        }
        LLAvatarActions::startIM(item->getAvatarId());
    }

    void PeopleTab::startIMWithSelection()
    {
        onPeopleAction("im");
    }

    bool PeopleTab::pointOnList(S32 x, S32 y) const
    {
        S32 list_x = 0;
        S32 list_y = 0;
        return mList && localPointToOtherView(x, y, &list_x, &list_y, mList) && mList->pointInView(list_x, list_y);
    }

    bool PeopleTab::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                      void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
    {
        // A person or a calling card dropped on the list goes into the shown set (Firestorm FIRE-31146).
        // The list (a scroll container) takes every drag itself, so the tab looks first.
        if ((cargo_type != DAD_PERSON && cargo_type != DAD_CALLINGCARD) || !pointOnList(x, y))
        {
            return LLPanel::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
        }
        if (!drop)
        {
            // Only for the list to scroll when the drag nears its edge; the answer is ours.
            LLPanel::handleDragAndDrop(x, y, mask, false, cargo_type, cargo_data, accept, tooltip_msg);
        }

        const LLUUID avatar_id = draggedAvatar(cargo_type, cargo_data);
        const bool can_add = avatar_id.notNull() && currentSetIsUserSet() && namesShown(uuid_vec_t{ avatar_id });
        if (can_add && drop)
        {
            LGGContactSets::instance().addToSet(uuid_vec_t{ avatar_id }, currentSet());
        }
        *accept = can_add ? ACCEPT_YES_MULTI : ACCEPT_NO;
        return true;
    }

    //============================================================================ ProfileSetsText

    ProfileSetsText::ProfileSetsText(const Params& p)
    :   LLTextBox(p)
    {
        // Hidden by the XUI (visible="false") until refresh() has a set to show; the label is read
        // there, after the factory has given the XUI value.
        setClickedCallback([](void*)
        {
            LLFloaterSidePanelContainer::showPanel("people", "panel_people",
                                                   LLSD().with("people_panel_tab_name", PEOPLE_TAB_NAME));
        });
    }

    void ProfileSetsText::setAvatarId(const LLUUID& avatar_id)
    {
        if (!mSetsChangedConnection.connected())
        {
            mSetsChangedConnection = LGGContactSets::instance().setContactSetChangeCallback(
                [this](LGGContactSets::EContactSetUpdate) { refresh(); });
        }
        mAvatarId = avatar_id;
        refresh();
    }

    void ProfileSetsText::refresh()
    {
        static LLCachedControl<bool> colorize(gSavedSettings, "FSContactSetsColorizeFriends", false);

        const LGGContactSets& contact_sets = LGGContactSets::instance();
        LGGContactSets::string_vec_t names;
        if (mAvatarId.notNull())
        {
            for (const std::string& set_name : contact_sets.getFriendSets(mAvatarId))
            {
                if (!set_name.empty() && !contact_sets.isInternalSetName(set_name))
                {
                    names.push_back(set_name);
                }
            }
        }

        // What would be shown: the names, and their colours when coloured.
        std::string key = colorize ? "c" : "-";
        for (const std::string& set_name : names)
        {
            key += set_name;
            key += '\n';
            if (colorize)
            {
                const LLColor4 color = contact_sets.getSetColor(set_name);
                key += llformat("%.3f %.3f %.3f %.3f;", color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], color.mV[VALPHA]);
            }
        }
        if (key == mShownKey)
        {
            return;
        }
        mShownKey = key;

        if (!mLabelRead)
        {
            mLabel = getText(); // the XUI value, before this replaces it
            mLabelRead = true;
        }
        setVisible(!names.empty());
        if (names.empty())
        {
            return;
        }

        setText(LLStringUtil::null);
        appendText(mLabel + " ", false);
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i > 0)
            {
                appendText(", ", false);
            }
            LLStyle::Params style;
            if (colorize)
            {
                const LLColor4 color = contact_sets.getSetColor(names[i]);
                style.color = color;
                style.readonly_color = color;
            }
            appendText(names[i], false, style);
        }
    }

    //============================================================================ avatar picker

    bool fillPickerSets(LLComboBox* sets)
    {
        if (!sets)
        {
            return false;
        }
        const std::string previous = sets->getSimple();
        sets->clearRows();
        const LGGContactSets::string_vec_t names = LGGContactSets::instance().getAllContactSets();
        for (const std::string& set_name : names)
        {
            sets->add(set_name);
        }
        sets->sortByName();
        sets->setEnabled(!names.empty());
        if (names.empty())
        {
            return false;
        }
        if (previous.empty() || !sets->setSimple(previous))
        {
            sets->selectFirstItem();
        }
        return true;
    }

    bool fillPickerMembers(const LLComboBox* sets, LLScrollListCtrl* members, const LLUUID& skip_id, PickerSelection selection)
    {
        if (!sets || !members)
        {
            return true;
        }

        uuid_vec_t keep_selected;
        if (selection == PickerSelection::KEEP)
        {
            for (const LLScrollListItem* item : members->getAllSelected())
            {
                keep_selected.push_back(item->getUUID());
            }
        }
        members->deleteAllItems();

        const LGGContactSets::ContactSet* set = LGGContactSets::instance().getContactSet(sets->getSimple());
        if (!set)
        {
            return true;
        }

        bool all_names = true;
        for (const LLUUID& id : set->mFriends)
        {
            if (id.isNull() || id == skip_id)
            {
                continue;
            }
            LLSD row;
            row["id"] = id;
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(id, &av_name))
            {
                row["columns"][0]["column"] = "name";
                row["columns"][0]["value"] = av_name.getDisplayName();
                row["columns"][1]["column"] = "username";
                row["columns"][1]["value"] = av_name.getUserName();
            }
            else
            {
                row["columns"][0]["column"] = "name";
                row["columns"][0]["value"] = gCacheName->getDefaultName();
                all_names = false; // asked for by get(); the next fill has it
            }
            members->addElement(row);
        }
        members->sortByColumnIndex(0, true);

        if (selection == PickerSelection::ALL)
        {
            members->selectAll();
        }
        else if (!keep_selected.empty())
        {
            members->selectMultiple(keep_selected);
        }
        return all_names;
    }
}
