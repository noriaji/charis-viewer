// Stand-alone test of somaretarget.cpp and motionclip.cpp (not part of the viewer build) with the real llmath,
// against the Python twin's answers (research/324 section 2.4, T3):
// selftest: tools/motionbricks-eval/build_retarget_test.bat
//
//   somaretarget_test.exe <golden folder> <soma_skel.json>
//
// It needs llcommon.lib of a viewer build and boost::json, so it has no "cl /nologo" line of its own:
// tools/motionbricks-eval/build_retarget_test.bat of the sl-viewer repository builds and runs it, with
// tools/motionbricks-eval/golden/ (made by make_golden.py from self-made poses only) and soma_skel.json, and
// tools/selftest runs that (unit-somaretarget; the line above keeps its plain unit test runner off this file).
//   1. somatables.h equals soma_skel.json (names, parents, offsets, rest rotations, rest positions);
//   2. the rest skeleton (golden/sl_rest_skeleton.json) gives the same k;
//   3. SL to SOMA of the 9 poses against golden/sl_to_soma.json: 0.01 degrees, 0.05 mm (the same sums in float);
//   4. SL to SOMA to SL comes back: every joint 0.01 degrees, the pelvis 0.05 mm (also with undriven joints turned);
//   5. SOMA to SL of the 7 answers against golden/clips.json "raw": 0.01 degrees, 0.05 mm;
//   6. buildClip (fitting the ends, EDGE_FRAMES) against golden/clips.json "fixed": 0.01 degrees, 0.05 mm, and
//      (6b) a clip whose undriven joints (spine between, fingers) differ at the two ends, from an answer whose
//      signs flip frame to frame: the ends exact, the undriven joints on the ease curve, and away from the ends
//      the driven joints' world rotations exactly what the answer asks (C^-1 Q Qrest^-1 C), i.e. the undriven
//      joints are given to SOMA to SL before the driven ones are solved;
//   7. guards: NaN, infinity, degenerate rotations, out of range, wrong sizes, frame counts, bad skeletons;
//   8. the playback's time (cursorAt / samplePose), and its buffers kept (no reallocation) frame to frame;
//   9. that the comparisons do catch a joint 1 degree off (a test that cannot fail is no test);
//  10. the time of the conversions (research/324 section 6).
// A missing or unreadable golden file is a failure, not a skip.

#include "linden_common.h"

#include "motionclip.h"
#include "somaretarget.h"
#include "somatables.h"

#include "llmath.h"

#include <boost/json/src.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace plus::posermotion;
namespace json = boost::json;

// ---------------------------------------------------------------- checks
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

// The conversions are the same sums as the Python twin's (in float, not double): only rounding may differ. The
// design's T3 limits (research/324 section 2.4: 0.5 degrees, 1 mm, the clip 1 degree) are far above that.
static constexpr double TOL_ROT_DEG       = 0.01;
static constexpr double TOL_POS_M         = 0.00005;
static constexpr double TOL_ROUNDTRIP_DEG = 0.01;
static constexpr double TOL_CLIP_DEG      = 0.01;

static double deg(const LLQuaternion& a, const LLQuaternion& b)
{
    return rotationAngle(a, b) * RAD_TO_DEG;
}

static double dist(const LLVector3& a, const LLVector3& b)
{
    return (a - b).length();
}

/// v is a number within tol (std::isfinite: under /fp:fast a comparison with NaN cannot be trusted).
static bool within(double v, double tol)
{
    return std::isfinite(v) && v <= tol;
}

/// The largest of what was compared, and how many (a comparison of nothing must not pass).
struct Worst
{
    double value = 0.0;
    long   count = 0;
    std::string where;
    void add(double v, const std::string& at)
    {
        ++count;
        if (std::isfinite(value) && (!std::isfinite(v) || v > value)) // NaN counts as the worst, and stays
        {
            value = v;
            where = at;
        }
    }
};

// ---------------------------------------------------------------- reading the golden files
static json::value readJson(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::printf("FAIL cannot open %s\n", path.c_str());
        ++failures;
        return nullptr;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    boost::system::error_code ec;
    json::value v = json::parse(ss.str(), ec);
    if (ec)
    {
        std::printf("FAIL cannot parse %s: %s\n", path.c_str(), ec.message().c_str());
        ++failures;
        return nullptr;
    }
    return v;
}

static F32 num(const json::value& v)
{
    return (F32)v.to_number<double>();
}

static LLVector3 vec3(const json::value& v)
{
    const json::array& a = v.as_array();
    return LLVector3(num(a[0]), num(a[1]), num(a[2]));
}

static LLQuaternion quatXyzw(const json::value& v) // SL, LLQuaternion's order
{
    const json::array& a = v.as_array();
    return LLQuaternion(num(a[0]), num(a[1]), num(a[2]), num(a[3]));
}

static LLQuaternion quatWxyz(const json::value& v) // SOMA
{
    const json::array& a = v.as_array();
    return LLQuaternion(num(a[1]), num(a[2]), num(a[3]), num(a[0]));
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
                if (rest.joints[i].name == parent)
                {
                    j.parent = i;
                }
            }
            CHECK(j.parent >= 0);
        }
        j.pos   = vec3(jv.at("pos"));
        j.scale = vec3(jv.at("scale"));
        rest.joints.push_back(j);
    }
    return rest;
}

static SlPose identityPose(const SomaRetarget& rt)
{
    SlPose p;
    p.local.assign(rt.jointCount(), LLQuaternion::DEFAULT);
    p.pelvis = rt.pelvisRest();
    return p;
}

/// A pose of golden/poses.json or a frame of golden/clips.json ({local: {joint: [x,y,z,w]}, pelvis}).
static SlPose poseFromJson(const SomaRetarget& rt, const json::value& v, long* undriven = nullptr)
{
    SlPose p = identityPose(rt);
    for (const auto& kv : v.at("local").as_object())
    {
        const S32 j = rt.find(std::string(kv.key()));
        CHECK(j >= 0);
        if (j >= 0)
        {
            p.local[j] = quatXyzw(kv.value());
            if (undriven && !rt.isDriven(j))
            {
                ++*undriven;
            }
        }
    }
    p.pelvis = vec3(v.at("pelvis"));
    return p;
}

static SomaFrame frameFromAnswer(const json::value& f)
{
    SomaFrame out;
    out.hips = vec3(f.at("hips"));
    const json::array& local = f.at("local").as_array();
    CHECK(local.size() == SOMA_JOINT_COUNT);
    for (S32 s = 0; s < SOMA_JOINT_COUNT && s < (S32)local.size(); ++s)
    {
        out.local[s] = quatWxyz(local[s]);
    }
    return out;
}

static KeyPose keyOf(const SlPose& p)
{
    KeyPose k;
    k.sl = p;
    return k;
}

/// The SL frame against an expected one: the rotations of the joints the expected frame lists, and the pelvis.
static void compareSl(const SomaRetarget& rt, const SlPose& got, const json::value& want, Worst& rot, Worst& pos,
                      const std::string& at)
{
    for (const auto& kv : want.at("local").as_object())
    {
        const S32 j = rt.find(std::string(kv.key()));
        if (j < 0)
        {
            rot.add(1e9, at + " " + std::string(kv.key()) + " missing");
            continue;
        }
        rot.add(deg(got.local[j], quatXyzw(kv.value())), at + " " + std::string(kv.key()));
    }
    pos.add(dist(got.pelvis, vec3(want.at("pelvis"))), at + " pelvis");
}

static void report(const char* what, const Worst& w, double tol, const char* unit, long min_count)
{
    std::printf("  %-34s max %.6f %s over %ld (%s)%s\n", what, w.value, unit, w.count, w.where.c_str(),
                within(w.value, tol) && w.count >= min_count ? "" : "  <-- FAIL");
    if (!within(w.value, tol) || w.count < min_count)
    {
        ++failures;
    }
}

/// The 21 pairs, written again here so that the test does not read them from the code it tests.
static const char* const TEST_PAIRS[][2] = {
    { "Hips", "mPelvis" }, { "Spine2", "mTorso" }, { "Chest", "mChest" }, { "Neck1", "mNeck" }, { "Head", "mHead" },
    { "LeftShoulder", "mCollarLeft" }, { "LeftArm", "mShoulderLeft" }, { "LeftForeArm", "mElbowLeft" },
    { "LeftHand", "mWristLeft" }, { "LeftLeg", "mHipLeft" }, { "LeftShin", "mKneeLeft" }, { "LeftFoot", "mAnkleLeft" },
    { "LeftToeBase", "mFootLeft" }, { "RightShoulder", "mCollarRight" }, { "RightArm", "mShoulderRight" },
    { "RightForeArm", "mElbowRight" }, { "RightHand", "mWristRight" }, { "RightLeg", "mHipRight" },
    { "RightShin", "mKneeRight" }, { "RightFoot", "mAnkleRight" }, { "RightToeBase", "mFootRight" },
};

static S32 somaIndexOf(const char* name)
{
    for (S32 s = 0; s < soma::JOINT_COUNT; ++s)
    {
        if (std::string(soma::REST[s].name) == name)
        {
            return s;
        }
    }
    return -1;
}

/// SL's world rotation that the answer's frame asks of the SL joint paired with SOMA joint s: C^-1 Q Qrest^-1 C
/// (column vectors; in the viewer's order a * b is first a, then b).
static LLQuaternion askedSlWorld(const SomaFrame& f, S32 s)
{
    std::array<LLQuaternion, SOMA_JOINT_COUNT> world;
    for (S32 j = 0; j < SOMA_JOINT_COUNT; ++j)
    {
        LLQuaternion q = f.local[j];
        q.normalize();
        world[j] = soma::REST[j].parent < 0 ? q : q * world[soma::REST[j].parent];
    }
    const float* r = soma::REST[s].rest_world;
    const LLQuaternion rest_inv = ~LLQuaternion(r[1], r[2], r[3], r[0]);
    const LLQuaternion d = rest_inv * world[s]; // column vectors: Q * Qrest^-1
    return LLQuaternion(d.mQ[VX], -d.mQ[VZ], d.mQ[VY], d.mQ[VW]); // C^-1 (.) C: the axis (x, y, z) -> (x, -z, y)
}

/// SL world rotations of a pose (row vectors: world = local * parent).
static std::vector<LLQuaternion> slWorld(const SomaRetarget& rt, const LLQuaternion* local)
{
    std::vector<LLQuaternion> w(rt.jointCount());
    for (S32 j = 0; j < rt.jointCount(); ++j)
    {
        const S32 p = rt.joint(j).parent;
        w[j] = p < 0 ? local[j] : local[j] * w[p];
    }
    return w;
}

static LLQuaternion turn(F32 degrees, const LLVector3& axis)
{
    return LLQuaternion(degrees * DEG_TO_RAD, axis);
}

template <typename F> static double microsPer(long count, F&& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < count; ++i)
    {
        f();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / (double)count;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: somaretarget_test <golden folder> <soma_skel.json>\n");
        return 2;
    }
    const std::string golden = std::string(argv[1]) + "/";
    const json::value skel     = readJson(argv[2]);
    const json::value rest_doc = readJson(golden + "sl_rest_skeleton.json");
    const json::value poses    = readJson(golden + "poses.json");
    const json::value s2s      = readJson(golden + "sl_to_soma.json");
    const json::value answers  = readJson(golden + "answers.json");
    const json::value clips    = readJson(golden + "clips.json");
    if (failures)
    {
        return 1;
    }

    // ---- 1. the table equals soma_skel.json ----
    {
        const json::array& joints = skel.at("joints").as_array();
        CHECK(joints.size() == (size_t)soma::JOINT_COUNT);
        double worst = 0.0;
        for (S32 i = 0; i < soma::JOINT_COUNT && i < (S32)joints.size(); ++i)
        {
            const soma::JointRest& r = soma::REST[i];
            const json::value&     j = joints[i];
            CHECK(std::string(j.at("name").as_string()) == r.name);
            CHECK(j.at("parent").to_number<int>() == r.parent);
            auto cmp = [&worst](const json::value& v, const float* t, int count)
            {
                const json::array& a = v.as_array();
                for (int k = 0; k < count; ++k)
                {
                    worst = std::max(worst, std::fabs(a[k].to_number<double>() - (double)t[k]));
                }
            };
            cmp(j.at("offset"), r.offset, 3);
            cmp(j.at("rest_local"), r.rest_local, 4);
            cmp(j.at("rest_world"), r.rest_world, 4);
            cmp(j.at("rest_pos"), r.rest_pos, 3);
        }
        std::printf("1. somatables.h vs soma_skel.json: largest difference %.3g\n", worst);
        CHECK(worst < 1e-6);
    }

    // ---- 2. the rest skeleton and k ----
    const RestSkeleton rest = restFromJson(rest_doc);
    SomaRetarget       rt;
    CHECK(rt.init(rest) == RetargetResult::Ok);
    if (!rt.ready())
    {
        std::printf("FAIL the rest skeleton was refused\n");
        return 1;
    }
    {
        const double k = rest_doc.at("k").to_number<double>();
        std::printf("2. %d SL joints, k %.6f (Python %.6f)\n", rt.jointCount(), rt.hipScale(), k);
        CHECK(rt.jointCount() == 133);
        CHECK(std::fabs(rt.hipScale() - k) < 1e-5);
        S32 driven = 0;
        for (S32 j = 0; j < rt.jointCount(); ++j)
        {
            driven += rt.isDriven(j) ? 1 : 0;
        }
        CHECK(driven == 21);
    }

    RetargetWork work;

    // ---- 3. SL to SOMA ----
    std::vector<std::pair<std::string, SlPose>> pose_list;
    {
        long undriven = 0;
        for (const auto& kv : poses.at("poses").as_object())
        {
            pose_list.emplace_back(std::string(kv.key()), poseFromJson(rt, kv.value(), &undriven));
        }
        // the Python clips add the undriven joints after SOMA to SL; with none in the poses, that is the same
        CHECK(undriven == 0);
        Worst rot, pos;
        for (const auto& [name, pose] : pose_list)
        {
            SomaPose sp;
            CHECK(rt.slToSoma(pose, sp, work) == RetargetResult::Ok);
            const json::value& want = s2s.at("poses").at(name);
            for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
            {
                rot.add(deg(sp.rot[s], quatWxyz(want.at("rot").as_array()[s])), name + " " + soma::REST[s].name);
                pos.add(dist(sp.pos[s], vec3(want.at("pos").as_array()[s])), name + " " + soma::REST[s].name);
            }
        }
        std::printf("3. SL to SOMA (%zu poses)\n", pose_list.size());
        CHECK(pose_list.size() == 9);
        report("rotation", rot, TOL_ROT_DEG, "deg", 9 * 30);
        report("position", pos, TOL_POS_M, "m", 9 * 30);
    }

    // ---- 4. SL to SOMA to SL ----
    {
        Worst rot, pos;
        auto roundTrip = [&](const std::string& name, const SlPose& pose)
        {
            SomaPose sp;
            CHECK(rt.slToSoma(pose, sp, work) == RetargetResult::Ok);
            SomaFrame f;
            SomaRetarget::localsOf(sp, f);
            SlPose back = pose; // the undriven joints are given as they were
            for (S32 j = 0; j < rt.jointCount(); ++j)
            {
                if (rt.isDriven(j))
                {
                    back.local[j] = LLQuaternion(0.3f, 0.1f, -0.2f, 0.9f); // must be overwritten
                }
            }
            back.pelvis = LLVector3(9.f, 9.f, 9.f);
            CHECK(rt.somaToSl(f, back, work) == RetargetResult::Ok);
            for (S32 j = 0; j < rt.jointCount(); ++j)
            {
                rot.add(deg(back.local[j], pose.local[j]), name + " " + rt.joint(j).name);
            }
            pos.add(dist(back.pelvis, pose.pelvis), name + " pelvis");
        };
        for (const auto& [name, pose] : pose_list)
        {
            roundTrip(name, pose);
        }
        // undriven joints turned (spine between, a finger, the tail): the driven ones must take them into account
        SlPose odd = pose_list.front().second;
        const char* undriven[] = { "mSpine1", "mSpine3", "mHandIndex1Left", "mTail1", "mFaceJaw" };
        F32 a = 12.f;
        for (const char* n : undriven)
        {
            const S32 j = rt.find(n);
            CHECK(j >= 0 && !rt.isDriven(j));
            if (j >= 0)
            {
                odd.local[j] = turn(a, LLVector3(0.3f, 0.8f, -0.5f)) * odd.local[j];
                a += 7.f;
            }
        }
        odd.local[rt.find("mShoulderLeft")] = turn(-40.f, LLVector3(1.f, 0.2f, 0.f));
        roundTrip("undriven-turned", odd);
        std::printf("4. SL to SOMA to SL\n");
        report("rotation", rot, TOL_ROUNDTRIP_DEG, "deg", 10L * 133);
        report("pelvis", pos, TOL_POS_M, "m", 10);
    }

    // ---- 5. SOMA to SL of the answers (raw) ----
    std::vector<std::vector<SomaFrame>> answer_frames;
    {
        Worst rot, pos;
        const json::array& alist = answers.at("answers").as_array();
        const json::array& clist = clips.at("clips").as_array();
        CHECK(alist.size() == 7 && clist.size() == alist.size());
        for (size_t c = 0; c < alist.size() && c < clist.size(); ++c)
        {
            CHECK(alist[c].at("id") == clist[c].at("id"));
            std::vector<SomaFrame> frames;
            for (const json::value& f : alist[c].at("frames").as_array())
            {
                frames.push_back(frameFromAnswer(f));
            }
            const json::array& raw = clist[c].at("raw").as_array();
            CHECK(raw.size() == frames.size());
            for (size_t f = 0; f < frames.size() && f < raw.size(); ++f)
            {
                SlPose io = identityPose(rt);
                CHECK(rt.somaToSl(frames[f], io, work) == RetargetResult::Ok);
                compareSl(rt, io, raw[f], rot, pos, std::string(clist[c].at("name").as_string()) + " f" + std::to_string(f));
            }
            answer_frames.push_back(std::move(frames));
        }
        std::printf("5. SOMA to SL (%zu answers)\n", answer_frames.size());
        report("rotation", rot, TOL_ROT_DEG, "deg", 300 * 21);
        report("pelvis", pos, TOL_POS_M, "m", 300);
    }

    // ---- 6. the clip (ends fitted) ----
    std::vector<MotionClip> built;
    {
        Worst rot, pos, sign;
        const json::array& clist = clips.at("clips").as_array();
        for (size_t c = 0; c < clist.size() && c < answer_frames.size(); ++c)
        {
            const std::string name(clist[c].at("name").as_string());
            const size_t      gt = name.find('>');
            const std::string s  = name.substr(0, gt);
            const std::string e  = name.substr(gt + 1);
            const SlPose      ps = poseFromJson(rt, poses.at("poses").at(s));
            const SlPose      pe = poseFromJson(rt, poses.at("poses").at(e));
            MotionClip        clip;
            ClipStats         st;
            CHECK(buildClip(rt, keyOf(ps), keyOf(pe), answer_frames[c], clip, &st) == RetargetResult::Ok);
            CHECK(st.edge_frames == clist[c].at("edge_frames").to_number<int>());
            const json::array& fixed = clist[c].at("fixed").as_array();
            CHECK(clip.frame_count == (S32)fixed.size());
            KeyPose at;
            sizePoseFor(clip, at);
            for (S32 f = 0; f < clip.frame_count && f < (S32)fixed.size(); ++f)
            {
                CHECK(samplePose(clip, cursorAtFrame(clip.frame_count, f), at));
                compareSl(rt, at.sl, fixed[f], rot, pos, name + " f" + std::to_string(f));
                for (S32 j = 0; j < rt.jointCount(); ++j)
                {
                    if (!rt.isDriven(j)) // no undriven joint in the poses: they stay at rest
                    {
                        rot.add(deg(at.sl.local[j], LLQuaternion::DEFAULT), name + " " + rt.joint(j).name);
                    }
                    if (f > 0)
                    {
                        const F32 d = dot(clip.local[(size_t)(f - 1) * clip.joint_count + j], clip.local[(size_t)f * clip.joint_count + j]);
                        sign.add(d < 0.f ? 1.0 : 0.0, name + " sign " + rt.joint(j).name);
                    }
                }
            }
            // the ends are the poses exactly
            for (S32 j = 0; j < rt.jointCount(); ++j)
            {
                CHECK(deg(clip.local[j], ps.local[j]) < 0.01);
                CHECK(deg(clip.local[(size_t)(clip.frame_count - 1) * clip.joint_count + j], pe.local[j]) < 0.01);
            }
            CHECK(dist(clip.pelvis.front(), ps.pelvis) < 1e-4 && dist(clip.pelvis.back(), pe.pelvis) < 1e-4);
            std::printf("   %-28s %2d frames, ends before the fitting: start %.1f deg %.1f cm, end %.1f deg %.1f cm\n",
                        name.c_str(), clip.frame_count, st.start_max_deg, st.start_pelvis_m * 100.f, st.end_max_deg,
                        st.end_pelvis_m * 100.f);
            built.push_back(std::move(clip));
        }
        std::printf("6. the clips (ends fitted over %d frames)\n", EDGE_FRAMES);
        report("rotation", rot, TOL_CLIP_DEG, "deg", 300 * 133);
        report("pelvis", pos, TOL_POS_M, "m", 300);
        report("sign flips between frames", sign, 0.0, "", 300 * 132);
    }

    // ---- 6b. undriven joints that differ at the two ends, an answer with flipping signs ----
    {
        SlPose ps = pose_list.front().second;   // stand
        SlPose pe = pose_list[1].second;        // raise_right
        struct Turn
        {
            const char* joint;
            F32         start_deg, end_deg;
            LLVector3   axis;
        };
        const Turn turns[] = {
            { "mSpine2", 15.f, -20.f, LLVector3(0.f, 1.f, 0.f) },
            { "mSpine3", -10.f, 25.f, LLVector3(1.f, 0.f, 0.f) },
            { "mHandIndex1Left", 30.f, -10.f, LLVector3(0.f, 0.f, 1.f) },
            { "mHandIndex2Left", 0.f, 50.f, LLVector3(0.f, 0.f, 1.f) },
        };
        for (const Turn& t : turns)
        {
            const S32 j = rt.find(t.joint);
            CHECK(j >= 0 && !rt.isDriven(j));
            if (j >= 0)
            {
                ps.local[j] = turn(t.start_deg, t.axis);
                pe.local[j] = turn(t.end_deg, t.axis);
            }
        }
        std::vector<SomaFrame> answer = answer_frames.front();
        for (size_t f = 1; f < answer.size(); f += 2) // the same turns, the other way round on the sphere
        {
            for (LLQuaternion& q : answer[f].local)
            {
                q = LLQuaternion(-q.mQ[VX], -q.mQ[VY], -q.mQ[VZ], -q.mQ[VW]);
            }
        }
        MotionClip clip;
        ClipStats  st;
        CHECK(buildClip(rt, keyOf(ps), keyOf(pe), answer, clip, &st) == RetargetResult::Ok);
        const S32 n = clip.frame_count, J = clip.joint_count;
        CHECK(n == (S32)answer.size() && J == rt.jointCount() && n > 2 * st.edge_frames);
        Worst ends, ease, asked, sign;
        for (S32 j = 0; j < J && n > 1; ++j)
        {
            ends.add(deg(clip.local[j], ps.local[j]), "first " + rt.joint(j).name);
            ends.add(deg(clip.local[(size_t)(n - 1) * J + j], pe.local[j]), "last " + rt.joint(j).name);
        }
        ends.add(dist(clip.pelvis.front(), ps.pelvis) * 1000.0, "first pelvis (mm)");
        ends.add(dist(clip.pelvis.back(), pe.pelvis) * 1000.0, "last pelvis (mm)");
        long inner = 0;
        for (S32 f = 0; f < n; ++f)
        {
            const LLQuaternion* row = &clip.local[(size_t)f * J];
            const F32 t = (F32)(0.5 - 0.5 * std::cos(3.14159265358979323846 * f / (n - 1)));
            for (S32 j = 0; j < J; ++j)
            {
                if (!rt.isDriven(j))
                {
                    LLQuaternion want = slerp(t, ps.local[j], pe.local[j]);
                    want.normalize();
                    ease.add(deg(row[j], want), "f" + std::to_string(f) + " " + rt.joint(j).name);
                }
                if (f > 0)
                {
                    sign.add(dot(clip.local[(size_t)(f - 1) * J + j], row[j]) < 0.f ? 1.0 : 0.0, "sign " + rt.joint(j).name);
                }
            }
            if (f >= st.edge_frames && f <= n - 1 - st.edge_frames) // the fitting's weights are 0 here
            {
                ++inner;
                const std::vector<LLQuaternion> w = slWorld(rt, row);
                for (const auto& pr : TEST_PAIRS)
                {
                    const S32 s = somaIndexOf(pr[0]);
                    const S32 l = rt.find(pr[1]);
                    CHECK(s >= 0 && l >= 0);
                    if (s >= 0 && l >= 0)
                    {
                        asked.add(deg(w[l], askedSlWorld(answer[f], s)), "f" + std::to_string(f) + " " + pr[1]);
                    }
                }
            }
        }
        std::printf("6b. undriven joints apart at the ends (%d frames, %ld away from the ends)\n", n, inner);
        report("ends (deg; pelvis mm)", ends, TOL_ROUNDTRIP_DEG, "", 2L * 133 + 2);
        report("undriven on the ease curve", ease, TOL_CLIP_DEG, "deg", 58L * 112);
        report("driven world = C^-1 Q Qrest^-1 C", asked, TOL_CLIP_DEG, "deg", (long)(n - 2 * st.edge_frames) * 21); // every frame away from the ends, every pair
        report("sign flips between frames", sign, 0.0, "", 57L * 133);
    }

    // ---- 7. guards ----
    {
        const F32 nan = std::numeric_limits<F32>::quiet_NaN();
        const F32 inf = std::numeric_limits<F32>::infinity();
        const SlPose stand = pose_list.front().second;
        SomaPose sp;

        // rest skeletons that are refused
        {
            SomaRetarget bad;
            CHECK(bad.init(RestSkeleton{}) == RetargetResult::BadSkeleton && !bad.ready());
            RestSkeleton r = rest;
            r.joints[5].parent = 7; // a parent after its child
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[3].name = r.joints[2].name; // two of one name
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[rt.find("mWristLeft")].name = "mWristLeftX"; // a paired joint missing
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[4].parent = -1; // two roots
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[6].pos.mV[VZ] = nan;
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[6].scale.mV[VY] = 0.f;
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[0].pos.mV[VZ] = -1.f; // the hips below the floor: no k
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            r = rest;
            r.joints[9].pos.mV[VX] = 1e6f;
            CHECK(bad.init(r) == RetargetResult::BadSkeleton);
            CHECK(bad.slToSoma(stand, sp, work) == RetargetResult::BadSkeleton);
            SlPose io = stand;
            CHECK(bad.somaToSl(SomaFrame{}, io, work) == RetargetResult::BadSkeleton);
        }

        // SL poses that are refused
        {
            SlPose p = stand;
            p.local.pop_back();
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::BadInput);
            p = stand;
            p.local[7] = LLQuaternion(nan, 0.f, 0.f, 1.f);
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::NotFinite);
            p.local[7] = LLQuaternion(0.f, inf, 0.f, 1.f);
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::NotFinite);
            p.local[7] = LLQuaternion(0.f, 0.f, 0.f, 0.f); // degenerate
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::BadInput);
            p.local[7] = LLQuaternion(0.f, 0.f, 0.f, 3.f);
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::BadInput);
            p = stand;
            p.pelvis.mV[VX] = nan;
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::NotFinite);
            p.pelvis.mV[VX] = 1e6f;
            CHECK(rt.slToSoma(p, sp, work) == RetargetResult::BadInput);
            // a rotation of length 1.5 is the same turn
            SomaPose a, b;
            p = stand;
            const S32 sh = rt.find("mShoulderRight");
            p.local[sh] = turn(50.f, LLVector3(0.f, 1.f, 0.f));
            CHECK(rt.slToSoma(p, a, work) == RetargetResult::Ok);
            for (F32& c : p.local[sh].mQ)
            {
                c *= 1.5f;
            }
            CHECK(rt.slToSoma(p, b, work) == RetargetResult::Ok);
            CHECK(deg(a.rot[17], b.rot[17]) < 1e-3); // RightArm, the partner of mShoulderRight
        }

        // SOMA frames that are refused, and io left alone
        {
            const SomaFrame good = answer_frames.front().front();
            SlPose io = stand;
            const SlPose before = io;
            auto unchanged = [&]()
            {
                bool same = dist(io.pelvis, before.pelvis) == 0.0;
                for (size_t j = 0; j < io.local.size(); ++j)
                {
                    same = same && io.local[j] == before.local[j];
                }
                return same;
            };
            SomaFrame f = good;
            f.hips.mV[VY] = nan;
            CHECK(rt.somaToSl(f, io, work) == RetargetResult::NotFinite && unchanged());
            f = good;
            f.hips.mV[VZ] = 20.f;
            CHECK(rt.somaToSl(f, io, work) == RetargetResult::BadInput && unchanged());
            f = good;
            f.local[12] = LLQuaternion(0.f, 0.f, 0.f, 0.f);
            CHECK(rt.somaToSl(f, io, work) == RetargetResult::BadInput && unchanged());
            f = good;
            f.local[3] = LLQuaternion(0.f, nan, 0.f, 1.f);
            CHECK(rt.somaToSl(f, io, work) == RetargetResult::NotFinite && unchanged());
            f = good;
            io.local[rt.find("mHandIndex1Left")] = LLQuaternion(0.f, 0.f, 0.f, 0.f); // an undriven joint given bad
            const SlPose before2 = io;
            CHECK(rt.somaToSl(f, io, work) == RetargetResult::BadInput);
            CHECK(io.local[rt.find("mWristLeft")] == before2.local[rt.find("mWristLeft")]);
            SlPose small = stand;
            small.local.resize(10);
            CHECK(rt.somaToSl(good, small, work) == RetargetResult::BadInput);
        }

        // clips that are refused, and out left alone
        {
            const MotionClip keep = built.front();
            MotionClip out = keep;
            std::vector<SomaFrame> fr = answer_frames.front();
            const KeyPose ks = keyOf(stand), ke = keyOf(pose_list[1].second);
            auto same = [&]() { return out.frame_count == keep.frame_count && out.local == keep.local; };
            std::vector<SomaFrame> shorter(fr.begin(), fr.begin() + (CLIP_MIN_FRAMES - 1));
            CHECK(buildClip(rt, ks, ke, shorter, out) == RetargetResult::BadInput && same());
            std::vector<SomaFrame> longer = fr;
            while ((S32)longer.size() <= CLIP_MAX_FRAMES)
            {
                longer.push_back(fr.back());
            }
            CHECK(buildClip(rt, ks, ke, longer, out) == RetargetResult::BadInput && same());
            std::vector<SomaFrame> holed = fr;
            holed[holed.size() / 2].local[20].mQ[VW] = nan;
            CHECK(buildClip(rt, ks, ke, holed, out) == RetargetResult::NotFinite && same());
            KeyPose ke2 = ke;
            ke2.extra_rot.push_back(LLQuaternion::DEFAULT); // the start has none
            CHECK(buildClip(rt, ks, ke2, fr, out) == RetargetResult::BadInput && same());
            KeyPose ks2 = ks;
            ks2.sl.local[40] = LLQuaternion(0.f, 0.f, 0.f, 0.f);
            CHECK(buildClip(rt, ks2, ke, fr, out) == RetargetResult::BadInput && same());
            ks2 = ks;
            ks2.sl.pelvis.mV[VY] = inf;
            CHECK(buildClip(rt, ks2, ke, fr, out) == RetargetResult::BadInput && same());
            SomaRetarget not_ready;
            // a failed init leaves nothing half made: not ready, no joints, and a later init works again
            SomaRetarget again;
            CHECK(again.init(rest) == RetargetResult::Ok && again.jointCount() == 133);
            RestSkeleton no_pelvis = rest;
            no_pelvis.joints[rt.find("mPelvis")].name = "mPelvisX"; // fails after the SL side was read
            CHECK(again.init(no_pelvis) == RetargetResult::BadSkeleton);
            CHECK(!again.ready() && again.jointCount() == 0 && again.find("mTorso") < 0);
            CHECK(!again.isDriven(0) && !again.isDriven(-1) && !again.isDriven(100000));
            CHECK(again.slToSoma(ks.sl, sp, work) == RetargetResult::BadSkeleton);
            CHECK(again.init(rest) == RetargetResult::Ok && again.ready() && again.isDriven(again.find("mTorso")));
            SomaPose sa, sb;
            CHECK(again.slToSoma(ks.sl, sa, work) == RetargetResult::Ok && rt.slToSoma(ks.sl, sb, work) == RetargetResult::Ok);
            CHECK(deg(sa.rot[11], sb.rot[11]) == 0.0 && dist(sa.pos[25], sb.pos[25]) == 0.0);
            CHECK(!rt.isDriven(-1) && !rt.isDriven(rt.jointCount()));
            CHECK(buildClip(not_ready, ks, ke, fr, out) == RetargetResult::BadSkeleton && same());
        }
        std::printf("7. guards checked\n");
    }

    // ---- 8. the playback ----
    {
        const MotionClip& clip = built.front();
        const S32 last = clip.frame_count - 1;
        const F64 dur  = clip.durationSec();
        CHECK(std::fabs(dur - (F64)last / CLIP_FPS) < 1e-12);
        PlayCursor c = cursorAt(clip.frame_count, 0.0, LoopMode::Once);
        CHECK(c.frame == 0 && c.next == 1 && c.blend == 0.f && !c.finished);
        c = cursorAt(clip.frame_count, 2.5 / CLIP_FPS, LoopMode::Once);
        CHECK(c.frame == 2 && c.next == 3 && std::fabs(c.blend - 0.5f) < 1e-4f);
        c = cursorAt(clip.frame_count, dur, LoopMode::Once);
        CHECK(c.frame == last && c.next == last && c.finished);
        c = cursorAt(clip.frame_count, 1e9, LoopMode::Once);
        CHECK(c.frame == last && c.finished);
        c = cursorAt(clip.frame_count, std::numeric_limits<F64>::quiet_NaN(), LoopMode::Once);
        CHECK(c.frame == 0 && c.blend == 0.f);
        c = cursorAt(clip.frame_count, -3.0, LoopMode::Loop);
        CHECK(c.frame == 0 && c.blend == 0.f && !c.finished);
        c = cursorAt(clip.frame_count, dur + 0.25, LoopMode::Loop); // the hold at the end
        CHECK(c.frame == last && !c.finished);
        c = cursorAt(clip.frame_count, dur + LOOP_HOLD_SEC + 1.25 / CLIP_FPS, LoopMode::Loop);
        CHECK(c.frame == 1 && std::fabs(c.blend - 0.25f) < 1e-3f);
        c = cursorAt(clip.frame_count, 1e9 + 0.123, LoopMode::Loop);
        CHECK(c.frame >= 0 && c.frame <= last && c.blend >= 0.f && c.blend <= 1.f);
        c = cursorAt(0, 1.0, LoopMode::Once);
        CHECK(c.frame == 0 && c.next == 0 && c.finished); // an empty clip is finished at once
        c = cursorAt(0, 1.0, LoopMode::Loop);
        CHECK(c.frame == 0 && !c.finished);
        c = cursorAt(clip.frame_count, std::numeric_limits<F64>::infinity(), LoopMode::Once);
        CHECK(c.frame == last && c.next == last && c.finished);
        c = cursorAt(clip.frame_count, std::numeric_limits<F64>::infinity(), LoopMode::Loop);
        CHECK(c.frame == last && c.next == last && !c.finished);
        c = cursorAt(clip.frame_count, -std::numeric_limits<F64>::infinity(), LoopMode::Once);
        CHECK(c.frame == 0 && c.blend == 0.f && !c.finished);
        c = cursorAt(1, 1.0, LoopMode::Once);
        CHECK(c.frame == 0 && c.finished);
        CHECK(cursorAtFrame(clip.frame_count, -4).frame == 0 && cursorAtFrame(clip.frame_count, 999).frame == last);

        KeyPose out;
        CHECK(!samplePose(clip, c, out)); // not sized
        sizePoseFor(clip, out);
        // halfway between two frames: the pelvis halfway, a rotation between
        CHECK(samplePose(clip, cursorAt(clip.frame_count, 10.5 / CLIP_FPS, LoopMode::Once), out));
        const LLVector3 mid = (clip.pelvis[10] + clip.pelvis[11]) * 0.5f;
        CHECK(dist(out.sl.pelvis, mid) < 1e-5);
        const S32 sh = rt.find("mShoulderRight");
        const LLQuaternion qa = clip.local[(size_t)10 * clip.joint_count + sh];
        const LLQuaternion qb = clip.local[(size_t)11 * clip.joint_count + sh];
        CHECK(std::fabs(deg(qa, out.sl.local[sh]) - deg(out.sl.local[sh], qb)) < 0.01);
        CHECK(!samplePose(MotionClip{}, c, out));
        MotionClip broken = clip;
        broken.local.pop_back(); // not as buildClip makes it
        CHECK(!samplePose(broken, c, out));
        broken = clip;
        broken.joint_count = 0;
        CHECK(!samplePose(broken, c, out));

        // no allocation in the per-frame calls (and in the conversions once their scratch is sized): the buffers
        // stay where they are (llcommon replaces operator new, so the test cannot count allocations itself; the
        // functions create no container of their own)
        const void* local_at = out.sl.local.data();
        const void* rot_at   = out.extra_rot.data();
        F64 t = 0.0;
        bool sampled = true;
        for (int i = 0; i < 20000; ++i, t += 1.0 / 144.0)
        {
            sampled = samplePose(clip, cursorAt(clip.frame_count, t, LoopMode::Loop), out) && sampled;
        }
        CHECK(sampled && out.sl.local.data() == local_at && out.extra_rot.data() == rot_at);
        SomaPose  sp;
        SomaFrame sf;
        SlPose    io = pose_list.front().second;
        rt.slToSoma(pose_list.front().second, sp, work);
        SomaRetarget::localsOf(sp, sf);
        rt.somaToSl(sf, io, work);
        const void* world_at = work.world.data();
        const void* pos_at   = work.pos.data();
        const void* io_at    = io.local.data();
        for (int i = 0; i < 100; ++i)
        {
            rt.slToSoma(pose_list[i % pose_list.size()].second, sp, work);
            SomaRetarget::localsOf(sp, sf);
            rt.somaToSl(sf, io, work);
        }
        CHECK(work.world.data() == world_at && work.pos.data() == pos_at && io.local.data() == io_at);
        std::printf("8. playback: cursor and sample checked, buffers kept over 20000 samples and 100 conversions\n");

        // the extra tracks: from the start to the end on the ease curve
        KeyPose ks = keyOf(pose_list.front().second), ke = keyOf(pose_list[1].second);
        ks.extra_rot = { LLQuaternion::DEFAULT, turn(10.f, LLVector3(1.f, 0.f, 0.f)) };
        ke.extra_rot = { turn(90.f, LLVector3(0.f, 0.f, 1.f)), turn(10.f, LLVector3(1.f, 0.f, 0.f)) };
        ks.extra_vec = { LLVector3(0.f, 0.f, 0.f) };
        ke.extra_vec = { LLVector3(1.f, 2.f, 3.f) };
        MotionClip ec;
        CHECK(buildClip(rt, ks, ke, answer_frames.front(), ec) == RetargetResult::Ok);
        CHECK(ec.extra_rot_count == 2 && ec.extra_vec_count == 1);
        const S32 n = ec.frame_count, f = n / 3;
        const F64 te = 0.5 - 0.5 * std::cos(3.14159265358979323846 * f / (n - 1));
        CHECK(deg(ec.extra_rot[0], ks.extra_rot[0]) < 1e-3 && deg(ec.extra_rot[(size_t)(n - 1) * 2], ke.extra_rot[0]) < 1e-3);
        CHECK(std::fabs(deg(ec.extra_rot[(size_t)f * 2], LLQuaternion::DEFAULT) - 90.0 * te) < 0.01);
        CHECK(deg(ec.extra_rot[(size_t)f * 2 + 1], ks.extra_rot[1]) < 1e-3);
        CHECK(dist(ec.extra_vec[(size_t)f], LLVector3(1.f, 2.f, 3.f) * (F32)te) < 1e-5);
        CHECK(dist(ec.extra_vec[(size_t)(n - 1)], ke.extra_vec[0]) < 1e-6);
    }

    // ---- 9. the comparisons do catch a joint 1 degree off ----
    {
        const json::value& want = s2s.at("poses").at(pose_list.front().first);
        SomaPose sp;
        rt.slToSoma(pose_list.front().second, sp, work);
        // the result with one joint turned 1 degree, among the right ones, through the same comparison
        Worst shifted;
        for (S32 s = 0; s < SOMA_JOINT_COUNT; ++s)
        {
            const LLQuaternion got = s == 13 ? turn(1.f, LLVector3(0.f, 1.f, 0.f)) * sp.rot[s] : sp.rot[s];
            shifted.add(deg(got, quatWxyz(want.at("rot").as_array()[s])), soma::REST[s].name);
        }
        CHECK(shifted.count == SOMA_JOINT_COUNT && !within(shifted.value, TOL_ROT_DEG) && shifted.where == "LeftHand");
        CHECK(!within(shifted.value, 0.5)); // and the design's own limit
        Worst w;
        w.add(0.0, "fine");
        w.add(std::numeric_limits<double>::quiet_NaN(), "nan");
        w.add(0.0, "fine after");
        CHECK(!within(w.value, TOL_ROT_DEG)); // a NaN result is a failure, and stays one
        std::printf("9. a joint 1 degree off is caught, a NaN is a failure\n");
    }

    // ---- 10. time ----
    {
        SomaPose  sp;
        SomaFrame sf;
        SlPose    io = pose_list.front().second;
        size_t    i  = 0;
        const double sl_to_soma = microsPer(20000, [&]() { rt.slToSoma(pose_list[i++ % pose_list.size()].second, sp, work); });
        rt.slToSoma(pose_list.front().second, sp, work);
        SomaRetarget::localsOf(sp, sf);
        const double soma_to_sl = microsPer(20000, [&]() { rt.somaToSl(sf, io, work); });
        const KeyPose ks = keyOf(pose_list.front().second), ke = keyOf(pose_list[1].second);
        MotionClip clip;
        const double build = microsPer(200, [&]() { buildClip(rt, ks, ke, answer_frames.front(), clip); });
        KeyPose out;
        sizePoseFor(clip, out);
        F64 t = 0.0;
        const double sample = microsPer(100000, [&]() { samplePose(clip, cursorAt(clip.frame_count, t += 0.007, LoopMode::Loop), out); });
        std::printf("10. time: SL to SOMA %.2f us/pose, SOMA to SL %.2f us/frame, buildClip (%d frames) %.1f us, "
                    "samplePose %.2f us\n", sl_to_soma, soma_to_sl, clip.frame_count, build, sample);
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
