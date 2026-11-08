/**
 * @file shadowreuse.h
 * @brief The main view's sun shadow walks kept from frame to frame while
 *        nothing they read changed (LLRHI_SHADOW_SAME_INPUT,
 *        research/113-static-shadow-reuse.md in the sl-viewer repository).
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

#ifndef PLUS_SHADOWREUSE_SHADOWREUSE_H
#define PLUS_SHADOWREUSE_SHADOWREUSE_H

#include "stdtypes.h"
#include "llrhi.h"

#include <vector>

class LLCamera;
class LLCullResult;
class LLSpatialGroup;
class LLSpatialPartition;

// The <plus> hooks in LLPipeline::renderSunShadowCascades (the cull threads'
// walk of each cascade, LLRHI_CULL_THREADS) and LLPipeline::shiftObjects.
//
// A partition's walk for a cascade (LLSpatialPartition::cullShadowInto) only
// reads the cascade camera's planes, the octree (each node's bounds, extents,
// element count and SKIP_FRUSTUM_CHECK), the groups' GPU shadow list
// residency with LLPipeline::sRHIListCullKinds, and the zero coverage view.
// The octree changes only through LLViewerOctreeGroup::unbound (an entry
// inserted, removed, moved or resized; a node added or removed), which marks
// the node and every ancestor dirty and counts the clean root turning dirty,
// or through LLSpatialGroup::shift (a region crossing, noteShift). So when the
// partition's root is the same group (same serial), clean after its rebound,
// with the same unbound count, and the camera, the coverage view, the
// residency and the shifts are the same as at its last walk, that walk's
// groups are what a new walk would find, in the same order: they are handed
// over instead. Everything the caller does with them afterwards (setVisible,
// grabReferences, the object cache and sky culls, the state sorts, the
// casters' fingerprints, the drawing) runs every frame as before.
namespace plus::shadowreuse
{
    // Main thread, before the cascades' walks, after every partition's root
    // was rebound. keep: this call is the main view's sun cascades (not a
    // probe's, not a cube snapshot); residency: LLPipeline's count of GPU
    // shadow list residency changes; cull_kinds: sRHIListCullKinds.
    void beginWalks(bool keep, const std::vector<LLSpatialPartition*>& parts, U32 cascades, U32 residency, U32 cull_kinds);

    // Main thread, after beginWalks, for each cascade k: its camera as the
    // walk sees it (with the water clip plane) and its zero coverage view.
    void beginCascade(U32 k, S32 index, LLCamera& camera, const LLRHI::CoverageView* coverage);

    // Cull thread of cascade k (only it touches k's lists): parts[i]'s walk
    // into result and skipped, or the last one handed over.
    void walk(U32 k, size_t i, LLSpatialPartition* part, LLCamera& camera, LLCullResult& result,
              LLRHI::CoverageView* coverage, std::vector<LLSpatialGroup*>* skipped);

    // Main thread, after the walks: the counts, a #RHIShadow line every 5 s.
    void endWalks();

    // LLPipeline::shiftObjects: every octree's bounds moved without an unbound.
    void noteShift();
}

#endif // PLUS_SHADOWREUSE_SHADOWREUSE_H
