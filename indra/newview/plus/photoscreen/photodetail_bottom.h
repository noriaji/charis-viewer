/**
 * @file photodetail_bottom.h
 * @brief The bottom of the detailed photo screen: the strip of presets (camera positions, poses, scenes,
 *        my presets) as cards with small pictures.
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_BOTTOM_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_BOTTOM_H

#include "photoscreen.h"

namespace plus::photoscreen::detail
{
    // The cards of the preset tab chosen, collected and shown again.
    void rebuildCards();

    // The strip's buttons (once, when the panels are made).
    void bindBottomPanel();

    // The strip's place for the frame `m`.
    void layoutBottom(const Margins& m);

    // A quarter of a second: the cards made again when what they show changed.
    void tickBottom();
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_BOTTOM_H
