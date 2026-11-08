// Stand-alone test of shotmath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX shotmath_test.cpp && shotmath_test.exe
#include "shotmath.h"

#include <cstdio>

using namespace plus::camerashot;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

int main()
{
    // easing: ends fixed, monotonic
    for (Ease e : { Ease::Linear, Ease::Smooth, Ease::SlowIn, Ease::SlowOut })
    {
        CHECK(near(easeValue(e, 0.0), 0.0));
        CHECK(near(easeValue(e, 1.0), 1.0));
        double prev = 0.0;
        for (int i = 1; i <= 100; ++i)
        {
            const double v = easeValue(e, i / 100.0);
            CHECK(v >= prev);
            prev = v;
        }
        CHECK(near(easeValue(e, -3.0), 0.0) && near(easeValue(e, 3.0), 1.0));
    }
    CHECK(easeValue(Ease::SlowIn, 0.5) < 0.5 && easeValue(Ease::SlowOut, 0.5) > 0.5);
    CHECK(near(easeValue(Ease::Smooth, 0.5), 0.5));
    // smooth: gentle, not a plain smoothstep: never faster than 1.25 times the even speed,
    // and it has moved at least a twentieth in the first tenth
    {
        double fastest = 0.0;
        for (int i = 0; i < 1000; ++i)
        {
            fastest = std::max(fastest, (easeValue(Ease::Smooth, (i + 1) / 1000.0) - easeValue(Ease::Smooth, i / 1000.0)) * 1000.0);
        }
        CHECK(fastest < 1.2501);
        CHECK(easeValue(Ease::Smooth, 0.1) > 0.05);
    }

    // dolly zoom: the width covered at the subject is the same all the way
    const double fov0 = 1.0, d0 = 4.0;
    const double width0 = 2.0 * d0 * std::tan(fov0 * 0.5);
    for (int i = 0; i <= 20; ++i)
    {
        const double d = d0 + (1.5 - d0) * i / 20.0;
        const double fov = fovForConstantWidth(fov0, d0, d);
        CHECK(near(2.0 * d * std::tan(fov * 0.5), width0));
    }
    CHECK(near(fovForConstantWidth(fov0, d0, d0), fov0));
    CHECK(fovForConstantWidth(fov0, d0, 2.0) > fov0);  // closer: wider
    CHECK(fovForConstantWidth(fov0, d0, 8.0) < fov0);  // farther: narrower

    // zoom: 2 times magnification halves tan(fov/2)
    CHECK(near(std::tan(fovForMagnification(fov0, 2.0) * 0.5), std::tan(fov0 * 0.5) / 2.0));
    CHECK(near(fovForMagnification(fov0, 1.0), fov0));

    // dolly end distances
    CHECK(near(dollyEndDistance(4.0, 2.0, true), 2.0));
    CHECK(near(dollyEndDistance(4.0, 2.0, false), 8.0));
    CHECK(near(dollyEndDistance(4.0, 0.2, true), 4.0));       // amount below 1 counts as 1
    CHECK(near(dollyEndDistance(1.0, 100.0, true), SHOT_MIN_DISTANCE));

    // dolly zoom limits
    {
        const double d1 = dollyZoomEndDistance(1.0, 4.0, 50.0, true);   // would need a huge angle
        CHECK(near(fovForConstantWidth(1.0, 4.0, d1), SHOT_MAX_FOV, 1e-9) || near(d1, SHOT_MIN_DISTANCE));
        const double d2 = dollyZoomEndDistance(1.0, 4.0, 50.0, false);  // would need a tiny angle
        CHECK(near(fovForConstantWidth(1.0, 4.0, d2), SHOT_MIN_FOV, 1e-9));
        const double d3 = dollyZoomEndDistance(1.0, 4.0, 1.5, true);    // inside the limits: as asked
        CHECK(near(d3, 4.0 / 1.5));
    }

    // zoom limits
    CHECK(near(zoomEndMagnification(1.0, 2.0, true), 2.0));
    CHECK(near(zoomEndMagnification(1.0, 2.0, false), 0.5));
    CHECK(near(fovForMagnification(1.0, zoomEndMagnification(1.0, 100.0, true)), SHOT_MIN_FOV, 1e-9));
    CHECK(near(fovForMagnification(1.0, zoomEndMagnification(1.0, 100.0, false)), SHOT_MAX_FOV, 1e-9));
    // the camera's own limits (a 16:9 window: 175 degrees / (16/9) = about 1.72 rad at most)
    {
        const FovLimits wide = fovLimits(5.0 * 3.14159265358979 / 180.0, 1.718);
        CHECK(near(wide.hi, 1.718) && near(wide.lo, SHOT_MIN_FOV));
        CHECK(near(clampFov(2.0, wide), 1.718));
        const double d1 = dollyZoomEndDistance(1.047, 4.0, 3.0, true, wide);   // 60 degrees, 3 times in: needs 120
        CHECK(near(fovForConstantWidth(1.047, 4.0, d1), 1.718, 1e-9));
        CHECK(near(fovForMagnification(1.0, zoomEndMagnification(1.0, 5.0, false, wide)), 1.718, 1e-9));
        const FovLimits defaults = fovLimits(0.0, 10.0);                      // a camera wider than a shot
        CHECK(near(defaults.lo, SHOT_MIN_FOV) && near(defaults.hi, SHOT_MAX_FOV));
        const FovLimits crossed = fovLimits(1.0, 0.5);                        // lo never above hi
        CHECK(crossed.lo <= crossed.hi);
    }
    CHECK(near(zoomMagnificationAt(4.0, 0.0), 1.0) && near(zoomMagnificationAt(4.0, 1.0), 4.0));
    CHECK(near(zoomMagnificationAt(4.0, 0.5), 2.0));

    // the orbit: the distance and the height stay, a full turn comes back to the start, the way round is right
    {
        CHECK(near(orbitDegrees(0.0), ORBIT_MIN_DEGREES) && near(orbitDegrees(1.0e9), ORBIT_MAX_DEGREES) && near(orbitDegrees(360.0), 360.0));
        const double x0 = 3.0, y0 = 4.0;
        for (int i = 0; i <= 20; ++i)
        {
            double x = 0.0, y = 0.0;
            rotateAboutZ(x0, y0, orbitRadians(540.0, i % 2 == 0, i / 20.0), x, y);
            CHECK(near(std::sqrt(x * x + y * y), 5.0));
        }
        double x = 0.0, y = 0.0;
        rotateAboutZ(x0, y0, orbitRadians(360.0, true, 1.0), x, y);
        CHECK(near(x, x0, 1e-9) && near(y, y0, 1e-9));
        // looking at the origin from +X, a camera going to its right goes to +Y
        rotateAboutZ(1.0, 0.0, orbitRadians(90.0, true, 1.0), x, y);
        CHECK(near(x, 0.0, 1e-9) && near(y, 1.0, 1e-9));
        rotateAboutZ(1.0, 0.0, orbitRadians(90.0, false, 1.0), x, y);
        CHECK(near(y, -1.0, 1e-9));
        CHECK(near(orbitRadians(90.0, true, 5.0), orbitRadians(90.0, true, 1.0)));   // the progress is held at 1
        CHECK(orbitHasSide(10.0, 10.0) && orbitHasSide(3.0, 20.0));                  // level; 8.6 degrees from the vertical
        CHECK(!orbitHasSide(0.3, 20.0) && !orbitHasSide(0.1, 1.0) && !orbitHasSide(0.0, 5.0));
    }

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
