// Stand-alone test of shakemath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX shakemath_test.cpp && shakemath_test.exe
#include "shakemath.h"

#include <cstdio>

using namespace plus::camerapath;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

struct Stats
{
    double maxAbs[6] = {};
    double mean[6]   = {};
    double rms[6]    = {};
};

static Stats measure(const ShakeParams& p, double seconds)
{
    Stats s;
    const int n = (int)(seconds * 60);
    for (int i = 0; i < n; ++i)
    {
        const ShakeOffset o = shakeAt(p, i / 60.0);
        const double v[6] = { o.right, o.up, o.forward, o.yaw, o.pitch, o.roll };
        for (int c = 0; c < 6; ++c)
        {
            s.maxAbs[c] = std::max(s.maxAbs[c], std::fabs(v[c]));
            s.mean[c] += v[c] / n;
            s.rms[c] += v[c] * v[c] / n;
        }
    }
    for (double& r : s.rms)
    {
        r = std::sqrt(r);
    }
    return s;
}

int main()
{
    ShakeParams none;
    CHECK(shakeAt(none, 1.0).up == 0.0);
    ShakeParams zero;
    zero.kind   = ShakeKind::Quake;
    zero.amount = 0.0;
    CHECK(shakeAt(zero, 1.0).up == 0.0 && shakeAt(zero, 1.0).roll == 0.0);

    // same seed, same time: same; another seed: another
    ShakeParams a;
    a.kind = ShakeKind::Handheld;
    ShakeParams b = a;
    b.seed = 2;
    CHECK(shakeAt(a, 3.3).yaw == shakeAt(a, 3.3).yaw);
    CHECK(shakeAt(a, 3.3).yaw != shakeAt(b, 3.3).yaw);

    // bounded; no drift (the mean stays near 0); a stronger kind shakes more
    const ShakeKind kinds[] = { ShakeKind::Handheld, ShakeKind::Walking, ShakeKind::Running, ShakeKind::Quake };
    double rmsUp[4] = {};
    for (int k = 0; k < 4; ++k)
    {
        ShakeParams p;
        p.kind   = kinds[k];
        p.amount = 1.0;
        p.speed  = 1.0;
        const Stats s = measure(p, 120.0);
        for (int c = 0; c < 6; ++c)
        {
            CHECK(s.maxAbs[c] < 0.5 || c >= 3);                       // metres: nothing wild
            CHECK(std::fabs(s.mean[c]) < 0.25 * s.maxAbs[c] + 1e-9);  // balanced around the path
        }
        CHECK(s.maxAbs[3] < 6.0 && s.maxAbs[4] < 6.0 && s.maxAbs[5] < 6.0);   // degrees
        rmsUp[k] = s.rms[1];
    }
    CHECK(rmsUp[0] < rmsUp[1] && rmsUp[1] < rmsUp[2]);   // handheld < walking < running (vertical)

    // amount scales the size
    ShakeParams small = a;
    small.amount = 0.25;
    ShakeParams big = a;
    big.amount = 1.0;
    CHECK(measure(big, 30).rms[3] > 3.0 * measure(small, 30).rms[3]);

    // applyShake: with no offset the sample is as it was; the place moves along the camera axes
    Sample s;
    s.q      = lookQuat(0, 1, 0);   // looking along +Y
    s.v[C_X] = 10;
    s.v[C_Y] = 20;
    s.v[C_Z] = 30;
    ShakeOffset o;
    o.up      = 1.0;
    o.forward = 2.0;
    o.right   = 0.5;
    Sample moved = s;
    applyShake(moved, o);
    CHECK(std::fabs(moved.v[C_Z] - 31.0) < 1e-9);   // up is +Z
    CHECK(std::fabs(moved.v[C_Y] - 22.0) < 1e-9);   // forward is +Y
    CHECK(std::fabs(moved.v[C_X] - 10.5) < 1e-9);   // looking along +Y, right is +X
    ShakeOffset none_offset;
    Sample same = s;
    applyShake(same, none_offset);
    CHECK(std::fabs(dot(same.q, s.q)) > 1.0 - 1e-12 && same.v[C_X] == 10);
    ShakeOffset turn;
    turn.yaw = 10.0;
    Sample turned = s;
    applyShake(turned, turn);
    const Vec3 f = forwardOf(turned.q);
    CHECK(std::fabs(std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z) - 1.0) < 1e-9 && std::fabs(f.z) < 1e-9 && std::fabs(f.x) > 0.1);

    if (failures == 0)
    {
        std::printf("shakemath: all ok\n");
    }
    return failures == 0 ? 0 : 1;
}
