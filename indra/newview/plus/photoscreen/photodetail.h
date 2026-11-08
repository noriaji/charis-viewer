/**
 * @file photodetail.h
 * @brief The detailed mode of the photo screen: the panels around the world (research/218 in the
 *        sl-viewer repository). The frame (photoscreen.h) calls place() when its layout changes.
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_H

#include "llrect.h"
#include "stdtypes.h"

class LLView;

namespace plus::photoscreen::detail
{
    // Puts the panels of the detailed mode on the photo screen's overlay (made at the first call and kept),
    // around the world's rectangle (UI coordinates, origin bottom left) of a window ui_w x ui_h; or hides them
    // when `simple` (the simple mode, or a window too small for a frame). Called by the photo screen's layout
    // whenever the window, the mode or the shape of the picture changes: not every frame.
    void place(LLView* overlay, const LLRect& world, S32 ui_w, S32 ui_h, bool simple);

    // How far down from the world's top the detailed mode's own parts over the world reach (UI pixels) between
    // left and right (UI x coordinates): the chips' row at the top left, the histogram's frame at the top right.
    // 0 while its panels are not shown (the simple mode, a window too small for a frame). photodetail_center.cpp.
    S32 topTaken(S32 left, S32 right);
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_H
