/**
 * @file camerashot.cpp
 * @brief Camera shots: the move itself, one step a frame from an idle callback.
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

// The shot is worked out from the camera as it is when it starts:
//
//   pivot     the subject's part (it moves when the avatar does), or, without a
//             subject, the camera's focus point, which stays where it is
//   dir0      the unit vector from the pivot to the camera at the start
//   d0        the distance between them at the start
//
// and, at the eased progress e of every frame,
//
//   Dolly      camera = pivot + dir0 * lerp(d0, d1, e), the angle unchanged
//   Zoom       camera as at the start, the angle from the magnification m1^e
//   DollyZoom  as Dolly, the angle from the distance (fovForConstantWidth), so the
//              subject keeps its size in the picture
//
// and the camera looks at the pivot (without a subject: at the focus point of the
// start, so its direction does not change; with one, the view turns from the focus
// point of the start to the pivot over AIM_BLEND_SECONDS). The numbers are in shotmath.h.
//
// The camera is put where the official camera tools put it (LLAgentCamera::
// setCameraPosAndFocusGlobal, as plus/photomode and plus/posermcp do): it leaves
// the avatar and stays until Esc or the camera controls bring it back. Esc ends a
// shot (the camera is on the avatar again); touching the camera controls during a
// shot makes the two fight, so stop the shot first (the floater's Stop button).
//
// The angle of view of a shot is the camera's own (LLViewerCamera::setDefaultFOV), not the
// saved CameraAngle: the setting is never written, so it does not carry a shot's angle into
// the next session or mark the camera preset as changed (llpresetsmanager.cpp watches it).
// A shot's angle stays on the screen after it ends (or is stopped) until the camera goes back
// to the avatar (Esc, the camera controls) or the region is lost; then CameraAngle, the user's
// angle, is put back on the camera. An angle the user sets meanwhile goes into CameraAngle and
// on the camera at once, as always (its commit signal).
//
// The official camera smooths its position a little (CameraPositionSmoothing, a time
// constant of 20 ms at the default): the camera is about a frame behind the one given.
// At the speeds of a shot that is a few centimetres, too little to show in a dolly zoom.

#include "llviewerprecompiledheaders.h"

#include "camerashot.h"

#include "plus/camerapath/camerapath.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lljoint.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llworld.h"

namespace plus::camerashot
{
    namespace
    {
        constexpr char SETTING_FOV[] = "CameraAngle";
        // The subject is lost (left the region, logged off) for this long: the shot ends.
        constexpr double SUBJECT_LOST_SECONDS = 2.0;
        constexpr double MIN_SECONDS          = 0.2;
        // In place of a broken setting (NaN, infinite): the defaults of PlusCameraShotSeconds and -Amount.
        constexpr double DEFAULT_SECONDS      = 6.0;
        constexpr double DEFAULT_AMOUNT       = 2.0;
        constexpr double DEFAULT_ORBIT_DEGREES = 360.0;
        // Just after the flycam, the camera's focus point when the agent camera's is too near to keep
        // (as plus/camroll's holdCameraWhereItIs()).
        constexpr double HELD_FOCUS_DEFAULT   = 10.0;
        constexpr double HELD_FOCUS_MIN       = 0.5;
        // The view turns from where it looked to the subject over this long, so locking on does not snap.
        constexpr double AIM_BLEND_SECONDS    = 0.6;

        struct Joints
        {
            const char* name;
            double      up;   // metres above the joint: the face is above the skull's base, the body's middle above the hips
        };

        Joints jointsOf(Part part)
        {
            switch (part)
            {
            case Part::Chest: return { "mChest", 0.0 };
            case Part::Body:  return { "mPelvis", 0.3 };
            default:          return { "mHead", 0.07 };
            }
        }

        LLVOAvatar* findAvatar(const LLUUID& id)
        {
            if (id.isNull())
            {
                return nullptr;
            }
            if (id == gAgentID)
            {
                return isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
            }
            LLViewerObject* object = gObjectList.findObject(id);
            if (!object || object->isDead() || !object->isAvatar())
            {
                return nullptr;
            }
            return static_cast<LLVOAvatar*>(object);
        }

        // The place of the subject's part, in global coordinates. False when it cannot be had now.
        bool subjectPosition(const LLUUID& id, Part part, LLVector3d& position)
        {
            LLVOAvatar* avatar = findAvatar(id);
            if (!avatar || avatar->isDead())
            {
                return false;
            }
            const Joints joints = jointsOf(part);
            LLJoint*     joint  = avatar->getJoint(joints.name);
            LLVector3    agent_position = joint ? joint->getWorldPosition() : avatar->getPositionAgent();
            agent_position.mV[VZ] += (F32)joints.up;
            position = gAgent.getPosGlobalFromAgent(agent_position);
            return true;
        }

        struct Shot
        {
            ShotParams    params;
            double    startTime = 0.0;
            LLVector3d focus0;        // the focus point at the start (the pivot without a subject)
            LLVector3d position0;
            LLVector3d dir0;          // unit, pivot -> camera
            double    d0 = 0.0;
            double    d1 = 0.0;       // Dolly, DollyZoom
            double    m1 = 1.0;       // Zoom
            double    fov0 = 1.0;
            FovLimits fovRange;       // what the camera can show in this window (start())
            LLVector3d lastPivot;
            double    subjectLostSince = -1.0;
            U64       regionHandle = 0; // the region the avatar was in at the start (global places are of it)
        };

        Shot sShot;
        Shot sStart;            // the last shot started: its camera at the start
        bool sHasStart = false;
        bool sRunning  = false;
        float sProgress = 1.f;
        bool sFovHeld  = false; // the camera may show a shot's angle, not CameraAngle
        bool sTicking  = false;

        void tick(void*);

        // The idle callback runs while a shot runs or the camera shows a shot's angle.
        void updateTicking()
        {
            const bool want = sRunning || sFovHeld;
            if (want == sTicking)
            {
                return;
            }
            if (want)
            {
                gIdleCallbacks.addFunction(&tick, nullptr);
            }
            else
            {
                gIdleCallbacks.deleteFunction(&tick, nullptr);
            }
            sTicking = want;
        }

        void finish()
        {
            sRunning  = false;
            sProgress = 1.f;
            updateTicking();
        }

        // The flycam (3D mouse) puts the camera where its own attitude says (LLViewerJoystick::moveFlycam):
        // a camera put by the agent camera is not seen while it is on.
        bool flycamOn()
        {
            return LLViewerJoystick::getInstance()->getOverrideCamera();
        }

        // Esc, or the camera controls' reset, puts the camera back on the avatar.
        bool cameraBack()
        {
            return !gAgent.getRegion() || gAgentCamera.getFocusOnAvatar() || gAgentCamera.cameraMouselook();
        }

        // The user's angle (CameraAngle, never written here) on the camera again once it is back.
        void watchFov()
        {
            if (sFovHeld && cameraBack())
            {
                const F32 fov = gSavedSettings.getF32(SETTING_FOV);
                if (llfinite(fov)) // NaN would pass setDefaultFOV's llclamp()
                {
                    LLViewerCamera::getInstance()->setDefaultFOV(fov);
                }
                sFovHeld = false;
            }
            updateTicking();
        }

        void putCamera(const LLVector3d& position, const LLVector3d& focus, double fov)
        {
            gAgentCamera.setCameraPosAndFocusGlobal(position, focus, LLUUID::null);
            gAgentCamera.stopCameraAnimation();   // setCameraPosAndFocusGlobal starts one when the focus moved
            sFovHeld = true;
            LLViewerCamera::getInstance()->setDefaultFOV((F32)fov);
            updateTicking();
        }

        void tick(void*)
        {
            if (!sRunning)
            {
                watchFov();
                return;
            }
            // The camera back on the avatar: the shot is over, and the user's angle comes back.
            if (cameraBack())
            {
                finish();
                watchFov();
                return;
            }
            // The flycam turned on: it moves the camera itself, the shot would only seem to run.
            if (flycamOn())
            {
                finish();
                return;
            }

            const ShotParams& p       = sShot.params;
            const double  now     = LLTimer::getTotalSeconds();
            const double  t       = (now - sShot.startTime) / p.seconds;   // seconds made finite and >= MIN_SECONDS by start()
            const double  e       = easeValue(p.ease, t);

            LLVector3d pivot = sShot.focus0;
            if (!p.subject.isNull())
            {
                if (subjectPosition(p.subject, p.part, pivot))
                {
                    sShot.lastPivot        = pivot;
                    sShot.subjectLostSince = -1.0;
                }
                else
                {
                    if (sShot.subjectLostSince < 0.0)
                    {
                        sShot.subjectLostSince = now;
                    }
                    else if (now - sShot.subjectLostSince > SUBJECT_LOST_SECONDS)
                    {
                        finish();
                        return;
                    }
                    pivot = sShot.lastPivot;   // keeps the last place it was seen
                }
            }

            LLVector3d position = sShot.position0;
            double     fov      = sShot.fov0;
            switch (p.kind)
            {
            case Kind::Dolly:
            {
                const double d = sShot.d0 + (sShot.d1 - sShot.d0) * e;
                position       = pivot + sShot.dir0 * d;
                break;
            }
            case Kind::Zoom:
                fov = fovForMagnification(sShot.fov0, zoomMagnificationAt(sShot.m1, e));
                break;
            case Kind::DollyZoom:
            {
                const double d = sShot.d0 + (sShot.d1 - sShot.d0) * e;
                position       = pivot + sShot.dir0 * d;
                fov            = fovForConstantWidth(sShot.fov0, sShot.d0, d);
                break;
            }
            case Kind::Orbit:
            {
                // the start's direction turned about the vertical axis: the distance and the height stay
                double x = 0.0, y = 0.0;
                rotateAboutZ(sShot.dir0.mdV[VX], sShot.dir0.mdV[VY], orbitRadians(p.degrees, p.in, e), x, y);
                position = pivot + LLVector3d(x, y, sShot.dir0.mdV[VZ]) * sShot.d0;
                break;
            }
            }

            const double aim   = easeValue(Ease::Smooth, (now - sShot.startTime) / AIM_BLEND_SECONDS);
            const LLVector3d focus = sShot.focus0 + (pivot - sShot.focus0) * aim;
            putCamera(position, focus, clampFov(fov, sShot.fovRange));
            sProgress = (float)std::min(t, 1.0);
            if (t >= 1.0)
            {
                finish();
            }
        }
    }

    bool start(const ShotParams& params, std::string& error)
    {
        error.clear();
        if (!gAgent.getRegion() || !isAgentAvatarValid())
        {
            error = "shot_err_no_avatar";
            return false;
        }
        if (flycamOn())
        {
            // It would start from the camera of before the flycam, and not be seen: the user turns it off
            // where the camera is wanted (a shot from the flycam's place has no focus point to work from).
            error = "shot_err_flycam";
            return false;
        }
        plus::camerapath::stop();   // a camera path running would move the camera on from the shot
        if (gAgentCamera.cameraMouselook())
        {
            // The third-person camera is made only at the next frame: started now, the shot would start
            // from the camera in the head.
            error = "shot_err_mouselook";
            return false;
        }

        Shot shot;
        shot.params         = params;
        shot.params.seconds = llfinite(params.seconds) ? std::max(params.seconds, MIN_SECONDS) : DEFAULT_SECONDS;
        shot.params.amount  = llfinite(params.amount) ? params.amount : DEFAULT_AMOUNT;
        shot.params.degrees = orbitDegrees(llfinite(params.degrees) ? params.degrees : DEFAULT_ORBIT_DEGREES);
        shot.regionHandle   = gAgent.getRegion()->getHandle();
        const LLViewerCamera* viewer_camera = LLViewerCamera::getInstance();
        shot.fovRange       = fovLimits(viewer_camera->getMinView(), viewer_camera->getMaxView()); // the window's aspect
        shot.fov0           = clampFov(viewer_camera->getDefaultFOV(), shot.fovRange); // CameraAngle, or the last shot's angle

        // Just after the flycam was turned off, the camera stays at the flycam's view until the avatar
        // moves (LLViewerJoystick::toggleFlycam(), LLViewerCamera::updateCameraLocation()), and the agent
        // camera still holds the place of before the flycam: the shot starts from what is on the screen.
        LLViewerJoystick* joystick = LLViewerJoystick::getInstance();
        const bool        held     = !joystick->getCameraNeedsUpdate();
        if (held)
        {
            const LLViewerCamera* camera = LLViewerCamera::getInstance();
            shot.position0               = gAgent.getPosGlobalFromAgent(camera->getOrigin());
            double distance              = (gAgentCamera.getFocusGlobal() - shot.position0).magVec();
            if (distance < HELD_FOCUS_MIN)
            {
                distance = HELD_FOCUS_DEFAULT;
            }
            shot.focus0 = shot.position0 + LLVector3d(camera->getAtAxis()) * distance;
        }
        else
        {
            shot.position0 = gAgentCamera.getCameraPositionGlobal();
            shot.focus0    = gAgentCamera.getFocusGlobal();
        }

        LLVector3d pivot = shot.focus0;
        if (!params.subject.isNull() && !subjectPosition(params.subject, params.part, pivot))
        {
            error = "shot_err_subject";
            return false;
        }
        shot.lastPivot = pivot;

        LLVector3d toCamera = shot.position0 - pivot;
        shot.d0             = toCamera.magVec();
        if (shot.d0 < SHOT_MIN_DISTANCE)
        {
            error = "shot_err_too_close";
            return false;
        }
        shot.dir0 = toCamera / shot.d0;
        if (shot.params.kind == Kind::Orbit && !orbitHasSide(std::hypot(toCamera.mdV[VX], toCamera.mdV[VY]), shot.d0))
        {
            error = "shot_err_overhead";   // straight above or below it: there is no side to go round
            return false;
        }

        // From shot.params, whose amount was made finite above (+inf would send a dolly out to infinity).
        switch (shot.params.kind)
        {
        case Kind::Dolly:
            shot.d1 = dollyEndDistance(shot.d0, shot.params.amount, shot.params.in);
            break;
        case Kind::Zoom:
            shot.m1 = zoomEndMagnification(shot.fov0, shot.params.amount, shot.params.in, shot.fovRange);
            break;
        case Kind::DollyZoom:
            shot.d1 = dollyZoomEndDistance(shot.fov0, shot.d0, shot.params.amount, shot.params.in, shot.fovRange);
            break;
        case Kind::Orbit:
            break;   // the distance stays (d1 is not used)
        }
        shot.startTime = LLTimer::getTotalSeconds();

        // The first frame is the camera where it is, so nothing jumps; it leaves the avatar for the free camera.
        gAgentCamera.setFocusOnAvatar(false, false);
        putCamera(shot.position0, shot.focus0, shot.fov0);
        if (held)
        {
            joystick->setCameraNeedsUpdate(true); // the camera is made from the agent camera again (as camroll does)
        }
        sStart    = shot;
        sHasStart = true;

        finish(); // a shot running is replaced
        sShot     = shot;
        sRunning  = true;
        sProgress = 0.f;
        updateTicking();
        return true;
    }

    void stop()
    {
        finish();
    }

    void holdFov(float fov)
    {
        if (!llfinite(fov) || fov <= 0.f)
        {
            return;
        }
        sFovHeld = true;
        LLViewerCamera::getInstance()->setDefaultFOV(fov);
        updateTicking();
    }

    bool hasStart()
    {
        // Only in the region it was taken in: after a teleport its place would be somewhere else entirely
        // (as photocamera.cpp's temporary view).
        const LLViewerRegion* region = gAgent.getRegion();
        return sHasStart && region && region->getHandle() == sStart.regionHandle;
    }

    bool goToStart()
    {
        if (!hasStart())
        {
            return false;
        }
        finish();
        plus::camerapath::stop();   // as start(): a camera path running would move the camera on from the start
        if (gAgentCamera.cameraMouselook())
        {
            gAgentCamera.changeCameraToDefault();
        }
        if (flycamOn())
        {
            // A place to go to: the camera is handed back from the flycam, as the photo tools' saved
            // positions do (photocamera.cpp goToView).
            handle_toggle_flycam();
        }
        // Made again from the agent camera, also just after the flycam was turned off (it would stay at the
        // flycam's view until the avatar moves), as photocamera.cpp goToView.
        LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
        gAgentCamera.setFocusOnAvatar(false, false);
        putCamera(sStart.position0, sStart.focus0, sStart.fov0);
        return true;
    }

    bool running()
    {
        return sRunning;
    }

    float progress()
    {
        return sProgress;
    }

    bool subjectPlace(const LLUUID& id, Part part, LLVector3d& position)
    {
        return subjectPosition(id, part, position);
    }

    std::vector<Subject> nearbySubjects(float radius)
    {
        std::vector<Subject> subjects;
        if (!isAgentAvatarValid())
        {
            return subjects;
        }
        subjects.push_back({ gAgentID, gAgentAvatarp->getFullname() });

        uuid_vec_t              ids;
        std::vector<LLVector3d> positions;
        const LLVector3d        origin = gAgentCamera.getCameraPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, origin, radius);

        std::vector<std::pair<double, Subject>> found;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == gAgentID)
            {
                continue;
            }
            LLVOAvatar* avatar = findAvatar(ids[i]);
            if (!avatar || avatar->isControlAvatar())
            {
                continue;
            }
            // RLVa @shownames: the anonym (plus/rlv, research/248 3.4); the camera path's subject list takes it from here too
            std::string name = avatar->getFullname();
            if (plus::rlv::enabled())
            {
                name = plus::rlv::avatarNameForPlus(ids[i], name);
            }
            found.push_back({ (positions[i] - origin).magVec(), { ids[i], std::move(name) } });
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : found)
        {
            subjects.push_back(entry.second);
        }
        return subjects;
    }
}
