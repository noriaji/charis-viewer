/**
 * @file camerashot.h
 * @brief Camera shots: a slow push-in or pull-out, a zoom and a dolly zoom,
 *        optionally locked on an avatar (camerashot).
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

#ifndef PLUS_CAMERASHOT_CAMERASHOT_H
#define PLUS_CAMERASHOT_CAMERASHOT_H

#include <string>
#include <vector>

#include "lluuid.h"

#include "shotmath.h"

namespace plus::camerashot
{
    enum class Kind
    {
        Dolly,      // the camera moves toward / away from the subject; the angle stays
        Zoom,       // the angle changes; the camera stays
        DollyZoom,  // both, so the subject keeps its size and the background rushes in or out
        Orbit       // the camera goes round the subject (or the point it looks at), keeping its distance and height
    };

    // Which part of an avatar the camera looks at.
    enum class Part
    {
        Face,
        Chest,
        Body
    };

    struct ShotParams
    {
        Kind kind = Kind::Dolly;
        bool in = true;                   // push in / zoom in; false: pull out / zoom out
        double amount = 2.0;              // at least 1: "2" is twice as close (or twice the magnification)
        double degrees = 360.0;           // Orbit: how far round (360: once round); the other kinds do not use it
                                          // in = true: the camera goes to its own right (counter-clockwise from above)
        double seconds = 4.0;
        Ease ease = Ease::Smooth;
        // Lock-on. A null id: no lock-on, the camera's focus point now stays where it is.
        // An avatar's id (also your own): the camera keeps looking at that avatar's part.
        LLUUID subject;
        Part part = Part::Face;
    };

    // Starts a shot from the camera as it is now. It takes the free camera (the camera
    // leaves the avatar, as Esc / the camera controls bring it back). Another shot
    // running is replaced. False with a text for the user in 'error' (a key of the
    // floater's strings: "shot_err_no_avatar", "shot_err_flycam", "shot_err_mouselook",
    // "shot_err_too_close", "shot_err_overhead", "shot_err_subject"). A seconds or amount that is not finite is
    // taken as the setting's default.
    bool start(const ShotParams& params, std::string& error);

    // Stops where the camera is.
    void stop();
    // Puts an angle on the camera the way a shot does (plus/photomode's saved views): not in
    // CameraAngle, and the user's CameraAngle comes back when the camera goes back to the avatar.
    // An angle that is not finite or not above 0 is ignored.
    void holdFov(float fov);
    // The camera as it was when the last shot started (position, direction and angle):
    // for another take. False before the first shot, and in another region than it was taken in.
    bool hasStart();
    bool goToStart();
    bool running();
    // 0..1 of the time of the shot running (1 when none runs).
    float progress();

    // The avatars that can be the subject: yours first, then those within 'radius' metres
    // of the camera, the nearest first. Names are the viewers' full names (anonyms under RLVa's @shownames).
    struct Subject
    {
        LLUUID id;
        std::string name;
    };
    std::vector<Subject> nearbySubjects(float radius);

    // Where a part of an avatar is now, in global coordinates (the same place a shot looks at). False when the
    // avatar cannot be seen (left the region, not rendered yet). For plus/camerapath.
    bool subjectPlace(const LLUUID& id, Part part, LLVector3d& position);
}

#endif // PLUS_CAMERASHOT_CAMERASHOT_H
