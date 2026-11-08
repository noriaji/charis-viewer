/**
 * @file pathmath.h
 * @brief Camera path: the numbers (a timeline of points, the position, angle and direction at a time).
 *        Plain C++, no viewer types, so it can be tested on its own (pathmath_test.cpp).
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

#ifndef PLUS_CAMERAPATH_PATHMATH_H
#define PLUS_CAMERAPATH_PATHMATH_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

// The timeline. Point 0 is where the path starts. Every other point i has
//
//   seconds   the time the camera takes from point i-1 to point i; 0 is a cut (the camera is at point i
//             in the next frame, with no move between)
//   hold      the time the camera stays at point i after it got there
//   motion    how the camera goes from point i-1 to point i
//
// so the camera arrives at point i at arrive[i] = arrive[i-1] + hold[i-1] + seconds[i], and the path
// lasts until the last point's hold is over.
//
//   Smooth    one curve through the points that keeps going (a cubic Hermite curve whose slope at a point
//             is taken from the points before and after it, in time). The camera stops only at the ends of
//             the path and at points with a hold; before or after a cut or a non-smooth move it goes on
//             at the slope of the smooth side
//   Linear    a straight line at an even speed
//   Ease      a straight line that starts and stops gently (the camera is at rest at both ends)
//
// The position, the angle of view, the tilt and the distance of the focus point are numbers, moved
// by the same curve. The direction is a unit quaternion (the camera looks at the focus point, up is
// +Z), moved by slerp, or, for a Smooth move, by squad (the quaternion counterpart of the Hermite curve:
// the direction keeps turning through the point and does not stop there).
namespace plus::camerapath
{
    enum class Motion
    {
        Smooth,
        Linear,
        Ease,
        Orbit    // an arc about a pivot (the point's pivot), the camera facing it; see Timeline::orbitSegment()
    };

    constexpr size_t COMPONENTS = 6;   // x, y, z, field of view, tilt, focus distance
    enum Component
    {
        C_X, C_Y, C_Z, C_FOV, C_ROLL, C_FOCUS
    };

    struct Quat
    {
        double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
    };

    inline Quat operator*(const Quat& a, const Quat& b)
    {
        return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                 a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
    }
    inline Quat conjugate(const Quat& q) { return { q.w, -q.x, -q.y, -q.z }; }
    inline double dot(const Quat& a, const Quat& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Quat negated(const Quat& q) { return { -q.w, -q.x, -q.y, -q.z }; }
    inline Quat normalized(const Quat& q)
    {
        const double n = std::sqrt(dot(q, q));
        return n > 1e-12 ? Quat{ q.w / n, q.x / n, q.y / n, q.z / n } : Quat{};
    }
    // q on the same side of the sphere as 'reference' (q and -q are the same turn).
    inline Quat sameSide(const Quat& q, const Quat& reference) { return dot(q, reference) < 0.0 ? negated(q) : q; }

    // From a to b along the arc between them as given, without first turning b to a's side. For squad, whose ends move
    // with the progress: choosing the short way afresh at each step would switch ways part-way and jump (round 6).
    // b nearly opposite a is the same turn as a; that one case goes the short way (no full spin, no zero-length lerp).
    inline Quat slerpNoFlip(const Quat& a, const Quat& b0, double t)
    {
        const Quat b = dot(a, b0) < -0.9995 ? negated(b0) : b0;
        const double d = std::clamp(dot(a, b), -1.0, 1.0);
        if (d > 0.9995)
        {
            return normalized({ a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t });
        }
        const double theta = std::acos(d);
        const double s     = std::sin(theta);
        const double wa    = std::sin((1.0 - t) * theta) / s;
        const double wb    = std::sin(t * theta) / s;
        return { a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb };
    }
    // From a to b the short way round (q and -q are the same turn).
    inline Quat slerp(const Quat& a, const Quat& b, double t) { return slerpNoFlip(a, sameSide(b, a), t); }

    // The turn from 'from' to 'to' as a rotation vector (the quaternion's logarithm) and back.
    struct Vec3
    {
        double x = 0.0, y = 0.0, z = 0.0;
    };
    inline Vec3 logRelative(const Quat& from, const Quat& to)
    {
        Quat rel = conjugate(from) * to;
        if (rel.w < 0.0)
        {
            rel = negated(rel);
        }
        const double s = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        if (s < 1e-9)
        {
            return {};
        }
        const double k = std::atan2(s, rel.w) / s;
        return { rel.x * k, rel.y * k, rel.z * k };
    }
    inline Quat expVec(const Vec3& v)
    {
        const double a = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (a < 1e-9)
        {
            return {};
        }
        const double k = std::sin(a) / a;
        return { std::cos(a), v.x * k, v.y * k, v.z * k };
    }

    // The direction from a unit vector 'forward' (x axis of the camera), up +Z. Straight up or down:
    // any left axis will do.
    inline Quat lookQuat(double fx, double fy, double fz)
    {
        const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-9)
        {
            return {};
        }
        fx /= fl; fy /= fl; fz /= fl;
        // left = up x forward = (-fy, fx, 0)
        double lx = -fy, ly = fx, lz = 0.0;
        const double ll = std::sqrt(lx * lx + ly * ly);
        if (ll < 1e-6)
        {
            lx = 0.0; ly = 1.0;
        }
        else
        {
            lx /= ll; ly /= ll;
        }
        // up = forward x left
        const double ux = fy * lz - fz * ly;
        const double uy = fz * lx - fx * lz;
        const double uz = fx * ly - fy * lx;
        // The matrix with the columns forward, left, up, to a quaternion.
        const double m00 = fx, m10 = fy, m20 = fz;
        const double m01 = lx, m11 = ly, m21 = lz;
        const double m02 = ux, m12 = uy, m22 = uz;
        const double trace = m00 + m11 + m22;
        Quat q;
        if (trace > 0.0)
        {
            const double s = std::sqrt(trace + 1.0) * 2.0;
            q = { 0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s };
        }
        else if (m00 > m11 && m00 > m22)
        {
            const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
            q = { (m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s };
        }
        else if (m11 > m22)
        {
            const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
            q = { (m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s };
        }
        else
        {
            const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
            q = { (m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s };
        }
        return normalized(q);
    }

    // The camera's forward axis (the first column of the rotation).
    inline Vec3 forwardOf(const Quat& q)
    {
        return { 1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.w * q.z), 2.0 * (q.x * q.z - q.w * q.y) };
    }

    // The turn of an angle (radians) about a unit axis, and a vector turned by q.
    inline Quat axisAngle(double ax, double ay, double az, double radians)
    {
        const double h = std::sin(radians * 0.5);
        return { std::cos(radians * 0.5), ax * h, ay * h, az * h };
    }
    inline Vec3 rotate(const Quat& q, const Vec3& v)
    {
        // v' = q * (0, v) * q^-1
        const Quat r = q * Quat{ 0.0, v.x, v.y, v.z } * conjugate(q);
        return { r.x, r.y, r.z };
    }

    struct KeyPoint
    {
        double v[COMPONENTS] = {};
        Quat   q;
        double seconds = 0.0;   // from the point before (0: a cut); ignored for point 0
        double hold    = 0.0;
        Motion motion  = Motion::Smooth;
        // Motion::Orbit: what the camera goes round (hasPivot), and the whole laps on top of the short way round
        // from the point before (signed: + goes the way a positive angle about the vertical axis does).
        double pivot[3]    = {};
        bool   hasPivot    = false;
        double orbitTurns  = 0.0;
        // Made by Timeline::set(): whether the move goes round (the pivot is not nearly straight above or below either
        // end), and the sweep (radians) it has then. While the pivot moves the sweep follows continuously from that one
        // (it never jumps to the other way round).
        bool   orbitReady  = false;
        double orbitSweep  = 0.0;
    };

    struct Sample
    {
        double v[COMPONENTS] = {};
        Quat   q;
    };

    // The change of the roll (degrees) from a point whose own tilt is from_base to the next one with to_base, with
    // 'turns' whole turns on top: the short way round (within half a turn) plus the turns. A timeline's roll adds
    // these up, so the roll of every point is its own tilt however many turns went before, and 179 to -179 is a
    // turn of 2 degrees, not 358.
    inline double rollStep(double from_base, double to_base, double turns)
    {
        return std::remainder(to_base - from_base, 360.0) + 360.0 * turns;
    }

    inline double smoothstep(double u)
    {
        u = std::clamp(u, 0.0, 1.0);
        return u * u * (3.0 - 2.0 * u);
    }

    class Timeline
    {
    public:
        // Takes the points and works out the times. Negative times count as 0.
        void set(std::vector<KeyPoint> points)
        {
            mPoints = std::move(points);
            mArrive.assign(mPoints.size(), 0.0);
            mTotal = 0.0;
            for (size_t i = 0; i < mPoints.size(); ++i)
            {
                KeyPoint& p = mPoints[i];
                p.seconds = i == 0 ? 0.0 : std::max(p.seconds, 0.0);
                p.hold    = std::max(p.hold, 0.0);
                mArrive[i] = i == 0 ? 0.0 : mArrive[i - 1] + mPoints[i - 1].hold + p.seconds;
                mTotal     = mArrive[i] + p.hold;
            }
            for (size_t i = 1; i < mPoints.size(); ++i)
            {
                prepareOrbit(i);
            }
        }

        // Replaces the direction and the focus distance of point i (a point that looks at an avatar:
        // the direction changes while the avatar moves). The times stay.
        void setDirection(size_t i, const Quat& q, double focusDistance)
        {
            if (i < mPoints.size())
            {
                mPoints[i].q          = q;
                mPoints[i].v[C_FOCUS] = focusDistance;
            }
        }

        // The place point i goes round, when it moves (an avatar it looks at).
        void setPivot(size_t i, double x, double y, double z)
        {
            if (i < mPoints.size())
            {
                mPoints[i].pivot[0] = x;
                mPoints[i].pivot[1] = y;
                mPoints[i].pivot[2] = z;
                mPoints[i].hasPivot = true;
            }
        }

        size_t size() const { return mPoints.size(); }
        double total() const { return mTotal; }
        // The time the camera is at point i.
        double arrival(size_t i) const { return i < mArrive.size() ? mArrive[i] : 0.0; }

        // The camera at time t (seconds from the start, clamped to the path). False with no points.
        bool evaluate(double t, Sample& out) const
        {
            const size_t n = mPoints.size();
            if (n == 0)
            {
                return false;
            }
            t = std::clamp(t, 0.0, mTotal);
            // The last point reached by t (arrivals never go down).
            size_t a = 0;
            while (a + 1 < n && mArrive[a + 1] <= t)
            {
                ++a;
            }
            const double leave = mArrive[a] + mPoints[a].hold;   // when the camera leaves point a
            if (a + 1 == n || t < leave)
            {
                copyPoint(mPoints[a], out);
                return true;
            }
            const size_t b = a + 1;
            segment(a, b, (t - leave) / mPoints[b].seconds, out);
            return true;
        }

    private:
        static void copyPoint(const KeyPoint& p, Sample& out)
        {
            std::copy(std::begin(p.v), std::end(p.v), std::begin(out.v));
            out.q = p.q;
        }

        // The slope (per second) of component c at the start (atStart) or the end of the move a -> b. The angle of view,
        // the tilt and the focus distance are kept from overshooting: a smooth curve through points that only go up
        // does not dip back between them (a spin of several turns would turn back for a moment otherwise). The
        // place is left as the curve makes it.
        //
        // The limit at a point comes from the average slopes of both moves next to it (d1 before, d2 after), so the two
        // moves get the same slope there and the speed does not jump: level on one side or a turning point (d1 d2 <= 0)
        // is a stop, else the slope is held to three times the smaller of the two (Fritsch-Carlson). Rests, cuts and
        // the moves that are not smooth keep the slope rawSlope gives.
        double slope(size_t c, size_t a, size_t b, bool atStart) const
        {
            const double m = rawSlope(c, a, b, atStart);
            if (c != C_FOV && c != C_ROLL && c != C_FOCUS)
            {
                return m;
            }
            double d1 = 0.0, d2 = 0.0;
            if (atStart)
            {
                const KeyPoint& p = mPoints[a];
                if (a == 0 || p.hold > 0.0 || !curvedInto(a, c))
                {
                    return m;   // a rest, or a move into the point that is not smooth: no limit needed
                }
                d1 = (p.v[c] - mPoints[a - 1].v[c]) / p.seconds;
                d2 = (mPoints[b].v[c] - p.v[c]) / mPoints[b].seconds;
            }
            else
            {
                const KeyPoint& p = mPoints[b];
                if (b + 1 == mPoints.size() || p.hold > 0.0 || !curvedInto(b + 1, c))
                {
                    return m;
                }
                d1 = (p.v[c] - mPoints[a].v[c]) / mPoints[b].seconds;
                d2 = (mPoints[b + 1].v[c] - p.v[c]) / mPoints[b + 1].seconds;
            }
            if (d1 * d2 <= 0.0)
            {
                return 0.0;
            }
            const double limit = 3.0 * std::min(std::fabs(d1), std::fabs(d2));
            return std::fabs(m) > limit ? (m > 0.0 ? limit : -limit) : m;
        }

        // Whether component c of the move into point i is on the Hermite curve, so that the curve of the move next to it
        // goes on through the point at the same slope: a smooth move, and for the angle of view, the tilt and the focus
        // distance also a move round a pivot (orbitSegment() moves those three on the curve; its place is an arc, so the
        // place of the move next to it keeps the slope of its own side). Without the orbit here the two moves met at a
        // point with different slopes and the speed of those three jumped there (fix-round97).
        bool curvedInto(size_t i, size_t c) const
        {
            const KeyPoint& p = mPoints[i];
            if (!(p.seconds > 0.0))
            {
                return false;   // a cut
            }
            return p.motion == Motion::Smooth || (p.motion == Motion::Orbit && (c == C_FOV || c == C_ROLL || c == C_FOCUS));
        }

        double rawSlope(size_t c, size_t a, size_t b, bool atStart) const
        {
            const double h = mPoints[b].seconds;
            if (atStart)
            {
                if (a == 0 || mPoints[a].hold > 0.0)
                {
                    return 0.0;
                }
                if (curvedInto(a, c))
                {
                    return (mPoints[b].v[c] - mPoints[a - 1].v[c]) / (h + mPoints[a].seconds);
                }
                return (mPoints[b].v[c] - mPoints[a].v[c]) / h;
            }
            if (b + 1 == mPoints.size() || mPoints[b].hold > 0.0)
            {
                return 0.0;
            }
            if (curvedInto(b + 1, c))
            {
                return (mPoints[b + 1].v[c] - mPoints[a].v[c]) / (h + mPoints[b + 1].seconds);
            }
            return (mPoints[b].v[c] - mPoints[a].v[c]) / h;
        }

        // The squad control quaternion at the start or the end of a -> b: where the direction is
        // heading through the point. At rest, or on a side that is not smooth, the point itself.
        Quat control(size_t a, size_t b, bool atStart) const
        {
            if (atStart)
            {
                const Quat qa = mPoints[a].q;
                if (a == 0 || mPoints[a].hold > 0.0 || !(mPoints[a].seconds > 0.0 && mPoints[a].motion == Motion::Smooth))
                {
                    return qa;
                }
                return qa * expVec(towardNeighbours(qa, mPoints[b].q, mPoints[a - 1].q));
            }
            const Quat qb = mPoints[b].q;
            if (b + 1 == mPoints.size() || mPoints[b].hold > 0.0
                || !(mPoints[b + 1].seconds > 0.0 && mPoints[b + 1].motion == Motion::Smooth))
            {
                return qb;
            }
            return qb * expVec(towardNeighbours(qb, mPoints[b + 1].q, mPoints[a].q));
        }

        // -(log(q^-1 next) + log(q^-1 prev)) / 4
        static Vec3 towardNeighbours(const Quat& q, const Quat& next, const Quat& prev)
        {
            const Vec3 n = logRelative(q, next);
            const Vec3 p = logRelative(q, prev);
            return { -(n.x + p.x) * 0.25, -(n.y + p.y) * 0.25, -(n.z + p.z) * 0.25 };
        }

        // Decides at the start whether the move into point i goes round its pivot, and how far: the short way round
        // from the point before plus the laps. Not when there is no pivot or an end is nearly straight above or below it
        // (no side to go round): the move is then a smooth one.
        void prepareOrbit(size_t i)
        {
            constexpr double MIN_RADIUS = 0.05;   // metres
            constexpr double TWO_PI     = 6.283185307179586;
            KeyPoint& pb = mPoints[i];
            pb.orbitReady = false;
            if (pb.motion != Motion::Orbit || !pb.hasPivot)
            {
                return;
            }
            const KeyPoint& pa = mPoints[i - 1];
            const double ax = pa.v[C_X] - pb.pivot[0], ay = pa.v[C_Y] - pb.pivot[1];
            const double bx = pb.v[C_X] - pb.pivot[0], by = pb.v[C_Y] - pb.pivot[1];
            if (std::sqrt(ax * ax + ay * ay) < MIN_RADIUS || std::sqrt(bx * bx + by * by) < MIN_RADIUS)
            {
                return;
            }
            pb.orbitSweep = std::remainder(std::atan2(by, bx) - std::atan2(ay, ax), TWO_PI) + TWO_PI * pb.orbitTurns;
            pb.orbitReady = true;
        }

        // How far along the curve of a smooth move: where the position comes to rest (the ends of the path, a stay)
        // the direction does too, in the same shape; a rest on one side only gets the matching half.
        double shapedProgress(size_t a, size_t b, double u) const
        {
            const bool rest_a = a == 0 || mPoints[a].hold > 0.0;
            const bool rest_b = b + 1 == mPoints.size() || mPoints[b].hold > 0.0;
            if (rest_a && rest_b)
            {
                return u * u * (3.0 - 2.0 * u);
            }
            if (rest_a)
            {
                return u * u * (2.0 - u);
            }
            if (rest_b)
            {
                const double v = 1.0 - u;
                return 1.0 - v * v * (2.0 - v);
            }
            return u;
        }

        // Component c on the cubic Hermite curve of the move a -> b.
        double hermite(size_t c, size_t a, size_t b, double u) const
        {
            const double h   = mPoints[b].seconds;
            const double u2  = u * u;
            const double u3  = u2 * u;
            const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
            const double h10 = u3 - 2.0 * u2 + u;
            const double h01 = -2.0 * u3 + 3.0 * u2;
            const double h11 = u3 - u2;
            return h00 * mPoints[a].v[c] + h10 * h * slope(c, a, b, true)
                 + h01 * mPoints[b].v[c] + h11 * h * slope(c, a, b, false);
        }

        // The move a -> b that goes round the pivot of b: the place goes along an arc about the pivot (the radius and the
        // height change from a's to b's), the way round that is the short one plus the whole laps of b (orbitTurns), and
        // the camera keeps facing the pivot. False when it cannot (no pivot, or a or b nearly straight above or below
        // it, where there is no side to go round): the move is then a smooth one.
        bool orbitSegment(size_t a, size_t b, double u, Sample& out) const
        {
            constexpr double MIN_RADIUS = 1e-3;   // metres: a pivot that moved onto the camera (prepareOrbit asks for more)
            constexpr double TWO_PI     = 6.283185307179586;
            const KeyPoint& pa = mPoints[a];
            const KeyPoint& pb = mPoints[b];
            if (!pb.hasPivot || !pb.orbitReady)
            {
                return false;
            }
            const double ax = pa.v[C_X] - pb.pivot[0], ay = pa.v[C_Y] - pb.pivot[1];
            const double bx = pb.v[C_X] - pb.pivot[0], by = pb.v[C_Y] - pb.pivot[1];
            const double la = std::sqrt(ax * ax + ay * ay), lb = std::sqrt(bx * bx + by * by);
            if (la < MIN_RADIUS || lb < MIN_RADIUS)
            {
                return false;
            }
            const double s     = shapedProgress(a, b, u);
            const double start = std::atan2(ay, ax);
            // The sweep the move had at the start, and the same angle (a whole number of turns apart) nearest to it: the
            // pivot moving a little never turns the camera the other way round, however close to half a turn it is.
            const double sweep = pb.orbitSweep + std::remainder(std::atan2(by, bx) - start - pb.orbitSweep, TWO_PI);
            const double angle = start + sweep * s;
            const double radius = la + (lb - la) * s;
            out.v[C_X] = pb.pivot[0] + radius * std::cos(angle);
            out.v[C_Y] = pb.pivot[1] + radius * std::sin(angle);
            out.v[C_Z] = pa.v[C_Z] + (pb.v[C_Z] - pa.v[C_Z]) * s;
            for (size_t c : { (size_t)C_FOV, (size_t)C_ROLL, (size_t)C_FOCUS })
            {
                out.v[c] = hermite(c, a, b, u);
            }
            // Facing the pivot from where the camera is, reached from a's own direction over the first quarter of the move:
            // a's offset from facing the pivot where a is, shrunk to nothing over that quarter. A fixed offset, not a blend
            // towards 'look' (which turns fast with a whole turn or more, so the short way to it would flip part-way).
            const Quat look = lookQuat(pb.pivot[0] - out.v[C_X], pb.pivot[1] - out.v[C_Y], pb.pivot[2] - out.v[C_Z]);
            const Quat look0  = lookQuat(pb.pivot[0] - pa.v[C_X], pb.pivot[1] - pa.v[C_Y], pb.pivot[2] - pa.v[C_Z]);
            const Quat offset = conjugate(look0) * pa.q;
            out.q = normalized(look * slerp(Quat{}, offset, 1.0 - smoothstep(u * 4.0)));
            return true;
        }

        void segment(size_t a, size_t b, double u, Sample& out) const
        {
            u = std::clamp(u, 0.0, 1.0);
            const KeyPoint& pa = mPoints[a];
            const KeyPoint& pb = mPoints[b];
            switch (pb.motion)
            {
            case Motion::Linear:
            case Motion::Ease:
            {
                const double e = pb.motion == Motion::Ease ? smoothstep(u) : u;
                for (size_t c = 0; c < COMPONENTS; ++c)
                {
                    out.v[c] = pa.v[c] + (pb.v[c] - pa.v[c]) * e;
                }
                out.q = slerp(pa.q, pb.q, e);
                return;
            }
            case Motion::Orbit:
                if (orbitSegment(a, b, u, out))
                {
                    return;
                }
                [[fallthrough]];
            case Motion::Smooth:
            default:
            {
                for (size_t c = 0; c < COMPONENTS; ++c)
                {
                    out.v[c] = hermite(c, a, b, u);
                }
                const Quat qb = sameSide(pb.q, pa.q);
                const Quat sa = sameSide(control(a, b, true), pa.q);
                const Quat sb = sameSide(control(a, b, false), qb);
                const double shaped = shapedProgress(a, b, u);
                out.q = slerpNoFlip(slerpNoFlip(pa.q, qb, shaped), slerpNoFlip(sa, sb, shaped), 2.0 * shaped * (1.0 - shaped));
                return;
            }
            }
        }

        std::vector<KeyPoint>  mPoints;
        std::vector<double> mArrive;
        double              mTotal = 0.0;
    };
}

#endif // PLUS_CAMERAPATH_PATHMATH_H
