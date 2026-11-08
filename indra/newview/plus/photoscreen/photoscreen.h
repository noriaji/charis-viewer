/**
 * @file photoscreen.h
 * @brief Photo screen: a whole-window mode for taking pictures (research/218 in the sl-viewer
 *        repository). The frame of it: entering and leaving (the UI that was shown is hidden and
 *        comes back as it was), the 3D world's rectangle (a middle rectangle of the shape of the
 *        picture in the detailed mode, the whole window in the simple one) and the buttons that
 *        switch the two. The panels of the two modes come in later branches, on overlay().
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

#ifndef PLUS_PHOTOSCREEN_PHOTOSCREEN_H
#define PLUS_PHOTOSCREEN_PHOTOSCREEN_H

#include "llrect.h"
#include "stdtypes.h"

class LLLineEditor;
class LLView;

namespace plus::photoscreen
{
    // Whether the photo screen is on. Main thread.
    bool isActive();

    // The simple mode (the world fills the window) is the one that is on; false: the detailed one.
    // Remembered over sessions (PlusPhotoScreenSimple).
    bool isSimple();
    void setSimple(bool simple);

    // Goes in. Does nothing if it is on, or the viewer is not in the world (not logged in, quitting).
    // With the UI hidden by View > Hide UI the hiding is undone first. Returns whether it is on.
    bool enter();

    // Goes out: the UI that was shown comes back (the menu bar, toolbars, bars, floaters, detached
    // windows) and the HUD attachments as they were. Safe to call when it is off, any number of times.
    void leave();

    void toggle();

    // The <plus> hook in LLViewerWindow::updateWorldViewRect(), where the placeholder's rectangle
    // is read: the world's rectangle while the photo screen is on, in UI coordinates (as
    // calcScreenRect(), before the display scale is applied). ui_w, ui_h: the window in the same
    // coordinates. False when off (the official rectangle stands). Not called in mouselook or with
    // the UI off: the world is the whole window there.
    bool worldRect(S32 ui_w, S32 ui_h, LLRect& rect);

    // The frame the detailed mode leaves around the world, in UI pixels for a window ui_w x ui_h (the sizes of
    // computeRect()): the rail and the panel on the left (panel 0 when folded), the panel on the right (0 when
    // folded), the band at the bottom and the one at the top. False in the simple mode, and when the window is
    // too small for a frame (the world is the whole window then).
    struct Margins
    {
        S32  rail = 0, left_panel = 0, right_panel = 0, bottom = 0, top = 0;
        S32  left_panel_full = 0;   // the left panel's width also when folded (it opens over the world then)
        bool wide = true;   // false: the panels at the sides are folded (the narrow window)
    };
    bool margins(S32 ui_w, S32 ui_h, Margins& out);

    // The world's rectangle as it is laid out now (UI coordinates, origin bottom left) while the photo screen
    // is on and shows its frame (not in mouselook). For the composition guide, which draws in the world only.
    bool shownWorldRect(LLRect& rect);

    // How far down from that rectangle's top the photo screen's own parts over the world reach between left and
    // right (UI x coordinates): the detailed mode's chips and histogram frame. 0 when off, in the simple mode or
    // while the frame is not shown. For what goes at the top of the world (plus/focuslock's hint): under them.
    S32 worldTopTaken(S32 left, S32 right);

    // The view the panels of the photo screen go on (a child of the root view over the whole window;
    // it takes no mouse by itself). Null while the photo screen is off.
    LLView* overlay();

    // Where plus/chatconsole's lines may start (UI pixels from the window's bottom): over the simple mode's
    // chat line while it is shown, else 0. The console is drawn in the simple mode only (it has no lines of its
    // own; the detailed mode shows the nearby chat in its world).
    S32 chatConsoleFloor();
    // And where they must stay under: the simple mode's rail and open panel (0: no limit, the console's own top).
    S32 chatConsoleCeiling();

    // The chat line of either mode, on Enter: says its text (trimmed) to the people nearby and empties it. The line
    // keeps the keyboard for the next one unless CloseChatOnReturn is on (as the official chat bar). Main thread.
    void sendChatLine(LLLineEditor* editor);
}

#endif // PLUS_PHOTOSCREEN_PHOTOSCREEN_H
