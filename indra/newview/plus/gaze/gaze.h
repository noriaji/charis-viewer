/**
 * @file gaze.h
 * @brief Camera gaze: make avatars look at the camera in this viewer only.
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

#ifndef PLUS_GAZE_H
#define PLUS_GAZE_H

#include "stdtypes.h"

class LLVOAvatar;

namespace plus::gaze
{
    // Who looks at the camera (PlusGazeMode). Nothing is sent to other viewers
    // in any mode: it only changes the pose drawn here (and in snapshots).
    enum Mode : S32
    {
        MODE_OFF  = 0,  // the pose as animations and the official look-at make it
        MODE_SELF = 1,  // your own avatar only
        MODE_ALL  = 2   // every avatar that is drawn; counts as MODE_SELF unless PlusGazeAllowEveryone is on
    };

    // The UI's entry points (the photo mode card). setMode() clamps and saves
    // PlusGazeMode; the sliders are the settings PlusGazeEyeLimit (degrees),
    // PlusGazeHeadMax (degrees), PlusGazeNeckShare (0-1) and PlusGazeLag (seconds).
    Mode getMode();
    void setMode(Mode mode);

    // The <plus> hook in LLVOAvatar::updateCharacter(): called right after the
    // avatar's animations were blended into its joints and before the world
    // matrices and head offset are updated. Writes the neck, head and eye
    // rotations over whatever the animations (any priority, additive or not),
    // the AO and the official look-at left there. Main thread; does nothing
    // (and costs nothing) while the mode is off and no avatar is left to undo.
    void apply(LLVOAvatar* avatar);
}

#endif // PLUS_GAZE_H
