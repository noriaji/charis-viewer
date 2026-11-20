/**
 * @file flycamsens.h
 * @brief The flycam's number keys: 1-9 its sensitivity, 0 levels it.
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

#ifndef PLUS_FLYCAMSENS_H
#define PLUS_FLYCAMSENS_H

#include "llkeyboard.h"

#include <functional>
#include <string>

// Number keys as quiet shortcuts of the flycam's speed and level (kept; not named in the flycam's how-to): while the
// flycam is on, 1-9 set the speed to that level of PlusFlycamSpeedLevels and 0 turns the camera level. The speed is
// the one setting the mouse wheel steps and a 3D mouse and the keyboard flycam fly by, and the wheel's press is the 0
// key (plus/flycammouse: onWheel(), onMiddleClick(), setSpeedLevel(), levelNow()); these keys only call those. With
// the flycam off the keys are not taken, so a digit does what it did before. A control that takes text keeps its digits.
namespace plus::flycamsens
{
    // The keyboard actions "flycam_sens_1" ... "flycam_sens_9" (digit 1-9; another digit is not taken): the speed
    // becomes that level's factor.
    bool keySens(EKeystate state, S32 digit);

    // The keyboard action "flycam_level" (the 0 key): the flycam turns level in a quarter of a second, the
    // direction it looks in kept (LLAgentCamera::levelFlycam()); its own roll and the tilt on top (Q / E, the
    // photo tools') both go. The flycam's input waits for the turn (LLFlycam::integrate(), as for the official
    // unroll); the key again starts the turn again from where it is. Taken while the flycam is on.
    bool keyLevel(EKeystate state);

    // One of the commands above (bindKeys()): a key the user's key bindings give to one of them stays theirs.
    bool isOwnCommand(const std::string& command);

    // The keys 1-9 and 0 in every mode a flycam can be used in, on top of the user's own key bindings.
    // A digit the user's file gives to another command keeps it: bind puts keySens (keyLevel for 0) in front
    // of that command, which gets the key whenever ours returns false (llviewerinput.cpp, loadBindingsXML);
    // given to one of ours (isOwnCommand()), the key is left to the file's choice.
    using BindKey = std::function<bool(S32 mode, KEY key, MASK mask, const char* command)>;
    void bindKeys(const BindKey& bind);
}

#endif // PLUS_FLYCAMSENS_H
