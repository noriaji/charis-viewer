/**
 * @file background.h
 * @brief A window that lost focus but is still on screen keeps working as in front (plus/background)
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

#ifndef PLUS_BACKGROUND_H
#define PLUS_BACKGROUND_H

// research/303. Upstream counts a window that has no focus as in the background even while it is on
// screen: it sleeps BackgroundYieldTime (40 ms) every frame (about 20 fps, the texture cache paused),
// fixes the discard bias at its top after TextureDiscardBackgroundedTime (60 s) whatever VRAM has
// free, and lowers media to low priority. Working in another window with the viewer beside it
// shows all of that: blurred textures and a jerky picture, for nothing, VRAM having room.
//
// PlusBackgroundKeepVisible (on by default; LLRHI_BACKGROUND_KEEP=0 turns it off whatever the
// setting says): while the window has no focus but can be seen, those three treat it as in
// front (the frame rate is then PlusFpsLimit's and VSync's, as in front). It can be seen when it
// is visible and not minimized and, asked every half second: it is on a display and the session
// has an input desktop (LLRHI::windowOnADisplay, the swapchain's own question: not while the
// computer is locked or a remote desktop is disconnected), the desktop manager has not cloaked it
// (another virtual desktop) and no screen saver runs. A window that cannot be seen is as upstream
// (the bias falls after TextureDiscardMinimizedTime, 1 s; it sleeps). A window covered by other
// windows is told apart (research/303 case C, PlusBackgroundHiddenStop, LLRHI_BACKGROUND_HIDDEN=0 turns it
// off): once a second nine points of its client area are asked of WindowFromPoint; all nine on windows of other
// programs (two checks in a row) means covered, one on the viewer's own window, or anything that cannot be told
// (a point off the screen, a layered or click-through window on top, as overlays are), means seen. A monitor
// switched off by the power plan is not noticed (research/303: a power-setting notification, put off until
// it is seen to be needed). Off: as upstream.
// Laptops on battery: turn it off.
//
// Input, the joystick and the foreground frame count still follow the real focus (another
// application's input is not ours to read; the count is a statistic).
//
// Main thread only. Log (tag Background): one line when a stay without focus begins while the window is
// kept, at most one every 5 s.
namespace plus::background
{
    // True when the window counts as in front although it may have no focus (see above): the setting is on
    // and the window is visible and not minimized. Worked out at each call (two window queries; no state
    // is kept between calls); meant for the places that test "no focus" (they ask only when there is none).
    bool treatAsForeground();

    // True when the window has no focus but its textures are kept as in front: as treatAsForeground, and also when
    // other programs' windows cover it (research/303 case C: the sleep and the media's priority are the official
    // ones then, the discard bias is not fixed: a covered verdict that is wrong for a window that is seen, a window
    // capture or a see-through window, costs only frame rate; VRAM running short is the bias's own business).
    bool keepTextures();
}

#endif // PLUS_BACKGROUND_H
