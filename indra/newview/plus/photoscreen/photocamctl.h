/**
 * @file photocamctl.h
 * @brief The camera controls of the photo screen's camera tab (photo-camera, research/260): the official
 *        camera controls' orbit pad, pan pad and zoom (floater_camera.xml, LLFloaterCamera) and the tilt row
 *        of plus/camroll, drawn in the photo screen's style (plus/photoscreen/psui).
 *
 * The pads are the official LLJoystickCameraRotate / LLJoystickCameraTrack (the same moves: faster toward the
 * edge, the dot in the middle puts the camera back); only their look is the photo screen's. Made in the code by
 * the left panel's column (photodetail_left.cpp) and placed with Stack::view(): made again with the column.
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

#ifndef PLUS_PHOTOSCREEN_PHOTOCAMCTL_H
#define PLUS_PHOTOSCREEN_PHOTOCAMCTL_H

#include "stdtypes.h"

#include <string>

class LLView;

namespace plus::photoscreen::camctl
{
    // The words of the controls (the left panel's strings, en / ja).
    struct Words
    {
        std::string orbit, orbit_tip;           // the orbit pad's caption and tip
        std::string pan, pan_tip;               // the pan pad's
        std::string zoom, zoom_tip;             // the zoom bar's
        std::string zoom_in_tip, zoom_out_tip;  // its + and - buttons
        std::string tilt_left_tip, tilt_right_tip, tilt_value_tip;
        std::string level, level_tip;           // the button that levels the camera
    };

    // The orbit pad, the zoom (+, the bar, -) and the pan pad side by side, a caption under each: a see-through
    // panel width wide and padsHeight(width) high (null if it could not be made).
    S32     padsHeight(S32 width);
    LLView* makePads(S32 width, const Words& words);

    // The tilt row: tilt left, the angle, tilt right and "Level" (as the official camera controls' tilt row,
    // plus/camroll). width wide, TILT_H high.
    inline constexpr S32 TILT_H = 32;
    LLView* makeTiltRow(S32 width, const Words& words);
}

#endif // PLUS_PHOTOSCREEN_PHOTOCAMCTL_H
