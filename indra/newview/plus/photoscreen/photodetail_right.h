/**
 * @file photodetail_right.h
 * @brief The right of the detailed photo screen: the camera settings in folding sections (camera,
 *        exposure and tone, color grading, effects, output) over the shutter strip (research/221 13., 14.).
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_RIGHT_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_RIGHT_H

#include "photoscreen.h"

namespace plus::photoscreen::detail
{
    // The sections' rows and the shutter strip, made once for the panel (later calls only lay them out).
    void fillRight();

    // The right column's place for the frame `m`: the whole height of the window at the right (hidden when
    // the sides are folded).
    void layoutRight(const Margins& m);

    // Every quarter of a second while shown: the values of the open sections' rows (each widget written only
    // when its value changed).
    void tickRight();

    // Opens the section (its header shows it open), lays the rows out again and scrolls it into view (the
    // focus button over the world asks for the lens section).
    void openRightSection(int section);
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_RIGHT_H
