/**
 * @file freeze.h
 * @brief World freeze: stop the world's time (avatars' animations, moving
 *        objects, particles, texture animations, flexi prims, clouds, the
 *        waves) while the camera, the UI and every setting stay live.
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

#ifndef PLUS_FREEZE_H
#define PLUS_FREEZE_H

#include "stdtypes.h"

class LLMessageSystem;
class LLViewerRegion;

namespace plus::freeze
{
    // The UI's entry points (the "stop the world" button of the photo mode card).
    // setFrozen() writes PlusWorldFreeze; the setting's listener does the work, so
    // a UI bound to the setting itself works the same. The setting is not saved:
    // the viewer always starts with the world running.
    bool isFrozen();
    void setFrozen(bool frozen);

    // Seconds of world time: the viewer's own clock (LLFrameTimer::getElapsedSeconds())
    // minus all the time spent frozen. For the shaders' clocks (the waves). Does
    // not run while frozen, continues from where it stopped when the world resumes.
    // While frozen by the measuring tools' cue (LLRHI_CUE_DIR/worldfreeze.on, research/218
    // in the sl-viewer repository) it is a fixed value instead, the same in every start.
    F64 worldSeconds();

    // Seconds since the freeze began (0 when not frozen); for the UI.
    F32 frozenSeconds();

    // The <plus> hook in LLViewerObjectList::processObjectUpdate(), before the
    // message is read. While frozen, a terse update (the only message that
    // moves an object) is kept here instead of being applied: the latest one
    // per object, applied when the world resumes. Returns true when the message
    // was taken and must not be processed. Full updates are left to
    // holdFullUpdate() while frozen; after the world resumes a live update
    // discards what is still held for its object.
    // Main thread.
    bool interceptObjectUpdate(LLMessageSystem* mesgsys, S32 update_type, bool compressed);

    // The <plus> hook at the top of the loop over the object blocks of
    // processObjectUpdate() (ObjectUpdate, ObjectUpdateCompressed). While frozen
    // and with PlusWorldFreezeAppearance on, a full update of an object already
    // shown (its colour, full bright, glow, light, textures, shape: what a script
    // changes) is held, the latest per object, and applied when the world resumes.
    // New objects, avatars, attachments, selected objects and changes of parent or
    // region always go through. Returns true when the block was taken and must be
    // skipped. Main thread.
    bool holdFullUpdate(LLMessageSystem* mesgsys, S32 block, S32 update_type, bool compressed, LLViewerRegion* regionp);

    // The <plus> hook in process_kill_object(), for each object of a KillObject message
    // (local_id, from the message's sender): what is held for that object is forgotten,
    // so the table follows the objects that exist. Main thread.
    void onObjectKilled(LLMessageSystem* mesgsys, U32 local_id);
}

#endif // PLUS_FREEZE_H
