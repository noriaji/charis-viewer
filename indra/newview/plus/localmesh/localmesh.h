/**
 * @file localmesh.h
 * @brief Local mesh: a mesh file on this computer shown on an object of the world, for
 *        this viewer only, without uploading it.
 *
 * Ported from Firestorm's vjlocalmesh.h, Local Mesh by Vaalith Jinn (see ORIGIN.md).
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

// Ported from Firestorm's Local Mesh (vjlocalmesh.cpp and vjfloaterlocalmesh.cpp, Vaalith
// Jinn, 2022; LGPL 2.1), research/140.
//
// How it works
// ------------
// A file (.dae, .gltf, .glb; with its lower LODs beside it as <name>_LOD2, _LOD1, _LOD0) is
// read on a worker thread into meshes, each centred and scaled into the unit cube the way
// an upload does. Shown on an object (a mesh object of the world, not rigged), a mesh gets
// volume params of its own (a random mesh id) and, in the volume manager, a volume for each
// of the four LODs, filled with its faces and marked loaded: the object's setVolume() then
// finds every LOD it may ask for already there and never asks the mesh repository for the
// made-up id. A LOD the files do not have is a copy of the next better one. The rebuild is
// the one of a mesh that has just been downloaded (notifyMeshLoaded(): the faces and
// texture entries follow the new face count), so the renderer has nothing to learn.
//
// While an object shows a local mesh, the simulator's updates of it are held back
// (holdsUpdates(), a hook in LLVOVolume::processUpdateMessage()): an update would put the
// real mesh back. Nothing of the local mesh goes the other way either: what belongs to its
// faces is not sent (texture entries, PBR and legacy materials, media: the simulator would
// take the local face count, and a texture dropped on a face, for the object's), nor a scale
// (and the position that comes with it) the user gives it (simulatorUpdateType()). Taking the
// local mesh off puts back the object's volume, scale and texture entries and asks the
// simulator for the object again (what it held back meanwhile), and for its children when
// the linked set was scaled meanwhile (they moved here only); the object then takes updates
// again.
//
// Main thread only (the readers run on workers, see localmeshread.h).

#ifndef PLUS_LOCALMESH_LOCALMESH_H
#define PLUS_LOCALMESH_LOCALMESH_H

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <boost/signals2/connection.hpp>

#include "lluuid.h"

class LLViewerObject;

namespace plus::localmesh
{
    constexpr S32 LOD_COUNT = 4;        // 0 lowest .. 3 high, as LLModel's LODs
    constexpr S32 LOD_HIGH = 3;

    enum class Status
    {
        Loading,
        Active,
        Error
    };

    // What the window shows of a file.
    struct FileInfo
    {
        LLUUID id;
        std::string name;                       // the file's name, without its folder
        Status status = Status::Loading;
        std::array<bool, LOD_COUNT> lods{};     // the LODs read from files
        std::vector<std::string> meshes;        // the names of the file's meshes
        S32 shownOn = 0;                        // objects showing one of them
    };

    // Files
    void addFile(const std::string& path);
    void removeFile(const LLUUID& file_id);     // also takes its meshes off the objects
    void reloadFile(const LLUUID& file_id);
    std::vector<FileInfo> files();
    std::vector<std::string> fileLog(const LLUUID& file_id);

    // Objects. canShowOn() says why not when it returns false: "NoObject", "NotMesh", "Rigged"
    // or "NotLoaded" (the window's strings are named after them).
    bool canShowOn(const LLViewerObject* object, std::string* why_not = nullptr);
    bool show(const LLUUID& file_id, S32 mesh_index, LLViewerObject* object, bool file_size);
    void takeOff(LLViewerObject* object);
    bool isShownOn(const LLViewerObject* object);

    // Called when a file is added, removed, finishes loading or the objects change.
    boost::signals2::connection setChangedCallback(const std::function<void()>& callback);

    // The hooks. holdsUpdates(): true while the object shows a local mesh, so that the
    // simulator's update is not applied (LLVOVolume::processUpdateMessage()) and what belongs
    // to the local mesh's faces is not sent: texture entries (LLViewerObject::sendTEUpdate()),
    // PBR materials (LLGLTFMaterialList::queueApply()/queueModify()), legacy materials
    // (LLMaterialMgr::put()) and media (LLVOVolume::sendMediaDataUpdate()).
    // simulatorUpdateType(): what LLSelectMgr::packMultipleUpdate() sends of the object (the
    // UPD_* bits): no scale, and no position with it, while it shows a local mesh.
    bool holdsUpdates(const LLViewerObject* object);
    U8 simulatorUpdateType(const LLViewerObject* object, U8 type);
}

#endif // PLUS_LOCALMESH_LOCALMESH_H
