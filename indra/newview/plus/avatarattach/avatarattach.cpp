/**
 * @file avatarattach.cpp
 * @brief The attachments of an avatar, listed in a window (the avatar's context menu).
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

// See avatarattach.h for what the window does and where its names come from. This is the XUI window (frozen: used in
// safe mode, where RmlUi is not started, and with PlusRmlUiWindows off; the choice is made once, in init()); its work
// is attachmodel.cpp's, shared with the RmlUi window (attachrml.cpp, research/386).

#include "llviewerprecompiledheaders.h"

#include "avatarattach.h"
#include "attachmodel.h"
#include "attachrml.h"
#include "plus.h"
#include "plus/safemode/safemode.h"

#include <unordered_map>
#include <vector>

#include "llagent.h"
#include "llapp.h"
#include "llbutton.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llscrolllistcell.h"
#include "llscrolllistcolumn.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewerobject.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

namespace plus::avatarattach
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_avatar_attachments";

        // The avatar of the context menu: the selection's primary object is the avatar (or, on the
        // menu of an attachment, the attachment). As the read-aloud voice of the same menu does.
        LLUUID selectedAvatarId()
        {
            LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject();
            if (!object)
            {
                return LLUUID::null;
            }
            if (object->isAvatar())
            {
                return object->getID();
            }
            if (LLVOAvatar* avatar = object->getAvatar())
            {
                return avatar->getID();
            }
            return LLUUID::null;
        }

        class AttachmentsFloater : public LLFloater, public AttachmentsModel::View
        {
            friend class ::LLFloaterReg;

        public:
            explicit AttachmentsFloater(const LLSD& key) : LLFloater(key), mModel(*this) {}

            ~AttachmentsFloater() override
            {
                mModel.stopAsking();
            }

            bool postBuild() override
            {
                mList   = getChild<LLScrollListCtrl>("attachments_list");
                mStatus = getChild<LLTextBox>("status");
                mCopyNameBtn = getChild<LLButton>("copy_name_btn");
                mCopyIdBtn   = getChild<LLButton>("copy_id_btn");
                mInspectBtn  = getChild<LLButton>("inspect_btn");

                mColPoint    = columnIndex("point");
                mColName     = columnIndex("name");
                mColCreator  = columnIndex("creator");
                mColPrims    = columnIndex("prims");
                mColDistance = columnIndex("distance");

                mModel.init();

                mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
                mList->setDoubleClickCallback([this]() { mModel.inspect(selectedId()); });
                mCopyNameBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { mModel.copyName(selectedId()); });
                mCopyIdBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { mModel.copyId(selectedId()); });
                mInspectBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { mModel.inspect(selectedId()); });
                getChild<LLUICtrl>("refresh_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mModel.askAgain(); });
                updateButtons();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                mModel.open(key.asUUID());
                updateButtons();
            }

            void onClose(bool app_quitting) override
            {
                mModel.stopAsking();
                LLFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mModel.tick())
                {
                    updateButtons();
                }
                LLFloater::draw();
            }

            // AttachmentsModel::View
            std::string text(const char* id, const LLStringUtil::format_map_t& args) override
            {
                return getString(id, args);
            }
            void setTitle(const std::string& title) override { LLFloater::setTitle(title); }
            void setStatus(const std::string& text) override { mStatus->setText(text); }
            void setEmptyText(const std::string& text) override { mList->setCommentText(text); }
            void clearRows() override { mList->deleteAllItems(); }

            // The changed cells when the same objects are there, the whole list (selection and scroll kept) otherwise.
            void showRows(const std::vector<Row>& now_rows, const std::vector<Row>& was_rows, bool same_objects) override
            {
                if (same_objects)
                {
                    mIndexById.clear();
                    for (size_t i = 0; i < now_rows.size(); ++i)
                    {
                        mIndexById[now_rows[i].id] = i;
                    }
                    const S32 columns[] = { mColPoint, mColName, mColCreator, mColPrims, mColDistance };
                    for (LLScrollListItem* item : mList->getAllData())
                    {
                        auto found = item ? mIndexById.find(item->getUUID()) : mIndexById.end();
                        if (found == mIndexById.end())
                        {
                            continue;
                        }
                        const Row& now = now_rows[found->second];
                        const Row& was = was_rows[found->second];
                        for (S32 column : columns)
                        {
                            LLScrollListCell* cell = column >= 0 ? item->getColumn(column) : nullptr;
                            const LLSD value = cellValue(now, column);
                            if (cell && cellValue(was, column).asString() != value.asString())
                            {
                                cell->setValue(value);
                            }
                        }
                    }
                    mList->setNeedsSort();
                    return;
                }
                const LLUUID selected = mList->getSelectedValue().asUUID();
                const S32 scroll = mList->getScrollPos();
                mList->deleteAllItems();
                for (const Row& row : now_rows)
                {
                    LLSD element;
                    element["id"] = row.id;
                    LLSD& columns = element["columns"];
                    const std::pair<const char*, S32> names[] = {
                        { "point", mColPoint }, { "name", mColName }, { "creator", mColCreator },
                        { "prims", mColPrims }, { "distance", mColDistance } };
                    S32 n = 0;
                    for (const auto& name : names)
                    {
                        if (name.second < 0)
                        {
                            continue;
                        }
                        columns[n]["column"] = name.first;
                        columns[n]["value"]  = cellValue(row, name.second);
                        ++n;
                    }
                    mList->addElement(element);
                }
                if (selected.notNull())
                {
                    mList->selectByID(selected);
                }
                mList->setScrollPos(scroll);
            }

        private:
            S32 columnIndex(const std::string& name)
            {
                LLScrollListColumn* column = mList->getColumn(name);
                return column ? column->mIndex : -1;
            }

            LLSD cellValue(const Row& row, S32 column) const
            {
                if (column == mColPoint)    return LLSD(row.point);
                if (column == mColName)     return LLSD(row.name);
                if (column == mColCreator)  return LLSD(row.creator);
                if (column == mColPrims)    return LLSD(row.prims);
                if (row.distance < 0)       return LLSD(mModel.distanceText(row));
                return LLSD(row.distance);
            }

            LLUUID selectedId() const { return mList->getSelectedValue().asUUID(); }

            void updateButtons()
            {
                const AttachmentsModel::Buttons buttons = mModel.buttons(selectedId());
                mCopyNameBtn->setEnabled(buttons.copy_name);
                mCopyIdBtn->setEnabled(buttons.copy_id);
                mInspectBtn->setEnabled(buttons.inspect);
            }

            LLScrollListCtrl* mList   = nullptr;
            LLTextBox*        mStatus = nullptr;
            LLButton*         mCopyNameBtn = nullptr;
            LLButton*         mCopyIdBtn   = nullptr;
            LLButton*         mInspectBtn  = nullptr;
            S32 mColPoint = -1, mColName = -1, mColCreator = -1, mColPrims = -1, mColDistance = -1;
            std::unordered_map<LLUUID, size_t> mIndexById;
            AttachmentsModel mModel;
        };

        void init()
        {
            if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
            {
                LLFloaterReg::add(FLOATER_NAME, "floater_plus_avatar_attachments.xml",
                                  (LLFloaterBuildFunc)&LLFloaterReg::build<AttachmentsFloater>);
            }
            else
            {
                addRmlFloater(FLOATER_NAME);
            }

            // "self": the agent's own menu (the agent is not the selection there).
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.AvatarAttachments.Show", [](LLUICtrl*, const LLSD& param)
            {
                if (LLApp::isExiting())
                {
                    return;
                }
                const LLUUID id = param.asString() == "self" ? gAgentID : selectedAvatarId();
                if (id.notNull())
                {
                    LLFloaterReg::showInstance(FLOATER_NAME, LLSD(id), true);
                }
            });
        }

        plus::FeatureRegistration sRegistration({ "avatarattach", &init });
    }

    bool onObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name)
    {
        return AttachmentsModel::dispatchObjectProperties(object_id, creator_id, name);
    }
}
