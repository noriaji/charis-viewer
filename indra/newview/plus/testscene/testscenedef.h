/**
 * @file testscenedef.h
 * @brief Test scene: the scene definition (the keys of app_settings/plus/testscene/<name>.xml), read and checked.
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

#ifndef PLUS_TESTSCENE_DEF_H
#define PLUS_TESTSCENE_DEF_H

#include "llsd.h"
#include "stdtypes.h"
#include "v3math.h"

#include <string>
#include <vector>

namespace plus::testscene
{
    // The limits (a scene file or an override beyond them is refused, not cut: a measurement must not run on a
    // scene other than the one asked for).
    constexpr U32 MAX_OBJECTS       = 20000;           // prims + meshes + alpha sheets
    constexpr U32 MAX_TEXTURES      = 2048;            // each of textures, alpha_textures
    constexpr U32 MAX_TEXELS        = 64u * 1024 * 1024; // all generated textures together (RGBA8: 256 MB, not compressed)
    constexpr U32 MAX_MESH_KINDS    = 32;
    constexpr U32 MAX_CLUSTERS      = 1024;
    constexpr S64 MAX_FILE_BYTES    = 256 * 1024;

    enum class Shape : U8 { Box, Cylinder, Sphere, Torus, Prism, Tube, Count };

    // Every key of a scene file, with its default. Lengths in metres, region axes.
    struct Definition
    {
        std::string name;                 // the file's stem (or "custom")
        std::string description;          // free text, for the log only
        U32  seed = 1;

        // Where: the avatar's place in the region rounded to 8 m (so a few metres either way at the landing point
        // builds the same scene), "height" above it; or a place in the region ("center": [x, y, z]).
        bool      centerOnAgent = true;
        LLVector3 center;                 // region coordinates when !centerOnAgent
        F32  height   = 80.f;             // the scene's middle above the avatar (centerOnAgent)
        F32  radius   = 48.f;             // objects within +- radius on x and y of the middle
        F32  vertical = 24.f;             // and within +- vertical / 2 on z
        U32  clusters = 0;                // 0: spread evenly; N: N places, each object near one of them
        F32  clusterRadius = 8.f;

        // Opaque prims.
        U32  prims = 0;
        std::vector<Shape> shapes { Shape::Box };   // picked from evenly
        F32  sizeMin = 0.5f;
        F32  sizeMax = 3.f;
        U32  textures = 16;               // generated textures for prims and meshes (how many draws a group needs)
        U32  textureSize = 256;           // pixels, a power of two 32..1024
        bool texturePerFace = false;      // false: one texture for all of an object's faces
        F32  fullbright = 0.f;            // the part of the opaque objects drawn full bright (0..1)

        // Meshes: made from the faces of prims (no file), 4 LODs each, shared by the objects of a kind.
        U32  meshes = 0;
        U32  meshKinds = 4;

        // Alpha sheets: one face each (a flat box, the other faces fully transparent and so not drawn).
        U32  alphaSheets = 0;
        F32  alpha = 0.5f;                // the sheets' opacity (0.05..0.95)
        U32  alphaTextures = 8;
        F32  sheetSizeMin = 1.f;
        F32  sheetSizeMax = 4.f;
        F32  texanim = 0.f;               // the part of the sheets with a scrolling texture animation (0..1)
        F32  texanimRate = 0.5f;          // their speed, texture widths a second

        // The camera for lookAt(): from the middle + view, looking at the middle. Unset: from the -x side, a little above.
        bool      hasView = false;
        LLVector3 view;

        U32 objectCount() const { return prims + meshes + alphaSheets; }
    };

    // The key = value pairs of 'map' over 'def' (a file's map, or the overrides of a tool or a cue). Values may be
    // LLSD numbers, booleans, arrays, or strings ("0.3", "agent", "10,20,30", "box,sphere"). False with a reason
    // (an unknown key, a value out of range); 'def' may then be half changed.
    bool applyMap(const LLSD& map, Definition& def, std::string& error);

    // The definition of the scene 'name': a name of app_settings/plus/testscene/ (without .xml) or a full .xml
    // file name. False with a reason.
    bool loadDefinition(const std::string& name, Definition& def, std::string& error);

    // The checks across keys (the totals). False with a reason.
    bool checkTotals(const Definition& def, std::string& error);

    // "key=value ..." of the main numbers, for the log.
    std::string describe(const Definition& def);

    const char* shapeName(Shape shape);

    // The scenes in app_settings/plus/testscene/ (names without .xml), sorted.
    std::vector<std::string> sceneNames();
}

#endif // PLUS_TESTSCENE_DEF_H
