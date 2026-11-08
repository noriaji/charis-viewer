/**
 * @file flycammouse.cpp
 * @brief The flycam on the keyboard and mouse: look with the mouse, wheel speed, Shift boost, gentle keys.
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

#include "llviewerprecompiledheaders.h"

#include "flycammouse.h"

#include "plus/camroll/camroll.h"
#include "plus/plus.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llnotifications.h"
#include "llmenugl.h"
#include "llmodaldialog.h"
#include "llrootview.h"
#include "llnotificationsutil.h"
#include "lltoolmgr.h"
#include "lltrans.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewercamera.h"
#include "llviewerjoystick.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <algorithm>
#include <cmath>

namespace plus::flycammouse
{
    namespace
    {
        constexpr char SETTING_MODE[]  = "PlusFlycamMouseMode";
        constexpr char SETTING_KEEP_VIEW[] = "PlusFlycamKeepView";
        constexpr F64 KEEP_FOCUS_METERS = 8.0;   // how far ahead the camera looks once the flycam is left (the point it orbits and zooms about)
        constexpr char SETTING_SENS[]  = "PlusFlycamMouseSens";
        constexpr char SETTING_BOOST[] = "PlusFlycamBoost";
        constexpr char SETTING_SMOOTH[] = "PlusFlycamSmooth";
        constexpr char SETTING_SPEED[] = "PlusFlycamSpeedMul";
        constexpr char SETTING_ROLL_SPEED[] = "PlusFlycamRollSpeed";

        constexpr F32 SPEED_MIN       = 0.05f;
        constexpr F32 SPEED_MAX       = 50.f;
        constexpr F32 WHEEL_STEP      = 1.15f;   // per notch
        constexpr F32 BOOST_MAX       = 20.f;
        constexpr F32 BOOST_EASE_TIME = 0.2f;    // seconds for the Shift boost to come and go
        constexpr F32 SMOOTH_MAX      = 2.f;     // seconds
        constexpr S32 MAX_CHANNELS    = 8;
        constexpr F32 ROLL_SPEED_MIN  = 1.f;     // degrees a second (the photo tools' slider), so Q / E always tilt
        constexpr F32 ROLL_SPEED_MAX  = 90.f;    // as the photo tools' tilt keys (PlusCameraRollKeySpeed)
        constexpr F32 ROLL_FAST       = 3.f;     // with Shift, as the photo tools' tilt keys (photokeys.cpp FAST)
        // The tilt keys start at most this gently and stop within this (seconds), whatever PlusFlycamSmooth says: framing a
        // picture wants the tilt to stop where the key is let go
        constexpr F32 ROLL_START_MAX  = 0.1f;
        constexpr F32 ROLL_STOP_TIME  = 0.03f;
        constexpr F32 ROLL_STOP_LEFT  = 0.05f;   // the smoothed key is let go of below this (no coasting on)
        constexpr F32 ROLL_MAX_STEP_TIME = 0.1f; // seconds of a frame at most: a long frame does not tilt a long way at once
        // the right button is a click (not a look) when released within this time and this far from the press
        constexpr F64 CLICK_TIME      = 0.2;
        constexpr S32 CLICK_DISTANCE  = 4;
        // LLToolGun's mouse look, so the flycam turns as far for a move of the mouse as the mouselook does
        constexpr F32 NOMINAL_MOUSE_SENSITIVITY = 0.0025f;

        enum class Hold { None, Pending, Looking };

        Hold    sHold = Hold::None;
        F64     sPressTime = 0.0;
        S32     sPressX = 0;
        S32     sPressY = 0;
        bool    sReplaying = false;         // the click we give back to the viewer is not ours to take again
        bool    sCursorTaken = false;       // we hid the cursor and keep it at the middle of the world view
        bool    sButtonDown = false;        // the right button we took is still down
        F32     sSmoothed[MAX_CHANNELS] = {};
        F32     sBoost = 1.f;
        F32     sRollKey = 0.f;             // keyRoll() this frame: -1 (Q, to the left), 1 (E, to the right) or 0
        F32     sRollSmoothed = 0.f;        // the tilt keys, started and stopped gently
        F32     sRollBoost = 1.f;           // Shift's ROLL_FAST, eased as the move boost (sBoost)
        LLNotificationPtr sSpeedTip;
        bool    sTurningOn = false;         // toggle() is turning the flycam on
        bool    sOurs = false;              // the flycam that is on was turned on by toggle() (no 3D mouse): isOurs()

        bool keyboardFocusIsText()
        {
            const LLUICtrl* focus = dynamic_cast<const LLUICtrl*>(gFocusMgr.getKeyboardFocus());
            return focus && focus->acceptsTextInput();
        }

        void takeCursor()
        {
            sCursorTaken = true;
            gViewerWindow->moveCursorToCenter();
            // The photo screen moves the world view off the window's centre: where the cursor was warped (not with
            // MouseWarpMode 2), it goes to the world view's centre, not over the screen's panels (their clicks
            // and wheel would take the flycam's).
            if (gViewerWindow->getCurrentMouseX() == gViewerWindow->getWorldViewWidthScaled() / 2
                && gViewerWindow->getCurrentMouseY() == gViewerWindow->getWorldViewHeightScaled() / 2)
            {
                const LLRect world = gViewerWindow->getWorldViewRectScaled();
                gViewerWindow->moveCursorTo(world.getCenterX(), world.getCenterY());
            }
            if (!gViewerWindow->getCursorHidden())
            {
                gViewerWindow->hideCursor();
            }
        }

        void giveCursor()
        {
            if (sCursorTaken)
            {
                sCursorTaken = false;
                // the mouselook keeps its cursor hidden (and the flycam can be entered from it)
                if (!gAgentCamera.cameraMouselook())
                {
                    gViewerWindow->showCursor();
                }
            }
        }

        void endHold()
        {
            sHold = Hold::None;
            sButtonDown = false;
            giveCursor();
        }

        // A popup has the keyboard: a combo's open list (LLUI::addPopup() puts it under the popup holder of the main view,
        // and the keyboard goes to its list; nothing here sets gFocusMgr's top control)
        bool focusInPopup()
        {
            static LLHandle<LLView> holder;
            static bool             looked = false;     // the holder lives as long as the main view: looked up once
            if (!looked && gViewerWindow && gViewerWindow->getRootView())
            {
                looked = true;
                if (LLView* found = gViewerWindow->getRootView()->findChildView("popup_holder", true))
                {
                    holder = found->getHandle();
                }
            }
            const LLView* popups = holder.get();
            if (!popups)
            {
                return false;
            }
            for (const LLView* view = dynamic_cast<const LLView*>(gFocusMgr.getKeyboardFocus()); view; view = view->getParent())
            {
                if (view == popups)
                {
                    return true;
                }
            }
            return false;
        }

        // Mode 2: the mouse looks whenever the flycam is on and nothing else wants the mouse
        bool alwaysLooking()
        {
            return gKeyboard && !(gKeyboard->currentMask(false) & MASK_ALT)
                && gFocusMgr.getAppHasFocus()
                && !gFocusMgr.getMouseCapture()
                && !gFocusMgr.getTopCtrl()
                && !focusInPopup()                                     // a combo's open list
                && !(gMenuHolder && gMenuHolder->hasVisibleMenu())   // the pie menu and the context menus
                && !keyboardFocusIsText()
                && !LLToolMgr::getInstance()->inBuildMode();
        }

        void showSpeed(F32 speed)
        {
            if (sSpeedTip && !sSpeedTip->isCancelled())
            {
                LLNotifications::instance().cancel(sSpeedTip);
            }
            LLStringUtil::format_map_t args;
            args["[SPEED]"] = llformat("%.2f", speed);
            sSpeedTip = LLNotificationsUtil::add("SystemMessageTip",
                                                 LLSD().with("MESSAGE", LLTrans::getString("PlusFlycamSpeed", args)));
        }

        // The settings on the flycam's tab in Preferences > Game Controls (panel_plus_prefs_flycam_mouse.xml).
        // With the signal, so the tab's controls and the cached controls follow; Cancel there puts them back
        // (not the flying speed: see bindSpeedSlider()).
        constexpr const char* PREFS_SETTINGS[] =
        {
            SETTING_MODE, SETTING_SENS, SETTING_SPEED, SETTING_BOOST, SETTING_SMOOTH, SETTING_ROLL_SPEED,
        };

        // The flying speed's slider on that tab has no control_name: the wheel sets the speed while the flycam is on,
        // also while Preferences is open, and Cancel or the close button would put back the speed of when it opened
        // (research/275 L071). The slider writes the setting when moved and follows it (the wheel) while it is there.
        // What the slider or Restore Defaults changes is put back by Cancel as the other settings are: the speed
        // before the first such change is kept (sSpeedBefore) until OK (preferencesApplied()) or Cancel
        // (preferencesCancelled()); a turn of the wheel after it makes the wheel's speed the one that stays.
        boost::signals2::scoped_connection sSpeedSliderFollow;
        bool sSpeedChangedInPrefs = false;
        F32  sSpeedBefore = 1.f;

        // The slider or Restore Defaults is about to change the speed.
        void noteSpeedBeforePrefsChange()
        {
            if (!sSpeedChangedInPrefs)
            {
                sSpeedBefore = gSavedSettings.getF32(SETTING_SPEED);
                sSpeedChangedInPrefs = true;
            }
        }

        void resetPrefs()
        {
            noteSpeedBeforePrefsChange();
            for (const char* name : PREFS_SETTINGS)
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        }

        void bindSpeedSlider(LLUICtrl* slider)
        {
            LLControlVariable* control = gSavedSettings.getControl(SETTING_SPEED);
            if (!slider || !control)
            {
                return;
            }
            slider->setValue(control->getValue());
            LLHandle<LLUICtrl> handle = slider->getHandle();
            sSpeedSliderFollow = control->getSignal()->connect([handle](LLControlVariable*, const LLSD& value, const LLSD&)
            {
                LLUICtrl* follower = handle.get();
                if (follower && follower->getValue().asReal() != value.asReal())
                {
                    follower->setValue(value);
                }
            });
        }

        void setSpeedFromSlider(LLUICtrl* slider)
        {
            const F32 speed = llclamp((F32)slider->getValue().asReal(), SPEED_MIN, SPEED_MAX);
            if (speed != gSavedSettings.getF32(SETTING_SPEED))
            {
                noteSpeedBeforePrefsChange();
                gSavedSettings.setF32(SETTING_SPEED, speed);
            }
        }

        void init()
        {
            // registered before any floater is built (the preferences build their panels later)
            LLUICtrl::CommitCallbackRegistry::Registrar& registrar = LLUICtrl::CommitCallbackRegistry::defaultRegistrar();
            registrar.add("Plus.FlycamMouse.ResetPrefs", [](LLUICtrl*, const LLSD&) { resetPrefs(); });
            registrar.add("Plus.FlycamMouse.BindSpeed", [](LLUICtrl* slider, const LLSD&) { bindSpeedSlider(slider); });
            registrar.add("Plus.FlycamMouse.SetSpeed", [](LLUICtrl* slider, const LLSD&) { setSpeedFromSlider(slider); });
        }

        plus::FeatureRegistration sRegistration({ "flycammouse", &init, {} });
    }

    Frame update(F32 delta_time, F32* keys, S32 count)
    {
        static LLCachedControl<S32> mode_setting(gSavedSettings, SETTING_MODE, 1);
        static LLCachedControl<F32> sens_setting(gSavedSettings, SETTING_SENS, 1.f);
        static LLCachedControl<F32> boost_setting(gSavedSettings, SETTING_BOOST, 3.f);
        static LLCachedControl<F32> smooth_setting(gSavedSettings, SETTING_SMOOTH, 0.15f);
        static LLCachedControl<F32> speed_setting(gSavedSettings, SETTING_SPEED, 1.f);
        static LLCachedControl<F32> roll_speed_setting(gSavedSettings, SETTING_ROLL_SPEED, 30.f);
        static LLCachedControl<F32> mouse_sens_setting(gSavedSettings, "MouseSensitivity", 3.f);
        static LLCachedControl<bool> invert_setting(gSavedSettings, "InvertMouse", false);

        Frame frame;

        // The keys start and stop gently: each channel follows its key with a time constant
        const F32 smooth = llclamp((F32)smooth_setting, 0.f, SMOOTH_MAX);
        const F32 follow = smooth > 0.f ? llmin(1.f, delta_time / smooth) : 1.f;
        const S32 channels = llmin(count, MAX_CHANNELS);
        for (S32 i = 0; i < channels; ++i)
        {
            sSmoothed[i] += (keys[i] - sSmoothed[i]) * follow;
            if (keys[i] == 0.f && std::fabs(sSmoothed[i]) < 0.001f)
            {
                sSmoothed[i] = 0.f;
            }
            keys[i] = sSmoothed[i];
        }

        // Speed: the wheel's factor, and Shift for the boost (eased, so it does not jump)
        const bool boosting = gKeyboard && (gKeyboard->currentMask(false) & MASK_SHIFT) && !keyboardFocusIsText();
        const F32 boost_target = boosting ? llclamp((F32)boost_setting, 1.f, BOOST_MAX) : 1.f;
        sBoost += (boost_target - sBoost) * llmin(1.f, delta_time / BOOST_EASE_TIME);
        frame.move_scale = llclamp((F32)speed_setting, SPEED_MIN, SPEED_MAX) * sBoost;

        // The tilt keys (Q / E) step the photo tools' tilt (plus/camroll), which goes on top of the flycam: in
        // degrees a second (Shift eased in and out as the move boost), starting gently and stopping where the key is let
        // go, within the photo tools' +-45 (a wider tilt, a camera path's, is not pulled in). The setting is written only
        // while it changes; nothing runs while neither key is held.
        const F32 roll_key = sRollKey;
        sRollKey = 0.f;
        if (roll_key != 0.f || sRollSmoothed != 0.f)
        {
            const F32 roll_time = llmin(delta_time, ROLL_MAX_STEP_TIME);
            const F32 roll_ease = roll_key != 0.f ? llmin(smooth, ROLL_START_MAX) : ROLL_STOP_TIME;
            // the same at any frame rate (dt / ease would stop at once at 30 fps and coast further at 144 fps)
            sRollSmoothed += (roll_key - sRollSmoothed) * (roll_ease > 0.f ? 1.f - std::exp(-roll_time / roll_ease) : 1.f);
            if (roll_key == 0.f && std::fabs(sRollSmoothed) < ROLL_STOP_LEFT)
            {
                sRollSmoothed = 0.f;
                sRollBoost = 1.f;   // the next press starts from the plain speed
            }
            sRollBoost += ((boosting ? ROLL_FAST : 1.f) - sRollBoost) * llmin(1.f, roll_time / BOOST_EASE_TIME);
            const F32 rate = llclamp((F32)roll_speed_setting, ROLL_SPEED_MIN, ROLL_SPEED_MAX) * sRollBoost;
            const F32 now  = plus::camroll::tiltDegrees();
            // No limit: the tilt turns all the way round, kept within +-180 degrees (the same picture)
            const F32 next = std::remainder(now + sRollSmoothed * rate * roll_time, 360.f);
            if (next != now)
            {
                plus::camroll::setDegrees(next);
            }
        }

        // The mouse looks
        const S32 mode = mode_setting;
        if (mode == 0)
        {
            if (sHold != Hold::None || sCursorTaken)
            {
                endHold();
            }
            return frame;
        }

        if (sHold == Hold::Pending)
        {
            const S32 moved = std::abs(gViewerWindow->getCurrentMouseX() - sPressX)
                            + std::abs(gViewerWindow->getCurrentMouseY() - sPressY);
            if (moved > CLICK_DISTANCE || LLFrameTimer::getElapsedSeconds() - sPressTime > CLICK_TIME)
            {
                sHold = Hold::Looking;
                takeCursor();
            }
        }

        bool looking = false;
        if (sHold == Hold::Looking)
        {
            // the button went up without our seeing it (the focus was lost), or the app lost the focus
            if (!sButtonDown || !gFocusMgr.getAppHasFocus())
            {
                endHold();
            }
            else
            {
                looking = true;
            }
        }
        else if (sHold == Hold::None && mode == 2)
        {
            if (alwaysLooking())
            {
                looking = true;
                if (!sCursorTaken)
                {
                    takeCursor();
                }
            }
            else
            {
                giveCursor();
            }
        }
        else if (sHold == Hold::None)
        {
            giveCursor();   // the mode went from 2 to 1 while the cursor was held
        }

        if (looking)
        {
            const F32 mouse_sens = clamp_rescale((F32)mouse_sens_setting, 0.f, 15.f, 0.5f, 2.75f)
                * NOMINAL_MOUSE_SENSITIVITY * llclamp((F32)sens_setting, 0.05f, 10.f);
            // LLToolGun's signs: the mouse to the right turns right, the mouse up looks up (its DY is positive upwards).
            // LLFlycam's angles: positive yaw is to the left, positive pitch is downwards (checked on LLFlycam::integrate())
            frame.look_yaw   = -(F32)gViewerWindow->getCurrentMouseDX() * mouse_sens;
            frame.look_pitch = -(F32)gViewerWindow->getCurrentMouseDY() * mouse_sens * (invert_setting ? -1.f : 1.f);
            takeCursor();   // back to the middle (it also zeroes the delta)
        }
        return frame;
    }

    bool pressRight(LLWindow* window, LLCoordGL pos, MASK mask, S32 x, S32 y)
    {
        if (sReplaying || !gAgentCamera.isUsingFlycam() || gSavedSettings.getS32(SETTING_MODE) != 1
            || (mask & (MASK_CONTROL | MASK_ALT)) || LLToolMgr::getInstance()->inBuildMode())
        {
            return false;
        }
        sHold = Hold::Pending;
        sButtonDown = true;
        sPressTime = LLFrameTimer::getElapsedSeconds();
        sPressX = x;
        sPressY = y;
        return true;
    }

    bool releaseRight(LLWindow* window, LLCoordGL pos, MASK mask)
    {
        if (sHold == Hold::None)
        {
            return false;
        }
        const bool was_click = sHold == Hold::Pending;
        endHold();
        // the press took the mouse capture and the release is ours: let go of it
        if (window)
        {
            window->releaseMouse();
            window->setMouseClipping(false);
        }
        if (was_click && window)
        {
            // A short click is the viewer's right click (the pie menu): give it the press and the release it
            // did not get
            sReplaying = true;
            gViewerWindow->handleRightMouseDown(window, pos, mask);
            gViewerWindow->handleRightMouseUp(window, pos, mask);
            sReplaying = false;
        }
        return true;
    }

    bool onWheel(S32 clicks)
    {
        if (clicks == 0 || !gAgentCamera.isUsingFlycam() || !gKeyboard || gKeyboard->currentMask(true) != MASK_NONE)
        {
            return false;
        }
        // scrolling forward (negative clicks) flies faster
        const F32 speed = llclamp(gSavedSettings.getF32(SETTING_SPEED) * (F32)std::pow(WHEEL_STEP, (F32)-clicks),
                                  SPEED_MIN, SPEED_MAX);
        gSavedSettings.setF32(SETTING_SPEED, speed);
        sSpeedChangedInPrefs = false;   // the wheel's speed stays, whatever Preferences does after
        showSpeed(speed);
        return true;
    }

    void preferencesApplied()
    {
        sSpeedChangedInPrefs = false;
    }

    void preferencesCancelled()
    {
        if (!sSpeedChangedInPrefs)
        {
            return;
        }
        sSpeedChangedInPrefs = false;
        const F32 speed = llfinite(sSpeedBefore) ? llclamp(sSpeedBefore, SPEED_MIN, SPEED_MAX) : 1.f;
        if (speed != gSavedSettings.getF32(SETTING_SPEED))
        {
            gSavedSettings.setF32(SETTING_SPEED, speed);
        }
    }

    void cancel()
    {
        if (sHold != Hold::None || sCursorTaken)
        {
            endHold();
        }
        // the keys and the boost start from rest the next time (a key held when the flycam went off must not coast on)
        std::fill(std::begin(sSmoothed), std::end(sSmoothed), 0.f);
        sBoost = 1.f;
        sRollKey = 0.f;
        sRollSmoothed = 0.f;
        sRollBoost = 1.f;
    }

    void keepView()
    {
        // Only the flycam of the keyboard and the mouse (a 3D mouse's official flycam gives the camera back as ever), and not
        // from the mouselook (it goes back to the mouselook view).
        static LLCachedControl<bool> keep(gSavedSettings, SETTING_KEEP_VIEW, true);
        if (!sOurs || !keep || gAgentCamera.cameraMouselook() || !gAgent.getRegion())
        {
            return;
        }
        // The camera stays where the flycam left it, as the camera controls leave it (the free camera of the official
        // tools: setCameraPosAndFocusGlobal, as plus/camerashot does): until Esc or the controls bring it back.
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3d position = gAgent.getPosGlobalFromAgent(camera->getOrigin());
        const LLVector3d focus = position + LLVector3d(camera->getAtAxis()) * KEEP_FOCUS_METERS;
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(position, focus, LLUUID::null);
        gAgentCamera.stopCameraAnimation();   // setCameraPosAndFocusGlobal starts one when the focus moved
    }

    void releaseFocusForKey(KEY key, MASK mask)
    {
        // Only a key below 0x80 (the letters, the digits, the space bar and the signs; the arrow keys, Home, Enter, Esc,
        // Tab, Delete and the like are not, and stay with a slider, a list or a dialog as before) that the flycam's mode
        // binds (the user's bindings, matched as scanKey() does). Never with Ctrl: Ctrl+C / Ctrl+A belong to the control
        // (a read-only chat history copies; losing the focus would clear the edit menu's handler) and to the floaters'
        // own shortcuts; Alt+E / Alt+C look up and down. Not a text field (it types), a modal dialog (Enter / Esc close
        // it), a combo's open list or an open menu.
        if (key >= 0x80 || (mask & MASK_CONTROL) || !gAgentCamera.isUsingFlycam() || !gFocusMgr.getKeyboardFocus()
            || gFocusMgr.getKeystrokesOnly() || LLModalDialog::activeCount() > 0 || gFocusMgr.getTopCtrl() || focusInPopup()
            || (gMenuHolder && gMenuHolder->hasVisibleMenu()) || keyboardFocusIsText()
            || !gViewerInput.isKeyBound((S32)MODE_FLYCAM, key, mask))
        {
            return;
        }
        gFocusMgr.setKeyboardFocus(nullptr);
    }

    bool keyRoll(EKeystate state, F32 direction)
    {
        // alone or with Shift (faster); with Ctrl or Alt the key is the menus' or another flycam key's (Alt+E looks up).
        // Not in a flycam entered from the mouselook: the tilt would stay on the mouselook after it (camroll nudge()).
        if (state != KEYSTATE_UP && gAgentCamera.isUsingFlycam() && !gAgentCamera.cameraMouselook() && !keyboardFocusIsText()
            && !(gKeyboard && (gKeyboard->currentMask(false) & (MASK_CONTROL | MASK_ALT))))
        {
            sRollKey = direction;
        }
        return true;
    }

    void keyUnroll()
    {
        if (gAgentCamera.isUsingFlycam() && !keyboardFocusIsText())
        {
            sRollSmoothed = 0.f;
            plus::camroll::setDegrees(0.f);
        }
    }

    bool available()
    {
        return true;
    }

    void toggle()
    {
        if (!gAgentCamera.isUsingFlycam())
        {
            // as LLViewerJoystick::toggleFlycam(): turning the flycam on is being back at the keyboard
            if (gAwayTimer.getElapsedTimeF32() > LLAgent::MIN_AFK_TIME)
            {
                gAgent.clearAFK();
            }
            else
            {
                gAwayTriggerTimer.reset();
            }
        }
        sTurningOn = !gAgentCamera.isUsingFlycam();
        gAgentCamera.toggleFlycam();    // on: entered() takes sTurningOn as sOurs
        sTurningOn = false;
        LLViewerJoystick* joystick = LLViewerJoystick::getInstance();
        // a 3D mouse switched on takes the flycam from where it is, without a jump (as LLViewerJoystick::toggleFlycam())
        joystick->setNeedsReset(true);
        if (!gAgentCamera.isUsingFlycam())
        {
            // the agent's camera is made again now, not kept at the flycam's view until the avatar moves
            joystick->setCameraNeedsUpdate(true);
        }
    }

    bool isOurs()
    {
        return sOurs && gAgentCamera.isUsingFlycam();
    }

    void entered()
    {
        sOurs = sTurningOn;
        // The keys fly, as in the mouselook: a text field keeping the keyboard (the chat bar, which a click on the menu
        // leaves it with) would take W A S D, Q E C and the space as letters
        if (keyboardFocusIsText())
        {
            gFocusMgr.setKeyboardFocus(nullptr);
        }
        showLeaveButtonAndHint();
    }

    void toggleFromKey()
    {
        if (!keyboardFocusIsText())
        {
            // the menu's way (View.JoystickFlycam): with no 3D mouse toggle() above, with one the official 3D mouse flycam
            handle_toggle_flycam();
        }
    }

    void toggleFromUi()
    {
        gFocusMgr.setKeyboardFocus(nullptr);
        handle_toggle_flycam();
    }

    bool isOn()
    {
        return gAgentCamera.isUsingFlycam();
    }

    void bindKeys(const BindKey& bind)
    {
        static const struct { KEY key; const char* command; } KEYS[] =
        {
            { 'W', "flycam_move_forward" }, { 'S', "flycam_move_backward" },
            { 'A', "flycam_truck_left" },   { 'D', "flycam_truck_right" },
            { ' ', "flycam_move_up" },      { 'C', "flycam_move_down" },   // ' ': the space bar (llkeyboard.cpp)
        };
        for (const auto& entry : KEYS)
        {
            bind((S32)MODE_FLYCAM, entry.key, MASK_SHIFT, entry.command);
        }
    }
}
