/**
 * @file activecensus.h
 * @brief Why drawables became active (the census, R0), and the orphan reunion
 *        that leaves a still static parent static (R1), research/180 10.12.
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

#ifndef PLUS_ACTIVECENSUS_H
#define PLUS_ACTIVECENSUS_H

#include "llrhi.h"  // LLRHI::sFrameCostDetail, sActiveOrphan

#include <vector>

class LLDrawable;
class LLSpatialGroup;
class LLViewerObject;

// An active drawable is moved, idled and rebuilt as its own linkset every
// frame it is on the move list, and draws with its object's matrix; upstream
// never makes a linkset static again (LLDrawable::makeActive: "linked set
// will now NEVER become static"). In the dummy town about 1,000 prims stay
// active while still (research/180 10.11). R0 counts why they became active;
// R1 removes the cause found first: the orphan reunion. research/180 10.15:
// the census splits the moves by who made them and whether they changed
// anything; the move same (LLRHI_MOVE_SAME=1) keeps a static drawable static
// when its move changes nothing.
namespace plus::activecensus
{
    // Why a drawable became active. A linkset's prims made active along with
    // it take the reason of the prim that started it.
    enum Reason : U8
    {
        WHY_BEFORE = 0,  // before the census was on
        WHY_ORPHAN,      // its parent's move at the orphan reunion (findOrphans)
        WHY_MOVE,        // a root on the move list: a position, rotation or scale update
        WHY_ANIM,        // a child prim on the move list: moving against its root
        WHY_PHYSICS,     // velocity, acceleration or physics
        WHY_OMEGA,       // a spin (angular velocity)
        WHY_EDIT,        // selected (being edited)
        WHY_ATTACHMENT,
        WHY_FLEX,
        WHY_ANIMESH,
        WHY_VOLUME,      // volume or material updates from the server, more than 8 (LLVOVolume::onDrawableUpdateFromServer)
        WHY_LINK,        // linked to a prim already active (setDrawableParent)
        WHY_OTHER,
        WHY_COUNT
    };

    // The census runs with LLRHI_FRAME_COST=2. Main thread.
    inline bool censusOn() { return LLRHI::sFrameCostDetail; }

    // The <plus> line in LLDrawable::makeActive, where it turns ACTIVE: the
    // outermost one classifies the drawable, the ones nested in it (the
    // parent, the children) take that reason. Off: one flag test.
    class Activation
    {
    public:
        explicit Activation(LLDrawable* drawable) : mOn(censusOn())
        {
            if (mOn)
            {
                begin(drawable);
            }
        }
        ~Activation()
        {
            if (mOn)
            {
                end();
            }
        }
        Activation(const Activation&) = delete;
        Activation& operator=(const Activation&) = delete;

    private:
        static void begin(LLDrawable* drawable);
        static void end();
        const bool mOn;
    };

    // Names the reason of a makeActive the drawable's state does not tell
    // (LLVOVolume::onDrawableUpdateFromServer). Off: one flag test.
    class ReasonScope
    {
    public:
        explicit ReasonScope(Reason reason) : mOn(censusOn())
        {
            if (mOn)
            {
                mPrevious = push(reason);
            }
        }
        ~ReasonScope()
        {
            if (mOn)
            {
                push(mPrevious);
            }
        }
        ReasonScope(const ReasonScope&) = delete;
        ReasonScope& operator=(const ReasonScope&) = delete;

    private:
        static Reason push(Reason reason);  // returns the one before
        const bool mOn;
        Reason mPrevious = WHY_BEFORE;
    };

    // --- research/180 10.15: where a move came from, and moves that change nothing ---

    // Who put a drawable on the move list (LLPipeline::markMoved). The
    // innermost MoveScope names it; the state of the drawable and the
    // scope's object refine it.
    enum MoveSource : U8
    {
        SRC_BEFORE = 0,     // on the list before the census was on
        SRC_FULL,           // processUpdateMessage of an ObjectUpdate
        SRC_TERSE,          // ... of an ImprovedTerseObjectUpdate
        SRC_COMPRESSED,     // ... of an ObjectUpdateCompressed
        SRC_CACHED,         // ... of an object from the cache
        SRC_UPDATE_PARENT,  // an update of another object moved this one (a link change: its new or old parent)
        SRC_IDLE,           // idleUpdate (resolved to one of the three below)
        SRC_IDLE_SPIN,      // ... a spin (angular velocity)
        SRC_IDLE_MOTION,    // ... velocity or acceleration
        SRC_IDLE_OTHER,     // ... a MOVED flag left from before
        SRC_LINK,           // setDrawableParent, LLVOVolume::setParent
        SRC_ORPHAN,         // the orphan reunion (findOrphans)
        SRC_EDIT,           // selected
        SRC_ATTACHMENT,
        SRC_OTHER,
        SRC_COUNT
    };
    constexpr U8 SRC_MASK = 0x1f;
    constexpr U8 SRC_VIA_CHILD = 0x80;  // a parent put on the list by its child's move (markMoved's recursion)
    constexpr U8 SRC_SAME = 0x40;       // mPlusActiveSource: the move changed nothing (position, rotation, scale)
    constexpr U8 SRC_COUNTED = 0x40;    // mPlusMoveSource: the first updateMove since it was put on the list is counted

    // The census or the move same is on: the move scopes and hooks run.
    inline bool movesOn() { return LLRHI::sFrameCostDetail || LLRHI::sMoveSame; }

    // ObjectUpdate's kind (EObjectUpdateType) as a source.
    MoveSource updateSource(U32 update_type);

    // Names who moves within it, and the object it is for. Off: one flag test.
    class MoveScope
    {
    public:
        MoveScope(MoveSource source, const LLViewerObject* vobj) : mOn(movesOn())
        {
            if (mOn)
            {
                push(source, vobj);
            }
        }
        ~MoveScope()
        {
            if (mOn)
            {
                pop();
            }
        }
        MoveScope(const MoveScope&) = delete;
        MoveScope& operator=(const MoveScope&) = delete;

    private:
        void push(MoveSource source, const LLViewerObject* vobj);
        void pop();
        const bool mOn;
        MoveSource mPreviousSource = SRC_BEFORE;
        const LLViewerObject* mPreviousObject = nullptr;
    };

    // Around markMoved's recursion to the parent: the parent goes on the list
    // because of its child. Off: one flag test.
    class ViaChild
    {
    public:
        ViaChild() : mOn(movesOn())
        {
            if (mOn)
            {
                enter();
            }
        }
        ~ViaChild()
        {
            if (mOn)
            {
                leave();
            }
        }
        ViaChild(const ViaChild&) = delete;
        ViaChild& operator=(const ViaChild&) = delete;

    private:
        static void enter();
        static void leave();
        const bool mOn;
    };

    // The <plus> line in LLPipeline::markMoved where a (non bridge) drawable
    // goes on the move list: names its source. Main thread.
    void markedSlow(LLDrawable* drawable);
    inline void marked(LLDrawable* drawable)
    {
        if (movesOn())
        {
            markedSlow(drawable);
        }
    }

    // The <plus> guard in LLDrawable::updateMove, before upstream's
    // makeActive: counts the move by source and whether it changes anything
    // (census); keepsStatic() is true when LLRHI::sMoveSame leaves the static
    // drawable static (the move changes nothing). The destructor, after the
    // rest of upstream's move, verifies the drawable kept static is where its
    // object is (sMoveSameVerify) and queues the check 30 frames on.
    class MoveCheck
    {
    public:
        explicit MoveCheck(LLDrawable* drawable) : mOn(movesOn())
        {
            if (mOn)
            {
                begin(drawable);
            }
        }
        ~MoveCheck()
        {
            if (mOn)
            {
                end();
            }
        }
        bool keepsStatic() const { return mKeep; }
        MoveCheck(const MoveCheck&) = delete;
        MoveCheck& operator=(const MoveCheck&) = delete;

    private:
        void begin(LLDrawable* drawable);
        void end();
        const bool mOn;
        bool mKeep = false;
        U8 mPreviousMove = 0;
        LLDrawable* mDrawable = nullptr;    // for the length of updateMove only
    };

    // The <plus> hook in LLViewerObjectList::findOrphans, before upstream's
    // markMoved of the parent: true when the parent is to stay static
    // (LLRHI::sActiveOrphan on and the reunion would not move it: a static
    // root volume, not on the move list, its drawable's position, rotation
    // and scale already the object's, no velocity, acceleration, spin or
    // physics, not selected, not an attachment, flex or animesh, and the
    // child not active, not an avatar, not flex, not spinning). Then the
    // child is parented as when the parent came first (setDrawableParent
    // with a static parent: its xform at once, a rebuild in region space).
    // Counts the reunions (census and R1), and queues the verify. Main thread.
    bool reunionKeepsStatic(LLViewerObject* parent, LLViewerObject* child);

    // --- research/186 step 1: the spin census (LLRHI_FRAME_COST=2) ---
    // What the linksets only their spinning children keep active (the census's
    // "held only by spinning children", flagged at each line on their root
    // drawable) cost each frame, timed where their bridge's work is done; the
    // "spin census" line every 5 s. No change of behaviour.
    enum CostKind : U8
    {
        COST_MOVE = 0,      // updateMovedList: resolved to one of the three below
        COST_ROOT_MOVE,     // the root's updateMove, put on the list by its (spinning) child
        COST_ROOT_OWN,      // ... for its own reason (kept aside: not the spin's)
        COST_BRIDGE_MOVE,   // the bridge's updateMove (octree balance, the region partition's move)
        COST_SORT,          // stateSort(bridge) of the main view: changeLOD, updateDistance (every prim's LOD)
        COST_CULL,          // the bridge's cull (markVisible, setVisible): resolved to one of the three below
        COST_CULL_MAIN,
        COST_CULL_SHADOW,
        COST_CULL_OTHER,
        COST_KINDS
    };

    // Times the scope when the drawable (a root, or a bridge) is a flagged
    // linkset's, on the main thread, outermost only. Off: one flag test.
    class CostScope
    {
    public:
        CostScope(LLDrawable* drawable, CostKind kind) : mOn(censusOn())
        {
            if (mOn)
            {
                begin(drawable, kind);
            }
        }
        ~CostScope()
        {
            if (mTiming)
            {
                end();
            }
        }
        CostScope(const CostScope&) = delete;
        CostScope& operator=(const CostScope&) = delete;

    private:
        void begin(LLDrawable* drawable, CostKind kind);
        void end();
        const bool mOn;
        bool mTiming = false;
        CostKind mKind = COST_MOVE;
        S64 mStart = 0;
    };

    // In stateSort(bridge), when changeLOD asks for updateDistance: counts
    // the update and the prims it walks. Off: one flag test.
    void lodUpdatedSlow(LLDrawable* bridge);
    inline void lodUpdated(LLDrawable* bridge)
    {
        if (censusOn())
        {
            lodUpdatedSlow(bridge);
        }
    }

    // renderSunShadowCascades, before its sort: the flagged linksets' bridge
    // groups the main view's cascades saw (all go to the moving casters,
    // before plus/shadowfar), and their draw infos times cascades.
    void shadowGroupsSlow(const std::vector<LLSpatialGroup*>& groups);
    inline void shadowGroups(const std::vector<LLSpatialGroup*>& groups, bool main_view)
    {
        if (censusOn() && main_view)
        {
            shadowGroupsSlow(groups);
        }
    }

    // Reunions kept static waiting for their verify.
    extern bool sVerifyPending;

    // Once a frame after the move list (LLPipeline::updateMove): the verify
    // of the reunions kept static 30 frames ago, and the ActiveCensus line
    // every 5 s (the census walks the objects then, not each frame).
    void frameSlow();
    inline void frame()
    {
        if (LLRHI::sFrameCostDetail || LLRHI::sActiveOrphan || LLRHI::sMoveSame || sVerifyPending)
        {
            frameSlow();
        }
    }
}

#endif // PLUS_ACTIVECENSUS_H
