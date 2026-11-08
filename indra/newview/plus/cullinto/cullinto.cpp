/**
 * @file cullinto.cpp
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "cullinto.h"
#include "cullrecord.h"
#include "cullverify.h"

#include "llappviewer.h"
#include "llcamera.h"
#include "llrhi.h"
#include "llrhiocclusion.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvieweroctree.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

extern bool gCubeSnapshot;
void rhi_run_cull_jobs(U32 count, const std::function<void(U32)>& job);   // pipeline.cpp
U32 rhi_cull_threads();                                                     // pipeline.cpp

namespace plus::cullinto
{
    thread_local Record* tRecord = nullptr;

namespace
{
    constexpr U32 QUERY_RESULT = 0x8866;    // GL_QUERY_RESULT

// ---- research/184 cull into rule (tools/selftest/sim/gen_cullinto.py extracts this block) ----
    // How the walk below a partition's root is cut into jobs. LLViewerOctreeCull::
    // traverse walks the root's children in order, the first reached with the
    // root's frustum result res (1 partly in, 2 all in), each next one with what
    // the walk of the one before left in mRes. Each job starts with res.
    //  - res 2: what every walk leaves is 2 again (nothing below resets it), so
    //    every child may start a job.
    //  - res 1: what a walk leaves is 1 or 0 and depends on the walk (an
    //    occluded child returns at once and leaves what it was reached with, any
    //    other resets it). A child without SKIP_FRUSTUM_CHECK walks the same
    //    either way (it makes its own frustum check), one with the flag does not.
    //    So a child with the flag must be reached as in the serial walk: the
    //    children up to the last one with it (after the first) go in one job from
    //    the first child, which carries mRes along exactly; every child after it
    //    starts a job of its own. (Rebound gives the flag only to an only child:
    //    with several children, none has it and each starts a job.)
    struct CullSplit
    {
        // skip[i]: child i has SKIP_FRUSTUM_CHECK. Writes the first child of each
        // job in order (a job walks up to the next one's first, the last one up
        // to child_count) and returns the number of jobs (0 when res is 0).
        static U32 plan(S32 res, U32 child_count, const bool* skip, U32* job_first)
        {
            if (res == 0 || child_count == 0)
            {
                return 0;
            }
            U32 from = 1;   // the first child that may start a job after child 0
            if (res != 2)
            {
                for (U32 i = 1; i < child_count; ++i)
                {
                    if (skip[i])
                    {
                        from = i + 1;
                    }
                }
            }
            U32 jobs = 0;
            job_first[jobs++] = 0;
            for (U32 i = from; i < child_count; ++i)
            {
                job_first[jobs++] = i;
            }
            return jobs;
        }
    };
// ---- end of the research/184 cull into rule ----

    struct Job
    {
        LLSpatialPartition* part;
        S32 res;
        U32 first;
        U32 end;
        Record* record;
    };

    struct Part
    {
        LLSpatialPartition* part;
        Record* root;           // the root's part (main thread)
        Record* serial;         // verify: the serial walk
        U32 first_job;
        U32 job_count;
    };

    struct Region
    {
        LLViewerRegion* region;
        U32 first_part;
        U32 part_count;
    };

    struct Counts
    {
        U64 frames = 0;
        U64 parts = 0;
        U64 jobs = 0;
        U64 ops = 0;
        U64 verify_frames = 0;
        U64 compared = 0;       // ops compared
        U64 mismatches = 0;     // parts whose ops differ
        U64 states = 0;         // groups left otherwise
        U64 answer_misses = 0;
        U64 failures = 0;       // jobs that threw
    };

    // Kept from frame to frame (no allocation once they have grown).
    std::vector<std::unique_ptr<Record>> sRecords;
    U32 sRecordsUsed = 0;
    std::vector<Job> sJobs;
    std::vector<Part> sParts;
    std::vector<Region> sRegions;
    std::vector<Answer> sAnswers;
    std::vector<GroupMarks> sBefore;
    std::vector<GroupMarks> sAfterSplit;
    std::vector<GroupMarks> sAfterSerial;

    Counts sCounts;     // this log period
    Counts sTotals;     // since the switches last changed (the auto mode)
    U32 sAutoFrames = 0;
    U64 sAutoComparedFrom = 0;
    bool sSeen = false;
    bool sSeenVerify = false;
    F32 sLoggedAt = 0.f;
    U32 sMismatchesLogged = 0;

    constexpr U32 AUTO_FRAMES = 600;        // frames of the world compared before on
    constexpr U64 AUTO_COMPARED = 100000;   // and ops compared
    constexpr U32 MISMATCHES_LOGGED = 8;
    constexpr F32 LOG_PERIOD = 5.f;

    Record* take_record()
    {
        if (sRecordsUsed == sRecords.size())
        {
            sRecords.push_back(std::make_unique<Record>());
        }
        Record* record = sRecords[sRecordsUsed++].get();
        record->clear();
        return record;
    }

    // The world camera's frame cull with what a cull thread may take.
    bool wanted(LLCamera& camera)
    {
        if (!LLRHI::sCullThreads || !rhi_cull_threads())
        {
            return false;
        }
        if (!isMainCull(camera))
        {
            return false;
        }
        // The zero coverage counters are written by every test (one view for all walks).
        if (LLRHI::sTinyStats)
        {
            return false;
        }
        // A cull thread reads occlusion answers only through the native table (its own
        // lock); the GL path belongs to the main thread.
        if (LLPipeline::sUseOcclusion > 1 && (LLRHI::Occlusion::sMode == 0 || !LLRHI::sOcclusionResult))
        {
            return false;
        }
        return true;
    }

    void take_marks(std::vector<GroupMarks>& out)
    {
        out.clear();
        for (const Part& p : sParts)
        {
            takeMarks(p.part, out);
        }
    }

    const char* op_name(U32 kind)
    {
        switch (kind)
        {
        case Op::VISIBLE_GROUP:  return "visible";
        case Op::DRAWABLE_GROUP: return "drawable";
        case Op::MARK_OCCLUDER:  return "occluder";
        case Op::MARK_REBUILD:   return "rebuild";
        case Op::RELEASE_QUERY:  return "release";
        default:                 return "?";
        }
    }

    // The split walk's ops of part p, in the serial walk's order, against its
    // serial walk's (verify). True when they are the same.
    bool compare_part(const Part& p, U64& compared)
    {
        const Record& serial = *p.serial;
        bool same = true;
        size_t at = 0;
        U32 occluded = p.root->occluded, unoccluded = p.root->unoccluded, visible_nodes = p.root->visible_nodes;
        auto compare_ops = [&](const Record& record)
        {
            for (const Op& op : record.ops)
            {
                if (same && (at >= serial.ops.size() || !(serial.ops[at] == op)))
                {
                    same = false;
                    if (sMismatchesLogged < MISMATCHES_LOGGED)
                    {
                        ++sMismatchesLogged;
                        const Op* other = at < serial.ops.size() ? &serial.ops[at] : nullptr;
                        LL_WARNS("RHIProfile") << "cull par verify: partition " << p.part->mPartitionType << " op " << at << ": split "
                                               << op_name(op.kind) << " " << (void*)op.group << " " << op.name << ", serial "
                                               << (other ? op_name(other->kind) : "(none)") << " " << (other ? (void*)other->group : nullptr)
                                               << " " << (other ? other->name : 0) << " (" << serial.ops.size() << " ops)" << LL_ENDL;
                    }
                }
                ++at;
            }
        };
        compare_ops(*p.root);
        for (U32 j = p.first_job; j < p.first_job + p.job_count; ++j)
        {
            const Record& record = *sJobs[j].record;
            compare_ops(record);
            occluded += record.occluded;
            unoccluded += record.unoccluded;
            visible_nodes += record.visible_nodes;
        }
        compared += at;
        if (same && at != serial.ops.size())
        {
            same = false;
            if (sMismatchesLogged < MISMATCHES_LOGGED)
            {
                ++sMismatchesLogged;
                LL_WARNS("RHIProfile") << "cull par verify: partition " << p.part->mPartitionType << ": split " << at << " ops, serial "
                                       << serial.ops.size() << LL_ENDL;
            }
        }
        if (same && (occluded != serial.occluded || unoccluded != serial.unoccluded || visible_nodes != serial.visible_nodes))
        {
            same = false;
            if (sMismatchesLogged < MISMATCHES_LOGGED)
            {
                ++sMismatchesLogged;
                LL_WARNS("RHIProfile") << "cull par verify: partition " << p.part->mPartitionType << ": counts (occluded, unoccluded, visible) split "
                                       << occluded << " " << unoccluded << " " << visible_nodes << ", serial " << serial.occluded << " "
                                       << serial.unoccluded << " " << serial.visible_nodes << LL_ENDL;
            }
        }
        return same;
    }

    // The switches' state at each call: a change starts the counts over. The auto
    // mode clears its own flag when it changes a switch, so a change seen with the
    // flag still set came from a cue: the auto mode is over.
    void follow_switches(bool verify)
    {
        if (!sSeen || verify != sSeenVerify)
        {
            if (sSeen)
            {
                LLRHI::sCullParAuto = false;
            }
            sTotals = Counts();
            sAutoFrames = 0;
            sAutoComparedFrom = 0;
            sSeen = true;
            sSeenVerify = verify;
            LL_INFOS("RHIProfile") << "cull par " << (verify ? (LLRHI::sCullParAuto ? "auto (verify first)" : "verify") : "on")
                                   << ", cull threads " << rhi_cull_threads() << " (research/184 A1)" << LL_ENDL;
        }
    }

    void decide_auto()
    {
        if (!LLRHI::sCullParAuto)
        {
            return;
        }
        if (!LLRHI::sCullParVerify)
        {
            LLRHI::sCullParAuto = false;    // a cue changed it: the auto mode is over
            return;
        }
        if (!LLRHI::sWorldShown)
        {
            // The login screen: verified, not counted toward on.
            sAutoFrames = 0;
            sAutoComparedFrom = sTotals.compared;
        }
        else
        {
            ++sAutoFrames;
        }
        if (sTotals.mismatches || sTotals.states || sTotals.answer_misses || sTotals.failures)
        {
            LLRHI::sCullPar = false;
            LLRHI::sCullParVerify = false;  // a cue turning it on again: on, not the verify of the auto mode
            LLRHI::sCullParAuto = false;
            LL_WARNS("RHIProfile") << "cull par (auto): verify found " << sTotals.mismatches << " partitions with other ops, "
                                   << sTotals.states << " groups left otherwise, " << sTotals.answer_misses << " answers missed, "
                                   << sTotals.failures << " failed jobs over " << sTotals.verify_frames
                                   << " frames: off for this run (research/184)" << LL_ENDL;
            return;
        }
        const U64 world_compared = sTotals.compared - sAutoComparedFrom;
        if (sAutoFrames >= AUTO_FRAMES && world_compared >= AUTO_COMPARED)
        {
            // From the next frame on: the split walk only.
            LLRHI::sCullParVerify = false;
            LLRHI::sCullParAuto = false;
            LL_INFOS("RHIProfile") << "cull par (auto): " << world_compared << " ops compared over " << sAutoFrames
                                   << " frames of the world, no mismatch: on (research/184)" << LL_ENDL;
        }
    }

    void log_counts()
    {
        if (gFrameTimeSeconds - sLoggedAt < LOG_PERIOD)
        {
            return;
        }
        sLoggedAt = gFrameTimeSeconds;
        const Counts c = sCounts;
        sCounts = Counts();
        if (!LLRHI::sPeriodicLogs || !c.frames)
        {
            return;
        }
        const F64 f = (F64)c.frames;
        std::string line = llformat("cull par [%s] over %llu frames, per frame: partitions %.0f jobs %.1f ops %.0f",
                                    !sSeenVerify ? "on" : LLRHI::sCullParAuto ? "auto verify" : "verify",
                                    (unsigned long long)c.frames, c.parts / f, c.jobs / f, c.ops / f);
        if (c.verify_frames)
        {
            line += llformat(", verify frames %llu compared %llu mismatches %llu states %llu answer misses %llu",
                             (unsigned long long)c.verify_frames, (unsigned long long)c.compared, (unsigned long long)c.mismatches,
                             (unsigned long long)c.states, (unsigned long long)c.answer_misses);
        }
        if (c.failures)
        {
            line += llformat(", failed jobs %llu", (unsigned long long)c.failures);
        }
        LL_INFOS("RHIProfile") << line << LL_ENDL;
    }

    // The main thread's part before the fork: every root rebound (verify: then
    // the groups' marks taken), every root's part walked, the jobs planned.
    void plan(LLCamera& camera, bool verify, bool cost)
    {
        costMark(cost, "cull rebound");
        sJobs.clear();
        sParts.clear();
        sRegions.clear();
        sRecordsUsed = 0;
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            Region r = { region, (U32)sParts.size(), 0 };
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (part && gPipeline.hasRenderType(part->mDrawableType))
                {
                    ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();    // the whole tree, as cull() does first
                    sParts.push_back({ part, nullptr, nullptr, 0, 0 });
                    ++r.part_count;
                }
            }
            sRegions.push_back(r);
        }
        costMark(cost, "cull roots");
        if (verify)
        {
            take_marks(sBefore);
        }
        bool skip[8];
        U32 job_first[8];
        for (Part& p : sParts)
        {
            p.root = take_record();
            p.root->answers_mode = verify ? Record::ANSWERS_LOG : Record::ANSWERS_PASS;
            S32 res = 0;
            {
                RecordScope scope(p.root);
                res = p.part->cullRoot(camera);
            }
            p.first_job = (U32)sJobs.size();
            const OctreeNode* root = p.part->mOctree;
            const U32 children = root->getChildCount();
            U32 jobs = 0;
            if (children <= 8)
            {
                for (U32 i = 0; i < children; ++i)
                {
                    const LLViewerOctreeGroup* group = (const LLViewerOctreeGroup*)root->getChild(i)->getListener(0);
                    skip[i] = group && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK);
                }
                jobs = CullSplit::plan(res, children, skip, job_first);
            }
            else if (res && children)
            {
                job_first[jobs++] = 0;      // not an octree's node: all its children in one job, as the serial walk
            }
            for (U32 j = 0; j < jobs; ++j)
            {
                Record* record = take_record();
                record->answers_mode = p.root->answers_mode;
                sJobs.push_back({ p.part, res, job_first[j], j + 1 < jobs ? job_first[j + 1] : children, record });
            }
            p.job_count = jobs;
        }
    }

    void walk_jobs(LLCamera& camera)
    {
        rhi_run_cull_jobs((U32)sJobs.size(), [&camera](U32 index)
        {
            Job& job = sJobs[index];
            RecordScope scope(job.record);
            try
            {
                job.part->cullChildren(camera, job.res, job.first, job.end);
            }
            catch (...)
            {
                job.record->failed = true;      // the main thread walks it again
            }
        });
    }

    // After the join (not verify): a job that threw is walked again on the main
    // thread (what the walk did before it threw, it does the same way again; the
    // occlusion checks' wait counts may step twice; the query names it already
    // took from its groups are kept to be given back), and the switch goes off.
    void redo_failed(LLCamera& camera)
    {
        static std::vector<U32> names;
        for (Job& job : sJobs)
        {
            if (!job.record->failed)
            {
                continue;
            }
            names.clear();
            for (const Op& op : job.record->ops)
            {
                if (op.kind == Op::RELEASE_QUERY)
                {
                    names.push_back(op.name);
                }
            }
            ++sCounts.failures;
            ++sTotals.failures;
            if (LLRHI::sCullPar)
            {
                LLRHI::sCullPar = false;
                LLRHI::sCullParAuto = false;
                LL_WARNS("RHIProfile") << "cull par: a walk on a cull thread failed: walked again on the main thread, off for this run"
                                       << LL_ENDL;
            }
            const U32 mode = job.record->answers_mode;
            job.record->clear();
            job.record->answers_mode = mode;
            for (U32 name : names)
            {
                job.record->pushName(name);
            }
            RecordScope scope(job.record);
            job.part->cullChildren(camera, job.res, job.first, job.end);
        }
    }

    // The verify after the split walk (not applied): the groups put back as
    // before it, the serial walk recorded with the same answers, both compared.
    void verify_serial(LLCamera& camera)
    {
        ++sCounts.verify_frames;
        ++sTotals.verify_frames;
        U64 failures = 0;
        for (const Job& job : sJobs)
        {
            failures += job.record->failed ? 1 : 0;
        }
        take_marks(sAfterSplit);
        putMarks(sBefore);

        sAnswers.clear();
        for (U32 i = 0; i < sRecordsUsed; ++i)
        {
            const std::vector<Answer>& answers = sRecords[i]->answers;
            sAnswers.insert(sAnswers.end(), answers.begin(), answers.end());
        }
        std::sort(sAnswers.begin(), sAnswers.end());

        U64 answer_misses = 0;
        for (Part& p : sParts)
        {
            p.serial = take_record();
            p.serial->answers_mode = Record::ANSWERS_REPLAY;
            p.serial->replay = &sAnswers;
            {
                RecordScope scope(p.serial);
                p.part->cull(camera);
            }
            answer_misses += p.serial->answer_misses;
        }

        take_marks(sAfterSerial);
        U64 states = 0;
        if (sAfterSerial.size() != sAfterSplit.size())
        {
            states = llmax(sAfterSerial.size(), sAfterSplit.size());
        }
        else
        {
            for (size_t i = 0; i < sAfterSerial.size(); ++i)
            {
                if (!sameMarks(sAfterSerial[i], sAfterSplit[i]))
                {
                    if (!states && sMismatchesLogged < MISMATCHES_LOGGED)
                    {
                        ++sMismatchesLogged;
                        const GroupMarks& a = sAfterSplit[i];
                        const GroupMarks& b = sAfterSerial[i];
                        LL_WARNS("RHIProfile") << "cull par verify: group " << i << " " << (void*)a.group << " left otherwise: state "
                                               << std::hex << a.marks.group_state << "/" << b.marks.group_state << " occlusion "
                                               << a.marks.occlusion_state << "/" << b.marks.occlusion_state << std::dec << " query "
                                               << a.marks.query << "/" << b.marks.query << " checks " << a.marks.check_count << "/"
                                               << b.marks.check_count << " (split/serial)" << LL_ENDL;
                    }
                    ++states;
                }
            }
        }

        U64 compared = 0, mismatches = 0;
        for (const Part& p : sParts)
        {
            mismatches += compare_part(p, compared) ? 0 : 1;
        }
        sCounts.compared += compared;
        sTotals.compared += compared;
        sCounts.mismatches += mismatches;
        sTotals.mismatches += mismatches;
        sCounts.states += states;
        sTotals.states += states;
        sCounts.answer_misses += answer_misses;
        sTotals.answer_misses += answer_misses;
        sCounts.failures += failures;
        sTotals.failures += failures;
    }
}

bool run(LLCamera& camera)
{
    if (!LLRHI::sCullPar)
    {
        if (sSeen)
        {
            sSeen = false;
            if (LLRHI::sCullParAuto)
            {
                LLRHI::sCullParVerify = false;  // the auto mode's verify: a cue turning it on again is on
            }
            LLRHI::sCullParAuto = false;    // off by a cue or by the auto mode: over either way
            LL_INFOS("RHIProfile") << "cull par off (research/184 A1)" << LL_ENDL;
        }
        return false;
    }
    if (!wanted(camera))
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    const bool verify = LLRHI::sCullParVerify;
    follow_switches(verify);

    const bool cost = LLRHI::sFrameCostDetail;     // wanted(): the main camera's cull (costDetail)
    plan(camera, verify, cost);
    costMark(cost, "cull jobs");
    walk_jobs(camera);
    if (verify)
    {
        costMark(cost, "cull verify");
        verify_serial(camera);
    }
    else
    {
        costMark(cost, "cull redo");
        redo_failed(camera);
    }

    // The serial walk's order: per region its partitions (each its root's part,
    // then its jobs in order), then the region's object cache.
    LLCullResult& result = *LLPipeline::cullIntoResult();
    for (const Region& r : sRegions)
    {
        costMark(cost, "cull apply");
        for (U32 i = r.first_part; i < r.first_part + r.part_count; ++i)
        {
            const Part& p = sParts[i];
            if (verify)
            {
                applyRecord(*p.serial, result);
                continue;
            }
            applyRecord(*p.root, result);
            for (U32 j = p.first_job; j < p.first_job + p.job_count; ++j)
            {
                applyRecord(*sJobs[j].record, result);
            }
        }
        if (LLVOCachePartition* vo_part = r.region->getVOCachePartition())
        {
            costMark(cost, "cull vo cache");
            vo_part->cull(camera, LLPipeline::sUseOcclusion > 0);
        }
    }
    costMark(cost, "cull end");     // the counts, the auto mode, the log

    ++sCounts.frames;
    sCounts.parts += sParts.size();
    sCounts.jobs += sJobs.size();
    for (U32 i = 0; i < sRecordsUsed; ++i)
    {
        sCounts.ops += sRecords[i]->ops.size();
    }
    if (verify)
    {
        decide_auto();
    }
    log_counts();
    return true;
}

bool isMainCull(const LLCamera& camera)
{
    return &camera == LLViewerCamera::getInstance() && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD &&
           !gCubeSnapshot && !LLPipeline::sShadowRender && !LLPipeline::sReflectionRender && !LLPipeline::sImpostorRender;
}

U32 cullThreads(U32 cores)
{
    static LLCachedControl<U32> setting(gSavedSettings, "PlusCullThreads", 0);
    const U32 most = cores > 1 ? cores - 1 : 0;
    const U32 wanted = llmin((U32)setting, 4u);
    return wanted ? llmin(wanted, most) : llmin(most, 3u);
}

void queryObject(U32 name, U32 pname, U32* params)
{
    Record* record = tRecord;
    if (!record || record->answers_mode == Record::ANSWERS_PASS || !params)
    {
        LLRHI::Occlusion::getQueryObjectuiv(name, pname, params);
        return;
    }
    const Answer key = { ((U64)name << 1) | (pname == QUERY_RESULT ? 1u : 0u), 0 };
    if (record->answers_mode == Record::ANSWERS_REPLAY && record->replay)
    {
        const std::vector<Answer>& answers = *record->replay;
        std::vector<Answer>::const_iterator found = std::lower_bound(answers.begin(), answers.end(), key);
        if (found != answers.end() && found->key == key.key)
        {
            *params = found->value;
            return;
        }
        ++record->answer_misses;
        LLRHI::Occlusion::getQueryObjectuiv(name, pname, params);
        return;
    }
    LLRHI::Occlusion::getQueryObjectuiv(name, pname, params);
    record->answers.push_back({ key.key, *params });
}
} // namespace plus::cullinto
