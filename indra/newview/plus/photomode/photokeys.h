/**
 * @file photokeys.h
 * @brief Photo tools: keys for the camera's tilt (plus/camroll) and the composition guide.
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

#ifndef PLUS_PHOTOMODE_PHOTOKEYS_H
#define PLUS_PHOTOMODE_PHOTOKEYS_H

#include "llkeyboard.h" // EKeystate

// The keyboard actions of the photo tools. llviewerinput.cpp registers them by these names (a hook,
// like the official actions); app_settings/key_bindings.xml binds them by default and the key
// settings (Preferences > Move & View > Controls, control_table_contents_camera.xml) list them, so
// the user can change them like any other key.
//
//   roll_left / roll_right   tilt the camera while the key is held (Shift: faster); the names are
//                            Firestorm's, which binds them to R and T as well
//   roll_reset               level the camera
//   photo_guide_toggle       show or hide the composition guide
//   photo_guide_next         the next kind of guide (none, thirds, golden ratio, ... horizon, none ...)
//   photo_screen_toggle      go into the photo screen (plus/photoscreen) or out of it (F12; research/221). A global
//                            key: it acts also while a text field or another control has the keyboard focus
//
// They act as WASD does: not while a control that takes text (the chat, a text field) has the keyboard
// focus, but right after a button or slider of the photo tools was used. The official handling keeps
// the letter keys in a text control; these check that again for the numeric keypad, which goes past it.
// Only the key alone or with Shift acts: Ctrl and Alt are the menus'.
namespace plus::photomode
{
    bool keyRollLeft(EKeystate state);
    bool keyRollRight(EKeystate state);
    bool keyRollReset(EKeystate state);
    bool keyGuideToggle(EKeystate state);
    bool keyGuideNext(EKeystate state);
    bool keyPhotoScreenToggle(EKeystate state);

    // Once per command (PlusPhotoKeysCommands lists the ones done): a key_bindings.xml the user saved
    // before a command existed gets its default keys from app_settings/key_bindings.xml, in every mode,
    // unless the user already bound the command there or the key is taken in that mode. A command
    // already done is never added again, so a key the user took away stays away. The new file is written
    // beside the old one and read back before it replaces it; the old one is kept as
    // key_bindings_before_photo_keys.xml. A command is marked done only when that worked (or nothing was
    // needed). From the feature's onUIReady.
    void addDefaultKeys();
}

#endif // PLUS_PHOTOMODE_PHOTOKEYS_H
