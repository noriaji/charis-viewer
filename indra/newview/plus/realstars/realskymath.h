/**
 * @file realskymath.h
 * @brief Where the real stars stand: time to sidereal time, precession, equatorial to horizon
 *        coordinates (research/177-real-sky-stars.md §1, §2.1). Plain C++, F64, header only.
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

#ifndef PLUS_REALSTARS_REALSKYMATH_H
#define PLUS_REALSTARS_REALSKYMATH_H

// Formulas from J. Meeus, "Astronomical Algorithms", 2nd ed.: 12.4 (mean sidereal time),
// 13 (equatorial to horizon), 21.2 (rigorous precession), 25 (the Sun, low accuracy).
// Left out, all under a pixel or so on screen: nutation (<= 17"), aberration (<= 20"),
// proper motion, refraction, UT1 - UTC (< 1 s).
//
// Frames, all right-handed:
//   J2000 / of date: x to the vernal equinox, z to the north celestial pole.
//   horizon (ENU):   x east, y north, z up (the SL agent frame; whether LLSettingsSky's sun
//                    direction is the same ENU is checked at stage 2, research/177 §1.1).
// Time is F64 throughout: a Julian day needs more digits than F32 has.

#include <cmath>

namespace plus
{
namespace realstars
{
namespace math
{
    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;          // degrees to radians
    constexpr double ARCSEC = DEG / 3600.0;     // arc seconds to radians
    constexpr double JD_UNIX_EPOCH = 2440587.5; // 1970-01-01T00:00:00Z
    constexpr double JD_J2000 = 2451545.0;      // 2000-01-01T12:00:00 (TT; taken as UT here)

    struct Vec3d
    {
        double x, y, z;
    };

    // Row-major: (M v)_i = sum_j m[i][j] v_j.
    struct Mat3d
    {
        double m[3][3];
    };

    inline Vec3d mul(const Mat3d& a, const Vec3d& v)
    {
        return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
                 a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
                 a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
    }

    inline Mat3d mul(const Mat3d& a, const Mat3d& b)
    {
        Mat3d r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        return r;
    }

    inline Mat3d transpose(const Mat3d& a)
    {
        Mat3d r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a.m[j][i];
        return r;
    }

    // Active rotations (they turn a vector by +angle, counter-clockwise seen from the axis's tip).
    inline Mat3d rotZ(double rad)
    {
        const double c = std::cos(rad), s = std::sin(rad);
        return { { { c, -s, 0.0 }, { s, c, 0.0 }, { 0.0, 0.0, 1.0 } } };
    }

    inline Mat3d rotY(double rad)
    {
        const double c = std::cos(rad), s = std::sin(rad);
        return { { { c, 0.0, s }, { 0.0, 1.0, 0.0 }, { -s, 0.0, c } } };
    }

    // An angle in degrees to [0, 360).
    inline double wrapDeg360(double deg)
    {
        double r = std::fmod(deg, 360.0);
        if (r < 0.0)
            r += 360.0;
        return r >= 360.0 ? 0.0 : r;
    }

    // An angle in degrees to [-180, 180).
    inline double wrapDeg180(double deg)
    {
        return wrapDeg360(deg + 180.0) - 180.0;
    }

    // ---- time -----------------------------------------------------------------------------

    // The Julian day of a Unix time (seconds since 1970-01-01T00:00:00Z, UTC).
    inline double julianDayFromUnix(double unixSeconds)
    {
        return JD_UNIX_EPOCH + unixSeconds / 86400.0;
    }

    // Julian centuries since J2000.0.
    inline double centuriesSinceJ2000(double jd)
    {
        return (jd - JD_J2000) / 36525.0;
    }

    // Greenwich mean sidereal time in degrees, [0, 360) (Meeus 12.4).
    inline double gmstDeg(double jd)
    {
        const double d = jd - JD_J2000;
        const double t = d / 36525.0;
        return wrapDeg360(280.46061837 + 360.98564736629 * d + 0.000387933 * t * t - t * t * t / 38710000.0);
    }

    // Local mean sidereal time in degrees, [0, 360). Longitude east positive.
    inline double lstDeg(double jd, double lonDegEast)
    {
        return wrapDeg360(gmstDeg(jd) + lonDegEast);
    }

    // ---- directions -----------------------------------------------------------------------

    inline Vec3d unitFromRaDec(double raDeg, double decDeg)
    {
        const double a = raDeg * DEG, d = decDeg * DEG;
        return { std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d) };
    }

    // Right ascension [0, 360) and declination of a direction (need not be unit length).
    inline void raDecFromVec(const Vec3d& v, double& raDeg, double& decDeg)
    {
        raDeg = wrapDeg360(std::atan2(v.y, v.x) / DEG);
        decDeg = std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)) / DEG;
    }

    struct AltAz
    {
        double altDeg; // above the horizon
        double azDeg;  // from north through east, [0, 360)
    };

    // Altitude and azimuth of a direction in the horizon frame (ENU).
    inline AltAz altAzFromEnu(const Vec3d& v)
    {
        AltAz r;
        r.altDeg = std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)) / DEG;
        r.azDeg = wrapDeg360(std::atan2(v.x, v.y) / DEG);
        return r;
    }

    // research/177 §1.3 written out for one star: hour angle H, declination, latitude.
    inline AltAz altAzFromHourAngle(double hDeg, double decDeg, double latDeg)
    {
        const double h = hDeg * DEG, d = decDeg * DEG, p = latDeg * DEG;
        const Vec3d enu = { -std::cos(d) * std::sin(h),
                            std::cos(p) * std::sin(d) - std::sin(p) * std::cos(d) * std::cos(h),
                            std::sin(p) * std::sin(d) + std::cos(p) * std::cos(d) * std::cos(h) };
        return altAzFromEnu(enu);
    }

    // ---- the rotations --------------------------------------------------------------------

    // Mean equator and equinox of J2000.0 to those of the date (Meeus 21.2 and 21.3, the
    // rigorous method with the epoch J2000.0): alpha' = alpha + zeta, a turn of -theta about
    // y, then + z about the pole.
    inline Mat3d precessionMatrix(double jd)
    {
        const double t = centuriesSinceJ2000(jd);
        const double zeta  = (2306.2181 * t + 0.30188 * t * t + 0.017998 * t * t * t) * ARCSEC;
        const double z     = (2306.2181 * t + 1.09468 * t * t + 0.018203 * t * t * t) * ARCSEC;
        const double theta = (2004.3109 * t - 0.42665 * t * t - 0.041833 * t * t * t) * ARCSEC;
        return mul(rotZ(z), mul(rotY(-theta), rotZ(zeta)));
    }

    // Equatorial of the date to horizon (ENU) for local sidereal time L and latitude phi
    // (research/177 §1.3, the matrix R).
    inline Mat3d horizonMatrix(double lstDegrees, double latDeg)
    {
        const double l = lstDegrees * DEG, p = latDeg * DEG;
        const double sl = std::sin(l), cl = std::cos(l), sp = std::sin(p), cp = std::cos(p);
        return { { { -sl, cl, 0.0 },
                   { -sp * cl, -sp * sl, cp },
                   { cp * cl, cp * sl, sp } } };
    }

    // J2000 direction to horizon (ENU): R(LST, phi) * P(date). The one matrix the stars are drawn with.
    inline Mat3d skyMatrix(double jd, double lonDegEast, double latDeg)
    {
        return mul(horizonMatrix(lstDeg(jd, lonDegEast), latDeg), precessionMatrix(jd));
    }

    // ---- the Sun (for "follow SL's sun", research/177 §2.1 (b)) ---------------------------

    // The Sun's apparent right ascension and declination of the date, in degrees (Meeus 25,
    // low accuracy, about 0.01 degree).
    inline void sunRaDecDeg(double jd, double& raDeg, double& decDeg)
    {
        const double t = centuriesSinceJ2000(jd);
        const double l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t;
        const double m = (357.52911 + 35999.05029 * t - 0.0001537 * t * t) * DEG;
        const double c = (1.914602 - 0.004817 * t - 0.000014 * t * t) * std::sin(m)
                       + (0.019993 - 0.000101 * t) * std::sin(2.0 * m)
                       + 0.000289 * std::sin(3.0 * m);
        const double omega = (125.04 - 1934.136 * t) * DEG;
        const double lambda = (l0 + c - 0.00569 - 0.00478 * std::sin(omega)) * DEG;
        const double eps0 = 23.0 + 26.0 / 60.0 + 21.448 / 3600.0
                          - (46.8150 * t + 0.00059 * t * t - 0.001813 * t * t * t) / 3600.0;
        const double eps = (eps0 + 0.00256 * std::cos(omega)) * DEG;
        raDeg = wrapDeg360(std::atan2(std::cos(eps) * std::sin(lambda), std::cos(lambda)) / DEG);
        decDeg = std::asin(std::sin(eps) * std::sin(lambda)) / DEG;
    }

    // The hour angle (degrees, [-180, 180)) of a direction given in the horizon frame (ENU)
    // at latitude phi: H = atan2(-E, cos(phi) U - sin(phi) N). With the Sun's right ascension
    // of the date, LST = H + alpha (research/177 §2.1 (b)).
    inline double hourAngleFromEnuDeg(const Vec3d& enu, double latDeg)
    {
        const double p = latDeg * DEG;
        return wrapDeg180(std::atan2(-enu.x, std::cos(p) * enu.z - std::sin(p) * enu.y) / DEG);
    }

    // research/177 §2.1 (b): J2000 to horizon (ENU) with the sky turned so that the Sun of the
    // date jd stands at the hour angle SL's sun (sunEnu, in the agent frame: x east, y north,
    // z up) has at latitude phi. The longitude does not enter: the turn comes from the sun.
    inline Mat3d skyMatrixFollowingSun(double jd, const Vec3d& sunEnu, double latDeg)
    {
        double ra = 0.0, dec = 0.0;
        sunRaDecDeg(jd, ra, dec);
        const double lst = wrapDeg360(hourAngleFromEnuDeg(sunEnu, latDeg) + ra);
        return mul(horizonMatrix(lst, latDeg), precessionMatrix(jd));
    }

    // ---- the look of a catalogue star -----------------------------------------------------

    // The surface temperature (kelvin) of a star of the colour index B-V (Ballesteros 2012).
    inline double temperatureOfBV(double bv)
    {
        const double b = bv < -0.4 ? -0.4 : (bv > 2.0 ? 2.0 : bv);
        return 4600.0 * (1.0 / (0.92 * b + 1.7) + 1.0 / (0.92 * b + 0.62));
    }

    // Altitude (degrees) of the sun's direction in the agent frame, from its z (need not be unit).
    inline double altitudeDegOfEnu(const Vec3d& v)
    {
        return std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y)) / DEG;
    }
} // namespace math
} // namespace realstars
} // namespace plus

#endif // PLUS_REALSTARS_REALSKYMATH_H
