/**
 * @file shortcutbar.h
 * @brief Menu shortcut bar (research/225 in the sl-viewer repository): what other features ask of it.
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

#ifndef PLUS_SHORTCUTBAR_H
#define PLUS_SHORTCUTBAR_H

namespace plus::shortcutbar
{
    // While on, the bar is not shown whatever the setting and View > Hide UI say (the photo screen
    // turns it on for as long as it is in). Turning it off shows the bar again only if the setting
    // and the UI would show it. Takes effect at once; safe before the bar is made and after the UI is gone.
    void setSuppressed(bool on);
}

#endif // PLUS_SHORTCUTBAR_H
