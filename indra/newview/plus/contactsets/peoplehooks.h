/**
 * @file peoplehooks.h
 * @brief What official files call for the contact sets of the People window, the profile, the
 *        avatar picker and the avatar menus (plus/contactsets/peopletab.cpp, research/333)
 *
 * The only header of plus/contactsets/peopletab that official files include: declarations only,
 * no heavy headers (the classes are in peopletab.h). Main thread only.
 *
 * This is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#ifndef PLUS_CONTACTSETS_PEOPLEHOOKS_H
#define PLUS_CONTACTSETS_PEOPLEHOOKS_H

#include "lluuid.h"
#include "contactsets.h" // showAddToSet(), showMoveToSet(), namesShown() (declarations only)

#include <string>

class LLComboBox;
class LLPanel;
class LLScrollListCtrl;

namespace plus::contactsets
{
    // The name of the tab in panel_people.xml (opened with "people_panel_tab_name").
    constexpr char PEOPLE_TAB_NAME[] = "contact_sets_panel";

    // showAddToSet(), showMoveToSet() and namesShown() (the "Add to Set" window and the RLV check of the
    // menus): contactsets.h, included above.

    // llpanelpeople.cpp (getCurrentItemID(s)): when tab is the Contact Sets tab, its selected
    // people are appended to ids and true is returned; false (ids untouched) for any other tab.
    bool getTabSelection(const LLPanel* tab, uuid_vec_t& ids);

    // llpanelprofile.cpp (onOpen): the line of the profile that lists the avatar's contact sets
    // (widget "plus_contact_sets_text" in profile_panel) follows this avatar. Nothing without it.
    void showProfileSets(LLPanel* profile_panel, const LLUUID& avatar_id);

    // llfloateravatarpicker.cpp, the "Contact Sets" tab. fillPickerSets(): the user's sets into
    // the combo, keeping the chosen one if it is still there; false when there are none.
    // fillPickerMembers(): the people of the chosen set into the list, without skip_id (the
    // agent when the picker leaves it out, else null); false while some name is not in the name
    // cache yet (asked for; fill again later).
    enum class PickerSelection : unsigned char
    {
        KEEP, // the people selected before stay selected
        ALL   // all of the set (a picker that takes more than one)
    };
    bool fillPickerSets(LLComboBox* sets);
    bool fillPickerMembers(const LLComboBox* sets, LLScrollListCtrl* members, const LLUUID& skip_id, PickerSelection selection);
}

#endif // PLUS_CONTACTSETS_PEOPLEHOOKS_H
