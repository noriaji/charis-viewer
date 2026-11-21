/**
 * @file grouptitlescore.cpp
 * @brief Group titles: the titles of the agent's groups as rows (plus/grouptitles; from Firestorm's fsfloatergrouptitles.cpp).
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Copyright (c) 2012 Ansariel Hiller @ Second Life
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

#include "grouptitlescore.h"
#include "grouptitles.h"

#include "llagent.h"
#include "llgroupmgr.h"
#include "llmenugl.h"
#include "lluictrlfactory.h"
#include "llviewermenu.h"

#include <set>

namespace plus::grouptitles
{
    // Tells the window when a group's titles came in.
    class TitlesObserver : public LLGroupMgrObserver
    {
    public:
        TitlesObserver(const LLUUID& group_id, std::function<void()> changed)
            : LLGroupMgrObserver(group_id), mChanged(std::move(changed))
        {
            LLGroupMgr::getInstance()->addObserver(this);
        }
        ~TitlesObserver() override
        {
            if (LLGroupMgr::instanceExists())
            {
                LLGroupMgr::getInstance()->removeObserver(this);
            }
        }
        void changed(LLGroupChange gc) override
        {
            if (gc == GC_TITLES && mChanged)
            {
                mChanged();
            }
        }

    private:
        std::function<void()> mChanged;
    };

    GroupTitlesData::GroupTitlesData(std::function<void()> changed)
        : mChanged(std::move(changed))
    {
        mAssignmentsChanged = onAssignmentsChanged([this]() { if (mChanged) { mChanged(); } });
        gAgent.addListener(this, "new group");   // the groups, the active one (fired once per group)
    }

    GroupTitlesData::~GroupTitlesData()
    {
        gAgent.removeListener(this);
        mAssignmentsChanged.disconnect();
        mObservers.clear();
    }

    bool GroupTitlesData::handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD&)
    {
        if (event->desc() == "new group" && mChanged)
        {
            mChanged();
        }
        return false;   // others listen too
    }

    void GroupTitlesData::requestAll()
    {
        mObservers.clear();
        for (const LLGroupData& group : gAgent.mGroups)
        {
            requestGroup(group.mID);
        }
        if (mChanged)
        {
            mChanged();
        }
    }

    void GroupTitlesData::requestGroup(const LLUUID& group_id)
    {
        mObservers[group_id] = std::make_unique<TitlesObserver>(group_id, mChanged);
        LLGroupMgr::getInstance()->sendGroupTitlesRequest(group_id);
    }

    // A group joined while the window is open is asked for; a group left is forgotten.
    void GroupTitlesData::followGroups()
    {
        std::set<LLUUID> current;
        for (const LLGroupData& group : gAgent.mGroups)
        {
            current.insert(group.mID);
            if (mObservers.find(group.mID) == mObservers.end())
            {
                requestGroup(group.mID);
            }
        }
        for (auto it = mObservers.begin(); it != mObservers.end();)
        {
            it = current.count(it->first) ? std::next(it) : mObservers.erase(it);
        }
    }

    bool GroupTitlesData::usable(const LLUUID& group_id)
    {
        return group_id.isNull() || gAgent.isInGroup(group_id);
    }

    std::vector<TitleRow> GroupTitlesData::rows(const TitleLabels& labels, const std::string& filter_upper, std::string& active_key)
    {
        followGroups();
        active_key.clear();
        std::vector<TitleRow> out;

        auto add = [&](const LLUUID& group_id, const LLUUID& role_id, const std::string& title, const std::string& group_name,
                       bool active, bool is_group)
        {
            if (!filter_upper.empty())
            {
                std::string title_upper = title;
                std::string group_upper = group_name;
                LLStringUtil::toUpper(title_upper);
                LLStringUtil::toUpper(group_upper);
                if (title_upper.find(filter_upper) == std::string::npos && group_upper.find(filter_upper) == std::string::npos)
                {
                    return;
                }
            }
            TitleRow row;
            row.group_id = group_id;
            row.role_id = role_id;
            row.key = group_id.asString() + role_id.asString();   // only the pair is unique
            row.title = title;
            row.group_name = group_name;
            row.regions = regionListForTitle(group_id, role_id);
            row.active = active;
            row.is_group = is_group;
            if (active)
            {
                active_key = row.key;
            }
            out.push_back(std::move(row));
        };

        const LLUUID& active_group = gAgent.getGroupID();
        add(LLUUID::null, LLUUID::null, labels.no_title, labels.none_group, active_group.isNull(), false);
        for (const LLGroupData& group : gAgent.mGroups)
        {
            const LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group.mID);
            if (!data)
            {
                continue;
            }
            for (const LLGroupTitle& title : data->mTitles)
            {
                add(group.mID, title.mRoleID, title.mTitle, group.mName, title.mSelected && group.mID == active_group, true);
            }
        }
        // Regions given to a title of a group the agent is not in (left it): a row of their own, so the
        // assignment can be cleared (Auto Region skips it).
        for (const auto& [group_id, role_id] : assignedTitles())
        {
            if (group_id.notNull() && !gAgent.isInGroup(group_id))
            {
                add(group_id, role_id, labels.unknown_title, labels.not_member, false, true);
            }
        }
        return out;
    }

    void deleteClearMenu(LLHandle<LLContextMenu>& handle)
    {
        if (LLContextMenu* menu = handle.get())
        {
            gMenuHolder->removeChild(menu);
            delete menu;
        }
        handle.markDead();
    }

    void clearRegionsOf(const LLUUID& group_id, const LLUUID& role_id, const std::string& all_label, S32 screen_x, S32 screen_y,
                        LLView* spawning, LLHandle<LLContextMenu>& menu_handle)
    {
        const std::vector<std::string> regions = regionsForTitle(group_id, role_id);
        if (regions.size() <= 1)
        {
            if (!regions.empty())
            {
                clearAssignmentByRegion(regions.front());
            }
            return;
        }

        deleteClearMenu(menu_handle);
        LLContextMenu::Params menu_params;
        menu_params.name("clear_region_menu");
        menu_params.visible(false);
        LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);
        for (size_t i = 0; i < regions.size(); ++i)
        {
            LLMenuItemCallGL::Params item_params;
            item_params.label(regions[i]);
            item_params.name("region_" + std::to_string(i));
            const std::string region = regions[i];
            item_params.on_click.function([region](LLUICtrl*, const LLSD&) { clearAssignmentByRegion(region); });
            menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(item_params));
        }
        menu->addSeparator();
        LLMenuItemCallGL::Params all_params;
        all_params.label(all_label);
        all_params.name("clear_all_regions");
        all_params.on_click.function([group_id, role_id](LLUICtrl*, const LLSD&) { clearAssignment(group_id, role_id); });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(all_params));

        menu_handle = menu->getHandle();
        gMenuHolder->addChild(menu);
        menu->show(screen_x, screen_y, spawning);
    }
}
