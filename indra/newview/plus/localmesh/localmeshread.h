/**
 * @file localmeshread.h
 * @brief Local mesh: the file readers (COLLADA and glTF), as plain triangle lists.
 *
 * Takes the place of Firestorm's importer interfaces (vjlocalmeshimportdae.h,
 * fslocalmeshimportgltf.h; see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Local Mesh contribution source code
 * Copyright (C) 2022, Vaalith Jinn.
 * Copyright (C) 2026, Beq Janus.
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

// The readers know nothing of volumes or objects: each turns one file into meshes made of
// faces (a face is what one material covers), in metres and in the viewer's axes (Z up),
// with the file's own units and up axis already applied. localmesh.cpp centres and scales
// them the way an upload does and makes the volume faces.
//
// Run on a worker thread (localmesh.cpp's loader). They touch nothing of the viewer's state
// (the COLLADA DOM is held by one reader at a time). The files are the user's and may be
// broken or made to harm: every index, offset and count is checked before it is used, and
// what they give is finite (localmesh.cpp checks the sizes again). They may still throw (out
// of memory): the loader catches it.
//
// Rigging (skin weights and joints) is not read: a rigged file shows as a static mesh.

#ifndef PLUS_LOCALMESH_LOCALMESHREAD_H
#define PLUS_LOCALMESH_LOCALMESHREAD_H

#include <string>
#include <vector>

#include "v2math.h"
#include "v3math.h"

namespace plus::localmesh
{
    struct RawFace
    {
        std::vector<LLVector3> positions;
        std::vector<LLVector3> normals;    // empty, or one per position (made from the triangles if empty)
        std::vector<LLVector2> uvs;        // empty, or one per position
        std::vector<U32>       indices;    // triangles, counter-clockwise seen from the front
    };

    struct RawMesh
    {
        std::string          name;
        std::vector<RawFace> faces;        // at most LL_SCULPT_MESH_MAX_FACES
    };

    using Log = std::vector<std::string>;

    // What one file may make, so that a small file cannot ask for unbounded memory (a node
    // repeated a hundred thousand times over one large mesh, say). Far above what a model
    // for Second Life has: an object has at most 8 faces of 65535 vertices.
    constexpr size_t MAX_FILE_MESHES = 256;
    constexpr size_t MAX_FILE_VERTICES = 2u << 20;      // in all the faces of the file
    constexpr size_t MAX_FILE_CORNERS = 12u << 20;      // triangle corners (indices), in all
    constexpr S32 MAX_NAME_BYTES = 128;                 // of a name from the file, in the log and the window

    // Each returns false when nothing could be read; the log says why, and what was skipped.
    // readGLTF() also lists the image files beside the file that its materials use (full
    // paths), for Local Textures.
    bool readDAE(const std::string& path, bool preprocess, std::vector<RawMesh>& meshes, Log& log);
    bool readGLTF(const std::string& path, std::vector<RawMesh>& meshes, std::vector<std::string>& images, Log& log);
}

#endif // PLUS_LOCALMESH_LOCALMESHREAD_H
