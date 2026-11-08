/**
 * @file cullinto.h
 * @brief The main camera's octree walks on the cull threads, by subtree below
 *        each partition's root (cull-par, research/184 A1 in the sl-viewer
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

#ifndef PLUS_CULLINTO_CULLINTO_H
#define PLUS_CULLINTO_CULLINTO_H

#include "stdtypes.h"
#include "llrhi.h"      // LLRHI::sFrameCostDetail, cpuMark (costMark)

class LLCamera;

// The <plus> hooks in LLPipeline::updateCull (the partition loop) and in the
// cull threads' constructor (their number).
//
// The walk of a partition's octree for the main camera (LLSpatialPartition::
// cull, LLOctreeCull) writes, besides LLPipeline::sCull and a few queues, only
// the groups it reaches and their descendants: the occlusion check (reading
// the parent's OCCLUDED, setting and clearing OCCLUDED down the group's own
// branch), the visible frame, the distance and pixel area. So the main thread
// rebounds every root, does each root's part (its check and its visit), and
// the subtrees below the roots are walked at the same time on the cull
// threads and by itself (fork and join). Each walk records what it would
// write elsewhere (plus/cullinto/cullrecord.h); after the join the main
// thread does it in the order of the serial walk (regions, partitions,
// children), with each region's object cache cull after its partitions as
// before. Same lists, same order, same queues.
//
// Only for the world camera's frame cull (not the shadow, water, probe,
// impostor or HUD cameras), with the cull threads on (LLRHI_CULL_THREADS) and
// the occlusion answers read natively (LLRHI_OCCLUSION_BATCH, not the GL
// path, which a cull thread must not take). Switch: LLRHI::sCullPar (llrhi.h).
namespace plus::cullinto
{
    // The head of LLPipeline::updateCull, after sCull was cleared: the
    // partition loop (with the object cache culls) done here; false when the
    // caller does it (off, or not this camera / mode).
    bool run(LLCamera& camera);

    // The number of cull threads (besides the calling thread), once when they
    // start: PlusCullThreads 1 to 4, 0 (default) the cores less one up to 3;
    // never more than the cores less one.
    U32 cullThreads(U32 cores);

    // The world camera's frame cull (not the shadow, water, probe, impostor or
    // HUD cameras, not a cube snapshot).
    bool isMainCull(const LLCamera& camera);

    // research/184 9: the main camera's cull cut finer in the FrameCost line
    // (LLRHI_FRAME_COST=2, main thread): "cull setup" (references, the result's
    // clear), "cull rebound" (the roots' bounds), "cull walk" (the octree walks:
    // the checks, occlusion and markNotCulled), "cull vo cache" (the object
    // cache culls), "cull sky"; with cull-par also "cull roots" (each root's own
    // part and the split), "cull jobs" (the fork and join: the main thread's
    // share and the wait), "cull verify" / "cull redo", "cull apply" (the
    // records done in order), "cull end" (the counts, the auto mode, the log).
    // After updateCull: "pre-gbuffer after cull" (on to
    // the sun shadows). Their sum is the old "pre-gbuffer cull" segment. Off,
    // one flag test.
    inline bool costDetail(const LLCamera& camera)
    {
        return LLRHI::sFrameCostDetail && isMainCull(camera);
    }
    inline void costMark(bool on, const char* name)
    {
        if (on)
        {
            LLRHI::cpuMark(name);
        }
    }
}

#endif // PLUS_CULLINTO_CULLINTO_H
