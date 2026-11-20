/**
 * @file motiontimeline_test.cpp
 * @brief Stand-alone test of the timeline of "Motion Between Poses" (research/357 section 8): the key list and the
 * table of pairs, the segment states and the generate queue, concatClips and makeInterpClip, the refit and the
 * "regenerating is advised" mark, the playback's reload and the loop hold, and the limits. No viewer is run:
 * tools/motionbricks-eval/build_timeline_test.bat builds it from motiontimeline.cpp, motiontracks.cpp,
 * motionclip.cpp, motionplayback.cpp and somaretarget.cpp against golden/sl_rest_skeleton.json. A model's answer is
 * made up here (the two poses seen from SOMA, blended), because what is tested is the bookkeeping around it.
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

// selftest: tools/motionbricks-eval/build_timeline_test.bat

#include "linden_common.h"

#include "motionclient.h"
#include "motionplayback.h"
#include "motionsession.h"
#include "motiontimeline.h"

#include "llmath.h"
#include "m3math.h"

#include <boost/json/src.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "somatables.h"

using namespace plus::posermotion;

// the timeline's copies of the model's numbers are the model's
static_assert(TIMELINE_TOKENS_AUTO == MOTION_TOKENS_AUTO && TIMELINE_TOKENS_MIN == MOTION_TOKENS_MIN &&
              TIMELINE_TOKENS_MAX == MOTION_TOKENS_MAX && TIMELINE_TOKENS_DEFAULT == MOTION_TOKENS_DEFAULT);
static_assert(timelineFramesOfTokens(TIMELINE_TOKENS_MIN) == motionFramesOfTokens(MOTION_TOKENS_MIN) &&
              timelineFramesOfTokens(TIMELINE_TOKENS_MAX) == motionFramesOfTokens(MOTION_TOKENS_MAX) &&
              timelineFramesOfTokens(TIMELINE_TOKENS_MIN) == CLIP_MIN_FRAMES &&
              timelineFramesOfTokens(TIMELINE_TOKENS_MAX) == CLIP_MAX_FRAMES && timelineFramesOfTokens(5) == 0 &&
              timelineFramesOfTokens(17) == 0);
namespace json    = boost::json;
namespace history = plus::posermcp::history;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

// ---------------------------------------------------------------- set-up
static json::value readJson(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    boost::system::error_code ec;
    json::value v = json::parse(ss.str(), ec);
    if (!in || ec)
    {
        std::printf("FAIL cannot read %s\n", path.c_str());
        ++failures;
        return nullptr;
    }
    return v;
}

static LLVector3 vec3(const json::value& v)
{
    const json::array& a = v.as_array();
    return LLVector3((F32)a[0].to_number<double>(), (F32)a[1].to_number<double>(), (F32)a[2].to_number<double>());
}

static RestSkeleton restFromJson(const json::value& doc)
{
    RestSkeleton rest;
    for (const json::value& jv : doc.at("joints").as_array())
    {
        RestJoint j;
        j.name = std::string(jv.at("name").as_string());
        if (!jv.at("parent").is_null())
        {
            const std::string parent(jv.at("parent").as_string());
            for (S32 i = 0; i < (S32)rest.joints.size(); ++i)
            {
                if (rest.joints[(size_t)i].name == parent)
                {
                    j.parent = i;
                }
            }
        }
        j.pos   = vec3(jv.at("pos"));
        j.scale = vec3(jv.at("scale"));
        rest.joints.push_back(j);
    }
    return rest;
}

static history::JointState joint(const std::string& name, const LLQuaternion& q, bool enabled = true)
{
    history::JointState s;
    s.name     = name;
    s.target   = history::Quat{ q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW] };
    s.own      = s.target;
    s.enabled  = enabled;
    s.rotation = true;
    return s;
}

/// A pose of the joints the model drives (and a pelvis): the elbow turned by elbow_deg, the pelvis moved x metres.
static history::Pose pose(F32 elbow_deg, F32 pelvis_x = 0.f, F32 knee_deg = 0.f)
{
    history::Pose p;
    p.joints.push_back(joint("mPelvis", LLQuaternion::DEFAULT));
    p.joints.back().position = history::Vec3{ pelvis_x, 0.f, 0.f };
    p.joints.push_back(joint("mTorso", LLQuaternion(0.1f, LLVector3::y_axis)));
    p.joints.push_back(joint("mShoulderLeft", LLQuaternion(0.2f, LLVector3::x_axis)));
    p.joints.push_back(joint("mElbowLeft", LLQuaternion(elbow_deg * DEG_TO_RAD, LLVector3::z_axis)));
    p.joints.push_back(joint("mHipLeft", LLQuaternion(0.15f, LLVector3::y_axis)));
    p.joints.push_back(joint("mKneeLeft", LLQuaternion(knee_deg * DEG_TO_RAD, LLVector3::y_axis)));
    return p;
}

static double angleBetween(const LLQuaternion& a, const LLQuaternion& b) { return rotationAngle(a, b) * RAD_TO_DEG; }

static double dotOf(const LLQuaternion& a, const LLQuaternion& b) { return dot(a, b); }

/// The model's answer, made up: the blend of the two poses seen from SOMA (so its ends are the keys').
static std::vector<SomaFrame> fakeAnswer(const SomaRetarget& retarget, const GenerateJob& job, F32 hips_shift = 0.f)
{
    SomaFrame a, b;
    SomaRetarget::localsOf(job.context[TIMELINE_CONTEXT_FRAMES - 1], a);
    SomaRetarget::localsOf(job.targets[0], b);
    const S32 n = timelineFramesOfTokens(job.tokens);
    std::vector<SomaFrame> frames((size_t)n);
    for (S32 f = 0; f < n; ++f)
    {
        const F32 t = (F32)f / (F32)(n - 1);
        SomaFrame& o = frames[(size_t)f];
        o.hips = a.hips + (b.hips - a.hips) * t + LLVector3(hips_shift * t, 0.f, 0.f);
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            o.local[(size_t)s] = slerp(t, a.local[(size_t)s], b.local[(size_t)s]);
            o.local[(size_t)s].normalize();
        }
    }
    (void)retarget;
    return frames;
}

static PairId pairOf(const MotionTimeline& tl, S32 i) { return PairId{ tl.key(i).id, tl.key(i + 1).id }; }

// ---------------------------------------------------------------- 1. keys, pairs, limits
static void testKeys()
{
    MotionTimeline tl;
    KeyId a = NO_KEY, b = NO_KEY, c = NO_KEY, d = NO_KEY;
    CHECK(tl.addKey(pose(0.f), &a) == TimelineResult::Ok && a != NO_KEY);
    CHECK(tl.segmentCount() == 0 && tl.totalFrames() == 1);
    CHECK(tl.addKey(pose(30.f), &b) == TimelineResult::Ok && tl.addKey(pose(60.f), &c) == TimelineResult::Ok);
    CHECK(tl.addKey(pose(90.f), &d) == TimelineResult::Ok);
    CHECK(a != b && b != c && c != d && tl.keyCount() == 4 && tl.segmentCount() == 3);
    CHECK(tl.segment(0).method == SegMethod::Generate && tl.segment(0).state == SegState::Unmade);

    // a pair keeps what was set on it when the neighbours around it change
    CHECK(tl.setMethod(PairId{ c, d }, SegMethod::Linear) == TimelineResult::Ok);
    CHECK(tl.setSeconds(PairId{ c, d }, 3.f) == TimelineResult::Ok);
    CHECK(tl.moveKey(a, 3) == TimelineResult::Ok); // b c d a
    CHECK(tl.key(0).id == b && tl.key(3).id == a && tl.segmentCount() == 3);
    const PairCache* cd = tl.cache(PairId{ c, d });
    CHECK(cd && cd->method == SegMethod::Linear && cd->seconds == 3.f);
    CHECK(!tl.cache(PairId{ a, b }) && tl.cache(PairId{ d, a }) && tl.cache(PairId{ b, c }));
    CHECK(tl.moveKey(a, 0) == TimelineResult::Ok && tl.cache(PairId{ a, b }) && !tl.cache(PairId{ d, a }));
    CHECK(tl.cache(PairId{ a, b })->method == SegMethod::Generate); // a new first pair: the method of the one after it (Generate here)

    // a deleted key's id is never given again; deleting joins the neighbours in a new pair
    CHECK(tl.removeKey(b) == TimelineResult::Ok && tl.keyCount() == 3 && tl.find(b) == nullptr);
    CHECK(tl.cache(PairId{ a, c }) && !tl.cache(PairId{ a, b }) && tl.cache(PairId{ c, d })->seconds == 3.f);
    KeyId e = NO_KEY;
    CHECK(tl.addKey(pose(10.f), &e) == TimelineResult::Ok && e != a && e != b && e != c && e != d);
    CHECK(tl.removeKey(9999) == TimelineResult::NoSuchKey && tl.moveKey(9999, 0) == TimelineResult::NoSuchKey);
    CHECK(tl.moveKey(a, 99) == TimelineResult::BadValue && tl.insertKey(99, pose(0.f)) == TimelineResult::BadValue);

    // a duplicate is right after its key, joined by a Smooth pair (the poses are equal)
    KeyId dup = NO_KEY;
    CHECK(tl.duplicateKey(c, &dup) == TimelineResult::Ok && tl.indexOf(dup) == tl.indexOf(c) + 1);
    const PairCache* cdup = tl.cache(PairId{ c, dup });
    CHECK(cdup && cdup->method == SegMethod::Smooth && cdup->state == SegState::Made);
    CHECK(tl.setMethod(PairId{ c, dup }, SegMethod::Generate) == TimelineResult::SamePoses);
    CHECK(tl.findSegment(PairId{ c, dup })->same_poses);

    // the cap on keys
    MotionTimeline full;
    for (S32 i = 0; i < TIMELINE_MAX_KEYS; ++i)
    {
        CHECK(full.addKey(pose((F32)i), nullptr) == TimelineResult::Ok);
    }
    CHECK(full.addKey(pose(5.f)) == TimelineResult::TooManyKeys && full.keyCount() == TIMELINE_MAX_KEYS);
    CHECK(full.duplicateKey(full.key(0).id) == TimelineResult::TooManyKeys);

    // a pair that inherits its method
    MotionTimeline tl2;
    tl2.addKey(pose(0.f));
    tl2.addKey(pose(10.f));
    tl2.setMethod(pairOf(tl2, 0), SegMethod::Smooth);
    tl2.addKey(pose(20.f));
    CHECK(tl2.segment(1).method == SegMethod::Smooth);

    // clearAll
    tl.clearAll();
    CHECK(tl.keyCount() == 0 && tl.segmentCount() == 0 && tl.totalFrames() == 0 && !tl.busy());
    KeyId again = NO_KEY;
    tl.addKey(pose(0.f), &again);
    CHECK(again > e); // ids go on counting
}

static void testLimitsAndTimes()
{
    MotionTimeline tl;
    for (S32 i = 0; i < 4; ++i)
    {
        tl.addKey(pose((F32)(i * 20)));
    }
    for (S32 i = 0; i < 3; ++i)
    {
        CHECK(tl.setMethod(pairOf(tl, i), SegMethod::Smooth) == TimelineResult::Ok);
    }
    CHECK(tl.setSeconds(pairOf(tl, 0), 10.f) == TimelineResult::Ok && tl.setSeconds(pairOf(tl, 1), 10.f) == TimelineResult::Ok);
    CHECK(tl.setSeconds(pairOf(tl, 2), 9.9f) == TimelineResult::Ok);
    CHECK(tl.totalFrames() == 1 + 300 + 300 + 297);
    CHECK(tl.setSeconds(pairOf(tl, 2), 10.5f) == TimelineResult::Ok && tl.segment(2).frames == 300); // clamped to 10 s
    CHECK(tl.totalFrames() == TIMELINE_MAX_FRAMES);
    CHECK(tl.setSeconds(pairOf(tl, 2), 9.9f) == TimelineResult::Ok);
    CHECK(tl.setHold(tl.key(0).id, 0.2f) == TimelineResult::TooLong && tl.key(0).hold_sec == 0.f); // 6 frames over
    CHECK(tl.setHold(tl.key(0).id, 0.1f) == TimelineResult::Ok && tl.totalFrames() == TIMELINE_MAX_FRAMES);
    CHECK(tl.addKey(pose(5.f)) == TimelineResult::TooLong && tl.keyCount() == 4);
    CHECK(tl.setHold(tl.key(1).id, -1.f) == TimelineResult::BadValue && tl.setHold(tl.key(1).id, 6.f) == TimelineResult::BadValue);
    CHECK(tl.setHold(tl.key(1).id, NAN) == TimelineResult::BadValue);
    CHECK(tl.setSeconds(pairOf(tl, 0), NAN) == TimelineResult::BadValue);
    // key frames: hold of key 0 is 3 frames, then the first segment
    CHECK(tl.keyFrame(0) == 0 && tl.holdFrames(0) == 3 && tl.segment(0).start_frame == 3 && tl.keyFrame(1) == 3 + 300);
    CHECK(tl.moveKey(tl.key(3).id, 0) == TimelineResult::Ok && tl.totalFrames() == 1 + 30 + 300 + 300 + 3); // (d,a) is new: it takes the method (Smooth) of the pair that follows

    // the shortest segment, and a hold on the last key
    MotionTimeline s;
    s.addKey(pose(0.f));
    s.addKey(pose(10.f));
    s.setMethod(pairOf(s, 0), SegMethod::Linear);
    CHECK(s.setSeconds(pairOf(s, 0), 0.01f) == TimelineResult::Ok && s.segment(0).frames == 6);
    CHECK(s.setHold(s.key(1).id, 1.f) == TimelineResult::Ok && s.totalFrames() == 1 + 6 + 30);
    // generate segments' length: automatic is 10 tokens (33 steps); a chosen length
    MotionTimeline g;
    g.addKey(pose(0.f));
    g.addKey(pose(10.f));
    CHECK(g.segment(0).frames == 33);
    CHECK(g.setTokens(pairOf(g, 0), 6) == TimelineResult::Ok && g.segment(0).frames == 17);
    CHECK(g.setTokens(pairOf(g, 0), 17) == TimelineResult::BadValue && g.setTokens(pairOf(g, 0), 5) == TimelineResult::BadValue);
    CHECK(g.setTokens(pairOf(g, 0), TIMELINE_TOKENS_AUTO) == TimelineResult::Ok && g.segment(0).frames == 33);
    CHECK(g.setSeconds(pairOf(g, 0), 2.f) == TimelineResult::BadValue); // not for Generate
}

// ---------------------------------------------------------------- 2./3. concatClips and makeInterpClip
static KeyPose keyPoseOf(const SomaRetarget& retarget, const history::Pose& p)
{
    TrackLayout          layout;
    std::vector<KeyPose> keys;
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &p, &p }, layout, keys) == TrackResult::Ok);
    return keys.empty() ? KeyPose() : keys[0];
}

static bool sameFrame(const MotionClip& a, S32 fa, const MotionClip& b, S32 fb, double tol_deg = 0.01)
{
    for (S32 j = 0; j < a.joint_count; ++j)
    {
        if (angleBetween(a.local[(size_t)fa * a.joint_count + j], b.local[(size_t)fb * b.joint_count + j]) > tol_deg)
        {
            return false;
        }
    }
    return (a.pelvis[(size_t)fa] - b.pelvis[(size_t)fb]).length() < 1e-5f;
}

static bool signsKept(const MotionClip& c)
{
    for (S32 f = 1; f < c.frame_count; ++f)
    {
        for (S32 j = 0; j < c.joint_count; ++j)
        {
            if (dotOf(c.local[(size_t)(f - 1) * c.joint_count + j], c.local[(size_t)f * c.joint_count + j]) < 0.0)
            {
                return false;
            }
        }
    }
    return true;
}

static void testClips(const SomaRetarget& retarget)
{
    const KeyPose k0 = keyPoseOf(retarget, pose(0.f, 0.f)), k1 = keyPoseOf(retarget, pose(80.f, 0.1f, 40.f)),
                  k2 = keyPoseOf(retarget, pose(-60.f, -0.1f, 10.f));
    MotionClip ease, lin;
    CHECK(makeInterpClip(k0, k1, 31, InterpCurve::EaseInOut, ease) == RetargetResult::Ok && ease.frame_count == 31);
    CHECK(makeInterpClip(k0, k1, 31, InterpCurve::Linear, lin) == RetargetResult::Ok);
    MotionClip e0, e1;
    // 3. the ends are the keys' (any sign), with and without ease
    for (const MotionClip* c : { &ease, &lin })
    {
        for (S32 j = 0; j < c->joint_count; ++j)
        {
            CHECK(angleBetween(c->local[(size_t)j], k0.sl.local[(size_t)j]) < 0.01);
            CHECK(angleBetween(c->local[(size_t)(c->frame_count - 1) * c->joint_count + j], k1.sl.local[(size_t)j]) < 0.01);
        }
        CHECK((c->pelvis.front() - k0.sl.pelvis).length() < 1e-6f && (c->pelvis.back() - k1.sl.pelvis).length() < 1e-6f);
        CHECK(signsKept(*c));
    }
    // ease is slow at the start and equal to linear in the middle; linear is even
    const S32 elbow = retarget.find("mElbowLeft");
    CHECK(elbow >= 0);
    const double a_ease1 = angleBetween(ease.local[(size_t)ease.joint_count + elbow], ease.local[(size_t)elbow]);
    const double a_lin1  = angleBetween(lin.local[(size_t)lin.joint_count + elbow], lin.local[(size_t)elbow]);
    CHECK(a_ease1 < a_lin1 * 0.2 && a_lin1 > 1.5);
    CHECK(angleBetween(ease.local[(size_t)15 * ease.joint_count + elbow], lin.local[(size_t)15 * lin.joint_count + elbow]) < 0.05);
    // refused: too few / too many frames, sizes that differ, a bad rotation
    MotionClip keep = ease;
    CHECK(makeInterpClip(k0, k1, 1, InterpCurve::Linear, keep) == RetargetResult::BadInput);
    CHECK(makeInterpClip(k0, k1, TIMELINE_MAX_FRAMES + 1, InterpCurve::Linear, keep) == RetargetResult::BadInput);
    CHECK(makeInterpClip(k0, k1, TIMELINE_MIN_FRAMES, InterpCurve::Linear, keep) == RetargetResult::Ok && keep.frame_count == 2);
    keep = ease;
    KeyPose wrong = k1;
    wrong.extra_vec.push_back(LLVector3::zero);
    CHECK(makeInterpClip(k0, wrong, 10, InterpCurve::Linear, keep) == RetargetResult::BadInput && keep.frame_count == 31);
    KeyPose degenerate = k1;
    degenerate.sl.local[(size_t)elbow] = LLQuaternion(0.f, 0.f, 0.f, 0.f);
    CHECK(makeInterpClip(k0, degenerate, 10, InterpCurve::Linear, keep) == RetargetResult::BadInput && keep.frame_count == 31);
    // a flipped sign at an end does not make it go the long way
    KeyPose flipped = k1;
    for (LLQuaternion& q : flipped.sl.local)
    {
        q = LLQuaternion(-q.mQ[VX], -q.mQ[VY], -q.mQ[VZ], -q.mQ[VW]);
    }
    MotionClip flip;
    CHECK(makeInterpClip(k0, flipped, 31, InterpCurve::Linear, flip) == RetargetResult::Ok && signsKept(flip));

    // 2. concatClips: 10, 20 and 5 frames, holds 0, 3, 0, 2 -> 1 + 9 + 19 + 4 + 5 = 38
    MotionClip c0, c1, c2, all;
    CHECK(makeInterpClip(k0, k1, 10, InterpCurve::Linear, c0) == RetargetResult::Ok);
    CHECK(makeInterpClip(k1, k2, 20, InterpCurve::EaseInOut, c1) == RetargetResult::Ok);
    CHECK(makeInterpClip(k2, k0, 5, InterpCurve::Linear, c2) == RetargetResult::Ok);
    CHECK(concatClips({ &c0, &c1, &c2 }, { 0, 3, 0, 2 }, all) == ConcatResult::Ok);
    CHECK(all.frame_count == 38 && all.joint_count == c0.joint_count && (S32)all.pelvis.size() == 38);
    CHECK((S32)all.local.size() == 38 * all.joint_count);
    CHECK(sameFrame(all, 0, c0, 0) && sameFrame(all, 9, c0, 9) && sameFrame(all, 9, c1, 0)); // the seam is one frame
    for (S32 f = 9; f <= 12; ++f) // key 1 and its 3 held frames
    {
        CHECK(sameFrame(all, f, c0, 9));
    }
    CHECK(sameFrame(all, 13, c1, 1) && sameFrame(all, 31, c1, 19) && sameFrame(all, 31, c2, 0));
    CHECK(sameFrame(all, 32, c2, 1) && sameFrame(all, 35, c2, 4) && sameFrame(all, 36, c2, 4) && sameFrame(all, 37, c2, 4));
    CHECK(signsKept(all));
    // no holds, one clip
    CHECK(concatClips({ &c0 }, { 0, 0 }, all) == ConcatResult::Ok && all.frame_count == 10);
    // refused
    MotionClip untouched = all;
    CHECK(concatClips({}, { 0 }, all) == ConcatResult::NoClips);
    CHECK(concatClips({ &c0, &c1 }, { 0, 0 }, all) == ConcatResult::BadHold);
    CHECK(concatClips({ &c0 }, { 0, -1 }, all) == ConcatResult::BadHold);
    MotionClip other = c1;
    other.extra_vec_count = 3;
    other.extra_vec.assign((size_t)other.frame_count * 3, LLVector3::zero);
    CHECK(concatClips({ &c0, &other }, { 0, 0, 0 }, all) == ConcatResult::Mismatch);
    MotionClip broken = c1;
    broken.pelvis.pop_back();
    CHECK(concatClips({ &c0, &broken }, { 0, 0, 0 }, all) == ConcatResult::Mismatch);
    CHECK(concatClips({ nullptr }, { 0, 0 }, all) == ConcatResult::Mismatch);
    MotionClip big1, big2;
    CHECK(makeInterpClip(k0, k1, 600, InterpCurve::Linear, big1) == RetargetResult::Ok);
    CHECK(makeInterpClip(k1, k2, 600, InterpCurve::Linear, big2) == RetargetResult::Ok);
    CHECK(concatClips({ &big1, &big2 }, { 0, 0, 0 }, all) == ConcatResult::TooLong && all.frame_count == untouched.frame_count);
    CHECK(concatClips({ &big1, &big2 }, { 0, 0, 0 }, all) != ConcatResult::Ok);
    CHECK(makeInterpClip(k0, k1, 451, InterpCurve::Linear, big1) == RetargetResult::Ok &&
          makeInterpClip(k1, k2, 451, InterpCurve::Linear, big2) == RetargetResult::Ok);
    CHECK(concatClips({ &big1, &big2 }, { 0, 0, 0 }, all) == ConcatResult::Ok && all.frame_count == TIMELINE_MAX_FRAMES);
    CHECK(concatClips({ &big1, &big2 }, { 0, 1, 0 }, all) == ConcatResult::TooLong);
    // the model's answer range is not the joined clip's range (the two constants are apart)
    static_assert(CLIP_MAX_FRAMES == 58 && CLIP_MIN_FRAMES == 18 && TIMELINE_MIN_FRAMES == 2 && TIMELINE_MAX_FRAMES == 901);
}

// ---------------------------------------------------------------- 1. the state machine
static void testStates(const SomaRetarget& retarget)
{
    MotionTimeline tl;
    tl.addKey(pose(0.f));
    tl.addKey(pose(40.f));
    tl.addKey(pose(80.f));
    const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok);
    CHECK(tl.clip().frame_count == 67 && tl.totalFrames() == 67); // stand-in Smooth clips of 34 frames

    // S0 -> S1 -> S2 -> S3, one job at a time
    CHECK(tl.requestAll() == 2 && tl.queuedCount() == 2 && tl.segment(0).state == SegState::Queued);
    CHECK(tl.requestGenerate(p0) == TimelineResult::Busy);
    CHECK(tl.requestAll() == 0);
    auto job = tl.nextJob(retarget);
    CHECK(job && job->pair == p0 && job->tokens == TIMELINE_TOKENS_DEFAULT && !job->extra);
    CHECK(tl.segment(0).state == SegState::Running && tl.segment(1).state == SegState::Queued && tl.running());
    CHECK(!tl.nextJob(retarget)); // one at a time
    CHECK(tl.setMethod(p0, SegMethod::Smooth) == TimelineResult::Busy && tl.setTokens(p0, 8) == TimelineResult::Busy);
    const GenerateJob first = *job;
    CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(tl.segment(0).state == SegState::Made && tl.segment(0).answer_count == 1 && tl.segment(0).selected == 0);
    CHECK(!tl.running() && tl.busy() && tl.queuedCount() == 1);
    // nothing is out: a second answer for the same pair (a late one) is dropped, and changes nothing
    CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, first)) == AnswerFate::Dropped);
    CHECK(tl.segment(0).answer_count == 1 && tl.segment(0).state == SegState::Made && tl.queuedCount() == 1);

    // a failure of one segment: only that one is S5, the queue goes on
    job = tl.nextJob(retarget);
    CHECK(job && job->pair == p1);
    CHECK(!tl.onFailure(JobError::RunFailed));
    CHECK(tl.segment(1).state == SegState::Failed && tl.segment(1).fail == JobError::RunFailed && !tl.busy());
    CHECK(tl.requestAll() == 1 && tl.segment(1).state == SegState::Queued && tl.segment(0).state == SegState::Made); // S3 is not "all"
    job = tl.nextJob(retarget);
    CHECK(job && job->pair == p1);
    // a failure of the whole: the queue and the running one go back
    CHECK(tl.requestGenerate(p0) == TimelineResult::Ok);
    CHECK(tl.onFailure(JobError::Timeout) && !tl.busy());
    CHECK(tl.segment(1).state == SegState::Unmade && tl.segment(0).state == SegState::Made && tl.segment(1).fail == JobError::None);
    for (JobError e : { JobError::LowMemory, JobError::NoHelper, JobError::BadFolder, JobError::ModelMissing,
                        JobError::ModelUnreadable, JobError::Timeout, JobError::HelperEnded })
    {
        CHECK(isWholeFailure(e));
    }
    for (JobError e : { JobError::None, JobError::BadInput, JobError::RunFailed, JobError::BadAnswer, JobError::CannotBuild })
    {
        CHECK(!isWholeFailure(e));
    }

    // the user's cancel: S1 and S2 back to S0 / S3, edits never cancel
    CHECK(tl.requestAll() == 1 && tl.requestGenerate(p0) == TimelineResult::Ok);
    job = tl.nextJob(retarget);
    CHECK(job && job->pair == p1 && tl.queuedCount() == 1);
    tl.updateKey(tl.key(0).id, pose(5.f));                   // an edit while a job is out
    tl.setHold(tl.key(0).id, 0.5f);
    CHECK(tl.running() && tl.queuedCount() == 1);
    CHECK(tl.cancelGeneration() && !tl.busy());
    CHECK(tl.segment(1).state == SegState::Unmade && tl.segment(0).state == SegState::Made);
    CHECK(!tl.cancelGeneration());

    // an answer is fitted to the keys as they are when it comes (the poses it was asked for are not the ones now)
    CHECK(tl.requestGenerate(p1) == TimelineResult::Ok);
    job = tl.nextJob(retarget);
    CHECK(job && job->pair == p1);
    const KeyId last = tl.key(2).id;
    tl.updateKey(last, pose(120.f));
    CHECK(tl.running()); // not cancelled
    const std::vector<SomaFrame> answer = fakeAnswer(retarget, *job);
    CHECK(tl.onAnswer(retarget, std::vector<SomaFrame>(answer)) == AnswerFate::Used);
    CHECK(!tl.stale() && tl.segment(1).state == SegState::Made);
    {
        const PairCache* c = tl.cache(p1);
        CHECK(c && c->answers.size() == 1 && c->answers[0].sent_end.joints.size() > 0);
        // the clip ends at the key as it is now
        const KeyPose& end = tl.keyPose(2);
        const MotionClip& clip = tl.clip();
        for (S32 j = 0; j < clip.joint_count; ++j)
        {
            CHECK(angleBetween(clip.local[(size_t)(clip.frame_count - 1) * clip.joint_count + j], end.sl.local[(size_t)j]) < 0.05);
        }
    }

    // an answer for a pair that is gone is dropped
    CHECK(tl.requestGenerate(p1) == TimelineResult::Ok);
    job = tl.nextJob(retarget);
    CHECK(job && tl.removeKey(last) == TimelineResult::Ok && tl.segmentCount() == 1);
    CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Dropped && !tl.running() && !tl.busy());
    CHECK(tl.segment(0).state == SegState::Made && tl.segment(0).answer_count == 1);
    // a queued job of a pair that went goes with it
    tl.addKey(pose(10.f));
    CHECK(tl.setTokens(pairOf(tl, 1), 6) == TimelineResult::Ok && tl.requestGenerate(pairOf(tl, 1)) == TimelineResult::Ok);
    CHECK(tl.queuedCount() == 1 && tl.removeKey(tl.key(2).id) == TimelineResult::Ok && tl.queuedCount() == 0);
    CHECK(!tl.nextJob(retarget));

    // an answer of the wrong length is a failure of that segment; one that cannot be fitted too
    MotionTimeline w;
    w.addKey(pose(0.f));
    w.addKey(pose(30.f));
    w.requestGenerate(pairOf(w, 0));
    job = w.nextJob(retarget);
    std::vector<SomaFrame> bad = fakeAnswer(retarget, *job);
    bad.pop_back();
    CHECK(w.onAnswer(retarget, std::move(bad)) == AnswerFate::CannotBuild && w.segment(0).state == SegState::Failed);
    CHECK(w.segment(0).fail == JobError::BadAnswer && w.segment(0).answer_count == 0);
    w.requestGenerate(pairOf(w, 0));
    job = w.nextJob(retarget);
    std::vector<SomaFrame> nan = fakeAnswer(retarget, *job);
    nan[5].hips = LLVector3(NAN, 0.f, 0.f);
    CHECK(w.onAnswer(retarget, std::move(nan)) == AnswerFate::CannotBuild && w.segment(0).state == SegState::Failed);
    CHECK(w.segment(0).answer_count == 0 && !w.busy());
    // a failed segment is asked again by the user only
    CHECK(w.requestGenerate(pairOf(w, 0)) == TimelineResult::Ok);

    // keys that cannot be made into a layout: what is queued fails (BadInput), nothing is sent
    MotionTimeline nolayout;
    history::Pose ex, ey;
    ex.joints.push_back(joint("mElbowLeft", LLQuaternion::DEFAULT));
    ey.joints.push_back(joint("mElbowRight", LLQuaternion::DEFAULT));
    nolayout.addKey(ex);
    nolayout.addKey(ey);
    CHECK(nolayout.requestGenerate(pairOf(nolayout, 0)) == TimelineResult::Ok);
    CHECK(!nolayout.nextJob(retarget) && nolayout.segment(0).state == SegState::Failed &&
          nolayout.segment(0).fail == JobError::BadInput && !nolayout.busy());

    // equal poses cannot be generated
    MotionTimeline same;
    same.addKey(pose(0.f));
    same.addKey(pose(0.f));
    CHECK(same.segment(0).method == SegMethod::Smooth); // made as Smooth, not Generate
    same.setMethod(pairOf(same, 0), SegMethod::Linear);
    CHECK(same.requestGenerate(pairOf(same, 0)) == TimelineResult::BadValue);
    MotionTimeline edit;
    edit.addKey(pose(0.f));
    edit.addKey(pose(10.f));
    const KeyId ek = edit.key(1).id;
    edit.updateKey(ek, pose(0.f)); // the keys became equal while the pair is Generate
    CHECK(edit.requestGenerate(pairOf(edit, 0)) == TimelineResult::SamePoses && edit.requestAll() == 0);
    CHECK(edit.refresh(retarget) == TimelineResult::Ok); // the stand-in is a still clip
}

// ---------------------------------------------------------------- 1. more candidates
static void testCandidates(const SomaRetarget& retarget)
{
    MotionTimeline tl;
    tl.addKey(pose(0.f));
    tl.addKey(pose(50.f));
    tl.addKey(pose(0.f, 0.f));
    const PairId p = pairOf(tl, 0);
    CHECK(!tl.canRequestMoreAnswers(p) && tl.requestMoreAnswers(p) == 0); // no answer yet
    tl.requestGenerate(p);
    auto job = tl.nextJob(retarget);
    CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(tl.canRequestMoreAnswers(p)); // the rule the window greys its button by is the one the request takes
    CHECK(tl.requestMoreAnswers(p) == 2 && tl.segment(0).state == SegState::Queued);
    CHECK(!tl.canRequestMoreAnswers(p) && tl.requestMoreAnswers(p) == 0); // queued or running: nothing more is asked
    job = tl.nextJob(retarget);
    CHECK(job && job->extra && job->tokens == 8);
    CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(tl.segment(0).selected == 0 && tl.segment(0).frames == 33 && tl.segment(0).state == SegState::Queued); // the other is queued
    job = tl.nextJob(retarget);
    CHECK(job && job->extra && job->tokens == 12);
    // the second one fails: the segment stays S3 with the two it has (a note of the failure)
    CHECK(!tl.onFailure(JobError::RunFailed));
    CHECK(tl.segment(0).state == SegState::Made && tl.segment(0).answer_count == 2 && tl.segment(0).fail == JobError::RunFailed);
    CHECK(tl.canRequestMoreAnswers(p));
    CHECK(tl.requestMoreAnswers(p) == 1); // 12 only; 8 is there
    job = tl.nextJob(retarget);
    CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used && tl.segment(0).answer_count == 3);
    CHECK(!tl.canRequestMoreAnswers(p));
    CHECK(tl.requestMoreAnswers(p) == 0); // full
    // three answers {10, 8, 12} and the 12 chosen: its +2 side (14) is inside 6..16 and not held, but the segment is full
    CHECK(tl.selectAnswer(p, 2) == TimelineResult::Ok && tl.cache(p)->answers[2].tokens == 12);
    CHECK(!tl.canRequestMoreAnswers(p) && tl.requestMoreAnswers(p) == 0);
    CHECK(tl.selectAnswer(p, 0) == TimelineResult::Ok);

    // choosing another answer changes the length, and the key after it moves in time
    const S32 key2_before = tl.keyFrame(2);
    CHECK(tl.selectAnswer(p, 1) == TimelineResult::Ok); // answers: 10, 8, 12
    CHECK(tl.segment(0).frames == 25 && tl.keyFrame(2) == key2_before - 8 && tl.stale());
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.clip().frame_count == tl.totalFrames());
    CHECK(tl.selectAnswer(p, 7) == TimelineResult::BadValue);
    // asking for a length that has an answer selects it; one that has none makes the segment S0 (answers kept)
    CHECK(tl.setTokens(p, 12) == TimelineResult::Ok && tl.segment(0).selected == 2 && tl.segment(0).state == SegState::Made);
    CHECK(tl.setTokens(p, 14) == TimelineResult::Ok && tl.segment(0).selected == -1 && tl.segment(0).state == SegState::Unmade &&
          tl.segment(0).answer_count == 3 && tl.segment(0).frames == 49);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok); // the stand-in has the asked length
    CHECK(tl.setTokens(p, TIMELINE_TOKENS_AUTO) == TimelineResult::Ok && tl.segment(0).selected == 0);
    // a method change keeps the answers
    CHECK(tl.setMethod(p, SegMethod::Smooth) == TimelineResult::Ok && tl.segment(0).state == SegState::Made);
    CHECK(tl.setMethod(p, SegMethod::Generate) == TimelineResult::Ok && tl.segment(0).answer_count == 3 && tl.segment(0).state == SegState::Made);
}

// ---------------------------------------------------------------- 4. the refit and the mark
static double endGap(const MotionTimeline& tl, S32 pair_index)
{
    // the largest turn between the joined clip at the end of the segment and the key there
    const S32 f = tl.keyFrame(pair_index + 1);
    const MotionClip& c = tl.clip();
    double worst = 0.0;
    for (S32 j = 0; j < c.joint_count; ++j)
    {
        worst = std::max(worst, angleBetween(c.local[(size_t)f * c.joint_count + j], tl.keyPose(pair_index + 1).sl.local[(size_t)j]));
    }
    return worst;
}

static void testRefit(const SomaRetarget& retarget)
{
    MotionTimeline tl;
    tl.addKey(pose(0.f));
    tl.addKey(pose(60.f));
    tl.addKey(pose(0.f, 0.f, 20.f));
    tl.requestAll();
    while (auto job = tl.nextJob(retarget))
    {
        CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    }
    CHECK(tl.refresh(retarget) == TimelineResult::Ok);
    const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
    const KeyId  mid = tl.key(1).id;
    CHECK(!tl.cache(p0)->suggest_regen && !tl.cache(p1)->suggest_regen && endGap(tl, 0) < 0.05);
    const U32 layout0 = tl.layoutVersion();

    // the key is changed a little: the ends follow it at once (no helper), no mark
    tl.updateKey(mid, pose(70.f));
    CHECK(tl.stale() && !tl.busy());
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && endGap(tl, 0) < 0.05 && endGap(tl, 1) < 0.05);
    CHECK(!tl.cache(p0)->suggest_regen && tl.layoutVersion() == layout0);
    // the pair keeps its answer (still one, still selected); a refit asks for nothing
    CHECK(tl.cache(p0)->answers.size() == 1 && tl.segment(0).state == SegState::Made && !tl.running());

    // the mark is against what the answer was made for, not against the last edit: 10 degrees at a time adds up
    tl.updateKey(mid, pose(60.f)); // as it was made
    tl.refresh(retarget);
    CHECK(!tl.cache(p0)->suggest_regen);
    tl.updateKey(mid, pose(70.f));
    tl.refresh(retarget);
    CHECK(!tl.cache(p0)->suggest_regen); // 10
    tl.updateKey(mid, pose(80.f));
    tl.refresh(retarget);
    CHECK(tl.cache(p0)->suggest_regen && tl.segment(0).suggest_regen); // 20 from what the answer ended at
    tl.updateKey(mid, pose(60.f));
    tl.refresh(retarget);
    CHECK(!tl.cache(p0)->suggest_regen); // back: no mark

    // the pelvis moved 20 cm at the end: the mark too
    MotionTimeline pv;
    pv.addKey(pose(0.f));
    pv.addKey(pose(30.f));
    pv.requestAll();
    auto job = pv.nextJob(retarget);
    CHECK(job && pv.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    pv.refresh(retarget);
    CHECK(!pv.cache(pairOf(pv, 0))->suggest_regen);
    pv.updateKey(pv.key(1).id, pose(30.f, 0.2f));
    pv.refresh(retarget);
    CHECK(pv.cache(pairOf(pv, 0))->suggest_regen && endGap(pv, 0) < 0.05);
    CHECK(!pv.busy() && !pv.nextJob(retarget)); // a refit asks nothing

    // a joint the other keys lack is not written
    const U32 clip_before = tl.clipVersion();
    history::Pose changed = pose(60.f);
    changed.joints.push_back(joint("mWristLeft", LLQuaternion(0.4f, LLVector3::x_axis)));
    tl.updateKey(mid, changed);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.layoutVersion() == layout0 && tl.clipVersion() > clip_before);

    // a joint every key has, driven in a new key: the layout grows and every segment is made again
    MotionTimeline lay;
    for (F32 e : { 0.f, 40.f, 80.f })
    {
        history::Pose q = pose(e);
        q.joints.push_back(joint("mWristLeft", LLQuaternion(0.1f, LLVector3::x_axis), false));
        lay.addKey(q);
    }
    CHECK(lay.refresh(retarget) == TimelineResult::Ok);
    const U32 lv = lay.layoutVersion();
    lay.requestAll();
    while (auto j = lay.nextJob(retarget))
    {
        lay.onAnswer(retarget, fakeAnswer(retarget, *j));
    }
    lay.refresh(retarget);
    const S32 joints_before = (S32)lay.layout().joints.size();
    history::Pose driven = pose(80.f);
    driven.joints.push_back(joint("mWristLeft", LLQuaternion(0.1f, LLVector3::x_axis), true));
    lay.updateKey(lay.key(2).id, driven);
    CHECK(lay.refresh(retarget) == TimelineResult::Ok);
    CHECK(lay.layoutVersion() > lv && (S32)lay.layout().joints.size() == joints_before + 1);
    for (S32 i = 0; i < 2; ++i)
    {
        const PairCache* c = lay.cache(pairOf(lay, i));
        CHECK(c && c->fit_layout == lay.layoutVersion() && c->answers.size() == 1);
        CHECK(c->clip.joint_count == lay.clip().joint_count && endGap(lay, i) < 0.05);
    }
    // a key that lacks a joint takes it out of the layout
    lay.addKey(pose(40.f));
    CHECK(lay.refresh(retarget) == TimelineResult::Ok && (S32)lay.layout().joints.size() == joints_before);
    // keys that share no driven joint cannot be made
    MotionTimeline none;
    history::Pose x, y;
    x.joints.push_back(joint("mElbowLeft", LLQuaternion::DEFAULT));
    y.joints.push_back(joint("mElbowRight", LLQuaternion::DEFAULT));
    none.addKey(x);
    none.addKey(y);
    CHECK(none.refresh(retarget) == TimelineResult::BuildFailed && none.lastTrackResult() == TrackResult::NoJoints);
    MotionTimeline lone;
    lone.addKey(pose(0.f));
    CHECK(lone.refresh(retarget) == TimelineResult::NeedsKeys && lone.clip().empty());
    SomaRetarget notready;
    CHECK(tl.refresh(notready) == TimelineResult::NotReady);
    tl.invalidateClips();
    CHECK(tl.stale() && tl.refresh(retarget) == TimelineResult::Ok);
}

// ---------------------------------------------------------------- 6. the joined clip as played
static void testJoined(const SomaRetarget& retarget)
{
    MotionTimeline tl;
    tl.addKey(pose(0.f));
    tl.addKey(pose(50.f));
    tl.addKey(pose(-30.f, 0.2f));
    tl.setMethod(pairOf(tl, 0), SegMethod::Smooth);
    tl.setMethod(pairOf(tl, 1), SegMethod::Linear);
    tl.setSeconds(pairOf(tl, 0), 1.f);
    tl.setSeconds(pairOf(tl, 1), 0.5f);
    tl.setHold(tl.key(1).id, 1.f);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok);
    CHECK(tl.totalFrames() == 1 + 30 + 30 + 15 && tl.clip().frame_count == tl.totalFrames());
    CHECK(tl.keyFrame(1) == 30 && tl.keyFrame(2) == 30 + 30 + 15 && tl.segment(1).start_frame == 60);
    // the held key: the pose stays for 30 frames, then the second segment starts
    for (S32 f = 30; f <= 60; ++f)
    {
        CHECK(sameFrame(tl.clip(), f, tl.clip(), 30, 1e-4));
    }
    CHECK(!sameFrame(tl.clip(), 61, tl.clip(), 30, 1e-4));
    CHECK(tl.totalSeconds() > 2.49 && tl.totalSeconds() < 2.51);
    // the position of a playback is kept when the clip is made again
    ClipPlayback pb;
    CHECK(pb.load(tl.totalFrames(), TIMELINE_MIN_FRAMES, TIMELINE_MAX_FRAMES) && pb.seek(50) && pb.frame() == 50);
    tl.setSeconds(pairOf(tl, 1), 1.f);
    tl.refresh(retarget);
    CHECK(pb.reload(tl.totalFrames()) && pb.frame() == 50 && pb.frameCount() == tl.totalFrames() && !pb.playing());
    pb.play(10.0);
    CHECK(pb.playing() && pb.reload(tl.totalFrames()) && !pb.playing()); // a playing one stops (the window stops it first)
    CHECK(pb.reload(40) && pb.frame() == 39); // clamped to the shorter clip
    CHECK(!pb.reload(TIMELINE_MAX_FRAMES + 1) && pb.state() == ClipState::None);
    CHECK(!pb.reload(40)); // C0: nothing to keep
    // the loop hold: 0 when the last key is the first
    CHECK(tl.loopHoldSec() == LOOP_HOLD_SEC);
    KeyId loop = NO_KEY;
    CHECK(tl.closeLoop(&loop) == TimelineResult::Ok && tl.key(3).id == loop && tl.loopHoldSec() == 0.0);
    CHECK(tl.closeLoop() == TimelineResult::BadValue); // already a loop
    tl.refresh(retarget);
    const MotionClip& c = tl.clip();
    CHECK(sameFrame(c, 0, c, c.frame_count - 1, 0.01)); // its last frame is its first
    CHECK(cursorAt(31, 1.25, LoopMode::Loop).frame == 30);    // the default hold of 0.5 s: stays on the last frame
    CHECK(cursorAt(31, 1.25, LoopMode::Loop, 0.0).frame == 7); // no hold: again from the start (0.25 s in)
    CHECK(cursorAt(31, 1.25, LoopMode::Loop, NAN).frame == 7 && cursorAt(31, 1.25, LoopMode::Loop, -1.0).frame == 7); // a bad hold is 0: wraps
    CHECK(cursorAt(31, 1.0, LoopMode::Loop, 0.0).frame == 0);
    CHECK(cursorAt(31, 0.5, LoopMode::Loop, 0.0).frame == 15 && cursorAt(31, 1.2, LoopMode::Once, 0.0).finished);
    // a removed loop key
    CHECK(tl.removeKey(loop) == TimelineResult::Ok && tl.loopHoldSec() == LOOP_HOLD_SEC);
    // playing the joined clip: sizePoseFor and samplePose on it
    tl.refresh(retarget);
    KeyPose out;
    sizePoseFor(tl.clip(), out);
    CHECK(samplePose(tl.clip(), cursorAtFrame(tl.clip().frame_count, 20), out));
    const S32 elbow = retarget.find("mElbowLeft");
    CHECK(angleBetween(out.sl.local[(size_t)elbow], tl.clip().local[(size_t)20 * tl.clip().joint_count + (size_t)elbow]) < 1e-3);
}

// ---------------------------------------------------------------- 4. N poses make one layout
static void testLayout(const SomaRetarget& retarget)
{
    history::Pose a = pose(0.f), b = pose(30.f), c = pose(60.f);
    TrackLayout          layout;
    std::vector<KeyPose> keys;
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a, &b, &c }, layout, keys) == TrackResult::Ok);
    CHECK(keys.size() == 3 && layout.joints.size() == 6);
    // a joint only some keys have is not written
    b.joints.push_back(joint("mWristLeft", LLQuaternion(0.1f, LLVector3::x_axis)));
    TrackLayout          l2;
    std::vector<KeyPose> k2;
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a, &b, &c }, l2, k2) == TrackResult::Ok && l2.joints.size() == 6);
    // one driven in any key counts; the layout is the same for all keys (sizes alike)
    a.joints.push_back(joint("mWristLeft", LLQuaternion::DEFAULT, false));
    c.joints.push_back(joint("mWristLeft", LLQuaternion::DEFAULT, false));
    TrackLayout          l3;
    std::vector<KeyPose> k3;
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a, &b, &c }, l3, k3) == TrackResult::Ok && l3.joints.size() == 7);
    CHECK(k3[0].extra_vec.size() == k3[1].extra_vec.size() && k3[1].extra_vec.size() == k3[2].extra_vec.size());
    // a position track exists when it is not zero in any key
    history::Pose d = pose(0.f);
    d.joints.push_back(joint("mWristLeft", LLQuaternion::DEFAULT, false));
    d.joints[1].position = history::Vec3{ 0.f, 0.1f, 0.f }; // mTorso
    TrackLayout          l4;
    std::vector<KeyPose> k4;
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a, &d, &c }, l4, k4) == TrackResult::Ok);
    CHECK(k4[0].extra_vec.size() == 1 && k4[1].extra_vec[0].mV[VY] == 0.1f && k4[2].extra_vec[0].mV[VY] == 0.f);
    // refused, and the outputs stay
    TrackLayout          keep = l4;
    std::vector<KeyPose> kk   = k4;
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a }, keep, kk) == TrackResult::NoJoints && kk.size() == 3);
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a, nullptr }, keep, kk) == TrackResult::BadPose);
    history::Pose nanp = pose(0.f);
    nanp.joints[0].position = history::Vec3{ NAN, 0.f, 0.f };
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &a, &nanp }, keep, kk) == TrackResult::BadPose && kk.size() == 3);
    // the two-pose form does what it did
    TrackLayout l5;
    KeyPose     s, e;
    CHECK(makeKeyPoses(retarget, a, c, l5, s, e) == TrackResult::Ok && l5.joints.size() == 6); // the wrist is driven in neither
}

// ---------------------------------------------------------------- review of 2026-11-15: the paths that were not tried
static void answerNext(MotionTimeline& tl, const SomaRetarget& retarget, F32 hips_shift = 0.f)
{
    auto job = tl.nextJob(retarget);
    CHECK(job.has_value());
    if (job)
    {
        CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job, hips_shift)) == AnswerFate::Used);
    }
}

static void testReview(const SomaRetarget& retarget)
{
    // the answer asked for a length that is not the selected one replaces a full segment's answer, none is lost by the
    // length limit (the old code erased the wrong one), and the choice follows the answer
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(50.f));
        const PairId p = pairOf(tl, 0);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        tl.requestMoreAnswers(p);
        answerNext(tl, retarget);
        answerNext(tl, retarget); // answers 10, 8, 12
        CHECK(tl.cache(p)->answers.size() == 3 && tl.segment(0).state == SegState::Made);
        CHECK(tl.setTokens(p, 14) == TimelineResult::Ok && tl.segment(0).selected == -1);
        CHECK(tl.setMethod(p, SegMethod::Smooth) == TimelineResult::Ok);
        CHECK(tl.setMethod(p, SegMethod::Generate) == TimelineResult::Ok);
        CHECK(tl.cache(p)->selected == 0 && tl.cache(p)->tokens == 10); // the asked length follows the chosen answer
        CHECK(tl.setTokens(p, 14) == TimelineResult::Ok);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        CHECK(tl.cache(p)->answers.size() == 3 && tl.segment(0).selected >= 0 &&
              tl.cache(p)->answers[(size_t)tl.segment(0).selected].tokens == 14 && tl.segment(0).frames == 49 &&
              tl.segment(0).state == SegState::Made);
        CHECK(tl.totalFrames() == 50 && tl.clip().frame_count == 50);
    }

    // a choice or a length made while generating is refused (queued or running)
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(50.f));
        const PairId p = pairOf(tl, 0);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        CHECK(tl.requestMoreAnswers(p) == 2);
        CHECK(tl.selectAnswer(p, 0) == TimelineResult::Busy && tl.setTokens(p, 8) == TimelineResult::Busy);
        auto job = tl.nextJob(retarget); // an extra one is running: still busy
        CHECK(job && tl.selectAnswer(p, 0) == TimelineResult::Busy);
        // a failure of the whole in the middle of the extra ones: the segment is S3 again with the answer it had
        CHECK(tl.onFailure(JobError::Timeout) && tl.segment(0).state == SegState::Made && tl.segment(0).answer_count == 1);
        CHECK(!tl.busy() && tl.segment(0).fail == JobError::None);
        // and the user's cancel
        tl.requestMoreAnswers(p);
        job = tl.nextJob(retarget);
        CHECK(job && tl.cancelGeneration() && tl.segment(0).state == SegState::Made && tl.segment(0).answer_count == 1 && !tl.busy());
        // an end of the run with nothing out changes nothing but a whole failure gives the queue back
        CHECK(!tl.onFailure(JobError::RunFailed) && tl.segment(0).state == SegState::Made);
        tl.requestMoreAnswers(p);
        CHECK(tl.onFailure(JobError::NoHelper) && !tl.busy() && tl.segment(0).state == SegState::Made);
    }

    // keys that cannot be made into a layout: the answered segment's extra jobs leave it S3, a segment without an
    // answer is S5; a retarget that is not ready gives the queue back untouched
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(50.f));
        const PairId p = pairOf(tl, 0);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        tl.requestMoreAnswers(p);
        SomaRetarget notready;
        CHECK(!tl.nextJob(notready) && tl.segment(0).state == SegState::Made && tl.segment(0).fail == JobError::None && !tl.busy());
        tl.requestMoreAnswers(p);
        history::Pose lonely;
        lonely.joints.push_back(joint("mElbowRight", LLQuaternion::DEFAULT));
        tl.updateKey(tl.key(1).id, lonely);
        CHECK(!tl.nextJob(retarget) && tl.segment(0).state == SegState::Made && tl.segment(0).fail == JobError::BadInput &&
              tl.segment(0).answer_count == 1 && !tl.busy());
    }

    // the limits roll the change back, all of it (state, fail, selection, length) -- near the 901 frames
    {
        MotionTimeline tl;
        for (S32 i = 0; i < 4; ++i)
        {
            tl.addKey(pose((F32)(i * 20)));
        }
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1), p2 = pairOf(tl, 2);
        tl.setMethod(p0, SegMethod::Smooth);
        tl.setMethod(p1, SegMethod::Smooth);
        tl.setSeconds(p0, 10.f);
        tl.setSeconds(p1, 10.f);
        CHECK(tl.setTokens(p2, 6) == TimelineResult::Ok);
        tl.requestGenerate(p2);
        answerNext(tl, retarget);
        CHECK(tl.setTokens(p2, 10) == TimelineResult::Ok);
        tl.requestGenerate(p2);
        answerNext(tl, retarget);
        CHECK(tl.setTokens(p2, 6) == TimelineResult::Ok && tl.segment(2).selected == 0);
        const F32 hold1 = 133.f / 30.f;
        CHECK(tl.setHold(tl.key(0).id, 5.f) == TimelineResult::Ok && tl.setHold(tl.key(1).id, hold1) == TimelineResult::Ok);
        CHECK(tl.totalFrames() == TIMELINE_MAX_FRAMES);
        CHECK(tl.selectAnswer(p2, 1) == TimelineResult::TooLong && tl.cache(p2)->selected == 0 && tl.cache(p2)->tokens == 6 &&
              tl.segment(2).frames == 17);
        CHECK(tl.setTokens(p2, 10) == TimelineResult::TooLong && tl.cache(p2)->selected == 0 && tl.cache(p2)->tokens == 6 &&
              tl.segment(2).state == SegState::Made);
        CHECK(tl.setMethod(p2, SegMethod::Smooth) == TimelineResult::TooLong && tl.cache(p2)->method == SegMethod::Generate &&
              tl.segment(2).state == SegState::Made && tl.cache(p2)->selected == 0);
        CHECK(tl.totalFrames() == TIMELINE_MAX_FRAMES && tl.refresh(retarget) == TimelineResult::Ok);
        // the reason of a failed segment is not lost by a refused change
        MotionTimeline f;
        for (S32 i = 0; i < 4; ++i)
        {
            f.addKey(pose((F32)(i * 20)));
        }
        f.setMethod(pairOf(f, 0), SegMethod::Smooth);
        f.setMethod(pairOf(f, 1), SegMethod::Smooth);
        f.setSeconds(pairOf(f, 0), 10.f);
        f.setSeconds(pairOf(f, 1), 10.f);
        f.setTokens(pairOf(f, 2), 6);
        f.requestGenerate(pairOf(f, 2));
        auto job = f.nextJob(retarget);
        CHECK(job && !f.onFailure(JobError::RunFailed) && f.segment(2).state == SegState::Failed);
        f.setHold(f.key(0).id, 5.f);
        f.setHold(f.key(1).id, hold1);
        CHECK(f.totalFrames() == TIMELINE_MAX_FRAMES);
        CHECK(f.setTokens(pairOf(f, 2), 10) == TimelineResult::TooLong && f.segment(2).state == SegState::Failed &&
              f.segment(2).fail == JobError::RunFailed && f.cache(pairOf(f, 2))->tokens == 6);
    }
    // a key out of the 901 by remove or move (the new pairs are counted at their longest)
    {
        MotionTimeline tl;
        for (S32 i = 0; i < 5; ++i)
        {
            tl.addKey(pose((F32)(i * 15)));
        }
        for (S32 i = 0; i < 4; ++i)
        {
            tl.setMethod(pairOf(tl, i), SegMethod::Smooth);
        }
        tl.setSeconds(pairOf(tl, 0), 10.f);
        tl.setSeconds(pairOf(tl, 1), 10.f);
        tl.setSeconds(pairOf(tl, 2), 0.2f);
        tl.setSeconds(pairOf(tl, 3), 0.2f);
        tl.setHold(tl.key(0).id, 5.f);
        tl.setHold(tl.key(4).id, 4.f);
        CHECK(tl.totalFrames() == 1 + 600 + 12 + 150 + 120);
        const KeyId k3 = tl.key(3).id;
        CHECK(tl.removeKey(k3) == TimelineResult::TooLong && tl.keyCount() == 5 && tl.indexOf(k3) == 3);
        CHECK(tl.moveKey(k3, 0) == TimelineResult::TooLong && tl.indexOf(k3) == 3);
        CHECK(tl.totalFrames() == 1 + 600 + 12 + 150 + 120);
    }

    // a pair that stays keeps its answer through a key put in, a duplicate, a move; a duplicate carries the pair after it
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(40.f));
        tl.addKey(pose(80.f));
        const PairId bc = pairOf(tl, 1);
        tl.requestGenerate(bc);
        answerNext(tl, retarget);
        CHECK(tl.cache(bc)->answers.size() == 1);
        KeyId front = NO_KEY, tail = NO_KEY;
        CHECK(tl.insertKey(0, pose(5.f), &front) == TimelineResult::Ok && tl.addKey(pose(120.f), &tail) == TimelineResult::Ok);
        CHECK(tl.cache(bc) && tl.cache(bc)->answers.size() == 1 && tl.cache(bc)->state == SegState::Made);
        CHECK(tl.segment(0).method == SegMethod::Generate); // a new first pair: the method of the one after it
        const KeyId b = bc.from, c = bc.to;
        KeyId       dup = NO_KEY;
        CHECK(tl.duplicateKey(b, &dup) == TimelineResult::Ok);
        CHECK(tl.cache(PairId{ b, dup })->method == SegMethod::Smooth);
        const PairCache* moved = tl.cache(PairId{ dup, c });
        CHECK(moved && moved->method == SegMethod::Generate && moved->answers.size() == 1 && moved->state == SegState::Made);
        CHECK(!tl.cache(bc));
        CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.clip().frame_count == tl.totalFrames());
        // the first pair of a list takes the method of the pair that follows it
        MotionTimeline m;
        m.addKey(pose(0.f));
        m.addKey(pose(30.f));
        m.addKey(pose(60.f));
        m.setMethod(pairOf(m, 0), SegMethod::Linear);
        m.setMethod(pairOf(m, 1), SegMethod::Linear);
        m.insertKey(0, pose(90.f));
        CHECK(m.segment(0).method == SegMethod::Linear);
        // a clear tells whether a request was out
        m.requestGenerate(pairOf(m, 0)); // Linear: refused, nothing queued
        MotionTimeline q;
        q.addKey(pose(0.f));
        q.addKey(pose(30.f));
        CHECK(!q.clearAll());
        q.addKey(pose(0.f));
        q.addKey(pose(30.f));
        q.requestGenerate(pairOf(q, 0));
        CHECK(q.nextJob(retarget) && q.clearAll() && !q.busy());
    }

    // a duplicate is held to the limit as it is (the pair after the key moves to the copy, long as it is)
    {
        MotionTimeline tl;
        for (S32 i = 0; i < 3; ++i)
        {
            tl.addKey(pose((F32)(i * 30)));
        }
        tl.setMethod(pairOf(tl, 0), SegMethod::Smooth);
        tl.setMethod(pairOf(tl, 1), SegMethod::Smooth);
        tl.setSeconds(pairOf(tl, 0), 10.f);
        tl.setSeconds(pairOf(tl, 1), 10.f);
        tl.setHold(tl.key(0).id, 5.f);
        tl.setHold(tl.key(2).id, 4.5f);
        CHECK(tl.totalFrames() == 1 + 600 + 150 + 135);
        CHECK(tl.duplicateKey(tl.key(1).id) == TimelineResult::TooLong && tl.keyCount() == 3 && tl.totalFrames() == 886);
        CHECK(tl.refresh(retarget) == TimelineResult::Ok);
        tl.setHold(tl.key(2).id, 3.f);
        CHECK(tl.duplicateKey(tl.key(1).id) == TimelineResult::Ok && tl.keyCount() == 4 && tl.totalFrames() == 1 + 600 + 240 + 30);
        CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.clip().frame_count == tl.totalFrames());
    }

    // the limit of a duplicate to the frame: 871 + 30 = 901 goes, 872 does not
    for (const bool fits : { true, false })
    {
        MotionTimeline tl;
        for (S32 i = 0; i < 3; ++i)
        {
            tl.addKey(pose((F32)(i * 30)));
        }
        tl.setMethod(pairOf(tl, 0), SegMethod::Smooth);
        tl.setMethod(pairOf(tl, 1), SegMethod::Smooth);
        tl.setSeconds(pairOf(tl, 0), 10.f);
        tl.setSeconds(pairOf(tl, 1), 10.f);
        tl.setHold(tl.key(0).id, 5.f);
        tl.setHold(tl.key(2).id, fits ? 4.f : 121.f / 30.f);
        CHECK(tl.totalFrames() == (fits ? 871 : 872));
        CHECK((tl.duplicateKey(tl.key(1).id) == TimelineResult::Ok) == fits);
        CHECK(!fits || (tl.totalFrames() == TIMELINE_MAX_FRAMES && tl.refresh(retarget) == TimelineResult::Ok));
    }

    // a failed regeneration leaves the segment S3 with the answer in use (and "all" does not take it again)
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(50.f));
        const PairId p = pairOf(tl, 0);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        CHECK(tl.requestGenerate(p) == TimelineResult::Ok);
        auto job = tl.nextJob(retarget);
        CHECK(job && !tl.onFailure(JobError::RunFailed));
        CHECK(tl.segment(0).state == SegState::Made && tl.segment(0).selected == 0 && tl.segment(0).fail == JobError::RunFailed);
        CHECK(tl.requestAll() == 0 && tl.requestGenerate(p) == TimelineResult::Ok && tl.segment(0).fail == JobError::None);
    }

    // S0 is "no answer in use": a stopped job gives it back for a segment whose answers are not chosen
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(50.f));
        const PairId p = pairOf(tl, 0);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        CHECK(tl.setTokens(p, 14) == TimelineResult::Ok && tl.segment(0).state == SegState::Unmade && tl.segment(0).selected == -1);
        CHECK(tl.setMethod(p, SegMethod::Generate) == TimelineResult::Ok && tl.segment(0).selected == -1 && tl.cache(p)->tokens == 14);
        tl.requestGenerate(p);
        CHECK(!tl.cancelGeneration() && tl.segment(0).state == SegState::Unmade); // queued only: nothing was out
        CHECK(tl.requestAll() == 1);
        auto job = tl.nextJob(retarget);
        CHECK(job && tl.onFailure(JobError::Timeout) && tl.segment(0).state == SegState::Unmade);
        CHECK(tl.requestAll() == 1);
        // the same for the pair a duplicate carries
        tl.cancelGeneration();
        KeyId dup = NO_KEY;
        CHECK(tl.duplicateKey(tl.key(0).id, &dup) == TimelineResult::Ok);
        CHECK(tl.cache(PairId{ dup, tl.key(2).id })->state == SegState::Unmade);
    }

    // a failure of one extra answer leaves the segment waiting for the other
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(50.f));
        const PairId p = pairOf(tl, 0);
        tl.requestGenerate(p);
        answerNext(tl, retarget);
        CHECK(tl.requestMoreAnswers(p) == 2);
        auto job = tl.nextJob(retarget);
        CHECK(job && !tl.onFailure(JobError::RunFailed) && tl.segment(0).state == SegState::Queued && tl.busy());
        job = tl.nextJob(retarget);
        CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used && tl.segment(0).state == SegState::Made &&
              tl.segment(0).answer_count == 2);
    }

    // the same method is nothing to do; the layout goes with the keys; a pair that replaces one takes the one before
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(30.f));
        CHECK(tl.refresh(retarget) == TimelineResult::Ok);
        const U32 lv = tl.layoutVersion();
        CHECK(!tl.layout().joints.empty());
        tl.removeKey(tl.key(1).id);
        CHECK(tl.refresh(retarget) == TimelineResult::NeedsKeys && tl.layout().joints.empty() && tl.layoutVersion() > lv);
        MotionTimeline m;
        for (S32 i = 0; i < 4; ++i)
        {
            m.addKey(pose((F32)(i * 20)));
        }
        m.moveKey(m.key(3).id, 0); // the ids are no longer in the order of the keys: 4, 1, 2, 3
        m.setMethod(pairOf(m, 0), SegMethod::Linear);
        m.setMethod(pairOf(m, 1), SegMethod::Smooth);
        m.setMethod(pairOf(m, 2), SegMethod::Generate);
        const KeyId second = m.key(1).id;
        m.removeKey(second); // the pair that replaces (4,1) and (1,2) takes the method of the one before: Linear
        CHECK(m.segment(0).method == SegMethod::Linear);
        // the layout version does not move when nothing about the layout does
        MotionTimeline one;
        one.addKey(pose(0.f));
        const U32 v = one.layoutVersion();
        CHECK(one.refresh(retarget) == TimelineResult::NeedsKeys && one.refresh(retarget) == TimelineResult::Ok && one.layoutVersion() == v);
        one.updateKey(one.key(0).id, pose(5.f));
        CHECK(one.refresh(retarget) == TimelineResult::NeedsKeys && one.layoutVersion() == v);
    }

    // indices out of range give nothing and read nothing; a refresh that fails leaves what was made
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(30.f));
        CHECK(tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(tl.key(-1).id == NO_KEY && tl.key(9).id == NO_KEY && tl.segment(9).pair.from == NO_KEY && tl.segment(-1).frames == 0);
        CHECK(tl.keyFrame(9) == 0 && tl.keyFrame(-3) == 0 && tl.keyPose(9).sl.local.empty() && tl.holdFrames(9) == 0);
        const U32 version = tl.clipVersion(), lv = tl.layoutVersion();
        const S32 frames = tl.clip().frame_count;
        history::Pose bad = pose(10.f);
        bad.joints[0].position = history::Vec3{ NAN, 0.f, 0.f };
        CHECK(tl.updateKey(tl.key(1).id, bad) == TimelineResult::Ok);
        CHECK(tl.refresh(retarget) == TimelineResult::BuildFailed && tl.stale());
        CHECK(tl.clipVersion() == version && tl.layoutVersion() == lv && tl.clip().frame_count == frames);
        CHECK(tl.keyPose(1).sl.local.size() == tl.clip().local.size() / (size_t)frames); // the key poses are the old ones too
        CHECK(tl.updateKey(tl.key(1).id, pose(35.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    }

    // a one-frame clip is not joined; a playback of no frames is refused
    {
        const KeyPose k0 = keyPoseOf(retarget, pose(0.f)), k1 = keyPoseOf(retarget, pose(40.f));
        MotionClip two, one, all;
        CHECK(makeInterpClip(k0, k1, 2, InterpCurve::Linear, two) == RetargetResult::Ok);
        one                 = two;
        one.frame_count     = 1;
        one.local.resize((size_t)one.joint_count);
        one.pelvis.resize(1);
        CHECK(concatClips({ &one }, { 0, 0 }, all) == ConcatResult::Mismatch);
        CHECK(concatClips({ &two, &one }, { 0, 0, 0 }, all) == ConcatResult::Mismatch);
        ClipPlayback pb;
        CHECK(!pb.load(0, 0, 10) && !pb.load(0, -5, 10) && pb.state() == ClipState::None);
        CHECK(pb.load(1, 0, 10) && !pb.reload(0, 0, 10));
    }

    // the yardstick: the answer's ends measured as the refit is (the same layout), so nothing is marked at first; a pelvis
    // that the model's end is far from is marked at once (15 cm), and 6 cm of change is marked, 4 cm is not
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(40.f));
        tl.requestGenerate(pairOf(tl, 0));
        answerNext(tl, retarget);
        const PairCache* c = tl.cache(pairOf(tl, 0));
        CHECK(c->answers[0].made_stats.end_max_deg == c->fit_stats.end_max_deg &&
              c->answers[0].made_stats.end_pelvis_m == c->fit_stats.end_pelvis_m && !c->suggest_regen);
        tl.updateKey(tl.key(1).id, pose(40.f, 0.04f));
        tl.refresh(retarget);
        CHECK(!tl.cache(pairOf(tl, 0))->suggest_regen);
        tl.updateKey(tl.key(1).id, pose(40.f, 0.06f));
        tl.refresh(retarget);
        CHECK(tl.cache(pairOf(tl, 0))->suggest_regen);
        MotionTimeline shifted;
        shifted.addKey(pose(0.f));
        shifted.addKey(pose(40.f));
        shifted.requestGenerate(pairOf(shifted, 0));
        answerNext(shifted, retarget, 0.6f); // the model's hips end 0.6 m (SOMA) off the key's pelvis
        const PairCache* fc = shifted.cache(pairOf(shifted, 0));
        CHECK(fc->fit_stats.end_pelvis_m >= REGEN_PELVIS_JUMP_M && fc->suggest_regen);
        MotionTimeline plain;
        plain.addKey(pose(0.f));
        plain.addKey(pose(40.f));
        plain.requestGenerate(pairOf(plain, 0));
        answerNext(plain, retarget, 0.f);
        CHECK(!plain.cache(pairOf(plain, 0))->suggest_regen);
    }

    // equality of the two keys is worked out again only when a pose was replaced, and it follows the replacement
    {
        MotionTimeline tl;
        tl.addKey(pose(0.f));
        tl.addKey(pose(0.f));
        tl.setMethod(pairOf(tl, 0), SegMethod::Smooth);
        CHECK(tl.segment(0).same_poses);
        tl.updateKey(tl.key(1).id, pose(20.f));
        CHECK(!tl.segment(0).same_poses && tl.setMethod(pairOf(tl, 0), SegMethod::Generate) == TimelineResult::Ok);
        tl.updateKey(tl.key(0).id, pose(20.f));
        CHECK(tl.segment(0).same_poses && tl.requestGenerate(pairOf(tl, 0)) == TimelineResult::SamePoses);
    }
}

// ---------------------------------------------------------------- research/361: passing through
static_assert(TIMELINE_CONTEXT_FRAMES == MOTION_CONTEXT_FRAMES && TIMELINE_MAX_TARGETS == MOTION_MAX_TARGETS);

static SomaPose somaOf(const SomaRetarget& retarget, const SlPose& p)
{
    SomaPose     out;
    RetargetWork work;
    CHECK(retarget.slToSoma(p, out, work) == RetargetResult::Ok);
    return out;
}

/// The largest distance between a joint of the two poses (m).
static double somaGap(const SomaPose& a, const SomaPose& b)
{
    double g = 0.0;
    for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
    {
        g = std::max(g, (double)(a.pos[(size_t)s] - b.pos[(size_t)s]).length());
    }
    return g;
}

/// The pose of a key of these tests: the elbow turned, the pelvis 5 mm per degree on, so that every point of the body moves a
/// good deal (the automatic judgement counts the mean over the 12 points: a bare elbow moves two of them).
static history::Pose elbowPose(F32 elbow_deg) { return pose(elbow_deg, elbow_deg * 0.005f); }

/// A timeline of keys that differ in the elbow (and the pelvis) only: a straight way for every point (a key between is passed when
/// Automatic), or there and back (a turning point).
static MotionTimeline elbows(std::initializer_list<F32> angles)
{
    MotionTimeline tl;
    tl.setPassingEnabled(true); // (the timeline's default is off, the window turns it on; the tests of passing want it on)
    for (F32 a : angles)
    {
        CHECK(tl.addKey(elbowPose(a)) == TimelineResult::Ok);
    }
    return tl;
}

/// Everything queued is asked and answered (the answers are made up from the context's last frame and the first target).
static void answerAll(MotionTimeline& tl, const SomaRetarget& retarget, S32 expect_jobs)
{
    S32 jobs = 0;
    while (auto job = tl.nextJob(retarget))
    {
        ++jobs;
        CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    }
    CHECK(jobs == expect_jobs && !tl.busy());
}

static void testAutoGolden(const std::string& golden)
{
    const json::value doc = readJson(golden + "auto_pass_golden.json");
    CHECK(doc.is_object());
    if (!doc.is_object())
    {
        return;
    }
    const json::array& cases = doc.as_object().at("cases").as_array();
    CHECK(cases.size() >= 5);
    S32 through = 0, stopped = 0;
    for (const json::value& cv : cases)
    {
        const json::object& c = cv.as_object();
        LLVector3           p[3][PASS_POINT_COUNT];
        const char*         names[3] = { "pos_A_sl_m", "pos_B_sl_m", "pos_C_sl_m" };
        for (S32 m = 0; m < 3; ++m)
        {
            const json::array& arr = c.at(names[m]).as_array();
            CHECK((S32)arr.size() == PASS_POINT_COUNT);
            for (S32 k = 0; k < PASS_POINT_COUNT && k < (S32)arr.size(); ++k)
            {
                p[m][k] = vec3(arr[(size_t)k]);
            }
        }
        const AutoPassResult r = judgeAutoPass(p[0], p[1], p[2]);
        // the golden's cos with every point counted alike (the first version of the file counted the lower body double and
        // keeps the other in cos_by_variant); the positions come from the SL rig, ours from SOMA: +-0.01
        // the cos that counts every point alike: the file's own when its lower body weight is 1 (the revised rule), else the
        // variant of the first version's file that has every weight 1
        double expect_cos = c.at("cos").to_number<double>();
        if (doc.as_object().at("thresholds").as_object().at("lower_weight").to_number<double>() != 1.0)
        {
            bool found = false;
            for (const auto& kv : c.at("cos_by_variant").as_object())
            {
                if (std::string(kv.key()).find("\xe9\x87\x8d\xe3\x81\xbf\xe5\x85\xa8\xe9\x83\xa8 1\xef\xbc\x88-0.3") == 0)
                {
                    expect_cos = kv.value().to_number<double>();
                    found      = true;
                }
            }
            CHECK(found);
        }
        const bool cos_ok = std::fabs((double)r.cos - expect_cos) < 0.01;
        CHECK(cos_ok);
        CHECK(std::fabs((double)r.move1_m * 100.0 - c.at("move1_cm").to_number<double>()) < 0.5);
        CHECK(std::fabs((double)r.move2_m * 100.0 - c.at("move2_cm").to_number<double>()) < 0.5);
        // a "via" (a key on the way) is passed, a "detour" (a turning point) is stopped at; the two near the line keep 0.13 off it
        const bool via = c.at("kind").as_string() == "via";
        CHECK(r.through == via);
        CHECK(std::fabs((double)r.cos - (double)PASS_AUTO_COS) > 0.12);
        (r.through ? through : stopped) += 1;
    }
    CHECK(through == 2 && stopped == 3); // the golden has both kinds

    // the rule's corners on made-up points: a still key, a straight way, there and back, the sign of the turning point
    LLVector3 a[PASS_POINT_COUNT], b[PASS_POINT_COUNT], c[PASS_POINT_COUNT];
    for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
    {
        a[k] = LLVector3(0.f, 0.f, (F32)k * 0.1f);
        b[k] = a[k] + LLVector3(0.10f, 0.f, 0.f); // 10 cm along x
        c[k] = b[k] + LLVector3(0.10f, 0.f, 0.f);
    }
    AutoPassResult r = judgeAutoPass(a, b, c);
    CHECK(r.through && r.cos > 0.999f && std::fabs(r.move1_m - 0.1f) < 1e-5f);
    r = judgeAutoPass(a, b, a); // there and back
    CHECK(!r.through && r.cos < -0.999f);
    r = judgeAutoPass(a, a, b); // the key does not move on the coming side
    CHECK(!r.through && r.move1_m == 0.f);
    LLVector3 near_b[PASS_POINT_COUNT];
    for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
    {
        near_b[k] = a[k] + LLVector3(0.029f, 0.f, 0.f); // 2.9 cm: under the 3 cm
    }
    r = judgeAutoPass(a, near_b, c);
    CHECK(!r.through);
    for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
    {
        near_b[k] = a[k] + LLVector3(0.031f, 0.f, 0.f);
    }
    r = judgeAutoPass(a, near_b, c); // 3.1 cm and on the same way: passed
    CHECK(r.through);
    // every point counts alike: the upper body goes on a little (+5 cm) while the legs and the hips go back (-10 cm): the
    // cosine is about -0.17, over the limit (counting the lower body double it would be about -0.46)
    LLVector3 d[PASS_POINT_COUNT];
    for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
    {
        b[k] = a[k] + LLVector3(0.10f, 0.f, 0.f);
        d[k] = b[k] + LLVector3(k >= 7 ? -0.10f : 0.05f, 0.f, 0.f);
    }
    r = judgeAutoPass(a, b, d);
    CHECK(r.through && r.cos < -0.15f && r.cos > -0.2f);
    for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
    {
        d[k] = b[k] + LLVector3(k >= 5 ? -0.10f : 0.05f, 0.f, 0.f); // 7 of 12 go back: cos = (5*0.005 - 7*0.01) / sqrt(0.12 * 0.0725)
    }
    r = judgeAutoPass(a, b, d);
    CHECK(!r.through && r.cos < -0.4f && r.cos > -0.55f);
}

static void testPassEligibility(const SomaRetarget& retarget)
{
    MotionTimeline tl = elbows({ 0.f, 40.f, 80.f, 120.f });
    const KeyId    a = tl.key(0).id, b = tl.key(1).id, c = tl.key(2).id, d = tl.key(3).id;
    CHECK(tl.passEligibility(a) == PassEligibility::EndKey && tl.passEligibility(d) == PassEligibility::EndKey);
    CHECK(tl.passEligibility(b) == PassEligibility::Ok && tl.passEligibility(c) == PassEligibility::Ok);
    CHECK(tl.passEligibility(9999) == PassEligibility::EndKey);
    // a hold stops it
    CHECK(tl.setHold(b, 0.01f) == TimelineResult::Ok && tl.passEligibility(b) == PassEligibility::Hold &&
          tl.passEligibility(c) == PassEligibility::Ok);
    CHECK(tl.setHold(b, 0.f) == TimelineResult::Ok && tl.passEligibility(b) == PassEligibility::Ok);
    // a Smooth or Linear segment next to it stops both of its keys
    CHECK(tl.setMethod(PairId{ b, c }, SegMethod::Smooth) == TimelineResult::Ok);
    CHECK(tl.passEligibility(b) == PassEligibility::NeighbourNotGenerate && tl.passEligibility(c) == PassEligibility::NeighbourNotGenerate);
    CHECK(tl.setMethod(PairId{ b, c }, SegMethod::Generate) == TimelineResult::Ok && tl.passEligibility(c) == PassEligibility::Ok);
    // the same pose twice (the way to stop at a key): the copy and the key lose it, the pair between is Smooth
    KeyId dup = NO_KEY;
    CHECK(tl.duplicateKey(b, &dup) == TimelineResult::Ok);
    CHECK(tl.passEligibility(b) == PassEligibility::NeighbourNotGenerate && tl.passEligibility(dup) == PassEligibility::NeighbourNotGenerate);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && !tl.key(tl.indexOf(b)).pass_effective && !tl.key(tl.indexOf(dup)).pass_effective);
    // Generate for equal poses is refused, but a key that was edited into the same pose as the neighbour: SamePose
    MotionTimeline same = elbows({ 0.f, 40.f, 80.f });
    CHECK(same.updateKey(same.key(2).id, elbowPose(40.f)) == TimelineResult::Ok);
    CHECK(same.passEligibility(same.key(1).id) == PassEligibility::SamePose);
    CHECK(std::string(describe(PassEligibility::Hold)) != std::string(describe(PassEligibility::SamePose)));
    CHECK(std::string(describe(TimelineResult::NeedPrev)) != "unknown" && std::string(describe(JobError::PrevMissing)) != "unknown");
    CHECK(!isWholeFailure(JobError::PrevMissing));
}

static void testPassFlags(const SomaRetarget& retarget)
{
    // Automatic: a straight way is passed; there and back, and hardly any move, are stopped at
    MotionTimeline straight = elbows({ 0.f, 40.f, 80.f });
    MotionTimeline back     = elbows({ 0.f, 80.f, 0.5f });
    MotionTimeline still    = elbows({ 0.f, 1.f, 2.f });
    CHECK(straight.refresh(retarget) == TimelineResult::Ok && back.refresh(retarget) == TimelineResult::Ok &&
          still.refresh(retarget) == TimelineResult::Ok);
    CHECK(straight.key(1).pass_effective && straight.key(1).pass_auto);
    CHECK(!back.key(1).pass_effective && !back.key(1).pass_auto);
    CHECK(!still.key(1).pass_effective);
    CHECK(!straight.key(0).pass_effective && !straight.key(2).pass_effective);
    CHECK(straight.key(1).pass == PassMode::Auto && straight.key(1).strength == PassStrength::Normal);
    // "Through" passes the turning point too, and only an eligible key
    CHECK(back.setPass(back.key(1).id, PassMode::Through) == TimelineResult::Ok && back.stale());
    CHECK(back.refresh(retarget) == TimelineResult::Ok && back.key(1).pass_effective && !back.key(1).pass_auto);
    CHECK(back.setPass(back.key(0).id, PassMode::Through) == TimelineResult::Ok);
    CHECK(back.refresh(retarget) == TimelineResult::Ok && !back.key(0).pass_effective);
    CHECK(back.setHold(back.key(1).id, 0.01f) == TimelineResult::Ok && back.refresh(retarget) == TimelineResult::Ok &&
          !back.key(1).pass_effective); // a hold stops even a key that is set to pass
    // the same value again changes nothing (no stale)
    CHECK(straight.setPass(straight.key(1).id, PassMode::Auto) == TimelineResult::Ok && !straight.stale());
    CHECK(straight.setStrength(straight.key(1).id, PassStrength::Normal) == TimelineResult::Ok && !straight.stale());
    CHECK(straight.setStrength(straight.key(1).id, PassStrength::Weak) == TimelineResult::Ok && straight.stale());
    CHECK(straight.setPass(9999, PassMode::Through) == TimelineResult::NoSuchKey &&
          straight.setStrength(9999, PassStrength::Weak) == TimelineResult::NoSuchKey);
    // the judgement is kept (the memo) and worked out again when a key around it changes or goes
    CHECK(straight.refresh(retarget) == TimelineResult::Ok);
    const TimelineKey::AutoMemo memo = straight.key(1).auto_memo;
    CHECK(memo.known && memo.through && memo.ver_next == straight.key(2).pose_ver);
    CHECK(straight.updateKey(straight.key(2).id, elbowPose(0.f)) == TimelineResult::Ok && straight.refresh(retarget) == TimelineResult::Ok);
    CHECK(!straight.key(1).pass_effective && straight.key(1).auto_memo.ver_next == straight.key(2).pose_ver); // 40 -> 0: back
    CHECK(straight.removeKey(straight.key(2).id) == TimelineResult::Ok && straight.refresh(retarget) == TimelineResult::Ok &&
          !straight.key(1).pass_effective); // it is the last key now
    // fewer than two keys: nothing is passed
    CHECK(straight.removeKey(straight.key(1).id) == TimelineResult::Ok && straight.refresh(retarget) == TimelineResult::NeedsKeys &&
          !straight.key(0).pass_effective);
}

/// What the window shows of a key's passing (passViewOf): every line of the rule, and no view left from a key that is gone.
static void testPassView(const SomaRetarget& retarget)
{
    MotionTimeline straight = elbows({ 0.f, 40.f, 80.f });
    MotionTimeline back     = elbows({ 0.f, 80.f, 0.5f });
    CHECK(straight.refresh(retarget) == TimelineResult::Ok && back.refresh(retarget) == TimelineResult::Ok);
    // nothing selected, or a key that is not there: the default view (Auto, Normal, nothing to press, no line)
    CHECK(passViewOf(straight, NO_KEY) == PassView() && passViewOf(straight, 9999) == PassView());
    // the middle key, Auto: judged "passes" (the strength counts) / "stops" (the strength does not; the line says how to pass)
    const PassView via = passViewOf(straight, straight.key(1).id);
    CHECK(via.key == straight.key(1).id && via.mode_enabled && via.judged == 1 && via.strength_enabled && via.note == PassNote::StopHint);
    const PassView turn = passViewOf(back, back.key(1).id);
    CHECK(turn.mode_enabled && turn.judged == 0 && !turn.strength_enabled && turn.note == PassNote::PassHint);
    // an end key: nothing to press, why
    const PassView end = passViewOf(straight, straight.key(0).id);
    CHECK(!end.mode_enabled && !end.strength_enabled && end.judged == -1 && end.note == PassNote::EndKey);
    // Through: no judgement, the strength counts; its value is shown
    CHECK(back.setPass(back.key(1).id, PassMode::Through) == TimelineResult::Ok && back.setStrength(back.key(1).id, PassStrength::Weak) == TimelineResult::Ok);
    const PassView through = passViewOf(back, back.key(1).id);
    CHECK(through.mode == PassMode::Through && through.strength == PassStrength::Weak && through.judged == -1 &&
          through.strength_enabled && through.note == PassNote::StopHint);
    // ... and refreshed (not stale): still no judgement for Through (pass_auto is not worked out for it)
    CHECK(back.refresh(retarget) == TimelineResult::Ok && !back.stale());
    const PassView through_now = passViewOf(back, back.key(1).id);
    CHECK(through_now.judged == -1 && through_now.mode_enabled && through_now.strength_enabled && through_now.note == PassNote::StopHint);
    // a Through key with a hold: its mode is shown, nothing to press, why
    CHECK(back.setHold(back.key(1).id, 0.5f) == TimelineResult::Ok && back.refresh(retarget) == TimelineResult::Ok);
    const PassView held = passViewOf(back, back.key(1).id);
    CHECK(held.mode == PassMode::Through && !held.mode_enabled && !held.strength_enabled && held.note == PassNote::Hold);
    CHECK(back.setHold(back.key(1).id, 0.f) == TimelineResult::Ok);
    // Auto again but not refreshed yet (stale): no judgement shown, so no strength from an old one
    CHECK(back.setPass(back.key(1).id, PassMode::Auto) == TimelineResult::Ok && back.stale());
    const PassView stale = passViewOf(back, back.key(1).id);
    CHECK(stale.mode_enabled && stale.judged == -1 && !stale.strength_enabled && stale.note == PassNote::StopHint);
    CHECK(straight.setStrength(straight.key(1).id, PassStrength::Weak) == TimelineResult::Ok && straight.stale() &&
          passViewOf(straight, straight.key(1).id).judged == -1 && !passViewOf(straight, straight.key(1).id).strength_enabled);
    CHECK(straight.refresh(retarget) == TimelineResult::Ok && passViewOf(straight, straight.key(1).id).judged == 1);
    // the reasons: a hold, a neighbour that is not Generate, the same pose next to it
    CHECK(straight.setHold(straight.key(1).id, 0.5f) == TimelineResult::Ok && passViewOf(straight, straight.key(1).id).note == PassNote::Hold &&
          !passViewOf(straight, straight.key(1).id).mode_enabled);
    CHECK(straight.setHold(straight.key(1).id, 0.f) == TimelineResult::Ok);
    CHECK(straight.setMethod(pairOf(straight, 0), SegMethod::Smooth) == TimelineResult::Ok &&
          passViewOf(straight, straight.key(1).id).note == PassNote::NeighbourNotGenerate);
    CHECK(straight.setMethod(pairOf(straight, 0), SegMethod::Generate) == TimelineResult::Ok);
    // a copy next to it (the pair between is Smooth then), and a pose made equal to a neighbour's (both segments stay Generate)
    KeyId copy = NO_KEY;
    CHECK(straight.duplicateKey(straight.key(1).id, &copy) == TimelineResult::Ok && straight.refresh(retarget) == TimelineResult::Ok);
    CHECK(passViewOf(straight, copy).note == PassNote::NeighbourNotGenerate &&
          passViewOf(straight, straight.key(1).id).note == PassNote::NeighbourNotGenerate);
    MotionTimeline same = elbows({ 0.f, 40.f, 80.f });
    CHECK(same.updateKey(same.key(1).id, elbowPose(0.f)) == TimelineResult::Ok && same.refresh(retarget) == TimelineResult::Ok);
    CHECK(same.segment(0).method == SegMethod::Generate && same.segment(0).same_poses);
    CHECK(passViewOf(same, same.key(1).id).note == PassNote::SamePose && !passViewOf(same, same.key(1).id).mode_enabled);
    // every key deleted (the last one too): the view of the key that was selected is the default again
    const KeyId last = straight.key(straight.keyCount() - 1).id;
    while (straight.keyCount() > 0)
    {
        CHECK(straight.removeKey(straight.key(0).id) == TimelineResult::Ok);
    }
    CHECK(passViewOf(straight, last) == PassView() && passViewOf(straight, copy) == PassView() && passViewOf(straight, NO_KEY) == PassView());
}

static void testPassRequests(const SomaRetarget& retarget)
{
    const S32 elbow = retarget.find("mElbowLeft");
    MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.key(1).pass_effective);
    const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
    CHECK(tl.requestAll() == 2);

    // (A,B): B is passed. The context is A four times, the targets B and the way on to C: 6, 12 and 18 % of it
    auto j0 = tl.nextJob(retarget);
    CHECK(j0 && j0->pair == p0 && j0->ntargets == 4);
    if (!j0)
    {
        return;
    }
    const SomaPose pose_a = somaOf(retarget, tl.keyPose(0).sl), pose_b = somaOf(retarget, tl.keyPose(1).sl);
    for (S32 k = 0; k < TIMELINE_CONTEXT_FRAMES; ++k)
    {
        CHECK(somaGap(j0->context[k], pose_a) < 1e-4);
    }
    CHECK(somaGap(j0->targets[0], pose_b) < 1e-4);
    for (S32 k = 1; k < TIMELINE_MAX_TARGETS; ++k)
    {
        KeyPose step;
        CHECK(blendKeyPose(tl.keyPose(1), tl.keyPose(2), 0.06 * k, step) == RetargetResult::Ok);
        CHECK(somaGap(j0->targets[k], somaOf(retarget, step.sl)) < 1e-4);
        // and that is where the elbow should be: 40 degrees and 6 % of the 40 on
        CHECK(angleBetween(step.sl.local[(size_t)elbow], LLQuaternion((40.f + 40.f * 0.06f * (F32)k) * DEG_TO_RAD, LLVector3::z_axis)) < 0.01);
    }
    CHECK(somaGap(j0->targets[1], j0->targets[0]) > 0.005 && somaGap(j0->targets[2], j0->targets[1]) > 0.005 &&
          somaGap(j0->targets[3], j0->targets[2]) > 0.005); // three different poses, going on one way
    CHECK(somaGap(j0->targets[2], pose_b) > somaGap(j0->targets[1], pose_b) && somaGap(j0->targets[3], pose_b) > somaGap(j0->targets[2], pose_b));
    CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *j0)) == AnswerFate::Used);

    // (B,C): the context is the last four frames of the answer before it (the last one is B), C is the last key: one target
    auto j1 = tl.nextJob(retarget);
    CHECK(j1 && j1->pair == p1 && j1->ntargets == 1);
    if (!j1)
    {
        return;
    }
    std::vector<SlPose> tail;
    CHECK(tailPoses(tl.cache(p0)->clip, 4, tail) && tail.size() == 4);
    if (tail.size() == 4)
    {
        for (S32 k = 0; k < TIMELINE_CONTEXT_FRAMES; ++k)
        {
            CHECK(somaGap(j1->context[k], somaOf(retarget, tail[(size_t)k])) < 1e-5);
        }
    }
    CHECK(somaGap(j1->context[3], pose_b) < 1e-3);                  // it ends at B
    CHECK(somaGap(j1->context[0], j1->context[3]) > 0.003);         // and it is moving: B four times is not that
    CHECK(somaGap(j1->context[1], j1->context[0]) > 1e-4 && somaGap(j1->context[3], j1->context[2]) > 1e-4);
    CHECK(somaGap(j1->targets[0], somaOf(retarget, tl.keyPose(2).sl)) < 1e-4);
    CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *j1)) == AnswerFate::Used);
    // what the answers remember they were asked with
    const SegmentAnswer& a0 = tl.cache(p0)->answers[0];
    const SegmentAnswer& a1 = tl.cache(p1)->answers[0];
    CHECK(a0.used.made && !a0.used.ctx_moving && a0.used.to_through && a0.used.to_strength == PassStrength::Normal);
    CHECK(a1.used.made && a1.used.ctx_moving && !a1.used.to_through && a1.used.ctx_serial == a0.serial && a0.serial != 0 &&
          a1.serial != a0.serial && a1.used.ctx_last2.size() == 2);

    // a weak key: 3, 6 and 9 %
    MotionTimeline weak = elbows({ 0.f, 40.f, 80.f });
    CHECK(weak.setStrength(weak.key(1).id, PassStrength::Weak) == TimelineResult::Ok && weak.requestAll() == 2);
    auto w0 = weak.nextJob(retarget);
    CHECK(w0 && w0->ntargets == 4);
    if (w0)
    {
        for (S32 k = 1; k < TIMELINE_MAX_TARGETS; ++k)
        {
            KeyPose step;
            CHECK(blendKeyPose(weak.keyPose(1), weak.keyPose(2), 0.03 * k, step) == RetargetResult::Ok);
            CHECK(somaGap(w0->targets[k], somaOf(retarget, step.sl)) < 1e-4);
        }
    }

    // four keys, both between passed: (B,C) has the context of (A,B) and the targets C, and 6, 12, 18 % of the way to D
    MotionTimeline four = elbows({ 0.f, 40.f, 80.f, 120.f });
    CHECK(four.refresh(retarget) == TimelineResult::Ok && four.key(1).pass_effective && four.key(2).pass_effective);
    CHECK(four.requestAll() == 3);
    answerNext(four, retarget);
    auto f1 = four.nextJob(retarget);
    CHECK(f1 && f1->pair == pairOf(four, 1) && f1->ntargets == 4);
    if (f1)
    {
        KeyPose step;
        CHECK(blendKeyPose(four.keyPose(2), four.keyPose(3), 0.12, step) == RetargetResult::Ok);
        CHECK(somaGap(f1->targets[2], somaOf(retarget, step.sl)) < 1e-4 && somaGap(f1->targets[0], somaOf(retarget, four.keyPose(2).sl)) < 1e-4);
        CHECK(somaGap(f1->context[3], somaOf(retarget, four.keyPose(1).sl)) < 1e-3 && somaGap(f1->context[0], f1->context[3]) > 0.003);
        CHECK(four.onAnswer(retarget, fakeAnswer(retarget, *f1)) == AnswerFate::Used);
    }
    auto f2 = four.nextJob(retarget);
    CHECK(f2 && f2->pair == pairOf(four, 2) && f2->ntargets == 1);
    if (f2)
    {
        CHECK(somaGap(f2->context[3], somaOf(retarget, four.keyPose(2).sl)) < 1e-3 && somaGap(f2->context[0], f2->context[3]) > 0.003);
    }
}

static void testPassQueue(const SomaRetarget& retarget)
{
    // 1. the segment before it has no answer: it cannot be asked on its own (NeedPrev); queued, or answered, it can
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        CHECK(tl.refresh(retarget) == TimelineResult::Ok);
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
        CHECK(tl.requestGenerate(p1) == TimelineResult::NeedPrev && tl.segment(1).state == SegState::Unmade && tl.queuedCount() == 0);
        CHECK(tl.requestGenerate(p0) == TimelineResult::Ok && tl.requestGenerate(p1) == TimelineResult::Ok && tl.queuedCount() == 2);
        answerAll(tl, retarget, 2);
        CHECK(tl.segment(0).state == SegState::Made && tl.segment(1).state == SegState::Made);
        // an old answer is enough to go on from
        CHECK(tl.requestGenerate(p1) == TimelineResult::Ok);
        answerAll(tl, retarget, 1);
        // a failed segment has none
        MotionTimeline fl = elbows({ 0.f, 40.f, 80.f });
        CHECK(fl.requestGenerate(pairOf(fl, 0)) == TimelineResult::Ok && fl.nextJob(retarget) && !fl.onFailure(JobError::RunFailed));
        CHECK(fl.segment(0).state == SegState::Failed && fl.requestGenerate(pairOf(fl, 1)) == TimelineResult::NeedPrev);
        // "generate all" asks the segment before first
        MotionTimeline all = elbows({ 0.f, 40.f, 80.f });
        CHECK(all.requestAll() == 2);
        auto first = all.nextJob(retarget);
        CHECK(first && first->pair == pairOf(all, 0));
    }

    // 2. the queue [(B,C),(A,B)]: (B,C) waits (stays queued), (A,B) goes first, then (B,C) with the new answer's end
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
        const U32    old_serial = tl.cache(p0)->answers[0].serial;
        CHECK(tl.requestGenerate(p1) == TimelineResult::Ok && tl.requestGenerate(p0) == TimelineResult::Ok && tl.queuedCount() == 2);
        auto job = tl.nextJob(retarget);
        CHECK(job && job->pair == p0 && tl.queuedCount() == 1 && tl.segment(1).state == SegState::Queued && tl.running());
        CHECK(!tl.nextJob(retarget)); // one at a time
        CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
        const U32 new_serial = tl.cache(p0)->answers[0].serial;
        CHECK(new_serial != old_serial);
        job = tl.nextJob(retarget);
        CHECK(job && job->pair == p1 && tl.queuedCount() == 0);
        CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
        CHECK(tl.cache(p1)->answers[0].used.ctx_serial == new_serial && tl.segment(1).regen_reasons == 0 && tl.segment(0).regen_reasons == 0);
    }

    // 3. while (B,C) waits, the answer before it goes away or the key stops being passed: no waiting for ever
    {
        // (A,B) loses its answer (a new length) -> (B,C) goes back with a note, nothing is asked, nothing is left queued
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
        CHECK(tl.requestGenerate(p1) == TimelineResult::Ok && tl.queuedCount() == 1);
        CHECK(tl.setTokens(p0, 6) == TimelineResult::Ok && tl.segment(0).state == SegState::Unmade && tl.segment(0).selected == -1);
        CHECK(!tl.nextJob(retarget) && tl.queuedCount() == 0 && !tl.running() && !tl.busy());
        CHECK(tl.segment(1).state == SegState::Made && tl.segment(1).fail == JobError::PrevMissing); // it has an answer: S3, with the note
        CHECK(tl.requestGenerate(p1) == TimelineResult::NeedPrev);                                   // and it is not asked on its own now
    }
    {
        // the same for a segment that has no answer: S0 with the note; asking again clears the note
        MotionTimeline u = elbows({ 0.f, 40.f, 80.f });
        u.requestAll();
        answerNext(u, retarget); // (A,B) is made, (B,C) is queued
        CHECK(u.setTokens(pairOf(u, 0), 8) == TimelineResult::Ok && u.segment(0).state == SegState::Unmade);
        CHECK(!u.nextJob(retarget) && u.segment(1).state == SegState::Unmade && u.segment(1).fail == JobError::PrevMissing && !u.busy());
        CHECK(u.requestGenerate(pairOf(u, 0)) == TimelineResult::Ok && u.requestGenerate(pairOf(u, 1)) == TimelineResult::Ok &&
              u.segment(1).fail == JobError::None);
    }
    {
        // the key stops being passed while (B,C) waits behind (A,B): the plain context, no waiting
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
        CHECK(tl.requestGenerate(p1) == TimelineResult::Ok && tl.requestGenerate(p0) == TimelineResult::Ok);
        CHECK(tl.setMethod(p0, SegMethod::Smooth) == TimelineResult::Busy); // queued: refused (an edit never cancels)
        CHECK(tl.setHold(tl.key(1).id, 0.01f) == TimelineResult::Ok);
        auto job = tl.nextJob(retarget); // (B,C) was queued first and B is not passed now
        CHECK(job && job->pair == p1 && job->ntargets == 1 && tl.queuedCount() == 1);
        if (job)
        {
            CHECK(somaGap(job->context[0], job->context[3]) < 1e-6); // B four times
        }
        CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
        CHECK(tl.cache(p1)->answers[0].used.made && !tl.cache(p1)->answers[0].used.ctx_moving);
        answerAll(tl, retarget, 1);
    }
    {
        // the method of (A,B) changes while (B,C) waits and (A,B) is not queued: B is not eligible any more
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
        CHECK(tl.requestGenerate(p1) == TimelineResult::Ok);
        CHECK(tl.setMethod(p0, SegMethod::Smooth) == TimelineResult::Ok);
        auto job = tl.nextJob(retarget);
        CHECK(job && job->pair == p1 && job->ntargets == 1);
        if (job)
        {
            CHECK(somaGap(job->context[0], job->context[3]) < 1e-6);
        }
    }

    // 4. a chain of three: asked and answered in order; a failure of the first puts the rest back (never a job that waits for ever)
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f, 120.f });
        CHECK(tl.requestAll() == 3);
        for (S32 i = 0; i < 3; ++i)
        {
            auto job = tl.nextJob(retarget);
            CHECK(job && job->pair == pairOf(tl, i) && job->ntargets == (i < 2 ? 4 : 1));
            if (job)
            {
                CHECK(tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
            }
        }
        MotionTimeline f = elbows({ 0.f, 40.f, 80.f, 120.f });
        CHECK(f.requestAll() == 3);
        CHECK(f.nextJob(retarget) && !f.onFailure(JobError::RunFailed));
        CHECK(f.segment(0).state == SegState::Failed && f.queuedCount() == 2);
        CHECK(!f.nextJob(retarget)); // (B,C) has nothing to go on from: it and (C,D) go back, nothing is out
        CHECK(f.queuedCount() == 0 && !f.busy() && f.segment(1).state == SegState::Unmade && f.segment(2).state == SegState::Unmade &&
              f.segment(1).fail == JobError::PrevMissing && f.segment(2).fail == JobError::PrevMissing);
        // a stop in the chain: (B,C) does not depend on (A,B) when B is stopped at
        MotionTimeline g = elbows({ 0.f, 40.f, 80.f, 120.f });
        CHECK(g.setHold(g.key(1).id, 0.01f) == TimelineResult::Ok && g.requestAll() == 3);
        CHECK(g.nextJob(retarget) && !g.onFailure(JobError::RunFailed));
        auto job = g.nextJob(retarget);
        CHECK(job && job->pair == pairOf(g, 1) && job->ntargets == 4 && somaGap(job->context[0], job->context[3]) < 1e-6);
        CHECK(g.queuedCount() == 1);
    }

    // 5. a failed new answer of the segment before it leaves the old answer: the one after it goes on from that
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
        const U32    serial = tl.cache(p0)->answers[0].serial;
        CHECK(tl.requestGenerate(p1) == TimelineResult::Ok && tl.requestGenerate(p0) == TimelineResult::Ok);
        CHECK(tl.nextJob(retarget) && !tl.onFailure(JobError::RunFailed));
        CHECK(tl.segment(0).state == SegState::Made && tl.segment(0).fail == JobError::RunFailed);
        auto job = tl.nextJob(retarget);
        CHECK(job && job->pair == p1 && somaGap(job->context[0], job->context[3]) > 0.003);
        CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used && tl.cache(p1)->answers[0].used.ctx_serial == serial);
    }

    // 6. the whole run fails with (B,C) waiting: both go back (S0 / S3), nothing is left; cancel too
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        CHECK(tl.nextJob(retarget) && tl.onFailure(JobError::Timeout));
        CHECK(tl.segment(0).state == SegState::Unmade && tl.segment(1).state == SegState::Unmade && !tl.busy());
        tl.requestAll();
        CHECK(tl.nextJob(retarget) && tl.cancelGeneration() && tl.segment(1).state == SegState::Unmade && !tl.busy());
    }

    // 7. it never spins: a queue that is not empty with nothing running gives a job or gets shorter, whatever the state
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f, 120.f, 160.f });
        tl.requestAll();
        S32 guard = 0;
        while (tl.queuedCount() > 0 && !tl.running() && guard++ < 50)
        {
            const S32 before = tl.queuedCount();
            auto      job    = tl.nextJob(retarget);
            CHECK(job || tl.queuedCount() < before);
            if (job && guard == 3)
            {
                CHECK(!tl.onFailure(JobError::BadInput)); // the third fails: the passed ones after it go back
                continue;
            }
            if (job)
            {
                tl.onAnswer(retarget, fakeAnswer(retarget, *job));
            }
        }
        CHECK(guard < 50 && !tl.busy());
        // the queue [(B,C),(A,B)] with (B,C) behind a segment that is a Smooth one now is not stuck either
        MotionTimeline m = elbows({ 0.f, 40.f, 80.f });
        m.requestAll();
        m.setHold(m.key(1).id, 0.01f); // stopped at: both go on by themselves
        guard = 0;
        while (m.queuedCount() > 0 && guard++ < 10)
        {
            auto job = m.nextJob(retarget);
            CHECK(job);
            if (job)
            {
                m.onAnswer(retarget, fakeAnswer(retarget, *job));
            }
        }
        CHECK(!m.busy() && m.segment(0).state == SegState::Made && m.segment(1).state == SegState::Made);
    }
}

static void testPassMarks(const SomaRetarget& retarget)
{
    auto           regenOf = [](const MotionTimeline& t, S32 i) { return t.segment(i).regen_reasons; };
    MotionTimeline tl      = elbows({ 0.f, 40.f, 80.f });
    tl.requestAll();
    answerAll(tl, retarget, 2);
    const KeyId  b  = tl.key(1).id;
    const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && regenOf(tl, 0) == 0 && regenOf(tl, 1) == 0 && !tl.segment(0).ctx_stale);

    // a small change of B (4 degrees) is under the thresholds: no mark
    CHECK(tl.updateKey(b, elbowPose(44.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK(regenOf(tl, 0) == 0 && regenOf(tl, 1) == 0);
    // a big one (30 degrees): the segment into B is marked (its target moved); the one out of B only for its own ends
    // (the marks do not run on along the chain: the answer before it is the same one)
    CHECK(tl.updateKey(b, elbowPose(70.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK((regenOf(tl, 0) & REGEN_TARGETS) && tl.segment(0).ctx_stale);
    CHECK(regenOf(tl, 1) == REGEN_ENDS && !tl.segment(1).ctx_stale && tl.segment(1).suggest_regen);
    // put it back: the marks go (a value worked out again, not a state that stays)
    CHECK(tl.updateKey(b, elbowPose(40.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK(regenOf(tl, 0) == 0 && regenOf(tl, 1) == 0 && !tl.segment(0).ctx_stale);

    // B stops being passed (a hold): both are marked, the answers stay; back to what it was: clean
    CHECK(tl.setHold(b, 0.01f) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK((regenOf(tl, 0) & REGEN_PASS) && (regenOf(tl, 1) & REGEN_PASS) && tl.segment(0).answer_count == 1 && tl.segment(1).answer_count == 1);
    CHECK(tl.setHold(b, 0.f) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok && regenOf(tl, 0) == 0 && regenOf(tl, 1) == 0);
    // the strength: only the segment whose targets used it
    CHECK(tl.setStrength(b, PassStrength::Weak) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK((regenOf(tl, 0) & REGEN_PASS) && regenOf(tl, 1) == 0);
    CHECK(tl.setStrength(b, PassStrength::Normal) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok && regenOf(tl, 0) == 0);
    // the answer before it is another one: the segment out of B is marked (context), the new one is not
    CHECK(tl.requestGenerate(p0) == TimelineResult::Ok);
    answerAll(tl, retarget, 1);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && regenOf(tl, 0) == 0 && (regenOf(tl, 1) & REGEN_CONTEXT) && tl.segment(1).ctx_stale);
    // both kinds at once: one mark with both reasons
    CHECK(tl.updateKey(b, elbowPose(70.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK(regenOf(tl, 1) == (REGEN_ENDS | REGEN_CONTEXT) && tl.segment(1).suggest_regen && tl.segment(1).ctx_stale);
    CHECK(tl.updateKey(b, elbowPose(40.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    // asked again, it is clean
    CHECK(tl.requestGenerate(p1) == TimelineResult::Ok);
    answerAll(tl, retarget, 1);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && regenOf(tl, 0) == 0 && regenOf(tl, 1) == 0);

    // the record is what was SENT: B changes while the job is out, the answer is marked when it comes
    MotionTimeline out = elbows({ 0.f, 40.f, 80.f });
    out.requestGenerate(pairOf(out, 0));
    auto job = out.nextJob(retarget);
    CHECK(job && job->ntargets == 4);
    CHECK(out.updateKey(out.key(1).id, elbowPose(70.f)) == TimelineResult::Ok);
    CHECK(job && out.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(out.refresh(retarget) == TimelineResult::Ok && (out.segment(0).regen_reasons & REGEN_TARGETS));
    // B loses its eligibility while the job is out (it was asked with the way on past B)
    MotionTimeline lost = elbows({ 0.f, 40.f, 80.f });
    lost.requestGenerate(pairOf(lost, 0));
    job = lost.nextJob(retarget);
    CHECK(job && lost.setHold(lost.key(1).id, 0.01f) == TimelineResult::Ok);
    CHECK(job && lost.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(lost.refresh(retarget) == TimelineResult::Ok && (lost.segment(0).regen_reasons & REGEN_PASS));

    // a segment that was not asked with passing and still is not: nothing but its own mark
    MotionTimeline plain = elbows({ 0.f, 40.f, 80.f });
    plain.setHold(plain.key(1).id, 0.01f);
    plain.requestAll();
    answerAll(plain, retarget, 2);
    CHECK(plain.refresh(retarget) == TimelineResult::Ok && regenOf(plain, 0) == 0 && regenOf(plain, 1) == 0 &&
          !plain.cache(pairOf(plain, 0))->answers[0].used.to_through);
    CHECK(plain.updateKey(plain.key(1).id, elbowPose(70.f)) == TimelineResult::Ok && plain.refresh(retarget) == TimelineResult::Ok);
    CHECK(!plain.segment(0).ctx_stale && !plain.segment(1).ctx_stale); // "the ends moved" is suggest_regen's mark

    // the rest skeleton changes (invalidateClips): the judgement is made again, the marks stay what they are
    tl.invalidateClips();
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.key(1).pass_effective && regenOf(tl, 0) == 0 && regenOf(tl, 1) == 0);
    // a joint driven in every key now: the layout changes (every pose has all the joints, so nothing asked differs)
    {
        MotionTimeline lay = elbows({ 0.f, 40.f, 80.f });
        lay.requestAll();
        answerAll(lay, retarget, 2);
        const U32 layout_before = lay.layoutVersion();
        for (S32 i = 0; i < 3; ++i)
        {
            history::Pose p = elbowPose((F32)(i * 40));
            p.joints.push_back(joint("mHipRight", LLQuaternion(0.1f, LLVector3::y_axis)));
            CHECK(lay.updateKey(lay.key(i).id, p) == TimelineResult::Ok);
        }
        CHECK(lay.refresh(retarget) == TimelineResult::Ok && lay.layoutVersion() != layout_before);
        CHECK(!lay.segment(1).ctx_stale && !lay.segment(0).ctx_stale); // a joint more is no change of what was asked
    }
}

static void testPassChain(const SomaRetarget& retarget)
{
    MotionTimeline tl = elbows({ 0.f, 40.f, 80.f, 120.f });
    CHECK(tl.refresh(retarget) == TimelineResult::Ok);
    const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1), p2 = pairOf(tl, 2);
    CHECK(tl.chainFrom(p0) == std::vector<PairId>({ p0, p1, p2 }));
    CHECK(tl.chainFrom(p1) == std::vector<PairId>({ p1, p2 }));
    CHECK(tl.chainFrom(p2) == std::vector<PairId>({ p2 }));
    CHECK(tl.chainFrom(PairId{ p0.to, p0.from }).empty() && tl.chainFrom(PairId{ 99, 100 }).empty());
    // C is stopped at: the chain ends with the segment into it
    CHECK(tl.setHold(tl.key(2).id, 0.01f) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
    CHECK(tl.chainFrom(p0) == std::vector<PairId>({ p0, p1 }) && tl.chainFrom(p1) == std::vector<PairId>({ p1 }) &&
          tl.chainFrom(p2) == std::vector<PairId>({ p2 }));
    CHECK(tl.setHold(tl.key(2).id, 0.f) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);

    // the whole chain asked again, the segments that are made too (the ones without a mark included)
    CHECK(tl.requestAll() == 3);
    answerAll(tl, retarget, 3);
    S32 queued = 0;
    CHECK(tl.requestChain(p0, &queued) == TimelineResult::Ok && queued == 3 && tl.queuedCount() == 3);
    CHECK(tl.requestChain(p1, &queued) == TimelineResult::Busy && queued == 0 && tl.queuedCount() == 3); // nothing more is queued
    answerAll(tl, retarget, 3);
    CHECK(tl.requestChain(p1, &queued) == TimelineResult::Ok && queued == 2 && tl.queuedCount() == 2);
    answerAll(tl, retarget, 2);
    // the chain from a segment whose from key is passed needs the one before it
    MotionTimeline fresh = elbows({ 0.f, 40.f, 80.f, 120.f });
    CHECK(fresh.refresh(retarget) == TimelineResult::Ok);
    // the window's checks say what the calls will (pt-wire: the buttons are greyed by them)
    CHECK(fresh.canRequestGenerate(pairOf(fresh, 1)) == TimelineResult::NeedPrev && fresh.canRequestChain(pairOf(fresh, 1)) == TimelineResult::NeedPrev);
    CHECK(fresh.canRequestGenerate(pairOf(fresh, 0)) == TimelineResult::Ok && fresh.canRequestChain(pairOf(fresh, 0)) == TimelineResult::Ok);
    CHECK(fresh.canRequestChain(PairId{ 99, 100 }) == TimelineResult::NoSuchPair && fresh.queuedCount() == 0);
    CHECK(fresh.requestChain(pairOf(fresh, 1), &queued) == TimelineResult::NeedPrev && queued == 0 && fresh.queuedCount() == 0 &&
          fresh.segment(1).state == SegState::Unmade);
    CHECK(fresh.requestChain(pairOf(fresh, 0), &queued) == TimelineResult::Ok && queued == 3);
    // queued: a segment of the chain is busy, so the whole chain is refused, and the queued one itself is Busy
    CHECK(fresh.canRequestChain(pairOf(fresh, 0)) == TimelineResult::Busy && fresh.canRequestGenerate(pairOf(fresh, 1)) == TimelineResult::Busy);
    CHECK(fresh.requestChain(PairId{ 99, 100 }, &queued) == TimelineResult::NoSuchPair && queued == 0);
    // a segment that is not Generate: refused
    MotionTimeline sm = elbows({ 0.f, 40.f, 80.f });
    sm.setMethod(pairOf(sm, 1), SegMethod::Smooth);
    CHECK(sm.requestChain(pairOf(sm, 1)) == TimelineResult::BadValue && sm.canRequestChain(pairOf(sm, 1)) == TimelineResult::BadValue);
}

static void testPassClips(const SomaRetarget& retarget)
{
    const S32           elbow = retarget.find("mElbowLeft");
    const KeyPose       k0    = keyPoseOf(retarget, pose(0.f, 0.f));
    TrackLayout         layout;
    std::vector<KeyPose> keys;
    const history::Pose pa = pose(0.f, 0.f), pb = pose(80.f, 0.1f, 40.f);
    CHECK(makeKeyPoses(retarget, std::vector<const history::Pose*>{ &pa, &pb }, layout, keys) == TrackResult::Ok && keys.size() == 2);
    if (keys.size() != 2)
    {
        return;
    }
    const KeyPose &a = keys[0], &b = keys[1];
    KeyPose        mid;
    CHECK(blendKeyPose(a, b, 0.25, mid) == RetargetResult::Ok);
    CHECK(angleBetween(mid.sl.local[(size_t)elbow], LLQuaternion(20.f * DEG_TO_RAD, LLVector3::z_axis)) < 0.01);
    CHECK(std::fabs(mid.sl.pelvis.mV[VX] - (a.sl.pelvis.mV[VX] + 0.25f * (b.sl.pelvis.mV[VX] - a.sl.pelvis.mV[VX]))) < 1e-6f);
    // the ends; and the same pose as a frame of makeInterpClip (it is one blend: the Linear clip goes by the same t)
    KeyPose e0, e1;
    CHECK(blendKeyPose(a, b, 0.0, e0) == RetargetResult::Ok && blendKeyPose(a, b, 1.0, e1) == RetargetResult::Ok);
    MotionClip lin;
    CHECK(makeInterpClip(a, b, 41, InterpCurve::Linear, lin) == RetargetResult::Ok);
    for (S32 j = 0; j < lin.joint_count; ++j)
    {
        CHECK(angleBetween(e0.sl.local[(size_t)j], a.sl.local[(size_t)j]) < 0.01 && angleBetween(e1.sl.local[(size_t)j], b.sl.local[(size_t)j]) < 0.01);
        CHECK(angleBetween(mid.sl.local[(size_t)j], lin.local[(size_t)10 * lin.joint_count + j]) < 0.01); // frame 10 of 40: 0.25
    }
    CHECK((mid.sl.pelvis - lin.pelvis[10]).length() < 1e-6f);
    // past 1 the same curve goes on
    KeyPose past;
    CHECK(blendKeyPose(a, b, 1.2, past) == RetargetResult::Ok);
    CHECK(angleBetween(past.sl.local[(size_t)elbow], LLQuaternion(96.f * DEG_TO_RAD, LLVector3::z_axis)) < 0.01);
    // the other sign of every rotation (the same turn): the same pose, on a's side of the sphere
    KeyPose flipped = b;
    for (LLQuaternion& q : flipped.sl.local)
    {
        q = LLQuaternion(-q.mQ[VX], -q.mQ[VY], -q.mQ[VZ], -q.mQ[VW]);
    }
    KeyPose mid2;
    CHECK(blendKeyPose(a, flipped, 0.25, mid2) == RetargetResult::Ok);
    for (S32 j = 0; j < lin.joint_count; ++j)
    {
        CHECK(angleBetween(mid.sl.local[(size_t)j], mid2.sl.local[(size_t)j]) < 0.01 && dotOf(a.sl.local[(size_t)j], mid2.sl.local[(size_t)j]) >= 0.0);
    }
    // refused, and out is not touched
    KeyPose keep = k0;
    CHECK(blendKeyPose(a, b, NAN, keep) == RetargetResult::BadInput && keep.sl.local.size() == k0.sl.local.size());
    KeyPose shorter = b;
    shorter.sl.local.pop_back();
    CHECK(blendKeyPose(a, shorter, 0.5, keep) == RetargetResult::BadInput);
    KeyPose degenerate = b;
    degenerate.sl.local[0] = LLQuaternion(0.f, 0.f, 0.f, 0.f);
    CHECK(blendKeyPose(a, degenerate, 0.5, keep) == RetargetResult::BadInput);
    CHECK(blendKeyPose(KeyPose(), KeyPose(), 0.5, keep) == RetargetResult::BadInput);
    CHECK((keep.sl.pelvis - k0.sl.pelvis).length() < 1e-9f);

    // tailPoses: the last frames in time order, the last one is the clip's last
    std::vector<SlPose> tail{ SlPose() };
    CHECK(tailPoses(lin, 4, tail) && tail.size() == 4);
    for (size_t k = 0; k < tail.size() && tail.size() == 4; ++k)
    {
        const S32 f = lin.frame_count - 4 + (S32)k;
        CHECK((S32)tail[k].local.size() == lin.joint_count && (tail[k].pelvis - lin.pelvis[(size_t)f]).length() < 1e-7f);
        for (S32 j = 0; j < lin.joint_count; ++j)
        {
            CHECK(angleBetween(tail[k].local[(size_t)j], lin.local[(size_t)f * lin.joint_count + j]) < 1e-3);
        }
    }
    CHECK(tailPoses(lin, lin.frame_count, tail) && (S32)tail.size() == lin.frame_count);
    std::vector<SlPose> untouched{ SlPose(), SlPose() };
    CHECK(!tailPoses(lin, 0, untouched) && !tailPoses(lin, lin.frame_count + 1, untouched) && untouched.size() == 2);
    CHECK(!tailPoses(MotionClip(), 1, untouched));
    MotionClip broken = lin;
    broken.pelvis.pop_back();
    CHECK(!tailPoses(broken, 4, untouched) && untouched.size() == 2);

    // tailStepDiffers: the speed at the end of the clip against two frames it was asked with
    MotionClip ease;
    CHECK(makeInterpClip(a, b, 41, InterpCurve::EaseInOut, ease) == RetargetResult::Ok);
    std::vector<SlPose> last2;
    CHECK(tailPoses(ease, 2, last2) && last2.size() == 2);
    SlPose f19, f20;
    f19.local.assign(&ease.local[(size_t)19 * ease.joint_count], &ease.local[(size_t)19 * ease.joint_count] + ease.joint_count);
    f20.local.assign(&ease.local[(size_t)20 * ease.joint_count], &ease.local[(size_t)20 * ease.joint_count] + ease.joint_count);
    CHECK(!tailStepDiffers(last2, ease, 0.01f, 1.f));                                   // its own end: the same speed
    CHECK(tailStepDiffers(std::vector<SlPose>{ f19, f20 }, ease, 1.f, 1.f));            // the middle of an ease: fast, the end is slow
    CHECK(!tailStepDiffers(std::vector<SlPose>{ f19, f20 }, ease, 20.f, 1.f));          // under the limit it is the same
    CHECK(tailStepDiffers(std::vector<SlPose>{ f19 }, ease, 1.f, 1.f) && tailStepDiffers({}, ease, 1.f, 1.f)); // not two poses
    SlPose shortened = f19;
    shortened.local.pop_back();
    CHECK(tailStepDiffers(std::vector<SlPose>{ shortened, f20 }, ease, 1.f, 1.f));      // another set of joints
    CHECK(tailStepDiffers(last2, MotionClip(), 1.f, 1.f) && tailStepDiffers(last2, broken, 1.f, 1.f));
    // the pelvis's step counts too (a joint's step is the same, the hips are moving otherwise)
    std::vector<SlPose> hips = last2;
    hips[1].pelvis += LLVector3(0.05f, 0.f, 0.f);
    CHECK(tailStepDiffers(hips, ease, 20.f, 0.01f) && !tailStepDiffers(hips, ease, 20.f, 0.1f));
    CHECK(!tailStepDiffers(last2, ease, 20.f, 1e-4f)); // its own two frames, a narrow limit: the same frames are compared (the ease's end steps differ by about 3e-4 m from the ones before)
}

// ---------------------------------------------------------------- the review of research/361 (pt-core), each a case that was missed
static void testPassReview(const SomaRetarget& retarget)
{
    // the 12 points are the joints they say: head, elbows, wrists, middle finger ends, knees, ankles, hips
    {
        SomaPose all;
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            all.pos[(size_t)s] = LLVector3((F32)s, 0.f, 0.f);
        }
        LLVector3 pts[PASS_POINT_COUNT];
        passPoints(all, pts);
        const char* names[PASS_POINT_COUNT] = { "Head", "LeftForeArm", "RightForeArm", "LeftHand", "RightHand", "LeftHandMiddleEnd",
                                                "RightHandMiddleEnd", "LeftShin", "RightShin", "LeftFoot", "RightFoot", "Hips" };
        for (S32 k = 0; k < PASS_POINT_COUNT; ++k)
        {
            S32 index = -1;
            for (S32 s = 0; s < soma::JOINT_COUNT; ++s)
            {
                if (std::string(soma::REST[s].name) == names[k])
                {
                    index = s;
                }
            }
            CHECK(index >= 0 && (S32)pts[k].mV[VX] == index);
        }
    }

    // the speed of the pelvis at the end of the answer before it counts: B goes a metre on with the same elbow, the same answer
    // stays in use, the segment out of B is marked for its context (the joints' steps are the same)
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        CHECK(tl.setPass(tl.key(1).id, PassMode::Through) == TimelineResult::Ok); // (the pelvis goes there and back: Automatic would stop)
        tl.requestAll();
        answerAll(tl, retarget, 2);
        CHECK(tl.updateKey(tl.key(1).id, pose(40.f, 1.2f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(tl.key(1).pass_effective && (tl.segment(1).regen_reasons & REGEN_CONTEXT) && tl.segment(1).ctx_stale);
        CHECK(tl.updateKey(tl.key(1).id, elbowPose(40.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(tl.segment(1).regen_reasons == 0);
    }

    // the key after B is another one now (the versions count from 1 in every key): (A,B) is marked
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        CHECK(tl.setPass(tl.key(1).id, PassMode::Through) == TimelineResult::Ok);
        tl.requestAll();
        answerAll(tl, retarget, 2);
        CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.segment(0).regen_reasons == 0);
        CHECK(tl.insertKey(2, elbowPose(-40.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(tl.key(1).pass_effective && (tl.segment(0).regen_reasons & REGEN_TARGETS)); // the way on from B went the other way
        MotionTimeline four = elbows({ 0.f, 40.f, 80.f, 120.f });
        four.requestAll();
        answerAll(four, retarget, 3);
        CHECK(four.removeKey(four.key(2).id) == TimelineResult::Ok && four.refresh(retarget) == TimelineResult::Ok);
        CHECK(four.keyCount() == 3 && (four.segment(0).regen_reasons & REGEN_TARGETS));
    }
    // the key after B moves a long way: (A,B) is marked (its way on from B changed)
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        CHECK(tl.updateKey(tl.key(2).id, elbowPose(150.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(tl.key(1).pass_effective && (tl.segment(0).regen_reasons & REGEN_TARGETS));
        CHECK(tl.updateKey(tl.key(2).id, elbowPose(84.f)) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(!(tl.segment(0).regen_reasons & REGEN_TARGETS)); // 4 degrees: not
    }

    // a segment out of a passed key can be asked while the one before it is running (S2), as while it is queued (S1)
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestGenerate(pairOf(tl, 0));
        auto job = tl.nextJob(retarget);
        CHECK(job && tl.running() && tl.segment(0).state == SegState::Running);
        CHECK(tl.requestGenerate(pairOf(tl, 1)) == TimelineResult::Ok && tl.queuedCount() == 1);
        MotionTimeline all = elbows({ 0.f, 40.f, 80.f });
        all.requestGenerate(pairOf(all, 0));
        CHECK(all.nextJob(retarget) && all.requestAll() == 1 && all.segment(1).state == SegState::Queued);
        CHECK(!all.nextJob(retarget)); // one is out: nothing more is sent
        CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
        auto next = tl.nextJob(retarget);
        CHECK(next && next->pair == pairOf(tl, 1) && somaGap(next->context[0], next->context[3]) > 0.003);
    }

    // the one rule: "more candidates" is refused too when the segment before it has no answer
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        CHECK(tl.canRequestMoreAnswers(pairOf(tl, 1)) && tl.canRequestMoreAnswers(pairOf(tl, 0)));
        CHECK(tl.setTokens(pairOf(tl, 0), 6) == TimelineResult::Ok); // (A,B) has no answer in use now
        CHECK(!tl.canRequestMoreAnswers(pairOf(tl, 1)) && tl.requestMoreAnswers(pairOf(tl, 1)) == 0 && tl.queuedCount() == 0);
        CHECK(tl.requestGenerate(pairOf(tl, 1)) == TimelineResult::NeedPrev);
        CHECK(tl.segment(1).state == SegState::Made && tl.segment(1).fail == JobError::None);
    }

    // a chain is all or nothing: a later segment that is queued, or two keys that are the same, refuse before anything is queued
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f, 120.f });
        tl.requestAll();
        answerAll(tl, retarget, 3);
        CHECK(tl.requestGenerate(pairOf(tl, 2)) == TimelineResult::Ok); // the last is queued
        S32 queued = 5;
        CHECK(tl.requestChain(pairOf(tl, 0), &queued) == TimelineResult::Busy && queued == 0 && tl.queuedCount() == 1);
        CHECK(tl.segment(0).state == SegState::Made && tl.segment(1).state == SegState::Made);
        answerAll(tl, retarget, 1);
        // the keys that follow become the same pose (the flags are of the last refresh): refused as SamePoses, nothing queued
        CHECK(tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(tl.updateKey(tl.key(2).id, elbowPose(40.f)) == TimelineResult::Ok); // C is B now; no refresh yet
        CHECK(tl.requestChain(pairOf(tl, 0), &queued) == TimelineResult::SamePoses && queued == 0 && tl.queuedCount() == 0);
        CHECK(tl.segment(0).state == SegState::Made && tl.segment(1).state == SegState::Made && tl.segment(2).state == SegState::Made);
    }

    // a stop inside a run: the segments after a key that is stopped at do not go back with the ones before it
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f, 120.f, 160.f });
        CHECK(tl.setHold(tl.key(2).id, 0.01f) == TimelineResult::Ok); // C is stopped at; B and D are passed
        CHECK(tl.requestAll() == 4);
        CHECK(tl.nextJob(retarget) && !tl.onFailure(JobError::RunFailed)); // (A,B) fails
        auto job = tl.nextJob(retarget); // (B,C) has nothing to go on from and goes back; (C,D) is asked
        CHECK(job && job->pair == pairOf(tl, 2) && job->ntargets == 4 && somaGap(job->context[0], job->context[3]) < 1e-6);
        CHECK(tl.segment(1).state == SegState::Unmade && tl.segment(1).fail == JobError::PrevMissing);
        CHECK(tl.queuedCount() == 1 && tl.segment(3).state == SegState::Queued); // (D,E) waits behind (C,D)
    }

    // what is kept of the context is the END of the answer before it (the last two frames), in time order
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        tl.requestAll();
        answerAll(tl, retarget, 2);
        std::vector<SlPose> tail;
        const PairCache*    first  = tl.cache(pairOf(tl, 0));
        const PairCache*    second = tl.cache(pairOf(tl, 1));
        CHECK(tailPoses(first->clip, 4, tail) && tail.size() == 4 && second->answers[0].used.ctx_last2.size() == 2);
        if (tail.size() == 4 && second->answers[0].used.ctx_last2.size() == 2)
        {
            for (S32 k = 0; k < 2; ++k)
            {
                const SlPose& kept = second->answers[0].used.ctx_last2[(size_t)k];
                CHECK((kept.pelvis - tail[(size_t)k + 2].pelvis).length() < 1e-6f);
                for (S32 j = 0; j < (S32)kept.local.size(); ++j)
                {
                    CHECK(angleBetween(kept.local[(size_t)j], tail[(size_t)k + 2].local[(size_t)j]) < 1e-3);
                }
            }
            // and it is not the first two of the four
            CHECK((second->answers[0].used.ctx_last2[0].pelvis - tail[0].pelvis).length() > 1e-4f);
        }
    }

    // the memo of the automatic judgement is for these keys, not for these version numbers: A, B, then another C
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.key(1).pass_effective);
        CHECK(tl.removeKey(tl.key(2).id) == TimelineResult::Ok && tl.addKey(elbowPose(0.f)) == TimelineResult::Ok); // back to where it began
        CHECK(tl.key(2).pose_ver == 1 && tl.refresh(retarget) == TimelineResult::Ok);
        CHECK(!tl.key(1).pass_effective && !tl.key(1).pass_auto);
    }

    // values outside the enums are refused (a widget's integer cast)
    {
        MotionTimeline tl = elbows({ 0.f, 40.f, 80.f });
        CHECK(tl.setPass(tl.key(1).id, (PassMode)7) == TimelineResult::BadValue && tl.key(1).pass == PassMode::Auto);
        CHECK(tl.setStrength(tl.key(1).id, (PassStrength)200) == TimelineResult::BadValue && tl.key(1).strength == PassStrength::Normal);
    }
}

// ---------------------------------------------------------------- passing through: the timeline's default is off (the window turns it on)
static void testPassOffByDefault(const SomaRetarget& retarget)
{
    MotionTimeline tl;
    CHECK(!tl.passingEnabled());
    for (F32 a : { 0.f, 40.f, 80.f })
    {
        CHECK(tl.addKey(elbowPose(a)) == TimelineResult::Ok);
    }
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && !tl.key(1).pass_effective && !tl.key(1).pass_auto);
    CHECK(tl.setPass(tl.key(1).id, PassMode::Through) == TimelineResult::Ok && tl.refresh(retarget) == TimelineResult::Ok &&
          !tl.key(1).pass_effective); // even a key set to pass is stopped at
    // as before research/361: the second segment on its own is asked, with a plain context and one target
    const PairId p0 = pairOf(tl, 0), p1 = pairOf(tl, 1);
    CHECK(tl.requestGenerate(p1) == TimelineResult::Ok);
    auto job = tl.nextJob(retarget);
    CHECK(job && job->pair == p1 && job->ntargets == 1 && somaGap(job->context[0], job->context[3]) < 1e-6);
    CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(tl.requestGenerate(p0) == TimelineResult::Ok);
    job = tl.nextJob(retarget);
    CHECK(job && job->pair == p0 && job->ntargets == 1);
    CHECK(job && tl.onAnswer(retarget, fakeAnswer(retarget, *job)) == AnswerFate::Used);
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && tl.segment(0).regen_reasons == 0 && tl.segment(1).regen_reasons == 0);
    // turned on: the key is passed, and the answers made without it are marked
    CHECK(tl.setPass(tl.key(1).id, PassMode::Auto) == TimelineResult::Ok); // the automatic judgement says "pass" for these keys
    tl.setPassingEnabled(true);
    CHECK(tl.stale() && tl.refresh(retarget) == TimelineResult::Ok && tl.key(1).pass_effective && tl.key(1).pass_auto);
    CHECK((tl.segment(0).regen_reasons & REGEN_PASS) && (tl.segment(1).regen_reasons & REGEN_PASS));
    tl.setPassingEnabled(true);
    CHECK(!tl.stale()); // the same value: nothing
    tl.setPassingEnabled(false);
    CHECK(!tl.key(1).pass_effective && !tl.key(1).pass_auto); // at once, before any refresh
    CHECK(tl.refresh(retarget) == TimelineResult::Ok && !tl.key(1).pass_effective && tl.segment(0).regen_reasons == 0);
}

static void testPassingThrough(const SomaRetarget& retarget, const std::string& golden)
{
    testAutoGolden(golden);
    testPassEligibility(retarget);
    testPassFlags(retarget);
    testPassView(retarget);
    testPassClips(retarget);
    testPassRequests(retarget);
    testPassQueue(retarget);
    testPassMarks(retarget);
    testPassChain(retarget);
    testPassReview(retarget);
    testPassOffByDefault(retarget);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: motiontimeline_test <golden folder>\n");
        return 2;
    }
    std::string golden = argv[1];
    if (!golden.empty() && golden.back() != '\\' && golden.back() != '/')
    {
        golden += '\\';
    }
    const json::value doc = readJson(golden + "sl_rest_skeleton.json");
    if (doc.is_null())
    {
        return 1;
    }
    SomaRetarget retarget;
    CHECK(retarget.init(restFromJson(doc)) == RetargetResult::Ok);
    if (!retarget.ready())
    {
        return 1;
    }
    testKeys();
    testLimitsAndTimes();
    testClips(retarget);
    testLayout(retarget);
    testStates(retarget);
    testCandidates(retarget);
    testRefit(retarget);
    testJoined(retarget);
    testReview(retarget);
    testPassingThrough(retarget, golden);
    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all timeline tests passed\n");
    return 0;
}
