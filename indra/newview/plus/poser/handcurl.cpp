// Charis: see handcurl.h.
#include "handcurl.h"

#include <algorithm>
#include <cmath>

namespace plus
{
namespace handcurl
{
namespace
{
// Left-hand raw joint values of the shipped presets (app_settings/plus/poser/hand_presets).
constexpr Raw FINGER_OPEN[FINGER_COUNT][3] = {  // Flat.xml
    {{0.033f, 0.f, 0.25f}, {0.033f, 0.f, -0.013f}, {0.02f, 0.f, 0.f}},
    {{0.007f, 0.f, -0.059f}, {0.033f, 0.f, 0.f}, {0.02f, 0.f, -0.046f}},
    {{0.026f, 0.f, -0.237f}, {0.039f, 0.f, -0.033f}, {0.02f, 0.f, -0.007f}},
    {{0.158f, -0.22f, -0.545f}, {0.039f, 0.031f, -0.033f}, {0.013f, 0.f, 0.105f}},
};
constexpr Raw FINGER_TIGHT[FINGER_COUNT][3] = {  // Tight_Grip.xml
    {{-0.631f, 0.22f, 0.493f}, {-1.196f, 0.534f, 0.046f}, {-1.275f, 0.785f, 0.072f}},
    {{-0.861f, -0.188f, 0.066f}, {-1.386f, 0.188f, -0.013f}, {-1.11f, 0.f, -0.066f}},
    {{-0.946f, -0.094f, -0.361f}, {-1.327f, -0.628f, -0.276f}, {-0.94f, -0.251f, 0.f}},
    {{-0.808f, -0.503f, -0.88f}, {-1.754f, -1.1f, -0.506f}, {-0.972f, -0.534f, -0.039f}},
};
constexpr Raw THUMB_OPEN[3] = {{-0.013f, 0.f, -0.007f}, {-0.039f, 0.f, 0.02f}, {-0.033f, 0.f, 0.f}};  // Flat.xml Thumb1..3
constexpr Raw THUMB_OKAY    = {-0.158f, 0.785f, 0.296f};  // Okay.xml Thumb1: the thumb facing the index finger
constexpr Raw THUMB_THUMBSUP = {0.322f, 0.1885f, 0.1708f};  // Thumbs_Up.xml Thumb1: the thumb arched back (the target of the arch)
constexpr Raw THUMB_OPPOSED = {0.02f, 1.1f, 0.033f};                                                     // Grip.xml Thumb1: across the palm
// The thumb's bend is measured, not derived: the owner bent Thumb2 and Thumb3 of the left hand in the poser's adjust tab until
// they looked like a natural bend toward the palm (Thumb1 at 0) and read the three boxes: Thumb2 up/down 25.84, left/right
// -1.95, roll -10.3; Thumb3 up/down 59.78, left/right -12.8, roll 6.64 (degrees). For these joints the boxes are the raw
// angles as (x, y, z) = (left/right, roll, up/down) (joint_transform_mHandThumb*Left is SWAP_ROLL_AND_PITCH). The bend is
// mostly z. The owner wants that shape at HALF the slider (the slider's end is a deeper bend): the end of the path is the
// measured pose carried on to twice its distance from the open thumb (THUMB_BENT_AT_HALF), with the bend (z) held to
// THUMB_BEND_LIMIT_Z (see below) and the other numbers shortened in proportion.
// (Two earlier ends were wrong: Two_Finger_Salute's Thumb2 with y 1.23 is the opposition carried on, the thumb lying down on
// the palm; a hinge worked out from the skeleton's bone directions bent the thumb out of its joint.)
constexpr Raw   THUMB_BENT_MEASURED[2] = {{-0.034f, -0.180f, 0.451f}, {-0.223f, 0.116f, 1.043f}};
constexpr float THUMB_BENT_AT_HALF     = 0.5f;  // the curl of the measured pose
// The root of the thumb (Thumb1) moves three ways, the owner's sliders: WRAP (a: the thumb folded over the palm toward the little
// finger, Flat -> Grip.xml, a turn about y), SWING (w, both ways: +1 the thumb turned toward the palm to face the fingers, Okay.xml's
// Thumb1 with the part along the wrap taken out, a swing in x and z; -1 the thumb spread away from the hand in the plane of the
// palm, a direction of its own, a turn about z: see THUMB_AWAY_ANGLE_DEG below) and ARCH
// (x: the thumb arched back toward the back of the hand, Thumbs_Up.xml's Thumb1 with the parts along the other two taken out,
// mostly a turn about x; a positive x lifts the thumb toward +z, the back of the hand). The targets in thumb1Basis() are the poses the
// directions point to; thumb1Basis() makes wrap, swing (positive side) and arch orthogonal (Okay = wrap 0.72 and swing 1; Thumbs_Up
// = arch 1 with a little of the others). The swing's negative side is not orthogonal to the arch: on that side the wrap and arch
// values read a little different (Thumbs_Up's arch 1.045, Relaxed's 1.10: custom starts at 1.15, so retargeting the arch can make
// Relaxed show as custom). Three directions span the three numbers of Thumb1, so nothing is left over: Thumb1 is "custom" only when a
// coordinate is out of its range (the swing -1..1, the others 0..1).
constexpr int   THUMB1_AXES = 3;
constexpr int   THUMB1_SWING = 1;
constexpr float THUMB1_RANGE_LOW[THUMB1_AXES] = {0.f, -1.f, 0.f};  // the high end is 1 for all
// The swing's negative side (the thumb spread away from the hand) is its own direction: a turn of Thumb1 about z that takes the
// thumb to THUMB_AWAY_ANGLE_DEG degrees from the index finger in the plane of the palm (at swing -1). THE ONE NUMBER TO TUNE BY EYE.
// The bone of Thumb1 points along (0.66, 0.75) (x, y: 41 degrees from the hand's axis y); a turn about z by -t takes it to
// (0.66 cos t + 0.75 sin t, 0.75 cos t - 0.66 sin t); the index finger lies at 11 degrees (its bone 25 degrees, and Flat.xml turns it
// 0.25 rad the other way). So t = angle + 11 - 41 degrees: 60 degrees between the thumb and the index finger is t = 0.52 rad (30
// degrees of turn); 90 degrees (the first try, "too far") was 1.05 rad, and 16 degrees of turn ("too little") 0.28 rad.
// USABLE RANGE: about 45 degrees and more (Pointing and Relaxed stay inside the swing's range; at 40 they read -1.25 and -1.19 and
// show as custom). The thumb of the flat hand is 30.4 degrees from the index finger: at 30.4 the away direction is 0 and the
// three-direction solution has no inverse, below it the away direction lies on the swing's side and -1 would read as positive.
// (This is a number to tune around 60, not below 45.) The angle is a thumb bone to an index bone; the whole thumb to the whole index
// finger looks about 4 degrees less.
constexpr float THUMB_AWAY_ANGLE_DEG = 60.f;
constexpr float THUMB_BONE_X         = 0.66f;   // the bone of Thumb1 (avatar_skeleton.xml: end 0.028 0.032, as a direction)
constexpr float THUMB_BONE_Y         = 0.75f;
constexpr float INDEX_BONE_X         = 0.017f;  // the bone of Index1 (end 0.017 0.036)
constexpr float INDEX_BONE_Y         = 0.036f;
// Thumb1's turn carries the axes of Thumb2 and 3 with it, so the bend of Thumb2 and 3 depends on (a, w, x). The measurements were made
// with the thumb at (a, w, x) = (0, 0, 0): THUMB_BENT_MEASURED above, and at the corners (1, 0, 0) (the wrap at its maximum,
// Thumb1 y 1.10; the owner bent Thumb2 and 3 naturally, like the thumb of a fist, and read Thumb2 up/down 63.19, left/right 2.27,
// roll -26.7; Thumb3 100.5, -46.5, -8.86: mostly z, Thumb2 1.10, Thumb3 1.75, with Thumb3 x -0.81) and (0, 1, 0) (the swing at its
// maximum; Thumb2 up/down 37.03, left/right 0.01, roll -0.12; Thumb3 35.18, -0.14, 0.05: a pure z bend, Thumb2 0.646, Thumb3 0.614,
// shallower than the other corners). (0, 0, 1) (the arch at its maximum) and (0, -1, 0) (the swing at its minimum, the thumb away from
// the hand) are NOT MEASURED YET: their entries are placeholders equal to the (0, 0, 0) measurement (they do not change the end);
// replace them by the owner's measurements with that Thumb1 pose (Thumb2 and 3 bent naturally).
constexpr Raw THUMB_BENT_MEASURED_AXIS[THUMB1_AXES + 1][2] = {
    {{0.040f, -0.466f, 1.103f}, {-0.812f, -0.155f, 1.754f}},      // wrap 1
    {{0.0002f, -0.0021f, 0.646f}, {-0.0024f, 0.0009f, 0.614f}},   // swing 1
    {{-0.034f, -0.180f, 0.451f}, {-0.223f, 0.116f, 1.043f}},      // arch 1: PLACEHOLDER, as (0, 0, 0)
    {{-0.034f, -0.180f, 0.451f}, {-0.223f, 0.116f, 1.043f}},      // swing -1 (away from the hand): PLACEHOLDER, as (0, 0, 0)
};
// The ends: with the thumb at the origin the measured pose is at HALF the slider, so the end is carried to twice its distance from the
// open thumb (THUMB_BENT_AT_HALF; the z of Thumb3 2.09 is within its limit). At the wrapped corner the measured pose is the shape the
// owner wants at the END of the slider (the thumb lying over the palm, the tip near the middle of it; twice would be z 3.5 rad, 200
// degrees), so the end is the measured pose itself (reach 1.0). The ends of the corners are joined by a plane (linear in a, w and x),
// and so is the middle of the slider. A limit on z (the other numbers shortened in proportion) is kept, for the corners and the plane
// between them (it does not bind now: it is there for a retuned reach or a measured corner).
constexpr float THUMB_BENT_REACH[THUMB1_AXES + 2] = {1.f / THUMB_BENT_AT_HALF, 1.0f, 1.f / THUMB_BENT_AT_HALF, 1.f / THUMB_BENT_AT_HALF, 1.f / THUMB_BENT_AT_HALF};  // [origin, wrap, swing, arch, swing away]: the end is the measured pose carried this many times as far from the open thumb
// The least bend of an end, z from the open thumb (about 23 degrees): the plane through the corner ends can get shallow where several
// corners add up (a corner measured shallow, a sum like e10 + e01 - e00), and below this the bend slider would hardly act.
constexpr float THUMB_BEND_MIN_Z = 0.4f;
constexpr float THUMB_BEND_LIMIT_Z[THUMB1_AXES + 2][2] = {{1000.f, 2.1f}, {1.6f, 2.5f}, {1000.f, 2.1f}, {1000.f, 2.1f}, {1000.f, 2.1f}};  // [origin, wrap, swing, arch, swing away][Thumb2, Thumb3]

// Tight_Grip is not a full fist (in-world it leaves the fingers about half bent; the slider's end is deeper): the closed
// end of the curl path is a full fist. Its bends (x, left hand) are CLOSED_BEND_X: knuckle, middle joint, tip joint
// (the little finger's middle joint goes deeper, as in the presets, which reach 1.23, 2.02 and 1.27); the fingertips come to
// the palm without going through it. The turning in y and z is Tight_Grip's carried on, CLOSED_SCALE times as far from the open
// hand: the path of a finger is straight from the open hand to Tight_Grip (curl 1 / CLOSED_SCALE = 0.8) and straight on from
// there to the full fist. In-world tuning is these numbers. CLOSED_SCALE must stay below about 1.35: the little finger's
// middle joint turns 1.1 rad in y at Tight_Grip, and no slider moves a y past JOINT_Y_LIMIT_RAD (handcurl.h: a y near pi/2 is
// read back wrapped, getEulerAngles keeps y within +-pi/2, which would throw the curl value): an end of the path beyond the limit
// would not be reached. (A finger made by hand can lie off the path in y, Thumbs_Up's little finger by -0.14: movedY() below
// holds what the sliders write.)
constexpr float CLOSED_SCALE     = 1.25f;
constexpr float CLOSED_BEND_X[3] = {1.40f, 1.75f, 1.40f};
constexpr float PINKY_PIP_BEND_X = 2.00f;

// Sideways angle of the knuckle away from the curl path at spread +1 / -1 (left hand z). The skeleton's fingers
// are fanned at rest (index 25 degrees toward +x, ring 19 and little finger 44 toward -x; avatar_skeleton.xml), a
// positive z turns a finger toward -x: fanning moves the index toward smaller z and the others toward larger z.
// Squeezing together is smaller. The little finger moves most, the middle finger least.
struct SpreadModel
{
    float open;
    float closed;
};
constexpr SpreadModel SPREAD[FINGER_COUNT] = {
    {-0.45f, 0.20f},   // index
    {0.12f, -0.09f},   // middle (it fans toward the ring finger)
    {0.40f, -0.18f},   // ring
    {0.70f, -0.28f},   // little finger
};

int fingerIndex(Finger f)
{
    const int i = static_cast<int>(f);
    return (i < 0 || i >= FINGER_COUNT) ? 0 : i;
}

float& axis(Raw& r, int a) { return a == 0 ? r.x : (a == 1 ? r.y : r.z); }
float axisOf(const Raw& r, int a) { return a == 0 ? r.x : (a == 1 ? r.y : r.z); }

/// The joint as it is in the left hand's frame (the right hand's is mirrored; mirroring twice gives the joint back).
Raw leftFrame(const Raw& r, bool rightHand) { return rightHand ? mirrorRaw(r) : r; }

/// A path through the joints of a digit: their numbers at the knots, straight between them. A finger has two
/// segments (open hand at curl 0, Tight_Grip at KNOT, the full fist at 1), the thumb's paths one (open, end).
struct Path
{
    int   joints       = 0;  // how many joints (the ones of the digit starting at firstJoint)
    int   firstJoint   = 0;
    int   segments     = 1;  // 1: pose[0] at curl 0, pose[1] at 1. 2: pose[0] at 0, pose[1] at knot, pose[2] at 1.
    float knot         = 1.f;
    Raw   pose[3][3];        // [knot][joint]
    bool  skipKnuckleZ = false;  // the knuckle's z is the spread, not part of the curl
};

Path fingerPath(Finger f)
{
    const int i = fingerIndex(f);
    Path      p;
    p.joints       = 3;
    p.segments     = 2;
    p.knot         = 1.f / CLOSED_SCALE;  // Tight_Grip
    p.skipKnuckleZ = true;
    for (int k = 0; k < 3; ++k)
    {
        p.pose[0][k] = FINGER_OPEN[i][k];
        p.pose[1][k] = FINGER_TIGHT[i][k];
        p.pose[2][k] = fingerClosedJoint(f, k);
    }
    return p;
}

/// The directions of Thumb1's motions (left frame): wrap, swing, arch, from the targets below minus the open thumb with what lies along
/// the earlier directions taken out (so these three are orthogonal), and the away direction of the swing's negative side (not orthogonal).
struct Thumb1Basis
{
    Raw   dir[THUMB1_AXES];
    float len2[THUMB1_AXES];
    Raw   away;    // the direction of the swing's negative side (from the open thumb, at swing -1)
    Raw   normal;  // wrap x arch: the plane of the two directions that the swing's two sides lie across
};

Raw cross(const Raw& a, const Raw& b) { return Raw{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot3(const Raw& a, const Raw& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// The turn about z (rad, negative) of Thumb1 at swing -1: the thumb at THUMB_AWAY_ANGLE_DEG from the index finger of the flat hand.
float thumbAwayZ()
{
    const float deg  = 3.14159265f / 180.f;
    const float thumb = std::atan2(THUMB_BONE_X, THUMB_BONE_Y);  // from the hand's axis y toward +x
    const float phi   = FINGER_OPEN[0][0].z;                      // the flat hand turns the index finger by z (toward -x)
    const float index = std::atan2(INDEX_BONE_X * std::cos(phi) - INDEX_BONE_Y * std::sin(phi), INDEX_BONE_X * std::sin(phi) + INDEX_BONE_Y * std::cos(phi));
    return -(THUMB_AWAY_ANGLE_DEG * deg + index - thumb);
}

Thumb1Basis thumb1Basis()
{
    const Raw   targets[THUMB1_AXES] = {THUMB_OPPOSED, THUMB_OKAY, THUMB_THUMBSUP};
    Thumb1Basis b;
    for (int i = 0; i < THUMB1_AXES; ++i)
    {
        Raw d{targets[i].x - THUMB_OPEN[0].x, targets[i].y - THUMB_OPEN[0].y, targets[i].z - THUMB_OPEN[0].z};
        for (int j = 0; j < i; ++j)
        {
            float dot = 0.f;
            for (int ax = 0; ax < 3; ++ax)
                dot += axisOf(d, ax) * axisOf(b.dir[j], ax);
            const float k = dot / b.len2[j];
            for (int ax = 0; ax < 3; ++ax)
                axis(d, ax) -= k * axisOf(b.dir[j], ax);
        }
        b.dir[i]  = d;
        b.len2[i] = d.x * d.x + d.y * d.y + d.z * d.z;
    }
    b.away   = Raw{0.f, 0.f, thumbAwayZ()};
    b.normal = cross(b.dir[0], b.dir[THUMB1_AXES - 1]);
    return b;
}

/// Thumb1 (left frame) for the coordinates c: the open thumb plus wrap, plus the swing (positive: along the swing direction, negative:
/// along the away direction, by its size), plus arch.
Raw thumb1Pose(const float c[THUMB1_AXES])
{
    const Thumb1Basis b = thumb1Basis();
    Raw               v = THUMB_OPEN[0];
    for (int i = 0; i < THUMB1_AXES; ++i)
    {
        const Raw& dir = (i == THUMB1_SWING && c[i] < 0.f) ? b.away : b.dir[i];
        const float u  = (i == THUMB1_SWING && c[i] < 0.f) ? -c[i] : c[i];
        v.x += u * dir.x;
        v.y += u * dir.y;
        v.z += u * dir.z;
    }
    return v;
}

/// Thumb1 as coordinates (left frame): the wrap, the swing and the arch. A non-finite joint counts as the open thumb. The swing's two
/// sides are two sets of axes joined at 0: the pose is on the positive side when it lies on the swing direction's side of the plane of
/// wrap and arch, else on the negative side (the away direction lies across that plane: checked for the numbers here). Each side has
/// its own solution (the positive one orthogonal projections, the negative one the three directions by triple products).
void thumb1Coords(const HandRaw& hand, bool rightHand, float c[THUMB1_AXES])
{
    const Thumb1Basis b = thumb1Basis();
    Raw               v = leftFrame(hand.joint[0][0], rightHand);
    if (!std::isfinite(v.x + v.y + v.z))
        v = THUMB_OPEN[0];
    const Raw rel{v.x - THUMB_OPEN[0].x, v.y - THUMB_OPEN[0].y, v.z - THUMB_OPEN[0].z};
    if (dot3(rel, b.normal) * dot3(b.dir[THUMB1_SWING], b.normal) >= 0.f)
    {
        for (int i = 0; i < THUMB1_AXES; ++i)
            c[i] = dot3(rel, b.dir[i]) / b.len2[i];
        if (!std::isfinite(c[0] + c[1] + c[2]))  // an enormous Thumb1 overflows the products: the open thumb instead
            c[0] = c[1] = c[2] = 0.f;
        return;
    }
    // the negative side: rel = a V + t A + x X (V = dir[0], A = away, X = dir[2])
    const Raw& V = b.dir[0];
    const Raw& X = b.dir[THUMB1_AXES - 1];
    const Raw& A = b.away;
    c[0]         = dot3(rel, cross(A, X)) / dot3(V, cross(A, X));
    c[1]         = -dot3(rel, cross(X, V)) / dot3(A, cross(X, V));
    c[2]         = dot3(rel, cross(V, A)) / dot3(X, cross(V, A));
    // an enormous Thumb1 overflows the products, and an away angle at the end of its usable range makes a denominator 0: no number
    // that is not finite leaves here (the open thumb instead)
    if (!std::isfinite(c[0] + c[1] + c[2]))
        c[0] = c[1] = c[2] = 0.f;
}

/// The end of the bend for one corner: the measured pose m carried `reach` times as far from the open thumb, z held to limitZ.
Raw thumbBentEndOf(int k, const Raw& m, float reach, float limitZ)
{
    const Raw   o  = THUMB_OPEN[k + 1];
    const float k2 = reach;
    Raw         d{(m.x - o.x) * k2, (m.y - o.y) * k2, (m.z - o.z) * k2};
    if (d.z > limitZ)
    {
        const float scale = limitZ / d.z;
        d                 = Raw{d.x * scale, d.y * scale, d.z * scale};
    }
    return Raw{o.x + d.x, o.y + d.y, o.z + d.z};
}

/// The end of the bend (curl 1) of Thumb2 (k = 0) or Thumb3 (k = 1) with Thumb1 at the coordinates c (wrap, swing, arch): the
/// plane through the corner ends, e = e0 + sum of u[i] (e[i] - e0), with z held to the limits of the corners joined the same way (a
/// sum of corners can pass the limit of any one of them). The swing has a corner on each side: a negative swing goes toward the
/// "swing away" corner by its size (a plane in two pieces, joined at 0).
Raw thumbBentEnd(int k, const float c[THUMB1_AXES])
{
    const Raw   o  = THUMB_OPEN[k + 1];
    const Raw   e0 = thumbBentEndOf(k, THUMB_BENT_MEASURED[k], THUMB_BENT_REACH[0], THUMB_BEND_LIMIT_Z[0][k]);
    Raw         d{e0.x - o.x, e0.y - o.y, e0.z - o.z};
    float       limitZ = THUMB_BEND_LIMIT_Z[0][k];
    for (int i = 0; i < THUMB1_AXES; ++i)
    {
        int   corner = i;  // the index into the measured table (the corner of the positive side)
        float u      = c[i];
        if (i == THUMB1_SWING && u < 0.f)
        {
            corner = THUMB1_AXES;  // the swing away: the extra corner at the end of the table
            u      = -u;
        }
        u             = clampf(u, 0.f, 1.f);
        const Raw   e = thumbBentEndOf(k, THUMB_BENT_MEASURED_AXIS[corner][k], THUMB_BENT_REACH[1 + corner], THUMB_BEND_LIMIT_Z[1 + corner][k]);
        d.x += u * (e.x - e0.x);
        d.y += u * (e.y - e0.y);
        d.z += u * (e.z - e0.z);
        limitZ += u * (THUMB_BEND_LIMIT_Z[1 + corner][k] - THUMB_BEND_LIMIT_Z[0][k]);
    }
    if (d.z > limitZ)
    {
        const float scale = limitZ / d.z;
        d                 = Raw{d.x * scale, d.y * scale, d.z * scale};
    }
    else if (d.z < THUMB_BEND_MIN_Z)
    {
        // too shallow: lengthened in proportion when it still bends the right way (the other numbers follow), else only z is raised
        if (d.z > 0.5f * THUMB_BEND_MIN_Z)
        {
            const float scale = THUMB_BEND_MIN_Z / d.z;
            d                 = Raw{d.x * scale, d.y * scale, d.z * scale};
        }
        else
            d.z = THUMB_BEND_MIN_Z;
    }
    return Raw{o.x + d.x, o.y + d.y, o.z + d.z};
}

Path thumbBendPath(const float c[THUMB1_AXES])
{
    Path p;
    p.joints     = 2;
    p.firstJoint = 1;
    for (int k = 0; k < 2; ++k)
    {
        p.pose[0][k] = THUMB_OPEN[k + 1];
        p.pose[1][k] = thumbBentEnd(k, c);
    }
    return p;
}

bool counted(const Path& p, int k, int a) { return !(p.skipKnuckleZ && k == 0 && a == 2); }

/// The two knots (curl values and poses) of segment s.
void segmentKnots(const Path& p, int s, float* cA, float* cB, int* poseA, int* poseB)
{
    if (p.segments == 1)
    {
        *cA = 0.f; *cB = 1.f; *poseA = 0; *poseB = 1;
    }
    else if (s == 0)
    {
        *cA = 0.f; *cB = p.knot; *poseA = 0; *poseB = 1;
    }
    else
    {
        *cA = p.knot; *cB = 1.f; *poseA = 1; *poseB = 2;
    }
}

/// The joint k of the path at curl c (straight on past the ends: the first segment before 0 and up to the knot, the
/// last one after it).
Raw pathPoint(const Path& p, float c, int k)
{
    float cA, cB;
    int   a, b;
    segmentKnots(p, (p.segments == 2 && c > p.knot) ? 1 : 0, &cA, &cB, &a, &b);
    const float t = (c - cA) / (cB - cA);
    Raw         r;
    for (int ax = 0; ax < 3; ++ax)
        axis(r, ax) = axisOf(p.pose[a][k], ax) + t * (axisOf(p.pose[b][k], ax) - axisOf(p.pose[a][k], ax));
    return r;
}

/// The progress of a digit's joints (left frame) along the path as one number: the projection of (joints - open pose) on a
/// fixed direction u, from the open pose to the path's end (the knuckle's z of a finger left out). u is the same whatever
/// the curl, so the part of the joints that lies off the path adds the same amount to the progress at any curl, and a
/// move by the path's own steps changes the progress by exactly that step: set then get gives the same value, setting the
/// same value again moves nothing, and the value does not jump from one segment of the path to the next.
float progressOf(const Path& p, const Raw* v)
{
    const int last = p.segments == 2 ? 2 : 1;
    float     sum  = 0.f;
    for (int k = 0; k < p.joints; ++k)
        for (int ax = 0; ax < 3; ++ax)
            if (counted(p, k, ax))
                sum += (axisOf(p.pose[last][k], ax) - axisOf(p.pose[0][k], ax)) * (axisOf(v[k], ax) - axisOf(p.pose[0][k], ax));
    return sum;
}

/// The curl value (not limited to 0..1) of a progress: the path's progress is a straight line to the knot and another on
/// to the end (it rises along both: the numbers were checked for the four fingers and the thumb's paths).
float curlOfProgress(const Path& p, float s)
{
    const float sEnd = progressOf(p, p.pose[p.segments == 2 ? 2 : 1]);
    float       c    = 0.f;
    if (p.segments == 1)
    {
        c = sEnd > 0.f ? s / sEnd : 0.f;
    }
    else
    {
        const float sKnot = progressOf(p, p.pose[1]);
        if (sKnot > 0.f && sEnd > sKnot)
            c = s <= sKnot ? p.knot * s / sKnot : p.knot + (1.f - p.knot) * (s - sKnot) / (sEnd - sKnot);
    }
    return std::isfinite(c) ? c : 0.f;
}

/// Where the joints of a digit lie on the path (not limited to 0..1), and the largest miss of any one number. A
/// non-finite joint counts as the open end.
float fitPath(const HandRaw& hand, bool rightHand, int digit, const Path& p, float* residual)
{
    Raw v[3];
    for (int k = 0; k < p.joints; ++k)
        v[k] = leftFrame(hand.joint[digit][p.firstJoint + k], rightHand);
    const float c = curlOfProgress(p, progressOf(p, v));
    if (residual)
    {
        float worst = 0.f;
        for (int k = 0; k < p.joints; ++k)
        {
            const Raw& u = v[k];
            const Raw  w = pathPoint(p, c, k);
            for (int ax = 0; ax < 3; ++ax)
            {
                if (!counted(p, k, ax))
                    continue;
                const float miss = std::fabs(axisOf(u, ax) - axisOf(w, ax));
                worst            = std::max(worst, std::isfinite(miss) ? miss : 1000.f);
            }
        }
        *residual = worst;
    }
    return c;
}

/// The y of a joint moved by dy, held to JOINT_Y_LIMIT_RAD (handcurl.h; y is the same number on both hands): the move stops at
/// the limit, a y already past it is not pushed further (and not pulled in: a move of 0 changes nothing, and one back toward 0 is
/// taken in full). Every y the sliders write goes through here.
float movedY(float y, float dy)
{
    if (dy > 0.f)
        return std::min(y + dy, std::max(y, JOINT_Y_LIMIT_RAD));
    if (dy < 0.f)
        return std::max(y + dy, std::min(y, -JOINT_Y_LIMIT_RAD));
    return y;
}

/// Moves the joints of a digit along the path from curl c0 to c1 (all three numbers of each joint, the knuckle z of a
/// finger too): each joint goes by the difference of the path's points, so what lies off the path stays off it (but for a y
/// that would pass its limit: movedY).
void movePath(HandRaw& hand, bool rightHand, int digit, const Path& p, float c0, float c1)
{
    if (!std::isfinite(c0) || !std::isfinite(c1))
        return;
    for (int k = 0; k < p.joints; ++k)
    {
        Raw&      joint = hand.joint[digit][p.firstJoint + k];
        Raw       v     = leftFrame(joint, rightHand);
        const Raw a     = pathPoint(p, c0, k);
        const Raw b     = pathPoint(p, c1, k);
        v.x += b.x - a.x;
        v.y = movedY(v.y, b.y - a.y);
        v.z += b.z - a.z;
        joint = leftFrame(v, rightHand);
    }
}

float curlOfPath(const HandRaw& hand, bool rightHand, int digit, const Path& p, bool* custom)
{
    float       residual = 0.f;
    const float c        = fitPath(hand, rightHand, digit, p, &residual);
    if (custom)
        *custom = residual > CUSTOM_RESIDUAL_RAD || c < -CUSTOM_MARGIN || c > 1.f + CUSTOM_MARGIN;
    return clampf(c, 0.f, 1.f);
}

void setCurlOfPath(HandRaw& hand, bool rightHand, int digit, const Path& p, float c)
{
    movePath(hand, rightHand, digit, p, fitPath(hand, rightHand, digit, p, nullptr), clampf(c, 0.f, 1.f));
}

/// Moves a finger along its path from curl c0 to c1. Closing pulls the fingers together: the spread (the knuckle's
/// sideways angle away from the path) shrinks in proportion to what is left of the opening, 1 - curl, so the closed hand
/// is the shipped fist whatever the spread was (the skeleton's fingers are fanned at rest: without this a fist made
/// from it keeps the little finger 30 degrees out). Opening does not bring it back (the ratio is never above 1: a
/// spread that was closed away cannot be recovered, and a spread must not grow without a limit): the open hand
/// comes back as the flat hand, and the spread slider fans it again.
void moveFinger(HandRaw& hand, bool rightHand, Finger f, float c0, float c1)
{
    if (!std::isfinite(c0) || !std::isfinite(c1))
        return;
    const int  slot = slotOf(f);
    const Path p    = fingerPath(f);
    const float gap = leftFrame(hand.joint[slot][0], rightHand).z - pathPoint(p, c0, 0).z;
    movePath(hand, rightHand, slot, p, c0, c1);
    const float w0 = clampf(1.f - c0, 0.f, 1.f);
    const float w1 = clampf(1.f - c1, 0.f, 1.f);
    const float k  = w0 > 1e-6f ? std::min(1.f, w1 / w0) : 1.f;
    if (k < 1.f && std::isfinite(gap))
    {
        Raw& knuckle = hand.joint[slot][0];
        Raw  v       = leftFrame(knuckle, rightHand);
        v.z          = pathPoint(p, c1, 0).z + gap * k;
        knuckle      = leftFrame(v, rightHand);
    }
}

// Spread without the -1..1 limit: a hand-made finger may lie beyond it, and a whole-hand shift must not pull it in.
float spreadToAngleUnclamped(const SpreadModel& m, float s) { return s >= 0.f ? s * m.open : -s * m.closed; }

float angleToSpreadUnclamped(const SpreadModel& m, float angle)
{
    // open and closed have opposite signs; pick the side of the curl path the angle is on
    const bool onOpenSide = (m.open > 0.f) == (angle >= 0.f);
    return onOpenSide ? angle / m.open : -angle / m.closed;
}

/// The knuckle z (left frame) the path has for this finger as it is now.
float pathKnuckleZ(const HandRaw& hand, bool rightHand, Finger f)
{
    const Path p = fingerPath(f);
    return pathPoint(p, fitPath(hand, rightHand, slotOf(f), p, nullptr), 0).z;
}

float spreadUnclamped(const HandRaw& hand, bool rightHand, Finger f)
{
    const float z = leftFrame(hand.joint[slotOf(f)][0], rightHand).z;
    return angleToSpreadUnclamped(SPREAD[fingerIndex(f)], z - pathKnuckleZ(hand, rightHand, f));
}

void writeKnuckleSpread(HandRaw& hand, bool rightHand, Finger f, float spreadValue)
{
    Raw& knuckle = hand.joint[slotOf(f)][0];
    Raw  v       = leftFrame(knuckle, rightHand);
    v.z          = pathKnuckleZ(hand, rightHand, f) + spreadToAngleUnclamped(SPREAD[fingerIndex(f)], spreadValue);
    knuckle      = leftFrame(v, rightHand);
}

constexpr Finger ALL_FINGERS[FINGER_COUNT] = {Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky};

// The version numbers of the preset files of the named hands (see namedHandFileVersion in handcurl.h), in the order of NamedHand:
// Open_Hand, Peace, Rock, Gun. (Version 5 was shipped twice with other joints: all four in an early, unnatural form, and
// later Open_Hand and Gun with the thumb of the flat hand. A copy of either in a user's folder is replaced by version 6.)
constexpr int NAMED_HAND_FILE_VERSION[NAMED_HAND_COUNT] = {6, 6, 6, 6};

constexpr bool namedHandFileVersionsBelow(int limit)
{
    for (int version : NAMED_HAND_FILE_VERSION)
        if (version >= limit)
            return false;
    return true;
}
static_assert(namedHandFileVersionsBelow(USER_HAND_PRESET_FILE_VERSION), "a shipped preset must stay below the version of a user's file");
}  // namespace

float clampf(float v, float lo, float hi)
{
    if (!(v >= lo))  // also catches NaN
        return lo;
    return v > hi ? hi : v;
}

Raw mirrorRaw(const Raw& r)
{
    Raw o;
    o.x = -r.x;
    o.y = r.y;
    o.z = -r.z;
    return o;
}

HandRaw mirrorHand(const HandRaw& hand)
{
    HandRaw out;
    for (int d = 0; d < HAND_DIGITS; ++d)
        for (int k = 0; k < 3; ++k)
            out.joint[d][k] = mirrorRaw(hand.joint[d][k]);
    return out;
}

Raw fingerOpenJoint(Finger finger, int joint) { return FINGER_OPEN[fingerIndex(finger)][std::min(std::max(joint, 0), 2)]; }
Raw fingerTightGripJoint(Finger finger, int joint) { return FINGER_TIGHT[fingerIndex(finger)][std::min(std::max(joint, 0), 2)]; }

// The closed end (curl 1): the full fist (see CLOSED_BEND_X, CLOSED_SCALE).
Raw fingerClosedJoint(Finger finger, int joint)
{
    const int  k = std::min(std::max(joint, 0), 2);
    const Raw  o = FINGER_OPEN[fingerIndex(finger)][k];
    const Raw  t = FINGER_TIGHT[fingerIndex(finger)][k];
    const bool deeper = fingerIndex(finger) == static_cast<int>(Finger::Pinky) && k == 1;
    return Raw{-(deeper ? PINKY_PIP_BEND_X : CLOSED_BEND_X[k]), o.y + CLOSED_SCALE * (t.y - o.y), o.z + CLOSED_SCALE * (t.z - o.z)};
}

float getFingerCurl(const HandRaw& hand, bool rightHand, Finger finger, bool* custom)
{
    return curlOfPath(hand, rightHand, slotOf(finger), fingerPath(finger), custom);
}

void setFingerCurl(HandRaw& hand, bool rightHand, Finger finger, float c)
{
    moveFinger(hand, rightHand, finger, fitPath(hand, rightHand, slotOf(finger), fingerPath(finger), nullptr), clampf(c, 0.f, 1.f));
}

float spreadToAngle(Finger finger, float s) { return spreadToAngleUnclamped(SPREAD[fingerIndex(finger)], clampf(s, -1.f, 1.f)); }

float angleToSpread(Finger finger, float angle)
{
    return clampf(angleToSpreadUnclamped(SPREAD[fingerIndex(finger)], angle), -1.f, 1.f);
}

float getFingerSpread(const HandRaw& hand, bool rightHand, Finger finger, bool* custom)
{
    float s = spreadUnclamped(hand, rightHand, finger);
    if (!std::isfinite(s))
        s = 0.f;
    if (custom)
        *custom = s < -1.f - CUSTOM_MARGIN || s > 1.f + CUSTOM_MARGIN;
    return clampf(s, -1.f, 1.f);
}

void setFingerSpread(HandRaw& hand, bool rightHand, Finger finger, float s)
{
    writeKnuckleSpread(hand, rightHand, finger, clampf(s, -1.f, 1.f));
}

namespace
{
/// The wrap (axis 0), swing (axis 1) or arch (axis 2) of Thumb1, with the custom flag (a coordinate out of its range by more than the margin).
float thumb1Axis(const HandRaw& hand, bool rightHand, int axisIndex, bool* custom)
{
    float c[THUMB1_AXES];
    thumb1Coords(hand, rightHand, c);
    if (custom)
        *custom = c[axisIndex] < THUMB1_RANGE_LOW[axisIndex] - CUSTOM_MARGIN || c[axisIndex] > 1.f + CUSTOM_MARGIN;
    return clampf(c[axisIndex], THUMB1_RANGE_LOW[axisIndex], 1.f);
}

/// Sets one coordinate of Thumb1 to value v (clamped to its range; the swing goes both ways); Thumb2 and 3 keep their bend: they go by the difference of the
/// bend path's points at the old and the new Thumb1 (a bend of 0 is the open thumb whatever Thumb1 does: nothing moves).
void setThumb1Axis(HandRaw& hand, bool rightHand, int axisIndex, float value)
{
    if (!std::isfinite(value))
        return;  // (clampf turns a NaN into the low end of the range: the swing would go to -1)
    float before[THUMB1_AXES];
    thumb1Coords(hand, rightHand, before);
    const float b  = getThumbBend(hand, rightHand);
    const Path  p0 = thumbBendPath(before);

    // the other two coordinates stay; the three directions span all of Thumb1, so the pose is made again from the coordinates
    float target[THUMB1_AXES] = {before[0], before[1], before[2]};
    target[axisIndex]         = clampf(value, THUMB1_RANGE_LOW[axisIndex], 1.f);
    Raw pose                  = thumb1Pose(target);
    if (!std::isfinite(pose.x + pose.y + pose.z))
        return;
    const float oldY = hand.joint[0][0].y;  // (a joint that is not finite was read as the open thumb: the pose is written as it is)
    if (std::isfinite(oldY))
        pose.y = movedY(oldY, pose.y - oldY);
    hand.joint[0][0] = leftFrame(pose, rightHand);

    float after[THUMB1_AXES];
    thumb1Coords(hand, rightHand, after);
    const Path p1 = thumbBendPath(after);
    for (int k = 0; k < 2; ++k)
    {
        Raw&      joint = hand.joint[0][1 + k];
        Raw       j     = leftFrame(joint, rightHand);
        const Raw a     = pathPoint(p0, b, k);
        const Raw c     = pathPoint(p1, b, k);
        j.x += c.x - a.x;
        j.y = movedY(j.y, c.y - a.y);
        j.z += c.z - a.z;
        joint = leftFrame(j, rightHand);
    }
}
}  // namespace

float getThumbWrap(const HandRaw& hand, bool rightHand, bool* custom) { return thumb1Axis(hand, rightHand, 0, custom); }

void setThumbWrap(HandRaw& hand, bool rightHand, float a) { setThumb1Axis(hand, rightHand, 0, a); }

float getThumbSwing(const HandRaw& hand, bool rightHand, bool* custom) { return thumb1Axis(hand, rightHand, 1, custom); }

void setThumbSwing(HandRaw& hand, bool rightHand, float w) { setThumb1Axis(hand, rightHand, 1, w); }

float getThumbArch(const HandRaw& hand, bool rightHand, bool* custom) { return thumb1Axis(hand, rightHand, 2, custom); }

void setThumbArch(HandRaw& hand, bool rightHand, float x) { setThumb1Axis(hand, rightHand, 2, x); }

float getThumbBend(const HandRaw& hand, bool rightHand, bool* custom)
{
    float c[THUMB1_AXES];
    thumb1Coords(hand, rightHand, c);
    return curlOfPath(hand, rightHand, 0, thumbBendPath(c), custom);
}

void setThumbBend(HandRaw& hand, bool rightHand, float b)
{
    float c[THUMB1_AXES];
    thumb1Coords(hand, rightHand, c);
    setCurlOfPath(hand, rightHand, 0, thumbBendPath(c), b);
}

float meanFingerCurl(const HandRaw& hand, bool rightHand)
{
    float sum = 0.f;
    for (Finger f : ALL_FINGERS)
        sum += getFingerCurl(hand, rightHand, f);
    return sum / FINGER_COUNT;
}

float meanFingerSpread(const HandRaw& hand, bool rightHand)
{
    float sum = 0.f;
    for (Finger f : ALL_FINGERS)
        sum += getFingerSpread(hand, rightHand, f);
    return sum / FINGER_COUNT;
}

// A finger moves along its path by delta, but not past 0 or 1 (the shown, clamped value is where it starts from);
// one that already lies past an end is not pushed further. Delta 0 changes nothing.
void shiftAllFingerCurl(HandRaw& hand, bool rightHand, float delta)
{
    for (Finger f : ALL_FINGERS)
    {
        const float c0 = getFingerCurl(hand, rightHand, f);  // where the finger is shown (0..1)
        moveFinger(hand, rightHand, f, c0, clampf(c0 + delta, 0.f, 1.f));
    }
}

void shiftAllFingerSpread(HandRaw& hand, bool rightHand, float delta)
{
    for (Finger f : ALL_FINGERS)
    {
        float s = spreadUnclamped(hand, rightHand, f);
        if (!std::isfinite(s))
            s = 0.f;
        writeKnuckleSpread(hand, rightHand, f, clampf(s + delta, std::min(-1.f, s), std::max(1.f, s)));
    }
}

const char* namedHandFileName(NamedHand which)
{
    switch (which)
    {
        case NamedHand::OpenHand: return "Open_Hand";
        case NamedHand::Peace: return "Peace";
        case NamedHand::Rock: return "Rock";
        case NamedHand::Gun: return "Gun";
    }
    return "";
}

int namedHandFileVersion(NamedHand which)
{
    const int i = static_cast<int>(which);
    return (i < 0 || i >= NAMED_HAND_COUNT) ? 0 : NAMED_HAND_FILE_VERSION[i];
}

// Curl c and spread s of index, middle, ring, little finger; the thumb's four numbers of the easy tab: wrap o (toward the little
// finger), swing w (-1 away from the hand .. 1 toward the palm), arch x (bent back) and bend b.
// Spread only matters for the open fingers (a closed finger has no spread: the fist is the shipped fist).
// AFTER A CHANGE HERE (or of anything the four hands are made from): write the shipped files again and raise
// NAMED_HAND_FILE_VERSION of the hands that changed (handcurl_genpresets_test.cpp, or tools/hand_presets.py --write where a
// program built here cannot be run; that tool is a port of the path from here through this file, and reads the numbers of this
// file: a change of the calculation itself is made there too). The selftest "hand-presets" and predeliver compare the two.
HandRaw makeNamedHand(NamedHand which)
{
    struct Spec
    {
        float c[FINGER_COUNT];
        float s[FINGER_COUNT];
        float o;
        float w;
        float x;
        float b;
    };
    Spec spec{};
    switch (which)
    {
        case NamedHand::OpenHand:  // the fingers a little apart (the outer ones least: they lie at the edges of the hand), the thumb a little away from the hand
            spec = Spec{{0.f, 0.f, 0.f, 0.f}, {0.3f, 0.4f, 0.4f, 0.3f}, 0.f, -0.3f, 0.f, 0.f};
            break;
        case NamedHand::Peace:  // index and middle finger up in a V (about 26 degrees between them), ring and little finger shut
            spec = Spec{{0.f, 0.f, 1.f, 1.f}, {0.4f, 1.f, 0.f, 0.f}, 0.8f, 0.f, 0.f, 0.5f};
            break;
        case NamedHand::Rock:  // index and little finger up, middle and ring finger shut, the thumb across them
            // (index and little finger 15 to 20 degrees out from the flat hand)
            spec = Spec{{0.f, 1.f, 1.f, 0.f}, {0.5f, 0.f, 0.f, 0.45f}, 0.8f, 0.f, 0.f, 0.5f};
            break;
        case NamedHand::Gun:  // index finger straight, the others shut, the thumb stood up: away from the hand and bent back
            spec = Spec{{0.f, 1.f, 1.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 0.f, -0.5f, 0.6f, 0.f};
            break;
    }

    HandRaw h;
    for (int k = 0; k < 3; ++k)
    {
        h.joint[0][k] = THUMB_OPEN[k];
        for (Finger f : ALL_FINGERS)
            h.joint[slotOf(f)][k] = FINGER_OPEN[fingerIndex(f)][k];
    }
    for (Finger f : ALL_FINGERS)
    {
        const int i = fingerIndex(f);
        setFingerCurl(h, false, f, spec.c[i]);
        setFingerSpread(h, false, f, spec.s[i]);
    }
    setThumbWrap(h, false, spec.o);
    setThumbSwing(h, false, spec.w);
    setThumbArch(h, false, spec.x);
    setThumbBend(h, false, spec.b);
    return h;
}

}  // namespace handcurl
}  // namespace plus
