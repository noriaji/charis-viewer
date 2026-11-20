// Stand-alone test of motionbvh.cpp, motiontracks.cpp and motionplayback.cpp (not part of the viewer build) with
// the real llmath (research/324, branch D2):
// selftest: tools/motionbricks-eval/build_motionui_test.bat
//
//   motionui_test.exe <golden folder>
//
// It needs llcommon.lib of a viewer build and boost::json, so it has no "cl /nologo" line of its own:
// tools/motionbricks-eval/build_motionui_test.bat of the sl-viewer repository builds and runs it (with
// golden/sl_rest_skeleton.json, made by make_golden.py from avatar_skeleton.xml).
//   1. bvhAnglesDegrees: SL's BVH loader (llbvhloader.cpp: the channels' names, mayaQ() of the reversed order,
//      the frame of makeTranslation(); reproduced here with the real mayaQ and StringToOrder) turns the angles back
//      into the rotation, for all six channel orders, random rotations and the gimbal lines (0.01 degrees);
//   2. readBvhHierarchy: a hierarchy as FSFloaterPoser::writeBvhFragment writes it (every channel order it
//      uses), and the hierarchies it refuses;
//   3. writeBvhMotion (with an extra track, and with the root's position on and off): Frames N + 1, Frame Time 1 / 30, the reference frame of zeros, the number of values per
//      line, and the values read back the loader's way give the clip (rotations 0.01 degrees, pelvis 0.01 mm);
//      what it refuses (and writes nothing then);
//   4. makeKeyPoses / trackValues on the rest skeleton: which joints are written, the pelvis track, the offsets,
//      and the poses it refuses;
//   5. ClipPlayback: the C machine's moves (load, play, stop, seek, the end, loop, reset) and the time from the
//      real clock at 2 fps and at 240 fps;
//   6. that the comparisons do catch a value 1 degree off (a test that cannot fail is no test);
//   7. the poser's own one-pose export (getEulerAngles into its ZXY channels) reads back as the same turn;
//   8. bvhTracksOf: the joints the clip writes get a track (a bone, or an extra track), the others are written 0;
//      the root's position only when the clip writes mPelvis';
//   9. the window's rules (motionsession.h): followActions over every state and flag, BeforeKeeper (the pose to
//      restore only after a clip went on, the first one kept), PoseEditT (one entry per edit, the snap held
//      SNAP_HOLD_SEC, nested and twice);
// SL's own loader (LLBVHLoader) is not linked: it reads anim.ini through gDirUtilp and writes its keyframes through
// llmessage's LLDataPacker, which this stand-alone build has neither of. Its sums are reproduced instead with the
// real mayaQ and StringToOrder, the frame of makeTranslation() and the outRot line of serialize()
// (llbvhloader.cpp: bvhStringToOrder, makeTranslation, serialize's frameRotInv * inRot * frameRot).
// A missing or unreadable golden file is a failure, not a skip.

#include "linden_common.h"

#include "motionbvh.h"
#include "motionplayback.h"
#include "motionsession.h"
#include "motiontracks.h"

#include "llmath.h"
#include "m3math.h"

#include <boost/json/src.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace plus::posermotion;
namespace json    = boost::json;
namespace history = plus::posermcp::history;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static constexpr double TOL_DEG = 0.01;

static double deg(const LLQuaternion& a, const LLQuaternion& b)
{
    return rotationAngle(a, b) * RAD_TO_DEG;
}

static bool within(double v, double tol)
{
    return std::isfinite(v) && v <= tol;
}

// ---------------------------------------------------------------- SL's loader, as llbvhloader.cpp does it
static const char* AXES = "XYZ";

/// The rotation the loader makes of a key's angles (by axis) for a joint whose rotation channels are in this order.
static LLQuaternion loaderRotation(const LLVector3& deg_by_axis, const BvhChannel order[3])
{
    // bvhStringToOrder(): the file's order reversed, then StringToOrder()
    const char reversed[4] = { AXES[order[2].axis], AXES[order[1].axis], AXES[order[0].axis], 0 };
    const LLQuaternion in_rot = mayaQ(deg_by_axis.mV[VX], deg_by_axis.mV[VY], deg_by_axis.mV[VZ], StringToOrder(reversed));
    // makeTranslation()
    LLMatrix3 fm;
    fm.setRows(LLVector3(0, 1, 0), LLVector3(0, 0, 1), LLVector3(1, 0, 0));
    const LLQuaternion frame_rot(fm);
    // outRot = frameRotInv * inRot * frameRot (no merge joints, offsetRot identity, the reference frame of zeros)
    return ~frame_rot * in_rot * frame_rot;
}

/// The loader's position of a 6-channel joint: (x, y, z) of the file in inches to SL metres.
static LLVector3 loaderPosition(const LLVector3& bvh_inches)
{
    LLMatrix3 fm;
    fm.setRows(LLVector3(0, 1, 0), LLVector3(0, 0, 1), LLVector3(1, 0, 0));
    return bvh_inches * fm / BVH_INCHES_PER_M;
}

static std::vector<BvhChannel> rotations(const char* order)
{
    std::vector<BvhChannel> ch;
    for (int i = 0; i < 3; ++i)
    {
        BvhChannel c;
        c.kind = BvhChannel::Kind::Rotation;
        c.axis = (U8)(order[i] - 'X');
        ch.push_back(c);
    }
    return ch;
}

static const char* ORDERS[6] = { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" };

static LLQuaternion randomRotation(std::mt19937& rng)
{
    std::normal_distribution<float> n(0.f, 1.f);
    LLQuaternion q(n(rng), n(rng), n(rng), n(rng));
    q.normalize();
    return q;
}

// ---------------------------------------------------------------- 1. the angles
static void testAngles()
{
    std::mt19937 rng(324);
    double worst = 0.0;
    long   count = 0;
    for (const char* order : ORDERS)
    {
        const std::vector<BvhChannel> ch = rotations(order);
        std::vector<LLQuaternion> qs;
        for (int n = 0; n < 2000; ++n)
        {
            qs.push_back(randomRotation(rng));
        }
        // single axes, the identity, and the gimbal lines of this order (the middle axis at +-90 degrees)
        qs.push_back(LLQuaternion::DEFAULT);
        for (int a = 0; a < 3; ++a)
        {
            LLVector3 axis;
            axis.mV[a] = 1.f;
            qs.push_back(LLQuaternion(0.7f, axis));
            qs.push_back(LLQuaternion(-2.9f, axis));
        }
        for (int n = 0; n < 200; ++n)
        {
            LLVector3 d((F32)(n % 7) * 13.f - 40.f, (n % 2) ? 90.f : -90.f, (F32)(n % 5) * 21.f - 50.f);
            LLVector3 by_axis;
            by_axis.mV[ch[0].axis] = d.mV[0];
            by_axis.mV[ch[1].axis] = d.mV[1] + (F32)((n % 3) - 1) * 1e-4f; // on and next to the line
            by_axis.mV[ch[2].axis] = d.mV[2];
            qs.push_back(loaderRotation(by_axis, ch.data()));
        }
        for (const LLQuaternion& q : qs)
        {
            const LLVector3 a = bvhAnglesDegrees(q, ch.data());
            const double d = deg(loaderRotation(a, ch.data()), q);
            ++count;
            if (!std::isfinite(d) || d > worst)
            {
                worst = d;
            }
        }
    }
    std::printf("1. angles: %ld rotations, 6 orders, worst %.5f deg\n", count, worst);
    CHECK(count > 10000);
    CHECK(within(worst, TOL_DEG));
}

// ---------------------------------------------------------------- 2. the hierarchy
// A made-up hierarchy in the form FSFloaterPoser::writeBvhFragment writes, with all six channel orders its
// translations can give (SWAP_NOTHING XYZ, SWAP_YAW_AND_ROLL ZYX, SWAP_ROLL_AND_PITCH XZY, SWAP_X2Z_Y2X_Z2Y YZX,
// SWAP_X2Y_Y2Z_Z2X ZXY, SWAP_YAW_AND_PITCH YXZ). The poser writes every joint ZXY today (its only
// bvh_joint_transform_ string, mHead, is that one too); the writer follows whatever order the file says.
static const char* HIERARCHY =
    "HIERARCHY\n"
    "ROOT mPelvis\n"
    "{\n"
    "\tOFFSET 0.000000 0.000000 0.000000\n"
    "\tCHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n"
    "\tJOINT mTorso\n"
    "\t{\n"
    "\t\tOFFSET 0.000000 3.422050 0.000000\n"
    "\t\tCHANNELS 3 Zrotation Yrotation Xrotation\n"
    "\t\tJOINT mCollarLeft\n"
    "\t\t{\n"
    "\t\t\tOFFSET 3.0 2.0 1.0\n"
    "\t\t\tCHANNELS 3 Xrotation Yrotation Zrotation\n"
    "\t\t\tJOINT mShoulderLeft\n"
    "\t\t\t{\n"
    "\t\t\t\tOFFSET 3.0 0.0 0.0\n"
    "\t\t\t\tCHANNELS 3 Xrotation Zrotation Yrotation\n"
    "\t\t\t\tEnd Site\n"
    "\t\t\t\t{\n"
    "\t\t\t\t\tOFFSET 1.0 0.0 0.0\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t\tJOINT mNeck\n"
    "\t\t{\n"
    "\t\t\tOFFSET 0.0 5.0 0.0\n"
    "\t\t\tCHANNELS 3 Yrotation Zrotation Xrotation\n"
    "\t\t\tEnd Site\n"
    "\t\t\t{\n"
    "\t\t\t\tOFFSET 0.0 1.0 0.0\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "\tJOINT mHipLeft\n"
    "\t{\n"
    "\t\tOFFSET 1.0 -1.0 0.0\n"
    "\t\tCHANNELS 3 Zrotation Xrotation Yrotation\n"
    "\t\tEnd Site\n"
    "\t\t{\n"
    "\t\t\tOFFSET 0.0 -1.0 0.0\n"
    "\t\t}\n"
    "\t}\n"
    "\tJOINT mGroin\n"
    "\t{\n"
    "\t\tOFFSET 0.0 -1.0 0.0\n"
    "\t\tCHANNELS 3 Yrotation Xrotation Zrotation\n"
    "\t\tEnd Site\n"
    "\t\t{\n"
    "\t\t\tOFFSET 0.0 -1.0 0.0\n"
    "\t\t}\n"
    "\t}\n"
    "}\n"
    "MOTION\n"
    "Frames: 2\n";

static bool readText(const std::string& text, std::vector<BvhJoint>& out)
{
    std::istringstream in(text);
    return readBvhHierarchy(in, out);
}

static void testHierarchy(std::vector<BvhJoint>& joints)
{
    CHECK(readText(HIERARCHY, joints));
    CHECK(joints.size() == 7);
    if (joints.size() == 7)
    {
        CHECK(joints[0].name == "mPelvis" && joints[0].channels.size() == 6);
        CHECK(joints[1].name == "mTorso" && joints[2].name == "mCollarLeft" && joints[3].name == "mShoulderLeft");
        CHECK(joints[4].name == "mNeck" && joints[5].name == "mHipLeft" && joints[6].name == "mGroin");
        CHECK(joints[0].channels[3].axis == 2 && joints[0].channels[4].axis == 0 && joints[0].channels[5].axis == 1);
        CHECK(joints[3].channels[0].axis == 0 && joints[3].channels[1].axis == 2 && joints[3].channels[2].axis == 1);
    }

    std::vector<BvhJoint> bad;
    const std::string root_line = "ROOT mPelvis\n{\nCHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n";
    CHECK(!readText("", bad) && bad.empty());
    CHECK(!readText("HIERARCHY\nJOINT mTorso\n{\nCHANNELS 3 Xrotation Yrotation Zrotation\n}\n", bad)); // no ROOT first
    CHECK(!readText(root_line + "}\nROOT mOther\n{\nCHANNELS 3 Xrotation Yrotation Zrotation\n}\n", bad)); // two roots
    CHECK(!readText(root_line + "JOINT mTorso\n{\nOFFSET 0 0 0\n}\n}\n", bad));                         // no channels
    CHECK(!readText(root_line + "JOINT mTorso\n{\nCHANNELS 3 Xrotation Xrotation Zrotation\n}\n}\n", bad)); // an axis twice
    CHECK(!readText(root_line + "JOINT mTorso\n{\nCHANNELS 3 Xrotation Yrotation Wrotation\n}\n}\n", bad)); // no such axis
    CHECK(!readText(root_line + "JOINT mTorso\n{\nCHANNELS 2 Xrotation Yrotation\n}\n}\n", bad));         // 2 channels
    CHECK(!readText(root_line + "JOINT mTorso\n{\nCHANNELS 3 Xrotation Yrotation\n}\n}\n", bad));         // cut short
    CHECK(!readText("ROOT mPelvis\n{\nCHANNELS 6 Yposition Xposition Zposition Zrotation Xrotation Yrotation\n}\n", bad)); // positions out of order
    CHECK(!readText("ROOT mPelvis\n{\nCHANNELS 6 Zrotation Xrotation Yrotation Xposition Yposition Zposition\n}\n", bad)); // rotations first
    CHECK(!readText(root_line + "CHANNELS 3 Xrotation Yrotation Zrotation\n}\n", bad)); // a second CHANNELS for a joint
    std::string many = root_line;
    for (int i = 0; i < BVH_MAX_JOINTS; ++i)
    {
        many += "JOINT j" + std::to_string(i) + "\n{\nCHANNELS 3 Xrotation Yrotation Zrotation\n}\n";
    }
    CHECK(!readText(many + "}\n", bad));
    CHECK(bad.empty());
    std::printf("2. hierarchy: read, and the 12 bad ones refused\n");
}

// ---------------------------------------------------------------- 3. the motion
static MotionClip makeClip(S32 frames, S32 joint_count, std::mt19937& rng)
{
    MotionClip clip;
    clip.frame_count = frames;
    clip.joint_count = joint_count;
    for (S32 f = 0; f < frames; ++f)
    {
        for (S32 j = 0; j < joint_count; ++j)
        {
            clip.local.push_back(randomRotation(rng));
        }
        clip.pelvis.push_back(LLVector3(0.01f * (F32)f, -0.02f * (F32)f, 1.0f + 0.003f * (F32)f));
    }
    return clip;
}

static std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        out.push_back(line);
    }
    return out;
}

static std::vector<double> numbers(const std::string& line)
{
    std::vector<double> out;
    std::istringstream in(line);
    double v;
    while (in >> v)
    {
        out.push_back(v);
    }
    return out;
}

/// Reads a written MOTION back the loader's way and compares it with the clip: the worst rotation (degrees) and
/// root position (m), and how many joint frames were compared (-1: the text is not the right shape).
static long readBack(const std::vector<BvhJoint>& joints, const BvhClipTracks& tracks, const MotionClip& clip,
                     const std::string& text, double& worst_rot, double& worst_pos)
{
    worst_rot = worst_pos = 0.0;
    const S32 frames = clip.frame_count;
    const std::vector<std::string> l = lines(text);
    if ((S32)l.size() != 3 + 1 + frames || l[0] != "MOTION" || l[1] != "Frames: " + std::to_string(frames + 1))
    {
        return -1;
    }
    float frame_time = 0.f;
    if (std::sscanf(l[2].c_str(), "Frame Time: %f", &frame_time) != 1 || std::fabs(frame_time - 1.f / 30.f) >= 1e-6f)
    {
        return -1;
    }
    size_t channels = 0;
    for (const BvhJoint& j : joints)
    {
        channels += j.channels.size();
    }
    const std::vector<double> zero = numbers(l[3]);
    if (zero.size() != channels)
    {
        return -1;
    }
    for (double v : zero)
    {
        if (v != 0.0)
        {
            return -1;
        }
    }
    long compared = 0;
    for (S32 f = 0; f < frames; ++f)
    {
        const std::vector<double> v = numbers(l[(size_t)(4 + f)]);
        if (v.size() != channels)
        {
            return -1;
        }
        size_t at = 0;
        for (size_t n = 0; n < joints.size(); ++n)
        {
            const BvhJoint& joint = joints[n];
            size_t first_rot = 0;
            if (joint.channels.size() == 6)
            {
                // as the loader: three positions in the file's x, y, z
                const LLVector3 pos  = loaderPosition(LLVector3((F32)v[at], (F32)v[at + 1], (F32)v[at + 2]));
                const LLVector3 want = (n == 0 && tracks.root_position) ? clip.pelvis[(size_t)f] - tracks.pelvis_rest
                                                                        : LLVector3::zero;
                worst_pos = std::max(worst_pos, (double)(pos - want).length());
                at += 3;
                first_rot = 3;
            }
            // as the loader: key.mRot[axis of the channel] = the value read
            LLVector3 by_axis;
            for (size_t c = first_rot; c < joint.channels.size(); ++c)
            {
                by_axis.mV[joint.channels[c].axis] = (F32)v[at++];
            }
            const LLQuaternion got   = loaderRotation(by_axis, &joint.channels[first_rot]);
            const S32          rest  = tracks.rest_of_joint[n];
            const S32          extra = tracks.extra_rot_of_joint[n];
            const LLQuaternion want  = rest >= 0    ? clip.local[(size_t)f * clip.joint_count + rest]
                                     : extra >= 0   ? clip.extra_rot[(size_t)f * clip.extra_rot_count + extra]
                                                    : LLQuaternion::DEFAULT;
            worst_rot = std::max(worst_rot, deg(got, want));
            ++compared;
        }
    }
    return compared;
}

static void testMotion(const std::vector<BvhJoint>& joints)
{
    std::mt19937 rng(1113);
    const S32 frames = 34, joint_count = 9, extra_count = 2;
    MotionClip clip = makeClip(frames, joint_count, rng);
    clip.extra_rot_count = extra_count;
    for (S32 i = 0; i < frames * extra_count; ++i)
    {
        clip.extra_rot.push_back(randomRotation(rng));
    }
    BvhClipTracks tracks;
    tracks.pelvis_rest        = LLVector3(0.f, 0.f, 1.067f);
    tracks.rest_of_joint      = { 0, 3, 5, 8, -1, 2, -1 };
    tracks.extra_rot_of_joint = { -1, -1, -1, -1, 1, -1, -1 }; // mNeck from an extra track, mGroin from none
    tracks.root_position      = true;

    double worst_rot = 0.0, worst_pos = 0.0;
    std::ostringstream out;
    CHECK(writeBvhMotion(out, joints, tracks, clip));
    const long compared = readBack(joints, tracks, clip, out.str(), worst_rot, worst_pos);
    std::printf("3. motion: %ld joint frames (an extra track among them), worst %.5f deg, pelvis %.4f mm\n", compared,
                worst_rot, worst_pos * 1000.0);
    CHECK(compared == (long)frames * (long)joints.size());
    CHECK(within(worst_rot, TOL_DEG));
    CHECK(within(worst_pos, 0.00001));

    // the root's position left at 0 when the clip does not move the pelvis
    BvhClipTracks still = tracks;
    still.root_position = false;
    std::ostringstream out_still;
    CHECK(writeBvhMotion(out_still, joints, still, clip));
    CHECK(readBack(joints, still, clip, out_still.str(), worst_rot, worst_pos) == compared);
    CHECK(within(worst_rot, TOL_DEG) && worst_pos == 0.0);
    // ...and the comparison does see a moved root when it should not move (not a test that cannot fail)
    CHECK(readBack(joints, still, clip, out.str(), worst_rot, worst_pos) == compared && worst_pos > 0.01);

    // refused, and nothing written
    auto refused = [&](const BvhClipTracks& t, const MotionClip& c)
    {
        std::ostringstream o;
        const bool ok = writeBvhMotion(o, joints, t, c);
        return !ok && o.str().empty();
    };
    BvhClipTracks short_tracks = tracks;
    short_tracks.rest_of_joint.pop_back();
    CHECK(refused(short_tracks, clip));
    BvhClipTracks short_extra = tracks;
    short_extra.extra_rot_of_joint.pop_back();
    CHECK(refused(short_extra, clip));
    BvhClipTracks far_tracks = tracks;
    far_tracks.rest_of_joint[2] = joint_count;
    CHECK(refused(far_tracks, clip));
    BvhClipTracks far_extra = tracks;
    far_extra.extra_rot_of_joint[4] = extra_count;
    CHECK(refused(far_extra, clip));
    MotionClip cut_extra = clip;
    cut_extra.extra_rot.pop_back();
    CHECK(refused(tracks, cut_extra));
    MotionClip nan_extra = clip;
    nan_extra.extra_rot[1] = LLQuaternion(NAN, 0.f, 0.f, 1.f);
    CHECK(refused(tracks, nan_extra));
    MotionClip nan_clip = clip;
    nan_clip.local[5] = LLQuaternion(NAN, 0.f, 0.f, 1.f);
    CHECK(refused(tracks, nan_clip));
    MotionClip flat_clip = clip;
    flat_clip.local[5] = LLQuaternion(0.f, 0.f, 0.f, 0.f);
    CHECK(refused(tracks, flat_clip));
    MotionClip far_clip = clip;
    far_clip.pelvis[3] = LLVector3(0.f, 0.f, 50.f);
    CHECK(refused(tracks, far_clip));
    MotionClip cut_clip = clip;
    cut_clip.local.pop_back();
    CHECK(refused(tracks, cut_clip));
    CHECK(refused(tracks, MotionClip()));
    std::vector<BvhJoint> none;
    std::ostringstream o;
    CHECK(!writeBvhMotion(o, none, BvhClipTracks(), clip) && o.str().empty());
}

// ---------------------------------------------------------------- 4. the key poses
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

static history::JointState joint(const std::string& name, const LLQuaternion& q, bool enabled = true, bool rotation = true)
{
    history::JointState s;
    s.name     = name;
    s.target   = history::Quat{ q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW] };
    s.own      = s.target;
    s.enabled  = enabled;
    s.rotation = rotation;
    return s;
}

static void testTracks(const std::string& golden)
{
    const json::value doc = readJson(golden + "sl_rest_skeleton.json");
    if (doc.is_null())
    {
        return;
    }
    SomaRetarget retarget;
    CHECK(retarget.init(restFromJson(doc)) == RetargetResult::Ok);
    if (!retarget.ready())
    {
        return;
    }

    const LLQuaternion elbow_a(0.5f, LLVector3::z_axis), elbow_b(-0.4f, LLVector3::z_axis);
    history::Pose a, b;
    a.joints.push_back(joint("mPelvis", LLQuaternion::DEFAULT));
    a.joints.back().position = history::Vec3{ 0.f, 0.f, -0.1f };
    b.joints.push_back(joint("mPelvis", LLQuaternion::DEFAULT));
    b.joints.back().position = history::Vec3{ 0.2f, 0.f, -0.3f };
    a.joints.push_back(joint("mElbowLeft", elbow_a));
    b.joints.push_back(joint("mElbowLeft", elbow_b));
    a.joints.push_back(joint("mHandIndex1Left", LLQuaternion::DEFAULT, false)); // driven in neither: not written
    b.joints.push_back(joint("mHandIndex1Left", LLQuaternion(0.3f, LLVector3::y_axis), false));
    a.joints.push_back(joint("mWristLeft", LLQuaternion::DEFAULT, false));      // driven in the end only: written
    b.joints.push_back(joint("mWristLeft", LLQuaternion(0.2f, LLVector3::x_axis), true));
    a.joints.push_back(joint("LEFT_HANDLE", LLQuaternion::DEFAULT, true, false)); // a collision volume: offsets only
    a.joints.back().scale = history::Vec3{ 0.1f, 0.f, 0.f };
    b.joints.push_back(joint("LEFT_HANDLE", LLQuaternion::DEFAULT, true, false));
    a.joints.push_back(joint("mNotABone", LLQuaternion(0.1f, LLVector3::x_axis))); // rotating, not in the skeleton
    b.joints.push_back(joint("mNotABone", LLQuaternion(0.6f, LLVector3::x_axis)));
    a.joints.push_back(joint("mOnlyInStart", LLQuaternion::DEFAULT));

    TrackLayout layout;
    KeyPose ka, kb;
    CHECK(makeKeyPoses(retarget, a, b, layout, ka, kb) == TrackResult::Ok);
    CHECK(layout.joints.size() == 5); // pelvis, elbow, wrist, the handle, the extra bone
    CHECK((S32)ka.sl.local.size() == retarget.jointCount() && (S32)kb.sl.local.size() == retarget.jointCount());
    CHECK(ka.extra_rot.size() == 1 && kb.extra_rot.size() == 1);
    CHECK(ka.extra_vec.size() == 1 && kb.extra_vec.size() == 1); // the handle's scale (its position is 0 at both ends)
    const LLVector3 rest_pelvis = retarget.pelvisRest();
    CHECK((ka.sl.pelvis - (rest_pelvis + LLVector3(0.f, 0.f, -0.1f))).length() < 1e-6f);
    CHECK((kb.sl.pelvis - (rest_pelvis + LLVector3(0.2f, 0.f, -0.3f))).length() < 1e-6f);
    const S32 elbow = retarget.find("mElbowLeft");
    const S32 index = retarget.find("mHandIndex1Left");
    CHECK(elbow >= 0 && index >= 0);
    // a finger is not one of the model's 21 pairs, but a bone of the rest skeleton: a written finger has rest >= 0
    // (its own sl.local track, which the BVH writes), not an extra track
    CHECK(index >= 0 && !retarget.isDriven(index));
    if (elbow >= 0 && index >= 0)
    {
        CHECK(deg(ka.sl.local[(size_t)elbow], elbow_a) < 1e-4 && deg(kb.sl.local[(size_t)elbow], elbow_b) < 1e-4);
        // not written, but in the skeleton the model sees
        CHECK(deg(kb.sl.local[(size_t)index], LLQuaternion(0.3f, LLVector3::y_axis)) < 1e-4);
    }
    bool saw_wrist = false, saw_index = false;
    for (S32 i = 0; i < (S32)layout.joints.size(); ++i)
    {
        const TrackJoint& t = layout.joints[(size_t)i];
        saw_wrist = saw_wrist || (t.name == "mWristLeft" && !t.enabled && t.rest >= 0);
        saw_index = saw_index || t.name == "mHandIndex1Left";
        TrackValues v;
        CHECK(trackValues(layout, i, kb, v));
        if (t.name == "mPelvis")
        {
            CHECK(t.pelvis && v.has_position && (v.position - LLVector3(0.2f, 0.f, -0.3f)).length() < 1e-6f);
        }
        if (t.name == "LEFT_HANDLE")
        {
            CHECK(!v.has_rotation && !v.has_position && v.has_scale);
            TrackValues va;
            CHECK(trackValues(layout, i, ka, va) && (va.scale - LLVector3(0.1f, 0.f, 0.f)).length() < 1e-6f);
        }
        if (t.name == "mNotABone")
        {
            CHECK(t.rest < 0 && t.extra_rot == 0 && v.has_rotation && deg(v.rotation, LLQuaternion(0.6f, LLVector3::x_axis)) < 1e-4);
        }
    }
    CHECK(saw_wrist && !saw_index);
    TrackValues v;
    CHECK(!trackValues(layout, -1, ka, v) && !trackValues(layout, (S32)layout.joints.size(), ka, v));
    KeyPose cut = ka;
    cut.sl.local.resize(3);
    CHECK(!trackValues(layout, 1, cut, v)); // the elbow's index is past a cut pose

    // a finger driven in one pose: written from its own skeleton track (so the BVH has it too)
    {
        history::Pose b_finger = b;
        b_finger.joints[2].enabled = true; // mHandIndex1Left
        TrackLayout lf;
        KeyPose fa, fb;
        CHECK(makeKeyPoses(retarget, a, b_finger, lf, fa, fb) == TrackResult::Ok);
        bool finger_bone = false;
        for (const TrackJoint& t : lf.joints)
        {
            finger_bone = finger_bone || (t.name == "mHandIndex1Left" && t.rest == index && t.extra_rot < 0);
        }
        CHECK(finger_bone);
    }

    // refused, and the outputs left as they were
    const size_t before = layout.joints.size();
    history::Pose nan_pose = b;
    nan_pose.joints[1].target = history::Quat{ NAN, 0.f, 0.f, 1.f };
    CHECK(makeKeyPoses(retarget, a, nan_pose, layout, ka, kb) == TrackResult::BadPose && layout.joints.size() == before);
    history::Pose flat = b;
    flat.joints[1].target = history::Quat{ 0.f, 0.f, 0.f, 0.f };
    CHECK(makeKeyPoses(retarget, a, flat, layout, ka, kb) == TrackResult::BadPose);
    history::Pose far_pose = b;
    far_pose.joints[0].position = history::Vec3{ 0.f, 0.f, 40.f };
    CHECK(makeKeyPoses(retarget, a, far_pose, layout, ka, kb) == TrackResult::BadPose);
    history::Pose far_scale = b;
    far_scale.joints[4].scale = history::Vec3{ 1e30f, 0.f, 0.f };
    CHECK(makeKeyPoses(retarget, a, far_scale, layout, ka, kb) == TrackResult::BadPose);
    history::Pose off = a;
    for (history::JointState& s : off.joints)
    {
        s.enabled = false;
    }
    history::Pose off_b = b;
    for (history::JointState& s : off_b.joints)
    {
        s.enabled = false;
    }
    CHECK(makeKeyPoses(retarget, off, off_b, layout, ka, kb) == TrackResult::NoJoints);
    CHECK(makeKeyPoses(SomaRetarget(), a, b, layout, ka, kb) == TrackResult::NotReady);
    CHECK(layout.joints.size() == before);
    std::printf("4. key poses: %zu joints written, the refused ones left the outputs\n", before);
}

// ---------------------------------------------------------------- 5. the playback
static void testPlayback()
{
    ClipPlayback p;
    CHECK(p.state() == ClipState::None && !p.hasClip());
    CHECK(!p.play(0.0) && !p.seek(3));
    CHECK(!p.load(CLIP_MIN_FRAMES - 1) && p.state() == ClipState::None);
    CHECK(!p.load(CLIP_MAX_FRAMES + 1) && p.state() == ClipState::None);
    CHECK(!p.load(TIMELINE_MIN_FRAMES - 1, TIMELINE_MIN_FRAMES, TIMELINE_MAX_FRAMES));
    CHECK(!p.load(TIMELINE_MAX_FRAMES + 1, TIMELINE_MIN_FRAMES, TIMELINE_MAX_FRAMES));
    CHECK(p.load(TIMELINE_MAX_FRAMES, TIMELINE_MIN_FRAMES, TIMELINE_MAX_FRAMES) && p.frameCount() == TIMELINE_MAX_FRAMES);
    CHECK(p.load(TIMELINE_MIN_FRAMES, TIMELINE_MIN_FRAMES, TIMELINE_MAX_FRAMES));
    p.reset();
    CHECK(!p.reload(40)); // C0: nothing to keep a position of
    CHECK(p.load(34) && p.state() == ClipState::Stopped && p.frame() == 0);
    CHECK(p.advance(5.0, LoopMode::Once).frame == 0 && p.state() == ClipState::Stopped); // not playing: the frame shown

    // play at 2 fps and at 240 fps: the same frame at the same time
    const F64 t0 = 100000.0; // long after the start of the viewer (F64, no rounding)
    for (F64 fps : { 2.0, 240.0 })
    {
        CHECK(p.seek(0) && p.play(t0));
        CHECK(!p.play(t0)); // already playing
        S32 last = -1;
        bool went_back = false;
        for (F64 t = t0; p.playing() && t < t0 + 5.0; t += 1.0 / fps)
        {
            const PlayCursor c = p.advance(t, LoopMode::Once);
            went_back = went_back || c.frame < last;
            last = c.frame;
            if (!c.finished)
            {
                // the frame of the time since the start (play() starts a thousandth of a frame early)
                const double x = (t - t0) * CLIP_FPS;
                CHECK(c.frame >= (S32)std::floor(x) && c.frame <= (S32)std::floor(x + 0.002));
            }
        }
        CHECK(!went_back);
        CHECK(p.state() == ClipState::Stopped && p.frame() == 33); // the end: C1 at the last frame
    }
    // from the end, play starts again from 0
    CHECK(p.play(t0) && p.advance(t0, LoopMode::Once).frame == 0);
    // stop keeps the frame reached
    p.advance(t0 + 10.5 / CLIP_FPS, LoopMode::Once);
    p.stop();
    CHECK(p.state() == ClipState::Stopped && p.frame() == 10);
    // and play goes on from there
    CHECK(p.play(t0 + 50.0) && p.advance(t0 + 50.0, LoopMode::Once).frame == 10);
    // the needle stops it
    CHECK(p.seek(20) && p.state() == ClipState::Stopped && p.frame() == 20);
    CHECK(p.shown().frame == 20 && p.shown().next == 20);
    CHECK(p.seek(99) && p.frame() == 33 && p.seek(-4) && p.frame() == 0);
    // looping: past the end and the hold it comes round, and does not stop
    CHECK(p.play(t0));
    const F64 period = 33.0 / CLIP_FPS + LOOP_HOLD_SEC;
    const PlayCursor c = p.advance(t0 + 2.0 * period + 5.5 / CLIP_FPS, LoopMode::Loop);
    CHECK(c.frame == 5 && p.playing());
    CHECK(p.advance(t0 + 33.0 / CLIP_FPS + 0.1, LoopMode::Loop).frame == 33 && p.playing()); // in the hold
    // NaN or a clock gone back: the start, not a jump
    CHECK(p.advance(NAN, LoopMode::Loop).frame == 0 && p.playing());
    CHECK(p.advance(t0 - 50.0, LoopMode::Loop).frame == 0);
    CHECK(!ClipPlayback().play(NAN));
    p.reset();
    CHECK(p.state() == ClipState::None && p.frame() == 0 && p.frameCount() == 0 && !p.seek(1));
    std::printf("5. playback: the C machine and the clock\n");
}

// ---------------------------------------------------------------- 6. the comparisons can fail
static void testCatches()
{
    const std::vector<BvhChannel> ch = rotations("ZXY");
    const LLQuaternion q(0.8f, LLVector3(0.3f, 0.5f, 0.8f));
    LLVector3 a = bvhAnglesDegrees(q, ch.data());
    a.mV[VY] += 1.f;
    const double off = deg(loaderRotation(a, ch.data()), q);
    CHECK(!within(off, TOL_DEG) && off > 0.5);
    CHECK(!within(NAN, TOL_DEG));
    std::printf("6. a value 1 degree off is caught (%.3f deg)\n", off);
}

// ---------------------------------------------------------------- 7. the poser's own one-pose BVH
// FSFloaterPoser::writeBvhMotion writes LLQuaternion::getEulerAngles(roll, pitch, yaw) as X Y Z. Every joint's
// CHANNELS line is Zrotation Xrotation Yrotation (getBvhJointTranslation reads bvh_joint_transform_<joint>; the XUI
// has only mHead's, SWAP_X2Y_Y2Z_Z2X, the default), so the loader gives the rotation back: its export and this
// writer agree.
static void testOnePoseConvention()
{
    std::mt19937 rng(7);
    const std::vector<BvhChannel> ch = rotations("ZXY");
    double worst = 0.0;
    for (int n = 0; n < 2000; ++n)
    {
        const LLQuaternion q = randomRotation(rng);
        F32 roll, pitch, yaw;
        q.getEulerAngles(&roll, &pitch, &yaw);
        // written in that order into the channels as listed: the loader keys them by the channels' axes
        const F32 written[3] = { roll * RAD_TO_DEG, pitch * RAD_TO_DEG, yaw * RAD_TO_DEG };
        LLVector3 by_axis;
        for (int c = 0; c < 3; ++c)
        {
            by_axis.mV[ch[(size_t)c].axis] = written[c];
        }
        worst = std::max(worst, deg(loaderRotation(by_axis, ch.data()), q));
        // and this writer's angles for the same channels are the same turn
        worst = std::max(worst, deg(loaderRotation(bvhAnglesDegrees(q, ch.data()), ch.data()), q));
    }
    std::printf("7. the poser's one-pose export (ZXY) read back: worst %.5f deg\n", worst);
    CHECK(within(worst, 0.05)); // getEulerAngles is float near the gimbal line
}

// ---------------------------------------------------------------- 8. the BVH tracks of a layout
static void testBvhTracks(const std::vector<BvhJoint>& joints)
{
    TrackLayout layout;
    layout.pelvis_rest = LLVector3(0.f, 0.f, 1.f);
    TrackJoint torso;
    torso.name = "mTorso";
    torso.rest = 3;
    TrackJoint offsets_only; // written for its offsets only (a joint without a rotation track)
    offsets_only.name  = "mShoulderLeft";
    offsets_only.rest  = -1;
    offsets_only.scale = 0;
    TrackJoint neck; // a rotating joint the rest skeleton lacks: its extra track
    neck.name      = "mNeck";
    neck.rest      = -1;
    neck.extra_rot = 1;
    TrackJoint hip;
    hip.name = "mHipLeft";
    hip.rest = 7;
    layout.joints = { torso, offsets_only, neck, hip };
    BvhClipTracks t = bvhTracksOf(joints, layout);
    CHECK(t.rest_of_joint.size() == joints.size() && t.extra_rot_of_joint.size() == joints.size());
    if (t.rest_of_joint.size() == joints.size() && t.extra_rot_of_joint.size() == joints.size() && joints.size() == 7)
    {
        // mPelvis, mCollarLeft, mGroin are not written by this layout; mShoulderLeft has no rotation track
        const S32 rest[7]  = { -1, 3, -1, -1, -1, 7, -1 };
        const S32 extra[7] = { -1, -1, -1, -1, 1, -1, -1 };
        for (int n = 0; n < 7; ++n)
        {
            CHECK(t.rest_of_joint[(size_t)n] == rest[n] && t.extra_rot_of_joint[(size_t)n] == extra[n]);
        }
    }
    CHECK(!t.root_position); // no mPelvis position track in the layout: the root stays
    CHECK((t.pelvis_rest - layout.pelvis_rest).length() == 0.f);
    TrackJoint pelvis;
    pelvis.name   = "mPelvis";
    pelvis.rest   = 0;
    pelvis.pelvis = true;
    layout.joints.push_back(pelvis);
    t = bvhTracksOf(joints, layout);
    CHECK(t.root_position && t.rest_of_joint[0] == 0);
    std::printf("8. BVH tracks: the joints the layout writes (bone or extra track), the root's position with mPelvis'\n");
}

// ---------------------------------------------------------------- 9. the window's rules (motionsession.h)
static void testFollow()
{
    const SelfPoseState states[] = { SelfPoseState::NotLoggedIn, SelfPoseState::NoPoser, SelfPoseState::NotPosing,
                                     SelfPoseState::OtherSelected, SelfPoseState::Posing };
    long cases = 0;
    for (SelfPoseState st : states)
    {
        for (int bits = 0; bits < 32; ++bits)
        {
            FollowInput in;
            in.state        = st;
            in.has_clip     = (bits & 1) != 0;
            in.joints_alive = (bits & 2) != 0;
            in.has_before   = (bits & 4) != 0;
            in.has_keys     = (bits & 8) != 0;
            in.request_out  = (bits & 16) != 0;
            const FollowActions a = followActions(in);
            const bool gone = st == SelfPoseState::NotLoggedIn || st == SelfPoseState::NoPoser || st == SelfPoseState::NotPosing;
            CHECK(a.stop_helper == (st == SelfPoseState::NotLoggedIn));
            CHECK(a.cancel_request == (gone && in.request_out));
            CHECK(a.note_gone == (in.has_clip && (gone || !in.joints_alive)));
            CHECK(a.drop_all == (a.note_gone || (gone && in.has_before)));
            CHECK(a.drop_keys == (st == SelfPoseState::NotLoggedIn && in.has_keys)); // the poser closing keeps the keys
            // an animesh selected (or posing) with a live clip: nothing is dropped
            if (!gone && in.has_clip && in.joints_alive)
            {
                CHECK(!a.drop_all && !a.drop_keys && !a.cancel_request);
            }
            ++cases;
        }
    }
    CHECK(cases == 160);
    std::printf("9a. follow: %ld cases\n", cases);
}

static void testBeforeKeeper()
{
    // the window's flow (MotionFloater::takeAnswer): "generate" asks wanted() and keeps the pose with the request;
    // the answer's outcome settles it (settleBefore). Only Applied makes it the pose to restore.
    BeforeKeeper<int> keeper;
    CHECK(!keeper.has() && keeper.wanted());
    for (AnswerOutcome outcome : { AnswerOutcome::NotForSelf, AnswerOutcome::NotAClip, AnswerOutcome::Gone })
    {
        std::optional<int> with_request;
        if (keeper.wanted())
        {
            with_request = 1;
        }
        settleBefore(outcome, keeper, with_request);
        CHECK(!with_request && !keeper.has() && keeper.wanted()); // nothing to restore; the next press still wants one
    }
    {
        std::optional<int> with_request = 7;
        settleBefore(AnswerOutcome::Applied, keeper, with_request);
        CHECK(!with_request && keeper.has() && keeper.get() == 7);
        keeper.drop();
    }
    // the first clip went on: its pose is the one to restore
    std::optional<int> first = 10;
    keeper.commit(first);
    CHECK(keeper.has() && keeper.get() == 10 && !first && !keeper.wanted());
    // a second generate's clip does not take its place (no pose is taken for it, and one offered is ignored)
    std::optional<int> second = 20;
    keeper.commit(second);
    CHECK(keeper.get() == 10 && !second);
    // nothing offered (the capture failed): nothing changes
    std::optional<int> none;
    keeper.drop();
    keeper.commit(none);
    CHECK(!keeper.has() && keeper.wanted());
    std::printf("9b. the pose from before: only after a clip went on, the first one kept\n");
}

struct FakeEdit
{
    int  begins = 0, holds = 0, ends = 0, snaps = 0, unsnaps = 0;
    bool record_ok = true, snap_ok = true;
    std::string last;
    bool beginRecord() { ++begins; return record_ok; }
    void holdRecord() { ++holds; }
    void endRecord(const char* summary) { ++ends; last = summary; }
    bool startSnap() { if (snap_ok) { ++snaps; } return snap_ok; }
    void stopSnap() { ++unsnaps; }
};

static void testPoseEdit()
{
    {
        PoseEditT<FakeEdit> e;
        e.begin();
        e.begin(); // nested: still one entry, one snap
        CHECK(e.open() && e.snapping() && e.backend().begins == 1 && e.backend().snaps == 1);
        e.hold();
        CHECK(e.backend().holds == 1);
        e.end("played", 100.0);
        e.end("again", 100.0); // the second end does nothing
        CHECK(!e.open() && e.backend().ends == 1 && e.backend().last == "played");
        e.releaseSnapWhenDue(100.0 + SNAP_HOLD_SEC * 0.5);
        CHECK(e.snapping()); // held a moment after the edit
        e.releaseSnapWhenDue(100.0 + SNAP_HOLD_SEC);
        CHECK(!e.snapping() && e.backend().unsnaps == 1);
        e.hold();
        CHECK(e.backend().holds == 1); // closed: no hold
        // a new edit while snapping keeps the snap and opens a new entry
        e.begin();
        CHECK(e.backend().begins == 2 && e.backend().snaps == 2);
        e.releaseSnapWhenDue(1e9); // open: not released
        CHECK(e.snapping());
    }
    {
        // the history not recording (MCP off): no hold, no end, the snap still works; no motion: no snap
        PoseEditT<FakeEdit> e;
        e.backend().record_ok = false;
        e.backend().snap_ok   = false;
        e.begin();
        e.hold();
        e.end("x", 0.0);
        CHECK(e.backend().holds == 0 && e.backend().ends == 0 && !e.snapping());
        e.backend().snap_ok = true;
        e.begin(); // a later begin may start the snap
        CHECK(e.snapping());
        e.releaseSnap();
        CHECK(!e.snapping() && e.backend().unsnaps == 1);
        e.releaseSnap(); // twice: once
        CHECK(e.backend().unsnaps == 1);
    }
    std::printf("9c. pose edit: one entry per edit, the snap held SNAP_HOLD_SEC, nested and twice\n");
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: motionui_test <golden folder>\n");
        return 2;
    }
    std::string golden = argv[1];
    if (!golden.empty() && golden.back() != '\\' && golden.back() != '/')
    {
        golden += '\\';
    }

    testAngles();
    std::vector<BvhJoint> joints;
    testHierarchy(joints);
    if (joints.size() == 7)
    {
        testMotion(joints);
    }
    testTracks(golden);
    testPlayback();
    testCatches();
    testOnePoseConvention();
    if (joints.size() == 7)
    {
        testBvhTracks(joints);
    }
    testFollow();
    testBeforeKeeper();
    testPoseEdit();

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
