/**
 * @file camerapath.cpp
 * @brief Camera path: the open path, playing it one step a frame from an idle callback, the pictures
 *        of a path frame by frame, and the files.
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

// The camera is put where the official camera tools put it (LLAgentCamera::setCameraPosAndFocusGlobal, as
// plus/camerashot and plus/photomode do): it leaves the avatar and stays until Esc or the camera controls
// bring it back; Esc ends the path. The angle of view is the camera's own (setDefaultFOV), not the saved
// CameraAngle, which is never written here; it stays on the screen after the path ends until the camera
// goes back to the avatar, then CameraAngle is put back (the same as camerashot.cpp). The tilt is
// plus/camroll's setting PlusCameraRoll: written when it changes, and left where the path ends.
//
// The path is worked out at the start of every frame from the time since it started (a Timeline of
// pathmath.h, rebuilt when the path is changed), so a frame that took long does not make the
// camera run behind. The path is kept in global coordinates, so a path made in one region plays in
// another at the same global place (the window warns).
//
// Order of the camera's day each frame (applyAt): the directions of the points that look at an avatar are made
// from where the avatar is now, the timeline gives the camera at the time, the shake (a function of the time,
// shakemath.h) is added, and the result goes on the camera. With the pictures of a path (Exporting) the time is
// not the clock's but step / fps, so the shake and the path are the same whatever the frames cost.

#include "llviewerprecompiledheaders.h"

#include "camerapath.h"
#include "plus.h"

#include "plus/camerashot/camerashot.h"
#include "plus/camerashot/shotmath.h"
#include "plus/camroll/camroll.h"
#include "plus/photomode/photoguide.h"
#include "plus/photomode/photomode.h"
#include "plus/photomode/photoshot.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "llagent.h"
#include "llagentcamera.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llui.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewermenu.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"

namespace plus::camerapath
{
    namespace
    {
        constexpr char   SETTING_FOV[]   = "CameraAngle";
        constexpr char   FOLDER[]        = "camerapaths";
        constexpr char   SUFFIX[]        = ".xml";
        constexpr char   SCRIPT_SUFFIX[] = ".json";
        // Of a saved path's name, in Windows' UTF-16 characters (the window's box takes as many): the folder (about
        // 80 under the user's AppData), the name, its suffix and the temporary file's stay inside Windows' 260.
        constexpr size_t MAX_NAME_CHARS  = 64;
        constexpr int    FILE_FORMAT     = 1;
        constexpr S64    MAX_FILE_BYTES  = 1024 * 1024;
        constexpr double MIN_FOCUS       = 0.25;   // metres: the focus point is never closer to the camera than this
        constexpr double MIN_FOV         = 0.15;   // as camerashot (about 8.6 degrees)
        constexpr double MAX_FOV         = 2.2;    // about 126 degrees
        constexpr double DEFAULT_FOV     = 1.047;  // CameraAngle's default, for a broken angle
        constexpr double MAX_SECONDS     = 600.0;  // of one move or hold
        constexpr int    ROUTE_STEPS     = 12;     // lines per move on the screen
        constexpr int    EXPORT_SETTLE_FRAMES = 4; // frames the world gets at a new camera before its picture is taken
        constexpr int    EXPORT_MAX_FRAMES    = 36000;
        constexpr int    EXPORT_MIN_FPS = 1;
        constexpr int    EXPORT_MAX_FPS = 120;

        Path     sPath;
        Timeline sTimeline;      // the path's
        Timeline sHeld;          // the camera held in place (State::InPlace): one point, apart from the path
        unsigned sRevision = 1;
        int      sSelected = -1;

        State  sState     = State::Idle;
        double sStartTime = 0.0;
        double sCountEnd  = 0.0;
        int    sCountShown = 0;
        double sFallbackFov = 1.0;   // for a point that has no angle of its own
        bool   sFovHeld   = false;   // the camera may show the path's angle, not CameraAngle
        bool   sTicking   = false;
        bool   sRollSet   = false;
        float  sLastRoll  = 0.f;

        struct Export
        {
            int          fps = 30;
            int          frame = 0;
            int          frames = 0;
            int          settle = 0;
            bool         shooting = false;
            unsigned     lastNumber = 0;
            std::string  folder;
            std::string  extension;
        };
        Export sExport;
        Export sLeftover;   // a stopped run whose picture is still being taken (shooting: tickLeftover())

        std::vector<std::vector<LLVector3d>> sRoute;
        bool sRouteDirty = true;
        bool sShowRoute  = true;
        bool sEditorOpen = false;

        // One turn of the picture on its own (spinOnce): the tilt goes from 'from' to 'to' (a full turn away) and
        // is put back at 'from' (the same picture) when it ends.
        struct Spin
        {
            bool   active = false;
            double start = 0.0;
            double seconds = 4.0;
            float  from = 0.f;
            float  to = 0.f;
            float  lastWritten = 0.f;
        };
        Spin sSpin;
        bool sKeepShakeRoll = false;   // the last picture of a path: its shake tilt goes into the setting, so it does not vanish

        void tick(void*);

        void updateTicking()
        {
            const bool want = sState != State::Idle || sFovHeld || sSpin.active || sLeftover.shooting;
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

        // What a point keeps (the files): the whole range, whatever the window is.
        double clampFov(double fov)
        {
            return std::clamp(fov, MIN_FOV, MAX_FOV);
        }

        // What the camera can show in this window: LLViewerCamera::setDefaultFOV clamps to getMinView()..getMaxView()
        // (about 98 degrees at most in a 16:9 window), so a move worked out past them would stop widening part-way.
        // Taken again at each start (the window may have been resized), as camerashot does (fix-round69).
        plus::camerashot::FovLimits sFovRange;

        void takeFovRange()
        {
            const LLViewerCamera* camera = LLViewerCamera::getInstance();
            sFovRange = plus::camerashot::fovLimits(camera->getMinView(), camera->getMaxView());
        }

        // An angle the camera is given now. A NaN would pass std::clamp (and setDefaultFOV's llclamp()).
        double runFov(double fov)
        {
            return plus::camerashot::clampFov(std::isfinite(fov) ? fov : DEFAULT_FOV, sFovRange);
        }

        // The flycam puts the camera where its own attitude says: a camera put by the agent camera is not
        // seen while it is on.
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
                if (llfinite(fov)) // NaN would pass setDefaultFOV's llclamp() (as camerashot.cpp)
                {
                    LLViewerCamera::getInstance()->setDefaultFOV(fov);
                }
                sFovHeld = false;
            }
            updateTicking();
        }

        // The angle the user has: CameraAngle once a path has put its own angle on the camera, else the camera's.
        double userFov()
        {
            return runFov(sFovHeld ? gSavedSettings.getF32(SETTING_FOV) : LLViewerCamera::getInstance()->getDefaultFOV());
        }

        void putCamera(const LLVector3d& position, const LLVector3d& focus, double fov)
        {
            gAgentCamera.setCameraPosAndFocusGlobal(position, focus, LLUUID::null);
            gAgentCamera.stopCameraAnimation();   // setCameraPosAndFocusGlobal starts one when the focus moved
            sFovHeld = true;
            LLViewerCamera::getInstance()->setDefaultFOV((F32)runFov(fov));
            updateTicking();
        }

        void putRoll(double roll_in)
        {
            float roll = (float)std::remainder(roll_in, 360.0);
            // A spin takes the roll far past a full turn (the timeline goes on turning one way); a whole turn looks the
            // same, so only what is left within half a turn is written. No spin can reach a limit of the setting.
            roll = llclamp(roll, -180.f, 180.f);
            if (!sRollSet || fabsf(roll - sLastRoll) > 0.005f)   // writing the setting every frame would wake its listeners
            {
                plus::camroll::setDegrees(roll);
                sLastRoll = roll;
                sRollSet  = true;
            }
        }

        KeyPoint keyOf(const camerapath::Point& point)
        {
            KeyPoint key;
            const LLVector3d to_focus = point.focus - point.position;
            key.v[C_X]     = point.position.mdV[VX];
            key.v[C_Y]     = point.position.mdV[VY];
            key.v[C_Z]     = point.position.mdV[VZ];
            key.v[C_FOV]   = point.fov > 0.f ? runFov(point.fov) : runFov(sFallbackFov);
            key.v[C_ROLL]  = point.roll;
            key.v[C_FOCUS] = std::max(to_focus.magVec(), MIN_FOCUS);
            key.q          = lookQuat(to_focus.mdV[VX], to_focus.mdV[VY], to_focus.mdV[VZ]);
            key.seconds    = std::clamp(point.seconds, 0.0, MAX_SECONDS);
            key.hold       = std::clamp(point.hold, 0.0, MAX_SECONDS);
            key.motion     = point.motion;
            key.orbitTurns = std::round(std::clamp((double)point.orbit, -(double)MAX_ORBIT, (double)MAX_ORBIT));
            if (point.motion == Motion::Orbit)
            {
                // What it goes round: the avatar it looks at (where it is now), else the point it looks at. The point
                // faces it, and the pivot follows the avatar while it plays (updateTracking()).
                LLVector3d pivot = point.focus;
                LLVector3d place;
                if (point.subject.notNull() && plus::camerashot::subjectPlace(point.subject, point.part, place))
                {
                    pivot = place;
                }
                key.hasPivot = true;
                key.pivot[0] = pivot.mdV[VX];
                key.pivot[1] = pivot.mdV[VY];
                key.pivot[2] = pivot.mdV[VZ];
                const LLVector3d to_pivot = pivot - point.position;
                key.q          = lookQuat(to_pivot.mdV[VX], to_pivot.mdV[VY], to_pivot.mdV[VZ]);
                key.v[C_FOCUS] = std::max(to_pivot.magVec(), MIN_FOCUS);
            }
            return key;
        }

        // The timeline from the points. A point with no angle of its own takes the camera's angle at the start.
        void rebuild()
        {
            std::vector<KeyPoint> points;
            points.reserve(sPath.points.size());
            // The roll of a key is its point's own tilt plus whole turns: from one point to the next it takes the short way
            // round (179 to -179 is 2 degrees) and the point's spin adds its turns, and it goes on past a full turn
            // instead of coming back (rollStep).
            double roll = 0.0, previous_own = 0.0;
            for (size_t i = 0; i < sPath.points.size(); ++i)
            {
                KeyPoint key = keyOf(sPath.points[i]);
                const double own = key.v[C_ROLL];
                const double turns = std::round(std::clamp((double)sPath.points[i].spin, -(double)MAX_SPIN, (double)MAX_SPIN));   // whole turns
                roll = i == 0 ? own : roll + rollStep(previous_own, own, turns);
                previous_own = own;
                key.v[C_ROLL] = roll;
                points.push_back(key);
            }
            sTimeline.set(std::move(points));
            sRouteDirty = true;
        }

        // The directions of the points that look at an avatar, from where the avatar is now. An avatar that
        // cannot be seen leaves the direction it had.
        void updateTracking()
        {
            if (sState == State::InPlace)
            {
                return;   // the timeline is the one point of the camera held in place, not the path's
            }
            for (size_t i = 0; i < sPath.points.size(); ++i)
            {
                const camerapath::Point& point = sPath.points[i];
                LLVector3d target;
                if (point.subject.isNull() || !plus::camerashot::subjectPlace(point.subject, point.part, target))
                {
                    continue;
                }
                const LLVector3d to_target = target - point.position;
                const double     distance  = to_target.magVec();
                if (distance < 0.01)
                {
                    continue;
                }
                sTimeline.setDirection(i, lookQuat(to_target.mdV[VX], to_target.mdV[VY], to_target.mdV[VZ]),
                                       std::max(distance, MIN_FOCUS));
                if (point.motion == Motion::Orbit)
                {
                    sTimeline.setPivot(i, target.mdV[VX], target.mdV[VY], target.mdV[VZ]);   // the avatar it goes round moved
                }
            }
        }

        // The camera at 'seconds' into the path, the shake at 'shake_seconds'.
        void applyAt(double seconds, double shake_seconds)
        {
            updateTracking();
            Sample sample;
            if (!(sState == State::InPlace ? sHeld : sTimeline).evaluate(seconds, sample))
            {
                return;
            }
            // While something runs, the shake's tilt goes on the camera only (camroll setShakeDegrees(), put back to 0 by
            // finish()), not into the tilt setting every frame. A still picture (previewAt, nothing running) keeps it
            // in the setting: nothing would take it off again.
            double shake_roll = 0.0;
            if (sPath.shake.kind != ShakeKind::None)
            {
                const ShakeOffset offset = shakeAt(sPath.shake, shake_seconds);
                applyShake(sample, offset);
                if (sState != State::Idle && !sKeepShakeRoll)
                {
                    shake_roll = offset.roll;
                }
            }
            const Vec3 forward = forwardOf(sample.q);
            const LLVector3d position(sample.v[C_X], sample.v[C_Y], sample.v[C_Z]);
            const double     focus_distance = std::max(sample.v[C_FOCUS], MIN_FOCUS);
            const LLVector3d focus = position + LLVector3d(forward.x, forward.y, forward.z) * focus_distance;
            putCamera(position, focus, sample.v[C_FOV]);
            putRoll(sample.v[C_ROLL] - shake_roll);
            plus::camroll::setShakeDegrees((float)shake_roll);
        }

        // Ends a turn of the picture where it began.
        void stopSpin()
        {
            if (sSpin.active)
            {
                sSpin.active = false;
                plus::camroll::setDegrees(sSpin.from);
                ++sRevision;
                updateTicking();
            }
        }

        void tickSpin()
        {
            // Somebody else wrote the tilt (the slider, a key, Esc): theirs is kept.
            if (std::fabs(plus::camroll::currentDegrees() - sSpin.lastWritten) > 0.5f)
            {
                sSpin.active = false;
                ++sRevision;
                updateTicking();
                return;
            }
            const double t = (LLTimer::getTotalSeconds() - sSpin.start) / sSpin.seconds;
            if (t >= 1.0)
            {
                stopSpin();   // a full turn is the same picture: back to where it began
                return;
            }
            const float value = sSpin.from + (sSpin.to - sSpin.from) * (float)smoothstep(t);
            plus::camroll::setDegrees(value);
            sSpin.lastWritten = std::clamp(value, -plus::camroll::MAX_WIDE_DEGREES, plus::camroll::MAX_WIDE_DEGREES);
        }

        // A tilt may be whole turns away from level (a camera put at 400 degrees by something else): the picture is the
        // same, so it is put back within half a turn, as 360 becomes 0. A path writes its tilt within half a turn
        // already (putRoll), so this is for what was there before and for the picture left by a preview.
        void wrapRoll()
        {
            const float roll = plus::camroll::currentDegrees();
            if (std::fabs(roll) > 180.f && !flycamOn())
            {
                plus::camroll::setDegrees(roll - 360.f * std::round(roll / 360.f));
            }
        }

        void finish()
        {
            stopSpin();
            sKeepShakeRoll = false;
            plus::camroll::setShakeDegrees(0.f);   // the shake's tilt was on the camera only (applyAt)
            if (sState != State::Idle)
            {
                sState = State::Idle;
                ++sRevision;
                wrapRoll();
            }
            if (sExport.shooting)
            {
                sLeftover = sExport;   // its picture is on the way: tickLeftover() files it
            }
            sExport = Export();
            updateTicking();
        }

        double elapsed()
        {
            return LLTimer::getTotalSeconds() - sStartTime;
        }

        void say(const char* string_name, const std::string& number = std::string())
        {
            LLStringUtil::format_map_t args;
            args["[NUMBER]"] = number;
            plus::photomode::showGuideMessage(LLTrans::getString(string_name, args));
        }

        void tickPlaying()
        {
            const double raw   = elapsed();
            const double total = sTimeline.total();
            double       t     = raw;
            if (raw >= total)
            {
                if (sPath.loop && total > 0.0)
                {
                    t = std::fmod(raw, total);   // back to the first point with a cut; the shake goes on
                }
                else
                {
                    sKeepShakeRoll = true;
                    applyAt(total, raw);   // the last picture, held (finish() ends the camera's own shake tilt)
                    finish();
                    return;
                }
            }
            applyAt(t, raw);
        }

        void tickCountdown()
        {
            const double now  = LLTimer::getTotalSeconds();
            const double left = sCountEnd - now;
            if (left <= 0.0)
            {
                sStartTime = now;
                sState     = State::Playing;
                ++sRevision;
                say("PlusCameraPathGo");
                make_ui_sound("UISndSnapshot");
                return;
            }
            const int number = (int)std::ceil(left);
            if (number != sCountShown)
            {
                sCountShown = number;
                say("PlusCameraPathCount", std::to_string(number));
                make_ui_sound("UISndAlert");
                ++sRevision;
            }
        }

        void stopExport(const std::string& text)
        {
            if (!text.empty())
            {
                plus::photomode::showGuideMessage(text);
                LL_WARNS("PlusCameraPath") << text << LL_ENDL;
            }
            finish();
        }

        std::string frameFile(const Export& run, int number)
        {
            return gDirUtilp->add(run.folder, llformat("frame_%05d.%s", number, run.extension.c_str()));
        }

        // The picture of the frame is in: moves it into the run's folder (made at the first one).
        bool keepPicture(Export& run, const plus::photomode::ShotResult& shot)
        {
            if (run.folder.empty())
            {
                char stamp[32] = {};
                const std::time_t now = std::time(nullptr);
                std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
                std::string name = sPath.name.empty() ? std::string("path") : sPath.name;
                run.folder = gDirUtilp->add(gDirUtilp->getDirName(shot.path), std::string("camerapath_") + name + "_" + stamp);
                if (LLFile::mkdir(run.folder) != 0 && !LLFile::isdir(run.folder))
                {
                    run.folder.clear();
                    return false;
                }
                run.extension = gDirUtilp->getExtension(shot.path);
                if (run.extension.empty())
                {
                    run.extension = "png";
                }
            }
            return LLFile::rename(shot.path, frameFile(run, run.frame + 1)) == 0;
        }

        // The picture asked for by a run that was stopped meanwhile (finish()): it is still taken, and goes into
        // that run's folder as its last frame instead of staying among the ordinary photos.
        void tickLeftover()
        {
            const plus::photomode::ShotState shot_state = plus::photomode::shotState();
            const plus::photomode::ShotResult& shot = plus::photomode::lastShot();
            if (shot.number == sLeftover.lastNumber || shot_state == plus::photomode::ShotState::Pending
                || shot_state == plus::photomode::ShotState::Busy)
            {
                return;   // not taken yet
            }
            if (shot.ok && !keepPicture(sLeftover, shot))
            {
                LL_WARNS("PlusCameraPath") << "The last frame of a stopped run stays at " << shot.path << LL_ENDL;
            }
            sLeftover = Export();
            updateTicking();
        }

        void tickExport()
        {
            const double t = (double)sExport.frame / (double)sExport.fps;
            applyAt(std::min(t, sTimeline.total()), t);

            if (!sExport.shooting)
            {
                // The world gets a few frames at the new camera (the camera is smoothed a little, and the
                // photo button rests between pictures) before the picture is taken.
                if (++sExport.settle < EXPORT_SETTLE_FRAMES || plus::photomode::shotState() != plus::photomode::ShotState::Idle)
                {
                    return;
                }
                sExport.lastNumber = plus::photomode::lastShot().number;
                if (!plus::photomode::takePhoto())
                {
                    stopExport(plus::photomode::lastShot().error);
                    return;
                }
                sExport.shooting = true;
                ++sRevision;
                return;
            }
            const plus::photomode::ShotState shot_state = plus::photomode::shotState();
            const plus::photomode::ShotResult& shot = plus::photomode::lastShot();
            if (shot.number == sExport.lastNumber || shot_state == plus::photomode::ShotState::Pending
                || shot_state == plus::photomode::ShotState::Busy)
            {
                return;   // not taken yet
            }
            sExport.shooting = false;   // this picture is in: no leftover for finish() to wait for
            if (!shot.ok || !keepPicture(sExport, shot))
            {
                stopExport(shot.ok ? LLTrans::getString("PlusCameraPathExportMove") : shot.error);
                return;
            }
            ++sExport.frame;
            sExport.settle = 0;
            ++sRevision;
            if (sExport.frame >= sExport.frames)
            {
                LLStringUtil::format_map_t args;
                args["[FOLDER]"] = sExport.folder;
                const std::string text = LLTrans::getString("PlusCameraPathExportDone", args);
                plus::photomode::showGuideMessage(text);
                LL_INFOS("PlusCameraPath") << text << LL_ENDL;
                finish();
            }
        }

        void tick(void*)
        {
            if (sLeftover.shooting)
            {
                tickLeftover();
            }
            if (sSpin.active)
            {
                tickSpin();
            }
            if (sState == State::Idle)
            {
                watchFov();
                return;
            }
            // The camera back on the avatar (Esc): the path is over, and the user's angle comes back.
            if (cameraBack())
            {
                finish();
                watchFov();
                return;
            }
            // The flycam turned on: it moves the camera itself, the path would only seem to run.
            if (flycamOn())
            {
                finish();
                return;
            }
            switch (sState)
            {
            case State::Counting:  tickCountdown(); break;
            case State::Playing:   tickPlaying(); break;
            case State::InPlace:   applyAt(0.0, elapsed()); break;
            case State::Exporting: tickExport(); break;
            default: break;
            }
        }

        bool cameraExists()
        {
            return gAgent.getRegion() && isAgentAvatarValid();
        }

        // The camera ready to be put: not in mouselook, off the avatar.
        void takeCamera()
        {
            if (gAgentCamera.cameraMouselook())
            {
                gAgentCamera.changeCameraToDefault();
            }
            gAgentCamera.setFocusOnAvatar(false, false);
            // Just after the flycam was turned off, the camera it left behind stays until the avatar moves;
            // made again from the place put here (as photocamera.cpp goToView does).
            LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
        }

        // The reasons the camera cannot be taken now; false with the key of the reason in 'error'.
        bool canTakeCamera(std::string& error)
        {
            if (!cameraExists())
            {
                error = "path_err_no_camera";
                return false;
            }
            if (flycamOn())
            {
                // It would start from the camera of before the flycam, and not be seen: the user turns it off.
                error = "path_err_flycam";
                return false;
            }
            return true;
        }

        void beginRun()
        {
            plus::camerashot::stop();   // a shot running would move the camera on from the path
            takeFovRange();
            sFallbackFov = userFov();
            rebuild();
            takeCamera();
            sRollSet = false;
        }

        // --- the route -----------------------------------------------------------

        void buildRoute()
        {
            sRoute.clear();
            sRouteDirty = false;
            const size_t count = sPath.points.size();
            if (count == 0)
            {
                return;
            }
            sRoute.emplace_back();
            sRoute.back().push_back(sPath.points[0].position);
            for (size_t i = 1; i < count; ++i)
            {
                const camerapath::Point& point = sPath.points[i];
                if (point.seconds <= 0.0)
                {
                    sRoute.emplace_back();   // a cut: no line between the two
                    sRoute.back().push_back(point.position);
                    continue;
                }
                const double leave  = sTimeline.arrival(i - 1) + std::clamp(sPath.points[i - 1].hold, 0.0, MAX_SECONDS);
                const double arrive = sTimeline.arrival(i);
                Sample sample;
                // an orbit with laps needs lines for each lap, or it shows as a star
                const int steps = point.motion == Motion::Orbit ? ROUTE_STEPS * (1 + (int)std::fabs(point.orbit)) : ROUTE_STEPS;
                for (int step = 1; step <= steps; ++step)
                {
                    if (sTimeline.evaluate(leave + (arrive - leave) * step / steps, sample))
                    {
                        sRoute.back().push_back(LLVector3d(sample.v[C_X], sample.v[C_Y], sample.v[C_Z]));
                    }
                }
            }
        }

        // --- files ---------------------------------------------------------------

        std::string folder()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, FOLDER);
        }

        bool isScriptName(const std::string& name)
        {
            const size_t n = strlen(SCRIPT_SUFFIX);
            return name.size() > n && LLStringUtil::compareInsensitive(name.substr(name.size() - n), SCRIPT_SUFFIX) == 0;
        }

        // The names Windows keeps for devices (CON, NUL, COM1 ...), with any extension after them: such a file
        // cannot be made as a file there.
        bool reservedOnWindows(const std::string& name)
        {
            std::string stem = name.substr(0, name.find('.'));
            LLStringUtil::trimTail(stem);   // "CON .xml" is CON too
            LLStringUtil::toUpper(stem);
            for (const char* reserved : { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" })
            {
                if (stem == reserved)
                {
                    return true;
                }
            }
            return stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0'
                   && stem[3] <= '9';
        }

        std::string fileFor(const std::string& name)
        {
            if (isScriptName(name))
            {
                return gDirUtilp->add(folder(), LLDir::getScrubbedFileName(name));
            }
            return gDirUtilp->add(folder(), LLDir::getScrubbedFileName(name) + SUFFIX);
        }

        LLSD vector3dToLLSD(const LLVector3d& v)
        {
            LLSD data = LLSD::emptyArray();
            data.append(v.mdV[VX]);
            data.append(v.mdV[VY]);
            data.append(v.mdV[VZ]);
            return data;
        }

        LLVector3d vector3dFromLLSD(const LLSD& data)
        {
            if (!data.isArray() || data.size() != 3)
            {
                return LLVector3d();
            }
            return LLVector3d(data[0].asReal(), data[1].asReal(), data[2].asReal());
        }

        LLSD toLLSD(const Path& source)
        {
            LLSD data;
            data["format"]    = FILE_FORMAT;
            data["region"]    = source.region;
            data["loop"]      = source.loop;
            data["countdown"] = source.countdown;
            data["shake"]["kind"]   = (S32)source.shake.kind;
            data["shake"]["amount"] = source.shake.amount;
            data["shake"]["speed"]  = source.shake.speed;
            data["shake"]["seed"]   = (S32)std::min(source.shake.seed, MAX_SEED);   // read back as it was (fromLLSD)
            data["points"] = LLSD::emptyArray();
            for (const camerapath::Point& point : source.points)
            {
                LLSD entry;
                entry["name"]    = point.name;
                entry["pos"]     = vector3dToLLSD(point.position);
                entry["focus"]   = vector3dToLLSD(point.focus);
                entry["fov"]     = (F64)point.fov;
                entry["roll"]    = (F64)point.roll;
                entry["spin"]    = (F64)point.spin;
                entry["orbit"]   = (F64)point.orbit;
                entry["seconds"] = point.seconds;
                entry["hold"]    = point.hold;
                entry["motion"]  = (S32)point.motion;
                if (point.subject.notNull())
                {
                    entry["subject"] = point.subject;
                    entry["part"]    = (S32)point.part;
                }
                data["points"].append(entry);
            }
            return data;
        }

        // False when 'data' is not a path (a hand-made or damaged file); the numbers are limited.
        double finiteOr(double value, double fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        bool fromLLSD(const LLSD& data, Path& result)
        {
            if (!data.isMap() || !data["points"].isArray() || data["format"].asInteger() > FILE_FORMAT)
            {
                return false;
            }
            result.region    = data["region"].asString();
            result.loop      = data["loop"].asBoolean();
            result.countdown = std::clamp(data["countdown"].asInteger(), 0, MAX_COUNTDOWN);
            const S32 kind = data["shake"]["kind"].asInteger();
            result.shake.kind   = kind >= (S32)ShakeKind::Handheld && kind <= (S32)ShakeKind::Quake ? static_cast<ShakeKind>(kind) : ShakeKind::None;
            result.shake.amount = std::clamp(finiteOr(data["shake"]["amount"].asReal(), 0.5), 0.0, 1.0);
            result.shake.speed  = std::clamp(finiteOr(data["shake"]["speed"].asReal(), 0.5), 0.0, 1.0);
            result.shake.seed   = (unsigned)std::clamp(data["shake"]["seed"].asInteger(), 0, (S32)MAX_SEED);
            result.points.clear();
            for (const LLSD& entry : llsd::inArray(data["points"]))
            {
                if (result.points.size() >= MAX_POINTS)
                {
                    break;
                }
                camerapath::Point point;
                point.name     = entry["name"].asString();
                point.position = vector3dFromLLSD(entry["pos"]);
                point.focus    = vector3dFromLLSD(entry["focus"]);
                if (point.position.isExactlyZero() || point.focus.isExactlyZero() || !saneGlobalPlace(point.position)
                    || !saneGlobalPlace(point.focus))
                {
                    continue;   // without a place (or one out of any grid) there is nothing to go to
                }
                const double fov = finiteOr(entry["fov"].asReal(), 0.0);
                point.fov     = fov > 0.0 ? (float)clampFov(fov) : 0.f;
                point.roll    = (float)std::remainder(finiteOr(entry["roll"].asReal(), 0.0), 360.0);   // a tilt of 400 is 40
                point.spin    = std::round(llclamp((float)finiteOr(entry["spin"].asReal(), 0.0), -MAX_SPIN, MAX_SPIN));   // whole turns
                point.seconds = std::clamp(finiteOr(entry["seconds"].asReal(), 3.0), 0.0, MAX_SECONDS);
                point.hold    = std::clamp(finiteOr(entry["hold"].asReal(), 0.0), 0.0, MAX_SECONDS);
                const S32 motion = entry["motion"].asInteger();
                point.motion  = motion == (S32)Motion::Linear ? Motion::Linear
                              : motion == (S32)Motion::Ease   ? Motion::Ease
                              : motion == (S32)Motion::Orbit  ? Motion::Orbit : Motion::Smooth;
                point.orbit   = std::round(llclamp((float)finiteOr(entry["orbit"].asReal(), 0.0), -MAX_ORBIT, MAX_ORBIT));   // whole laps
                point.subject = entry["subject"].asUUID();
                const S32 part = entry["part"].asInteger();
                point.part    = static_cast<plus::camerashot::Part>(std::clamp(part, 0, 2));
                result.points.push_back(point);
            }
            if (result.points.empty())
            {
                result.region.clear();   // the region was where its first point was made; there is none
            }
            return true;
        }

        // While the frames are taken the points stay as they are: the run steps through the timeline it started with
        // (a point removed under it, or all of them, would leave it taking the same picture again and again).
        bool editLocked()
        {
            return sState == State::Exporting;
        }

        // The region the path was made in, after another point became the first: the region the new first point is
        // in, when the viewer knows it (the avatar's region and its neighbours); else the name stays.
        void regionFromFirstPoint()
        {
            if (sPath.points.empty())
            {
                sPath.region.clear();   // the next point added sets it
                return;
            }
            if (const LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(sPath.points.front().position))
            {
                sPath.region = region->getName();
            }
        }

        // The keys act as WASD does: only with the keyboard focus in the world (scanKey skips a key the UI
        // took, and LLViewerWindow::handleKey takes every key while a UI control has the focus; see
        // photokeys.cpp). The check here keeps a key rebound to the numeric keypad, which goes past the UI,
        // from acting while a text control has the focus. Ctrl and Alt are the menus'.
        bool keyIsOurs()
        {
            if (plus::textHasKeyboard())
            {
                return false;
            }
            return !gKeyboard || !(gKeyboard->currentMask(false) & (MASK_CONTROL | MASK_ALT));
        }
    }

    Path& path()
    {
        return sPath;
    }

    void changed()
    {
        rebuild();   // a camera held in place has a timeline of its own: the shake is tuned while it runs
        sSelected = std::min(sSelected, (int)sPath.points.size() - 1);
        sRouteDirty = true;
        ++sRevision;
    }

    unsigned revision()
    {
        return sRevision;
    }

    int selectedPoint()
    {
        return sSelected;
    }

    void selectPoint(int index)
    {
        sSelected = index < (int)sPath.points.size() ? index : (int)sPath.points.size() - 1;
    }

    bool addPoint(std::string& error)
    {
        error.clear();
        if (editLocked())
        {
            error = "path_err_busy";
            return false;
        }
        if (sPath.points.size() >= MAX_POINTS)
        {
            error = "path_err_full";
            return false;
        }
        plus::photomode::CameraView view;
        if (!plus::photomode::currentView(view))
        {
            error = "path_err_no_camera";
            return false;
        }
        camerapath::Point point;
        point.position = view.position;
        point.focus    = view.focus;
        point.fov      = view.fov;
        point.roll     = std::remainder(view.roll, 360.f);   // after a spin the camera may be a whole turn away from level
        if (sPath.points.empty())
        {
            sPath.region = view.region;
        }
        sPath.points.push_back(point);
        sSelected = (int)sPath.points.size() - 1;
        changed();
        return true;
    }

    bool overwritePoint(size_t index, std::string& error)
    {
        error.clear();
        if (editLocked())
        {
            error = "path_err_busy";
            return false;
        }
        plus::photomode::CameraView view;
        if (index >= sPath.points.size() || !plus::photomode::currentView(view))
        {
            error = "path_err_no_camera";
            return false;
        }
        camerapath::Point& point = sPath.points[index];
        point.position = view.position;
        point.focus    = view.focus;
        point.fov      = view.fov;
        point.roll     = std::remainder(view.roll, 360.f);
        if (index == 0)
        {
            sPath.region = view.region;
        }
        changed();
        return true;
    }

    void removePoint(size_t index)
    {
        if (!editLocked() && index < sPath.points.size())
        {
            sPath.points.erase(sPath.points.begin() + index);
            if (index == 0)
            {
                regionFromFirstPoint();
            }
            changed();
        }
    }

    void movePoint(size_t index, int direction)
    {
        const size_t other = direction < 0 ? index - 1 : index + 1;
        if (editLocked() || index >= sPath.points.size() || other >= sPath.points.size())   // also catches index 0 going up (wraps)
        {
            return;
        }
        std::swap(sPath.points[index], sPath.points[other]);
        if (index == 0 || other == 0)
        {
            regionFromFirstPoint();
        }
        sSelected = (int)other;
        changed();
    }

    void duplicatePoint(size_t index)
    {
        if (!editLocked() && index < sPath.points.size() && sPath.points.size() < MAX_POINTS)
        {
            const camerapath::Point copy = sPath.points[index];
            sPath.points.insert(sPath.points.begin() + index + 1, copy);
            sSelected = (int)index + 1;
            changed();
        }
    }

    void clearPath()
    {
        stop();
        sPath     = Path();
        sSelected = -1;
        changed();
    }

    double totalSeconds()
    {
        return sTimeline.total();
    }

    double arrivalSeconds(size_t index)
    {
        return sTimeline.arrival(index);
    }

    bool otherRegion()
    {
        return !sPath.region.empty() && plus::photomode::regionName() != sPath.region;
    }

    State state()
    {
        return sState;
    }

    bool playing()
    {
        return sState != State::Idle;
    }

    bool play(std::string& error)
    {
        error.clear();
        if (sPath.points.empty())
        {
            error = "path_err_empty";
            return false;
        }
        if (!canTakeCamera(error))
        {
            return false;
        }
        finish();
        beginRun();
        ++sRevision;
        if (sPath.countdown > 0)
        {
            // The first picture is shown while the countdown runs; the path starts when it is over.
            sState      = State::Counting;
            sCountEnd   = LLTimer::getTotalSeconds() + sPath.countdown;
            sCountShown = 0;
            applyAt(0.0, 0.0);
        }
        else
        {
            sState     = State::Playing;
            sStartTime = LLTimer::getTotalSeconds();
            applyAt(0.0, 0.0);   // the first picture at once; a path starts with a cut
        }
        updateTicking();
        return true;
    }

    bool spinOnce(double seconds, bool right, std::string& error)
    {
        error.clear();
        if (sState != State::Idle || sSpin.active)
        {
            error = "path_err_busy";
            return false;
        }
        if (!cameraExists())
        {
            error = "path_err_no_camera";
            return false;
        }
        if (flycamOn())
        {
            error = "path_err_flycam";   // the spin steps from currentDegrees(), which has the flycam's own roll in it too
            return false;
        }
        sSpin.seconds     = std::clamp(seconds, 0.5, MAX_SECONDS);
        sSpin.start       = LLTimer::getTotalSeconds();
        const float now_tilt = plus::camroll::currentDegrees();
        sSpin.from        = std::remainder(now_tilt, 360.f);   // the same picture, and room for the turn within the limit
        if (std::fabs(now_tilt - sSpin.from) > 0.01f)
        {
            plus::camroll::setDegrees(sSpin.from);
        }
        sSpin.to          = sSpin.from + (right ? 360.f : -360.f);
        sSpin.lastWritten = sSpin.from;
        sSpin.active      = true;
        ++sRevision;
        updateTicking();
        return true;
    }

    bool spinning()
    {
        return sSpin.active;
    }

    bool playInPlace(std::string& error)
    {
        error.clear();
        plus::photomode::CameraView view;
        if (!canTakeCamera(error))
        {
            return false;
        }
        if (!plus::photomode::currentView(view))
        {
            error = "path_err_no_camera";
            return false;
        }
        finish();
        plus::camerashot::stop();
        takeFovRange();
        sFallbackFov = runFov(view.fov > 0.f ? view.fov : LLViewerCamera::getInstance()->getDefaultFOV());
        // A timeline of the camera as it is now, apart from the path.
        camerapath::Point here;
        here.position = view.position;
        here.focus    = view.focus;
        here.fov      = view.fov;
        here.roll     = view.roll;
        sHeld.set({ keyOf(here) });
        takeCamera();
        sRollSet   = false;
        sState     = State::InPlace;
        sStartTime = LLTimer::getTotalSeconds();
        ++sRevision;
        applyAt(0.0, 0.0);
        updateTicking();
        return true;
    }

    void stop()
    {
        finish();
    }

    double time()
    {
        if (sState != State::Playing)
        {
            return 0.0;
        }
        const double total = sTimeline.total();
        if (total <= 0.0)
        {
            return 0.0;
        }
        return sPath.loop ? std::fmod(elapsed(), total) : std::min(elapsed(), total);
    }

    int countdownLeft()
    {
        return sState == State::Counting ? std::max(sCountShown, 1) : 0;
    }

    bool goToPoint(size_t index, std::string& error)
    {
        error.clear();
        if (!cameraExists() || index >= sPath.points.size())
        {
            error = "path_err_no_camera";
            return false;
        }
        stop();
        plus::camerashot::stop();
        if (flycamOn())
        {
            // A place to go to: the camera is handed back from the flycam, as the photo tools' saved
            // positions do (photocamera.cpp goToView).
            handle_toggle_flycam();
            LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
        }
        takeCamera();
        takeFovRange();
        const camerapath::Point& point = sPath.points[index];
        putCamera(point.position, point.focus,
                  point.fov > 0.f ? point.fov : userFov());
        sRollSet = false;
        putRoll(point.roll);
        return true;
    }

    bool previewAt(double seconds, std::string& error)
    {
        error.clear();
        if (sPath.points.empty())
        {
            error = "path_err_empty";
            return false;
        }
        if (sState != State::Idle)
        {
            return false;   // something runs: the timeline is not scrubbed meanwhile
        }
        if (!canTakeCamera(error))
        {
            return false;
        }
        beginRun();
        applyAt(std::clamp(seconds, 0.0, sTimeline.total()), seconds);
        wrapRoll();   // each call sets the picture afresh: a spin's 400 degrees show as 40
        return true;
    }

    bool startExport(int fps, std::string& error)
    {
        error.clear();
        if (sPath.points.empty())
        {
            error = "path_err_empty";
            return false;
        }
        if (!canTakeCamera(error))
        {
            return false;
        }
        if (plus::photomode::shotState() != plus::photomode::ShotState::Idle)
        {
            error = "path_err_export_busy";
            return false;
        }
        fps = std::clamp(fps, EXPORT_MIN_FPS, EXPORT_MAX_FPS);
        const double total = sTimeline.total();
        if (total <= 0.0)
        {
            error = "path_err_short";   // nothing moves: a picture is just a picture
            return false;
        }
        const double frames = std::ceil(total * fps) + 1.0;
        if (frames > EXPORT_MAX_FRAMES)
        {
            error = "path_err_too_long";   // said before it starts, not cut short silently at the end
            return false;
        }
        finish();
        sExport = Export();
        sExport.fps    = fps;
        sExport.frames = (int)frames;
        beginRun();
        sState = State::Exporting;
        ++sRevision;
        applyAt(0.0, 0.0);
        updateTicking();
        return true;
    }

    void exportProgress(int& frame, int& frames)
    {
        const bool on = sState == State::Exporting;
        frame  = on ? sExport.frame : 0;
        frames = on ? sExport.frames : 0;
    }

    std::string exportFolder()
    {
        return sExport.folder;
    }

    const std::vector<std::vector<LLVector3d>>& route()
    {
        if (sRouteDirty)
        {
            buildRoute();
        }
        return sRoute;
    }

    void setShowRoute(bool show)
    {
        sShowRoute = show;
    }

    bool showRoute()
    {
        return sShowRoute;
    }

    void setEditorOpen(bool open)
    {
        sEditorOpen = open;
    }

    bool routeVisible()
    {
        return sEditorOpen && sShowRoute && sState == State::Idle && !sPath.points.empty();
    }

    std::vector<std::string> savedNames()
    {
        std::vector<std::string> names;
        const std::string dir = folder();
        if (LLFile::isdir(dir))
        {
            std::string file;
            LLDirIterator xml(dir, std::string("*") + SUFFIX);
            while (xml.next(file))
            {
                if (file.size() > strlen(SUFFIX))
                {
                    names.push_back(file.substr(0, file.size() - strlen(SUFFIX)));
                }
            }
            LLDirIterator scripts(dir, std::string("*") + SCRIPT_SUFFIX);
            while (scripts.next(file))
            {
                names.push_back(file);   // with its suffix: that is what says it is a script
            }
        }
        std::sort(names.begin(), names.end(),
                  [](const std::string& a, const std::string& b) { return LLStringUtil::compareDictInsensitive(a, b) < 0; });
        return names;
    }

    bool save(const std::string& name_in, std::string& error)
    {
        error.clear();
        std::string name = name_in;
        LLStringUtil::trim(name);
        name = LLDir::getScrubbedFileName(name);
        // Control characters cannot be in a Windows file name (a tab pasted into the box made the write fail).
        std::replace_if(name.begin(), name.end(), [](char c) { return (unsigned char)c < 0x20; }, '_');
        if (utf8str_to_utf16str(name).size() > MAX_NAME_CHARS)
        {
            error = "path_err_name";
            return false;
        }
        if (isScriptName(name))
        {
            // The list tells a script from a saved path by this suffix.
            name = name.substr(0, name.size() - strlen(SCRIPT_SUFFIX)) + "_json";
        }
        if (name.empty())
        {
            error = "path_err_name";
            return false;
        }
        if (reservedOnWindows(name))
        {
            error = "path_err_name_reserved";
            return false;
        }
        const std::string dir = folder();
        if (!LLFile::isdir(dir) && LLFile::mkdir(dir) != 0)
        {
            error = "path_err_write";
            return false;
        }
        // Written next to the file, then moved over it: a failed write leaves an existing file as it was.
        const std::string file = fileFor(name);
        if (plus::writeFileReplacing(file, [](std::ostream& out) { LLSDSerialize::toPrettyXML(toLLSD(sPath), out); }) != plus::FileWrite::written)
        {
            error = "path_err_write";
            return false;
        }
        sPath.name = name;
        ++sRevision;
        return true;
    }

    namespace
    {
        // A path file (.xml, as save() writes) or a script (.json) into 'loaded', which becomes the open path named 'name'.
        bool openFile(const std::string& file, bool script, const std::string& name, std::string& error)
        {
            error.clear();
            Path loaded;
            if (script)
            {
                if (!importScript(file, loaded, error))
                {
                    return false;
                }
            }
            else
            {
                const S64 bytes = LLFile::size(file);
                if (bytes <= 0 || bytes > MAX_FILE_BYTES)
                {
                    error = "path_err_read";
                    return false;
                }
                LLSD data;
                llifstream stream(file.c_str());
                if (!stream.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(data, stream) || !fromLLSD(data, loaded))
                {
                    error = "path_err_read";
                    return false;
                }
            }
            stop();
            loaded.name = LLDir::getScrubbedFileName(name);
            sPath       = std::move(loaded);
            sSelected   = sPath.points.empty() ? -1 : 0;
            changed();
            return true;
        }
    }

    bool load(const std::string& name, std::string& error)
    {
        return openFile(fileFor(name), isScriptName(name), name, error);
    }

    bool loadFile(const std::string& file, std::string& error)
    {
        return openFile(file, isScriptName(file), gDirUtilp->getBaseFileName(file, true), error);
    }

    bool removeSaved(const std::string& name)
    {
        const bool removed = LLFile::remove(fileFor(name), ENOENT) == 0;
        ++sRevision;
        return removed;
    }

    bool keyAddPoint(EKeystate state)
    {
        if (state != KEYSTATE_DOWN || !keyIsOurs())
        {
            return false;
        }
        std::string error;
        if (addPoint(error))
        {
            say("PlusCameraPathAdded", std::to_string(sPath.points.size()));
        }
        else
        {
            say(error == "path_err_full"   ? "PlusCameraPathFull"
                : error == "path_err_busy" ? "PlusCameraPathExporting"
                                           : "PlusCameraPathNoCamera");
        }
        return true;
    }

    bool keyTogglePlay(EKeystate state)
    {
        if (state != KEYSTATE_DOWN || !keyIsOurs())
        {
            return false;
        }
        if (sState != State::Idle)
        {
            stop();
            say("PlusCameraPathStopped");
            return true;
        }
        std::string error;
        if (play(error))
        {
            if (sState == State::Playing)
            {
                say("PlusCameraPathPlaying");
            }
        }
        else
        {
            say(error == "path_err_empty"    ? "PlusCameraPathEmpty"
                : error == "path_err_flycam" ? "PlusCameraPathFlycam"
                                             : "PlusCameraPathNoCamera");
        }
        return true;
    }
}
