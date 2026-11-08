/**
 * @file shadowreuse.cpp
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

// Why handing over a walk is safe (the proof is in research/113, section 3):
//  - The walk is LLOctreeCullShadowInto over the partition's octree: for each
//    node, AABBInFrustum against the camera's planes (count, masks, planes)
//    with the node's bounds, then for leaves or partly inside nodes the
//    objects' bounds; SKIP_FRUSTUM_CHECK; then for each group it reaches,
//    isEmpty, the GPU shadow list residency and kind against sRHIListCullKinds,
//    and the zero coverage test of its object extents in the coverage view.
//    It writes nothing but its result lists. The same inputs give the same
//    lists in the same order.
//  - Bounds, extents, SKIP_FRUSTUM_CHECK and the shape change only in
//    LLViewerOctreeGroup::rebound, which does something only for a dirty
//    group; a group gets dirty only in unbound, which goes up to the root
//    (each parent it makes dirty counts it) and is called for every insertion,
//    removal, move or resize of an entry and every node added or removed
//    (LLViewerOctreeGroup / LLSpatialGroup handle*, updateInGroup). The one
//    other writer of bounds is LLSpatialGroup::shift (a region crossing),
//    counted here by noteShift. Element counts change only with insertions
//    and removals.
//  - Dirty groups always have dirty ancestors (unbound stops only at a dirty
//    parent; rebound cleans a group after its children). So a root that is
//    clean now (just rebound) and has the unbound count it had right after
//    the last walk had no change anywhere below it in between.
//  - The root is compared by its serial (LLViewerOctreeGroup::getRHISerial),
//    never the same for two groups, so a partition freed and another made at
//    the same address cannot pass. The stored groups are followed only when
//    the partition passes, and then each of them is still in that octree.
//  - Residency changes are counted by LLPipeline (mRHIListResidency); the
//    camera's planes and the coverage view are compared bit for bit.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "shadowreuse.h"

#include "llappviewer.h"
#include "llcamera.h"
#include "llspatialpartition.h"
#include "llvieweroctree.h"

#include <cstring>
#include <sstream>

namespace plus::shadowreuse
{
namespace
{
    constexpr U32 MAX_CASCADES = 4;
    constexpr U32 MAX_PLANES = LLCamera::AGENT_PLANE_USER_CLIP_NUM;

    // A partition as this frame's walks see it (main thread, before them).
    struct PartKey
    {
        const LLSpatialPartition* part = nullptr;  // compared, never followed
        U64 root_serial = 0;
        U32 root_unbounds = 0;
        U32 group_deaths = 0;   // the partition's mRHIGroupDeaths (balance() frees groups silently)
        bool clean = false;
    };

    // One partition's last walk for one cascade.
    struct Slice
    {
        PartKey key;
        bool valid = false;
        std::vector<LLSpatialGroup*> drawable_groups;
        std::vector<LLSpatialGroup*> visible_groups;
        std::vector<LLSpatialGroup*> skipped;   // left out by the zero coverage cull
    };

    // What a cascade's walks read besides the octrees, bit for bit.
    struct Inputs
    {
        S32 index = -1;
        U32 shifts = 0;
        U32 residency = 0;
        U32 cull_kinds = 0;
        U32 plane_count = 0;
        U8 plane_mask[MAX_PLANES] = {};
        F32 planes[MAX_PLANES][4] = {};
        bool coverage = false;
        F32 view_proj[16] = {};
        F32 width = 0.f;
        F32 height = 0.f;
        bool coverage_active = false;
        bool coverage_cull = false;

        bool operator==(const Inputs& rhs) const
        {
            // memcmp: bits, not values (-0 and +0, NaN), as the walk would see them.
            return index == rhs.index && shifts == rhs.shifts && residency == rhs.residency &&
                   cull_kinds == rhs.cull_kinds && plane_count == rhs.plane_count &&
                   !memcmp(plane_mask, rhs.plane_mask, sizeof(plane_mask)) && !memcmp(planes, rhs.planes, sizeof(planes)) &&
                   coverage == rhs.coverage && !memcmp(view_proj, rhs.view_proj, sizeof(view_proj)) &&
                   !memcmp(&width, &rhs.width, sizeof(width)) && !memcmp(&height, &rhs.height, sizeof(height)) &&
                   coverage_active == rhs.coverage_active && coverage_cull == rhs.coverage_cull;
        }
    };

    struct Cascade
    {
        Inputs inputs;              // what every valid slice was walked with
        bool has_inputs = false;
        bool began = false;         // beginCascade ran for this call
        bool same = false;          // this frame's inputs are those
        std::vector<Slice> slices;  // by position in the partition list
        // Counts of this frame, written by this cascade's thread only.
        U32 walked = 0;
        U32 handed = 0;
        U64 groups_handed = 0;
    };

    struct Stats
    {
        U32 frames = 0;             // frames with the walks kept
        U32 frames_off = 0;         // the main view's walks while the switch (or tiny stats) is off
        U64 walked = 0;
        U64 handed = 0;
        U64 groups_handed = 0;
        U32 cascades_same = 0;
        U32 cascades_other = 0;     // other camera, coverage view, residency or shift
        F32 logged_at = 0.f;
    };

    bool sOn = false;               // this call's walks are kept
    U32 sShifts = 0;
    U32 sResidency = 0;             // this call's, from beginWalks
    U32 sCullKinds = 0;
    U32 sCascadeCount = 0;
    std::vector<PartKey> sParts;
    Cascade sCascades[MAX_CASCADES];
    Stats sStats;

    void forget()
    {
        for (Cascade& c : sCascades)
        {
            c = Cascade();
        }
        sParts.clear();
    }

    template <typename T>
    void copy_into(std::vector<LLSpatialGroup*>& into, T begin, T end)
    {
        into.assign(begin, end);
    }
}

void beginWalks(bool keep, const std::vector<LLSpatialPartition*>& parts, U32 cascades, U32 residency, U32 cull_kinds)
{
    // LLRHI_TINY_STATS counts what each walk's coverage test sees: walk then.
    const bool on = LLRHI::sShadowSameInput && !LLRHI::sTinyStats;
    sOn = keep && on && cascades <= MAX_CASCADES;
    if (!sOn)
    {
        if (keep)
        {
            ++sStats.frames_off;
            if (!on && !sParts.empty())
            {
                forget();   // switched off: nothing is kept
            }
        }
        return;
    }
    sCascadeCount = cascades;
    sResidency = residency;
    sCullKinds = cull_kinds;
    sParts.resize(parts.size());
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const LLViewerOctreeGroup* root = (const LLViewerOctreeGroup*)parts[i]->mOctree->getListener(0);
        PartKey& key = sParts[i];
        key.part = parts[i];
        key.root_serial = root ? root->getRHISerial() : 0;
        key.root_unbounds = root ? root->getUnboundCount() : 0;
        key.group_deaths = parts[i]->mRHIGroupDeaths;
        key.clean = root && !root->isDirty();
    }
    for (U32 k = 0; k < cascades; ++k)
    {
        Cascade& c = sCascades[k];
        c.slices.resize(parts.size());  // positions past the end are dropped; new ones start invalid
        c.began = false;                // until beginCascade compares
        c.same = false;
        c.walked = c.handed = 0;
        c.groups_handed = 0;
    }
}

void beginCascade(U32 k, S32 index, LLCamera& camera, const LLRHI::CoverageView* coverage)
{
    if (!sOn || k >= sCascadeCount)
    {
        return;
    }
    Cascade& c = sCascades[k];
    Inputs now;
    now.index = index;
    now.shifts = sShifts;
    now.residency = sResidency;
    now.cull_kinds = sCullKinds;
    now.plane_count = camera.getAgentPlaneCount();
    for (U32 i = 0; i < MAX_PLANES && i < now.plane_count; ++i)
    {
        now.plane_mask[i] = camera.getAgentPlaneMask(i);
        const LLPlane& plane = camera.getAgentPlane(i);
        for (S32 j = 0; j < 4; ++j)
        {
            now.planes[i][j] = plane[j];
        }
    }
    if (coverage)
    {
        now.coverage = true;
        memcpy(now.view_proj, coverage->view_proj, sizeof(now.view_proj));
        now.width = coverage->width;
        now.height = coverage->height;
        now.coverage_active = coverage->active;
        now.coverage_cull = coverage->cull;
    }
    c.same = c.has_inputs && c.inputs == now;
    if (!c.same)
    {
        // Every slice is walked again this frame with the new inputs, or, at a
        // position this frame does not reach, stays invalid: the valid slices
        // are always walked with c.inputs.
        for (Slice& slice : c.slices)
        {
            slice.valid = false;
        }
    }
    c.inputs = now;
    c.has_inputs = true;
    c.began = true;
}

void walk(U32 k, size_t i, LLSpatialPartition* part, LLCamera& camera, LLCullResult& result,
          LLRHI::CoverageView* coverage, std::vector<LLSpatialGroup*>* skipped)
{
    if (!sOn || k >= sCascadeCount || i >= sParts.size() || !sCascades[k].began)
    {
        if (sOn && k < sCascadeCount && i < sCascades[k].slices.size())
        {
            sCascades[k].slices[i].valid = false;   // walked with inputs nobody noted
        }
        part->cullShadowInto(camera, result, coverage, skipped);
        return;
    }
    Cascade& c = sCascades[k];
    Slice& slice = c.slices[i];
    const PartKey& key = sParts[i];
    if (c.same && slice.valid && key.clean && slice.key.part == key.part && slice.key.root_serial == key.root_serial &&
        slice.key.root_unbounds == key.root_unbounds && slice.key.group_deaths == key.group_deaths && (skipped || slice.skipped.empty()))
    {
        // In the order the walk would push them (two lists, each in walk order).
        for (LLSpatialGroup* group : slice.drawable_groups)
        {
            result.pushDrawableGroup(group);
        }
        for (LLSpatialGroup* group : slice.visible_groups)
        {
            result.pushVisibleGroup(group);
        }
        if (skipped)
        {
            skipped->insert(skipped->end(), slice.skipped.begin(), slice.skipped.end());
        }
        ++c.handed;
        c.groups_handed += slice.drawable_groups.size() + slice.visible_groups.size() + slice.skipped.size();
        return;
    }
    const U32 drawable_from = result.getDrawableGroupsSize();
    const U32 visible_from = result.getVisibleGroupsSize();
    const size_t skipped_from = skipped ? skipped->size() : 0;
    part->cullShadowInto(camera, result, coverage, skipped);
    copy_into(slice.drawable_groups, result.beginDrawableGroups() + drawable_from, result.endDrawableGroups());
    copy_into(slice.visible_groups, result.beginVisibleGroups() + visible_from, result.endVisibleGroups());
    if (skipped)
    {
        copy_into(slice.skipped, skipped->begin() + skipped_from, skipped->end());
    }
    else
    {
        slice.skipped.clear();
    }
    slice.key = key;
    slice.valid = key.clean;
    ++c.walked;
}

void endWalks()
{
    if (!sOn)
    {
        return;
    }
    sOn = false;
    Stats& st = sStats;
    ++st.frames;
    for (U32 k = 0; k < sCascadeCount; ++k)
    {
        const Cascade& c = sCascades[k];
        st.walked += c.walked;
        st.handed += c.handed;
        st.groups_handed += c.groups_handed;
        ++(c.same ? st.cascades_same : st.cascades_other);
    }
    const F32 now = gFrameTimeSeconds;
    if (now - st.logged_at < 5.f)
    {
        return;
    }
    // fix-logcache: the counts start over every 5 s either way; written only
    // with LLRHI::sPeriodicLogs (development starts).
    if (st.frames && LLRHI::sPeriodicLogs)
    {
        const F64 per = 1.0 / st.frames;
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(1);
        line << "sun shadow walks kept (LLRHI_SHADOW_SAME_INPUT) over " << st.frames << " frames (off " << st.frames_off
             << "): partition walks per frame handed over " << st.handed * per << " of " << (st.handed + st.walked) * per
             << " (groups " << st.groups_handed * per << "); cascades with the same inputs " << st.cascades_same
             << ", other camera, coverage view, GPU list residency or region shift " << st.cascades_other;
        LL_INFOS("RHIShadow") << line.str() << LL_ENDL;
    }
    st = Stats();
    st.logged_at = now;
}

void noteShift()
{
    ++sShifts;
}
}
