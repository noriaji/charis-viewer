/**
 * @file llvkrtas.h
 * @brief Hardware ray tracing for photos, stage R2 (research/230): the static scene's
 * acceleration structures.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#ifndef LL_LLVKRTAS_H
#define LL_LLVKRTAS_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <string>

// The retained scene's records that sit in the attribute pool, are opaque and
// not rigged (LLVKScene::collectRayRecords) become acceleration structures:
//
//  - a bottom level structure for each record's triangles, built from the
//    pool's position stream and index buffer by address (nothing is copied),
//    with compaction. Records that share the same triangles share one. One
//    that no record near the camera names for a while is freed;
//  - one top level structure of the records within PlusRayTracingRange of the
//    camera, an instance for each (custom index: the record's id).
//
// Off (the viewer's source says so, or none is set, or the GPU cannot): each
// frame is one test and a return; whatever was built is freed once the frames in
// flight are done with it. Nothing turns it on but the person (plus::raytracing).
//
// Builds are recorded into the frame's upload commands after the recorder is
// done (LLVKMirror::sAfterRecorderFinish, through the scene's submission cut),
// under a per-frame budget of triangles; the rest follow in later frames.
// Main thread, under LLVKTracker::lock(). The draws do not use it yet (R3).
//
// LLRHI_RT_SELFTEST=1: a compute shader shoots a few rays into the structure
// and the log says what they hit (and that a ray that leaves the scene misses).
namespace LLVKRtAs
{
    // Once a frame at its end, with the frame's upload commands.
    void frame(VkCommandBuffer upload);
    // Frees everything now (the device idle).
    void shutdown();
    // The perf log's part since the last call (empty while never on).
    std::string takeStats(U32 frames);
}

#endif // LL_LLVKRTAS_H
