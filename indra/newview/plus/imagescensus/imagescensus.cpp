/**
 * @file imagescensus.cpp
 * @brief What the texture list's pass (pre-gbuffer images list) takes, and why (research/348 branch 0).
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

#include "imagescensus.h"

#include "lldrawable.h"
#include "lljoint.h"
#include "llrhi.h"
#include "llvoavatar.h"

#include <chrono>
#include <sstream>

namespace plus::imagescensus
{
    bool sOn = LLRHI::envSet("LLRHI_IMAGES_CENSUS");
    bool sActive = false;
}

namespace
{
    using namespace plus::imagescensus;

    constexpr F64 REPORT_SECONDS = 10.0;

    // The avatar kinds of the rigged faces (decided once per avatar and pass, from values that are only read).
    enum AvKind : U8
    {
        AK_SELF = 0,
        AK_ANIMESH,
        AK_FULL,        // a full avatar in view
        AK_OFFSCREEN,   // a full avatar out of view
        AK_IMPOSTOR,    // drawn as an impostor (update period over 1)
        AK_COUNT
    };
    const char* const AV_NAMES[AK_COUNT] = { "self", "animesh", "full on screen", "full off screen", "impostor" };

    // The kinds of joint a face is rigged to, as bits (the same line as LLVOAvatar::getJoint draws).
    enum JointKind : U8
    {
        JK_BONE = 1,
        JK_COLLISION = 2,
        JK_ATTACHMENT = 4
    };
    constexpr U32 JK_KINDS = 3;
    const char* const JK_NAMES[JK_KINDS] = { "bone", "collision volume", "attachment" };

    // 0: as the work was done (the joint's own flag), 1: as the pass began (the flag, or a joint this pass has already
    // rebuilt since: what a scan before any rebuild of this pass would meet).
    constexpr U32 MODES = 2;
    const char* const MODE_NAMES[MODES] = { "as the area was worked out", "as the pass began" };
    constexpr U32 CLASSES = 3;  // all joints clean / only collision volumes dirty / a bone or attachment dirty
    const char* const CLASS_NAMES[CLASSES] = { "all clean", "collision volume dirty only", "bone or attachment dirty" };

    struct Window
    {
        U64 frames = 0;
        U64 stage_ticks[STAGE_COUNT] = {};
        U64 stage_self[STAGE_COUNT] = {};   // the census's own ticks inside each stage
        F64 budget_ms = 0.0;                // updateImages' limit, summed over the frames
        // the fetch pass
        U64 fetch_frames = 0;
        U64 fetch_cut = 0;
        F64 fetch_budget_ms = 0.0;
        U64 textures = 0;
        U64 copied = 0;
        U64 due = 0;
        U64 visited = 0;
        F64 plan_ms = 0.0;
        F64 loop_ms = 0.0;                  // the fetch loop's time by the pass's own timer (the census inside)
        F64 uncut_loop_ms = 0.0;            // sum over the frames of loop x due / visited
        // the urgent pass
        U64 urgent_frames = 0;
        U64 urgent_cut = 0;
        U64 urgent_entries = 0;
        U64 urgent_reached = 0;
        // the rigged walks
        U64 walks = 0;
        U64 faces[AK_COUNT] = {};
        U64 faces_urgent = 0;
        U64 no_extents = 0;
        U64 joints = 0;
        U64 no_record = 0;                  // walks of an avatar the pass had no room to record
        // the walks' ticks, apart for those that ran in the fetch loop and in the urgent pass
        struct WalkTicks
        {
            U64 walk = 0;           // from the walk's start to its end
            U64 body = 0;           // the joints' work (getJoint, world matrix, box)
            U64 joint_self = 0;     // the census's reads of the joints
            U64 info = 0;           // updateRiggingInfo
            U64 calls = 0;          // beforeJoint calls
            U64 info_pairs = 0;
            U64 walks = 0;
        };
        WalkTicks walk_ticks[2];    // [0] the fetch loop, [1] the urgent pass
        U64 dirty_faces[MODES] = {};
        U64 dirty_kind[MODES][JK_KINDS] = {};
        U64 dirty_collision_only[MODES] = {};
        U64 dirty_av[MODES][AK_COUNT] = {};
        // the textures with a rigged face worked out in their visit
        U64 tex_rigged = 0;
        U64 tex_class[MODES][CLASSES] = {};
        U64 tex_ticks[MODES][CLASSES] = {};
    };
    Window sWindow;
    U64 sWindowTicks = 0;
    S64 sWindowNanoseconds = 0;
    F64 sReadTicks = -1.0;                  // one TSC read (measured once)

    // the pass
    Stage sStage = PROLOGUE;
    U64 sStageStart = 0;

    // the texture being visited
    bool sTexOpen = false;
    U64 sTexStart = 0;
    U64 sTexSelf = 0;
    U32 sTexRigged = 0;
    U8 sTexWorst[MODES] = {};

    // The avatars whose rigged faces the pass has worked: which of their joints it found dirty and rebuilt (so a later
    // face of the same avatar, that finds them clean, still counts them dirty as the pass began).
    struct Slot
    {
        const LLVOAvatar* avatar = nullptr;
        U8 kind = AK_FULL;
        U64 rebuilt[4] = {};                // by joint number, up to 256
    };
    constexpr U32 MAX_SLOTS = 128;
    Slot sSlots[MAX_SLOTS];
    U32 sSlotCount = 0;
    U32 sLastSlot = 0;

    const char* const STAGE_MARKS[STAGE_COUNT] =
    {
        nullptr,
        "pre-gbuffer images list fastcache",
        "pre-gbuffer images list create",
        "pre-gbuffer images list urgent",
        "pre-gbuffer images list fetch copy",
        "pre-gbuffer images list fetch",
        "pre-gbuffer images list after",
    };
    const char* const STAGE_NAMES[STAGE_COUNT] = { "prologue", "fast cache", "create", "urgent", "fetch copy", "fetch loop", "after" };

    S64 now_nanoseconds()
    {
        return (S64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // What one TSC read costs (back to back), measured once.
    F64 measure_read_ticks()
    {
        constexpr U32 N = 2000;
        U64 last = __rdtsc();
        const U64 first = last;
        for (U32 i = 0; i < N; ++i)
        {
            last = __rdtsc();
        }
        return (F64)(last - first) / N;
    }

    void open_window()
    {
        sWindow = Window();
        sWindowTicks = __rdtsc();
        sWindowNanoseconds = now_nanoseconds();
    }

    U8 classify_avatar(LLVOAvatar* avatar)
    {
        if (avatar->isSelf())
        {
            return AK_SELF;
        }
        if (avatar->isControlAvatar())
        {
            return AK_ANIMESH;
        }
        if (avatar->getUpdatePeriod() > 1)
        {
            return AK_IMPOSTOR;
        }
        // The octree entry's own stamp (LLDrawable::isVisible would also write it when only the group is in view).
        const LLDrawable* drawable = avatar->mDrawable.get();
        return drawable && drawable->LLViewerOctreeEntryData::isVisible() ? AK_FULL : AK_OFFSCREEN;
    }

    Slot* find_slot(LLVOAvatar* avatar)
    {
        if (sSlotCount > 0 && sSlots[sLastSlot].avatar == avatar)
        {
            return &sSlots[sLastSlot];
        }
        for (U32 i = 0; i < sSlotCount; ++i)
        {
            if (sSlots[i].avatar == avatar)
            {
                sLastSlot = i;
                return &sSlots[i];
            }
        }
        if (sSlotCount >= MAX_SLOTS)
        {
            return nullptr;
        }
        Slot& slot = sSlots[sSlotCount];
        slot = Slot();
        slot.avatar = avatar;
        slot.kind = classify_avatar(avatar);
        sLastSlot = sSlotCount++;
        return &slot;
    }

    bool rebuilt_bit(const Slot& slot, S32 number)
    {
        return number >= 0 && number < 256 && (slot.rebuilt[number >> 6] >> (number & 63) & 1);
    }

    void set_rebuilt_bit(Slot& slot, S32 number)
    {
        if (number >= 0 && number < 256)
        {
            slot.rebuilt[number >> 6] |= (U64)1 << (number & 63);
        }
    }

    U32 face_class(U8 mask)
    {
        return mask == 0 ? 0 : mask == JK_COLLISION ? 1 : 2;
    }

    // What the lines are made of: the window's frames, seconds and the TSC's tick in ns.
    struct Scale
    {
        F64 frames = 1.0;
        F64 fetch_frames = 1.0;
        F64 seconds = 0.0;
        F64 ns_per_tick = 0.0;
        F64 read_ticks = 0.0;

        // ms a frame of the window
        F64 ms(F64 ticks) const { return ticks * ns_per_tick * 1e-6 / frames; }
    };

    F64 percent(F64 part, F64 whole)
    {
        return whole > 0.0 ? 100.0 * part / whole : 0.0;
    }

    std::ostringstream line_start(int precision)
    {
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(precision);
        return out;
    }

    // The updateImages ms a frame, a stage at a time, without the census's own ticks.
    struct Stages
    {
        F64 ms[STAGE_COUNT] = {};
        F64 total = 0.0;
        F64 self = 0.0;
    };

    Stages stage_times(const Window& w, const Scale& scale)
    {
        Stages stages;
        for (U32 s = 0; s < STAGE_COUNT; ++s)
        {
            stages.ms[s] = scale.ms(llmax((F64)w.stage_ticks[s] - (F64)w.stage_self[s], 0.0));
            stages.total += stages.ms[s];
            stages.self += scale.ms((F64)w.stage_self[s]);
        }
        return stages;
    }

    // (i), and the stages of (iv)
    void report_budget(const Window& w, const Scale& scale, const Stages& stages)
    {
        const F64 loop_ms = stages.ms[FETCH_LOOP];
        // What the loop would add through every due texture (its time over the ones it reached), with the census's share
        // of the loop's time taken out of that too
        const F64 loop_raw_ms = w.loop_ms / scale.frames;
        const F64 uncut_loop_ms = loop_raw_ms > 0.0 ? loop_ms * (w.uncut_loop_ms / scale.frames) / loop_raw_ms : loop_ms;
        const F64 uncut_ms = stages.total - loop_ms + uncut_loop_ms;
        const F64 budget_ms = w.budget_ms / scale.frames;
        const F64 fps = scale.frames / llmax(scale.seconds, 1e-9);
        const F64 urgent_frames = (F64)llmax(w.urgent_frames, (U64)1);

        std::ostringstream out = line_start(2);
        out << "images census over " << w.frames << " frames (" << scale.seconds << " s, " << fps << " fps), updateImages ms a frame (the census's own "
            << stages.self << " taken out): total " << stages.total << " =";
        for (U32 s = 0; s < STAGE_COUNT; ++s)
        {
            out << (s ? " + " : " ") << STAGE_NAMES[s] << " " << stages.ms[s];
        }
        out << " (fetch planning " << w.plan_ms / scale.frames << "); budget: updateImages limit " << budget_ms << " ms, fetch pass limit "
            << w.fetch_budget_ms / scale.fetch_frames << " ms; fetch pass cut off by its limit in " << w.fetch_cut << " of " << w.fetch_frames
            << " frames (" << percent((F64)w.fetch_cut, (F64)w.fetch_frames) << "%), urgent pass in " << w.urgent_cut << " of " << w.urgent_frames << " ("
            << percent((F64)w.urgent_cut, (F64)w.urgent_frames) << "%, entries " << (F64)w.urgent_entries / urgent_frames << ", reached "
            << (F64)w.urgent_reached / urgent_frames << ")";
        out.precision(1);
        const F64 textures = (F64)w.textures / scale.fetch_frames;
        const F64 copied = (F64)w.copied / scale.fetch_frames;
        const F64 advance = copied * (w.due ? (F64)w.visited / (F64)w.due : 1.0);   // textures a frame the lap moves on
        out << "; fetch a frame: textures " << textures << ", copied " << copied << ", due " << (F64)w.due / scale.fetch_frames << ", visited "
            << (F64)w.visited / scale.fetch_frames << " (" << percent((F64)w.visited, (F64)w.due) << "% of due), a lap of the list about "
            << textures / llmax(advance, 1e-9) / llmax(fps, 1e-9) << " s (uncut " << textures / llmax(copied, 1e-9) / llmax(fps, 1e-9) << " s)";
        out.precision(2);
        out << "; uncut demand " << uncut_ms << " ms a frame (total - fetch loop + fetch loop x due / visited of each frame " << uncut_loop_ms << "), "
            << (budget_ms > 0.0 ? uncut_ms / budget_ms : 0.0) << " x the updateImages limit";
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }

    // The rigged walks of one pass, in ms a frame: the isRiggedTo scan (what is left of the walk), the joints' work and the
    // rigging info, each with the TSC reads that sit in it taken out.
    struct WalkMs
    {
        F64 scan = 0.0;
        F64 body = 0.0;
        F64 info = 0.0;
        F64 total() const { return scan + body + info; }
    };

    WalkMs walk_times(const Window::WalkTicks& t, const Scale& scale)
    {
        const F64 read = scale.read_ticks;
        WalkMs ms;
        ms.body = scale.ms(llmax((F64)t.body - read * (F64)t.calls, 0.0));
        ms.info = scale.ms(llmax((F64)t.info - read * (F64)t.info_pairs, 0.0));
        ms.scan = scale.ms(llmax((F64)t.walk - (F64)t.joint_self - (F64)t.body - (F64)t.info - read * (F64)(t.calls + t.info_pairs + t.walks), 0.0));
        return ms;
    }

    void put_walk(std::ostringstream& out, const char* where, const char* stage_name, const WalkMs& ms, F64 stage_ms)
    {
        out << where << " " << ms.total() << " = isRiggedTo scan " << ms.scan << " + joint work (getJoint, world matrix, box) " << ms.body
            << " + rigging info " << ms.info << " (" << percent(ms.total(), stage_ms) << "% of the " << stage_name << ")";
    }

    // (iv): the rigged walk
    void report_walk(const Window& w, const Scale& scale, const Stages& stages)
    {
        const WalkMs fetch = walk_times(w.walk_ticks[0], scale);
        const WalkMs urgent = walk_times(w.walk_ticks[1], scale);
        const F64 walks = (F64)llmax(w.walks, (U64)1);
        U64 faces = 0;
        for (U32 k = 0; k < AK_COUNT; ++k)
        {
            faces += w.faces[k];
        }

        std::ostringstream out = line_start(1);
        out << "images census rigged area a frame: worked out " << (F64)faces / scale.frames << " (";
        for (U32 k = 0; k < AK_COUNT; ++k)
        {
            out << (k ? ", " : "") << AV_NAMES[k] << " " << (F64)w.faces[k] / scale.frames;
        }
        out << "), in the urgent pass " << (F64)w.faces_urgent / scale.frames << ", no extents " << (F64)w.no_extents / scale.frames
            << ", joints worked " << (F64)w.joints / scale.frames << " (" << (F64)w.joints / walks << " a face)";
        out.precision(3);
        out << "; ms: rigged walk ";
        put_walk(out, "in the fetch loop", "fetch loop", fetch, stages.ms[FETCH_LOOP]);
        out << ", ";
        put_walk(out, "in the urgent pass", "urgent stage", urgent, stages.ms[URGENT]);
        out << " (together " << percent(fetch.total() + urgent.total(), stages.total) << "% of updateImages); the walks are timed with the TSC, "
            << scale.read_ticks << " ticks a read taken out";
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }

    // (ii)
    void report_dirty(const Window& w, const Scale& scale)
    {
        U64 faces = 0;
        for (U32 k = 0; k < AK_COUNT; ++k)
        {
            faces += w.faces[k];
        }
        std::ostringstream out = line_start(1);
        out << "images census rigged dirty joints (faces a frame of " << (F64)faces / scale.frames << "; a face counts in each joint kind it has)";
        for (U32 m = 0; m < MODES; ++m)
        {
            out << "; " << MODE_NAMES[m] << ": a dirty joint in " << (F64)w.dirty_faces[m] / scale.frames << " ("
                << percent((F64)w.dirty_faces[m], (F64)faces) << "%), by joint kind";
            for (U32 k = 0; k < JK_KINDS; ++k)
            {
                out << (k ? ", " : " ") << JK_NAMES[k] << " " << (F64)w.dirty_kind[m][k] / scale.frames;
            }
            out << ", collision volume only " << (F64)w.dirty_collision_only[m] / scale.frames << ", by avatar kind";
            for (U32 k = 0; k < AK_COUNT; ++k)
            {
                out << (k ? ", " : " ") << AV_NAMES[k] << " " << (F64)w.dirty_av[m][k] / scale.frames << " of " << (F64)w.faces[k] / scale.frames;
            }
        }
        out << "; walks of an avatar the pass had no room to record " << (F64)w.no_record / scale.frames << " a frame";
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }

    // (iii)
    void report_textures(const Window& w, const Scale& scale)
    {
        std::ostringstream out = line_start(2);
        out << "images census rigged textures a frame: visited with a rigged face worked out " << (F64)w.tex_rigged / scale.frames;
        for (U32 m = 0; m < MODES; ++m)
        {
            out << "; by the worst face " << MODE_NAMES[m] << ":";
            F64 head_ms = 0.0;     // what a lookahead on the main thread could hand to other threads
            for (U32 c = 0; c < CLASSES; ++c)
            {
                const F64 class_ms = scale.ms((F64)w.tex_ticks[m][c]);
                out << (c ? ", " : " ") << CLASS_NAMES[c] << " " << (F64)w.tex_class[m][c] / scale.frames << " (ms " << class_ms << ")";
                head_ms += c < 2 ? class_ms : 0.0;
            }
            out << ", all clean + collision volume only " << head_ms << " ms";
        }
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }

    void report()
    {
        const U64 ticks_now = __rdtsc();
        const S64 ns_now = now_nanoseconds();
        Scale scale;
        scale.seconds = (F64)(ns_now - sWindowNanoseconds) * 1e-9;
        scale.ns_per_tick = ticks_now > sWindowTicks ? (F64)(ns_now - sWindowNanoseconds) / (F64)(ticks_now - sWindowTicks) : 0.0;
        scale.frames = (F64)llmax(sWindow.frames, (U64)1);
        scale.fetch_frames = (F64)llmax(sWindow.fetch_frames, (U64)1);
        scale.read_ticks = llmax(sReadTicks, 0.0);
        const Stages stages = stage_times(sWindow, scale);
        report_budget(sWindow, scale, stages);
        report_walk(sWindow, scale, stages);
        report_dirty(sWindow, scale);
        report_textures(sWindow, scale);
    }
}

namespace plus::imagescensus
{
    void PassScope::begin()
    {
        if (sWindowNanoseconds == 0)    // the first pass
        {
            if (sReadTicks < 0.0)
            {
                sReadTicks = measure_read_ticks();
            }
            open_window();
        }
        sSlotCount = 0;
        sLastSlot = 0;
        sTexOpen = false;
        sStage = PROLOGUE;
        sActive = true;
        sStageStart = __rdtsc();
    }

    void PassScope::end()
    {
        const U64 now = __rdtsc();
        sActive = false;
        sTexOpen = false;
        Window& w = sWindow;
        w.stage_ticks[sStage] += now - sStageStart;
        ++w.frames;
        if ((F64)(now_nanoseconds() - sWindowNanoseconds) < REPORT_SECONDS * 1e9)
        {
            return;
        }
        report();
        open_window();
    }

    void enterStage(Stage stage)
    {
        const U64 now = __rdtsc();
        sWindow.stage_ticks[sStage] += now - sStageStart;
        sStage = stage;
        sStageStart = now;
        if (STAGE_MARKS[stage])
        {
            LLRHI::cpuDetailMark(STAGE_MARKS[stage]);
        }
    }

    void noteBudget(F32 seconds)
    {
        sWindow.budget_ms += (F64)seconds * 1000.0;
    }

    void noteFetch(F32 budget_seconds, size_t textures, size_t copied, size_t due, size_t visited, bool cut, F32 loop_seconds,
                   F64 plan_ms)
    {
        Window& w = sWindow;
        ++w.fetch_frames;
        w.fetch_cut += cut ? 1 : 0;
        w.fetch_budget_ms += (F64)budget_seconds * 1000.0;
        w.textures += textures;
        w.copied += copied;
        w.due += due;
        w.visited += visited;
        const F64 loop_ms = (F64)loop_seconds * 1000.0;
        w.loop_ms += loop_ms;
        w.plan_ms += plan_ms;
        // what the loop would take through every due texture: its time over the ones it reached
        w.uncut_loop_ms += visited > 0 ? loop_ms * (F64)due / (F64)visited : loop_ms;
    }

    void noteUrgent(size_t entries, size_t reached, bool cut)
    {
        Window& w = sWindow;
        ++w.urgent_frames;
        w.urgent_cut += cut ? 1 : 0;
        w.urgent_entries += entries;
        w.urgent_reached += reached;
    }

    void beginTexture()
    {
        sTexOpen = true;
        sTexRigged = 0;
        sTexSelf = 0;
        sTexWorst[0] = sTexWorst[1] = 0;
        sTexStart = __rdtsc();
    }

    void endTexture()
    {
        if (!sTexOpen)
        {
            return;
        }
        const U64 now = __rdtsc();
        sTexOpen = false;
        Window& w = sWindow;
        // the two reads of this visit (the start's, and this one) are the census's, in the fetch loop
        const F64 read = llmax(sReadTicks, 0.0);
        w.stage_self[FETCH_LOOP] += (U64)(2.0 * read + 0.5);
        if (sTexRigged == 0)
        {
            return;
        }
        const U64 taken = now - sTexStart;
        const U64 own = sTexSelf + (U64)(read + 0.5);
        const U64 ticks = taken > own ? taken - own : 0;
        ++w.tex_rigged;
        for (U32 m = 0; m < MODES; ++m)
        {
            const U8 c = sTexWorst[m];
            ++w.tex_class[m][c];
            w.tex_ticks[m][c] += ticks;
        }
    }

    void RiggedWalk::begin(LLVOAvatar* avatar)
    {
        const U64 before = __rdtsc();
        mAvatar = avatar;
        Slot* slot = find_slot(avatar);
        mSlot = slot;
        mKind = slot ? slot->kind : classify_avatar(avatar);
        mStart = __rdtsc();
        mSelfTicks = mStart - before;
    }

    void RiggedWalk::joint(S32 index)
    {
        const U64 before = __rdtsc();
        ++mCalls;
        // The same lookup the work does next (an array index, or a find in the attachment map): no effect
        LLJoint* joint = mAvatar->getJoint(index);
        if (joint)
        {
            Slot* slot = static_cast<Slot*>(mSlot);
            const U8 kind = index < mAvatar->mNumBones ? JK_BONE : index < mAvatar->mNumBones + mAvatar->mNumCollisionVolumes ? JK_COLLISION : JK_ATTACHMENT;
            const bool dirty = (joint->mDirtyFlags & LLJoint::MATRIX_DIRTY) != 0;
            if (dirty)
            {
                mComputeMask |= kind;
            }
            if (dirty || (slot && rebuilt_bit(*slot, index)))
            {
                mStartMask |= kind;
            }
            if (dirty && slot)
            {
                // The asking for its world matrix rebuilds it and the dirty ones above it: written down, for
                // the faces after this one (a flag read, nothing written to the joints)
                for (LLJoint* up = joint; up && (up->mDirtyFlags & LLJoint::MATRIX_DIRTY); up = up->getParent())
                {
                    set_rebuilt_bit(*slot, up->getJointNum());
                }
            }
            ++mJoints;
        }
        mBodyStart = __rdtsc();
        mJointSelfTicks += mBodyStart - before;
    }

    void RiggedWalk::finish(bool has_extents)
    {
        const U64 now = __rdtsc();
        Window& w = sWindow;
        ++w.walks;
        ++w.faces[mKind];
        w.faces_urgent += sStage == URGENT ? 1 : 0;
        w.no_extents += has_extents ? 0 : 1;
        w.joints += mJoints;
        w.no_record += mSlot ? 0 : 1;
        Window::WalkTicks& ticks = w.walk_ticks[sStage == URGENT ? 1 : 0];
        ticks.walk += now - mStart;
        ticks.body += mBodyTicks;
        ticks.joint_self += mJointSelfTicks;
        ticks.info += mInfoTicks;
        ticks.calls += mCalls;
        ticks.info_pairs += mInfoPairs;
        ++ticks.walks;
        const U8 masks[MODES] = { mComputeMask, mStartMask };
        for (U32 m = 0; m < MODES; ++m)
        {
            const U8 mask = masks[m];
            if (mask)
            {
                ++w.dirty_faces[m];
                ++w.dirty_av[m][mKind];
                for (U32 k = 0; k < JK_KINDS; ++k)
                {
                    w.dirty_kind[m][k] += (mask >> k) & 1;
                }
                w.dirty_collision_only[m] += mask == JK_COLLISION ? 1 : 0;
            }
            if (sTexOpen)
            {
                sTexWorst[m] = (U8)llmax((U32)sTexWorst[m], face_class(mask));
            }
        }
        if (sTexOpen)
        {
            ++sTexRigged;
        }
        // The census's own time: this walk's set-up, its reads of the joints and this closing, and the TSC reads that sit
        // outside those spans: one before the set-up, one before the closing, two a joint call (the one before its span
        // and the one that ends the work), two an info pair (the one before its span and the one that ends it)
        const F64 reads = 2.0 + 2.0 * mCalls + 2.0 * mInfoPairs;
        const U64 self = mSelfTicks + mJointSelfTicks + (__rdtsc() - now) + (U64)(reads * llmax(sReadTicks, 0.0) + 0.5);
        w.stage_self[sStage] += self;
        sTexSelf += self;
    }
}
