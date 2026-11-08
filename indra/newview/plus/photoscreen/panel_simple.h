/**
 * @file panel_simple.h
 * @brief The simple view of the photo screen (research/218): the world fills the window and a few
 *        see-through panels lie on it. Choices only, no numbers: who to focus on, the mood, the framing, the
 *        sky and water, the camera spot, the thirds lines, a line to type chat in.
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

#ifndef PLUS_PHOTOSCREEN_PANEL_SIMPLE_H
#define PLUS_PHOTOSCREEN_PANEL_SIMPLE_H

#include "llrect.h"
#include "stdtypes.h"

class LLView;

namespace plus::photoscreen::simple
{
    // Called by photoscreen.cpp whenever it lays its views out (entering, a change of the window or of the
    // mode, never every frame): makes the simple view on the first call, then shows it while the simple mode
    // is on (and hides it in the detailed one) and puts its parts in the window of w x h (UI coordinates).
    // overlay: the photo screen's overlay. Main thread.
    void onLayout(LLView* overlay, S32 w, S32 h, bool simple);

    // The top of the line to type chat in, and a gap (UI pixels from the window's bottom), while the simple view
    // is shown; 0 otherwise. The chat console's lines start over it (plus/chatconsole).
    S32 chatLineTop();
    // Under the rail and the open tab's panel, a gap left (UI pixels from the window's bottom), while the simple
    // view is shown; 0 otherwise (no limit). The chat console's lines stay under it.
    S32 chatConsoleTop();
}

#endif // PLUS_PHOTOSCREEN_PANEL_SIMPLE_H
