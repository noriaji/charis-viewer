/**
 * @file cullreuse.h
 * @brief The main camera's octree walks replayed from their last trace while
 *        the camera and the octree stay the same: the frustum tests and the
 *        tree's descent are skipped, the occlusion checks and the visits done
 *        again (cull-reuse, research/188 in the sl-viewer repository).
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

#ifndef PLUS_CULLINTO_CULLREUSE_H
#define PLUS_CULLINTO_CULLREUSE_H

#include "stdtypes.h"
#include "llvieweroctree.h"     // OctreeNode, LLViewerOctreeGroup

#include <vector>

class LLCamera;
class LLSpatialPartition;

// The <plus> hooks in LLPipeline::updateCull (the partition loop),
// LLPipeline::shiftObjects and LLSpatialPartition::cullTraced.
//
// The main camera's walk of a partition (LLSpatialPartition::cull,
// LLOctreeCull or LLOctreeCullNoFarClip) calls, in a fixed order, earlyFail
// (the occlusion check, then OCCLUDED) on every node it reaches, the frustum
// test where the frustum result it carries does not settle it, and visit
// (checkObjects, then processGroup = markNotCulled) on every node it goes
// into. The frustum tests and checkObjects read only the camera's planes,
// origin and corner distance and the octree (bounds, extents, element and
// child counts, SKIP_FRUSTUM_CHECK), which change only with an unbound or a
// region shift (plus/shadowreuse's proof, research/113 section 3). So while
// those stay the same, the walk's trace (every node reached, in order, with
// the frustum result it was reached with, the one its children were walked
// with, and whether visit processed it) tells the walk without the tests:
// the replay calls earlyFail and processGroup on the same nodes in the same
// order, and where an occlusion check answers otherwise than in the trace
// (a node seen again, or a sibling reached with another frustum result
// after it) walks that subtree anew. Everything earlyFail and markNotCulled
// do runs every frame as before.
//
// Only for the world camera's frame cull (plus::cullinto::isMainCull: not
// the shadow, water, probe, impostor or HUD cameras, not a cube snapshot),
// not with cull-par (its walks go first). Switches: LLRHI::sCullReuse,
// sCullReuseVerify, sCullReuseAuto (llrhi.h).
namespace plus::cullreuse
{
// ---- research/188 cull reuse rule (tools/selftest/sim/gen_cullreuse.py extracts this block) ----
    enum : U32
    {
        TRACE_OCCLUDED = 1,     // earlyFail answered true: nothing below was reached
        TRACE_FULL = 2,         // reached with 2, or with 1 and SKIP_FRUSTUM_CHECK: no test of its own
        TRACE_PROCESSED = 4,    // visit's checkObjects passed: processGroup ran
    };

    // A node the walk reached, in the walk's order (its subtree follows it).
    template <class NodeT, class GroupT>
    struct TraceEntry
    {
        NodeT node;
        GroupT group;
        U32 end;        // the index past its subtree
        S32 res_in;     // the frustum result it was reached with (the walk's mRes)
        S32 res;        // the one its children were walked with (0: none)
        U32 flags;
    };

    // W (the walk's culler) gives: Node, Group, Entry (TraceEntry<Node, Group>),
    // group(n), earlyFail(g), skipFrustum(g), frustumCheck(g), visitObjects(n, g, res)
    // (preprocess and checkObjects with mRes = res), processGroup(g), childCount(n),
    // child(n, i), prefetch(g).
    struct CullTrace
    {
        // LLViewerOctreeCull::traverse(n) reached with mRes == res, every node it
        // reaches appended to out; returns the mRes it leaves. after_early: earlyFail
        // of n was called already and answered false.
        template <class W>
        static S32 walk(W& w, typename W::Node n, S32 res, bool after_early, std::vector<typename W::Entry>& out)
        {
            const U32 at = (U32)out.size();
            const typename W::Group group = w.group(n);
            out.push_back({ n, group, at + 1, res, 0, 0 });
            if (!after_early && w.earlyFail(group))
            {
                out[at].flags = TRACE_OCCLUDED;
                return res;     // traverse returns before it touches mRes
            }
            S32 children_res = res;
            if (res == 2 || (res && w.skipFrustum(group)))
            {
                out[at].flags |= TRACE_FULL;
            }
            else
            {
                children_res = w.frustumCheck(group);
            }
            out[at].res = children_res;
            S32 cur = children_res;
            if (children_res)
            {
                // traverseChildren: visit(n), then each child reached with what the one before left.
                if (w.visitObjects(n, group, children_res))
                {
                    out[at].flags |= TRACE_PROCESSED;
                    w.processGroup(group);
                }
                const U32 count = w.childCount(n);
                for (U32 i = 0; i < count; ++i)
                {
                    cur = walk(w, w.child(n, i), cur, false, out);
                }
            }
            out[at].end = (U32)out.size();
            return (out[at].flags & TRACE_FULL) ? cur : 0;     // a node that tested resets mRes
        }

        // prev[i]'s subtree walked as the walk would go now, while every frustum
        // result prev holds is still the one a test would give: the trace of this
        // walk appended to next. cur: the walk's mRes, in and out. Returns
        // prev[i].end.
        template <class W>
        static U32 replay(W& w, const std::vector<typename W::Entry>& prev, U32 i, S32& cur, std::vector<typename W::Entry>& next)
        {
            const typename W::Entry& p = prev[i];
            if (i + PREFETCH < prev.size())
            {
                w.prefetch(prev[i + PREFETCH].group);
            }
            if (cur != p.res_in)
            {
                // Reached otherwise than in the trace (a sibling before it answered
                // its occlusion check otherwise): walked anew from here.
                cur = walk(w, p.node, cur, false, next);
                return p.end;
            }
            if (w.earlyFail(p.group))
            {
                const U32 at = (U32)next.size();
                next.push_back({ p.node, p.group, at + 1, cur, 0, TRACE_OCCLUDED });
                return p.end;   // cur stays
            }
            if (p.flags & TRACE_OCCLUDED)
            {
                // Seen again: what is below it was not reached in the trace.
                cur = walk(w, p.node, cur, true, next);
                return p.end;
            }
            const U32 at = (U32)next.size();
            next.push_back(p);
            S32 children = p.res;
            if (p.res)
            {
                if (p.flags & TRACE_PROCESSED)
                {
                    w.processGroup(p.group);
                }
                for (U32 j = i + 1; j < p.end;)
                {
                    j = replay(w, prev, j, children, next);
                }
            }
            next[at].end = (U32)next.size();
            cur = (p.flags & TRACE_FULL) ? children : 0;
            return p.end;
        }

        // The walk from the root (mRes 0, as a new culler starts): replayed from prev
        // when it holds one, else walked; next is this walk's trace.
        template <class W>
        static void run(W& w, typename W::Node root, const std::vector<typename W::Entry>* prev, std::vector<typename W::Entry>& next)
        {
            next.clear();
            S32 cur = 0;
            if (prev && !prev->empty() && (*prev)[0].node == root && (*prev)[0].end == prev->size())
            {
                replay(w, *prev, 0, cur, next);
            }
            else
            {
                walk(w, root, 0, false, next);
            }
        }

        static constexpr U32 PREFETCH = 4;     // entries ahead whose group is loaded
    };
// ---- end of the research/188 cull reuse rule ----

    typedef TraceEntry<const OctreeNode*, LLViewerOctreeGroup*> Entry;

    // A partition's walk in trace form (LLSpatialPartition::cullTraced).
    struct Trace
    {
        std::vector<Entry> nodes;
    };

    // The head of LLPipeline::updateCull's partition loop (after cull-par
    // passed). walks: the loop below walks the partitions (not a probe face's
    // shared cull).
    void beginCull(LLCamera& camera, bool walks);

    // In the loop, for each partition the loop would cull (in its order):
    // part->cull(camera), or its replay.
    void cull(LLSpatialPartition* part, LLCamera& camera);

    // In the loop, for each region position a partition is not culled at
    // (none there, or its render type off): forgets that position.
    void skip();

    // After the loop: the counts, the auto mode, a VKPerf line every 5 s.
    void endCull();

    // LLPipeline::shiftObjects: every octree's bounds moved without an unbound.
    void noteShift();
}

#endif // PLUS_CULLINTO_CULLREUSE_H
