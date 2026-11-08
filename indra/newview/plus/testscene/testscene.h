/**
 * @file testscene.h
 * @brief Test scene: a made-up load of objects that only this viewer has (boxes, meshes, alpha sheets with
 *        texture animations, generated textures), built above the avatar for repeatable frame-time measurements
 *        (research/171-local-test-scene.md, plan B). Nothing is sent to the simulator. Development only.
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

#ifndef PLUS_TESTSCENE_H
#define PLUS_TESTSCENE_H

#include "llsd.h"
#include "stdtypes.h"

#include <string>

// How it fits together
// --------------------
// A scene is a small LLSD file, app_settings/plus/testscene/<name>.xml (heavy-leeaf, light, rain-alpha,
// prims-only), or any .xml file by its full name: a seed, how many objects of what kind, how many generated
// textures, and where (testscenedef.cpp lists every key). The same seed builds the same scene: the log line
// "TESTSCENE built ... hash=..." tells two builds apart.
//
// The objects are made the way the viewer makes its own sky and water: gObjectList.createObjectViewer() gives an
// object with a new UUID and no local id that is never sent, then gPipeline.addObject() a drawable. Everything
// (position, size, volume, faces, textures) is set before addObject(), as processUpdateCore() does for an object
// the simulator sends, so the drawable is static and goes into the region's volume octree like a real still object
// (an object moved after that would become active: LLDrawable::updateMove()). Their mbCanSelect is false: no
// click, hover, pick, area search or rectangle selection (the <plus> line in LLSelectMgr::canSelectObject()).
// Textures are made from generated pictures (LLViewerTextureManager::getFetchedTexture(raw, FTT_LOCAL_FILE), as a
// local GLTF material's are): never fetched.
//
// Ways in (only with PlusTestScene on; the setting is not saved and is off at every start):
//   - the MCP tools testscene_build, testscene_clear, testscene_stats (plus/posermcp, testscenetools.cpp)
//   - with LLRHI_CUE_DIR: the cue files testscene.build (the scene's name, or "key = value" lines) and testscene.clear
//   - with LLRHI_CUE_DIR: "scene = <name>" in a camerapath.run (camerapathbench.cpp)
// The scene goes when it is cleared, when the avatar leaves the region (teleport, crossing, disconnect), and when
// the viewer quits ("LLApp" quitting, before the object list and the pipeline go).
//
// The cost: nothing while no scene is built (no callback, no check). With LLRHI_CUE_DIR, one cached setting check
// a frame (and a clock check while it is on) for the cue files.

namespace plus::testscene
{
    // PlusTestScene (not saved).
    bool enabled();

    // Builds the scene (a name in app_settings/plus/testscene/ or a full .xml file name) with 'overrides' (a map of
    // the scene's keys) over its values; a scene that was built goes first. Main thread, logged in. False with a
    // reason in 'error' (nothing is left built then). 'summary' gets the log line's text.
    bool build(const std::string& scene, const LLSD& overrides, std::string& error, std::string* summary = nullptr);

    // Removes the scene's objects (killObject), lets its textures and mesh volumes go. 'reason' is logged.
    // Main thread; nothing when no scene is built.
    void clear(const std::string& reason);

    bool isBuilt();

    // "<name>:<hash>" of the scene built, or empty; for the measurement logs.
    std::string tag();

    // Counts of the built scene, worked out now: objects alive and dead, drawables, how many are static in the
    // region's volume octree and how many active (bridges), the objects on the object list's active list, the
    // spatial groups they are in, faces, alpha faces, textures, the hash, where. Main thread. Undefined when none.
    LLSD stats();

    // Puts the camera where it sees the whole scene (the scene's "view"). Main thread. False with a reason.
    bool lookAt(std::string& error);

    // A paragraph for the MCP server's instructions.
    std::string mcpInstructions();
}

#endif // PLUS_TESTSCENE_H
