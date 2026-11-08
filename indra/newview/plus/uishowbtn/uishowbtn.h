/**
 * @file uishowbtn.h
 * @brief "Show UI" button: with the UI hidden, the mouse at the top edge of the window brings
 *        down a button that shows it again (as the full-screen bar of a web browser).
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

#ifndef PLUS_UISHOWBTN_H
#define PLUS_UISHOWBTN_H

#include "stdtypes.h"
#include "indra_constants.h"

namespace plus::uishowbtn
{
    // The <plus> hook in render_ui() (llviewerdisplay.cpp), after the 2D UI and the detached
    // windows, with the 2D setup (gViewerWindow->setup2DRender()); also in the UI-only frames
    // (plus/uiframe). Draws the button while it is down. Does nothing (a few bool reads) while
    // the UI is shown, in mouselook, in a snapshot and before the world. Main thread.
    void draw();

    // The <plus> hook in LLViewerWindow::handleAnyMouseClick(), before the UI gets the click:
    // x, y in the window's UI-scaled coordinates. Returns true when the click is the button's
    // (the left button down and up on it; the up shows the UI) and is not to go on. Main thread.
    bool handleClick(S32 x, S32 y, EMouseClickType clicktype, bool down);
}

#endif // PLUS_UISHOWBTN_H
