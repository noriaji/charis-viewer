/**
 * @file flycammouse.h
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

#ifndef PLUS_FLYCAMMOUSE_H
#define PLUS_FLYCAMMOUSE_H

#include "llkeyboard.h"
#include "llcoord.h"

#include <functional>
#include <string>

class LLWindow;

// The official flycam already flies on the keys (W A S D, up and down, the arrow keys, Z to switch it on; Charis has
// up on the space bar instead of E, flycam-keys). This adds what a drone or a free camera of another game has, for a
// player with no game pad or 3D mouse (research/219):
//   - the mouse looks around while the right button is held (PlusFlycamMouseMode 1), or all the time (2:
//     Alt gives the cursor back); a short click with the right button is still a right click;
//   - the wheel sets how fast the camera flies (PlusFlycamSpeedMul); with Shift, Ctrl or Alt it zooms as before;
//   - Shift held boosts the speed (PlusFlycamBoost);
//   - the keys start and stop gently (PlusFlycamSmooth);
//   - Q and E tilt the camera to the left and to the right (the photo tools' tilt, plus/camroll's PlusCameraRoll, on
//     top of the flycam; PlusFlycamRollSpeed degrees a second, 3 times with Shift), Home levels it.
// Nothing runs while the flycam is off.
// Ways in and out (flycam-entry): Alt+Shift+F in every mode (the menu item Charis > Flycam (Keyboard and Mouse) and
// the key bindings' toggle_flycam), Z in the flycam, and the "Leave the flycam" button at the bottom while it is on.
// In the photo screen, whose menu bar and leave button are hidden: its flycam icon over the world and the Camera tab's
// check box (detailed view), the Camera spot page's button (simple view) (toggleFromUi()).
namespace plus::flycammouse
{
    // What the flycam takes this frame, besides the keys.
    struct Frame
    {
        F32 move_scale = 1.f;   // on the move speed: the wheel's factor times the Shift boost
        F32 look_yaw   = 0.f;   // radians, in LLFlycam's signs: positive to the left
        F32 look_pitch = 0.f;   // radians, in LLFlycam's signs: positive downwards
    };

    // Once per frame while the flycam is on (LLAgentCamera::updateFlycam()), before the keys are added to the
    // other inputs. 'keys' are the keyboard's channels (LLGameControl::FlycamChannel), softened in place.
    Frame update(F32 delta_time, F32* keys, S32 count);

    // The right button, in LLViewerWindow::handleAnyMouseClick(). The press is offered after the UI has
    // declined it (a click on the world); the release is offered to everything, before anyone else, and is
    // ours whenever the press was. Both return true when the click was taken.
    bool pressRight(LLWindow* window, LLCoordGL pos, MASK mask, S32 x, S32 y);
    bool releaseRight(LLWindow* window, LLCoordGL pos, MASK mask);

    // The wheel over the world (LLViewerWindow::handleScrollWheel()); true when it set the speed.
    bool onWheel(S32 clicks);

    // The app lost the focus, or the flycam went off: let the cursor go and forget the hold.
    void cancel();

    // Called as the flycam goes off (LLAgentCamera::toggleFlycam()): the camera stays where the flycam left it
    // (PlusFlycamKeepView), as the camera controls leave it, instead of going back behind the avatar.
    void keepView();

    // The key bindings' flycam_roll_left (direction -1, Q) and flycam_roll_right (1, E), in llviewerinput.cpp: while
    // held, update() tilts the camera (plus::camroll::setDegrees(), the photo tools' tilt; - is to the left, as their
    // slider). Without "Allow Roll" (the game pads' setting, which the official roll channel needs). Always true.
    bool keyRoll(EKeystate state, F32 direction);

    // The key bindings' flycam_unroll (Home), besides the official unroll of the flycam's own roll: the tilt is level.
    void keyUnroll();

    // LLViewerWindow::handleKey(), before the UI sees the key: in the flycam, a key below 0x80 that the flycam's mode binds
    // (not with Ctrl) takes the keyboard from a control that is not a text field (a button or check box of a floater
    // clicked last would take the space bar as a press, and keep every key from the world). The arrow keys, Enter, Esc,
    // Tab, Delete and the others stay with it; a text field, a modal dialog, a combo's open list and an open menu keep it.
    void releaseFocusForKey(KEY key, MASK mask);

    // The flycam on the keys and the mouse needs no device and no setting switched on (the official keys and
    // this mouse look): the menu item can always be chosen, also with no 3D mouse (View.EnableJoystickFlycam).
    bool available();

    // On or off with no 3D mouse (LLViewerJoystick::toggleFlycam() with its settings off or no 3D mouse found: the
    // menu, the keys and the leave button all come through it). It does not write JoystickEnabled or
    // JoystickFlycamEnabled. Clears the away state as the official one does; going off, the camera is the agent's
    // camera again at once.
    void toggle();

    // The flycam that is on was turned on by toggle(): the keyboard and the mouse fly it, so switching the 3D mouse
    // off (LLViewerJoystick::updateEnabled(), the 3D mouse settings) leaves it on.
    bool isOurs();

    // The toggle_flycam key: as the menu (handle_toggle_flycam()), but not while a text field has the keyboard (a key
    // with Alt or Ctrl reaches the key bindings also then).
    void toggleFromKey();

    // A button or check box of a panel (the photo screen's, which hides the menu bar and the leave button): as the
    // menu (handle_toggle_flycam()), and the control lets the keyboard go so that the keys fly at once.
    void toggleFromUi();

    // The flycam is on (from any way in, a 3D mouse's too): for a control that shows it.
    bool isOn();

    // The keys that leave the flycam as the key settings have them ("Z | Alt+Shift+F"; the menu item's Alt+Shift+F
    // when none is bound), and leaveKey() the one of them to name in a short line: the first with Alt or Ctrl (it works
    // also while a text field has the keyboard), else the first. switchKey(): that one of the keys that turn the
    // flycam on (the third person mode's). Not for every frame: they read the key bindings (flycamexit.cpp).
    std::string leaveKeys();
    std::string leaveKey();
    std::string switchKey();

    // The flycam went on (LLAgentCamera::toggleFlycam(), from any way in): the keyboard is taken back from a text
    // field so the keys fly, the "Leave the flycam" button shows at the bottom (flycamexit.cpp) and, once a
    // session until the user says no more, a short how-to.
    void entered();

    // flycamexit.cpp, from entered(): the button (made the first time) and the how-to.
    void showLeaveButtonAndHint();

    // The <plus> hooks of LLFloaterPreference::apply() (OK) and cancel() (Cancel, the close box; OK comes there
    // too, after apply): the flying speed's slider is not bound by control_name (the wheel's speed is not put
    // back), so Cancel puts back a speed that the slider or Restore Defaults changed (research/275 L071).
    void preferencesApplied();
    void preferencesCancelled();

    // Shift + W A S D, the space and C, so a key keeps flying while Shift boosts it, in the flycam's mode, on top of the user's
    // own key bindings (the same way as plus::flycamsens::bindKeys()).
    using BindKey = std::function<bool(S32 mode, KEY key, MASK mask, const char* command)>;
    void bindKeys(const BindKey& bind);
}

#endif // PLUS_FLYCAMMOUSE_H
