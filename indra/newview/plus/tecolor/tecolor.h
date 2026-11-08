/**
 * @file tecolor.h
 * @brief "TE color without a group rebuild": a face's color change rewrites
 *        only the color bytes of the object's vertices, in place, instead of
 *        rebuilding the object's whole spatial group (all vertices of every
 *        object in it, the batches, and everything keyed on them).
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

// Why (research/81 in the sl-viewer repository)
// ---------------------------------------------
// Upstream's LLVOVolume::setTEColor sets REBUILD_COLOR, then dirtyMesh()
// widens it to REBUILD_ALL, and mColorChanged makes LLVOVolume::updateGeometry
// call dirtySpatialGroup(): the object's whole group is rebuilt (clearDrawMap,
// genDrawInfo, every attribute of every face of every object in it). A
// script that keeps changing a color does this every time. Yet a face's
// color only ever reaches the screen as the per-vertex color (LLFace::
// getGeometryVolume, one value for all the face's vertices), and the batches
// depend on the alpha only through its class (opaque >= 0.999, translucent,
// fully transparent 0; LLPipeline::getPoolTypeFromTE, LLFace::canRenderAsMask,
// the alpha face sort in rebuildGeom, the material shader masks).
//
// So when the RGB changes and the alpha does not (stage 1), or the alpha
// changes but stays strictly between 0 and 0.999 (stage 2), the hook applies
// the color and flags only REBUILD_COLOR + the group's MESH_DIRTY: the
// group's rebuildMesh rewrites the color (and glow) bytes of the object's
// faces in place (getGeometryVolume's existing color-only path) and uploads
// just those ranges. Same vertex values, same batches: the same picture.
//
// With the Vulkan renderer (LLRHI::sVulkanGL); the GPU benchmark process,
// plain GL, keeps upstream's behaviour.
//   LLRHI_COLOR_NOREBUILD=0   stage 1 off (RGB changes rebuild as upstream)
//   LLRHI_ALPHA_NOREBUILD=0   stage 2 off (alpha changes rebuild as upstream)
//   LLRHI_COLOR_STATS=1       counters, logged every 10 s ("RHIColor")
// While running, <LLRHI_CUE_DIR>/colornorebuild.toggle, alphanorebuild.toggle
// and colorstats.toggle flip them (seen within a second, at the next color
// change or group rebuild).

#ifndef PLUS_TECOLOR_H
#define PLUS_TECOLOR_H

#include "stdtypes.h"

#include <vector>

class LLColor4;
class LLDrawable;
class LLSpatialGroup;
class LLVOVolume;

namespace plus::tecolor
{
    // The <plus> hook in LLVOVolume::setTEColor(), at the start of the branch
    // where the face's color differs from the new one (te is valid). Returns
    // true when it applied the change itself (retval is then what
    // LLPrimitive::setTEColor returned); false: run upstream's code. Main thread.
    bool setTEColor(LLVOVolume* vobj, U8 te, const LLColor4& color, S32& retval);

    // The <plus> hooks in LLVolumeGeometryManager::rebuildGeom (a real rebuild,
    // GEOM_DIRTY or ALPHA_DIRTY) and rebuildMesh (MESH_DIRTY): times the
    // rebuild for the counters while LLRHI_COLOR_STATS is on, and tells which
    // rebuilds a color change caused. Nothing but a flag test otherwise.
    class BuildTimer
    {
    public:
        BuildTimer(LLSpatialGroup* group, bool mesh);
        ~BuildTimer();

        BuildTimer(const BuildTimer&) = delete;
        BuildTimer& operator=(const BuildTimer&) = delete;

    private:
        LLSpatialGroup* mGroup = nullptr;   // null: not counting this one
        bool mMesh = false;
        U64 mStart = 0;
        U64 mBytes = 0;                     // rebuildMesh: bytes the color drawables rewrite
        bool mColorOnly = true;             // rebuildMesh: every color drawable rewrites colors only
        std::vector<LLDrawable*> mColorDrawables;
    };
}

#endif // PLUS_TECOLOR_H
