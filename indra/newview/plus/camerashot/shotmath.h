/**
 * @file shotmath.h
 * @brief Camera shots: the arithmetic of the moves (no viewer types, so
 *        shotmath_test.cpp builds it alone).
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

#ifndef PLUS_CAMERASHOT_SHOTMATH_H
#define PLUS_CAMERASHOT_SHOTMATH_H

#include <algorithm>
#include <cmath>

namespace plus::camerashot
{
    // Angles are the vertical field of view in radians (the viewer's CameraAngle).
    constexpr double SHOT_MIN_FOV      = 0.15;  // about 8.6 degrees
    constexpr double SHOT_MAX_FOV      = 2.2;   // about 126 degrees
    constexpr double SHOT_MIN_DISTANCE = 0.25;  // metres: the camera never goes closer to its subject

    enum class Ease
    {
        Linear,
        Smooth,   // gentle start, gentle stop (half smoothstep, half even speed: the middle is 1.25 times the even speed)
        SlowIn,   // slow start
        SlowOut   // slow stop
    };

    // 0..1 -> 0..1, reaching 1 at t = 1.
    inline double easeValue(Ease ease, double t)
    {
        t = std::clamp(t, 0.0, 1.0);
        switch (ease)
        {
        // A plain smoothstep is 1.5 times the even speed in the middle and nearly still in the
        // first and last fifth: a 4 second move looks over in 2. Half of it with half even speed.
        case Ease::Smooth:  return 0.5 * t + 0.5 * t * t * (3.0 - 2.0 * t);
        case Ease::SlowIn:  return t * t;
        case Ease::SlowOut: return 1.0 - (1.0 - t) * (1.0 - t);
        default:            return t;
        }
    }

    // The limits a shot can use: SHOT_MIN_FOV..SHOT_MAX_FOV, narrowed to what the camera shows
    // (LLViewerCamera::setDefaultFOV clamps to getMinView()..getMaxView(), which depend on the window's
    // aspect: about 98 degrees at most in a 16:9 window). A shot worked out past them would stop
    // widening part-way and the dolly zoom would no longer keep its subject's size.
    struct FovLimits
    {
        double lo = SHOT_MIN_FOV;
        double hi = SHOT_MAX_FOV;
    };

    inline FovLimits fovLimits(double cameraMin, double cameraMax)
    {
        FovLimits limits;
        if (std::isfinite(cameraMax))
        {
            limits.hi = std::min(SHOT_MAX_FOV, cameraMax);
        }
        if (std::isfinite(cameraMin))
        {
            limits.lo = std::max(SHOT_MIN_FOV, cameraMin);
        }
        limits.lo = std::min(limits.lo, limits.hi); // std::clamp needs lo <= hi
        return limits;
    }

    inline double clampFov(double fov, const FovLimits& limits = FovLimits())
    {
        return std::clamp(fov, limits.lo, limits.hi);
    }

    // What a subject at distance d (field of view fov0 at distance d0) needs to stay
    // the same size in the picture: the width the picture covers there, 2 d tan(fov/2),
    // is kept. The dolly zoom ("vertigo" effect): the subject holds still while the
    // background rushes in or out.
    inline double fovForConstantWidth(double fov0, double d0, double d)
    {
        return 2.0 * std::atan(std::tan(fov0 * 0.5) * d0 / d);
    }

    // The field of view with the picture magnified m times (m > 1: closer).
    inline double fovForMagnification(double fov0, double m)
    {
        return 2.0 * std::atan(std::tan(fov0 * 0.5) / m);
    }

    // The end of a push-in or pull-out: the distance after the move. 'amount' is
    // at least 1: pushing in by 2 halves the distance, pulling out by 2 doubles it.
    inline double dollyEndDistance(double d0, double amount, bool in)
    {
        amount = std::max(1.0, amount);
        const double d1 = in ? d0 / amount : d0 * amount;
        return std::max(d1, SHOT_MIN_DISTANCE);
    }

    // The same for a dolly zoom: also the end distance is pulled back until the field of
    // view it needs (fovForConstantWidth) is inside the limits.
    inline double dollyZoomEndDistance(double fov0, double d0, double amount, bool in,
                                       const FovLimits& limits = FovLimits())
    {
        double d1 = dollyEndDistance(d0, amount, in);
        const double fov1 = fovForConstantWidth(fov0, d0, d1);
        if (fov1 > limits.hi)
        {
            d1 = d0 * std::tan(fov0 * 0.5) / std::tan(limits.hi * 0.5);
        }
        else if (fov1 < limits.lo)
        {
            d1 = d0 * std::tan(fov0 * 0.5) / std::tan(limits.lo * 0.5);
        }
        return std::max(d1, SHOT_MIN_DISTANCE);
    }

    // The magnification of a zoom, limited so the end field of view is inside the limits.
    inline double zoomEndMagnification(double fov0, double amount, bool in,
                                       const FovLimits& limits = FovLimits())
    {
        amount = std::max(1.0, amount);
        double m = in ? amount : 1.0 / amount;
        const double fov1 = fovForMagnification(fov0, m);
        if (fov1 < limits.lo)
        {
            m = std::tan(fov0 * 0.5) / std::tan(limits.lo * 0.5);
        }
        else if (fov1 > limits.hi)
        {
            m = std::tan(fov0 * 0.5) / std::tan(limits.hi * 0.5);
        }
        return m;
    }

    // The magnification part-way (e = eased progress 0..1): geometric, so the picture
    // grows at an even rate and not fast at the end.
    inline double zoomMagnificationAt(double m1, double e)
    {
        return std::pow(m1, e);
    }

    // The orbit: the camera goes round what it looks at, keeping its distance and its height. 'degrees' is how far
    // (a full turn is 360), limited to what a shot can take.
    constexpr double ORBIT_MIN_DEGREES = 30.0;    // the range of the window's slider
    constexpr double ORBIT_MAX_DEGREES = 720.0;
    constexpr double ORBIT_MIN_RADIUS  = 0.25;   // metres: nearer to straight above or below the camera has no side to go round

    // Whether a camera at 'distance' from its subject with 'horizontal' metres of that distance sideways has a side to go
    // round: not nearly straight above or below (within 5 degrees of the vertical, or nearer than ORBIT_MIN_RADIUS).
    // Close to the vertical the picture would only turn on the spot.
    inline bool orbitHasSide(double horizontal, double distance)
    {
        return horizontal >= std::max(ORBIT_MIN_RADIUS, distance * 0.08715574274765817);   // sin 5 degrees
    }

    inline double orbitDegrees(double degrees)
    {
        return std::clamp(degrees, ORBIT_MIN_DEGREES, ORBIT_MAX_DEGREES);
    }

    // The angle about the vertical axis at the eased progress e (0..1): positive turns counter-clockwise seen from
    // above, which takes a camera to its own right (right = true).
    inline double orbitRadians(double degrees, bool right, double e)
    {
        return (right ? 1.0 : -1.0) * orbitDegrees(degrees) * 0.017453292519943295 * std::clamp(e, 0.0, 1.0);
    }

    // The horizontal part (x, y) of a vector turned about the vertical axis; z stays.
    inline void rotateAboutZ(double x, double y, double radians, double& out_x, double& out_y)
    {
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        out_x = x * c - y * s;
        out_y = x * s + y * c;
    }
}

#endif // PLUS_CAMERASHOT_SHOTMATH_H
