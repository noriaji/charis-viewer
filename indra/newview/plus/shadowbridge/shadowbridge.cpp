/**
 * @file shadowbridge.cpp
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

// Why handing the nodes over gives the same cull (research/178 section 9):
//  - LLSpatialBridge::setVisible, for a cull that is not for selection, ends in
//    LLOctreeMarkNotCulled(transformCamera(camera)).traverse(mOctree): for every
//    node in preorder (node, then its children 0..n-1), the node's group
//    setVisible, then LLPipeline::markNotCulled(group, moved camera). No node
//    is left out by the camera.
//  - markNotCulled reads the camera only for the world camera (updateDistance,
//    CAMERA_WORLD and not gCubeSnapshot). Here the camera is a sun shadow
//    camera (checked per call), so the moved camera is never read and
//    transformCamera is not needed. Everything else markNotCulled reads (the
//    group, GPU list residency and kinds, the coverage view, sCull, the current
//    camera id) is read again on every call, as before.
//  - The nodes of an octree change only by insertions and removals, which
//    unbound the groups up to the root (see plus/shadowreuse/shadowreuse.cpp,
//    the proof of research/113 section 3): a root that is clean now, with the
//    serial and unbound count it had when its nodes were taken (also clean,
//    just rebound), has the same nodes in the same order. Anything else walks
//    the octree again.
//  - The nodes are kept for one cascade set (one renderSunShadowCascades
//    call) only: nothing is followed from one frame to the next.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "shadowbridge.h"

#include "llcamera.h"
#include "lldrawable.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llvieweroctree.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace plus::shadowbridge
{
namespace detail
{
    bool sActive = false;
}

namespace
{
    constexpr char SWITCH_ENV[] = "LLRHI_SHADOW_BRIDGE_ONCE";   // "0": every cascade walks (the official way)
    constexpr char VERIFY_ENV[] = "LLRHI_SHADOW_BRIDGE_VERIFY"; // "1": every hand over is compared with a walk
    constexpr char SWITCH_CUE[] = "shadowbridge.toggle";
    constexpr F64 CUE_POLL_SECONDS = 1.0;
    constexpr F32 LOG_SECONDS = 5.f;

    bool& switchOn()
    {
        static bool on = LLRHI::envDefaultOn(SWITCH_ENV);
        return on;
    }

    bool verifyOn()
    {
        static const bool on = LLRHI::envIs(VERIFY_ENV, "1");
        return on;
    }

    // At most once a second, with LLRHI_CUE_DIR only: shadowbridge.toggle
    // flips the switch for an A/B in one start (tools/native-ab.ps1). Between
    // cascade sets only.
    void pollCue()
    {
        static LLRHI::CuePoll cue_poll(CUE_POLL_SECONDS);
        if (!cue_poll.due() || !LLRHI::takeCue(SWITCH_CUE))
        {
            return;
        }
        switchOn() = !switchOn();
        LL_INFOS("RHIProfile") << "shadow bridge culls once per cascade set (research/178 C1b) "
                               << (switchOn() ? "on" : "off") << LL_ENDL;
    }

    inline U64 mix64(U64 h)
    {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    // A bridge's nodes taken in this cascade set: sNodes[begin, begin + count).
    struct Entry
    {
        const LLSpatialBridge* bridge = nullptr;    // compared, never followed
        U32 gen = 0;                                // the cascade set it was taken in
        U64 root_serial = 0;
        U32 root_unbounds = 0;
        U32 group_deaths = 0;                       // the bridge's mRHIGroupDeaths: a group freed, taken again
        U32 begin = 0;
        U32 count = 0;
    };

    // Open addressing by bridge; an entry of an older cascade set is a free
    // slot, so nothing is cleared between sets. Keeps its storage.
    class Table
    {
    public:
        // The entry for bridge in this set: found, or a new one (bridge set, gen
        // this set, the rest to be filled by the caller).
        Entry& at(const LLSpatialBridge* bridge, U32 gen, bool& found)
        {
            if ((mUsed + 1) * 2 > mSlots.size())
            {
                grow(gen);
            }
            size_t i = slot(bridge);
            while (mSlots[i].gen == gen)
            {
                if (mSlots[i].bridge == bridge)
                {
                    found = true;
                    return mSlots[i];
                }
                i = (i + 1) & mMask;
            }
            found = false;
            ++mUsed;
            mSlots[i] = Entry();
            mSlots[i].bridge = bridge;
            mSlots[i].gen = gen;
            return mSlots[i];
        }

        void beginSet()
        {
            mUsed = 0;
        }

        // gen wrapped to 0: no slot may look like the new sets' ones.
        void forget()
        {
            for (Entry& e : mSlots)
            {
                e = Entry();
            }
            mUsed = 0;
        }

    private:
        size_t slot(const LLSpatialBridge* bridge) const
        {
            return (size_t)(mix64((U64)(uintptr_t)bridge) & mMask);
        }

        void grow(U32 gen)
        {
            std::vector<Entry> old;
            old.swap(mSlots);
            const size_t size = old.empty() ? 1024 : old.size() * 2;
            mSlots.assign(size, Entry());
            mMask = size - 1;
            for (const Entry& e : old)
            {
                if (e.gen == gen)
                {
                    size_t i = slot(e.bridge);
                    while (mSlots[i].gen == gen)
                    {
                        i = (i + 1) & mMask;
                    }
                    mSlots[i] = e;
                }
            }
        }

        std::vector<Entry> mSlots;
        size_t mMask = 0;
        size_t mUsed = 0;
    };

    struct Stats
    {
        U32 sets = 0;               // main view cascade sets with the switch on
        U32 sets_off = 0;           // main view cascade sets with the switch off
        U64 bridges = 0;            // bridge culls (pushBridge) in sets with the switch on
        U64 in_frustum = 0;         // of those, inside the cascade (their nodes handed to markNotCulled)
        U64 walks = 0;              // octree walks
        U64 handed = 0;             // nodes handed over without a walk (bridges)
        U64 nodes_handed = 0;
        U64 checked = 0;            // hand overs compared with a walk (LLRHI_SHADOW_BRIDGE_VERIFY=1)
        U64 mismatched = 0;         // of those, not the same nodes in the same order
        U64 not_kept = 0;           // walks with the root still dirty after rebound (not kept)
        F64 sort_seconds_on = 0.0;  // main view drawable group sorts (all cascades), switch on
        F64 sort_seconds_off = 0.0; // the same, switch off
        F32 logged_at = 0.f;
    };

    U32 sGen = 0;
    bool sMainView = false;
    Table sTable;
    std::vector<LLSpatialGroup*> sNodes;    // this cascade set's nodes, by bridge spans
    std::vector<LLSpatialGroup*> sScratch;  // a walk to compare with (verify)
    Stats sStats;
    Stats sOtherViews;              // the probes' shared maps: counted, not logged
    Stats* sCount = &sStats;        // this cascade set's counts

    // LLOctreeMarkNotCulled's order: the node, then its children 0..n-1.
    void collect(const OctreeNode* node, std::vector<LLSpatialGroup*>& out)
    {
        out.push_back((LLSpatialGroup*)node->getListener(0));
        for (U32 i = 0; i < node->getChildCount(); ++i)
        {
            collect(node->getChild(i), out);
        }
    }

    // Takes the bridge's nodes into sNodes; returns the entry's span.
    void take(Entry& entry, const LLSpatialBridge* bridge, const LLSpatialGroup* root)
    {
        entry.root_serial = root->getRHISerial();
        entry.root_unbounds = root->getUnboundCount();
        entry.group_deaths = bridge->mRHIGroupDeaths;
        entry.begin = (U32)sNodes.size();
        collect(bridge->mOctree, sNodes);
        entry.count = (U32)sNodes.size() - entry.begin;
        ++sCount->walks;
    }

    // The bridge's nodes for this cascade (root: its root group, just rebound).
    // Returns the first index into sNodes and sets count.
    U32 nodesOf(LLSpatialBridge* bridge, const LLSpatialGroup* root, U32& count)
    {
        if (root->isDirty())
        {
            // Not clean after rebound: not a state the hand over is proved for.
            const U32 begin = (U32)sNodes.size();
            collect(bridge->mOctree, sNodes);
            count = (U32)sNodes.size() - begin;
            ++sCount->walks;
            ++sCount->not_kept;
            return begin;
        }
        bool found = false;
        Entry& entry = sTable.at(bridge, sGen, found);
        if (!found || entry.root_serial != root->getRHISerial() || entry.root_unbounds != root->getUnboundCount() ||
            entry.group_deaths != bridge->mRHIGroupDeaths)
        {
            take(entry, bridge, root);
            count = entry.count;
            return entry.begin;
        }
        if (verifyOn())
        {
            sScratch.clear();
            collect(bridge->mOctree, sScratch);
            ++sCount->checked;
            const bool same = sScratch.size() == entry.count &&
                              std::equal(sScratch.begin(), sScratch.end(), sNodes.begin() + entry.begin);
            if (!same)
            {
                ++sCount->mismatched;
                LL_WARNS_ONCE("RHIShadow") << "sun shadow bridges (research/178 C1b): kept nodes differ from a walk ("
                                           << entry.count << " kept, " << sScratch.size() << " walked)" << LL_ENDL;
                take(entry, bridge, root);  // the walk's nodes, as the official cull would use
                count = entry.count;
                return entry.begin;
            }
        }
        ++sCount->handed;
        sCount->nodes_handed += entry.count;
        count = entry.count;
        return entry.begin;
    }

    // LLSpatialBridge::setVisible(camera, NULL, false) for a sun shadow camera,
    // in its order, with the octree walk handed over.
    void setBridgeVisible(LLSpatialBridge* bridge, LLCamera& camera)
    {
        if (!gPipeline.hasRenderType(bridge->mDrawableType))
        {
            return;
        }

        //HACK don't draw attachments for avatars that haven't been visible in more than a frame
        LLViewerObject* vobj = bridge->mDrawable->getVObj();
        if (vobj && vobj->isAttachment() && !vobj->isHUDAttachment())
        {
            LLDrawable* av;
            LLDrawable* parent = bridge->mDrawable->getParent();

            if (parent)
            {
                LLViewerObject* objparent = parent->getVObj();
                av = objparent->mDrawable;
                LLSpatialGroup* group = av->getSpatialGroup();

                bool impostor = false;
                bool loaded = false;
                if (objparent->isAvatar())
                {
                    LLVOAvatar* avatarp = (LLVOAvatar*)objparent;
                    if (avatarp->isVisible())
                    {
                        impostor = objparent->isAvatar() && !LLPipeline::sImpostorRender && ((LLVOAvatar*)objparent)->isImpostor();
                        loaded = objparent->isAvatar() && ((LLVOAvatar*)objparent)->isFullyLoaded();
                    }
                    else
                    {
                        return;
                    }
                }

                if (!group ||
                    LLDrawable::getCurrentFrame() - av->getVisible() > 1 ||
                    impostor ||
                    !loaded)
                {
                    return;
                }
            }
        }

        LLSpatialGroup* root = (LLSpatialGroup*)bridge->mOctree->getListener(0);
        root->rebound();

        LLVector4a center;
        const LLVector4a* exts = bridge->getSpatialExtents();
        center.setAdd(exts[0], exts[1]);
        center.mul(0.5f);
        LLVector4a size;
        size.setSub(exts[1], exts[0]);
        size.mul(0.5f);

        if ((LLPipeline::sShadowRender && camera.AABBInFrustum(center, size)) ||
            LLPipeline::sImpostorRender ||
            (camera.AABBInFrustumNoFarClip(center, size) &&
             AABBSphereIntersect(exts[0], exts[1], camera.getOrigin(), camera.mFrustumCornerDist)))
        {
            // The pixel area test that follows there is for neither shadow nor
            // impostor renders: sShadowRender holds here (markVisibleSlow).
            bridge->LLDrawable::setVisible(camera);
            ++sCount->in_frustum;

            U32 count = 0;
            const U32 begin = nodesOf(bridge, root, count);
            for (U32 i = begin; i < begin + count; ++i)
            {
                LLSpatialGroup* group = sNodes[i];
                group->setVisible();    // LLOctreeMarkNotCulled::traverse
                gPipeline.markNotCulled(group, camera);    // visit; the moved camera is not read (see the top)
            }
        }
    }

    void logStats()
    {
        Stats& st = sStats;
        const F32 now = gFrameTimeSeconds;
        if (now - st.logged_at < LOG_SECONDS)
        {
            return;
        }
        // Written only with LLRHI::sPeriodicLogs (development starts); counts start over every 5 s.
        if ((st.sets || st.sets_off) && LLRHI::sPeriodicLogs)
        {
            const F64 per_on = st.sets ? 1.0 / st.sets : 0.0;
            const F64 per_off = st.sets_off ? 1.0 / st.sets_off : 0.0;
            std::ostringstream line;
            line.setf(std::ios::fixed);
            line.precision(2);
            line << "sun shadow bridges (LLRHI_SHADOW_BRIDGE_ONCE, research/178 C1b) " << (switchOn() ? "on" : "off")
                 << " over " << st.sets << " frames (off " << st.sets_off << "): drawable group sort ms per frame on "
                 << st.sort_seconds_on * 1000.0 * per_on << ", off " << st.sort_seconds_off * 1000.0 * per_off
                 << "; per frame (on): bridge culls " << st.bridges * per_on << ", in the cascade " << st.in_frustum * per_on
                 << ", octree walks " << st.walks * per_on << ", handed over " << st.handed * per_on << " (nodes "
                 << st.nodes_handed * per_on << "), dirty roots walked " << st.not_kept * per_on;
            if (verifyOn())
            {
                line << "; verify: checked " << st.checked << ", mismatched " << st.mismatched;
            }
            LL_INFOS("RHIShadow") << line.str() << LL_ENDL;
        }
        st = Stats();
        st.logged_at = now;
    }
}

CascadesScope::CascadesScope(bool main_view)
{
    sMainView = main_view;
    sCount = main_view ? &sStats : &sOtherViews;
    if (main_view)
    {
        pollCue();  // between cascade sets: one set is all on or all off
    }
    detail::sActive = switchOn();
    if (!detail::sActive)
    {
        if (main_view)
        {
            ++sStats.sets_off;
        }
        return;
    }
    if (++sGen == 0)
    {
        sTable.forget();
        sGen = 1;
    }
    sTable.beginSet();
    sNodes.clear();
    if (main_view)
    {
        ++sStats.sets;
    }
}

CascadesScope::~CascadesScope()
{
    detail::sActive = false;
    sNodes.clear();     // nothing is followed after the set
    if (sMainView)
    {
        logStats();
    }
    sMainView = false;
    sCount = &sStats;
    sOtherViews = Stats();
}

SortTimer::SortTimer(bool main_view)
    : mOn(main_view)
{
    if (mOn)
    {
        mStart = std::chrono::steady_clock::now();
    }
}

SortTimer::~SortTimer()
{
    if (mOn)
    {
        const F64 seconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - mStart).count();
        (detail::sActive ? sStats.sort_seconds_on : sStats.sort_seconds_off) += seconds;
    }
}

bool markVisibleSlow(LLDrawable* drawablep, LLCamera& camera, LLCullResult& cull)
{
    if (!drawablep || drawablep->isDead() || !drawablep->isSpatialBridge())
    {
        return false;
    }
    const S32 id = LLViewerCamera::sCurCameraID;
    if (!LLPipeline::sShadowRender || id < LLViewerCamera::CAMERA_SUN_SHADOW0 || id > LLViewerCamera::CAMERA_SUN_SHADOW3)
    {
        return false;   // the hand over is proved for the sun shadow cameras only (see the top)
    }
    LLSpatialBridge* bridge = (LLSpatialBridge*)drawablep;
    ++sCount->bridges;

    // LLPipeline::markVisible, its spatial bridge half, as it is there.
    const LLDrawable* root = bridge->mDrawable;
    llassert(root);
    if (root && root->getVObj()->isAttachment())
    {
        LLDrawable* rootparent = root->getParent();
        if (rootparent)
        {
            LLViewerObject* vobj = rootparent->getVObj();
            llassert(vobj);
            if (vobj)
            {
                LLVOAvatar* av = vobj->asAvatar();
                if (av &&
                    ((!LLPipeline::sImpostorRender && av->isImpostor())
                     || av->isInMuteList()
                     || (LLVOAvatar::AOA_JELLYDOLL == av->getOverallAppearance() && !av->needsImpostorUpdate())))
                {
                    return true;
                }
            }
        }
    }
    cull.pushBridge(bridge);
    setBridgeVisible(bridge, camera);   // drawablep->setVisible(camera)
    return true;
}
}
