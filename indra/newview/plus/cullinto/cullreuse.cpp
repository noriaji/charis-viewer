/**
 * @file cullreuse.cpp
 * @brief The main camera's octree walks replayed from their last trace while
 *        the camera and the octree stay the same (cull-reuse, research/188 in
 *        the sl-viewer repository).
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

// When a partition's walk is replayed (research/188 section 2):
//  - this main cull's camera inputs (the agent planes with their count and
//    masks, the origin, the corner distance, the region shifts) are bit for
//    bit those of the main cull before, so the camera stands still: the
//    frustum tests give what they gave then;
//  - the partition at this position of the loop is the same (pointer and its
//    root's serial, never the same for two groups), its root is clean after
//    the rebound, with the unbound count it had at the main cull before, and
//    the culler is the same (far clip): nothing in its octree changed;
//  - its trace was taken at that main cull or replayed then.
// A partition that fails any of them is walked as before (cull()), and when
// only the camera or the octree changed since the cull before it is not
// traced either: a moving camera or a changing octree costs the comparison
// only. The first cull of a still camera traces, the next ones replay.
// The groups the walk reaches still get earlyFail (the occlusion check) and
// markNotCulled every frame; the object cache culls and the sky are not
// touched.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "cullinto.h"
#include "cullrecord.h"
#include "cullreuse.h"
#include "cullverify.h"

#include "llcamera.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "llvieweroctree.h"
#include "pipeline.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace plus::cullreuse
{
namespace
{
    constexpr U32 MAX_PLANES = LLCamera::AGENT_PLANE_USER_CLIP_NUM;

    // What the frustum tests read of the camera, bit for bit.
    struct Inputs
    {
        U32 shifts = 0;
        U32 plane_count = 0;
        U8 plane_mask[MAX_PLANES] = {};
        F32 planes[MAX_PLANES][4] = {};
        F32 origin[3] = {};
        F32 corner = 0.f;
        bool far_clip = false;

        void take(LLCamera& camera, U32 shift_count)
        {
            shifts = shift_count;
            plane_count = camera.getAgentPlaneCount();
            memset(plane_mask, 0, sizeof(plane_mask));
            memset(planes, 0, sizeof(planes));
            for (U32 i = 0; i < MAX_PLANES && i < plane_count; ++i)
            {
                plane_mask[i] = camera.getAgentPlaneMask(i);
                const LLPlane& plane = camera.getAgentPlane(i);
                for (S32 j = 0; j < 4; ++j)
                {
                    planes[i][j] = plane[j];
                }
            }
            const LLVector3& at = camera.getOrigin();
            origin[0] = at.mV[0];
            origin[1] = at.mV[1];
            origin[2] = at.mV[2];
            corner = camera.mFrustumCornerDist;
            far_clip = LLPipeline::sUseFarClip;
        }

        bool operator==(const Inputs& rhs) const
        {
            // memcmp: bits, not values (-0 and +0, NaN), as the tests would see them.
            return shifts == rhs.shifts && plane_count == rhs.plane_count && far_clip == rhs.far_clip &&
                   !memcmp(plane_mask, rhs.plane_mask, sizeof(plane_mask)) && !memcmp(planes, rhs.planes, sizeof(planes)) &&
                   !memcmp(origin, rhs.origin, sizeof(origin)) && !memcmp(&corner, &rhs.corner, sizeof(corner));
        }
    };

    // A partition as a main cull saw it, after its root's rebound.
    // ---- research/188 cull reuse key (tools/selftest/sim/gen_cullreuse.py pulls this out; keep it self-contained)
    struct Key
    {
        const LLSpatialPartition* part = nullptr;  // compared, never followed
        U64 root_serial = 0;
        U32 root_unbounds = 0;
        U32 group_deaths = 0;   // the partition's mRHIGroupDeaths: any group freed, the trace is not replayed
        bool infinite_far_clip = false;

        bool operator==(const Key& rhs) const
        {
            return part == rhs.part && root_serial == rhs.root_serial && root_unbounds == rhs.root_unbounds &&
                   group_deaths == rhs.group_deaths && infinite_far_clip == rhs.infinite_far_clip;
        }
    };
    // ---- end of the research/188 cull reuse key ----

    // A position of the partition loop (region order, partition index).
    struct Slot
    {
        Key seen;               // at the last main cull
        bool seen_valid = false;
        bool traced = false;    // trace holds that cull's walk
        Trace trace;

        void forget()
        {
            seen_valid = false;
            traced = false;
        }
    };

    struct Counts
    {
        U64 culls = 0;          // main culls with the switch on
        U64 still = 0;          // of them with the camera standing still
        U64 walked = 0;         // partitions walked as before
        U64 traced = 0;         // partitions walked with a trace
        U64 replayed = 0;       // partitions replayed
        U64 nodes_replayed = 0; // nodes in the replays' traces
        U64 verified = 0;       // partitions verified
        U64 replays_verified = 0;
        U64 mismatches = 0;     // partitions whose records differ
        U64 states = 0;         // groups left otherwise
        U64 answer_misses = 0;
    };

    std::vector<Slot> sSlots;
    Trace sNext;                // the walk's new trace, swapped into its slot
    U32 sAt = 0;                // this cull's position in the loop
    bool sOn = false;           // this cull is the main camera's, with the switch on
    bool sStill = false;        // its camera inputs are the last main cull's
    bool sHaveLast = false;
    Inputs sLast;
    Inputs sNow;
    U32 sShifts = 0;

    // The verify's records and marks (kept from frame to frame).
    plus::cullinto::Record sReplayRecord;
    plus::cullinto::Record sOfficialRecord;
    std::vector<plus::cullinto::Answer> sAnswers;
    std::vector<plus::cullinto::GroupMarks> sBefore;
    std::vector<plus::cullinto::GroupMarks> sAfterReplay;
    std::vector<plus::cullinto::GroupMarks> sAfterOfficial;

    Counts sCounts;             // this log period
    Counts sTotals;             // since the switches last changed (the auto mode)
    U32 sAutoFrames = 0;
    U64 sAutoReplaysFrom = 0;
    bool sSeen = false;
    bool sSeenVerify = false;
    F32 sLoggedAt = 0.f;
    U32 sMismatchesLogged = 0;

    constexpr U32 AUTO_FRAMES = 600;        // main culls of the world compared before on
    constexpr U64 AUTO_REPLAYS = 2000;      // and replayed partitions compared
    constexpr U32 MISMATCHES_LOGGED = 8;
    constexpr F32 LOG_PERIOD = 5.f;
    constexpr F32 LOG_PERIOD_QUIET = 60.f;  // without LLRHI::sPeriodicLogs

    const char* mode_name()
    {
        if (!LLRHI::sCullReuseVerify)
        {
            return "on";
        }
        return LLRHI::sCullReuseAuto ? "auto verify" : "verify";
    }

    void forget_all()
    {
        sSlots.clear();
        sSlots.shrink_to_fit();
        sNext.nodes.clear();
        sNext.nodes.shrink_to_fit();
        sHaveLast = false;
    }

    // The switches' state at each main cull: a change starts the counts over.
    // The auto mode clears its own flag when it changes a switch, so a change
    // seen with the flag still set came from a cue: the auto mode is over.
    void follow_switches()
    {
        const bool verify = LLRHI::sCullReuseVerify;
        if (sSeen && verify == sSeenVerify)
        {
            return;
        }
        if (sSeen)
        {
            LLRHI::sCullReuseAuto = false;
        }
        sTotals = Counts();
        sAutoFrames = 0;
        sAutoReplaysFrom = 0;
        sSeen = true;
        sSeenVerify = verify;
        LL_INFOS("RHIProfile") << "cull reuse " << mode_name() << " (research/188)" << LL_ENDL;
    }

    Slot& slot_at(U32 at)
    {
        if (at >= sSlots.size())
        {
            sSlots.resize(at + 1);
        }
        return sSlots[at];
    }

    void note_mismatch(const LLSpatialPartition* part, const std::string& what)
    {
        if (sMismatchesLogged < MISMATCHES_LOGGED)
        {
            ++sMismatchesLogged;
            LL_WARNS("RHIProfile") << "cull reuse verify: partition " << part->mPartitionType << ": " << what << LL_ENDL;
        }
    }

    // The verify of one partition: its walk from the trace (or traced) recorded,
    // the groups put back as before it, the walk as before (cull()) recorded with
    // the same occlusion answers, both compared. The walk as before is what is
    // drawn. False when they differ.
    bool verify_walk(LLSpatialPartition* part, LLCamera& camera, const Trace* prev)
    {
        using namespace plus::cullinto;
        sBefore.clear();
        takeMarks(part, sBefore);

        sReplayRecord.clear();
        sReplayRecord.answers_mode = Record::ANSWERS_LOG;
        {
            RecordScope scope(&sReplayRecord);
            part->cullTraced(camera, prev, sNext);
        }
        sAfterReplay.clear();
        takeMarks(part, sAfterReplay);
        putMarks(sBefore);

        sAnswers = sReplayRecord.answers;
        std::sort(sAnswers.begin(), sAnswers.end());
        sOfficialRecord.clear();
        sOfficialRecord.answers_mode = Record::ANSWERS_REPLAY;
        sOfficialRecord.replay = &sAnswers;
        {
            RecordScope scope(&sOfficialRecord);
            part->cull(camera);
        }
        sAfterOfficial.clear();
        takeMarks(part, sAfterOfficial);
        applyRecord(sOfficialRecord, *LLPipeline::cullIntoResult());

        bool same = true;
        const std::vector<Op>& a = sReplayRecord.ops;
        const std::vector<Op>& b = sOfficialRecord.ops;
        if (a != b)
        {
            same = false;
            size_t at = 0;
            while (at < a.size() && at < b.size() && a[at] == b[at])
            {
                ++at;
            }
            note_mismatch(part, llformat("the records differ from op %u (replay %u ops, walk %u)", (U32)at, (U32)a.size(), (U32)b.size()));
        }
        if (sReplayRecord.occluded != sOfficialRecord.occluded || sReplayRecord.unoccluded != sOfficialRecord.unoccluded ||
            sReplayRecord.visible_nodes != sOfficialRecord.visible_nodes)
        {
            same = false;
            note_mismatch(part, llformat("counts (occluded, unoccluded, visible) replay %u %u %u, walk %u %u %u",
                                         sReplayRecord.occluded, sReplayRecord.unoccluded, sReplayRecord.visible_nodes,
                                         sOfficialRecord.occluded, sOfficialRecord.unoccluded, sOfficialRecord.visible_nodes));
        }
        U64 states = 0;
        if (sAfterReplay.size() != sAfterOfficial.size())
        {
            states = llmax(sAfterReplay.size(), sAfterOfficial.size());
        }
        else
        {
            for (size_t i = 0; i < sAfterReplay.size(); ++i)
            {
                states += sameMarks(sAfterReplay[i], sAfterOfficial[i]) ? 0 : 1;
            }
        }
        if (states)
        {
            note_mismatch(part, llformat("%u groups left otherwise", (U32)states));
        }
        const U64 misses = sOfficialRecord.answer_misses;
        for (Counts* c : { &sCounts, &sTotals })
        {
            ++c->verified;
            c->replays_verified += prev ? 1 : 0;
            c->mismatches += same ? 0 : 1;
            c->states += states;
            c->answer_misses += misses;
        }
        sReplayRecord.clear();
        sOfficialRecord.clear();
        return same && !states && !misses;
    }

    void decide_auto()
    {
        if (!LLRHI::sCullReuseAuto)
        {
            return;
        }
        if (!LLRHI::sCullReuseVerify)
        {
            LLRHI::sCullReuseAuto = false;  // a cue changed it: the auto mode is over
            return;
        }
        if (!LLRHI::sWorldShown)
        {
            // The login screen: verified, not counted toward on.
            sAutoFrames = 0;
            sAutoReplaysFrom = sTotals.replays_verified;
        }
        else
        {
            ++sAutoFrames;
        }
        if (sTotals.mismatches || sTotals.states || sTotals.answer_misses)
        {
            LLRHI::sCullReuse = false;
            LLRHI::sCullReuseVerify = false;    // a cue turning it on again: on, not the verify of the auto mode
            LLRHI::sCullReuseAuto = false;
            LL_WARNS("RHIProfile") << "cull reuse (auto): verify found " << sTotals.mismatches << " partitions with other records, "
                                   << sTotals.states << " groups left otherwise, " << sTotals.answer_misses << " answers missed over "
                                   << sTotals.verified << " partitions: off for this run (research/188)" << LL_ENDL;
            forget_all();
            return;
        }
        const U64 replays = sTotals.replays_verified - sAutoReplaysFrom;
        if (sAutoFrames >= AUTO_FRAMES && replays >= AUTO_REPLAYS)
        {
            // From the next cull on: the replays only.
            LLRHI::sCullReuseVerify = false;
            LLRHI::sCullReuseAuto = false;
            LL_INFOS("RHIProfile") << "cull reuse (auto): " << replays << " replayed partitions compared over " << sAutoFrames
                                   << " main culls of the world, no mismatch: on (research/188)" << LL_ENDL;
        }
    }

    // One VKPerf line per period: how much of the main camera's walk was replayed.
    void log_counts()
    {
        const F32 period = LLRHI::sPeriodicLogs ? LOG_PERIOD : LOG_PERIOD_QUIET;
        if (gFrameTimeSeconds - sLoggedAt < period)
        {
            return;
        }
        sLoggedAt = gFrameTimeSeconds;
        const Counts c = sCounts;
        sCounts = Counts();
        if (!c.culls)
        {
            return;
        }
        const F64 per = 1.0 / (F64)c.culls;
        const U64 parts = c.walked + c.traced + c.replayed;
        const F64 rate = parts ? 100.0 * (F64)c.replayed / (F64)parts : 0.0;
        std::string line = llformat("cull reuse [%s] (research/188): %llu main culls, still %llu; per cull partitions %.1f: replayed %.1f "
                                    "(%.0f%%), traced %.1f, walked %.1f; nodes replayed %.0f",
                                    mode_name(), (unsigned long long)c.culls, (unsigned long long)c.still, parts * per,
                                    c.replayed * per, rate, c.traced * per, c.walked * per, c.nodes_replayed * per);
        if (c.verified)
        {
            line += llformat("; verify: partitions %llu (replayed %llu), mismatches %llu, states %llu, answer misses %llu",
                             (unsigned long long)c.verified, (unsigned long long)c.replays_verified,
                             (unsigned long long)c.mismatches, (unsigned long long)c.states,
                             (unsigned long long)c.answer_misses);
        }
        LL_INFOS("VKPerf") << line << LL_ENDL;
    }
}

void beginCull(LLCamera& camera, bool walks)
{
    sOn = false;
    if (!walks || !plus::cullinto::isMainCull(camera))
    {
        return;     // another camera's cull: the main camera's traces stay
    }
    if (!LLRHI::sCullReuse)
    {
        if (sSeen)
        {
            sSeen = false;
            if (LLRHI::sCullReuseAuto)
            {
                LLRHI::sCullReuseVerify = false;    // the auto mode's verify: a cue turning it on again is on
            }
            LLRHI::sCullReuseAuto = false;
            LL_INFOS("RHIProfile") << "cull reuse off (research/188)" << LL_ENDL;
        }
        if (!sSlots.empty())
        {
            forget_all();
        }
        return;
    }
    follow_switches();
    sOn = true;
    sAt = 0;
    sNow.take(camera, sShifts);
    sStill = sHaveLast && sNow == sLast;
    sLast = sNow;
    sHaveLast = true;
    ++sCounts.culls;
    sCounts.still += sStill ? 1 : 0;
}

void cull(LLSpatialPartition* part, LLCamera& camera)
{
    if (!sOn)
    {
        part->cull(camera);
        return;
    }
    Slot& slot = slot_at(sAt++);
    LLViewerOctreeGroup* root = (LLViewerOctreeGroup*)part->mOctree->getListener(0);
    if (!root)
    {
        slot.forget();
        part->cull(camera);
        ++sCounts.walked;
        return;
    }
    root->rebound();    // as cull() does first
    Key key;
    key.part = part;
    key.root_serial = root->getRHISerial();
    key.root_unbounds = root->getUnboundCount();
    key.group_deaths = part->mRHIGroupDeaths;
    key.infinite_far_clip = part->mInfiniteFarClip;
    const bool same = sStill && !root->isDirty() && slot.seen_valid && slot.seen == key;
    slot.seen = key;
    slot.seen_valid = true;
    if (!same)
    {
        slot.traced = false;
        part->cull(camera);
        ++sCounts.walked;
        return;
    }

    const Trace* prev = slot.traced ? &slot.trace : nullptr;
    // The verify compares the traced walks too: what is drawn while it runs is the walk as before.
    const bool verify = LLRHI::sCullReuseVerify;
    bool kept = true;
    if (verify)
    {
        kept = verify_walk(part, camera, prev);
    }
    else
    {
        part->cullTraced(camera, prev, sNext);
    }
    if (prev)
    {
        ++sCounts.replayed;
        sCounts.nodes_replayed += sNext.nodes.size();
    }
    else
    {
        ++sCounts.traced;
    }
    std::swap(slot.trace.nodes, sNext.nodes);
    slot.traced = kept;     // a walk that differed is not replayed
}

void skip()
{
    if (sOn)
    {
        slot_at(sAt++).forget();
    }
}

void endCull()
{
    if (!sOn)
    {
        return;
    }
    sOn = false;
    if (sSlots.size() > sAt)
    {
        sSlots.resize(sAt);     // regions gone: their traces freed
    }
    if (LLRHI::sCullReuseVerify)
    {
        decide_auto();
    }
    log_counts();
}

void noteShift()
{
    ++sShifts;
}
}
