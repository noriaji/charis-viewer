// Stand-alone test of handcurl.h/.cpp (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX handcurl_test.cpp handcurl.cpp && handcurl_test.exe
#include "handcurl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace plus::handcurl;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

static const Finger FINGERS[] = {Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky};

/// A left hand with every finger on its open (curl 0) or closed (curl 1) pose, the thumb open.
static HandRaw makeLeftHand(bool closed)
{
    HandRaw h;
    for (Finger f : FINGERS)
        for (int k = 0; k < 3; ++k)
            h.joint[slotOf(f)][k] = closed ? fingerClosedJoint(f, k) : fingerOpenJoint(f, k);
    return h;
}

int main()
{
    // ---- the shipped presets: the curl direction ----
    // Left hand raw x of joints 1/2/3 of the fingers of presets that close them (Tight_Grip, Grip, Thumbs_Up, Horns,
    // Two_Finger_Salute): every bend that is more than a little is negative (the curl side) on the left hand.
    {
        const float bends[][3] = {
            {-0.63f, -1.20f, -1.27f}, {-0.86f, -1.39f, -1.11f}, {-0.95f, -1.33f, -0.94f}, {-0.81f, -1.75f, -0.97f},  // Tight_Grip
            {-0.18f, -0.71f, -0.52f}, {-0.50f, -0.66f, -0.81f}, {-0.52f, -0.73f, -0.68f}, {-0.58f, -0.48f, -0.21f},  // Grip
            {-1.17f, -1.64f, -0.27f}, {-1.19f, -1.58f, -0.39f}, {-1.15f, -1.48f, -0.52f}, {-1.23f, -1.75f, -0.01f},  // Thumbs_Up
            {-0.35f, -1.68f, -0.89f}, {-0.29f, -1.62f, -1.08f}, {-0.99f, -1.96f, -0.24f}, {-0.75f, -2.02f, -0.56f},  // Horns, Two_Finger_Salute
        };
        for (const auto& b : bends)
            for (int k = 0; k < 3; ++k)
                CHECK(b[k] < 0.f);
        // the closed pose of every finger is on the same side, joint by joint, and at least as deep as Tight_Grip
        // (which is not a full fist) but within what a finger can do
        // the bends of the full fist: knuckle 1.40, middle joint 1.75 (the little finger's 2.00), tip joint 1.40
        const float fistBends[3] = {1.40f, 1.75f, 1.40f};
        for (Finger f : FINGERS)
            for (int k = 0; k < 3; ++k)
            {
                const float want = (f == Finger::Pinky && k == 1) ? 2.00f : fistBends[k];
                CHECK(near(fingerClosedJoint(f, k).x, -want));
                CHECK(fingerClosedJoint(f, k).x < fingerOpenJoint(f, k).x);
                CHECK(fingerClosedJoint(f, k).x <= fingerTightGripJoint(f, k).x + 1e-5f);
                // the turning in y and z is Tight_Grip's carried on 1.25 times as far; a y near pi/2 would be read back wrapped
                CHECK(std::fabs(fingerClosedJoint(f, k).y) < 1.45f);
                CHECK(std::fabs(fingerClosedJoint(f, k).y) < JOINT_Y_LIMIT_RAD);  // the path's end is inside the limit of what is written
            }
        static_assert(JOINT_Y_LIMIT_RAD < 1.5707f, "below pi/2, where the poser's read-back of a joint wraps");
    }

    for (int side = 0; side < 2; ++side)
    {
        const bool right = side == 1;

        // ---- Tight_Grip reads as a curl of about 0.7 to 0.8 (and not custom); the open and the closed hand as 0 and 1 ----
        {
            HandRaw tight;
            for (Finger f : FINGERS)
                for (int k = 0; k < 3; ++k)
                    tight.joint[slotOf(f)][k] = right ? mirrorRaw(fingerTightGripJoint(f, k)) : fingerTightGripJoint(f, k);
            for (Finger f : FINGERS)
            {
                bool        custom = true;
                const float c      = getFingerCurl(tight, right, f, &custom);
                CHECK(near(c, 0.8, 1e-3));  // the middle knot of the path
                CHECK(!custom);
                CHECK(near(getFingerSpread(tight, right, f), 0.0, 1e-3));
                // and opening it again gives the flat hand back (Flat.xml), all three numbers of every joint
                setFingerCurl(tight, right, f, 0.f);
                for (int k = 0; k < 3; ++k)
                {
                    const Raw want = right ? mirrorRaw(fingerOpenJoint(f, k)) : fingerOpenJoint(f, k);
                    const Raw got  = tight.joint[slotOf(f)][k];
                    CHECK(near(got.x, want.x, 1e-3) && near(got.y, want.y, 1e-3) && near(got.z, want.z, 1e-3));
                }
            }
        }

        // ---- the open and the closed hand read as curl 0 and 1, not custom, spread 0 ----
        HandRaw open   = makeLeftHand(false);
        HandRaw closed = makeLeftHand(true);
        if (right)
        {
            open   = mirrorHand(open);
            closed = mirrorHand(closed);
        }
        for (Finger f : FINGERS)
        {
            bool custom = true;
            CHECK(near(getFingerCurl(open, right, f, &custom), 0.0) && !custom);
            CHECK(near(getFingerSpread(open, right, f), 0.0));
            custom = true;
            CHECK(near(getFingerCurl(closed, right, f, &custom), 1.0) && !custom);
            CHECK(near(getFingerSpread(closed, right, f), 0.0));
        }
        CHECK(near(meanFingerCurl(open, right), 0.0) && near(meanFingerCurl(closed, right), 1.0));

        // ---- set / get round trips; every joint moves in x, y and z along the line; the right hand is the mirror ----
        for (Finger f : FINGERS)
        {
            for (int i = 0; i <= 10; ++i)
            {
                const float c = i / 10.f;
                HandRaw     h = open;
                setFingerCurl(h, right, f, c);
                bool custom = true;
                CHECK(near(getFingerCurl(h, right, f, &custom), c) && !custom);
                CHECK(near(getFingerSpread(h, right, f), 0.0));
                // the same on the left hand gives mirrored numbers
                HandRaw leftHand = makeLeftHand(false);
                setFingerCurl(leftHand, false, f, c);
                for (int k = 0; k < 3; ++k)
                {
                    const Raw l = leftHand.joint[slotOf(f)][k];
                    const Raw r = h.joint[slotOf(f)][k];
                    const Raw m = right ? mirrorRaw(l) : l;
                    CHECK(near(r.x, m.x) && near(r.y, m.y) && near(r.z, m.z));
                }
            }
            // curl 1 is the closed pose joint by joint (left hand numbers)
            HandRaw h = makeLeftHand(false);
            setFingerCurl(h, false, f, 1.f);
            for (int k = 0; k < 3; ++k)
            {
                const Raw want = fingerClosedJoint(f, k);
                const Raw got  = h.joint[slotOf(f)][k];
                CHECK(near(got.x, want.x) && near(got.y, want.y) && near(got.z, want.z));
            }
            // out of range and NaN
            h = open;
            setFingerCurl(h, right, f, 5.f);
            CHECK(near(getFingerCurl(h, right, f), 1.0));
            setFingerCurl(h, right, f, -3.f);
            CHECK(near(getFingerCurl(h, right, f), 0.0));
            setFingerCurl(h, right, f, std::nanf(""));
            CHECK(near(getFingerCurl(h, right, f), 0.0));
        }

        // ---- spread: round trip at any curl; curl and spread do not disturb each other ----
        for (Finger f : FINGERS)
        {
            for (int i = -10; i <= 10; ++i)
            {
                const float s = i / 10.f;
                CHECK(near(angleToSpread(f, spreadToAngle(f, s)), s));
                for (int j = 0; j <= 4; ++j)
                {
                    const float c = j / 4.f;
                    HandRaw     h = open;
                    setFingerCurl(h, right, f, c);
                    setFingerSpread(h, right, f, s);
                    CHECK(near(getFingerSpread(h, right, f), s));
                    CHECK(near(getFingerCurl(h, right, f), c));
                    setFingerCurl(h, right, f, 1.f - c);
                    // closing pulls the fingers together: the spread goes with what is left of the opening (1 - curl);
                    // opening does not make it grow (the ratio is never above 1)
                    const float k = (c < 0.999f) ? std::min(1.f, c / (1.f - c)) : 1.f;
                    CHECK(near(getFingerSpread(h, right, f), s * k));
                }
            }
            // squeezing moves a finger by at most 0.3 rad
            CHECK(std::fabs(spreadToAngle(f, -1.f)) <= 0.3f);
        }
        // fan direction: the index toward smaller z, the others toward larger z (left hand numbers)
        CHECK(spreadToAngle(Finger::Index, 1.f) < 0.f);
        CHECK(spreadToAngle(Finger::Middle, 1.f) > 0.f);
        CHECK(spreadToAngle(Finger::Ring, 1.f) > 0.f);
        CHECK(spreadToAngle(Finger::Pinky, 1.f) > 0.f);
        {
            HandRaw h = makeLeftHand(false);
            setFingerSpread(h, false, Finger::Pinky, 1.f);
            CHECK(near(h.joint[slotOf(Finger::Pinky)][0].z, fingerOpenJoint(Finger::Pinky, 0).z + 0.7));
            // the knuckle's x and y are not spread's
            CHECK(near(h.joint[slotOf(Finger::Pinky)][0].x, fingerOpenJoint(Finger::Pinky, 0).x));
            CHECK(near(h.joint[slotOf(Finger::Pinky)][0].y, fingerOpenJoint(Finger::Pinky, 0).y));
        }

        // a joint written as numbers of this hand from numbers of the left hand, and back
        auto toSide = [right](const Raw& l) { return right ? mirrorRaw(l) : l; };

        // ---- a hand that starts from the skeleton's rest pose (all zero: the fingers fanned out) closes to the fist ----
        {
            HandRaw h;
            for (Finger f : FINGERS)
            {
                CHECK(getFingerSpread(h, right, f) != 0.f);  // fanned (spread shown), not the flat hand
                setFingerCurl(h, right, f, 1.f);
                const Raw knuckle = toSide(h.joint[slotOf(f)][0]);
                CHECK(near(knuckle.z, fingerClosedJoint(f, 0).z));  // the fingers have come together: the shipped fist
                CHECK(std::fabs(getFingerSpread(h, right, f)) < 0.1f);  // (the rest pose is a little off the path: the fit is about 1)
            }
        }

        // ---- opening never makes the spread (the knuckle's z away from the path) grow ----
        {
            // a hand loaded from elsewhere with a big sideways angle at curl 0.85 (Thumbs_Up has -0.3 to -0.6 on the left):
            // opening it keeps the angle, it does not blow up
            for (Finger f : FINGERS)
            {
                HandRaw h = open;
                setFingerCurl(h, right, f, 0.85f);
                Raw knuckle = toSide(h.joint[slotOf(f)][0]);
                knuckle.z -= 0.6f;
                h.joint[slotOf(f)][0] = toSide(knuckle);
                setFingerCurl(h, right, f, 0.f);
                CHECK(near(toSide(h.joint[slotOf(f)][0]).z, fingerOpenJoint(f, 0).z - 0.6));
            }
            // closed to 0.8 / 0.9 / 0.99 with the fan out (spread 1) and opened: the fan is what it was (0.7 rad for the little finger)
            for (double c : {0.8, 0.9, 0.99})
            {
                HandRaw h = open;
                setFingerCurl(h, right, Finger::Pinky, (float)c);
                setFingerSpread(h, right, Finger::Pinky, 1.f);
                setFingerCurl(h, right, Finger::Pinky, 0.f);
                CHECK(near(toSide(h.joint[slotOf(Finger::Pinky)][0]).z, fingerOpenJoint(Finger::Pinky, 0).z + 0.70));
            }
            // dragging the whole-hand bar from the rest pose to nearly closed and back: all four fingers end flat
            HandRaw h;
            shiftAllFingerCurl(h, right, 0.99f);
            shiftAllFingerCurl(h, right, -0.99f);
            for (Finger f : FINGERS)
                CHECK(std::fabs(toSide(h.joint[slotOf(f)][0]).z - fingerOpenJoint(f, 0).z) < 0.05f);
        }

        // ---- a finger off the path (Thumbs_Up has the index finger 0.2 to 0.5 rad off): set -> get agree at every curl,
        //      across the knot (0.8) too, setting the same value again moves nothing, and a round trip comes back ----
        {
            for (Finger f : FINGERS)
            {
                HandRaw h = open;
                setFingerCurl(h, right, f, 0.69f);
                // push the joints off the path: an offset in x, y and z of the three joints
                for (int k = 0; k < 3; ++k)
                {
                    Raw l = toSide(h.joint[slotOf(f)][k]);
                    l.x += 0.25f - 0.1f * k;
                    l.y += 0.30f - 0.2f * k;
                    if (k > 0)
                        l.z += 0.2f;
                    h.joint[slotOf(f)][k] = toSide(l);
                }
                const HandRaw off = h;
                for (int i = 75; i <= 90; ++i)
                {
                    const float c = i / 100.f;
                    setFingerCurl(h, right, f, c);
                    CHECK(near(getFingerCurl(h, right, f), c));
                    const HandRaw once = h;
                    setFingerCurl(h, right, f, c);  // the same value again: nothing moves
                    bool same = true;
                    for (int k = 0; k < 3; ++k)
                        same = same && near(h.joint[slotOf(f)][k].x, once.joint[slotOf(f)][k].x, 1e-5) &&
                               near(h.joint[slotOf(f)][k].y, once.joint[slotOf(f)][k].y, 1e-5) && near(h.joint[slotOf(f)][k].z, once.joint[slotOf(f)][k].z, 1e-5);
                    CHECK(same);
                }
                // small steps through the knot move the joints by small steps (no jumps)
                HandRaw g = off;
                setFingerCurl(g, right, f, 0.75f);
                for (int i = 76; i <= 85; ++i)
                {
                    const HandRaw before = g;
                    setFingerCurl(g, right, f, i / 100.f);
                    for (int k = 0; k < 3; ++k)
                        CHECK(std::fabs(g.joint[slotOf(f)][k].x - before.joint[slotOf(f)][k].x) < 0.06f);
                }
                // 0.69 -> 1 -> 0.69 comes back to the joints it started from (the spread aside: closing shrinks it)
                HandRaw r = off;
                const float c0 = getFingerCurl(r, right, f);
                setFingerCurl(r, right, f, 1.f);
                setFingerCurl(r, right, f, c0);
                for (int k = 0; k < 3; ++k)
                {
                    CHECK(near(r.joint[slotOf(f)][k].x, off.joint[slotOf(f)][k].x, 1e-3));
                    CHECK(near(r.joint[slotOf(f)][k].y, off.joint[slotOf(f)][k].y, 1e-3));
                }
                // the whole-hand bar: 0.2 up and down comes back
                HandRaw w = off;
                shiftAllFingerCurl(w, right, 0.15f);
                shiftAllFingerCurl(w, right, -0.15f);
                for (int k = 0; k < 3; ++k)
                    CHECK(near(w.joint[slotOf(f)][k].x, off.joint[slotOf(f)][k].x, 1e-3));
            }
        }

        // ---- no slider writes a y past JOINT_Y_LIMIT_RAD (the poser reads a joint back with y within +-pi/2: a y written past
        //      that comes back as another triple and the curl value jumps) ----
        {
            const float limit = JOINT_Y_LIMIT_RAD + 1e-5f;
            // Thumbs_Up.xml's little finger (left hand numbers): its middle joint lies -0.14 off the path in y, and the path
            // ends at y -1.38: closing it all the way would take y to -1.52. With the joint moved a little more by hand
            // (the adjust tab), further. The y stops at the limit, the curl still reads (nearly) what was set, and the steps
            // of a drag stay small.
            const Raw thumbsUpPinky[3] = {{-1.228712f, -0.345575f, -1.044734f}, {-1.747793f, -1.130973f, -0.749054f}, {-0.013141f, -0.219912f, 0.269397f}};
            const int pinky            = slotOf(Finger::Pinky);
            for (float extraY : {0.f, 0.1f, -0.1f})
            {
                HandRaw start = open;
                for (int k = 0; k < 3; ++k)
                    start.joint[pinky][k] = toSide(thumbsUpPinky[k]);
                Raw middle = toSide(start.joint[pinky][1]);
                middle.y += extraY;
                start.joint[pinky][1] = toSide(middle);

                HandRaw h = start;
                setFingerCurl(h, right, Finger::Pinky, 1.f);
                for (int k = 0; k < 3; ++k)
                    CHECK(std::fabs(h.joint[pinky][k].y) <= limit);
                CHECK(getFingerCurl(h, right, Finger::Pinky) > 0.97f);  // (the part of y that was cut off is missing from the fit)
                // the same value again moves nothing
                const HandRaw once = h;
                setFingerCurl(h, right, Finger::Pinky, getFingerCurl(h, right, Finger::Pinky));
                for (int k = 0; k < 3; ++k)
                    CHECK(near(h.joint[pinky][k].x, once.joint[pinky][k].x, 1e-5) && near(h.joint[pinky][k].y, once.joint[pinky][k].y, 1e-5));
                // and opening again takes the joints back along the path
                setFingerCurl(h, right, Finger::Pinky, 0.5f);
                CHECK(near(getFingerCurl(h, right, Finger::Pinky), 0.5, 1e-3));

                // a drag in the slider's steps from 0.90 to 1.00: every reading within 0.03 of the slider, no step of a joint
                // above 0.06 rad, never past the limit (before the limit, the read-back wrapped here and the finger thrashed)
                HandRaw g = start;
                setFingerCurl(g, right, Finger::Pinky, 0.9f);
                for (int i = 91; i <= 100; ++i)
                {
                    const HandRaw before = g;
                    setFingerCurl(g, right, Finger::Pinky, i / 100.f);
                    CHECK(std::fabs(getFingerCurl(g, right, Finger::Pinky) - i / 100.f) < 0.03f);
                    for (int k = 0; k < 3; ++k)
                    {
                        CHECK(std::fabs(g.joint[pinky][k].y) <= limit);
                        CHECK(std::fabs(g.joint[pinky][k].x - before.joint[pinky][k].x) < 0.06f);
                        CHECK(std::fabs(g.joint[pinky][k].y - before.joint[pinky][k].y) < 0.06f);
                    }
                }
                // the whole-hand bar goes through the same limit
                HandRaw w = start;
                shiftAllFingerCurl(w, right, 1.f);
                for (int k = 0; k < 3; ++k)
                    CHECK(std::fabs(w.joint[pinky][k].y) <= limit);
            }
            // every finger, every joint, pushed off the path in y either way: closing and opening never takes the y past the
            // limit; a y that already lies past it (1.7 off) is not pushed further out, and a move of nothing leaves it there
            for (Finger f : FINGERS)
                for (int k = 0; k < 3; ++k)
                    for (float off : {0.5f, -0.5f, 1.2f, -1.2f, 1.7f, -1.7f})
                    {
                        HandRaw h = open;
                        setFingerCurl(h, right, f, 0.5f);
                        h.joint[slotOf(f)][k].y += off;  // (y is the same number on both hands)
                        const float startY = h.joint[slotOf(f)][k].y;
                        const float bound  = std::max(limit, std::fabs(startY) + 1e-5f);
                        const HandRaw same = h;
                        shiftAllFingerCurl(h, right, 0.f);
                        CHECK(h.joint[slotOf(f)][k].y == same.joint[slotOf(f)][k].y);
                        for (float c : {1.f, 0.f, 1.f, 0.3f})
                        {
                            setFingerCurl(h, right, f, c);
                            for (int j = 0; j < 3; ++j)
                                CHECK(std::fabs(h.joint[slotOf(f)][j].y) <= (j == k ? bound : limit));
                        }
                    }
            // the thumb: Two_Finger_Salute.xml's Thumb1..3 (Thumb2 has y 1.23), every slider to its ends in turn
            const Raw salute[3] = {{-0.157696f, 1.071283f, 0.138055f}, {-1.320701f, 1.225221f, 1.018399f}, {-0.597929f, -0.251327f, -0.085347f}};
            for (float extraY : {0.f, 0.25f})
            {
                HandRaw h = open;
                for (int k = 0; k < 3; ++k)
                    h.joint[0][k] = toSide(salute[k]);
                h.joint[0][1].y += extraY;
                for (float bend : {0.f, 1.f})
                    for (float wrap : {0.f, 1.f})
                        for (float swing : {-1.f, 1.f})
                            for (float arch : {0.f, 1.f})
                            {
                                setThumbBend(h, right, bend);
                                setThumbWrap(h, right, wrap);
                                setThumbSwing(h, right, swing);
                                setThumbArch(h, right, arch);
                                setThumbBend(h, right, 1.f - bend);
                                for (int k = 0; k < 3; ++k)
                                    CHECK(std::fabs(h.joint[0][k].y) <= limit);
                            }
            }
        }

        // ---- the spread of a finger made by hand past the slider's range is custom (the slider shows the end of its range) ----
        {
            for (Finger f : FINGERS)
            {
                HandRaw h = open;
                bool    custom = true;
                CHECK(near(getFingerSpread(h, right, f, &custom), 0.0) && !custom);
                for (float s : {-1.f, 1.f})
                {
                    setFingerSpread(h, right, f, s);
                    custom = true;
                    CHECK(near(getFingerSpread(h, right, f, &custom), s) && !custom);
                    // 1.1 times the range is inside the margin, 1.3 times is custom
                    for (float times : {1.1f, 1.3f})
                    {
                        HandRaw past    = open;
                        Raw     knuckle = toSide(past.joint[slotOf(f)][0]);
                        knuckle.z += times * spreadToAngle(f, s);
                        past.joint[slotOf(f)][0] = toSide(knuckle);
                        custom                   = times < 1.2f;
                        CHECK(near(getFingerSpread(past, right, f, &custom), s));
                        CHECK(custom == (times > 1.2f));
                    }
                }
            }
            // Thumbs_Up.xml's middle finger (knuckle z -0.20, the path has +0.08 there: 3.1 times the range of the squeeze)
            HandRaw up = open;
            setFingerCurl(up, right, Finger::Middle, 0.75f);
            Raw knuckle = toSide(up.joint[slotOf(Finger::Middle)][0]);
            knuckle.z -= 0.28f;
            up.joint[slotOf(Finger::Middle)][0] = toSide(knuckle);
            bool custom = false;
            CHECK(near(getFingerSpread(up, right, Finger::Middle, &custom), -1.0) && custom);
            // a knuckle that is not finite reads as 0 and is not custom
            HandRaw bad;
            bad.joint[slotOf(Finger::Ring)][0].z = std::nanf("");
            custom = true;
            CHECK(near(getFingerSpread(bad, right, Finger::Ring, &custom), 0.0) && !custom);
        }

        // ---- a finger made joint by joint: custom, kept off the line, moved along it ----
        {
            HandRaw h = open;
            setFingerCurl(h, right, Finger::Ring, 0.5f);
            Raw tip = toSide(h.joint[slotOf(Finger::Ring)][2]);  // the tip joint, in left-hand numbers
            tip.x += 0.8f;                                       // bent back, far off the line
            h.joint[slotOf(Finger::Ring)][2] = toSide(tip);
            bool custom = false;
            getFingerCurl(h, right, Finger::Ring, &custom);
            CHECK(custom);
            // the slider moves the finger along its line: the curl is what was set, the finger stays off the line
            setFingerCurl(h, right, Finger::Ring, 0.3f);
            custom = false;
            CHECK(near(getFingerCurl(h, right, Finger::Ring, &custom), 0.3));
            CHECK(custom);
        }
        // a bent-back finger (all joints the other way) is custom
        {
            HandRaw h = open;
            setFingerCurl(h, right, Finger::Index, -0.8f);  // clamped to 0: still on the line
            bool custom = true;
            getFingerCurl(h, right, Finger::Index, &custom);
            CHECK(!custom);
            for (int k = 0; k < 3; ++k)
            {
                Raw l = toSide(h.joint[slotOf(Finger::Index)][k]);
                l.x   = -l.x + 0.4f;
                h.joint[slotOf(Finger::Index)][k] = toSide(l);
            }
            getFingerCurl(h, right, Finger::Index, &custom);
            CHECK(custom);
        }

        // ---- the whole-hand bars: relative, clamped, delta 0 changes nothing ----
        {
            HandRaw h = open;
            setFingerCurl(h, right, Finger::Index, 0.2f);
            setFingerCurl(h, right, Finger::Middle, 0.5f);
            setFingerCurl(h, right, Finger::Ring, 0.5f);
            setFingerCurl(h, right, Finger::Pinky, 0.9f);
            const HandRaw before = h;
            shiftAllFingerCurl(h, right, 0.f);
            bool same = true;
            for (int d = 0; d < HAND_DIGITS; ++d)
                for (int k = 0; k < 3; ++k)
                    same = same && near(h.joint[d][k].x, before.joint[d][k].x) && near(h.joint[d][k].y, before.joint[d][k].y) &&
                           near(h.joint[d][k].z, before.joint[d][k].z);
            CHECK(same);
            shiftAllFingerCurl(h, right, 0.2f);
            CHECK(near(getFingerCurl(h, right, Finger::Index), 0.4) && near(getFingerCurl(h, right, Finger::Middle), 0.7));
            CHECK(near(getFingerCurl(h, right, Finger::Pinky), 1.0));
            CHECK(near(meanFingerCurl(h, right), (0.4 + 0.7 + 0.7 + 1.0) / 4));
            shiftAllFingerCurl(h, right, -1.f);  // everything to 0 (clamped), each by its own amount
            CHECK(near(meanFingerCurl(h, right), 0.0));
            // a finger already past the closed pose is not pushed further
            HandRaw past = closed;
            for (int k = 0; k < 3; ++k)
            {
                Raw l = toSide(past.joint[slotOf(Finger::Index)][k]);
                const Raw o = fingerOpenJoint(Finger::Index, k), c = fingerClosedJoint(Finger::Index, k);
                l.x += 0.3f * (c.x - o.x);
                l.y += 0.3f * (c.y - o.y);
                l.z += 0.3f * (c.z - o.z);
                past.joint[slotOf(Finger::Index)][k] = toSide(l);
            }
            const HandRaw pastBefore = past;
            shiftAllFingerCurl(past, right, 0.1f);
            CHECK(near(past.joint[slotOf(Finger::Index)][1].x, pastBefore.joint[slotOf(Finger::Index)][1].x));
            // spread: relative shift, and a spread past the range is not pulled in by delta 0
            HandRaw s = open;
            for (Finger f : FINGERS)
                setFingerSpread(s, right, f, 0.5f);
            shiftAllFingerSpread(s, right, -1.f);
            for (Finger f : FINGERS)
                CHECK(near(getFingerSpread(s, right, f), -0.5));
            Raw k0 = toSide(s.joint[slotOf(Finger::Pinky)][0]);
            k0.z   = fingerOpenJoint(Finger::Pinky, 0).z + 1.4f * 0.70f;  // spread 1.4
            s.joint[slotOf(Finger::Pinky)][0] = toSide(k0);
            shiftAllFingerSpread(s, right, 0.f);
            CHECK(near(toSide(s.joint[slotOf(Finger::Pinky)][0]).z, fingerOpenJoint(Finger::Pinky, 0).z + 1.4 * 0.70));
            CHECK(near(getFingerSpread(s, right, Finger::Pinky), 1.0));
        }

        // ---- the thumb: opposition on Thumb1, bend on Thumb2 and 3; the right hand is the mirror ----
        {
            HandRaw h;
            setThumbWrap(h, right, 0.5f);
            setThumbBend(h, right, 0.4f);
            bool custom = true;
            CHECK(near(getThumbWrap(h, right, &custom), 0.5) && !custom);
            CHECK(near(getThumbBend(h, right, &custom), 0.4) && !custom);
            // Thumb1 y is the same on both hands (x and z mirror)
            const Raw t1 = h.joint[0][0];
            const Raw t2 = h.joint[0][1];
            CHECK(t1.y > 0.f);
            // moving one does not move the other
            setThumbBend(h, right, 1.f);
            CHECK(near(getThumbWrap(h, right), 0.5));
            setThumbWrap(h, right, 0.f);
            CHECK(near(getThumbBend(h, right), 1.0));
            // the bend is the measured one (left hand: Thumb2 z 0.451, Thumb3 z 1.043, mostly z); the right hand mirrors
            // from the open hand (Flat's thumb): the end of the bend is exactly the measured Thumb2 and Thumb3, c = 0 is Flat's
            // (Flat.xml's Thumb1..3 written out: the hand Open_Hand of makeNamedHand has its thumb swung away from the hand
            // since the four-axis thumb, and what follows starts from wrap, swing and arch 0)
            HandRaw bent = makeLeftHand(false);
            bent.joint[0][0] = Raw{-0.013f, 0.f, -0.007f};
            bent.joint[0][1] = Raw{-0.039f, 0.f, 0.02f};
            bent.joint[0][2] = Raw{-0.033f, 0.f, 0.f};
            if (right)
                bent = mirrorHand(bent);
            {
                bool c1 = true, c2 = true, c3 = true;
                CHECK(near(getThumbWrap(bent, right, &c1), 0.0) && near(getThumbSwing(bent, right, &c2), 0.0) && near(getThumbArch(bent, right, &c3), 0.0));
                CHECK(!c1 && !c2 && !c3 && near(getThumbBend(bent, right), 0.0));
            }
            const HandRaw openThumb = bent;
            setThumbBend(bent, right, 1.f);
            const Raw b2 = toSide(bent.joint[0][1]);
            const Raw b3 = toSide(bent.joint[0][2]);
            // the end (curl 1): the measured Thumb2 and Thumb3 carried on to twice their distance from the open thumb
            CHECK(near(b2.x, -0.029, 1e-3) && near(b2.y, -0.360, 1e-3) && near(b2.z, 0.882, 1e-3));
            CHECK(near(b3.x, -0.413, 1e-3) && near(b3.y, 0.232, 1e-3) && near(b3.z, 2.086, 1e-3));
            // half the slider is the measured pose
            HandRaw measured = openThumb;
            setThumbBend(measured, right, 0.5f);
            CHECK(near(toSide(measured.joint[0][1]).x, -0.034, 1e-3) && near(toSide(measured.joint[0][1]).y, -0.180, 1e-3) &&
                  near(toSide(measured.joint[0][1]).z, 0.451, 1e-3));
            CHECK(near(toSide(measured.joint[0][2]).x, -0.223, 1e-3) && near(toSide(measured.joint[0][2]).y, 0.116, 1e-3) &&
                  near(toSide(measured.joint[0][2]).z, 1.043, 1e-3));
            CHECK(near(toSide(bent.joint[0][0]).y, toSide(openThumb.joint[0][0]).y));  // Thumb1 (the opposition) is not touched
            HandRaw half = openThumb;
            setThumbBend(half, right, 1.f);
            setThumbBend(half, right, 0.f);
            CHECK(near(toSide(half.joint[0][1]).z, toSide(openThumb.joint[0][1]).z, 1e-3));
            // the bend depends on the opposition (Thumb1 turns the axes of Thumb2 and 3): every pair of values reads back exactly,
            // in either order, and the opposition moving does not change the bend
            for (float t : {0.f, 0.3f, 0.5f, 1.f})
                for (float b : {0.f, 0.4f, 0.5f, 1.f})
                {
                    HandRaw p = openThumb;
                    setThumbWrap(p, right, t);
                    setThumbBend(p, right, b);
                    CHECK(near(getThumbWrap(p, right), t) && near(getThumbBend(p, right), b));
                    HandRaw q = openThumb;
                    setThumbBend(q, right, b);
                    setThumbWrap(q, right, t);  // the other order: the bend stays
                    CHECK(near(getThumbWrap(q, right), t) && near(getThumbBend(q, right), b));
                    // both orders give the same joints
                    for (int k = 0; k < 3; ++k)
                        CHECK(near(p.joint[0][k].x, q.joint[0][k].x, 1e-3) && near(p.joint[0][k].y, q.joint[0][k].y, 1e-3) &&
                              near(p.joint[0][k].z, q.joint[0][k].z, 1e-3));
                    bool customP = true;
                    getThumbBend(p, right, &customP);
                    CHECK(!customP);
                }
            // a bend of 0 is the open thumb whatever the opposition (Thumb2 and 3 are not touched by it)
            {
                HandRaw p = openThumb;
                setThumbWrap(p, right, 1.f);
                CHECK(near(p.joint[0][1].x, openThumb.joint[0][1].x) && near(p.joint[0][1].z, openThumb.joint[0][1].z) &&
                      near(p.joint[0][2].x, openThumb.joint[0][2].x) && near(p.joint[0][2].z, openThumb.joint[0][2].z));
                // opposition 0 -> 1 -> 0 with a bend of 0.5 comes back
                HandRaw r = openThumb;
                setThumbBend(r, right, 0.5f);
                const HandRaw before = r;
                setThumbWrap(r, right, 1.f);
                setThumbWrap(r, right, 0.f);
                for (int k = 0; k < 3; ++k)
                    CHECK(near(r.joint[0][k].x, before.joint[0][k].x, 1e-3) && near(r.joint[0][k].z, before.joint[0][k].z, 1e-3));
                // at the opposition 1 the end of the bend (curl 1) is the second measurement itself: the thumb lying over the palm
                HandRaw e = openThumb;
                setThumbWrap(e, right, 1.f);
                setThumbBend(e, right, 1.f);
                CHECK(near(toSide(e.joint[0][1]).x, 0.040, 2e-3) && near(toSide(e.joint[0][1]).y, -0.466, 2e-3) &&
                      near(toSide(e.joint[0][1]).z, 1.103, 2e-3));
                CHECK(near(toSide(e.joint[0][2]).x, -0.812, 2e-3) && near(toSide(e.joint[0][2]).y, -0.155, 2e-3) &&
                      near(toSide(e.joint[0][2]).z, 1.754, 2e-3));
                // the middle of the slider is half way there (and at the opposition 0 it is the first measurement)
                HandRaw m = openThumb;
                setThumbWrap(m, right, 1.f);
                setThumbBend(m, right, 0.5f);
                CHECK(near(toSide(m.joint[0][1]).z, 0.5 * (1.103 + 0.02), 3e-3) && near(toSide(m.joint[0][2]).z, 0.5 * 1.754, 3e-3));
                // between, the ends change straight with the opposition: at 0.5 the end is half way between the two ends
                HandRaw h5 = openThumb;
                setThumbWrap(h5, right, 0.5f);
                setThumbBend(h5, right, 1.f);
                CHECK(near(toSide(h5.joint[0][2]).z, 0.5 * (2.086 + 1.754), 5e-3));
            }
            // the thumb root's three motions: wrap (Flat -> Grip), swing (Okay across it) and arch (Thumbs_Up across both) are
            // independent, and with the bend all combinations read back exactly, in any order
            for (float a : {0.f, 0.4f, 1.f})
                for (float w : {-1.f, -0.5f, 0.f, 0.5f, 1.f})
                    for (float ab : {0.f, 0.6f, 1.f})
                        for (float b : {0.f, 0.5f, 1.f})
                        {
                            HandRaw x = openThumb;
                            setThumbWrap(x, right, a);
                            setThumbSwing(x, right, w);
                            setThumbArch(x, right, ab);
                            setThumbBend(x, right, b);
                            CHECK(near(getThumbWrap(x, right), a) && near(getThumbSwing(x, right), w) &&
                                  near(getThumbArch(x, right), ab) && near(getThumbBend(x, right), b));
                            HandRaw y = openThumb;
                            setThumbBend(y, right, b);
                            setThumbArch(y, right, ab);
                            setThumbSwing(y, right, w);
                            setThumbWrap(y, right, a);
                            CHECK(near(getThumbWrap(y, right), a) && near(getThumbSwing(y, right), w) &&
                                  near(getThumbArch(y, right), ab) && near(getThumbBend(y, right), b));
                            for (int k = 0; k < 3; ++k)
                                CHECK(near(x.joint[0][k].x, y.joint[0][k].x, 2e-3) && near(x.joint[0][k].y, y.joint[0][k].y, 2e-3) &&
                                      near(x.joint[0][k].z, y.joint[0][k].z, 2e-3));
                            bool customX = true;
                            getThumbBend(x, right, &customX);
                            CHECK(!customX);
                            customX = true;
                            getThumbArch(x, right, &customX);
                            CHECK(!customX);
                            // b = 0: Thumb2 and 3 stay the open thumb whatever Thumb1 does
                            if (b == 0.f)
                                CHECK(near(x.joint[0][2].z, openThumb.joint[0][2].z, 1e-3) && near(x.joint[0][1].z, openThumb.joint[0][1].z, 1e-3));
                        }
            // the swing both ways: -1 is the thumb away from the hand (a turn about z from the open thumb that takes it to about 60 degrees from the index finger), and it reads back
            // negative, not custom; -1.3 is held to the range, and one beyond it (-1.4) is custom; the bend end for a negative swing follows the "swing away" corner (a placeholder
            // equal to the origin's now, so it is the same end as at 0)
            {
                HandRaw away = openThumb;
                setThumbSwing(away, right, -1.f);
                const Raw tAway = toSide(away.joint[0][0]);
                CHECK(near(tAway.x, -0.013, 1e-3) && near(tAway.y, 0.0, 1e-3) && tAway.z < -0.3 && tAway.z > -0.8);  // a pure turn about z
                // the thumb bone (0.66, 0.75) turned about z by that much stands at about 60 degrees to the index finger of the flat hand
                {
                    const double t = -(tAway.z + 0.007);  // the turn from the open thumb
                    const double c = std::cos(t), sn = std::sin(t);
                    const double thumbAngle = std::atan2(0.66 * c + 0.75 * sn, 0.75 * c - 0.66 * sn);  // from the hand's axis y
                    const double phi = fingerOpenJoint(Finger::Index, 0).z;                              // the flat hand turns the index finger by z
                    const double indexAngle = std::atan2(0.017 * std::cos(phi) - 0.036 * std::sin(phi), 0.017 * std::sin(phi) + 0.036 * std::cos(phi));
                    const double between = (thumbAngle - indexAngle) * 180.0 / 3.14159265358979;
                    CHECK(between > 58.0 && between < 63.0);
                }
                bool cs = true;
                CHECK(near(getThumbSwing(away, right, &cs), -1.0) && !cs);
                // -1.3 set from the open thumb is held to the range: Thumb1 the same as at -1, and not custom
                HandRaw held = openThumb;
                setThumbSwing(held, right, -1.3f);
                CHECK(near(toSide(held.joint[0][0]).z, toSide(away.joint[0][0]).z, 1e-4) && near(getThumbSwing(held, right), -1.0));
                bool ch = true;
                getThumbSwing(held, right, &ch);
                CHECK(!ch);
                // a NaN changes nothing
                HandRaw nanSet = openThumb;
                setThumbSwing(nanSet, right, 0.5f);
                setThumbSwing(nanSet, right, std::nanf(""));
                setThumbWrap(nanSet, right, std::nanf(""));
                CHECK(near(getThumbSwing(nanSet, right), 0.5) && near(getThumbWrap(nanSet, right), 0.0));
                HandRaw beyond = openThumb;
                beyond.joint[0][0] = toSide(Raw{-0.013f, 0.f, -0.007f + 1.4f * (toSide(away.joint[0][0]).z + 0.007f)});
                bool cf = false;
                getThumbSwing(beyond, right, &cf);
                CHECK(cf);
                // the bend end at swing -1 is a corner of its own: with the placeholder it is the origin's end
                HandRaw be = openThumb;
                setThumbSwing(be, right, -1.f);
                setThumbBend(be, right, 1.f);
                CHECK(near(toSide(be.joint[0][2]).z, 2.086, 5e-3));
            }
            // Tight_Grip, Pointing and Relaxed have a negative swing (about -0.12, -0.40, -0.39: the thumb spread away): inside the range now,
            // none of their three Thumb1 coordinates is custom
            {
                const Raw t1s[3] = {{0.01f, 0.85f, -0.04f}, {0.27f, 0.88f, -0.03f}, {0.35f, 0.69f, 0.03f}};
                for (const Raw& t : t1s)
                {
                    HandRaw pre;
                    pre.joint[0][0] = toSide(t);
                    bool c1 = true, c2 = true, c3 = true;
                    const float sw = getThumbSwing(pre, right, &c2);
                    getThumbWrap(pre, right, &c1);
                    getThumbArch(pre, right, &c3);
                    CHECK(!c1 && !c2 && !c3);
                    CHECK(sw < 0.f);
                }
            }
            // an enormous or non-finite Thumb1 reads as the open thumb and moving it writes nothing that is not finite
            {
                HandRaw huge = openThumb;
                huge.joint[0][0] = toSide(Raw{3e38f, -3e38f, 3e38f});
                setThumbSwing(huge, right, -0.5f);
                const Raw hv = huge.joint[0][0];
                CHECK(std::isfinite(hv.x) && std::isfinite(hv.y) && std::isfinite(hv.z));
                CHECK(std::isfinite(getThumbWrap(huge, right)) && std::isfinite(getThumbSwing(huge, right)) && std::isfinite(getThumbArch(huge, right)));
            }
            // each motion alone leaves the other two at 0 (the directions are orthogonal)
            for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
            {
                HandRaw x = openThumb;
                if (axisIndex == 0)
                    setThumbWrap(x, right, 0.7f);
                else if (axisIndex == 1)
                    setThumbSwing(x, right, 0.7f);
                else
                    setThumbArch(x, right, 0.7f);
                const float v[3] = {getThumbWrap(x, right), getThumbSwing(x, right), getThumbArch(x, right)};
                for (int i = 0; i < 3; ++i)
                    CHECK(near(v[i], i == axisIndex ? 0.7 : 0.0));
            }
            // the swing moves Thumb1 only (and with a bend of 0, nothing else); the wrap does not change the swing
            {
                HandRaw x = openThumb;
                setThumbSwing(x, right, 1.f);
                CHECK(near(getThumbWrap(x, right), 0.0) && near(toSide(x.joint[0][1]).z, toSide(openThumb.joint[0][1]).z, 1e-4));
                // Thumb1 at the swing 1 is Okay's Thumb1 with its wrap part taken out: (-0.158, 0.785, 0.296) minus 0.718 of (Grip - Flat)
                const Raw swung = toSide(x.joint[0][0]);
                CHECK(near(swung.x, -0.182, 1e-2) && near(swung.y, -0.005, 1e-2) && near(swung.z, 0.267, 1e-2));
            }
            // Okay.xml's Thumb1 reads as wrap 0.72 and swing 1, and is not custom; Grip's as wrap 1 and swing 0
            {
                HandRaw okay;
                okay.joint[0][0] = toSide(Raw{-0.158f, 0.785f, 0.296f});
                bool c1 = true, c2 = true;
                CHECK(near(getThumbWrap(okay, right, &c1), 0.72, 0.03) && near(getThumbSwing(okay, right, &c2), 1.0, 0.03));
                CHECK(!c1 && !c2);
                HandRaw grip;
                grip.joint[0][0] = toSide(Raw{0.02f, 1.1f, 0.033f});
                CHECK(near(getThumbWrap(grip, right), 1.0, 0.01) && near(getThumbSwing(grip, right), 0.0, 0.02));
                // Thumbs_Up's Thumb1 (0.322, 0.1885, 0.1708) is the arch target: it reads as arch 1 (with a little wrap and
                // swing, 0.19 and -0.06; the arch reads 1.045 on the negative side), none of the three is custom
                HandRaw up;
                up.joint[0][0] = toSide(Raw{0.322f, 0.1885f, 0.1708f});
                bool c1u = true, c2u = true, c3u = true;
                CHECK(near(getThumbArch(up, right, &c3u), 1.0, 0.01));
                getThumbWrap(up, right, &c1u);
                getThumbSwing(up, right, &c2u);
                CHECK(!c1u && !c2u && !c3u);
            }
            // Two_Finger_Salute's Thumb1 (-0.158, 1.071, 0.138) is pulled across the palm past the arch's start (x -0.26): the arch
            // is custom; and a non-finite Thumb1 is read and moved as the open thumb (no NaN is written)
            {
                HandRaw tfs;
                tfs.joint[0][0] = toSide(Raw{-0.158f, 1.071f, 0.138f});
                bool ct = false;
                getThumbArch(tfs, right, &ct);
                CHECK(ct);
                HandRaw bad;
                bad.joint[0][0] = toSide(Raw{std::nanf(""), 0.f, 0.f});
                CHECK(near(getThumbWrap(bad, right), 0.0, 0.01));
                setThumbWrap(bad, right, 0.5f);
                const Raw movedThumb1 = bad.joint[0][0];
                CHECK(std::isfinite(movedThumb1.x) && std::isfinite(movedThumb1.y) && std::isfinite(movedThumb1.z));
                CHECK(near(getThumbWrap(bad, right), 0.5));
            }
            // the end of the bend is the plane through the corner ends: at (0, 1) the swing corner (the measurement of the swing at its
            // maximum, taken as the bend 0.5: the end is twice as far), at (0.5, 0.5) half way from (0, 0) toward each of the others
            {
                HandRaw x = openThumb;
                setThumbSwing(x, right, 1.f);
                setThumbBend(x, right, 1.f);
                CHECK(near(toSide(x.joint[0][1]).x, 0.0394, 2e-3) && near(toSide(x.joint[0][1]).z, 1.272, 2e-3));
                CHECK(near(toSide(x.joint[0][2]).x, 0.0282, 2e-3) && near(toSide(x.joint[0][2]).z, 1.228, 2e-3));
                HandRaw sm = openThumb;
                setThumbSwing(sm, right, 1.f);
                setThumbBend(sm, right, 0.5f);  // the measurement itself
                CHECK(near(toSide(sm.joint[0][1]).z, 0.646, 2e-3) && near(toSide(sm.joint[0][2]).z, 0.614, 2e-3));
                HandRaw hh = openThumb;
                setThumbWrap(hh, right, 0.5f);
                setThumbSwing(hh, right, 0.5f);
                setThumbBend(hh, right, 1.f);
                // Thumb3 z: e00 2.086, e10 1.754, e01 1.228: 2.086 + 0.5 (1.754 - 2.086) + 0.5 (1.228 - 2.086)
                CHECK(near(toSide(hh.joint[0][2]).z, 2.086 + 0.5 * (1.754 - 2.086) + 0.5 * (1.228 - 2.086), 5e-3));
                CHECK(near(getThumbWrap(hh, right), 0.5) && near(getThumbSwing(hh, right), 0.5) && near(getThumbBend(hh, right), 1.0));
                // both at 1: the plane's corner e10 + e01 - e00 (Thumb3 z 0.896: shallower than either, no limit binds)
                HandRaw c11 = openThumb;
                setThumbWrap(c11, right, 1.f);
                setThumbSwing(c11, right, 1.f);
                setThumbBend(c11, right, 1.f);
                CHECK(near(toSide(c11.joint[0][2]).z, 1.754 + 1.228 - 2.086, 5e-3));
            }
            // the wrap moving does not change the swing, and the right hand is the mirror
            {
                HandRaw x = openThumb;
                setThumbSwing(x, right, 0.6f);
                setThumbWrap(x, right, 0.3f);
                CHECK(near(getThumbSwing(x, right), 0.6) && near(getThumbWrap(x, right), 0.3));
                setThumbWrap(x, right, 0.9f);
                CHECK(near(getThumbSwing(x, right), 0.6));
            }
            // a joint far off the line is custom
            Raw bad = toSide(h.joint[0][1]);
            bad.y += 3.f;
            h.joint[0][1] = toSide(bad);
            getThumbBend(h, right, &custom);
            CHECK(custom);
            // NaN is read as the open end
            HandRaw nanHand;
            nanHand.joint[0][1].x = std::nanf("");
            CHECK(near(getThumbBend(nanHand, right), 0.0));
        }
    }

    // ---- the hand presets made from the model: closed fingers are the full fist, open ones straight ----
    for (int n = 0; n < NAMED_HAND_COUNT; ++n)
    {
        const NamedHand which = static_cast<NamedHand>(n);
        const HandRaw   left  = makeNamedHand(which);
        const HandRaw   right = mirrorHand(left);
        CHECK(namedHandFileName(which)[0] != '\0');
        for (Finger f : FINGERS)
        {
            const float c = getFingerCurl(left, false, f);
            CHECK(near(getFingerCurl(right, true, f), c));
            CHECK(c < 0.001f || c > 0.999f);  // every finger of these hands is either straight or shut
            for (int k = 0; k < 3; ++k)
            {
                const Raw l = left.joint[slotOf(f)][k];
                const Raw r = right.joint[slotOf(f)][k];
                CHECK(near(r.x, -l.x) && near(r.y, l.y) && near(r.z, -l.z));
                if (c > 0.999f)
                {
                    // a shut finger bends the right way at every joint (negative x on the left) and turns the way the fist does
                    // (inward, as fingerClosedJoint: the outer fingers do not turn out); with its knuckle where the fist has it
                    const Raw want = fingerClosedJoint(f, k);
                    CHECK(l.x < -1.0f);
                    CHECK(near(l.x, want.x) && near(l.y, want.y) && near(l.z, want.z));
                }
                else
                {
                    CHECK(std::fabs(l.x) < 0.2f);  // a straight finger is straight (Flat.xml has the little finger knuckle at +0.16): no bend either way
                }
            }
        }
    }
    {
        const HandRaw peace = makeNamedHand(NamedHand::Peace);
        const HandRaw rock  = makeNamedHand(NamedHand::Rock);
        const HandRaw gun   = makeNamedHand(NamedHand::Gun);
        const HandRaw open  = makeNamedHand(NamedHand::OpenHand);
        // Peace: index and middle finger up and apart (a V), ring and little finger shut
        CHECK(getFingerCurl(peace, false, Finger::Index) < 0.001f && getFingerCurl(peace, false, Finger::Middle) < 0.001f);
        CHECK(getFingerCurl(peace, false, Finger::Ring) > 0.999f && getFingerCurl(peace, false, Finger::Pinky) > 0.999f);
        CHECK(near(getFingerSpread(peace, false, Finger::Index), 0.4) && near(getFingerSpread(peace, false, Finger::Middle), 1.0));
        // Rock: index and little finger up, middle and ring finger shut
        CHECK(getFingerCurl(rock, false, Finger::Index) < 0.001f && getFingerCurl(rock, false, Finger::Pinky) < 0.001f);
        CHECK(getFingerCurl(rock, false, Finger::Middle) > 0.999f && getFingerCurl(rock, false, Finger::Ring) > 0.999f);
        // Gun: only the index finger straight; the thumb stood up (away from the hand and bent back: swing -0.5, arch 0.6, no wrap or bend)
        CHECK(getFingerCurl(gun, false, Finger::Index) < 0.001f);
        for (Finger f : {Finger::Middle, Finger::Ring, Finger::Pinky})
            CHECK(getFingerCurl(gun, false, f) > 0.999f);
        CHECK(near(getThumbSwing(gun, false), -0.5) && near(getThumbArch(gun, false), 0.6));
        CHECK(near(getThumbWrap(gun, false), 0.0) && near(getThumbBend(gun, false), 0.0));
        // Open_Hand: the thumb a little away from the hand (swing -0.3) and straight
        CHECK(near(getThumbSwing(open, false), -0.3) && near(getThumbWrap(open, false), 0.0) && near(getThumbArch(open, false), 0.0) && near(getThumbBend(open, false), 0.0));
        // The numbers of Thumb1 the two files must hold (Open_Hand.xml and Gun.xml were once shipped with the flat hand's
        // thumb, -0.013 0 -0.007: the files are compared with makeNamedHand by handcurl_genpresets_test and
        // tools/hand_presets.py; these two lines tie that tool's port of the model to this one). They move with
        // THUMB_AWAY_ANGLE_DEG and the two hands' slider values.
        CHECK(near(open.joint[0][0].x, -0.0130, 5e-4) && near(open.joint[0][0].y, 0.0, 5e-4) && near(open.joint[0][0].z, -0.1620, 5e-4));
        CHECK(near(gun.joint[0][0].x, 0.1759, 5e-4) && near(gun.joint[0][0].y, -0.0099, 5e-4) && near(gun.joint[0][0].z, -0.1494, 5e-4));
        for (int n = 0; n < NAMED_HAND_COUNT; ++n)
            CHECK(namedHandFileVersion(static_cast<NamedHand>(n)) >= 6);  // above the 5 that files with other joints were shipped with
        // Peace and Rock: the thumb wrapped across the fingers, not swung or arched
        CHECK(near(getThumbSwing(peace, false), 0.0) && near(getThumbArch(peace, false), 0.0) && near(getThumbSwing(rock, false), 0.0) && near(getThumbArch(rock, false), 0.0));
        // the right hand reads the same four numbers (the mirror)
        for (const HandRaw* hh : {&peace, &rock, &gun, &open})
        {
            const HandRaw rr = mirrorHand(*hh);
            CHECK(near(getThumbWrap(rr, true), getThumbWrap(*hh, false)) && near(getThumbSwing(rr, true), getThumbSwing(*hh, false)));
            CHECK(near(getThumbArch(rr, true), getThumbArch(*hh, false)) && near(getThumbBend(rr, true), getThumbBend(*hh, false)));
        }
        // the thumb lies across in Peace and Rock
        CHECK(near(getThumbWrap(peace, false), 0.8) && near(getThumbBend(rock, false), 0.5));
        // Open_Hand: all four a little apart, not fanned out (the outer fingers least); none further than 0.25 rad from the flat hand
        for (Finger f : FINGERS)
        {
            const float s = getFingerSpread(open, false, f);
            CHECK(s > 0.2f && s < 0.5f && getFingerCurl(open, false, f) < 0.001f);
            CHECK(std::fabs(spreadToAngle(f, s)) < 0.25f);
        }
        // Rock: index and little finger 15 to 20 degrees (0.26 to 0.35 rad) out from the flat hand, not more; Peace: the V of 20 to 30 degrees
        CHECK(std::fabs(spreadToAngle(Finger::Index, getFingerSpread(rock, false, Finger::Index))) < 0.35f);
        CHECK(std::fabs(spreadToAngle(Finger::Pinky, getFingerSpread(rock, false, Finger::Pinky))) < 0.35f);
        {
            const float v = peace.joint[slotOf(Finger::Middle)][0].z - peace.joint[slotOf(Finger::Index)][0].z;  // z turns a finger toward -x
            const float flatGap = fingerOpenJoint(Finger::Middle, 0).z - fingerOpenJoint(Finger::Index, 0).z;
            // the angle between the fingers is (the skeleton's bone tilts) - (z difference); the V is the flat hand's plus the spread
            CHECK(v - flatGap > 0.2f && v - flatGap < 0.45f);
        }
    }

    // ---- mirror ----
    {
        const Raw l{0.01f, 0.85f, -0.04f};
        const Raw r = mirrorRaw(l);
        CHECK(near(r.x, -0.01) && near(r.y, 0.85) && near(r.z, 0.04));
        const Raw back = mirrorRaw(r);
        CHECK(near(back.x, l.x) && near(back.y, l.y) && near(back.z, l.z));
        HandRaw h = makeLeftHand(true);
        const HandRaw m = mirrorHand(h);
        for (Finger f : FINGERS)
        {
            CHECK(near(getFingerCurl(m, true, f), getFingerCurl(h, false, f)));
            CHECK(near(getFingerSpread(m, true, f), getFingerSpread(h, false, f)));
        }
    }

    CHECK(near(clampf(std::nanf(""), 0.f, 1.f), 0.0));
    std::printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
