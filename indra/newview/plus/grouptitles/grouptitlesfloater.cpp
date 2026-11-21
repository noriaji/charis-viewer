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
// The asking and the rows are GroupTitlesData's (grouptitlescore.cpp), shared with the RmlUi window
// (grouptitlesrml.cpp). This XUI window is frozen: it is used in safe mode and with PlusRmlUiWindows off.

#include "llviewerprecompiledheaders.h"

#include "grouptitles.h"
#include "grouptitlescore.h"
#include "grouptitlesrml.h"
#include "plus.h"
#include "plus/safemode/safemode.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llfiltereditor.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llgroupactions.h"
#include "llmenugl.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"

namespace plus::grouptitles
{
    namespace
    {
        const char* const FLOATER_NAME = "plus_group_titles";

        class GroupTitlesFloater : public LLFloater
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
                getChild<LLButton>("btnRefresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mData.requestAll(); });
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
                updateButtons();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                mNoneCheck->set(noneOnUnassigned());
                mSelectActive = true;
                mData.requestAll();
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
            explicit GroupTitlesFloater(const LLSD& key)
                : LLFloater(key),
                  mData([handle = getDerivedHandle<GroupTitlesFloater>()]()
                        {
                            if (GroupTitlesFloater* floater = handle.get())
                            {
                                floater->rebuildLater();
                            }
                        })
            {}

            ~GroupTitlesFloater() override
            {
                deleteClearMenu(mClearMenu);
            }

            // ---- the titles ---------------------------------------------------------------------

            void rebuild()
            {
                mRebuildPending = false;

                std::string keep;   // the row selected before, unless the active one is to be selected
                if (!mSelectActive)
                {
                    if (const LLScrollListItem* item = mList->getFirstSelected())
                    {
                        keep = item->getValue().asString();
                    }
                }
                mList->clearRows();

                TitleLabels labels;
                labels.no_title = getString("NoGroupTitle");
                labels.none_group = LLTrans::getString("GroupsNone");
                labels.unknown_title = getString("UnknownTitle");
                labels.not_member = getString("NotMember");
                std::string active_key;
                for (const TitleRow& row : mData.rows(labels, mFilterUpper, active_key))
                {
                    addRow(row);
                }

                // Else the active title, once its row is there; the user's choice is kept from then on.
                if ((keep.empty() || !mList->selectByValue(keep)) && !active_key.empty())
                {
                    mList->selectByValue(active_key);
                    mSelectActive = false;
                }
                mList->scrollToShowSelected();
                updateButtons();
            }

            void addRow(const TitleRow& title)
            {
                const char* const style = title.active ? "BOLD" : "NORMAL";
                LLSD item;
                item["value"] = title.key;
                LLSD& columns = item["columns"];
                columns[0]["column"] = "grouptitle";
                columns[0]["value"]  = title.title;
                columns[0]["font"]["style"] = style;
                columns[1]["column"] = "groupname";
                columns[1]["value"]  = title.group_name;
                columns[1]["font"]["style"] = style;
                columns[2]["column"] = "regionname";
                columns[2]["value"]  = title.regions;
                columns[2]["font"]["style"] = style;
                columns[3]["column"] = "role_id";
                columns[3]["value"]  = title.role_id;
                columns[4]["column"] = "group_id";
                columns[4]["value"]  = title.group_id;
                // "No group" first whichever way the list is sorted
                columns[5]["column"] = "title_sort_column";
                columns[5]["value"]  = title.is_group ? "1_" + title.title : "0";
                columns[6]["column"] = "name_sort_column";
                columns[6]["value"]  = title.is_group ? "1_" + title.group_name : "0";
                mList->addElement(item);
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
                const bool usable = any && GroupTitlesData::usable(group_id);
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

            // One region: taken away at once. More: a menu of them under the button, and "all".
            void clearRegion()
            {
                LLUUID group_id, role_id;
                if (!selected(group_id, role_id))
                {
                    return;
                }
                S32 screen_x = 0, screen_y = 0;
                mClearRegion->localPointToScreen(0, mClearRegion->getRect().getHeight(), &screen_x, &screen_y);
                clearRegionsOf(group_id, role_id, getString("ClearAllRegions"), screen_x, screen_y, mClearRegion, mClearMenu);
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
            bool        mSelectActive = true;   // select the active title once its row is there (opening)
            bool        mRebuildPending = false;

            LLHandle<LLContextMenu>     mClearMenu;
            GroupTitlesData             mData;      // last: its signals go first when the window goes
        };

        // The window is the RmlUi one (grouptitlesrml.cpp, research/386). This XUI version is frozen (not changed any
        // more: a setting added to the window goes to both) and is used in safe mode, where RmlUi is not started, and
        // with PlusRmlUiWindows off (the choice is made once, here: it takes effect at the next start).
        void init()
        {
            if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
            {
                LLFloaterReg::add(FLOATER_NAME, "floater_plus_group_titles.xml",
                                  (LLFloaterBuildFunc)&LLFloaterReg::build<GroupTitlesFloater>);
            }
            else
            {
                addRmlFloater(FLOATER_NAME);
            }
            // The toolbar button (commands_plus.xml) and the People window's Groups tab: Firestorm's icon.
            plus::declareUIImage("Plus_GroupTitles_Icon", "plus/group_titles_16.png");
            startAutoRegion();
        }

        plus::FeatureRegistration sRegistration({ "grouptitles", &init, {} });
    }
}
