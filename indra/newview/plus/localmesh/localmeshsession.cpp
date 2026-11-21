/**
 * @file localmeshsession.cpp
 * @brief Local mesh: the window's tools and target, shared by its two windows (plus/localmesh; from Firestorm's vjfloaterlocalmesh.cpp).
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Vaalith Jinn, 2022 (Firestorm's Local Mesh), LGPL 2.1
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

#include "localmeshsession.h"
#include "plus.h"

#include "llfilepicker.h"
#include "lltoolcomp.h"
#include "lltoolmgr.h"
#include "llviewermenufile.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

namespace plus::localmesh
{
    void ToolSession::open()
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

    void ToolSession::close(bool app_quitting)
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

    LLViewerObject* ToolSession::targetObject()
    {
        return LLSelectMgr::getInstance()->getSelection()->getFirstObject();
    }

    bool TargetTracker::update(bool force)
    {
        LLViewerObject* object = ToolSession::targetObject();
        std::string why_not;
        const bool can_show = canShowOn(object, &why_not);
        const bool shown = isShownOn(object);
        const LLUUID id = object ? object->getID() : LLUUID::null;
        if (!force && id == mState.id && why_not == mState.why_not && shown == mState.shown)
        {
            return false;
        }
        mState.id = id;
        mState.why_not = why_not;
        mState.can_show = can_show;
        mState.shown = shown;
        mState.name.clear();
        if (can_show)
        {
            LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(object);
            // The object's name is its owner's: made safe for a text that trusts its text (no links of theirs).
            if (node && node->mValid && !node->mName.empty())
            {
                mState.name = plus::safeText(node->mName);
            }
        }
        return true;
    }

    std::string lodsText(const FileInfo& info)
    {
        std::string lods;
        for (S32 lod = LOD_HIGH; lod >= 0; --lod)
        {
            lods += info.lods[lod] ? std::to_string(lod) : std::string("-");
        }
        return lods;
    }

    const FileInfo* findFile(const std::vector<FileInfo>& infos, const LLUUID& id)
    {
        for (const FileInfo& info : infos)
        {
            if (info.id == id)
            {
                return &info;
            }
        }
        return nullptr;
    }

    void addFilesFromPicker()
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

    void showOnTarget(const FileInfo& info, S32 mesh_index, const LLUUID& target_id, bool file_size)
    {
        show(info.id, mesh_index, gObjectList.findObject(target_id), file_size);
    }

    void takeOffTarget(const LLUUID& target_id)
    {
        takeOff(gObjectList.findObject(target_id));
    }
}
