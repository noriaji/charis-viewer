/**
 * @file animquick.h
 * @brief Quick animation preview: double-click an animation in the inventory and
 *        it plays in one reused preview window (research/332).
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

#ifndef PLUS_ANIMQUICK_ANIMQUICK_H
#define PLUS_ANIMQUICK_ANIMQUICK_H

#include "lluuid.h"

// A preview window (LLPreviewAnim) has a "pin" check box. A window that is not
// pinned is disposable: the next quick open closes it (which stops its own
// animation and nothing else) and opens the new window at the same place, then
// plays at once, locally or in the world as the user last chose. A pinned window
// is never touched. Windows opened any other way start pinned, so the old
// behaviour (several windows, press Play yourself) is kept as it was.
// No state is kept here: the pin lives in the window, the last choice in the
// setting PlusAnimQuickInworld. Main thread only (UI actions).
namespace plus::animquick
{
    // The double-click of an animation (LLAnimationBridgeAction::doIt). Returns
    // true when it opened and played the window itself, or played a disposable window of this
    // item again; false when the caller must do what the official code did (the setting is off,
    // the avatar is not ready, or a window of this item that is pinned or playing is already
    // open and only needs to come to the front).
    bool open(const LLUUID& item_id);

    // The user pressed a Play button (Inworld or Locally) to start: remember which,
    // for the next quick open.
    void rememberMode(bool inworld);

    // Whether the quick open is on (PlusAnimQuickPreview).
    bool enabled();

    // Assets a preview window plays "Locally" (not sent to the server). Such a motion never enters the avatar's
    // mPlayingAnimations, so Stop Animating (LLAgent::stopCurrentAnimations) cannot see it: the windows note their
    // local plays here, and Stop Animating calls stopLocalPlays(), which stops them at once and puts the Play buttons
    // of the windows that played them back. Main thread only.
    void noteLocalPlay(const LLUUID& asset);
    void forgetLocalPlay(const LLUUID& asset);
    void stopLocalPlays();
}

#endif
