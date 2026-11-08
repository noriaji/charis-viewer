/**
 * @file slwxparcel.h
 * @brief Watches the description of the parcel the agent is in for a
 *        [slwx ...] tag, and writes one back for a parcel the agent may
 *        edit (research/229 sections 4 and 7).
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

#ifndef PLUS_SLWX_SLWXPARCEL_H
#define PLUS_SLWX_SLWXPARCEL_H

#include "slwxtag.h"

#include <cstdint>
#include <string>

namespace plus::slwx::parcel
{
    // What the parcel asks for now. `generation` goes up whenever it changes
    // (a new tag, the tag gone, another parcel); `immediate` says the change
    // should be taken at once (after a teleport) instead of blended over `trans`.
    struct Target
    {
        bool active = false;           // a tag is there and it is not "off"
        tag::Params params;
        uint32_t seed = 0;             // the tag's seed, or the parcel's default one
        uint32_t generation = 0;
        bool immediate = false;
        int bad_items = 0;             // items of the tag that were broken (dropped)
    };

    // Every frame while the parcel weather is allowed (main thread): looks at
    // the agent's parcel and reads its tag when it changed. After a region
    // crossing the previous target is kept (up to 10 s) until the new
    // region's parcel is known; after a teleport it is dropped at once.
    void update();

    const Target& target();

    // Forgets everything (the weather turned off, or own weather took over).
    void reset();

    // The parcel the agent stands in is known and the agent may change its
    // description (owner, or a group role with the right to; the server
    // decides in the end).
    bool canEdit();

    // Puts `tag` into the description (replacing the first tag, or added at the
    // end), or takes the tag out (clearTag). Sends the parcel update. False
    // when not allowed, the parcel is unknown, or the text would be longer than 255 bytes.
    bool setTag(const std::string& tag);
    bool clearTag();
}

#endif // PLUS_SLWX_SLWXPARCEL_H
