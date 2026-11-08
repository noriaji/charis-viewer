/**
 * @file slwxsync.h
 * @brief The parcel weather's shared clock and noise: everybody who reads the
 *        same tag at the same UTC second sees the same gust (research/229
 *        section 3). No server in between: the time is the clock plus the
 *        server's difference, the noise a function of the seed and the time.
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

#ifndef PLUS_SLWX_SLWXSYNC_H
#define PLUS_SLWX_SLWXSYNC_H

#include "slwxtag.h"

#include <cstdint>

namespace plus::slwx::sync
{
    // The pure part (no viewer): what the weather is at `utc` seconds.
    // `seed` is the tag's seed or the default one (seedFor).
    struct Wind
    {
        float speed;        // m/s, 0..40
        float from_deg;     // 0..360, the way it blows from
    };

    // 32-bit mixing of two words (the lattice of the noise, the default seed).
    uint32_t hash32(uint32_t a, uint32_t b);

    // Smooth noise in [-1, 1] with features about `period` seconds long, at
    // `utc` seconds; `stream` picks an independent noise from the same seed.
    float noise(uint32_t seed, uint32_t stream, double utc, double period);

    // The wind at `utc`: the tag's speed varied slowly by `var`, plus gusts
    // (a slow swell to a sharp one or two seconds long) by `gust`, the
    // direction wavering by up to `wdirvar` degrees.
    Wind wind(const tag::Params& params, uint32_t seed, double utc);

    // The fall's rate (mm/h or cm/h) varied by `pvar` around `base`; never negative.
    float precipitation(float base, const tag::Params& params, uint32_t seed, double utc);

    // The snow on the ground `since` made, in m: depth + rate * hours since,
    // 0..DEPTH_MAX (the viewer's own ceiling is applied by whoever draws it).
    // `since` is used only within a year back or a day ahead of `utc`; else
    // the depth alone. No value of the tag can make it NaN or overflow.
    float snowDepth(const tag::Params& params, double utc);

    // The default seed of a parcel with no `seed` key: the region's id
    // (as 4 words) and the parcel's local id.
    uint32_t seedFor(const uint32_t region_words[4], int32_t parcel_local_id);

    // The viewer's side (slwxsync.cpp): UTC seconds by the clock and the
    // server's difference (whole seconds, as the login gave it). While the
    // world is frozen (plus/freeze) it stays at the time it froze.
    double now();
}

#endif // PLUS_SLWX_SLWXSYNC_H
