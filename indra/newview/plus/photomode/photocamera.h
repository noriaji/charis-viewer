/**
 * @file photocamera.h
 * @brief Photo tools: the hook for the region's push-back of the camera (the rest of photocamera.cpp,
 *        the remembered camera positions, is declared in photomode.h).
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

#ifndef PLUS_PHOTOMODE_PHOTOCAMERA_H
#define PLUS_PHOTOMODE_PHOTOCAMERA_H

class LLHost;
class LLVector4;

namespace plus::photomode
{
    // The hook in process_camera_constraint() (llviewermessage.cpp), before the plane the region sent
    // is given to LLAgentCamera: with PlusIgnoreSimCameraConstraint set and the photo mode open (the photo
    // screen on or the photo tools window open and not minimized; research/275 L166) it becomes "no plane" (all zero,
    // which LLAgentCamera skips), so the camera is not pushed back out of walls. Firestorm returns before
    // the plane is set (FSIgnoreSimulatorCameraConstraints); clearing it also drops a plane that came before.
    // sender is the region's host (the plane is kept under that region, to be put back when the
    // setting goes off while the agent is still there).
    void filterCameraCollidePlane(LLVector4& plane, const LLHost& sender);

    // Clears the plane at once when the setting is turned on in the photo mode, and puts the region's back when
    // it goes off; while it is on, follows the photo mode going in and out (looked at 4 times a second, not at
    // all while it is off). From the feature's init.
    void watchCameraConstraintSetting();
}

#endif // PLUS_PHOTOMODE_PHOTOCAMERA_H
