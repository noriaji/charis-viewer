/**
 * @file localmeshfloater.cpp
 * @brief Local mesh: the window (the files, the selected object, the log).
 *
 * Ported from Firestorm's vjfloaterlocalmesh.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Local Mesh contribution source code
 * Copyright (C) 2022, Vaalith Jinn.
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

// Menu: "Local Mesh" in menu_plus.xml (Floater.Toggle plus_local_mesh). The files and the
// objects showing them live in localmesh.cpp, not in the window: closing it keeps them.
//
// While the window is open, a click in the world selects an object (the inspect tool, as
// Firestorm's window), and the selected object is the one a mesh is shown on.
//
// Changed from Firestorm's: one page instead of three tabs (the list, the selected object
// and the log of the selected file are seen together); the window says which object is
// selected and, when a mesh cannot be shown on it, why; several files can be added at once;
// "Take off" is offered only for an object that shows a local mesh; closing the window gives
// back the selection mode it found (Firestorm's left "force selection" on).

#include "llviewerprecompiledheaders.h"

#include "localmesh.h"
#include "plus.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfilepicker.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lltoolcomp.h"
#include "lltoolmgr.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

namespace plus::localmesh
{
    namespace
    {
        const char* FLOATER_NAME = "plus_local_mesh";

        class LocalMeshFloater : public LLFloater
        {
        public:
            explicit LocalMeshFloater(const LLSD& key)
                : LLFloater(key)
            {
            }

            ~LocalMeshFloater() override
            {
                mChangedConnection.disconnect();
            }

            bool postBuild() override
            {
                mFiles = getChild<LLScrollListCtrl>("files");
                mMeshes = getChild<LLComboBox>("meshes");
                mTarget = getChild<LLTextBox>("target");
                mLog = getChild<LLTextEditor>("log");
                mFileSize = getChild<LLCheckBoxCtrl>("file_size");
                mShow = getChild<LLButton>("show");
                mTakeOff = getChild<LLButton>("take_off");
                mReload = getChild<LLButton>("reload");
                mRemove = getChild<LLButton>("remove");

                mFiles->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFileSelected(); });
                getChild<LLButton>("add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAdd(); });
                mReload->setCommitCallback([this](LLUICtrl*, const LLSD&) { reloadFile(selectedFile()); });
                mRemove->setCommitCallback([this](LLUICtrl*, const LLSD&) { removeFile(selectedFile()); });
                mShow->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShow(); });
                mTakeOff->setCommitCallback([this](LLUICtrl*, const LLSD&) { takeOff(gObjectList.findObject(mTargetID)); });

                mChangedConnection = setChangedCallback([this]() { refreshFiles(); });
                refreshFiles();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                // a click in the world selects (Firestorm's toggleSelectTool(true)); the value
                // before is kept only by the first open: an open again (after minimizing) would
                // keep our own true, and closing would leave the forced selection on
                const bool old_force = LLSelectMgr::getInstance()->setForceSelection(true);
                if (!mForcing)
                {
                    mOldForceSelection = old_force;
                    mForcing = true;
                }
                LLToolMgr::getInstance()->setTransientTool(LLToolCompInspect::getInstance());
                mSelection = LLSelectMgr::getInstance()->getSelection();
            }

            void onClose(bool app_quitting) override
            {
                if (app_quitting)
                {
                    return;
                }
                if (mForcing)
                {
                    LLSelectMgr::getInstance()->setForceSelection(mOldForceSelection);
                    mForcing = false;
                }
                if (LLToolMgr::getInstance()->getCurrentTool() == LLToolCompInspect::getInstance())
                {
                    LLToolMgr::getInstance()->clearTransientTool();
                }
                mSelection = static_cast<LLObjectSelection*>(nullptr);
            }

            void draw() override
            {
                refreshTarget(false);
                LLFloater::draw();
            }

        private:
            LLUUID selectedFile() const
            {
                LLScrollListItem* item = mFiles->getFirstSelected();
                return item ? item->getValue().asUUID() : LLUUID::null;
            }

            const FileInfo* selectedInfo() const
            {
                const LLUUID id = selectedFile();
                for (const FileInfo& info : mInfos)
                {
                    if (info.id == id)
                    {
                        return &info;
                    }
                }
                return nullptr;
            }

            void onAdd()
            {
                LLFilePickerReplyThread::startPicker(
                    [](const std::vector<std::string>& paths, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                    {
                        for (const std::string& path : paths)
                        {
                            addFile(path);
                        }
                    },
                    LLFilePicker::FFLOAD_MODEL, true);
            }

            void onShow()
            {
                const FileInfo* info = selectedInfo();
                if (info)
                {
                    show(info->id, mMeshes->getFirstSelectedIndex(), gObjectList.findObject(mTargetID), mFileSize->get());
                }
            }

            void refreshFiles()
            {
                const LLUUID selected = selectedFile();
                const size_t old_count = mInfos.size();
                mInfos = files();

                mFiles->deleteAllItems();
                for (const FileInfo& info : mInfos)
                {
                    std::string lods;
                    for (S32 lod = LOD_HIGH; lod >= 0; --lod)
                    {
                        lods += info.lods[lod] ? std::to_string(lod) : std::string("-");
                    }
                    LLSD row;
                    row["value"] = info.id;
                    row["columns"][0]["column"] = "status";
                    row["columns"][0]["value"] = getString(info.status == Status::Loading  ? "status_loading"
                                                           : info.status == Status::Active ? "status_active"
                                                                                           : "status_error");
                    row["columns"][1]["column"] = "name";
                    row["columns"][1]["value"] = info.name;
                    row["columns"][2]["column"] = "lods";
                    row["columns"][2]["value"] = lods;
                    row["columns"][3]["column"] = "meshes";
                    row["columns"][3]["value"] = (S32)info.meshes.size();
                    row["columns"][4]["column"] = "shown";
                    row["columns"][4]["value"] = info.shownOn;
                    mFiles->addElement(row);
                }

                // a file just added is selected, else the one that was
                if (mInfos.size() > old_count && !mInfos.empty())
                {
                    mFiles->selectByValue(mInfos.back().id);
                }
                else if (selected.notNull())
                {
                    mFiles->selectByValue(selected);
                }
                if (!mFiles->getFirstSelected() && !mInfos.empty())
                {
                    mFiles->selectFirstItem();
                }
                onFileSelected();
            }

            void onFileSelected()
            {
                const FileInfo* info = selectedInfo();

                const S32 old_mesh = mMeshes->getFirstSelectedIndex();
                const LLUUID old_file = mMeshesFile;
                mMeshes->removeall();
                mMeshesFile = info ? info->id : LLUUID::null;
                if (info)
                {
                    for (const std::string& name : info->meshes)
                    {
                        mMeshes->add(name);
                    }
                }
                if (mMeshes->getItemCount() > 0)
                {
                    const bool same_file = old_file == mMeshesFile && old_mesh >= 0 && old_mesh < mMeshes->getItemCount();
                    mMeshes->selectNthItem(same_file ? old_mesh : 0);
                }

                mLog->clear();
                if (info)
                {
                    std::string text;
                    for (const std::string& line : fileLog(info->id))
                    {
                        text += line + "\n";
                    }
                    mLog->setText(text);
                }
                refreshTarget(true);
            }

            // The selected object, what can be done with it, and the buttons.
            void refreshTarget(bool force)
            {
                LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getFirstObject();
                std::string why_not;
                const bool can_show = canShowOn(object, &why_not);
                const bool shown = isShownOn(object);
                const LLUUID id = object ? object->getID() : LLUUID::null;
                if (!force && id == mTargetID && why_not == mTargetWhyNot && shown == mTargetShown)
                {
                    return;
                }
                mTargetID = id;
                mTargetWhyNot = why_not;
                mTargetShown = shown;

                if (!can_show)
                {
                    mTarget->setText(getString("why_" + why_not));
                }
                else
                {
                    LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(object);
                    LLStringUtil::format_map_t args;
                    // The object's name is its owner's: no links of theirs in this text box,
                    // which trusts its text.
                    args["[NAME]"] = (node && node->mValid && !node->mName.empty()) ? plus::safeText(node->mName) : getString("unnamed");
                    mTarget->setText(getString(shown ? "target_shown" : "target", args));
                }

                const FileInfo* info = selectedInfo();
                const bool loading = info && info->status == Status::Loading;
                mReload->setEnabled(info && !loading);
                mRemove->setEnabled(info && !loading);
                mMeshes->setEnabled(info && info->status == Status::Active);
                mShow->setEnabled(can_show && info && info->status == Status::Active && mMeshes->getFirstSelectedIndex() >= 0);
                mTakeOff->setEnabled(shown);
            }

            LLScrollListCtrl* mFiles = nullptr;
            LLComboBox* mMeshes = nullptr;
            LLTextBox* mTarget = nullptr;
            LLTextEditor* mLog = nullptr;
            LLCheckBoxCtrl* mFileSize = nullptr;
            LLButton* mShow = nullptr;
            LLButton* mTakeOff = nullptr;
            LLButton* mReload = nullptr;
            LLButton* mRemove = nullptr;

            std::vector<FileInfo> mInfos;
            LLUUID mMeshesFile;
            LLUUID mTargetID;
            std::string mTargetWhyNot;
            bool mTargetShown = false;
            bool mOldForceSelection = false;
            bool mForcing = false;              // setForceSelection(true) is ours until onClose
            LLObjectSelectionHandle mSelection;     // keeps the inspect tool's selection (as Firestorm's)
            boost::signals2::connection mChangedConnection;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_local_mesh.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<LocalMeshFloater>);
        }

        plus::FeatureRegistration sRegistration({ "localmesh", &init, {} });
    }
}
