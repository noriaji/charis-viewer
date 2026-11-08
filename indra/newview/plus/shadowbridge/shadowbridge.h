/**
 * @file shadowbridge.h
 * @brief The sun shadow cascades' spatial bridge culls: each bridge's octree
 *        walked once per cascade set instead of once per cascade
 *        (LLRHI_SHADOW_BRIDGE_ONCE, research/178 section 9 in the sl-viewer
 *        repository).
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

#ifndef PLUS_SHADOWBRIDGE_H
#define PLUS_SHADOWBRIDGE_H

#include "stdtypes.h"

#include <chrono>

class LLCamera;
class LLCullResult;
class LLDrawable;

// LLPipeline::stateSortDrawableGroups marks every drawable of the bridge
// partitions' groups visible; for a spatial bridge that culls the bridge's own
// octree (LLSpatialBridge::setVisible): the camera moved into the bridge's
// frame (transformCamera) and every node of the octree handed to
// LLPipeline::markNotCulled. renderSunShadowCascades does this once per
// cascade, so up to four times per bridge and frame.
//
// The walk does not cull by the camera (LLOctreeMarkNotCulled visits every
// node), and the moved camera is only read by markNotCulled for the world
// camera (updateDistance). So for the sun shadow cameras the nodes and their
// order do not depend on the cascade. Inside one cascade set this module keeps
// each bridge's nodes from its first walk and hands them to markNotCulled
// again, in the same order, for the next cascades. What depends on the cascade
// (the frustum test, the attachment and impostor tests, the render type) is
// still done per cascade, as LLSpatialBridge::setVisible does it.
namespace plus::shadowbridge
{
    namespace detail
    {
        extern bool sActive;    // a cascade set's culls are on, with the switch on
    }

    // One renderSunShadowCascades call's cascade loop (RAII: the module is
    // active only inside it, whatever way the loop leaves).
    class CascadesScope
    {
    public:
        explicit CascadesScope(bool main_view);
        ~CascadesScope();
        CascadesScope(const CascadesScope&) = delete;
        CascadesScope& operator=(const CascadesScope&) = delete;
    };

    // Times one cascade's stateSortDrawableGroups of the main view, with the
    // switch on or off (the log's "ms per frame" compares the two).
    class SortTimer
    {
    public:
        explicit SortTimer(bool main_view);
        ~SortTimer();
        SortTimer(const SortTimer&) = delete;
        SortTimer& operator=(const SortTimer&) = delete;

    private:
        bool mOn;
        std::chrono::steady_clock::time_point mStart;
    };

    bool markVisibleSlow(LLDrawable* drawablep, LLCamera& camera, LLCullResult& cull);

    // From LLPipeline::stateSortDrawableGroups, for each drawable: true when it
    // did LLPipeline::markVisible's work for a spatial bridge itself. False
    // (do markVisible) outside a cascade set, with the switch off, and for
    // everything but a live spatial bridge in a sun shadow camera's cull.
    inline bool markVisible(LLDrawable* drawablep, LLCamera& camera, LLCullResult& cull)
    {
        return detail::sActive && markVisibleSlow(drawablep, camera, cull);
    }
}

#endif // PLUS_SHADOWBRIDGE_H
