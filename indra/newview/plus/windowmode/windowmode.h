/**
 * @file windowmode.h
 * @brief Borderless fullscreen (plus/windowmode)
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

#ifndef PLUS_WINDOWMODE_H
#define PLUS_WINDOWMODE_H

namespace plus::windowmode
{
    // The old exclusive FullScreen setting (display mode change, start only) becomes PlusBorderless.
    // Called before the window is made.
    void migrateExclusiveFullscreen();

    // The window has no frame now (the saved size and position are then not the window's own).
    bool isBorderless();
}

#endif // PLUS_WINDOWMODE_H
