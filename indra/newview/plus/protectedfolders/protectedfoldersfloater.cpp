/**
 * @file protectedfoldersfloater.cpp
 * @brief Protected folders: the window (the list, remove from the list, narrow by name).
 *
 * Ported from Firestorm's fsfloaterprotectedfolders.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2020&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2020 Ansariel Hiller @ Second Life
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

// Menu: "Protected Folders" in menu_plus.xml (Floater.Toggle plus_protected_folders).
// Ported from Firestorm's FSFloaterProtectedFolders. A double click shows the folder in the
// inventory. A folder the inventory does not know (not loaded yet, or deleted by another
// viewer) is listed in italics; the list is made again once the inventory has it.

#include "llviewerprecompiledheaders.h"

#include "protectedfolders.h"
#include "plus.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsearcheditor.h"
#include "lltimer.h"

namespace plus::protectedfolders
{
    namespace
    {
        const char* FLOATER_NAME = "plus_protected_folders";
        constexpr F32 RETRY_SECONDS = 2.f;          // how often to look again for folders the inventory lacked

        class ProtectedFoldersFloater : public LLFloater
        {
        public:
            explicit ProtectedFoldersFloater(const LLSD& key)
                : LLFloater(key)
            {
            }

            ~ProtectedFoldersFloater() override
            {
                mChangedConnection.disconnect();
            }

            bool postBuild() override
            {
                mList = getChild<LLScrollListCtrl>("folder_list");
                mList->setDoubleClickCallback([this]() { onDoubleClick(); });

                mRemove = getChild<LLButton>("remove_btn");
                mRemove->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemove(); });

                mFilter = getChild<LLSearchEditor>("filter_input");
                mFilter->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateList(); });

                mChangedConnection = onChanged([this]() { updateList(); });
                mUnknownText = getString("unknown_folder");
                return true;
            }

            void onOpen(const LLSD&) override
            {
                updateList();
            }

            void draw() override
            {
                // Folders the inventory did not have yet: look again now and then.
                if (mHasUnknown && mRetryTimer.getElapsedTimeF32() >= RETRY_SECONDS)
                {
                    updateList();
                }
                const bool can_remove = mList->getNumSelected() > 0;
                if (mRemove->getEnabled() != can_remove)
                {
                    mRemove->setEnabled(can_remove);
                }
                LLFloater::draw();
            }

        private:
            void updateList()
            {
                mRetryTimer.reset();
                mHasUnknown = false;

                std::string needle = mFilter->getText();
                LLStringUtil::trim(needle);
                LLStringUtil::toLower(needle);

                // Keep the selection across a refresh.
                uuid_vec_t selected;
                for (LLScrollListItem* item : mList->getAllSelected())
                {
                    selected.push_back(item->getUUID());
                }

                const S32 scroll = mList->getScrollPos();
                mList->clearRows();
                for (const LLUUID& id : folders())
                {
                    LLViewerInventoryCategory* cat = gInventory.getCategory(id);
                    const std::string name = cat ? cat->getName() : mUnknownText;
                    if (!cat)
                    {
                        mHasUnknown = true;
                    }
                    if (!needle.empty())
                    {
                        std::string lower = name;
                        LLStringUtil::toLower(lower);
                        if (lower.find(needle) == std::string::npos)
                        {
                            continue;
                        }
                    }

                    LLSD row;
                    row["value"] = id;
                    row["columns"][0]["column"] = "name";
                    row["columns"][0]["value"] = name;
                    if (!cat)
                    {
                        row["columns"][0]["font-style"] = "ITALIC";
                    }
                    mList->addElement(row);
                }
                mList->selectMultiple(selected);
                mList->setScrollPos(scroll);
            }

            void onRemove()
            {
                std::vector<LLUUID> ids;
                for (LLScrollListItem* item : mList->getAllSelected())
                {
                    ids.push_back(item->getUUID());
                }
                unprotect(ids);     // the list is made again by the change
            }

            void onDoubleClick()
            {
                const LLUUID id = mList->getStringUUIDSelectedItem();
                if (id.notNull() && gInventory.getCategory(id))
                {
                    show_item_original(id);
                }
            }

            LLScrollListCtrl* mList = nullptr;
            LLButton* mRemove = nullptr;
            LLSearchEditor* mFilter = nullptr;
            std::string mUnknownText;
            bool mHasUnknown = false;
            LLTimer mRetryTimer;
            boost::signals2::connection mChangedConnection;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_protected_folders.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<ProtectedFoldersFloater>);
        }

        plus::FeatureRegistration sRegistration({ "protectedfolders", &init, {} });
    }
}
