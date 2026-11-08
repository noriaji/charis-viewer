/**
 * @file slwx.h
 * @brief The parcel's weather: what a [slwx ...] tag in the description of the
 *        parcel the agent stands in makes of the rain, the snow and the wind,
 *        the same for everybody who reads it (research/229, Slipstream's
 *        format). plus/rain asks effective(); the floater (wx-ui) writes the
 *        tag through setParcelTag().
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

#ifndef PLUS_SLWX_SLWX_H
#define PLUS_SLWX_SLWX_H

#include "stdtypes.h"

#include <string>

// Main thread only. The tag's text <-> values (plus/slwx/slwxtag.h) is plain
// and has no such limit.
namespace plus::slwx
{
    struct Effective            // the weather now
    {
        bool  fromParcel;       // the parcel's weather is in effect
        S32   rainLevel;        // 0..3 (as PlusRain: 1 drizzle, 2 rain, 3 downpour)
        F32   rainMm;           // mm/h of rain (blended; the level is taken from it): plus/rain grows the drops with it
        S32   snowLevel;        // 0..3 (as PlusSnow: 1 light, 2 snow, 3 heavy)
        F32   windSpeed;        // m/s (gusts and variation in it)
        F32   windFromDeg;      // 0 north, the way it blows from (wavering in it)
        F32   windBaseSpeed;    // m/s: the tag's own speed, without gusts and variation (for what must not follow them)
        F32   storm;            // 0..1 (the second stage uses it)
    };

    // Every frame. False while the parcel's weather is not in effect (then the
    // settings' values stand); `out` is not written then. The parcel's weather
    // is in effect when PlusWeatherAllowParcel is on, PlusRain and PlusSnow are
    // both 0 (the viewer's own weather wins), and the parcel has a tag (or its
    // fall is still fading out).
    bool effective(Effective& out);

    // PlusWeatherAllowParcel (off by default): take the weather other people
    // wrote into their parcel. Nothing is sent. Off: effective() is always false.
    bool allowParcel();

    // The agent may change the description of the parcel it stands in.
    bool canEditCurrentParcel();

    // Writes `tag` (the whole "[slwx ...]" text) into the description of the
    // parcel the agent stands in: replaces its tag, or adds it at the end.
    // False when not allowed, the parcel is not known yet, or the description
    // would be over 255 bytes. The caller asks the owner first: the tag is
    // public text of the parcel.
    bool setParcelTag(const std::string& tag);

    // Takes the tag out of the description, nothing else.
    bool clearParcelTag();
}

#endif // PLUS_SLWX_SLWX_H
