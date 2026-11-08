/**
 * @file detach.h
 * @brief Detached floaters: a floater shown in a top-level window of its own,
 *        on any monitor, outside the main window (research/87, steps S0, S1).
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

#ifndef PLUS_DETACH_H
#define PLUS_DETACH_H

#include "llcoord.h"
#include "stdtypes.h"

class LLView;

// The <plus> hooks of the official viewer into the detached windows. Each is
// a quick test (or nothing) while no floater is detached. Main thread.
namespace plus::detach
{
    // render_ui() (llviewerdisplay.cpp) after the main window's 2D UI, drawn
    // or hidden, and plus/uiframe's UI-only frames: draws each detached
    // window's UI and hands it to the renderer, which presents it with this
    // frame.
    void drawWindows();

    // LLViewerWindow: whether a point in UI coordinates (the root view's, as
    // the main window's mouse handling scales them) is on a detached window.
    // Such points lie outside the main window.
    bool containsPoint(S32 x, S32 y);

    // The same for a point in the main window's client pixels (top-left
    // origin, LLWindow::getCursorPosition()).
    bool containsWindowPoint(const LLCoordWindow& point);

    // containsPoint(), or a mouse button went down on a detached window and
    // is still held, or a view on one has the mouse capture (a floater taken
    // apart while dragged out of the main window): the mouse point is used as it is, not moved to the main
    // window's edge (LLViewerWindow::saveLastMouse()).
    bool keepsMousePoint(S32 x, S32 y);

    // LLViewerWindow::updateUI(), building the views the mouse is over (their
    // onMouseEnter / onMouseLeave): whether view's screen bounding rect holds
    // the point, decided at a point on a detached window as the hit test
    // decides (the root view and main_view let it through to the detached
    // floaters and to main_view's menu, tooltip and popup layers; the main
    // window's other views never get it). Elsewhere the official test.
    bool hoverContains(const LLView* view, S32 x, S32 y);
}

#endif // PLUS_DETACH_H
