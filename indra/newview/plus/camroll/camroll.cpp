/**
 * @file camroll.cpp
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

// The official LLAgentCamera has a roll of its own (mRollAngle; it stays 0 because llviewerinput.cpp
// leaves out the keys that set it) and, at the very end of LLAgentCamera::updateCamera(), turns the
// camera's axes by it (the official code):
//     LLMatrix3 rot_mat(mRollAngle, 0.f, 0.f); rot_quat = LLQuaternion(rot_mat) * rot_quat; ... mXAxis, mYAxis, mZAxis
// That is a turn about the forward axis, as LLCoordFrame::roll() is ("RH rotation about mXAxis"),
// so this calls that. It is done at the end because everything the camera is made of (the focus
// offset, the look-at, the avatar's head) has been worked out by then and the tilt goes on top of
// it. The hooks zero the roll where the camera is reset. The roll is kept in the setting
// PlusCameraRoll, so other code can set it without touching LLAgentCamera, and it is not saved
// with the account (the photo tools keep it in each saved position, shared by every account).
// The flycam (LLAgentCamera::updateFlycam()) gets the same tilt on top of its own attitude
// (applyFlycam()), so one value tilts both cameras and the flycam stays on when it changes
// (flycam-keys).

#include "llviewerprecompiledheaders.h"

#include "camroll.h"

#include <cmath>

#include "plus.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llcontrol.h"
#include "llcoordframe.h"
#include "llframetimer.h"
#include "llpanel.h"
#include "lltextbox.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"

namespace plus::camroll
{
    namespace
    {
        constexpr char SETTING_ROLL[] = "PlusCameraRoll";
        constexpr F64  FOCUS_DEFAULT = 10.0; // m ahead, when the agent camera's focus is too near to keep
        constexpr F64  FOCUS_MIN     = 0.5;

        bool sResetting = false; // reset(): the camera is being put back on the avatar anyway
        F32  sShake     = 0.f;   // setShakeDegrees(): on top of the setting, for apply() only
        F32  sOnCamera  = 0.f;   // the tilt the camera's axes have now (put there by tiltCamera()), for flycamStart()

        // The setting, read every frame (apply(), the photo guide's horizon): cached, not looked up by name.
        LLCachedControl<F32>& rollSetting()
        {
            static LLCachedControl<F32> roll(gSavedSettings, SETTING_ROLL, 0.f);
            return roll;
        }

        // The setting to write (setDegrees(), every frame while a tilt key is held): looked up once, not by name.
        LLControlVariable* rollControl()
        {
            static LLControlVariablePtr control;
            if (control.isNull())
            {
                control = gSavedSettings.getControl(SETTING_ROLL); // tried again while the settings are not there yet
            }
            return control.get();
        }

        // A NaN or infinite roll (a broken settings file, a saved position) passes llclamp():
        // level instead.
        F32 finiteOrLevel(F32 degrees)
        {
            return llfinite(degrees) ? degrees : 0.f;
        }

        // The camera's view as it is now, made the agent camera's (a free camera, no glide): what the
        // flycam shows, or the flycam's view it left behind (kept until the avatar moves,
        // LLViewerJoystick::toggleFlycam()). Without it, making the camera again for the tilt would
        // jump back to where it was before the flycam.
        void holdCameraWhereItIs()
        {
            const LLViewerCamera* camera   = LLViewerCamera::getInstance();
            const LLVector3d      position = gAgent.getPosGlobalFromAgent(camera->getOrigin());
            F64                   distance = (gAgentCamera.getFocusGlobal() - position).magVec();
            if (distance < FOCUS_MIN)
            {
                distance = FOCUS_DEFAULT;
            }
            gAgentCamera.setFocusOnAvatar(false, false);
            gAgentCamera.setCameraPosAndFocusGlobal(position, position + LLVector3d(camera->getAtAxis()) * distance, LLUUID::null);
            gAgentCamera.stopCameraAnimation();
        }

        // Whoever wrote the setting (the photo tools' slider, their keys, the flycam's Q / E, a saved camera
        // position): the tilt shows. While the flycam is on, it goes on top of the flycam's attitude every
        // frame (applyFlycam()), so the flycam stays on (a saved camera position hands the camera back
        // itself, photocamera.cpp goToView()). Just after the flycam, the camera is not made again until
        // the avatar moves (apply() could not tilt it): it is made again now, from where it is.
        void onRollChanged(F32 degrees)
        {
            if (sResetting || !gAgent.getRegion() || gAgentCamera.cameraMouselook())
            {
                return;
            }
            LLViewerJoystick* joystick = LLViewerJoystick::getInstance();
            if (joystick->getOverrideCamera() || joystick->getCameraNeedsUpdate())
            {
                return; // made again every frame: applyFlycam() or apply() tilts it
            }
            holdCameraWhereItIs();
            joystick->setCameraNeedsUpdate(true);
        }

        // The tilt on top of the camera this frame: the setting (and a camera path's shake), turned onto the
        // camera's axes that were just made. Remembers it (sOnCamera) for flycamStart().
        void tiltCamera()
        {
            const F32 degrees = finiteOrLevel(rollSetting()) + sShake;
            if (fabsf(degrees) < 0.01f)
            {
                sOnCamera = 0.f;
                return;
            }
            // positive = the picture leans to the right (camroll.h): the camera's up axis turns toward its left
            // (LLCoordFrame::roll(a) gives up' = up*cos(a) - left*sin(a)). currentDegrees() measures the same way.
            LLViewerCamera::getInstance()->roll(-degrees * DEG_TO_RAD);
            sOnCamera = degrees;
        }

        // The camera controls' tilt row (drawCameraControls()). One camera floater at a time; the
        // children are looked up again when the panel is another one (the floater was built again).
        struct ControlsRow
        {
            LLHandle<LLPanel> panel;
            LLButton*         left  = nullptr;
            LLButton*         right = nullptr;
            LLTextBox*        value = nullptr;
            S32               shown_degrees = S32_MIN; // what value shows (S32_MIN: nothing yet)
            S32               shown_enabled = -1;      // the buttons' state (-1: not set yet)
        };
        ControlsRow sRow;

        // "left" tilts to the left (negative), anything else to the right, as the keys do (photokeys).
        F32 directionOf(const LLSD& param)
        {
            return param.asString() == "left" ? -1.f : 1.f;
        }

        void init()
        {
            if (LLControlVariable* control = gSavedSettings.getControl(SETTING_ROLL))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                                              { onRollChanged(finiteOrLevel((F32)value.asReal())); });
            }

            // The camera controls' tilt buttons (floater_camera.xml). Registered here, before any floater is built.
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.CamRoll.Step",
                [](LLUICtrl*, const LLSD& param) { nudge(directionOf(param) * STEP_DEGREES); });
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.CamRoll.Hold",
                [](LLUICtrl*, const LLSD& param)
                { nudge(directionOf(param) * HOLD_DEGREES_PER_SECOND * LLFrameTimer::getFrameDeltaTimeF32()); });
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.CamRoll.Level",
                [](LLUICtrl*, const LLSD&) { setDegrees(0.f); });
        }

        plus::FeatureRegistration sRegistration({ "camroll", &init, {} });
    }

    void apply()
    {
        LLViewerJoystick* joystick = LLViewerJoystick::getInstance();
        if (joystick->getOverrideCamera())
        {
            return; // the flycam's own frame tilts it (applyFlycam())
        }
        if (!joystick->getCameraNeedsUpdate())
        {
            // The camera's axes were not made again this frame (LLViewerCamera::updateCameraLocation
            // returns at once while this is false: after the flycam is turned off, until the agent
            // moves): they still have last frame's tilt, and turning them again would add to it
            // every frame (the view would spin).
            return;
        }
        tiltCamera();
    }

    void applyFlycam()
    {
        tiltCamera(); // the flycam made the camera's axes again just now, from its own attitude
    }

    LLQuaternion flycamStart()
    {
        // The camera's axes have the tilt on top (tiltCamera(), this frame or an earlier one): turned back
        // by it, the flycam starts from the attitude under it, and applyFlycam() puts the tilt on again
        // (without this the tilt would show twice).
        LLCoordFrame frame(LLViewerCamera::getInstance()->getQuaternion());
        if (fabsf(sOnCamera) >= 0.01f)
        {
            frame.roll(sOnCamera * DEG_TO_RAD);
        }
        return frame.getQuaternion();
    }

    void reset()
    {
        // The movement keys call it every frame (research/125 #23): while the
        // roll is 0 there is nothing to set (setting 0 again changed nothing).
        const F32 degrees = rollSetting();
        if (degrees != 0.f || !llfinite(degrees)) // NaN too (/fp:fast: a NaN compare is not kept): written level
        {
            sResetting = true;
            gSavedSettings.setF32(SETTING_ROLL, 0.f);
            sResetting = false;
        }
    }

    float currentDegrees()
    {
        if (!LLViewerJoystick::getInstance()->getOverrideCamera())
        {
            return finiteOrLevel(rollSetting()); // every frame from the photo guide's horizon: cached
        }
        // The flycam: how far its camera is turned from level, measured from its axes. Level means the
        // camera's up axis lies in the plane of the forward axis and the world's up.
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 at   = camera->getAtAxis();
        const LLVector3 up   = camera->getUpAxis();
        LLVector3       left = LLVector3::z_axis % at; // cross product: the level camera's left
        if (left.magVecSquared() < 1e-6f)
        {
            return 0.f; // looking straight up or down: no horizon to measure against
        }
        left.normalize();
        const LLVector3 level_up = at % left;
        // In apply()'s sense, so that setDegrees(currentDegrees()) gives the same picture: apply() turns the
        // level camera by roll(-d), which leaves its up axis at level_up*cos(d) + left*sin(d) (leaning to the
        // camera's left for a positive d); this gives that d back (-180..180).
        return atan2f(up * left, up * level_up) * RAD_TO_DEG;
    }

    float tiltDegrees()
    {
        return finiteOrLevel(rollSetting());
    }

    void setDegrees(float degrees)
    {
        // The flycam and the camera it left behind: onRollChanged().
        if (LLControlVariable* control = rollControl())
        {
            control->setValue(LLSD((F32)llclamp(finiteOrLevel(degrees), -MAX_WIDE_DEGREES, MAX_WIDE_DEGREES)));
        }
    }

    void setShakeDegrees(float degrees)
    {
        sShake = llclamp(finiteOrLevel(degrees), -MAX_DEGREES, MAX_DEGREES); // a shake is a few degrees
    }

    float stepped(float now_degrees, float delta_degrees, float limit_degrees)
    {
        const F32 now   = finiteOrLevel(now_degrees);
        const F32 limit = llmin(llmax(limit_degrees, fabsf(now)), MAX_WIDE_DEGREES);
        return llclamp(now + finiteOrLevel(delta_degrees), -limit, limit);
    }

    void nudge(float delta_degrees)
    {
        if (gAgentCamera.cameraMouselook())
        {
            return;
        }
        // From the tilt on top (the setting): with the flycam, its own roll (a game pad's, a 3D mouse's) is
        // not in it, and taking it in would add that roll again on top.
        const F32 now  = tiltDegrees();
        const F32 next = stepped(now, delta_degrees, CONTROLS_MAX_DEGREES);
        if (next != now)
        {
            setDegrees(next);
        }
    }

    void drawCameraControls(LLPanel& zoom_panel)
    {
        if (sRow.panel.get() != &zoom_panel)
        {
            sRow       = ControlsRow();
            sRow.panel = zoom_panel.getHandle();
            sRow.left  = zoom_panel.findChild<LLButton>("roll_left_btn");
            sRow.right = zoom_panel.findChild<LLButton>("roll_right_btn");
            sRow.value = zoom_panel.findChild<LLTextBox>("roll_value");
        }

        const S32 enabled = gAgentCamera.cameraMouselook() ? 0 : 1;
        if (enabled != sRow.shown_enabled)
        {
            sRow.shown_enabled = enabled;
            if (sRow.left)
            {
                sRow.left->setEnabled(enabled != 0);
            }
            if (sRow.right)
            {
                sRow.right->setEnabled(enabled != 0);
            }
        }

        const S32 degrees = (S32)lroundf(currentDegrees());
        if (degrees != sRow.shown_degrees && sRow.value)
        {
            sRow.shown_degrees = degrees;
            sRow.value->setText(llformat("%d\xC2\xB0", degrees)); // degree sign, UTF-8
        }
    }
}
