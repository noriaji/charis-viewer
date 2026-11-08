/**
 * @file derender.h
 * @brief Derender: objects and avatars hidden from the right-click menu until the next teleport
 *        (from Firestorm's temporary derender).
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

#ifndef PLUS_DERENDER_H
#define PLUS_DERENDER_H

#include "stdtypes.h"

#include <cstddef>

class LLMessageSystem;
class LLUUID;
class LLViewerObject;
class LLViewerRegion;

// "Derender" in the context menus of an object, another avatar and another avatar's attachment
// (research/251, from Firestorm's derenderObject() and LLViewerObjectList::mDerendered): the
// selection is killed here only, as a kill from the simulator would be, and its ids (the root and
// every prim and attachment under it) are kept, so that updates the simulator sends again do not
// make it again. Nothing is sent but the usual deselect of the right-click's selection (and, when
// they are let go, the requests that bring them back). The ids are dropped when a teleport lands in
// another region (begun by the viewer or the simulator; the agent's region-changed signal, no hook):
// a region the viewer connects to anew sends its objects, and the prims of a region that stays
// connected (a neighbour of the new one) are asked for again, as cache misses; a teleport within the
// region, a failed or a cancelled one keeps them. The ids of a region the viewer lets go (a walk away
// from it) go with it (LLWorld's region-removed signal). A fresh login starts with none.
// Left out: the agent's avatar and its own attachments and HUDs (their death takes them out of the
// outfit), an object the agent sits on, a control avatar on its own. Main thread only.

namespace plus::derender
{
    namespace detail
    {
        // The number of hidden ids: 0 makes the hooks one comparison.
        extern std::size_t sHiddenCount;
        bool contains(const LLUUID& id);
        void onOrphaned(LLViewerObject* child, U32 parent_id, U32 ip, U32 port);
        bool waitForHiddenSeat(LLViewerObject* avatar, U32 parent_id, LLMessageSystem* msg);
    }

    // The <plus> hooks in LLViewerObjectList::processObjectUpdate() (before createObject()) and
    // processObjectUpdateFromCache(): true when the object is hidden and is not to be made.
    inline bool isHidden(const LLUUID& id)
    {
        return detail::sHiddenCount != 0 && detail::contains(id);
    }

    // processObjectUpdateFromCache() refused the cache entry local_id of region for the hidden id:
    // the entry is taken out of the region's cache on the next idle (not while the region walks its
    // lists), so it is not tried again every frame. Its id is checked again then.
    void forgetCacheEntry(LLViewerRegion* region, U32 local_id, const LLUUID& id);

    // The <plus> hook at the end of LLViewerObjectList::orphanize(): child came with a parent the
    // viewer does not have. When that is the agent sitting on a hidden object, the object is shown
    // and asked for again (else the agent stays invisible with it).
    inline void onOrphaned(LLViewerObject* child, U32 parent_id, U32 ip, U32 port)
    {
        if (detail::sHiddenCount != 0)
        {
            detail::onOrphaned(child, parent_id, ip, port);
        }
    }

    // The <plus> hook in LLViewerObject::processUpdateMessage() where a seated avatar's new parent is
    // not known (the official viewer keeps it on the old seat, for a crossing that brings the avatar
    // before its seat): when that is the agent made to sit on a hidden object, the object is shown
    // and asked for again, and the agent is made an orphan of it (orphanize()), so that it sits on it
    // as it comes. True then; false leaves the official handling.
    inline bool waitForHiddenSeat(LLViewerObject* avatar, U32 parent_id, LLMessageSystem* msg)
    {
        return detail::sHiddenCount != 0 && detail::waitForHiddenSeat(avatar, parent_id, msg);
    }
}

#endif // PLUS_DERENDER_H
