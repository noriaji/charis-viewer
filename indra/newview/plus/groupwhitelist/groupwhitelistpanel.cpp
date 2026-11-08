/**
 * @file groupwhitelistpanel.cpp
 * @brief The group chat choice in the group window (General tab, "Me"):
 *        normal, blacklist or whitelist for this group, and the switch
 *        "only whitelisted groups" for all of them.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

// panel_group_general.xml (an official file, <!-- plus --> block) holds
// <panel class="plus_group_chat_filter" filename="panel_plus_group_chat_filter.xml"/>.
// The panel finds the group it shows from the group tab it is in
// (LLPanelGroupTab::getGroupID()), so LLPanelGroupGeneral needs no change.
// A choice is applied and saved at once (not by the group window's Apply button:
// it is the agent's own setting, not the group's).

#include "llviewerprecompiledheaders.h"

#include "groupwhitelist.h"

#include "llagent.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llpanel.h"
#include "llpanelgroup.h"

namespace
{
    using plus::groupwhitelist::Mode;

    // The values of the combo's items (panel_plus_group_chat_filter.xml).
    constexpr char VALUE_NORMAL[]    = "normal";
    constexpr char VALUE_BLACKLIST[] = "blacklist";
    constexpr char VALUE_WHITELIST[] = "whitelist";

    const char* modeValue(Mode mode)
    {
        switch (mode)
        {
        case Mode::Blacklist: return VALUE_BLACKLIST;
        case Mode::Whitelist: return VALUE_WHITELIST;
        case Mode::Normal:    break;
        }
        return VALUE_NORMAL;
    }

    Mode modeFromValue(const std::string& value)
    {
        if (value == VALUE_BLACKLIST)
        {
            return Mode::Blacklist;
        }
        if (value == VALUE_WHITELIST)
        {
            return Mode::Whitelist;
        }
        return Mode::Normal;
    }

    class GroupChatFilterPanel : public LLPanel
    {
    public:
        bool postBuild() override
        {
            mModeCombo     = getChild<LLComboBox>("plus_group_chat_mode");
            mWhitelistOnly = getChild<LLCheckBoxCtrl>("plus_group_chat_whitelist_only");

            mModeCombo->setCommitCallback([this](LLUICtrl*, const LLSD&)
            {
                plus::groupwhitelist::setMode(groupId(), modeFromValue(mModeCombo->getValue().asString()));
            });
            mWhitelistOnly->setCommitCallback([this](LLUICtrl*, const LLSD&)
            {
                plus::groupwhitelist::setWhitelistOnly(mWhitelistOnly->get());
            });
            return true;
        }

        void draw() override
        {
            refresh();
            LLPanel::draw();
        }

    private:
        // The group of the group tab this panel is in; null while a group is being created.
        LLUUID groupId() const
        {
            for (const LLView* view = getParent(); view; view = view->getParent())
            {
                if (const LLPanelGroupTab* tab = dynamic_cast<const LLPanelGroupTab*>(view))
                {
                    return tab->getGroupID();
                }
            }
            return LLUUID::null;
        }

        // Shows the group's choice; only a member's group has chat to choose for.
        void refresh()
        {
            const LLUUID group_id = groupId();
            const bool   member   = group_id.notNull() && gAgent.isInGroup(group_id, true);
            const U32    revision = plus::groupwhitelist::revision();
            if (group_id == mShownGroup && member == mShownMember && revision == mShownRevision)
            {
                return;
            }
            mShownGroup    = group_id;
            mShownMember   = member;
            mShownRevision = revision;

            mModeCombo->setValue(modeValue(plus::groupwhitelist::mode(group_id)));
            mModeCombo->setEnabled(member);
            mWhitelistOnly->set(plus::groupwhitelist::whitelistOnly());
        }

        LLComboBox*     mModeCombo     = nullptr;
        LLCheckBoxCtrl* mWhitelistOnly = nullptr;

        LLUUID mShownGroup;
        bool   mShownMember   = false;
        U32    mShownRevision = U32_MAX;
    };

    LLPanelInjector<GroupChatFilterPanel> sPanelRegistration("plus_group_chat_filter");
}
