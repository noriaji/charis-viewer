/**
 * @file grouptitlesfloater.cpp
 * @brief Group titles: the window (plus/grouptitles, from Firestorm's fsfloatergrouptitles.cpp).
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Opened where Firestorm opens it: the button of the People window's Groups tab (panel_people.xml, Firestorm:
// the Groups tab of its contacts window) and the toolbar button plus_group_titles (commands_plus.xml).
// Firestorm has no menu item for it.
//
// Every title of every group the agent is in, the group (bold: the active title) and the regions it is given
// to (Auto Region, grouptitles.cpp). The titles are asked of the server when the window opens and with the
// refresh button, one request per group; the list is built again (on the next idle, once however many
// replies and group changes came in the frame) from the group manager's titles and the agent's groups.

#include "llviewerprecompiledheaders.h"

#include "grouptitles.h"
#include "plus.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llevent.h"
#include "llfiltereditor.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llgroupactions.h"
#include "llgroupmgr.h"
#include "llmenugl.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llviewermenu.h"

#include <map>
#include <memory>
#include <set>

namespace plus::grouptitles
{
    namespace
    {
        const char* const FLOATER_NAME = "plus_group_titles";

        class GroupTitlesFloater;

        // Tells the window when a group's titles came in.
        class TitlesObserver : public LLGroupMgrObserver
        {
        public:
            TitlesObserver(const LLUUID& group_id, const LLHandle<GroupTitlesFloater>& floater)
                : LLGroupMgrObserver(group_id), mFloater(floater)
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
            void changed(LLGroupChange gc) override;

        private:
            LLHandle<GroupTitlesFloater> mFloater;
        };

        class GroupTitlesFloater : public LLFloater, public LLOldEvents::LLSimpleListener
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                mActivate    = getChild<LLButton>("btnActivate");
                mInfo        = getChild<LLButton>("btnInfo");
                mSetRegion   = getChild<LLButton>("btnSetRegion");
                mTypeRegion  = getChild<LLButton>("btnSetRegionManual");
                mClearRegion = getChild<LLButton>("btnClearRegion");
                mNoneCheck   = getChild<LLCheckBoxCtrl>("none_on_unassigned");
                mList        = getChild<LLScrollListCtrl>("title_list");
                mFilter      = getChild<LLFilterEditor>("filter_input");

                mActivate->setCommitCallback([this](LLUICtrl*, const LLSD&) { forSelected(&activateTitle); });
                getChild<LLButton>("btnRefresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { requestTitles(); });
                mInfo->setCommitCallback([this](LLUICtrl*, const LLSD&) { forSelected(&openGroupInfo); });
                mSetRegion->setCommitCallback([this](LLUICtrl*, const LLSD&) { forSelected(&setAssignmentForCurrentRegion); });
                mTypeRegion->setCommitCallback([this](LLUICtrl*, const LLSD&) { forSelected(&askRegionName); });
                mClearRegion->setCommitCallback([this](LLUICtrl*, const LLSD&) { clearRegion(); });
                mNoneCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { setNoneOnUnassigned(mNoneCheck->get()); });
                mList->setDoubleClickCallback([this]()
                                              {
                                                  if (mActivate->getEnabled())  // not a row of a group the agent is not in
                                                  {
                                                      forSelected(&activateTitle);
                                                  }
                                              });
                mList->setCommitCallback([this](LLUICtrl*, const LLSD&)
                                         {
                                             mSelectActive = false;     // the user chose: kept over rebuilds
                                             updateButtons();
                                         });
                mFilter->setCommitCallback([this](LLUICtrl*, const LLSD& value) { setFilter(value.asString()); });

                mList->sortByColumn("title_sort_column", true);
                mAssignmentsChanged = onAssignmentsChanged([this]() { rebuildLater(); });
                gAgent.addListener(this, "new group");   // the groups, the active one (fired once per group)
                updateButtons();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                mNoneCheck->set(noneOnUnassigned());
                mSelectActive = true;
                requestTitles();
                mList->setFocus(true);
            }

            // Ctrl+F: the filter, as in Firestorm.
            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (key == 'F' && mask == MASK_CONTROL)
                {
                    mFilter->setFocus(true);
                    return true;
                }
                return LLFloater::handleKeyHere(key, mask);
            }
            bool hasAccelerators() const override { return true; }

            bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD&) override
            {
                if (event->desc() == "new group")
                {
                    rebuildLater();
                }
                return false;   // others listen too
            }

            void rebuildLater()
            {
                if (mRebuildPending)
                {
                    return;
                }
                mRebuildPending = true;
                const LLHandle<GroupTitlesFloater> handle = getDerivedHandle<GroupTitlesFloater>();
                doOnIdleOneTime([handle]()
                                {
                                    if (GroupTitlesFloater* floater = handle.get())
                                    {
                                        floater->rebuild();
                                    }
                                });
            }

        private:
            explicit GroupTitlesFloater(const LLSD& key) : LLFloater(key) {}

            ~GroupTitlesFloater() override
            {
                gAgent.removeListener(this);
                mAssignmentsChanged.disconnect();
                deleteClearMenu();
                mObservers.clear();
            }

            // ---- the titles ---------------------------------------------------------------------

            // Every group's titles asked again (the window opened, the refresh button).
            void requestTitles()
            {
                mObservers.clear();
                for (const LLGroupData& group : gAgent.mGroups)
                {
                    requestGroup(group.mID);
                }
                rebuildLater();
            }

            void requestGroup(const LLUUID& group_id)
            {
                mObservers[group_id] = std::make_unique<TitlesObserver>(group_id, getDerivedHandle<GroupTitlesFloater>());
                LLGroupMgr::getInstance()->sendGroupTitlesRequest(group_id);
            }

            // A group joined while the window is open is asked for; a group left is forgotten.
            void followGroups()
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

            void rebuild()
            {
                mRebuildPending = false;
                followGroups();

                std::string keep;   // the row selected before, unless the active one is to be selected
                if (!mSelectActive)
                {
                    if (const LLScrollListItem* item = mList->getFirstSelected())
                    {
                        keep = item->getValue().asString();
                    }
                }
                mActiveKey.clear();
                mList->clearRows();

                const LLUUID& active_group = gAgent.getGroupID();
                addRow(LLUUID::null, LLUUID::null, getString("NoGroupTitle"), LLTrans::getString("GroupsNone"),
                       active_group.isNull(), false);
                for (const LLGroupData& group : gAgent.mGroups)
                {
                    const LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group.mID);
                    if (!data)
                    {
                        continue;
                    }
                    for (const LLGroupTitle& title : data->mTitles)
                    {
                        addRow(group.mID, title.mRoleID, title.mTitle, group.mName,
                               title.mSelected && group.mID == active_group, true);
                    }
                }
                // Regions given to a title of a group the agent is not in (left it): a row of their own, so the
                // assignment can be cleared (Auto Region skips it).
                for (const auto& [group_id, role_id] : assignedTitles())
                {
                    if (group_id.notNull() && !gAgent.isInGroup(group_id))
                    {
                        addRow(group_id, role_id, getString("UnknownTitle"), getString("NotMember"), false, true);
                    }
                }

                // Else the active title, once its row is there; the user's choice is kept from then on.
                if ((keep.empty() || !mList->selectByValue(keep)) && !mActiveKey.empty())
                {
                    mList->selectByValue(mActiveKey);
                    mSelectActive = false;
                }
                mList->scrollToShowSelected();
                updateButtons();
            }

            static std::string rowKey(const LLUUID& group_id, const LLUUID& role_id)
            {
                return group_id.asString() + role_id.asString();   // only the pair is unique
            }

            void addRow(const LLUUID& group_id, const LLUUID& role_id, const std::string& title,
                        const std::string& group_name, bool active, bool is_group)
            {
                if (!mFilterUpper.empty())
                {
                    std::string title_upper = title;
                    std::string group_upper = group_name;
                    LLStringUtil::toUpper(title_upper);
                    LLStringUtil::toUpper(group_upper);
                    if (title_upper.find(mFilterUpper) == std::string::npos && group_upper.find(mFilterUpper) == std::string::npos)
                    {
                        return;
                    }
                }

                const std::string key   = rowKey(group_id, role_id);
                const char* const style = active ? "BOLD" : "NORMAL";
                LLSD item;
                item["value"] = key;
                LLSD& columns = item["columns"];
                columns[0]["column"] = "grouptitle";
                columns[0]["value"]  = title;
                columns[0]["font"]["style"] = style;
                columns[1]["column"] = "groupname";
                columns[1]["value"]  = group_name;
                columns[1]["font"]["style"] = style;
                columns[2]["column"] = "regionname";
                columns[2]["value"]  = regionListForTitle(group_id, role_id);
                columns[2]["font"]["style"] = style;
                columns[3]["column"] = "role_id";
                columns[3]["value"]  = role_id;
                columns[4]["column"] = "group_id";
                columns[4]["value"]  = group_id;
                // "No group" first whichever way the list is sorted
                columns[5]["column"] = "title_sort_column";
                columns[5]["value"]  = is_group ? "1_" + title : "0";
                columns[6]["column"] = "name_sort_column";
                columns[6]["value"]  = is_group ? "1_" + group_name : "0";
                mList->addElement(item);
                if (active)
                {
                    mActiveKey = key;
                }
            }

            void setFilter(const std::string& text)
            {
                std::string upper = text;
                LLStringUtil::trimHead(upper);
                LLStringUtil::toUpper(upper);
                if (upper != mFilterUpper)
                {
                    mFilterUpper = upper;
                    rebuildLater();
                }
            }

            // ---- the selected row ---------------------------------------------------------------

            bool selected(LLUUID& group_id, LLUUID& role_id) const
            {
                const LLScrollListItem* item = mList->getFirstSelected();
                if (!item)
                {
                    return false;
                }
                group_id = item->getColumn(mList->getColumn("group_id")->mIndex)->getValue().asUUID();
                role_id  = item->getColumn(mList->getColumn("role_id")->mIndex)->getValue().asUUID();
                return true;
            }

            void updateButtons()
            {
                LLUUID group_id, role_id;
                const bool any = selected(group_id, role_id);
                // A row of a group the agent is not in can only have its regions cleared.
                const bool usable = any && (group_id.isNull() || gAgent.isInGroup(group_id));
                mActivate->setEnabled(usable);
                mInfo->setEnabled(any && group_id.notNull());
                mSetRegion->setEnabled(usable);
                mTypeRegion->setEnabled(usable);
                mClearRegion->setEnabled(any && !regionsForTitle(group_id, role_id).empty());
            }

            // The buttons' actions on the selected title (none selected: nothing).
            template <typename Action>
            void forSelected(Action action) const
            {
                LLUUID group_id, role_id;
                if (selected(group_id, role_id))
                {
                    action(group_id, role_id);
                }
            }

            static void openGroupInfo(const LLUUID& group_id, const LLUUID&)
            {
                if (group_id.notNull())
                {
                    LLGroupActions::show(group_id);
                }
            }

            void deleteClearMenu()
            {
                if (LLContextMenu* menu = mClearMenu.get())
                {
                    gMenuHolder->removeChild(menu);
                    delete menu;
                }
            }

            // One region: taken away at once. More: a menu of them under the button, and "all".
            void clearRegion()
            {
                LLUUID group_id, role_id;
                if (!selected(group_id, role_id))
                {
                    return;
                }
                const std::vector<std::string> regions = regionsForTitle(group_id, role_id);
                if (regions.size() <= 1)
                {
                    if (!regions.empty())
                    {
                        clearAssignmentByRegion(regions.front());
                    }
                    return;
                }

                deleteClearMenu();
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
                all_params.label(getString("ClearAllRegions"));
                all_params.name("clear_all_regions");
                all_params.on_click.function([group_id, role_id](LLUICtrl*, const LLSD&) { clearAssignment(group_id, role_id); });
                menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(all_params));

                mClearMenu = menu->getHandle();
                gMenuHolder->addChild(menu);
                S32 screen_x = 0, screen_y = 0;
                mClearRegion->localPointToScreen(0, mClearRegion->getRect().getHeight(), &screen_x, &screen_y);
                menu->show(screen_x, screen_y, mClearRegion);
            }

            LLButton*         mActivate    = nullptr;
            LLButton*         mInfo        = nullptr;
            LLButton*         mSetRegion   = nullptr;
            LLButton*         mTypeRegion  = nullptr;
            LLButton*         mClearRegion = nullptr;
            LLCheckBoxCtrl*   mNoneCheck   = nullptr;
            LLScrollListCtrl* mList        = nullptr;
            LLFilterEditor*   mFilter      = nullptr;

            std::string mFilterUpper;           // the filter in upper case (empty: all rows)
            std::string mActiveKey;             // rowKey() of the active title's row, if shown
            bool        mSelectActive = true;   // select the active title once its row is there (opening)
            bool        mRebuildPending = false;

            std::map<LLUUID, std::unique_ptr<TitlesObserver>> mObservers;   // the groups asked for
            boost::signals2::connection mAssignmentsChanged;
            LLHandle<LLContextMenu>     mClearMenu;
        };

        void TitlesObserver::changed(LLGroupChange gc)
        {
            if (gc == GC_TITLES)
            {
                if (GroupTitlesFloater* floater = mFloater.get())
                {
                    floater->rebuildLater();
                }
            }
        }

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_group_titles.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<GroupTitlesFloater>);
            // The toolbar button (commands_plus.xml) and the People window's Groups tab: Firestorm's icon.
            plus::declareUIImage("Plus_GroupTitles_Icon", "plus/group_titles_16.png");
            startAutoRegion();
        }

        plus::FeatureRegistration sRegistration({ "grouptitles", &init, {} });
    }
}
