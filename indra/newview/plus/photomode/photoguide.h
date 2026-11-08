/**
 * @file photoguide.h
 * @brief Photo tools: the composition guide (grid lines and the frame of the photo) drawn over the
 *        screen, not into the pictures.
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

#ifndef PLUS_PHOTOMODE_PHOTOGUIDE_H
#define PLUS_PHOTOMODE_PHOTOGUIDE_H

#include <string>

namespace plus::photomode
{
    // The kinds of PlusPhotoGuideKind.
    enum class GuideKind
    {
        None = 0, // only the frame of the photo, when its shape differs from the window's
        Thirds,
        Golden,   // the phi grid: lines at 38.2 % and 61.8 %
        Cross,    // the centre lines
        Diagonals,
        Grid4,
        Grid6,
        Horizon,  // a line through the centre that follows the tilt of the camera (plus/camroll)
        Count
    };

    // Puts the guide's view in the window (once the UI exists, from the feature's onUIReady). The view
    // sits above the 3D picture and under every window and toolbar, takes no mouse, draws only while
    // PlusPhotoGuideOn is set and never while a snapshot is rendered, so the lines are not in a picture
    // (a one-press photo has no UI at all; the snapshot window's "show UI" is left out by the guard).
    void attachGuide();

    // A short line in the middle of the screen for a moment (what a key of photokeys.cpp did), drawn by
    // the guide's view whether the guide is on or not, and never into a picture.
    void showGuideMessage(const std::string& text);

    // A label that stays at the top of the screen as long as it is set (the compare of the photo tools:
    // "before your changes"); empty takes it away. Drawn like the message: never into a picture.
    void setGuideBadge(const std::string& text);
}

#endif // PLUS_PHOTOMODE_PHOTOGUIDE_H
