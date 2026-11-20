/**
 * @file peopletab.h
 * @brief Contact sets in the People window and the places that open them (research/333)
 *
 * Ported from Firestorm: the contact sets part of llpanelpeople.cpp (LLPanelPeople::updateContactSets()
 * and the rest of [FS:CR] Contact sets), LLAvatarActions::addToContactSet()/moveToContactSet(), the
 * contact sets of the profile (FIRE-32401) and of the avatar picker (FIRE-34809). The data is
 * LGGContactSets (lggcontactsets.h, the cs-core branch); this file only reads it and asks it to change.
 *
 * - PeopleTab: the "Contact Sets" tab of the People window (panel class "plus_contact_sets_panel",
 *   panel_people_contact_sets.xml). It owns its combo, list, filter, buttons and menus; llpanelpeople.cpp
 *   only asks it for the selection (getCurrentItemID(s)).
 * - ProfileSetsText: the line "Contact sets: A, B" beside "Group memberships" in the profile
 *   (widget "plus_contact_sets_text").
 * - The avatar picker's "Contact Sets" tab and the "Add to Set" of the menus: peoplehooks.h (what
 *   official files include; this header is for peopletab.cpp).
 *
 * Main thread only (UI). Changes of the sets come by LGGContactSets::mChangedSignal; a hidden tab
 * only marks itself stale and rebuilds when it is shown (no work while the People window is closed).
 *
 * The contact sets part of the software is LGPL 2.1 (Firestorm); see plus/contactsets/lggcontactsets.h.
 */

#ifndef PLUS_CONTACTSETS_PEOPLETAB_H
#define PLUS_CONTACTSETS_PEOPLETAB_H

#include "llpanel.h"
#include "lltextbox.h"
#include "llcallingcard.h" // LLFriendObserver
#include "lggcontactsets.h"
#include "peoplehooks.h"

#include <boost/signals2.hpp>

class LLAvatarList;

namespace plus::contactsets
{
    class PeopleTab : public LLPanel, public LLFriendObserver
    {
        LOG_CLASS(PeopleTab);

    public:
        PeopleTab();
        ~PeopleTab() override;

        bool postBuild() override;
        void onVisibilityChange(bool new_visibility) override;
        bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                               void* cargo_data, EAcceptance* accept, std::string& tooltip_msg) override;

        // LLFriendObserver: a friend came, went, or changed online status.
        void changed(U32 mask) override;

        // The selected people of the list (for LLPanelPeople::getCurrentItemID(s), getTabSelection()).
        void getSelectedUUIDs(uuid_vec_t& ids) const;

    private:
        // How much of the tab no longer matches LGGContactSets. MEMBERS: the people of the shown
        // set; SETS: the list of sets in the combo too (and then the people).
        enum class Stale : U8
        {
            NONE,
            MEMBERS,
            SETS
        };

        void onSetsChanged(LGGContactSets::EContactSetUpdate type);
        void markStale(Stale stale);
        void rebuildIfStale();
        void rebuildSetCombo();
        void rebuildMemberList();
        void applySorting();

        // The value of the combo: a set of the user's, or CS_SET_ALL_SETS / CS_SET_NO_SETS / CS_SET_PSEUDONYM.
        std::string currentSet() const;
        // The shown set is one of the user's (people can be added to it and removed from it).
        bool currentSetIsUserSet() const;
        bool sortByOnlineStatus() const;
        // The selection when the menus may act on it: 1..MAX_SELECTED people whose names are shown.
        bool usableSelection(uuid_vec_t& ids) const;

        bool onEnable(const LLSD& userdata);
        void onAction(const LLSD& userdata);
        bool onSetAction(const std::string& item);    // false: not an action on the set
        void onPeopleAction(const std::string& item); // on the usable selection
        void onFilterEdit(const std::string& filter);
        void onListDoubleClicked(LLUICtrl* ctrl);
        void startIMWithSelection();
        bool pointOnList(S32 x, S32 y) const;

        LLComboBox* mSetCombo = nullptr;
        LLAvatarList* mList = nullptr;
        std::string mFilter; // as given to the list (trimmed at the head)
        Stale mStale = Stale::SETS; // built on the first show
        boost::signals2::scoped_connection mSetsChangedConnection;
        boost::signals2::scoped_connection mFilterConnection;
    };

    // The contact sets of one avatar as a line of text (in the colours of the sets when
    // FSContactSetsColorizeFriends is on). Hidden while the avatar is in none. A click opens the
    // Contact Sets tab of the People window. The XUI value is the label in front of the names.
    class ProfileSetsText : public LLTextBox
    {
        LOG_CLASS(ProfileSetsText);

    public:
        struct Params : public LLInitParam::Block<Params, LLTextBox::Params>
        {
        };

        void setAvatarId(const LLUUID& avatar_id);

    protected:
        friend class LLUICtrlFactory;
        explicit ProfileSetsText(const Params& p);

    private:
        void refresh();

        LLUUID mAvatarId;
        std::string mLabel;     // the XUI value ("Contact sets:"), read by the first refresh()
        bool mLabelRead = false;
        std::string mShownKey;  // what the text shows now (names and colours), to skip equal rewrites
        boost::signals2::scoped_connection mSetsChangedConnection;
    };
}

#endif // PLUS_CONTACTSETS_PEOPLETAB_H
