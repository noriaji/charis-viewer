/**
 * @file floatersnap.h
 * @brief Windows that touch move together, snapping to the near windows only (plus/floatersnap, research/387).
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

#ifndef PLUS_FLOATERSNAP_H
#define PLUS_FLOATERSNAP_H

class LLFloater;
class LLMouseHandler;

namespace plus::floatersnap
{
    // A function to call for every window a title bar drag moved (the dragged one, the others of its group, and the dependent
    // windows snapped to them, which the official code moves with their parent; each once per step; main thread, after the
    // window was moved, at every step of the drag). For a view that belongs to a window and must close or follow it when it
    // moves: the popups that go out of a window (plus/rmlui, research/386 rmlui-popup). Add it at the start of the program
    // (a feature's init or onUIReady): listeners are kept for good (there is no way to take one away) and have no context
    // pointer, so a feature adds ONE static function that finds its own window from the one it is told about. It must not
    // move or close that window, or drag again (it may close its own view). Windows moved by anything but a drag (the
    // official layout, a program's own setShape) are not told.
    void addMovedListener(void (*listener)(LLFloater* moved));

    // Whether captor, the mouse's captor, is the title bar of a drag that takes the windows touching the dragged one
    // along (the group is made when the title bar is pressed, and kept until it is let go). plus/detach reads it: such a
    // drag stays in the main window, as with Ctrl held. False for any other captor and when the feature is off.
    bool isGroupDrag(const LLMouseHandler* captor);
}

#endif // PLUS_FLOATERSNAP_H
