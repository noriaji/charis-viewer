/**
 * @file avatarattach.h
 * @brief The attachments of an avatar, listed in a window (the avatar's context menu).
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

// "Attachments..." in the context menu of an avatar (others' and the agent's, on the avatar
// and on one of its attachments) opens the window plus_avatar_attachments: one row an attached
// object (the root of each linkset): the attachment point, the object's name, its creator, the
// prims of the linkset and the distance. The window keeps the avatar's id, never a pointer,
// and finds the avatar again at each refresh (once a second while open, at once when a name
// arrives); an avatar out of view or gone is told so.
//
// The names come the way "Inspect" gets them: the root is selected on the simulator
// (ObjectSelect), which sends its ObjectProperties (name, creator, ...), handled by LLSelectMgr
// and passed here by the one hook in LLSelectMgr::processObjectProperties(). The viewer's own
// selection (LLSelectMgr, the highlight, the agent pointing at it) is not touched: each object
// is deselected again (ObjectDeselect) when its answer comes, when it times out, and when the
// window closes or shows another avatar - unless the user has selected it in the meantime.
// RequestObjectPropertiesFamily (the hover tip's way) is not used: the simulator does not
// answer it for many attachments of others. A few at a time; no answer in a few seconds: asked
// again, at most three times, then "not available" (the "Refresh" button asks once more).
// Nothing is asked while the window is closed. The agent's own attachments are named from the
// inventory without asking.
//
// Main thread only.

#ifndef PLUS_AVATARATTACH_AVATARATTACH_H
#define PLUS_AVATARATTACH_AVATARATTACH_H

#include <string>

#include "lluuid.h"

namespace plus::avatarattach
{
    // An ObjectProperties answer (the hook in LLSelectMgr::processObjectProperties()). True when
    // the window had selected this object on the simulator (so it is not the viewer's selection).
    bool onObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name);
}

#endif // PLUS_AVATARATTACH_AVATARATTACH_H
