/**
 * @file motiontimeline.cpp
 * @brief The key list, the segment table and the joined clip of research/357 (see motiontimeline.h).
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

#include "motiontimeline.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace plus::posermotion
{
    namespace
    {
        namespace history = posermcp::history;

        constexpr F64 HALF_TURN_DEG = 180.0;
        constexpr F64 PI            = 3.14159265358979323846;
        /// Two poses with no more than this between them are "the same" (history::samePose's defaults).
        constexpr F64 SAME_DEG   = 0.01;
        constexpr F64 SAME_M     = 1e-5;

        /// The turn from a to b in degrees, exact near 0 (the angle does not depend on the product order).
        F64 turnDeg(const history::Quat& a, const history::Quat& b)
        {
            const F64 w = (F64)a.w * b.w + (F64)a.x * b.x + (F64)a.y * b.y + (F64)a.z * b.z;
            const F64 x = (F64)a.w * b.x - (F64)b.w * a.x - ((F64)a.y * b.z - (F64)a.z * b.y);
            const F64 y = (F64)a.w * b.y - (F64)b.w * a.y - ((F64)a.z * b.x - (F64)a.x * b.z);
            const F64 z = (F64)a.w * b.z - (F64)b.w * a.z - ((F64)a.x * b.y - (F64)a.y * b.x);
            return 2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::fabs(w)) * HALF_TURN_DEG / PI;
        }

        F64 gap(const history::Vec3& a, const history::Vec3& b)
        {
            return std::max({ std::fabs((F64)a.x - b.x), std::fabs((F64)a.y - b.y), std::fabs((F64)a.z - b.z) });
        }

        /// The two poses hold the same joints and no joint differs by more than deg or metres (a joint that only
        /// one of them has, or one that is driven in one only, counts as different).
        bool posesWithin(const history::Pose& a, const history::Pose& b, F64 deg, F64 metres)
        {
            if (a.joints.size() != b.joints.size())
            {
                return false;
            }
            std::unordered_map<std::string, const history::JointState*> by_name;
            by_name.reserve(b.joints.size());
            for (const history::JointState& j : b.joints)
            {
                by_name.emplace(j.name, &j);
            }
            for (const history::JointState& ja : a.joints)
            {
                const auto it = by_name.find(ja.name);
                if (it == by_name.end())
                {
                    return false;
                }
                const history::JointState& jb = *it->second;
                if (ja.enabled != jb.enabled || turnDeg(ja.target, jb.target) > deg ||
                    gap(ja.position, jb.position) > metres || gap(ja.scale, jb.scale) > metres)
                {
                    return false;
                }
            }
            return true;
        }

        bool samePoses(const history::Pose& a, const history::Pose& b) { return posesWithin(a, b, SAME_DEG, SAME_M); }

        S32 holdFramesOf(F32 hold_sec)
        {
            return (S32)std::lround((F64)std::clamp(hold_sec, 0.f, HOLD_MAX_SEC) * CLIP_FPS);
        }

        bool layoutsEqual(const TrackLayout& a, const TrackLayout& b)
        {
            if (a.joints.size() != b.joints.size() || a.pelvis_rest != b.pelvis_rest)
            {
                return false;
            }
            for (size_t i = 0; i < a.joints.size(); ++i)
            {
                const TrackJoint &x = a.joints[i], &y = b.joints[i];
                if (x.name != y.name || x.rest != y.rest || x.extra_rot != y.extra_rot || x.position != y.position ||
                    x.scale != y.scale || x.pelvis != y.pelvis || x.enabled != y.enabled)
                {
                    return false;
                }
            }
            return true;
        }

        /// A pair of keys no pair is made for yet, with the method it inherits (syncPairs says from which pair),
        /// Smooth when the two poses are the same (Generate is refused for them).
        PairCache newPair(SegMethod inherited, const history::Pose& a, const history::Pose& b)
        {
            PairCache c;
            c.method = (inherited == SegMethod::Generate && samePoses(a, b)) ? SegMethod::Smooth : inherited;
            c.state  = c.method == SegMethod::Generate ? SegState::Unmade : SegState::Made;
            return c;
        }

        /// The SOMA joints of the 12 body points of the automatic judgement (research/361): head, elbows (the forearm
        /// joints), wrists (the hands), the middle finger ends, knees (the shins), ankles (the feet), hips.
        constexpr S32 PASS_SOMA_JOINT[PASS_POINT_COUNT] = { 6, 12, 18, 13, 19, 15, 21, 23, 27, 24, 28, 0 };

        /// The segment has an answer in use (a selected one).
        bool answerInUse(const PairCache* c)
        {
            return c && c->method == SegMethod::Generate && c->selected >= 0 && c->selected < (S32)c->answers.size();
        }

        /// A key pose now differs from the one an answer was asked with by more than REGEN_DEG at a joint or REGEN_M at the
        /// pelvis (the version is the quick answer: the same version is the same pose).
        bool keyPoseMoved(const KeyPose& old_pose, U32 old_ver, const KeyPose& now, U32 now_ver)
        {
            if (old_ver == now_ver)
            {
                return false;
            }
            if (old_pose.sl.local.size() != now.sl.local.size())
            {
                return true;
            }
            for (size_t j = 0; j < now.sl.local.size(); ++j)
            {
                if (rotationAngle(old_pose.sl.local[j], now.sl.local[j]) * RAD_TO_DEG > REGEN_DEG)
                {
                    return true;
                }
            }
            return (old_pose.sl.pelvis - now.sl.pelvis).length() > REGEN_M;
        }

        /// Generate: the model's tokens ("automatic" is TIMELINE_TOKENS_DEFAULT).
        S32 tokensOf(S32 tokens) { return tokens == TIMELINE_TOKENS_AUTO ? TIMELINE_TOKENS_DEFAULT : tokens; }

        /// What a setting change may touch, kept to put back when the change makes the whole too long.
        struct Settings
        {
            SegMethod method;
            SegState  state;
            JobError  fail;
            S32       selected;
            S32       tokens;
            F32       seconds;
        };
        Settings saved(const PairCache& c) { return Settings{ c.method, c.state, c.fail, c.selected, c.tokens, c.seconds }; }
        void     restore(PairCache& c, const Settings& s)
        {
            c.method   = s.method;
            c.state    = s.state;
            c.fail     = s.fail;
            c.selected = s.selected;
            c.tokens   = s.tokens;
            c.seconds  = s.seconds;
        }
        /// Takes answer `index` as the one used; the asked length follows it ("automatic" stays automatic for 10).
        void selectIndex(PairCache& c, S32 index)
        {
            c.selected = index;
            const S32 t = c.answers[(size_t)index].tokens;
            c.tokens    = (t == TIMELINE_TOKENS_DEFAULT && c.tokens == TIMELINE_TOKENS_AUTO) ? TIMELINE_TOKENS_AUTO : t;
        }
    } // namespace

    S32 framesOfSeconds(F32 seconds)
    {
        const F32 s = std::isfinite(seconds) ? std::clamp(seconds, SEGMENT_MIN_SEC, SEGMENT_MAX_SEC) : SEGMENT_DEFAULT_SEC;
        return (S32)std::lround((F64)s * CLIP_FPS) + 1;
    }

    const char* describe(TimelineResult result)
    {
        switch (result)
        {
        case TimelineResult::Ok:          return "ok";
        case TimelineResult::TooManyKeys: return "too many keys";
        case TimelineResult::TooLong:     return "the motion would be longer than 30 seconds";
        case TimelineResult::NoSuchKey:   return "no such key";
        case TimelineResult::NoSuchPair:  return "no such segment";
        case TimelineResult::BadValue:    return "value out of range";
        case TimelineResult::NeedsKeys:   return "two keys are needed";
        case TimelineResult::SamePoses:   return "the two poses are the same";
        case TimelineResult::NotReady:    return "the rest skeleton is not ready";
        case TimelineResult::Busy:        return "the segment is being generated";
        case TimelineResult::BuildFailed: return "the motion could not be made";
        case TimelineResult::NeedPrev:    return "the segment before it has to be generated first";
        }
        return "unknown";
    }

    bool isWholeFailure(JobError error)
    {
        switch (error)
        {
        case JobError::LowMemory:
        case JobError::NoHelper:
        case JobError::BadFolder:
        case JobError::ModelMissing:
        case JobError::ModelUnreadable:
        case JobError::Timeout:
        case JobError::HelperEnded:
            return true;
        case JobError::None:
        case JobError::BadInput:
        case JobError::RunFailed:
        case JobError::BadAnswer:
        case JobError::CannotBuild:
        case JobError::PrevMissing:
            break;
        }
        return false;
    }

    const char* describe(JobError error)
    {
        switch (error)
        {
        case JobError::None:            return "none";
        case JobError::LowMemory:       return "not enough free memory";
        case JobError::NoHelper:        return "the motion helper cannot be started";
        case JobError::BadFolder:       return "the model folder is not usable";
        case JobError::ModelMissing:    return "the model file is missing";
        case JobError::ModelUnreadable: return "the model cannot be read";
        case JobError::Timeout:         return "the helper took too long";
        case JobError::HelperEnded:     return "the helper ended";
        case JobError::BadInput:        return "the poses were refused";
        case JobError::RunFailed:       return "the model failed";
        case JobError::BadAnswer:       return "the answer was not usable";
        case JobError::CannotBuild:     return "the answer could not be fitted to the poses";
        case JobError::PrevMissing:     return "the segment before it was not made, so this one was put back";
        }
        return "unknown";
    }

    const char* describe(PassEligibility e)
    {
        switch (e)
        {
        case PassEligibility::Ok:                   return "ok";
        case PassEligibility::EndKey:               return "the first and the last key cannot be passed";
        case PassEligibility::NeighbourNotGenerate: return "both segments next to the key have to be generated";
        case PassEligibility::Hold:                 return "a key with a hold is stopped at";
        case PassEligibility::SamePose:             return "a neighbour has the same pose";
        }
        return "unknown";
    }

    void passPoints(const SomaPose& pose, LLVector3 (&out)[PASS_POINT_COUNT])
    {
        for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
        {
            out[k] = pose.pos[(size_t)PASS_SOMA_JOINT[k]];
        }
    }

    AutoPassResult judgeAutoPass(const LLVector3 (&a)[PASS_POINT_COUNT], const LLVector3 (&b)[PASS_POINT_COUNT],
                                 const LLVector3 (&c)[PASS_POINT_COUNT])
    {
        F64 dot = 0.0, norm1 = 0.0, norm2 = 0.0, move1 = 0.0, move2 = 0.0;
        for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
        {
            const LLVector3 d1 = b[k] - a[k], d2 = c[k] - b[k];
            dot += (F64)(d1 * d2);
            norm1 += (F64)(d1 * d1);
            norm2 += (F64)(d2 * d2);
            move1 += (F64)d1.length();
            move2 += (F64)d2.length();
        }
        AutoPassResult r;
        r.move1_m = (F32)(move1 / PASS_POINT_COUNT);
        r.move2_m = (F32)(move2 / PASS_POINT_COUNT);
        const F64 denom = std::sqrt(norm1) * std::sqrt(norm2);
        r.cos           = denom > 1e-12 && std::isfinite(dot) ? (F32)(dot / denom) : 0.f;
        // a key that hardly moves has an unreliable direction: it is stopped at (a number that is not a number as well)
        r.through = r.move1_m >= PASS_AUTO_MIN_MOVE && r.move2_m >= PASS_AUTO_MIN_MOVE && r.cos >= PASS_AUTO_COS;
        return r;
    }

    // ---- lookups ----------------------------------------------------------------------------------------------

    S32 MotionTimeline::indexOf(KeyId id) const
    {
        for (size_t i = 0; i < mKeys.size(); ++i)
        {
            if (mKeys[i].id == id)
            {
                return (S32)i;
            }
        }
        return -1;
    }

    const TimelineKey* MotionTimeline::find(KeyId id) const
    {
        const S32 i = indexOf(id);
        return i < 0 ? nullptr : &mKeys[(size_t)i];
    }

    TimelineKey* MotionTimeline::findMutable(KeyId id)
    {
        const S32 i = indexOf(id);
        return i < 0 ? nullptr : &mKeys[(size_t)i];
    }

    const TimelineKey& MotionTimeline::key(S32 index) const
    {
        static const TimelineKey none;
        return index >= 0 && index < (S32)mKeys.size() ? mKeys[(size_t)index] : none;
    }

    const SegmentInfo& MotionTimeline::segment(S32 index) const
    {
        static const SegmentInfo none;
        return index >= 0 && index < (S32)mSegments.size() ? mSegments[(size_t)index] : none;
    }

    S32 MotionTimeline::keyFrame(S32 index) const
    {
        return index >= 0 && index < (S32)mKeyFrames.size() ? mKeyFrames[(size_t)index] : 0;
    }

    const KeyPose& MotionTimeline::keyPose(S32 index) const
    {
        static const KeyPose none;
        return index >= 0 && index < (S32)mKeyPoses.size() ? mKeyPoses[(size_t)index] : none;
    }

    /// The keys index and index + 1 are equal. Worked out again only when one of them was replaced.
    bool MotionTimeline::samePair(S32 index)
    {
        PairCache& c = mPairs.at(PairId{ mKeys[(size_t)index].id, mKeys[(size_t)index + 1].id });
        const U32  a = mKeys[(size_t)index].pose_ver, b = mKeys[(size_t)index + 1].pose_ver;
        if (!c.same_known || c.same_from_ver != a || c.same_to_ver != b)
        {
            c.same          = samePoses(mKeys[(size_t)index].pose, mKeys[(size_t)index + 1].pose);
            c.same_known    = true;
            c.same_from_ver = a;
            c.same_to_ver   = b;
        }
        return c.same;
    }

    const SegmentInfo* MotionTimeline::findSegment(PairId pair) const
    {
        for (const SegmentInfo& s : mSegments)
        {
            if (s.pair == pair)
            {
                return &s;
            }
        }
        return nullptr;
    }

    const PairCache* MotionTimeline::cache(PairId pair) const
    {
        const auto it = mPairs.find(pair);
        return it == mPairs.end() ? nullptr : &it->second;
    }

    PairCache* MotionTimeline::cacheMutable(PairId pair)
    {
        const auto it = mPairs.find(pair);
        return it == mPairs.end() ? nullptr : &it->second;
    }

    S32 MotionTimeline::holdFrames(S32 index) const
    {
        return index >= 0 && index < (S32)mKeys.size() ? holdFramesOf(mKeys[(size_t)index].hold_sec) : 0;
    }

    /// Every queued job of the pair is taken out of the queue (its state is the caller's to set).
    void MotionTimeline::eraseQueued(PairId pair)
    {
        mQueue.erase(std::remove_if(mQueue.begin(), mQueue.end(), [&](const Queued& q) { return q.pair == pair; }), mQueue.end());
    }

    bool MotionTimeline::hasQueued(PairId pair) const
    {
        return std::any_of(mQueue.begin(), mQueue.end(), [&](const Queued& q) { return q.pair == pair; });
    }

    // ---- lengths ----------------------------------------------------------------------------------------------

    /// The frames of a segment's clip (first = its from key, last = its to key), from what is known without making it.
    S32 MotionTimeline::segmentFrames(const PairCache& c)
    {
        if (c.method == SegMethod::Generate)
        {
            if (c.selected >= 0 && c.selected < (S32)c.answers.size())
            {
                return timelineFramesOfTokens(c.answers[(size_t)c.selected].tokens);
            }
            return timelineFramesOfTokens(tokensOf(c.tokens));
        }
        return framesOfSeconds(c.seconds);
    }

    /// The joined clip's frames if the keys were in this order (a pair that has no entry yet counts as it would be
    /// made, taking the larger of Generate's and Smooth's default so that the check never lets too much through).
    S32 MotionTimeline::totalFor(const std::vector<KeyId>& order) const
    {
        S64 total = order.empty() ? 0 : 1;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const TimelineKey* k = find(order[i]);
            total += k ? holdFramesOf(k->hold_sec) : 0;
            if (i + 1 < order.size())
            {
                const auto it = mPairs.find(PairId{ order[i], order[i + 1] });
                total += (it != mPairs.end() ? segmentFrames(it->second)
                                             : std::max(timelineFramesOfTokens(TIMELINE_TOKENS_DEFAULT),
                                                        framesOfSeconds(SEGMENT_DEFAULT_SEC))) - 1;
            }
        }
        return (S32)std::min<S64>(total, 1 << 30);
    }

    // ---- keys -------------------------------------------------------------------------------------------------

    TimelineResult MotionTimeline::insertAt(S32 index, const history::Pose& pose, KeyId* out_id, bool check_length)
    {
        if (index < 0 || index > (S32)mKeys.size())
        {
            return TimelineResult::BadValue;
        }
        if ((S32)mKeys.size() >= TIMELINE_MAX_KEYS)
        {
            return TimelineResult::TooManyKeys;
        }
        std::vector<KeyId> order;
        order.reserve(mKeys.size() + 1);
        for (const TimelineKey& k : mKeys)
        {
            order.push_back(k.id);
        }
        order.insert(order.begin() + index, mNextId); // not in mKeys yet: counts as a key without a hold
        if (check_length && totalFor(order) > TIMELINE_MAX_FRAMES)
        {
            return TimelineResult::TooLong;
        }
        TimelineKey key;
        key.id       = mNextId++;
        key.pose     = pose;
        key.pose_ver = 1;
        const KeyId id = key.id;
        mKeys.insert(mKeys.begin() + index, std::move(key));
        structureChanged();
        if (out_id)
        {
            *out_id = id;
        }
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::addKey(const history::Pose& pose, KeyId* out_id)
    {
        return insertAt((S32)mKeys.size(), pose, out_id);
    }

    TimelineResult MotionTimeline::insertKey(S32 index, const history::Pose& pose, KeyId* out_id)
    {
        return insertAt(index, pose, out_id);
    }

    TimelineResult MotionTimeline::duplicateKey(KeyId id, KeyId* out_id)
    {
        const S32 i = indexOf(id);
        if (i < 0)
        {
            return TimelineResult::NoSuchKey;
        }
        if ((S32)mKeys.size() >= TIMELINE_MAX_KEYS)
        {
            return TimelineResult::TooManyKeys;
        }
        // the copy adds one Smooth pair of the default length (the pair after the key moves to the copy, as long as it was);
        // the whole is what it is now plus that
        if (mTotalFrames + framesOfSeconds(SEGMENT_DEFAULT_SEC) - 1 > TIMELINE_MAX_FRAMES)
        {
            return TimelineResult::TooLong;
        }
        const history::Pose copy = mKeys[(size_t)i].pose;
        // the pair that left the key goes on from the copy (same pose, so its settings and answers still fit)
        std::optional<PairCache> carried;
        KeyId                    next = NO_KEY;
        if (i + 1 < (S32)mKeys.size())
        {
            next = mKeys[(size_t)i + 1].id;
            if (const PairCache* old = cache(PairId{ id, next }))
            {
                carried = *old;
                restoreAfterStop(*carried); // a job of the old pair is dropped with it
                carried->fitted     = false;
                carried->same_known = false;
            }
        }
        KeyId                made = NO_KEY;
        const TimelineResult r    = insertAt(i + 1, copy, &made, false);
        if (r == TimelineResult::Ok)
        {
            // the pair between is a Smooth one whatever the one before was (equal poses cannot be generated)
            if (PairCache* c = cacheMutable(PairId{ id, made }))
            {
                *c = newPair(SegMethod::Smooth, copy, copy);
            }
            if (carried)
            {
                mPairs[PairId{ made, next }] = std::move(*carried);
            }
            updateView();
            if (out_id)
            {
                *out_id = made;
            }
        }
        return r;
    }

    TimelineResult MotionTimeline::removeKey(KeyId id)
    {
        const S32 i = indexOf(id);
        if (i < 0)
        {
            return TimelineResult::NoSuchKey;
        }
        std::vector<KeyId> order;
        for (const TimelineKey& k : mKeys)
        {
            if (k.id != id)
            {
                order.push_back(k.id);
            }
        }
        if (totalFor(order) > TIMELINE_MAX_FRAMES) // the pair that closes the gap may be longer than the two it replaces
        {
            return TimelineResult::TooLong;
        }
        mKeys.erase(mKeys.begin() + i);
        structureChanged();
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::moveKey(KeyId id, S32 to_index)
    {
        const S32 from = indexOf(id);
        if (from < 0)
        {
            return TimelineResult::NoSuchKey;
        }
        if (to_index < 0 || to_index >= (S32)mKeys.size())
        {
            return TimelineResult::BadValue;
        }
        if (to_index == from)
        {
            return TimelineResult::Ok;
        }
        std::vector<KeyId> order;
        for (const TimelineKey& k : mKeys)
        {
            order.push_back(k.id);
        }
        order.erase(order.begin() + from);
        order.insert(order.begin() + to_index, id);
        if (totalFor(order) > TIMELINE_MAX_FRAMES)
        {
            return TimelineResult::TooLong;
        }
        TimelineKey moved = std::move(mKeys[(size_t)from]);
        mKeys.erase(mKeys.begin() + from);
        mKeys.insert(mKeys.begin() + to_index, std::move(moved));
        structureChanged();
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::updateKey(KeyId id, const history::Pose& pose)
    {
        TimelineKey* k = findMutable(id);
        if (!k)
        {
            return TimelineResult::NoSuchKey;
        }
        k->pose = pose;
        ++k->pose_ver;
        mKeysChanged = true;
        markStale();
        updateView();
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::setPass(KeyId id, PassMode mode)
    {
        TimelineKey* k = findMutable(id);
        if (!k)
        {
            return TimelineResult::NoSuchKey;
        }
        if ((U8)mode > (U8)PassMode::Through)
        {
            return TimelineResult::BadValue; // not a value of the enum (a cast integer of a widget)
        }
        if (k->pass != mode)
        {
            k->pass = mode;
            markStale(); // pass_effective and the marks are worked out again by the next refresh
        }
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::setStrength(KeyId id, PassStrength strength)
    {
        TimelineKey* k = findMutable(id);
        if (!k)
        {
            return TimelineResult::NoSuchKey;
        }
        if ((U8)strength > (U8)PassStrength::Normal)
        {
            return TimelineResult::BadValue;
        }
        if (k->strength != strength)
        {
            k->strength = strength;
            markStale();
        }
        return TimelineResult::Ok;
    }

    /// From the methods, the hold and the "same poses" flag (updateView keeps it up to date): no pose maths.
    PassEligibility MotionTimeline::eligibilityAt(S32 index) const
    {
        const S32 n = (S32)mKeys.size();
        if (index <= 0 || index >= n - 1)
        {
            return PassEligibility::EndKey;
        }
        const PairCache* before = cache(PairId{ mKeys[(size_t)index - 1].id, mKeys[(size_t)index].id });
        const PairCache* after  = cache(PairId{ mKeys[(size_t)index].id, mKeys[(size_t)index + 1].id });
        if (!before || !after || before->method != SegMethod::Generate || after->method != SegMethod::Generate)
        {
            return PassEligibility::NeighbourNotGenerate;
        }
        if (mKeys[(size_t)index].hold_sec > 0.f)
        {
            return PassEligibility::Hold;
        }
        return (before->same || after->same) ? PassEligibility::SamePose : PassEligibility::Ok;
    }

    PassEligibility MotionTimeline::passEligibility(KeyId id) const
    {
        const S32 i = indexOf(id);
        return i < 0 ? PassEligibility::EndKey : eligibilityAt(i);
    }

    void MotionTimeline::setPassingEnabled(bool enabled)
    {
        if (mPassEnabled != enabled)
        {
            mPassEnabled = enabled;
            if (!enabled)
            {
                // off: at once (not at the next refresh, which may fail): no key is passed
                for (TimelineKey& k : mKeys)
                {
                    k.pass_effective = k.pass_auto = false;
                }
            }
            markStale();
        }
    }

    template <typename Undo>
    TimelineResult MotionTimeline::commitOrUndo(Undo&& undo)
    {
        updateView();
        if (mTotalFrames > TIMELINE_MAX_FRAMES)
        {
            undo();
            updateView();
            return TimelineResult::TooLong;
        }
        markStale();
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::setHold(KeyId id, F32 seconds)
    {
        TimelineKey* k = findMutable(id);
        if (!k)
        {
            return TimelineResult::NoSuchKey;
        }
        if (!std::isfinite(seconds) || seconds < 0.f || seconds > HOLD_MAX_SEC)
        {
            return TimelineResult::BadValue;
        }
        const F32 old = k->hold_sec;
        k->hold_sec   = seconds;
        return commitOrUndo([&] { k->hold_sec = old; });
    }

    TimelineResult MotionTimeline::closeLoop(KeyId* out_id)
    {
        if (mKeys.size() < 2)
        {
            return TimelineResult::NeedsKeys;
        }
        if (posesWithin(mKeys.front().pose, mKeys.back().pose, LOOP_SAME_DEG, LOOP_SAME_M))
        {
            return TimelineResult::BadValue; // already a loop
        }
        const history::Pose first = mKeys.front().pose;
        return insertAt((S32)mKeys.size(), first, out_id);
    }

    bool MotionTimeline::clearAll()
    {
        const bool was_running = mRunning.active;
        mKeys.clear();
        mPairs.clear();
        mQueue.clear();
        mRunning = Running{};
        mLayout.clear();
        mKeyPoses.clear();
        mClip        = MotionClip{};
        ++mLayoutVersion;
        ++mClipVersion;
        mTrackResult = TrackResult::Ok;
        structureChanged();
        return was_running;
    }

    void MotionTimeline::invalidateClips()
    {
        for (auto& entry : mPairs)
        {
            entry.second.fitted = false;
        }
        ++mSkeletonGen; // what the automatic judgement was worked out from is gone
        mKeysChanged = true;
        markStale();
    }

    // ---- pairs ------------------------------------------------------------------------------------------------

    void MotionTimeline::structureChanged()
    {
        syncPairs();
        mKeysChanged = true;
        markStale();
        updateView();
    }

    /// The table has an entry for each pair of neighbours and none else; a pair that went takes its answers and its
    /// queued jobs with it (a running job of it is dropped when its answer comes: onAnswer).
    void MotionTimeline::syncPairs()
    {
        std::map<PairId, PairCache> next;
        SegMethod inherited = SegMethod::Generate;
        if (mKeys.size() >= 2) // the first pair, when new, takes the method of the pair that follows it
        {
            const auto after = mPairs.find(PairId{ mKeys[1].id, mKeys.size() > 2 ? mKeys[2].id : NO_KEY });
            if (after != mPairs.end())
            {
                inherited = after->second.method;
            }
        }
        for (size_t i = 0; i + 1 < mKeys.size(); ++i)
        {
            const PairId pair{ mKeys[i].id, mKeys[i + 1].id };
            auto         it = mPairs.find(pair);
            if (it == mPairs.end())
            {
                // a pair that replaces one (a key put in between, one taken out) takes the method of the one that started
                // at this key, else of the one that ended at the next key
                const PairCache* from_here = nullptr;
                const PairCache* to_there  = nullptr;
                for (const auto& old : mPairs)
                {
                    from_here = old.first.from == pair.from ? &old.second : from_here;
                    to_there  = old.first.to == pair.to ? &old.second : to_there;
                }
                if (from_here || to_there)
                {
                    inherited = (from_here ? from_here : to_there)->method;
                }
            }
            PairCache c = it != mPairs.end() ? std::move(it->second) : newPair(inherited, mKeys[i].pose, mKeys[i + 1].pose);
            inherited   = c.method;
            next.emplace(pair, std::move(c));
        }
        mPairs = std::move(next);
        mQueue.erase(std::remove_if(mQueue.begin(), mQueue.end(), [&](const Queued& q) { return !mPairs.count(q.pair); }),
                     mQueue.end());
    }

    /// The views of the table: segments, key times, the length of the whole. Cheap (a handful of pairs), called at the
    /// end of every change.
    void MotionTimeline::updateView()
    {
        const size_t n = mKeys.size();
        mKeyFrames.assign(n, 0);
        mSegments.clear();
        S32 pos = 0;
        for (size_t i = 0; i < n; ++i)
        {
            mKeyFrames[i] = pos;
            const S32 hold = holdFramesOf(mKeys[i].hold_sec);
            if (i + 1 < n)
            {
                const PairId pair{ mKeys[i].id, mKeys[i + 1].id };
                const auto   it = mPairs.find(pair);
                if (it != mPairs.end())
                {
                    const PairCache& c = it->second;
                    SegmentInfo      s;
                    s.pair          = pair;
                    s.from_index    = (S32)i;
                    s.method        = c.method;
                    s.state         = c.state;
                    s.fail          = c.fail;
                    s.start_frame   = pos + hold;
                    s.frames        = segmentFrames(c) - 1;
                    s.answer_count  = (S32)c.answers.size();
                    s.selected      = c.selected;
                    s.tokens        = c.tokens;
                    s.seconds       = (F32)s.frames / (F32)CLIP_FPS;
                    s.suggest_regen = c.suggest_regen;
                    s.ctx_stale     = c.ctx_stale;
                    s.regen_reasons = c.regen_reasons;
                    s.same_poses    = samePair((S32)i);
                    mSegments.push_back(s);
                    pos += hold + s.frames;
                }
            }
            else
            {
                pos += hold + 1; // the last key: its frame and its hold
            }
        }
        mTotalFrames = n == 0 ? 0 : pos;
        if (n < 2)
        {
            mLoopHold = LOOP_HOLD_SEC;
            mLoopFirstId = mLoopLastId = NO_KEY;
        }
        else if (mKeys.front().id != mLoopFirstId || mKeys.back().id != mLoopLastId ||
                 mKeys.front().pose_ver != mLoopFirstVer || mKeys.back().pose_ver != mLoopLastVer)
        {
            mLoopFirstId  = mKeys.front().id;
            mLoopLastId   = mKeys.back().id;
            mLoopFirstVer = mKeys.front().pose_ver;
            mLoopLastVer  = mKeys.back().pose_ver;
            mLoopHold = posesWithin(mKeys.front().pose, mKeys.back().pose, LOOP_SAME_DEG, LOOP_SAME_M) ? 0.0 : LOOP_HOLD_SEC;
        }
    }

    // ---- segment settings -------------------------------------------------------------------------------------

    TimelineResult MotionTimeline::setMethod(PairId pair, SegMethod method)
    {
        PairCache* c = cacheMutable(pair);
        if (!c)
        {
            return TimelineResult::NoSuchPair;
        }
        if (c->state == SegState::Queued || c->state == SegState::Running)
        {
            return TimelineResult::Busy;
        }
        if (method == c->method)
        {
            return TimelineResult::Ok; // nothing changes (the answers are not chosen again, the length stays)
        }
        if (method == SegMethod::Generate && samePair(indexOf(pair.from)))
        {
            return TimelineResult::SamePoses;
        }
        const Settings before = saved(*c);
        c->method = method;
        c->fail   = JobError::None;
        if (method == SegMethod::Generate && c->selected < 0 && !c->answers.empty())
        {
            selectIndex(*c, 0);
        }
        c->state = (method == SegMethod::Generate && c->selected < 0) ? SegState::Unmade : SegState::Made;
        return commitOrUndo([&] { restore(*c, before); });
    }

    TimelineResult MotionTimeline::setSeconds(PairId pair, F32 seconds)
    {
        PairCache* c = cacheMutable(pair);
        if (!c)
        {
            return TimelineResult::NoSuchPair;
        }
        if (c->method == SegMethod::Generate || !std::isfinite(seconds))
        {
            return TimelineResult::BadValue;
        }
        const F32 old = c->seconds;
        c->seconds    = std::clamp(seconds, SEGMENT_MIN_SEC, SEGMENT_MAX_SEC);
        return commitOrUndo([&] { c->seconds = old; });
    }

    TimelineResult MotionTimeline::setTokens(PairId pair, S32 tokens)
    {
        PairCache* c = cacheMutable(pair);
        if (!c)
        {
            return TimelineResult::NoSuchPair;
        }
        if (c->method != SegMethod::Generate || (tokens != TIMELINE_TOKENS_AUTO && timelineFramesOfTokens(tokens) == 0))
        {
            return TimelineResult::BadValue;
        }
        if (c->state == SegState::Queued || c->state == SegState::Running)
        {
            return TimelineResult::Busy;
        }
        const Settings before = saved(*c);
        c->tokens   = tokens;
        c->selected = -1;
        for (size_t i = 0; i < c->answers.size(); ++i)
        {
            if (c->answers[i].tokens == tokensOf(tokens))
            {
                c->selected = (S32)i;
            }
        }
        c->state = c->selected >= 0 ? SegState::Made : SegState::Unmade;
        c->fail  = JobError::None;
        return commitOrUndo([&] { restore(*c, before); });
    }

    TimelineResult MotionTimeline::selectAnswer(PairId pair, S32 index)
    {
        PairCache* c = cacheMutable(pair);
        if (!c)
        {
            return TimelineResult::NoSuchPair;
        }
        if (index < 0 || index >= (S32)c->answers.size())
        {
            return TimelineResult::BadValue;
        }
        if (c->state == SegState::Queued || c->state == SegState::Running)
        {
            return TimelineResult::Busy;
        }
        const Settings before = saved(*c);
        selectIndex(*c, index);
        if (c->state == SegState::Unmade || c->state == SegState::Failed)
        {
            c->state = SegState::Made;
            c->fail  = JobError::None;
        }
        return commitOrUndo([&] { restore(*c, before); });
    }

    // ---- generation -------------------------------------------------------------------------------------------

    TimelineResult MotionTimeline::requestGenerate(PairId pair)
    {
        const TimelineResult r = queueOne(pair);
        if (r == TimelineResult::Ok)
        {
            updateView();
        }
        return r;
    }

    bool MotionTimeline::prevAvailable(S32 from_index) const
    {
        if (from_index <= 0 || from_index >= (S32)mKeys.size() || !mKeys[(size_t)from_index].pass_effective)
        {
            return true;
        }
        const PairCache* before = cache(PairId{ mKeys[(size_t)from_index - 1].id, mKeys[(size_t)from_index].id });
        return answerInUse(before) || (before && (before->state == SegState::Queued || before->state == SegState::Running));
    }

    TimelineResult MotionTimeline::canQueue(PairId pair, bool prev_queued) const
    {
        const PairCache* c = cache(pair);
        if (!c)
        {
            return TimelineResult::NoSuchPair;
        }
        if (c->method != SegMethod::Generate)
        {
            return TimelineResult::BadValue;
        }
        if (c->state == SegState::Queued || c->state == SegState::Running)
        {
            return TimelineResult::Busy;
        }
        const SegmentInfo* info = findSegment(pair);
        if (info && info->same_poses)
        {
            return TimelineResult::SamePoses;
        }
        return (prev_queued || prevAvailable(indexOf(pair.from))) ? TimelineResult::Ok : TimelineResult::NeedPrev;
    }

    TimelineResult MotionTimeline::queueOne(PairId pair)
    {
        const TimelineResult r = canQueue(pair, false);
        if (r != TimelineResult::Ok)
        {
            return r;
        }
        PairCache* c = cacheMutable(pair);
        c->state     = SegState::Queued;
        c->fail      = JobError::None;
        mQueue.push_back(Queued{ pair, c->tokens, false });
        return TimelineResult::Ok;
    }

    S32 MotionTimeline::chainLast(S32 from_index) const
    {
        S32 last = from_index;
        // the next segment is asked with this one's end when its from key (this one's to key) is passed
        while (last + 2 < (S32)mKeys.size() && mKeys[(size_t)last + 1].pass_effective)
        {
            ++last;
        }
        return last;
    }

    std::vector<PairId> MotionTimeline::chainFrom(PairId pair) const
    {
        std::vector<PairId> chain;
        const S32           first = indexOf(pair.from);
        if (first < 0 || first + 1 >= (S32)mKeys.size() || mKeys[(size_t)first + 1].id != pair.to)
        {
            return chain;
        }
        for (S32 j = first; j <= chainLast(first); ++j)
        {
            chain.push_back(PairId{ mKeys[(size_t)j].id, mKeys[(size_t)j + 1].id });
        }
        return chain;
    }

    TimelineResult MotionTimeline::canRequestChain(PairId pair) const
    {
        const S32 first = indexOf(pair.from);
        if (first < 0 || first + 1 >= (S32)mKeys.size() || mKeys[(size_t)first + 1].id != pair.to)
        {
            return TimelineResult::NoSuchPair;
        }
        // every segment with the rules of requestGenerate; the ones after the first are asked with the one queued before them
        const S32 last = chainLast(first);
        for (S32 j = first; j <= last; ++j)
        {
            const TimelineResult r = canQueue(PairId{ mKeys[(size_t)j].id, mKeys[(size_t)j + 1].id }, j > first);
            if (r != TimelineResult::Ok)
            {
                return r;
            }
        }
        return TimelineResult::Ok;
    }

    TimelineResult MotionTimeline::requestChain(PairId pair, S32* out_count)
    {
        if (out_count)
        {
            *out_count = 0;
        }
        // all or nothing: every segment is looked at first
        const TimelineResult can = canRequestChain(pair);
        if (can != TimelineResult::Ok)
        {
            return can;
        }
        const std::vector<PairId> chain = chainFrom(pair);
        S32 queued = 0;
        for (const PairId& p : chain)
        {
            const TimelineResult r = queueOne(p);
            if (r != TimelineResult::Ok)
            {
                updateView();
                return r;
            }
            ++queued;
        }
        updateView();
        if (out_count)
        {
            *out_count = queued;
        }
        return TimelineResult::Ok;
    }

    S32 MotionTimeline::requestAll()
    {
        S32 count = 0;
        for (size_t i = 0; i + 1 < mKeys.size(); ++i)
        {
            const PairId pair{ mKeys[i].id, mKeys[i + 1].id };
            const auto   it = mPairs.find(pair);
            if (it == mPairs.end() || it->second.method != SegMethod::Generate ||
                (it->second.state != SegState::Unmade && it->second.state != SegState::Failed))
            {
                continue;
            }
            if (queueOne(pair) == TimelineResult::Ok)
            {
                ++count;
            }
        }
        updateView();
        return count;
    }

    S32 MotionTimeline::moreTokens(const PairCache& c, S32 (&out)[2])
    {
        if (c.method != SegMethod::Generate || c.state != SegState::Made || c.selected < 0 || c.selected >= (S32)c.answers.size())
        {
            return 0;
        }
        const S32 base  = c.answers[(size_t)c.selected].tokens;
        S32       count = 0;
        for (S32 t : { base - 2, base + 2 })
        {
            if (timelineFramesOfTokens(t) == 0 || (S32)c.answers.size() + count >= TIMELINE_MAX_ANSWERS)
            {
                continue;
            }
            const bool have = std::any_of(c.answers.begin(), c.answers.end(), [&](const SegmentAnswer& a) { return a.tokens == t; });
            if (!have)
            {
                out[count++] = t;
            }
        }
        return count;
    }

    bool MotionTimeline::canRequestMoreAnswers(PairId pair) const
    {
        const PairCache* c = cache(pair);
        S32 tokens[2] = { 0, 0 };
        return c && prevAvailable(indexOf(pair.from)) && moreTokens(*c, tokens) > 0;
    }

    S32 MotionTimeline::requestMoreAnswers(PairId pair)
    {
        PairCache* c = cacheMutable(pair);
        S32 tokens[2] = { 0, 0 };
        const S32 count = c && prevAvailable(indexOf(pair.from)) ? moreTokens(*c, tokens) : 0;
        for (S32 i = 0; i < count; ++i)
        {
            mQueue.push_back(Queued{ pair, tokens[i], true });
        }
        if (count > 0)
        {
            c->state = SegState::Queued;
            c->fail  = JobError::None;
            updateView();
        }
        return count;
    }

    /// A segment whose job stopped without an answer goes back: S0 with no answer, else S3.
    void MotionTimeline::restoreAfterStop(PairCache& c)
    {
        c.state = (c.method == SegMethod::Generate && c.selected < 0) ? SegState::Unmade : SegState::Made; // S0: no answer in use
        c.fail  = JobError::None;
    }

    /// The queue is emptied and each segment in it goes back (S0 / S3).
    void MotionTimeline::restoreQueued()
    {
        for (const Queued& q : mQueue)
        {
            if (PairCache* c = cacheMutable(q.pair))
            {
                restoreAfterStop(*c);
            }
        }
        mQueue.clear();
    }

    /// The segment before key `index` has an answer in use, and its clip is made of it (so the end of it can be the context).
    bool MotionTimeline::prevUsable(S32 index) const
    {
        if (index <= 0 || index >= (S32)mKeys.size())
        {
            return false;
        }
        const PairCache* c = cache(PairId{ mKeys[(size_t)index - 1].id, mKeys[(size_t)index].id });
        return answerInUse(c) && c->fitted && c->fit_answer == c->selected;
    }

    /// research/361 section 3.1, the table of what nextJob does with a queued segment whose from key is `from_index`:
    /// from key not passed: Ready (the same as before). Passed: the segment before it is queued (and in the queue)
    /// -> Waits; it has an answer in use and is not queued -> Ready; else Missing.
    MotionTimeline::Wait MotionTimeline::waitFor(S32 from_index) const
    {
        if (from_index <= 0 || !mKeys[(size_t)from_index].pass_effective)
        {
            return Wait::Ready;
        }
        const PairId     before{ mKeys[(size_t)from_index - 1].id, mKeys[(size_t)from_index].id };
        const PairCache* c = cache(before);
        if (!c)
        {
            return Wait::Missing;
        }
        if (c->state == SegState::Queued && hasQueued(before))
        {
            return Wait::Waits;
        }
        return prevUsable(from_index) ? Wait::Ready : Wait::Missing;
    }

    void MotionTimeline::dropChain(S32 from_index)
    {
        for (S32 j = from_index; j <= chainLast(from_index); ++j)
        {
            const PairId pair{ mKeys[(size_t)j].id, mKeys[(size_t)j + 1].id };
            eraseQueued(pair);
            if (PairCache* c = cacheMutable(pair); c && c->state == SegState::Queued)
            {
                restoreAfterStop(*c);
                c->fail = JobError::PrevMissing;
            }
        }
    }

    bool MotionTimeline::composeJob(const SomaRetarget& retarget, S32 from_index, GenerateJob& job, SentRecord& sent) const
    {
        const TimelineKey& from = mKeys[(size_t)from_index];
        const TimelineKey& to   = mKeys[(size_t)from_index + 1];
        RetargetWork       work;
        SentRecord         rec;
        rec.made = true;

        // the context: the end of the segment before (from key passed), else the from key itself, four times
        if (from.pass_effective)
        {
            const PairCache* before = cache(PairId{ mKeys[(size_t)from_index - 1].id, from.id });
            std::vector<SlPose> tail;
            if (!answerInUse(before) || !tailPoses(before->clip, TIMELINE_CONTEXT_FRAMES, tail))
            {
                return false;
            }
            for (S32 k = 0; k < TIMELINE_CONTEXT_FRAMES; ++k)
            {
                if (retarget.slToSoma(tail[(size_t)k], job.context[k], work) != RetargetResult::Ok)
                {
                    return false;
                }
            }
            rec.ctx_moving = true;
            rec.ctx_serial = before->answers[(size_t)before->selected].serial;
            rec.ctx_last2.assign(tail.end() - 2, tail.end());
        }
        else
        {
            if (retarget.slToSoma(mKeyPoses[(size_t)from_index].sl, job.context[0], work) != RetargetResult::Ok)
            {
                return false;
            }
            for (S32 k = 1; k < TIMELINE_CONTEXT_FRAMES; ++k)
            {
                job.context[k] = job.context[0];
            }
        }

        // the targets: the to key, and (passed) three steps of its strength on the way to the key after it
        if (retarget.slToSoma(mKeyPoses[(size_t)from_index + 1].sl, job.targets[0], work) != RetargetResult::Ok)
        {
            return false;
        }
        job.ntargets = 1;
        if (to.pass_effective)
        {
            const F32 d = passStrengthD(to.strength);
            KeyPose   step;
            for (S32 k = 1; k < TIMELINE_MAX_TARGETS; ++k)
            {
                if (blendKeyPose(mKeyPoses[(size_t)from_index + 1], mKeyPoses[(size_t)from_index + 2], (F64)d * k, step) !=
                        RetargetResult::Ok ||
                    retarget.slToSoma(step.sl, job.targets[k], work) != RetargetResult::Ok)
                {
                    return false;
                }
            }
            job.ntargets    = TIMELINE_MAX_TARGETS;
            rec.to_through  = true;
            rec.to_strength = to.strength;
            rec.to_ver      = to.pose_ver;
            rec.next_ver    = mKeys[(size_t)from_index + 2].pose_ver;
            rec.next_id     = mKeys[(size_t)from_index + 2].id;
            rec.to_pose     = mKeyPoses[(size_t)from_index + 1];
            rec.next_pose   = mKeyPoses[(size_t)from_index + 2];
        }
        sent = std::move(rec);
        return true;
    }

    std::optional<GenerateJob> MotionTimeline::nextJob(const SomaRetarget& retarget)
    {
        if (mRunning.active || mQueue.empty())
        {
            return std::nullopt;
        }
        if (!retarget.ready())
        {
            restoreQueued(); // nothing was tried: back to S0 / S3
            updateView();
            return std::nullopt;
        }
        if ((mStale || mKeysChanged) && refresh(retarget) != TimelineResult::Ok)
        {
            // the keys cannot be made into a layout: what is queued fails (a segment that has answers stays S3)
            const std::vector<Queued> failed = std::move(mQueue);
            mQueue.clear();
            for (const Queued& q : failed)
            {
                if (PairCache* c = cacheMutable(q.pair))
                {
                    settleJob(q.pair, *c, JobError::BadInput);
                }
            }
            updateView();
            return std::nullopt;
        }
        // Every pass over the queue returns a job, takes an entry out, or only skips entries that wait for another queued
        // one (which is earlier in the key list, so the first of them cannot wait): it ends.
        for (size_t qi = 0; qi < mQueue.size();)
        {
            const Queued q = mQueue[qi];
            PairCache*   c = cacheMutable(q.pair);
            const S32    i = indexOf(q.pair.from);
            if (!c || i < 0 || c->method != SegMethod::Generate || c->state != SegState::Queued)
            {
                mQueue.erase(mQueue.begin() + (std::ptrdiff_t)qi);
                continue;
            }
            const Wait wait = waitFor(i);
            if (wait == Wait::Waits)
            {
                ++qi; // the segment before it is queued: its turn comes first (this one stays in the queue)
                continue;
            }
            if (wait == Wait::Missing)
            {
                dropChain(i); // no answer to go on from: back to S0 / S3 (never a stop that was not asked for)
                qi = 0;
                continue;
            }
            mQueue.erase(mQueue.begin() + (std::ptrdiff_t)qi);
            GenerateJob job;
            job.pair   = q.pair;
            job.tokens = tokensOf(q.tokens);
            job.extra  = q.extra;
            SentRecord sent;
            if (!composeJob(retarget, i, job, sent))
            {
                // the poses cannot be sent: the rest of this segment's jobs would fail the same way
                eraseQueued(q.pair);
                settleJob(q.pair, *c, JobError::BadInput);
                qi = 0;
                continue;
            }
            mRunning.active     = true;
            mRunning.pair       = q.pair;
            mRunning.tokens     = job.tokens;
            mRunning.extra      = q.extra;
            mRunning.from_ver   = mKeys[(size_t)i].pose_ver;
            mRunning.to_ver     = mKeys[(size_t)i + 1].pose_ver;
            mRunning.sent_start = mKeys[(size_t)i].pose;
            mRunning.sent_end   = mKeys[(size_t)i + 1].pose;
            mRunning.sent       = std::move(sent);
            c->state            = SegState::Running;
            updateView();
            return job;
        }
        if (!mQueue.empty())
        {
            restoreQueued(); // only waiting ones left, none of whose predecessors is queued: cannot be (kept as a guard)
        }
        updateView();
        return std::nullopt;
    }

    /// A job of this segment ended in a failure of its own: the segment stays S3 when an answer of it is in use (a failed
    /// new answer, a failed regeneration: the failure stays in `fail` as a note until the next request), else S5.
    void MotionTimeline::settleJob(PairId pair, PairCache& c, JobError error)
    {
        if (c.selected >= 0)
        {
            c.state = hasQueued(pair) ? SegState::Queued : SegState::Made;
        }
        else
        {
            c.state = SegState::Failed;
        }
        c.fail = error;
        updateView();
    }

    AnswerFate MotionTimeline::onAnswer(const SomaRetarget& retarget, std::vector<SomaFrame>&& frames)
    {
        if (!mRunning.active)
        {
            return AnswerFate::Dropped;
        }
        Running run  = std::move(mRunning);
        mRunning     = Running{};
        PairCache* c = cacheMutable(run.pair);
        if (!c)
        {
            updateView();
            return AnswerFate::Dropped; // its pair went while it was out
        }
        const S32 tokens = ((S32)frames.size() + 6) / 4;
        if (timelineFramesOfTokens(tokens) != (S32)frames.size() || tokens != run.tokens)
        {
            settleJob(run.pair, *c, JobError::BadAnswer);
            return AnswerFate::CannotBuild;
        }

        // what the model's ends were from the poses it was given (the yardstick of "regenerating is advised"): measured
        // in the layout of all keys when the keys are as they were sent (the refit is measured the same way), else
        // in a layout of the two poses it was sent
        SegmentAnswer answer;
        answer.tokens     = tokens;
        answer.sent_start = run.sent_start;
        answer.sent_end   = run.sent_end;
        {
            const S32  i = indexOf(run.pair.from);
            MotionClip scratch;
            bool       measured = false;
            if (i >= 0 && refresh(retarget) == TimelineResult::Ok && mKeys[(size_t)i].pose_ver == run.from_ver &&
                mKeys[(size_t)i + 1].pose_ver == run.to_ver)
            {
                measured = buildClip(retarget, mKeyPoses[(size_t)i], mKeyPoses[(size_t)i + 1], frames, scratch,
                                     &answer.made_stats) == RetargetResult::Ok;
            }
            else
            {
                TrackLayout          layout;
                std::vector<KeyPose> sent;
                measured = makeKeyPoses(retarget, std::vector<const history::Pose*>{ &answer.sent_start, &answer.sent_end },
                                        layout, sent) == TrackResult::Ok &&
                           buildClip(retarget, sent[0], sent[1], frames, scratch, &answer.made_stats) == RetargetResult::Ok;
            }
            if (!measured)
            {
                settleJob(run.pair, *c, JobError::CannotBuild);
                return AnswerFate::CannotBuild;
            }
        }

        // the same length again replaces; else it is added (a full segment replaces the selected one)
        S32 slot = -1;
        for (size_t i = 0; i < c->answers.size(); ++i)
        {
            if (c->answers[i].tokens == tokens)
            {
                slot = (S32)i;
            }
        }
        if (slot < 0)
        {
            slot = (S32)c->answers.size() < TIMELINE_MAX_ANSWERS ? (S32)c->answers.size() : std::max(c->selected, 0);
        }
        const bool takes = !run.extra || c->selected < 0 || slot == c->selected; // it becomes the one used
        if (takes && mTotalFrames - segmentFrames(*c) + timelineFramesOfTokens(tokens) > TIMELINE_MAX_FRAMES)
        {
            // longer than the plan and it does not fit: nothing is changed (the answers it has stay, none is lost)
            settleJob(run.pair, *c, JobError::CannotBuild);
            return AnswerFate::CannotBuild;
        }
        answer.frames = std::move(frames);
        answer.serial = mNextSerial++;
        answer.used   = std::move(run.sent);
        if (slot == (S32)c->answers.size())
        {
            c->answers.push_back(std::move(answer));
        }
        else
        {
            c->answers[(size_t)slot] = std::move(answer);
        }
        if (takes)
        {
            selectIndex(*c, slot);
            c->fitted = false;
        }
        c->fail  = JobError::None;
        c->state = hasQueued(run.pair) ? SegState::Queued : SegState::Made;
        updateView();
        markStale();
        refresh(retarget);
        return c->state == SegState::Failed ? AnswerFate::CannotBuild : AnswerFate::Used;
    }

    bool MotionTimeline::onFailure(JobError error)
    {
        const bool whole = isWholeFailure(error);
        const Running run = mRunning;
        mRunning          = Running{};
        if (whole)
        {
            if (run.active)
            {
                if (PairCache* c = cacheMutable(run.pair))
                {
                    restoreAfterStop(*c);
                }
            }
            restoreQueued();
        }
        else if (run.active)
        {
            if (PairCache* c = cacheMutable(run.pair))
            {
                settleJob(run.pair, *c, error);
            }
        }
        updateView();
        return whole;
    }

    bool MotionTimeline::cancelGeneration()
    {
        const bool was_running = mRunning.active;
        if (mRunning.active)
        {
            if (PairCache* c = cacheMutable(mRunning.pair))
            {
                restoreAfterStop(*c);
            }
        }
        mRunning = Running{};
        restoreQueued();
        updateView();
        return was_running;
    }

    // ---- the joined clip --------------------------------------------------------------------------------------

    /// Makes the segment's clip for the keys as they are: the selected answer refitted to them (the model's body
    /// motion, the ends moved onto the keys), else the interpolation. A refit that fails drops that answer (the segment
    /// is Failed, no answer is used, and it holds the interpolation of its asked length); false only when not even that
    /// can be made.
    bool MotionTimeline::fitPair(const SomaRetarget& retarget, const std::vector<KeyPose>& keys, U32 layout_version, S32 index,
                                 PairCache& c)
    {
        const KeyPose& a = keys[(size_t)index];
        const KeyPose& b = keys[(size_t)index + 1];
        c.suggest_regen = false;
        c.fit_stats     = ClipStats{};
        bool made       = false;
        if (c.method == SegMethod::Generate && c.selected >= 0)
        {
            const SegmentAnswer& ans = c.answers[(size_t)c.selected];
            if (buildClip(retarget, a, b, ans.frames, c.clip, &c.fit_stats) == RetargetResult::Ok)
            {
                made = true;
                c.suggest_regen = c.fit_stats.start_max_deg > ans.made_stats.start_max_deg + REGEN_DEG ||
                                  c.fit_stats.end_max_deg > ans.made_stats.end_max_deg + REGEN_DEG ||
                                  c.fit_stats.start_pelvis_m > ans.made_stats.start_pelvis_m + REGEN_M ||
                                  c.fit_stats.end_pelvis_m > ans.made_stats.end_pelvis_m + REGEN_M ||
                                  c.fit_stats.start_pelvis_m >= REGEN_PELVIS_JUMP_M || c.fit_stats.end_pelvis_m >= REGEN_PELVIS_JUMP_M;
            }
            else
            {
                c.answers.erase(c.answers.begin() + c.selected);
                c.selected = -1; // none is used until the user picks one (its length is not the one asked for)
                c.state    = SegState::Failed;
                c.fail     = JobError::CannotBuild;
            }
        }
        if (!made)
        {
            const InterpCurve curve = c.method == SegMethod::Linear ? InterpCurve::Linear : InterpCurve::EaseInOut;
            if (makeInterpClip(a, b, segmentFrames(c), curve, c.clip) != RetargetResult::Ok)
            {
                c.fitted = false;
                return false;
            }
        }
        c.fitted       = true;
        c.fit_method   = c.method;
        c.fit_answer   = c.selected;
        c.fit_frames   = segmentFrames(c);
        c.fit_layout   = layout_version;
        c.fit_from_ver = mKeys[(size_t)index].pose_ver;
        c.fit_to_ver   = mKeys[(size_t)index + 1].pose_ver;
        return true;
    }

    /// Everything is made in locals and taken over together at the end: a refresh that fails leaves the layout, the key
    /// poses and the clip as they were (every segment is made again next time).
    TimelineResult MotionTimeline::refresh(const SomaRetarget& retarget)
    {
        if (!retarget.ready())
        {
            return TimelineResult::NotReady;
        }
        if (!mStale && !mKeysChanged)
        {
            return TimelineResult::Ok;
        }
        if (mKeys.size() < 2)
        {
            mClip = MotionClip{};
            mKeyPoses.clear();
            for (TimelineKey& k : mKeys)
            {
                k.pass_effective = k.pass_auto = false;
            }
            if (!mLayout.joints.empty())
            {
                ++mLayoutVersion; // the joints changed (to none)
            }
            mLayout.clear();
            ++mClipVersion;
            mKeysChanged = false;
            mStale       = false;
            return TimelineResult::NeedsKeys;
        }
        const bool           rebuild = mKeysChanged;
        TrackLayout          layout;
        std::vector<KeyPose> keys;
        U32                  version = mLayoutVersion;
        if (rebuild)
        {
            std::vector<const history::Pose*> poses;
            poses.reserve(mKeys.size());
            for (const TimelineKey& k : mKeys)
            {
                poses.push_back(&k.pose);
            }
            mTrackResult = makeKeyPoses(retarget, poses, layout, keys);
            if (mTrackResult != TrackResult::Ok)
            {
                return TimelineResult::BuildFailed;
            }
            if (!layoutsEqual(layout, mLayout))
            {
                ++version;
            }
        }
        const std::vector<KeyPose>& use = rebuild ? keys : mKeyPoses;

        auto failed = [this]
        {
            for (auto& entry : mPairs)
            {
                entry.second.fitted = false; // what was made is not trusted: all again next time
            }
            return TimelineResult::BuildFailed;
        };
        std::vector<const MotionClip*> clips;
        clips.reserve(mKeys.size() - 1);
        for (size_t i = 0; i + 1 < mKeys.size(); ++i)
        {
            PairCache& c = mPairs.at(PairId{ mKeys[i].id, mKeys[i + 1].id });
            const bool stale = !c.fitted || c.fit_from_ver != mKeys[i].pose_ver || c.fit_to_ver != mKeys[i + 1].pose_ver ||
                               c.fit_layout != version || c.fit_method != c.method || c.fit_answer != c.selected ||
                               c.fit_frames != segmentFrames(c);
            if (stale && !fitPair(retarget, use, version, (S32)i, c))
            {
                return failed();
            }
            clips.push_back(&c.clip);
        }
        updateView(); // a failed answer may have changed a segment's length
        std::vector<S32> holds(mKeys.size());
        for (size_t k = 0; k < mKeys.size(); ++k)
        {
            holds[k] = holdFramesOf(mKeys[k].hold_sec);
        }
        MotionClip joined;
        if (concatClips(clips, holds, joined) != ConcatResult::Ok || joined.frame_count != mTotalFrames)
        {
            return failed();
        }
        if (rebuild)
        {
            mLayout        = std::move(layout);
            mKeyPoses      = std::move(keys);
            mLayoutVersion = version;
            mKeysChanged   = false;
        }
        mClip = std::move(joined);
        ++mClipVersion;
        mStale = false;
        updatePassFlags(retarget);
        updateRegenMarks();
        return TimelineResult::Ok;
    }

    /// pass_effective and pass_auto of every key. The automatic judgement is worked out again only when the keys around it
    /// (their ids and pose versions), the layout or the skeleton changed (the memo in the key).
    void MotionTimeline::updatePassFlags(const SomaRetarget& retarget)
    {
        const S32 n = (S32)mKeys.size();
        for (S32 i = 0; i < n; ++i)
        {
            TimelineKey& k   = mKeys[(size_t)i];
            k.pass_auto      = false;
            k.pass_effective = false;
            if (!mPassEnabled || eligibilityAt(i) != PassEligibility::Ok)
            {
                continue;
            }
            if (k.pass == PassMode::Through)
            {
                k.pass_effective = true;
                continue;
            }
            const TimelineKey &a = mKeys[(size_t)i - 1], &c = mKeys[(size_t)i + 1];
            TimelineKey::AutoMemo& m = k.auto_memo;
            if (!(m.known && m.prev == a.id && m.next == c.id && m.ver_prev == a.pose_ver && m.ver_self == k.pose_ver &&
                  m.ver_next == c.pose_ver && m.layout == mLayoutVersion && m.skeleton == mSkeletonGen))
            {
                SomaPose    pa, pb, pc;
                RetargetWork work;
                bool        through = false;
                if (retarget.slToSoma(mKeyPoses[(size_t)i - 1].sl, pa, work) == RetargetResult::Ok &&
                    retarget.slToSoma(mKeyPoses[(size_t)i].sl, pb, work) == RetargetResult::Ok &&
                    retarget.slToSoma(mKeyPoses[(size_t)i + 1].sl, pc, work) == RetargetResult::Ok)
                {
                    LLVector3 qa[PASS_POINT_COUNT], qb[PASS_POINT_COUNT], qc[PASS_POINT_COUNT];
                    passPoints(pa, qa);
                    passPoints(pb, qb);
                    passPoints(pc, qc);
                    through = judgeAutoPass(qa, qb, qc).through;
                }
                m = TimelineKey::AutoMemo{ true, a.id, c.id, a.pose_ver, k.pose_ver, c.pose_ver, mLayoutVersion, mSkeletonGen, through };
            }
            k.pass_auto      = m.through;
            k.pass_effective = m.through;
        }
    }

    /// The marks of every segment, from what its selected answer was asked with and the keys and answers as they are now
    /// (research/361 section 2.1): a value worked out again by every successful refresh, not a state that comes and goes.
    void MotionTimeline::updateRegenMarks()
    {
        for (size_t i = 0; i + 1 < mKeys.size(); ++i)
        {
            PairCache* c = cacheMutable(PairId{ mKeys[i].id, mKeys[i + 1].id });
            if (!c)
            {
                continue;
            }
            U8 reasons = c->suggest_regen ? REGEN_ENDS : 0;
            if (c->method == SegMethod::Generate && c->selected >= 0 && c->selected < (S32)c->answers.size() &&
                c->answers[(size_t)c->selected].used.made)
            {
                const SentRecord& u        = c->answers[(size_t)c->selected].used;
                const bool        from_now = mKeys[i].pass_effective, to_now = mKeys[i + 1].pass_effective;
                if (u.ctx_moving != from_now || u.to_through != to_now)
                {
                    reasons |= REGEN_PASS; // passed or stopped at since it was asked (a key that cannot be passed any more included)
                }
                if (u.to_through && to_now)
                {
                    if (u.to_strength != mKeys[i + 1].strength)
                    {
                        reasons |= REGEN_PASS;
                    }
                    if (u.next_id != mKeys[i + 2].id || // another key follows now (its version may be the same number)
                        keyPoseMoved(u.to_pose, u.to_ver, mKeyPoses[i + 1], mKeys[i + 1].pose_ver) ||
                        keyPoseMoved(u.next_pose, u.next_ver, mKeyPoses[i + 2], mKeys[i + 2].pose_ver))
                    {
                        reasons |= REGEN_TARGETS;
                    }
                }
                if (u.ctx_moving && from_now)
                {
                    const PairCache* before = cache(PairId{ mKeys[i - 1].id, mKeys[i].id });
                    const bool       another = !answerInUse(before) || before->answers[(size_t)before->selected].serial != u.ctx_serial;
                    if (another || tailStepDiffers(u.ctx_last2, before->clip, TAIL_VEL_DEG, TAIL_VEL_PELVIS_M))
                    {
                        reasons |= REGEN_CONTEXT;
                    }
                }
            }
            c->regen_reasons = reasons;
            c->ctx_stale     = (reasons & (U8)~REGEN_ENDS) != 0;
        }
        updateView();
    }

    PassView passViewOf(const MotionTimeline& timeline, KeyId id)
    {
        PassView v;
        const TimelineKey* key = timeline.find(id);
        if (!key)
        {
            return v;
        }
        v.key      = key->id;
        v.mode     = key->pass;
        v.strength = key->strength;
        switch (timeline.passEligibility(id))
        {
        case PassEligibility::EndKey:               v.note = PassNote::EndKey; return v;
        case PassEligibility::NeighbourNotGenerate: v.note = PassNote::NeighbourNotGenerate; return v;
        case PassEligibility::Hold:                 v.note = PassNote::Hold; return v;
        case PassEligibility::SamePose:             v.note = PassNote::SamePose; return v;
        case PassEligibility::Ok:                   break;
        }
        v.mode_enabled = true;
        if (key->pass == PassMode::Auto && !timeline.stale())
        {
            v.judged = key->pass_auto ? 1 : 0;   // pass_auto is the last successful refresh's
        }
        v.strength_enabled = key->pass == PassMode::Through || v.judged == 1;
        v.note             = v.judged == 0 ? PassNote::PassHint : PassNote::StopHint;
        return v;
    }
} // namespace plus::posermotion
