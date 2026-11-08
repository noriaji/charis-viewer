/**
 * @file shakemath.h
 * @brief Camera path: the camera shake (hand-held, walking, running, earthquake) as small offsets of the
 *        camera at a time. Plain C++, so it can be tested on its own (shakemath_test.cpp).
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

#ifndef PLUS_CAMERAPATH_SHAKEMATH_H
#define PLUS_CAMERAPATH_SHAKEMATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "pathmath.h"

// The shake is added to the camera after the path has worked out where it is, so switching it off gives
// the path as it was. Each of the six channels (three offsets of the camera, in its own axes, and three
// small turns) is a sum of a few sine waves of different frequencies whose phases come from the seed:
// smooth, never repeating in a way the eye catches, and the same every time for the same seed and
// time, so a path can be shot again with the same shake. The time is the path's, so a frame that took
// long does not change the shake.
//
//   Handheld   slow, small drift of the hands (about 0.5-1 Hz)
//   Walking    the steps of someone walking: up and down twice a second, sway once a second
//   Running    the same, harder and quicker
//   Quake      fast, rough shaking (5-9 Hz)
//
// amount (0..1) is the size: 0.5 is the size described above, 1 is twice that. speed (0..1) scales the
// frequencies: 0.5 is as described, 0 is half, 1 is twice.
namespace plus::camerapath
{
    enum class ShakeKind
    {
        None,
        Handheld,
        Walking,
        Running,
        Quake
    };

    struct ShakeParams
    {
        ShakeKind kind   = ShakeKind::None;
        double    amount = 0.5;
        double    speed  = 0.5;
        unsigned  seed   = 1;
    };

    struct ShakeOffset
    {
        double right = 0.0, up = 0.0, forward = 0.0;   // metres, along the camera's axes
        double yaw = 0.0, pitch = 0.0, roll = 0.0;     // degrees
    };

    namespace shake_detail
    {
        constexpr double TWO_PI = 6.283185307179586;

        inline uint32_t hash(uint32_t x)
        {
            x ^= x >> 16; x *= 0x7feb352dU;
            x ^= x >> 15; x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }

        // 0..2pi from the seed, the channel and the wave
        inline double phase(unsigned seed, int channel, int wave)
        {
            const uint32_t h = hash(seed * 0x9e3779b1U + (uint32_t)channel * 0x85ebca6bU + (uint32_t)wave * 0xc2b2ae35U + 17U);
            return (h & 0xffffff) / 16777216.0 * TWO_PI;
        }

        // About -1..1, never much beyond: three waves at 1, 1.73 and 2.91 times the base frequency.
        inline double noise(unsigned seed, int channel, double t, double frequency)
        {
            static constexpr double ratio[3]  = { 1.0, 1.73, 2.91 };
            static constexpr double weight[3] = { 0.62, 0.38, 0.22 };
            double sum = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                sum += weight[k] * std::sin(TWO_PI * frequency * ratio[k] * t + phase(seed, channel, k));
            }
            return sum;
        }

        // A step: mostly the base wave, with a little noise on top.
        inline double steady(unsigned seed, int channel, double t, double frequency)
        {
            return 0.8 * std::sin(TWO_PI * frequency * t + phase(seed, channel, 7)) + 0.2 * noise(seed, channel, t, frequency);
        }

        struct Channel
        {
            double amplitude;   // metres or degrees, at amount 0.5
            double frequency;   // Hz, at speed 0.5
            bool   steady;      // a step (true) or a drift (false)
        };

        struct Profile
        {
            Channel channel[6];   // right, up, forward, yaw, pitch, roll
        };

        inline const Profile& profileOf(ShakeKind kind)
        {
            static const Profile handheld = { { { 0.006, 0.70, false }, { 0.008, 0.90, false }, { 0.004, 0.60, false },
                                                { 0.30, 0.55, false },  { 0.25, 0.70, false },  { 0.20, 0.45, false } } };
            static const Profile walking  = { { { 0.012, 0.95, true },  { 0.030, 1.90, true },  { 0.008, 1.90, true },
                                                { 0.25, 0.95, true },   { 0.45, 1.90, true },   { 0.45, 0.95, true } } };
            static const Profile running  = { { { 0.020, 1.40, true },  { 0.060, 2.80, true },  { 0.020, 2.80, true },
                                                { 0.60, 1.40, true },   { 1.00, 2.80, true },   { 0.90, 1.40, true } } };
            static const Profile quake    = { { { 0.050, 6.00, false }, { 0.050, 7.00, false }, { 0.030, 5.00, false },
                                                { 1.20, 8.00, false },  { 1.20, 9.00, false },  { 1.50, 7.00, false } } };
            switch (kind)
            {
            case ShakeKind::Walking: return walking;
            case ShakeKind::Running: return running;
            case ShakeKind::Quake:   return quake;
            default:                 return handheld;
            }
        }

        inline double channelValue(const Channel& c, unsigned seed, int index, double t, double speedScale)
        {
            const double f = c.frequency * speedScale;
            return c.amplitude * (c.steady ? steady(seed, index, t, f) : noise(seed, index, t, f));
        }
    }

    // The camera's offsets at time t (seconds).
    inline ShakeOffset shakeAt(const ShakeParams& params, double t)
    {
        ShakeOffset out;
        if (params.kind == ShakeKind::None)
        {
            return out;
        }
        const double size  = 2.0 * std::clamp(params.amount, 0.0, 1.0);
        const double speed = 0.5 + 1.5 * std::clamp(params.speed, 0.0, 1.0);   // 0.5 .. 2
        if (size <= 0.0)
        {
            return out;
        }
        double v[6];
        const shake_detail::Profile& main = shake_detail::profileOf(params.kind);
        for (int i = 0; i < 6; ++i)
        {
            v[i] = shake_detail::channelValue(main.channel[i], params.seed, i, t, speed);
        }
        // Someone walking or running also has hands: the hand-held drift under the steps.
        if (params.kind == ShakeKind::Walking || params.kind == ShakeKind::Running)
        {
            const double hands = params.kind == ShakeKind::Walking ? 0.5 : 0.7;
            const shake_detail::Profile& base = shake_detail::profileOf(ShakeKind::Handheld);
            for (int i = 0; i < 6; ++i)
            {
                v[i] += hands * shake_detail::channelValue(base.channel[i], params.seed, i + 10, t, speed);
            }
        }
        out.right = v[0] * size; out.up = v[1] * size; out.forward = v[2] * size;
        out.yaw   = v[3] * size; out.pitch = v[4] * size; out.roll = v[5] * size;
        return out;
    }

    // The offsets on the camera of a sample: the place moves along the camera's own axes, the direction turns
    // a little about its up and left axes and the tilt changes by the roll.
    inline void applyShake(Sample& sample, const ShakeOffset& offset)
    {
        constexpr double DEGREES = 0.017453292519943295;
        const Vec3 forward = forwardOf(sample.q);
        const Vec3 left    = rotate(sample.q, { 0.0, 1.0, 0.0 });
        const Vec3 up      = rotate(sample.q, { 0.0, 0.0, 1.0 });
        sample.v[C_X] += -left.x * offset.right + up.x * offset.up + forward.x * offset.forward;
        sample.v[C_Y] += -left.y * offset.right + up.y * offset.up + forward.y * offset.forward;
        sample.v[C_Z] += -left.z * offset.right + up.z * offset.up + forward.z * offset.forward;
        sample.q = normalized(sample.q * axisAngle(0.0, 0.0, 1.0, offset.yaw * DEGREES)
                                       * axisAngle(0.0, 1.0, 0.0, offset.pitch * DEGREES));
        sample.v[C_ROLL] += offset.roll;
    }
}

#endif // PLUS_CAMERAPATH_SHAKEMATH_H
