/**
 * @file camroll.h
 * @brief Camera roll for the ordinary camera (tilting the camera about its forward axis).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc. (the original LLAgentCamera)
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

#ifndef PLUS_CAMROLL_CAMROLL_H
#define PLUS_CAMROLL_CAMROLL_H

#include "llquaternion.h"

class LLPanel;

// The roll is one setting, PlusCameraRoll (degrees, not saved between sessions, 0 = level): a positive
// angle leans the picture to the right (what the slider's + was seen to do): the camera itself turns
// counter-clockwise as seen from behind it (its up axis toward its left), so the horizon of a flat scene
// turns clockwise in the picture. A negative angle the other way. (Worked out from apply()'s formula,
// fix-round97; not yet looked at on screen.) currentDegrees() measures in the same sense, so
// setDegrees(currentDegrees()) keeps the picture as it is. Everything that wants to tilt the camera writes that setting (the photo tools'
// slider, a saved camera position, the keys); this feature reads it, and on a change makes the camera
// show it (setDegrees()).
namespace plus::camroll
{
    // The hook at the end of LLAgentCamera::updateCamera(): tilts the camera that was just worked
    // out. Does nothing while the flycam drives the camera (applyFlycam() tilts it then). Main thread,
    // every frame; a level camera costs a few compares.
    void apply();

    // The hook at the end of LLAgentCamera::updateFlycam(), where the flycam has just made the camera's
    // axes from its own attitude: the same tilt on top (the flycam's own roll, a game pad's with "Allow
    // Roll" or a 3D mouse's, stays under it). So the flycam's Q / E keys (plus/flycammouse), the photo
    // tools' slider and the camera controls' tilt buttons tilt the flycam without turning it off, the
    // tilt stays level with the horizon while the flycam turns, and the camera keeps it when the flycam
    // goes off (the setting stays). Main thread, every frame while the flycam is on.
    void applyFlycam();

    // LLAgentCamera::resetFlycamToCurrentView(): the camera's attitude as it is now without the tilt on top
    // (the flycam starts from it, the tilt goes on again with applyFlycam(), and does not show twice).
    LLQuaternion flycamStart();

    // The hooks where the camera is reset (resetView, resetCameraOrbit, switchCameraPreset): level again.
    void reset();

    // How far the camera is tilted now, in degrees (in the setting's sense, see above), as it is: with the
    // flycam this is measured from the camera's axes, otherwise it is the setting. For the saved camera
    // positions, the tilt buttons and keys (now + step), the photo guide's horizon.
    float currentDegrees();

    // The tilt on top of the camera: the setting, in degrees (0 = level). Outside the flycam the same as
    // currentDegrees(); with the flycam, without its own roll. For the keys and buttons that step it
    // (now + step), so the flycam's own roll is not added again.
    float tiltDegrees();

    // Tilts the camera by writing the setting. A write from anywhere (this, the photo tools' slider)
    // shows: while the flycam is on, on top of it (applyFlycam()); just after the flycam, the camera it
    // left behind is made again from where it is (camroll.cpp onRollChanged()).
    void setDegrees(float degrees);

    // A tilt on top of the setting, not written to it: a camera path's shake changes the tilt every frame,
    // and writing the setting that often would wake its listeners every frame. apply() adds it; whoever
    // set it puts it back to 0 when done (camerapath.cpp finish()). currentDegrees() leaves it out.
    void setShakeDegrees(float degrees);

    constexpr float MAX_DEGREES = 45.f; // what the photo tools' slider offers
    // What setDegrees() takes: a camera path tilts the camera further and spins the picture round
    // (several turns), the tilt window offers +-180. Only the limit of the setting; callers that offer
    // less (the photo tools, the keys) keep their own.
    constexpr float MAX_WIDE_DEGREES = 3000.f;
    // What the camera controls' tilt buttons offer (as the camera path window's tilt slider).
    constexpr float CONTROLS_MAX_DEGREES = 180.f;

    // A step of a tilt control: now + delta, within +-limit; a tilt already further out than limit
    // (a camera path's wide tilt or spin, say 360) is not pulled in to limit, it only cannot go
    // further out. For the buttons (nudge()) and the photo tools' keys.
    float stepped(float now_degrees, float delta_degrees, float limit_degrees);

    // Tilts the camera delta degrees further from where it is now (tiltDegrees()), within
    // +-CONTROLS_MAX_DEGREES (stepped()), through setDegrees(). Does nothing in mouselook (the camera
    // controls' tilt buttons are off there).
    void nudge(float delta_degrees);

    // The tilt row of the camera controls (floater_camera.xml, in the "zoom" panel).
    // The buttons call the XUI callbacks registered by this feature:
    //   Plus.CamRoll.Step  "left"/"right"  (mouse down: one step, STEP_DEGREES)
    //   Plus.CamRoll.Hold  "left"/"right"  (held past the button's delay: HOLD_DEGREES_PER_SECOND)
    //   Plus.CamRoll.Level                 (setDegrees(0))
    // The hook in LLPanelCameraZoom::draw() (only while the floater is shown): writes the row's angle
    // and turns the tilt buttons on or off, only when that changed.
    void drawCameraControls(LLPanel& zoom_panel);

    constexpr float STEP_DEGREES            = 5.f;
    constexpr float HOLD_DEGREES_PER_SECOND = 20.f;
}

#endif // PLUS_CAMROLL_CAMROLL_H
