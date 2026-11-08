/**
 * @file detachplaces.h
 * @brief Where each detached floater's window was: kept across sessions.
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

#ifndef PLUS_DETACH_DETACHPLACES_H
#define PLUS_DETACH_DETACHPLACES_H

#include "detachwin32.h"

#include <string>
#include <vector>

// The setting PlusDetachedFloaters (settings_plus.xml, an LLSD map by floater
// name): a floater that was shown apart opens in a window of its own again,
// on the monitor and at the place it had, and one that was open apart when
// the session ended opens so after the next login. Only floaters of one
// instance (no key) are kept. Main thread.
namespace plus::detach::places
{
    struct Place
    {
        bool detached = false;      // opens in a window of its own
        bool open = false;          // was open apart when the session ended
        ScreenRect rect;            // the window (desktop pixels); empty: none kept
        std::string monitor;        // the monitor it was on (Monitor::device)
    };

    // The place kept for name (all false and empty when none).
    Place get(const std::string& name);
    void set(const std::string& name, const Place& place);

    // The floaters kept as opening apart (detached).
    const std::vector<std::string>& detachedNames();

    // Where a window kept at place goes now (out): its rect on its monitor,
    // moved inside the monitor's work area if that changed size. False when
    // no rect is kept, its monitor is not plugged in, or the monitor moved
    // (the desktop arranged anew) so that the rect's middle is off it: the
    // caller then opens the window where the floater is.
    bool keptOnScreen(const Place& place, ScreenRect& out);
}

#endif // PLUS_DETACH_DETACHPLACES_H
