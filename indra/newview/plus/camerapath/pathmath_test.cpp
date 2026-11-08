// Stand-alone test of pathmath.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX pathmath_test.cpp && pathmath_test.exe
#include "pathmath.h"

#include <cstdio>

using namespace plus::camerapath;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static KeyPoint make(double x, double y, double z, double fx, double fy, double fz, double seconds, double hold = 0.0,
                  Motion motion = Motion::Smooth)
{
    KeyPoint p;
    p.v[C_X] = x; p.v[C_Y] = y; p.v[C_Z] = z;
    p.v[C_FOV] = 1.0; p.v[C_ROLL] = 0.0; p.v[C_FOCUS] = 10.0;
    p.q       = lookQuat(fx, fy, fz);
    p.seconds = seconds;
    p.hold    = hold;
    p.motion  = motion;
    return p;
}

int main()
{
    // lookQuat: the forward axis comes back
    const double dirs[][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { -1, 0.3, 0.2 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0.2, -0.9, -0.3 } };
    for (const auto& f : dirs)
    {
        const double l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        const Vec3 r   = forwardOf(lookQuat(f[0], f[1], f[2]));
        CHECK(near(r.x, f[0] / l) && near(r.y, f[1] / l) && near(r.z, f[2] / l));
    }

    // the times: arrival = previous arrival + previous hold + seconds; a cut takes no time
    Timeline t;
    t.set({ make(0, 0, 0, 1, 0, 0, 0), make(10, 0, 0, 1, 0, 0, 4, 1.0), make(10, 5, 0, 0, 1, 0, 0), make(20, 5, 0, 0, 1, 0, 2) });
    CHECK(near(t.arrival(0), 0) && near(t.arrival(1), 4) && near(t.arrival(2), 5) && near(t.arrival(3), 7));
    CHECK(near(t.total(), 7));

    Sample s;
    CHECK(t.evaluate(0.0, s) && near(s.v[C_X], 0));
    CHECK(t.evaluate(4.0, s) && near(s.v[C_X], 10));      // at point 1
    CHECK(t.evaluate(4.9, s) && near(s.v[C_X], 10));      // held there
    CHECK(t.evaluate(5.0, s) && near(s.v[C_Y], 5));       // the cut: at point 2 at once
    CHECK(t.evaluate(4.999, s) && near(s.v[C_Y], 0));     // and not before
    CHECK(t.evaluate(7.0, s) && near(s.v[C_X], 20));      // the end
    CHECK(t.evaluate(100.0, s) && near(s.v[C_X], 20));    // past the end
    CHECK(t.evaluate(-3.0, s) && near(s.v[C_X], 0));

    // a smooth move between two points: monotonic, at rest at both ends (no points beyond)
    Timeline two;
    two.set({ make(0, 0, 0, 1, 0, 0, 0), make(10, 0, 0, 1, 0, 0, 5) });
    double prev = -1.0;
    for (int i = 0; i <= 50; ++i)
    {
        CHECK(two.evaluate(i * 0.1, s));
        CHECK(s.v[C_X] >= prev - 1e-9);
        prev = s.v[C_X];
    }
    CHECK(near(prev, 10));
    CHECK(two.evaluate(0.05, s) && s.v[C_X] < 0.2);   // starts gently

    // a smooth curve through three points does not stop at the middle one (speed there is not 0)
    Timeline three;
    three.set({ make(0, 0, 0, 1, 0, 0, 0), make(10, 0, 0, 1, 0, 0, 5), make(20, 0, 0, 1, 0, 0, 5) });
    Sample a, b;
    CHECK(three.evaluate(4.9, a) && three.evaluate(5.1, b));
    CHECK((b.v[C_X] - a.v[C_X]) / 0.2 > 1.5);   // about 2 per second
    CHECK(near(three.total(), 10));

    // the same through a hold: the camera rests there
    Timeline held;
    held.set({ make(0, 0, 0, 1, 0, 0, 0), make(10, 0, 0, 1, 0, 0, 5, 2.0), make(20, 0, 0, 1, 0, 0, 5) });
    CHECK(held.evaluate(5.0, a) && held.evaluate(6.9, b) && near(a.v[C_X], b.v[C_X]));
    CHECK(held.evaluate(4.9, a) && held.evaluate(5.0, b) && (b.v[C_X] - a.v[C_X]) < 0.1);   // slowing to rest

    // a linear move is at an even speed
    Timeline lin;
    lin.set({ make(0, 0, 0, 1, 0, 0, 0), make(10, 0, 0, 1, 0, 0, 5, 0, Motion::Linear) });
    CHECK(lin.evaluate(1.0, s) && near(s.v[C_X], 2));
    CHECK(lin.evaluate(2.5, s) && near(s.v[C_X], 5));

    // the direction turns from +X to +Y through the middle, unit length always
    Timeline turn;
    turn.set({ make(0, 0, 0, 1, 0, 0, 0), make(0, 0, 0, 0, 1, 0, 4), make(0, 0, 0, -1, 0, 0, 4) });
    for (int i = 0; i <= 80; ++i)
    {
        CHECK(turn.evaluate(i * 0.1, s));
        CHECK(near(std::sqrt(dot(s.q, s.q)), 1.0, 1e-6));
    }
    CHECK(turn.evaluate(4.0, s));
    {
        const Vec3 f = forwardOf(s.q);
        CHECK(near(f.x, 0) && near(f.y, 1));
    }
    CHECK(turn.evaluate(2.0, s));
    {
        const Vec3 f = forwardOf(s.q);
        CHECK(f.x > 0.3 && f.y > 0.3);   // between +X and +Y
    }

    // one point, none
    Timeline one;
    one.set({ make(3, 4, 5, 1, 0, 0, 9) });
    CHECK(one.evaluate(10.0, s) && near(s.v[C_X], 3) && near(one.total(), 0));
    Timeline none;
    CHECK(!none.evaluate(0.0, s));

    // a turn between two points comes to rest at both ends as the position does (not a sudden start)
    {
        Timeline rest;
        rest.set({ make(0, 0, 0, 1, 0, 0, 0), make(0, 0, 0, 0, 1, 0, 5) });
        Sample first, mid;
        CHECK(rest.evaluate(0.1, first) && rest.evaluate(2.5, mid));
        const double turned_first = 2.0 * std::acos(std::min(1.0, std::fabs(dot(first.q, lookQuat(1, 0, 0)))));
        const double turned_mid   = 2.0 * std::acos(std::min(1.0, std::fabs(dot(mid.q, lookQuat(1, 0, 0)))));
        CHECK(turned_first < 0.03 * turned_mid + 0.002);   // 0.1 s of 5 s: still barely moving
        Sample end_a, end_b;
        CHECK(rest.evaluate(4.8, end_a) && rest.evaluate(5.0, end_b));
        CHECK(2.0 * std::acos(std::min(1.0, std::fabs(dot(end_a.q, end_b.q)))) < 0.02);   // and nearly still at the end
    }

    // an orbit: the pivot at the origin, the camera on a circle of 10 metres at a height of 3
    {
        auto orbit_point = [](double x, double y, double seconds, double turns) {
            KeyPoint p = make(x, y, 3.0, -x, -y, -3.0, seconds, 0.0, Motion::Orbit);
            p.hasPivot   = true;
            p.pivot[0]   = 0.0; p.pivot[1] = 0.0; p.pivot[2] = 0.0;
            p.orbitTurns = turns;
            p.q          = lookQuat(-x, -y, -3.0);
            return p;
        };
        // a quarter of a circle (the short way), then the long way round with a lap against it
        Timeline quarter;
        quarter.set({ orbit_point(10, 0, 0, 0), orbit_point(0, 10, 4, 0) });
        Sample out;
        for (int i = 0; i <= 40; ++i)
        {
            CHECK(quarter.evaluate(i * 0.1, out));
            CHECK(near(std::hypot(out.v[C_X], out.v[C_Y]), 10.0, 1e-6));   // the radius stays
            CHECK(near(out.v[C_Z], 3.0, 1e-6));                            // and the height
            const Vec3 f = forwardOf(out.q);                               // facing the pivot
            const double len = std::sqrt(out.v[C_X] * out.v[C_X] + out.v[C_Y] * out.v[C_Y] + 9.0);
            CHECK((f.x * -out.v[C_X] + f.y * -out.v[C_Y] + f.z * -3.0) / len > 0.999);
        }
        CHECK(quarter.evaluate(2.0, out) && near(out.v[C_X], 10.0 * std::cos(0.7853981633974483), 1e-6)
              && near(out.v[C_Y], 10.0 * std::sin(0.7853981633974483), 1e-6));   // half way is 45 degrees
        CHECK(quarter.evaluate(4.0, out) && near(out.v[C_X], 0.0, 1e-6) && near(out.v[C_Y], 10.0, 1e-6));
        Timeline long_way;
        long_way.set({ orbit_point(10, 0, 0, 0), orbit_point(0, 10, 4, -1) });   // a lap the other way: 270 degrees clockwise
        CHECK(long_way.evaluate(2.0, out) && near(out.v[C_X], -7.0710678, 1e-4) && near(out.v[C_Y], -7.0710678, 1e-4));   // half way is at -135 degrees
        // the same place and one lap: a full circle that comes back
        Timeline lap;
        lap.set({ orbit_point(10, 0, 0, 0), orbit_point(10, 0, 4, 1) });
        CHECK(lap.evaluate(2.0, out) && near(out.v[C_X], -10.0, 1e-6) && near(out.v[C_Y], 0.0, 1e-6));
        CHECK(lap.evaluate(4.0, out) && near(out.v[C_X], 10.0, 1e-6));
        // half a circle with the pivot moving a little (an avatar that sways): the camera keeps going the same way round
        // (through +Y), however the angle between the two points crosses half a turn
        for (double sway : { -0.02, -0.001, 0.0, 0.001, 0.02 })
        {
            Timeline half;
            half.set({ orbit_point(10, 0, 0, 0), orbit_point(-10, 0, 4, 0) });
            half.setPivot(1, 0.0, sway, 0.0);
            for (double at : { 1.0, 2.0, 3.0 })
            {
                CHECK(half.evaluate(at, out) && out.v[C_Y] > 4.0);
            }
        }
        // the fallback is decided at the start: a pivot that moves right under the camera later does not switch the curve
        {
            Timeline steady;
            steady.set({ orbit_point(10, 0, 0, 0), orbit_point(0, 10, 4, 0) });
            Sample before_move, after_move;
            CHECK(steady.evaluate(2.0, before_move));
            steady.setPivot(1, 9.9999, 0.0, 0.0);   // the first end is almost on the pivot
            CHECK(steady.evaluate(2.0, after_move) && std::isfinite(after_move.v[C_X]) && std::isfinite(after_move.v[C_Y]));
        }
        // no pivot, or a point straight above it: the move is a smooth one and nothing breaks
        Timeline above;
        KeyPoint top = orbit_point(0.0, 0.0, 4, 0);
        above.set({ orbit_point(10, 0, 0, 0), top });
        CHECK(above.evaluate(2.0, out) && std::isfinite(out.v[C_X]) && std::isfinite(dot(out.q, out.q)));
        KeyPoint bare = orbit_point(0, 10, 4, 0);
        bare.hasPivot = false;
        Timeline none_pivot;
        none_pivot.set({ orbit_point(10, 0, 0, 0), bare });
        CHECK(none_pivot.evaluate(2.0, out) && std::isfinite(out.v[C_X]));
        // fuzz: orbit points mixed with the others, everything stays finite and the direction is a unit quaternion
        unsigned seed4 = 4242;
        auto next4 = [&seed4]() { seed4 = seed4 * 1664525u + 1013904223u; return (seed4 >> 8) / 16777216.0; };
        for (int round = 0; round < 1500; ++round)
        {
            std::vector<KeyPoint> points;
            const int n = 2 + (int)(next4() * 6);
            for (int i = 0; i < n; ++i)
            {
                KeyPoint p = orbit_point((next4() - 0.5) * 40, (next4() - 0.5) * 40, i == 0 ? 0.0 : (next4() < 0.2 ? 0.0 : 0.5 + next4() * 4),
                                         std::round((next4() - 0.5) * 6));
                p.hold = next4() < 0.3 ? next4() * 2 : 0.0;
                p.hasPivot = next4() < 0.9;
                p.motion = next4() < 0.7 ? Motion::Orbit : (next4() < 0.5 ? Motion::Smooth : Motion::Linear);
                points.push_back(p);
            }
            Timeline mixed;
            mixed.set(points);
            for (int k = 0; k < 30; ++k)
            {
                CHECK(mixed.evaluate(next4() * (mixed.total() + 1.0), out));
                for (double v : out.v)
                {
                    CHECK(std::isfinite(v));
                }
                CHECK(std::fabs(dot(out.q, out.q) - 1.0) < 1e-6);
            }
        }
        // orbit and smooth moves one after another (fix-round97): the angle of view, the tilt and the focus distance go
        // through a point between them at one speed (no jump), and on numbers that only go up they never go back
        unsigned seed5 = 5151;
        auto next5 = [&seed5]() { seed5 = seed5 * 1664525u + 1013904223u; return (seed5 >> 8) / 16777216.0; };
        for (int round = 0; round < 800; ++round)
        {
            std::vector<KeyPoint> points;
            const int n = 3 + (int)(next5() * 5);
            double fov = 0.5, roll = 0.0, focus = 5.0;
            for (int i = 0; i < n; ++i)
            {
                const double angle = next5() * 6.283185307179586;
                KeyPoint p = orbit_point(10 * std::cos(angle), 10 * std::sin(angle), i == 0 ? 0.0 : 0.5 + next5() * 4, 0);
                p.hold   = next5() < 0.15 ? next5() : 0.0;
                p.motion = next5() < 0.5 ? Motion::Orbit : Motion::Smooth;
                fov   += next5() < 0.3 ? 0.0 : next5() * 0.2;
                roll  += next5() < 0.3 ? 0.0 : (next5() < 0.7 ? next5() * 20.0 : 360.0);
                focus += next5() < 0.3 ? 0.0 : next5() * 3.0;
                p.v[C_FOV] = fov; p.v[C_ROLL] = roll; p.v[C_FOCUS] = focus;
                points.push_back(p);
            }
            Timeline mixed;
            mixed.set(points);
            const size_t comps[] = { (size_t)C_FOV, (size_t)C_ROLL, (size_t)C_FOCUS };
            double last[3] = { -1e9, -1e9, -1e9 };
            for (int k = 0; k <= 300; ++k)
            {
                CHECK(mixed.evaluate(mixed.total() * k / 300.0, out));
                for (int j = 0; j < 3; ++j)
                {
                    CHECK(out.v[comps[j]] >= last[j] - 1e-6);
                    last[j] = out.v[comps[j]];
                }
            }
            for (size_t i = 1; i + 1 < points.size(); ++i)
            {
                if (points[i].hold > 0.0)
                {
                    continue;
                }
                const double t0 = mixed.arrival(i), e = 1e-6;
                Sample before_t, at_t, after_t;
                CHECK(mixed.evaluate(t0 - e, before_t) && mixed.evaluate(t0, at_t) && mixed.evaluate(t0 + e, after_t));
                for (size_t c : comps)
                {
                    const double left  = (at_t.v[c] - before_t.v[c]) / e;
                    const double right = (after_t.v[c] - at_t.v[c]) / e;
                    CHECK(std::fabs(left - right) < 1e-3 * (1.0 + std::fabs(left)) + 0.05);
                }
            }
        }
    }

    // rollStep: the short way round, and the turns on top
    CHECK(near(rollStep(179, -179, 0), 2.0) && near(rollStep(-179, 179, 0), -2.0));
    CHECK(near(rollStep(0, 90, 1), 450.0) && near(rollStep(10, 10, 2), 720.0) && near(rollStep(10, 10, -1), -360.0));
    {
        // added up along points, the roll of each one is its own tilt plus whole turns
        unsigned seed3 = 99;
        auto next3 = [&seed3]() { seed3 = seed3 * 1664525u + 1013904223u; return (seed3 >> 8) / 16777216.0; };
        double unwrapped = 0.0, base = 0.0;
        for (int i = 0; i < 200; ++i)
        {
            const double own = (next3() - 0.5) * 360.0;
            const double turns = std::round((next3() - 0.5) * 8.0);
            unwrapped = i == 0 ? own : unwrapped + rollStep(base, own, turns);
            base = own;
            CHECK(near(std::remainder(unwrapped - own, 360.0), 0.0, 1e-6));
        }
    }

    // a spin: the roll of the next point is 360 further (the same picture), and the roll goes all the way round
    {
        KeyPoint from = make(0, 0, 0, 1, 0, 0, 0);
        KeyPoint to   = make(0, 0, 0, 1, 0, 0, 3);
        to.v[C_ROLL] = from.v[C_ROLL] + 360.0;
        Timeline spin;
        spin.set({ from, to });
        double previous = -1.0;
        for (int i = 0; i <= 30; ++i)
        {
            Sample out;
            CHECK(spin.evaluate(i * 0.1, out));
            CHECK(out.v[C_ROLL] >= previous - 1e-9);   // turning one way all along
            previous = out.v[C_ROLL];
        }
        Sample half, last;
        CHECK(spin.evaluate(1.5, half) && near(half.v[C_ROLL], 180.0, 1e-6));
        CHECK(spin.evaluate(3.0, last) && near(last.v[C_ROLL], 360.0, 1e-9));
    }

    // the tilt on a smooth move through points that only go up never goes back (spins are many turns)
    {
        unsigned seed2 = 777;
        auto next2 = [&seed2]() { seed2 = seed2 * 1664525u + 1013904223u; return (seed2 >> 8) / 16777216.0; };
        for (int round = 0; round < 500; ++round)
        {
            const int n = 2 + (int)(next2() * 6);
            std::vector<KeyPoint> points;
            double roll = 0.0;
            for (int i = 0; i < n; ++i)
            {
                KeyPoint p = make(0, 0, 0, 1, 0, 0, i == 0 ? 0.0 : 0.5 + next2() * 4, next2() < 0.2 ? next2() * 2 : 0.0);
                roll += next2() < 0.4 ? 0.0 : (next2() < 0.5 ? 10.0 : 360.0 * (1 + (int)(next2() * 5)));   // a spin, a little, or none
                p.v[C_ROLL] = roll;
                points.push_back(p);
            }
            Timeline up;
            up.set(points);
            double before = -1.0;
            for (int k = 0; k <= 400; ++k)
            {
                Sample out;
                CHECK(up.evaluate(up.total() * k / 400.0, out));
                CHECK(out.v[C_ROLL] >= before - 1e-6);
                CHECK(out.v[C_ROLL] <= roll + 1e-6);
                before = out.v[C_ROLL];
            }
            // the speed of the tilt does not jump at a point where the camera does not rest (the slope there is the same
            // for the move before and the move after)
            for (size_t i = 1; i + 1 < points.size(); ++i)
            {
                if (points[i].hold > 0.0)
                {
                    continue;
                }
                const double t0 = up.arrival(i), e = 1e-6;
                Sample before_t, at_t, after_t;
                CHECK(up.evaluate(t0 - e, before_t) && up.evaluate(t0, at_t) && up.evaluate(t0 + e, after_t));
                const double left = (at_t.v[C_ROLL] - before_t.v[C_ROLL]) / e;
                const double right = (after_t.v[C_ROLL] - at_t.v[C_ROLL]) / e;
                CHECK(std::fabs(left - right) < 1e-3 * (1.0 + std::fabs(left)) + 0.05);
            }
        }
    }

    // fuzz: random points, times (zeros, cuts, holds, all three motions), random times of evaluation:
    // every number is finite, the direction is a unit quaternion, a Smooth move stays near its two points
    unsigned seed = 12345;
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
    for (int round = 0; round < 3000; ++round)
    {
        const int n = 1 + (int)(next() * 8);
        std::vector<KeyPoint> points;
        for (int i = 0; i < n; ++i)
        {
            KeyPoint p = make(next() * 200, next() * 200, next() * 50, next() - 0.5, next() - 0.5, next() - 0.5,
                              next() < 0.25 ? 0.0 : next() * 6, next() < 0.4 ? 0.0 : next() * 3,
                              next() < 0.5 ? Motion::Smooth : (next() < 0.5 ? Motion::Linear : Motion::Ease));
            p.v[C_FOV] = 0.3 + next();
            p.v[C_ROLL] = (next() - 0.5) * 40.0;
            points.push_back(p);
        }
        Timeline fuzz;
        fuzz.set(points);
        for (int k = 0; k < 40; ++k)
        {
            Sample out;
            CHECK(fuzz.evaluate(next() * (fuzz.total() + 2.0) - 1.0, out));
            for (double v : out.v)
            {
                CHECK(std::isfinite(v));
            }
            CHECK(std::isfinite(dot(out.q, out.q)) && std::fabs(dot(out.q, out.q) - 1.0) < 1e-6);
            for (size_t c = 0; c < 3; ++c)
            {
                CHECK(std::fabs(out.v[c]) < 1000.0);   // the points are within 200: a curve does not fly off
            }
        }
        // times: arrival is never earlier than the one before, the end is the last stay's end
        for (size_t i = 1; i < fuzz.size(); ++i)
        {
            CHECK(fuzz.arrival(i) >= fuzz.arrival(i - 1));
        }
        CHECK(fuzz.total() >= fuzz.arrival(fuzz.size() - 1));
    }

    // round 6: no direction jump within a move (the biggest turn in one 1/2000 step of the move stays small)
    {
        const auto worstStep = [](const Timeline& tl, double t0, double t1) {
            Sample prev;
            tl.evaluate(t0, prev);
            double worst = 0.0;
            for (int k = 1; k <= 2000; ++k)
            {
                Sample cur;
                tl.evaluate(t0 + (t1 - t0) * k / 2000.0, cur);
                worst = std::max(worst, 2.0 * std::acos(std::min(1.0, std::fabs(dot(cur.q, prev.q)))) * 57.29577951308232);
                prev = cur;
            }
            return worst;
        };
        // Smooth: squad's slerps switched to the short way part-way (173 degrees in one step at u = 0.67 of the first move;
        // the second set is the round-6 report's, yaw 43.7 / 94.7 / 5.1 / 54.1 degrees)
        {
            const Quat q3[] = { { -0.25779277523459332, 0.080760066804969929, 0.021628793958373305, 0.96257617460563061 },
                                { 0.97396322908468358, -0.025376334027063976, -0.17642732710137618, -0.14008950108465351 },
                                { 0.50523300186303333, -0.093865570356021361, -0.05539716226480737, -0.85607244024277152 } };
            const double x3[] = { -19.133007623202221, 17.14796706551855, -16.855163915810802 };
            const double s3[] = { 4.0547369294187172, 3.5495481807751967, 4.6720433578641423 };
            std::vector<KeyPoint> jumped;
            for (int i = 0; i < 3; ++i)
            {
                KeyPoint p = make(x3[i], 0, 0, 1, 0, 0, s3[i]);
                p.q        = q3[i];
                jumped.push_back(p);
            }
            const double yaws[] = { 43.7, 94.7, 5.1, 54.1 }, s4[] = { 1.29, 1.13, 3.02, 3.51 };
            std::vector<KeyPoint> reported;
            for (int i = 0; i < 4; ++i)
            {
                const double r = yaws[i] / 57.29577951308232;
                reported.push_back(make(i * 3.0, 0, 0, std::cos(r), std::sin(r), 0, s4[i]));
            }
            for (const auto& points : { jumped, reported })
            {
                Timeline smooth;
                smooth.set(points);
                for (size_t seg = 1; seg < smooth.size(); ++seg)
                {
                    CHECK(worstStep(smooth, smooth.arrival(seg - 1), smooth.arrival(seg)) < 5.0);
                }
            }
        }
        // Orbit with a whole turn: the blend from a's direction chased a target turning fast and flipped (145 degrees)
        {
            KeyPoint from = make(1.779, -0.425, 0.210, 1, 0, 0, 0);
            from.q = normalized(Quat{ 0.5975, 0.0605, -0.0453, 0.7983 });
            KeyPoint round = make(0.863, -6.137, -0.008, 1.739 - 0.863, -3.971 + 6.137, -0.285 + 0.008, 3.43, 0.0, Motion::Orbit);
            round.hasPivot = true;
            round.pivot[0] = 1.739; round.pivot[1] = -3.971; round.pivot[2] = -0.285;
            round.orbitTurns = 1;
            Timeline orbit;
            orbit.set({ from, round });
            CHECK(worstStep(orbit, 0.0, orbit.total()) < 5.0);
            Sample start, mid;
            CHECK(orbit.evaluate(0.0, start) && orbit.evaluate(orbit.total() * 0.5, mid));
            CHECK(std::fabs(dot(start.q, from.q)) > 1.0 - 1e-9);   // starts on a's own direction
            const Vec3 f = forwardOf(mid.q);
            const double dx = round.pivot[0] - mid.v[C_X], dy = round.pivot[1] - mid.v[C_Y], dz = round.pivot[2] - mid.v[C_Z];
            CHECK((f.x * dx + f.y * dy + f.z * dz) / std::sqrt(dx * dx + dy * dy + dz * dz) > 0.999);   // then faces the pivot
        }
    }

    if (failures == 0)
    {
        std::printf("pathmath: all ok\n");
    }
    return failures == 0 ? 0 : 1;
}
