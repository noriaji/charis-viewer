/**
 * @file slwxsync.cpp
 * @brief The parcel weather's shared clock and noise (research/229 section 3).
 *
 * The same tag read at the same UTC second gives the same wind and the same
 * rate in every Charis viewer: the noise is a function of the seed, a stream
 * number and the time only, with no state. (Slipstream's own noise is a
 * different function: the tag is the same, a gust's exact moment is not
 * across the two viewers.)
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

#include "llviewerprecompiledheaders.h"

#include "slwxsync.h"

#include "plus/freeze/freeze.h"

#include "lltimer.h"

#include <algorithm>
#include <cmath>

namespace plus::slwx::sync
{
    namespace
    {
        // Noise streams: independent noises from one seed.
        constexpr uint32_t STREAM_SLOW = 1;     // the wind's slow variation
        constexpr uint32_t STREAM_SWELL = 2;    // a gust's slow swell
        constexpr uint32_t STREAM_SHARP = 3;    // the sharp gusts
        constexpr uint32_t STREAM_DIR = 4;      // the direction's wavering
        constexpr uint32_t STREAM_PRECIP = 5;   // the fall's rate

        constexpr double YEAR_SECONDS = 365.0 * 86400.0;
        constexpr double DAY_SECONDS = 86400.0;

        // A lattice value in [-1, 1].
        float lattice(uint32_t seed, uint32_t stream, int64_t i)
        {
            const uint32_t h = hash32(hash32(seed, stream), hash32((uint32_t)(i & 0xffffffff), (uint32_t)((uint64_t)i >> 32)));
            return (float)(h >> 8) * (2.f / 16777215.f) - 1.f;
        }

        // Three octaves of the noise, in [-1, 1].
        float fbm(uint32_t seed, uint32_t stream, double utc, double period)
        {
            return 0.57f * noise(seed, stream, utc, period)
                 + 0.29f * noise(seed, stream + 16, utc, period / 2.6)
                 + 0.14f * noise(seed, stream + 32, utc, period / 6.8);
        }
    }

    uint32_t hash32(uint32_t a, uint32_t b)
    {
        uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u + (a << 6) + (a >> 2));
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
        return h;
    }

    float noise(uint32_t seed, uint32_t stream, double utc, double period)
    {
        if (!(period > 0.0) || !std::isfinite(utc))
        {
            return 0.f;
        }
        const double x = utc / period;
        const double base = std::floor(x);
        const float f = (float)(x - base);
        const float s = f * f * (3.f - 2.f * f);
        const int64_t i = (int64_t)base;
        const float a = lattice(seed, stream, i);
        const float b = lattice(seed, stream, i + 1);
        return a + (b - a) * s;
    }

    Wind wind(const tag::Params& params, uint32_t seed, double utc)
    {
        const float base = params.wind_speed;
        float speed = base * (1.f + 0.7f * params.var * fbm(seed, STREAM_SLOW, utc, 40.0));
        // Gusts: a swell over some seconds, and sharp ones a second or two long
        // above a threshold, so that they come now and then.
        const float swell = std::max(0.f, fbm(seed, STREAM_SWELL, utc, 25.0));
        const float edge = std::max(0.f, noise(seed, STREAM_SHARP, utc, 3.0) - 0.45f) / 0.55f;
        speed += base * params.gust * 0.6f * (0.6f * swell + edge * edge * 1.6f);
        speed = std::min(std::max(speed, 0.f), tag::WIND_SPEED_MAX);

        float from = params.wind_from + params.wdirvar * fbm(seed, STREAM_DIR, utc, 15.0);
        from = std::fmod(from, 360.f);
        if (from < 0.f)
        {
            from += 360.f;
        }
        return Wind{ speed, from };
    }

    float precipitation(float base, const tag::Params& params, uint32_t seed, double utc)
    {
        if (!(base > 0.f))
        {
            return 0.f;
        }
        return std::max(0.f, base * (1.f + 0.8f * params.pvar * fbm(seed, STREAM_PRECIP, utc, 60.0)));
    }

    float snowDepth(const tag::Params& params, double utc)
    {
        if (!params.has_depth)
        {
            return 0.f;
        }
        double depth = params.depth;
        if (params.has_since && std::isfinite(utc))
        {
            const double ago = utc - (double)params.since;
            if (ago >= -DAY_SECONDS && ago <= YEAR_SECONDS)
            {
                depth += (double)params.rate * ago / 3600.0;
            }
        }
        return (float)std::min(std::max(depth, 0.0), (double)tag::DEPTH_MAX);
    }

    uint32_t seedFor(const uint32_t region_words[4], int32_t parcel_local_id)
    {
        return hash32(hash32(hash32(region_words[0], region_words[1]), hash32(region_words[2], region_words[3])),
                      (uint32_t)parcel_local_id);
    }

    double now()
    {
        // Frozen: the UTC of the moment the world stopped (the clock minus the seconds since).
        const double t = (double)LLTimer::getTotalSeconds() + (double)gUTCOffset;
        return plus::freeze::isFrozen() ? t - (double)plus::freeze::frozenSeconds() : t;
    }
}
