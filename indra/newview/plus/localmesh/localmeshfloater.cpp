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
// Firestorm's window), and the selected object is the one a mesh is shown on. The tools and
// the target are localmeshsession.cpp's, shared with the RmlUi window (localmeshrml.cpp); this
// XUI window is frozen: it is used in safe mode and with PlusRmlUiWindows off.
//
// Changed from Firestorm's: one page instead of three tabs (the list, the selected object
// and the log of the selected file are seen together); the window says which object is
// selected and, when a mesh cannot be shown on it, why; several files can be added at once;
// "Take off" is offered only for an object that shows a local mesh; closing the window gives
// back the selection mode it found (Firestorm's left "force selection" on).

#include "llviewerprecompiledheaders.h"

#include "localmesh.h"
#include "localmeshrml.h"
#include "localmeshsession.h"
#include "plus.h"
#include "plus/safemode/safemode.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"

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
                getChild<LLButton>("add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { addFilesFromPicker(); });
                mReload->setCommitCallback([this](LLUICtrl*, const LLSD&) { reloadFile(selectedFile()); });
                mRemove->setCommitCallback([this](LLUICtrl*, const LLSD&) { removeFile(selectedFile()); });
                mShow->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShow(); });
                mTakeOff->setCommitCallback([this](LLUICtrl*, const LLSD&) { takeOffTarget(mTracker.state().id); });

                mChangedConnection = setChangedCallback([this]() { refreshFiles(); });
                refreshFiles();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                mTools.open();
            }

            void onClose(bool app_quitting) override
            {
                mTools.close(app_quitting);
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
                return findFile(mInfos, selectedFile());
            }

            void onShow()
            {
                const FileInfo* info = selectedInfo();
                if (info)
                {
                    showOnTarget(*info, mMeshes->getFirstSelectedIndex(), mTracker.state().id, mFileSize->get());
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
                    LLSD row;
                    row["value"] = info.id;
                    row["columns"][0]["column"] = "status";
                    row["columns"][0]["value"] = getString(info.status == Status::Loading  ? "status_loading"
                                                           : info.status == Status::Active ? "status_active"
                                                                                           : "status_error");
                    row["columns"][1]["column"] = "name";
                    row["columns"][1]["value"] = info.name;
                    row["columns"][2]["column"] = "lods";
                    row["columns"][2]["value"] = lodsText(info);
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
                if (!mTracker.update(force))
                {
                    return;
                }
                const TargetState& target = mTracker.state();
                if (!target.can_show)
                {
                    mTarget->setText(getString("why_" + target.why_not));
                }
                else
                {
                    LLStringUtil::format_map_t args;
                    args["[NAME]"] = target.name.empty() ? getString("unnamed") : target.name;
                    mTarget->setText(getString(target.shown ? "target_shown" : "target", args));
                }

                const FileInfo* info = selectedInfo();
                const bool loading = info && info->status == Status::Loading;
                mReload->setEnabled(info && !loading);
                mRemove->setEnabled(info && !loading);
                mMeshes->setEnabled(info && info->status == Status::Active);
                mShow->setEnabled(target.can_show && info && info->status == Status::Active && mMeshes->getFirstSelectedIndex() >= 0);
                mTakeOff->setEnabled(target.shown);
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
            ToolSession mTools;
            TargetTracker mTracker;
            boost::signals2::connection mChangedConnection;
        };

        // The window is the RmlUi one (localmeshrml.cpp, research/386). This XUI version is frozen (not changed any
        // more: a setting added to the window goes to both) and is used in safe mode, where RmlUi is not started, and
        // with PlusRmlUiWindows off (the choice is made once, here: it takes effect at the next start).
        void init()
        {
            if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
            {
                LLFloaterReg::add(FLOATER_NAME, "floater_plus_local_mesh.xml",
                                  (LLFloaterBuildFunc)&LLFloaterReg::build<LocalMeshFloater>);
            }
            else
            {
                addRmlFloater(FLOATER_NAME);
            }
        }

        plus::FeatureRegistration sRegistration({ "localmesh", &init, {} });
    }
}
