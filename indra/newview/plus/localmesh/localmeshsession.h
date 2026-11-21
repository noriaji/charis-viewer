/**
 * @file localmeshsession.h
 * @brief Local mesh: what the XUI and the RmlUi windows share of the window's work (research/386).
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

// The window's tools: while it is open a click in the world selects an object (the inspect tool, as Firestorm's
// window), and the selected object is the one a mesh is shown on. The window gives back the selection mode it found.
// The target (the selected object, why a mesh cannot be shown on it, whether it shows one) is looked at every frame
// the window is drawn, so it says whether it changed. Main thread only.

#ifndef PLUS_LOCALMESHSESSION_H
#define PLUS_LOCALMESHSESSION_H

#include "localmesh.h"

#include "llselectmgr.h"

#include <string>
#include <vector>

namespace plus::localmesh
{
    // The selection mode and the inspect tool a window borrows while it is open.
    class ToolSession
    {
    public:
        void open();
        void close(bool app_quitting);
        // The first selected object (null: none).
        static LLViewerObject* targetObject();

    private:
        bool mOldForceSelection = false;
        bool mForcing = false;                  // setForceSelection(true) is ours until close()
        LLObjectSelectionHandle mSelection;     // keeps the inspect tool's selection (as Firestorm's)
    };

    // The selected object and what can be done with it.
    struct TargetState
    {
        LLUUID id;
        std::string why_not;        // canShowOn(): "NoObject", "NotMesh", "Rigged", "NotLoaded" (empty: it can)
        bool can_show = false;
        bool shown = false;         // it shows a local mesh now
        std::string name;           // its name made safe to show (empty: not known yet)
    };

    class TargetTracker
    {
    public:
        // Looks at the selection; true when the target (its id, why not, or whether it shows a mesh) changed, or force.
        bool update(bool force);
        const TargetState& state() const { return mState; }

    private:
        TargetState mState;
    };

    // The LODs read from files, highest to lowest: "3210", a missing one "-".
    std::string lodsText(const FileInfo& info);
    // The file with this id among infos (null: none).
    const FileInfo* findFile(const std::vector<FileInfo>& infos, const LLUUID& id);
    // The file picker; every file chosen is added.
    void addFilesFromPicker();
    // "Show on the object" / "Take off" for the object with this id (not there any more: nothing).
    void showOnTarget(const FileInfo& info, S32 mesh_index, const LLUUID& target_id, bool file_size);
    void takeOffTarget(const LLUUID& target_id);
}

#endif // PLUS_LOCALMESHSESSION_H
