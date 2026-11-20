// Stand-alone test of poseshareproto.h/.cpp (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX poseshare_test.cpp poseshareproto.cpp && poseshare_test.exe
#include "poseshareproto.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace plus::poseshare::proto;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static unsigned long long rngState = 88172645463325252ULL;
static float frand(float lo, float hi)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return lo + (hi - lo) * static_cast<float>((rngState >> 11) & 0xFFFFFF) / 16777215.f;
}

// the angle in degrees between two rotations
static float angleBetween(const float a[4], const float b[4])
{
    float dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    float la  = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    float lb  = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
    dot /= (la * lb);
    if (dot > 1.f)
        dot = 1.f;
    return 2.f * std::acos(dot) * 57.29578f;
}

static void setRotation(JointValue& j, float x, float y, float z, float w)
{
    j.present     = true;
    j.rotation[0] = x;
    j.rotation[1] = y;
    j.rotation[2] = z;
    j.rotation[3] = w;
}

static Pose fullPose()
{
    Pose p;
    for (std::size_t i = 0; i < JOINT_COUNT; ++i)
    {
        float q[4] = { frand(-1.f, 1.f), frand(-1.f, 1.f), frand(-1.f, 1.f), frand(-1.f, 1.f) };
        setRotation(p.joints[i], q[0], q[1], q[2], q[3]);
    }
    return p;
}

static std::vector<unsigned char> rawOf(const std::string& text)
{
    std::vector<unsigned char> bytes;
    CHECK(base64Decode(text, bytes));
    return bytes;
}

static std::string textOf(const std::vector<unsigned char>& bytes) { return base64Encode(bytes.data(), bytes.size()); }

static bool decodes(const std::vector<unsigned char>& bytes)
{
    Pose p;
    return decodePose(textOf(bytes), p);
}

int main()
{
    // ---- the table
    {
        std::set<std::string> names;
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
        {
            CHECK(JOINT_NAMES[i] != nullptr && JOINT_NAMES[i][0] == 'm');
            names.insert(JOINT_NAMES[i]);
        }
        CHECK(names.size() == JOINT_COUNT);
        CHECK(std::string(JOINT_NAMES[0]) == "mHead");
        CHECK(std::string(JOINT_NAMES[JOINT_COUNT - 1]) == "mSpine4");
        static_assert(MASK_BYTES == 17, "130 joints are 17 mask bytes");
    }

    // ---- base64
    {
        auto enc = [](const std::string& s) { return base64Encode(reinterpret_cast<const unsigned char*>(s.data()), s.size()); };
        CHECK(enc("Man") == "TWFu");
        CHECK(enc("Ma") == "TWE=");
        CHECK(enc("M") == "TQ==");
        for (std::size_t n = 1; n < 40; ++n)
        {
            std::vector<unsigned char> data(n);
            for (auto& b : data)
                b = static_cast<unsigned char>(frand(0.f, 255.9f));
            std::vector<unsigned char> back;
            CHECK(base64Decode(textOf(data), back) && back == data);
        }
        std::vector<unsigned char> sink;
        CHECK(!base64Decode("", sink));
        CHECK(!base64Decode("TWE", sink));       // not a multiple of 4
        CHECK(!base64Decode("T===", sink));      // one letter is no byte
        CHECK(!base64Decode("TW=E", sink));      // a letter after the pad
        CHECK(!base64Decode("TQ=A", sink));
        CHECK(!base64Decode("TW==TWFu", sink));  // a pad in the middle
        CHECK(!base64Decode("T*Fu", sink));
        CHECK(!base64Decode("TWF ", sink));
        CHECK(!base64Decode("====", sink));
        CHECK(!base64Decode("TQ==TQ==", sink));
        CHECK(base64Decode("TWFu", sink) && sink.size() == 3);
    }

    // ---- rotations
    {
        float worst = 0.f;
        for (int n = 0; n < 2000; ++n)
        {
            Pose p;
            float q[4] = { frand(-1.f, 1.f), frand(-1.f, 1.f), frand(-1.f, 1.f), frand(-1.f, 1.f) };
            setRotation(p.joints[n % JOINT_COUNT], q[0], q[1], q[2], q[3]);
            std::string text;
            CHECK(encodePose(p, text));
            Pose back;
            CHECK(decodePose(text, back));
            const JointValue& j = back.joints[n % JOINT_COUNT];
            CHECK(j.present);
            const float err = angleBetween(q, j.rotation);
            if (err > worst)
                worst = err;
        }
        std::printf("worst rotation error %.3f degrees\n", worst);
        CHECK(worst < 0.25f); // about 0.1 degree as the design says, the corner cases up to a bit more

        // the corners: the identity, half turns about each axis, the same rotation as q and -q, a rotation not of unit length
        const float corners[][4] = { { 0, 0, 0, 1 },  { 0, 0, 0, -1 }, { 1, 0, 0, 0 }, { 0, 1, 0, 0 },
                                     { 0, 0, 1, 0 },  { -1, 0, 0, 0 }, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0, 0, 0, 5 } };
        for (const auto& c : corners)
        {
            Pose p;
            setRotation(p.joints[3], c[0], c[1], c[2], c[3]);
            std::string text;
            Pose        back;
            CHECK(encodePose(p, text) && decodePose(text, back) && angleBetween(c, back.joints[3].rotation) < 0.25f);
        }

        // not finite or no length: the identity
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const float bad[][4] = { { nan, 0, 0, 1 }, { 0, inf, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, -inf, nan } };
        const float identity[4] = { 0, 0, 0, 1 };
        for (const auto& c : bad)
        {
            Pose p;
            setRotation(p.joints[0], c[0], c[1], c[2], c[3]);
            std::string text;
            Pose        back;
            CHECK(encodePose(p, text) && decodePose(text, back) && angleBetween(identity, back.joints[0].rotation) < 0.25f);
        }
    }

    // ---- sizes: a whole pose and the most extras
    {
        Pose p = fullPose();
        std::string text;
        std::size_t dropped = 99;
        CHECK(encodePose(p, text, &dropped));
        CHECK(dropped == 0);
        CHECK(text.size() == 724);  // 3 + 17 + 130 * 4 + 1 bytes
        CHECK(text.size() <= PAYLOAD_MAX);
        Pose back;
        CHECK(decodePose(text, back));
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
            CHECK(back.joints[i].present && !back.joints[i].hasPosition && !back.joints[i].hasScale);

        // every joint with a position and a scale: far more than fits; the later ones are left out, the text is in the limit
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
        {
            JointValue& j = p.joints[i];
            j.hasPosition = j.hasScale = true;
            j.position[0] = 0.1f * static_cast<float>(i % 7) + 0.05f;
            j.position[1] = -0.2f;
            j.position[2] = 0.3f;
            j.scale[0]    = 0.5f;
            j.scale[1]    = -0.5f;
            j.scale[2]    = 2.f;
        }
        CHECK(encodePose(p, text, &dropped));
        CHECK(text.size() <= PAYLOAD_MAX);
        CHECK(dropped > 100); // 134 bytes of room: nine joints with both
        CHECK(decodePose(text, back));
        std::size_t withExtras = 0;
        bool        gap        = false;
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
        {
            const JointValue& j = back.joints[i];
            CHECK(j.present);
            if (j.hasPosition && j.hasScale)
            {
                ++withExtras;
                CHECK(!gap); // the first joints keep theirs, the later ones lose them
                CHECK(std::fabs(j.position[1] + 0.2f) < 0.001f && std::fabs(j.position[2] - 0.3f) < 0.001f);
                CHECK(std::fabs(j.scale[0] - 0.5f) < 0.001f && std::fabs(j.scale[1] + 0.5f) < 0.001f && std::fabs(j.scale[2] - 2.f) < 0.001f);
            }
            else
                gap = true;
        }
        CHECK(withExtras + dropped == JOINT_COUNT);
        CHECK(withExtras == 9);

        // positions only: the room is for more joints
        Pose q = fullPose();
        for (std::size_t i = 0; i < JOINT_COUNT; ++i)
        {
            q.joints[i].hasPosition = true;
            q.joints[i].position[0] = 0.25f;
        }
        CHECK(encodePose(q, text, &dropped) && text.size() <= PAYLOAD_MAX && dropped > 0);
        CHECK(decodePose(text, back));

        // no joint at all
        Pose none;
        CHECK(!encodePose(none, text) && text.empty());
    }

    // ---- positions and scales: the values, "no change" is not sent, not finite is not sent
    {
        Pose p;
        setRotation(p.joints[5], 0, 0, 0, 1);
        p.joints[5].hasPosition = true;
        p.joints[5].position[0] = -9.5f;
        p.joints[5].position[1] = 0.0123f;
        p.joints[5].position[2] = 12.f; // over the limit: held at it
        p.joints[5].hasScale    = true;
        p.joints[5].scale[0]    = 0.f;
        p.joints[5].scale[1]    = 0.01f;
        p.joints[5].scale[2]    = 40.f; // over: held
        setRotation(p.joints[6], 0, 0, 0, 1);
        p.joints[6].hasPosition = true; // zero: not sent
        p.joints[6].hasScale    = true; // zero change: not sent
        setRotation(p.joints[7], 0, 0, 0, 1);
        p.joints[7].hasPosition = true;
        p.joints[7].position[1] = std::numeric_limits<float>::quiet_NaN(); // not sent
        std::string text;
        Pose        back;
        CHECK(encodePose(p, text) && decodePose(text, back));
        const JointValue& j = back.joints[5];
        CHECK(j.hasPosition && j.hasScale);
        CHECK(std::fabs(j.position[0] + 9.5f) < 0.001f && std::fabs(j.position[1] - 0.0123f) < 0.001f && std::fabs(j.position[2] - 10.f) < 0.001f);
        CHECK(std::fabs(j.scale[0]) < 0.001f && std::fabs(j.scale[1] - 0.01f) < 0.001f && std::fabs(j.scale[2] - 19.f) < 0.001f);
        CHECK(!back.joints[6].hasPosition && !back.joints[6].hasScale);
        CHECK(!back.joints[7].hasPosition);
        CHECK(!back.joints[4].present);
    }

    // ---- what the reading refuses: each cut of a good text
    {
        Pose p = fullPose();
        p.joints[10].hasPosition = true;
        p.joints[10].position[0] = 0.5f;
        p.joints[20].hasScale = true;
        p.joints[20].scale[0] = 1.f;
        std::string text;
        CHECK(encodePose(p, text));
        const std::vector<unsigned char> good = rawOf(text);
        CHECK(decodes(good));
        Pose out;
        CHECK(decodePose(text, out));

        for (std::size_t cut = 0; cut < good.size(); ++cut) // every shorter text
        {
            std::vector<unsigned char> shorter(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
            if (!shorter.empty())
                CHECK(!decodes(shorter));
        }
        std::vector<unsigned char> longer = good; // a byte left over
        longer.push_back(0);
        CHECK(!decodes(longer));

        auto changed = [&](std::size_t at, unsigned char value)
        {
            std::vector<unsigned char> b = good;
            b[at] = value;
            return b;
        };
        CHECK(!decodes(changed(0, 2)));   // version
        CHECK(!decodes(changed(0, 0)));
        CHECK(!decodes(changed(1, 0)));   // kind
        CHECK(!decodes(changed(1, 2)));
        CHECK(!decodes(changed(2, 129))); // the count is not the mask's
        CHECK(!decodes(changed(2, 0)));
        CHECK(!decodes(changed(2, 131)));
        CHECK(!decodes(changed(2, 255)));
        // a mask bit beyond the table (bit 130 is bit 2 of the last mask byte), the count raised to match
        {
            std::vector<unsigned char> b = good;
            b[HEADER_BYTES + MASK_BYTES - 1] = static_cast<unsigned char>(b[HEADER_BYTES + MASK_BYTES - 1] | 0x04);
            CHECK(!decodes(b));
        }
        // a joint missing from the mask, the count lowered to match, the rotation block cut: the size is wrong
        {
            std::vector<unsigned char> b = good;
            b[HEADER_BYTES] = static_cast<unsigned char>(b[HEADER_BYTES] & 0xFE);
            b[2]            = 129;
            CHECK(!decodes(b));
        }

        const std::size_t extrasAt = HEADER_BYTES + MASK_BYTES + JOINT_COUNT * ROTATION_BYTES;
        CHECK(good[extrasAt] == 2);
        CHECK(good[extrasAt + 1] == 10 && good[extrasAt + 2] == 1);
        CHECK(!decodes(changed(extrasAt, 3)));        // more extras than the bytes hold
        CHECK(!decodes(changed(extrasAt, 131)));
        CHECK(!decodes(changed(extrasAt + 2, 0)));    // flags 0
        CHECK(!decodes(changed(extrasAt + 2, 4)));    // flags 4
        CHECK(!decodes(changed(extrasAt + 1, 130)));  // a joint beyond the table
        CHECK(!decodes(changed(extrasAt + 1, 255)));
        CHECK(!decodes(changed(extrasAt + 1, 20)));   // not in ascending order (20 then 20)
        CHECK(!decodes(changed(extrasAt + 1, 25)));   // the first one after the second one
        CHECK(decodes(changed(extrasAt + 1, 9)));     // 9, 20: in order (and present)
        // an extra for a joint that is not in the mask
        {
            Pose q;
            setRotation(q.joints[1], 0, 0, 0, 1);
            q.joints[1].hasPosition = true;
            q.joints[1].position[0] = 1.f;
            std::string t;
            CHECK(encodePose(q, t));
            std::vector<unsigned char> b = rawOf(t);
            CHECK(decodes(b));
            const std::size_t at = HEADER_BYTES + MASK_BYTES + ROTATION_BYTES + 1;
            CHECK(b[at] == 1);
            b[at] = 2;
            CHECK(!decodes(b));
        }
        // -32768 as a position
        {
            Pose q;
            setRotation(q.joints[1], 0, 0, 0, 1);
            q.joints[1].hasPosition = true;
            q.joints[1].position[0] = 1.f;
            std::string t;
            CHECK(encodePose(q, t));
            std::vector<unsigned char> b = rawOf(t);
            const std::size_t at = HEADER_BYTES + MASK_BYTES + ROTATION_BYTES + 1 + 2;
            b[at]     = 0x00;
            b[at + 1] = 0x80;
            CHECK(!decodes(b));
            b[at]     = 0x01;
            CHECK(decodes(b)); // 0x8001 is -32767: a value (the lowest)
        }

        // a mask of 130 joints with a count of 1 and the bytes of one rotation: the rotations must not be read past them
        {
            std::vector<unsigned char> b;
            b.push_back(static_cast<unsigned char>(FORMAT_VERSION));
            b.push_back(static_cast<unsigned char>(KIND_POSE));
            b.push_back(1);
            for (std::size_t i = 0; i < MASK_BYTES; ++i)
                b.push_back(i == MASK_BYTES - 1 ? 0x03 : 0xFF); // 130 bits
            for (int i = 0; i < 4; ++i)
                b.push_back(0);
            b.push_back(0); // no extras
            CHECK(!decodes(b));
        }
        // a flags byte of 4 (no value to read) with the sizes right
        {
            std::vector<unsigned char> b;
            b.push_back(static_cast<unsigned char>(FORMAT_VERSION));
            b.push_back(static_cast<unsigned char>(KIND_POSE));
            b.push_back(1);
            for (std::size_t i = 0; i < MASK_BYTES; ++i)
                b.push_back(i == 0 ? 0x01 : 0x00);
            for (int i = 0; i < 4; ++i)
                b.push_back(0);
            b.push_back(1); // one extra: joint 0
            b.push_back(0);
            b.push_back(1); // flags 1 (position): fine with its 6 bytes
            for (int i = 0; i < 6; ++i)
                b.push_back(0);
            CHECK(decodes(b));
            b[b.size() - 7] = 4;
            b.resize(b.size() - 6);
            CHECK(!decodes(b));
        }
        // a text of the right sort over PAYLOAD_MAX characters (a whole pose with 20 positions: 936 characters)
        {
            Pose big = fullPose();
            for (std::size_t i = 0; i < 20; ++i)
            {
                big.joints[i].hasPosition = true;
                big.joints[i].position[0] = 0.5f;
            }
            std::string t;
            CHECK(encodePose(big, t) && t.size() <= PAYLOAD_MAX); // the encoder keeps to the limit
            std::vector<unsigned char> b = rawOf(t);
            const std::size_t bigExtrasAt = HEADER_BYTES + MASK_BYTES + JOINT_COUNT * ROTATION_BYTES;
            b.resize(bigExtrasAt);
            b.push_back(20);
            for (unsigned char i = 0; i < 20; ++i)
            {
                b.push_back(i);
                b.push_back(1);
                for (int k = 0; k < 6; ++k)
                    b.push_back(k == 1 ? 0x40 : 0x00);
            }
            const std::string over = textOf(b);
            CHECK(over.size() > PAYLOAD_MAX);
            CHECK(!decodePose(over, out));
        }

        // too long a text
        std::string tooLong(PAYLOAD_MAX + 4, 'A');
        CHECK(!decodePose(tooLong, out));
        CHECK(!decodePose("", out));
        CHECK(!decodePose("not base64!", out));
    }

    // ---- sequence numbers
    {
        CHECK(seqIsAccepted(100, 101, false));
        CHECK(seqIsAccepted(100, 5000, false));
        CHECK(!seqIsAccepted(100, 100, false));  // the same one again
        CHECK(!seqIsAccepted(100, 99, false));   // late
        CHECK(!seqIsAccepted(1000000, 1, false)); // gap 999999: still late
        CHECK(seqIsAccepted(1000001, 1, false)); // gap 1000000: a new run
        CHECK(seqIsAccepted(300000000, 7, false));
        CHECK(seqIsAccepted(100, 100, true));    // an end may repeat the number
        CHECK(!seqIsAccepted(100, 99, true));
        CHECK(seqIsAccepted(0, 0, true));
        CHECK(!seqIsAccepted(0, 0, false));
    }

    // ---- counters of messages a second
    {
        RateLimit four(4);
        CHECK(four.take(10.0) && four.take(10.1) && four.take(10.5) && four.take(10.9));
        CHECK(!four.take(10.99));
        CHECK(!four.take(10.999));
        CHECK(four.take(11.0));  // a second after the window opened: a new one
        CHECK(four.take(11.2) && four.take(11.4) && four.take(11.6));
        CHECK(!four.take(11.7));
        CHECK(four.take(100.0)); // a long pause
        CHECK(four.take(5.0));   // a clock that went back opens a window
        RateLimit sixteen(16);
        int taken = 0;
        for (int i = 0; i < 100; ++i)
            taken += sixteen.take(50.0 + 0.005 * i) ? 1 : 0;
        CHECK(taken == 16);
        RateLimit none(0);
        CHECK(!none.take(1.0));
    }

    if (failures == 0)
        std::printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
