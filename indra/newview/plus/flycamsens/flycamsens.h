/**
 * @file flycamsens.h
 * @brief Sensitivity of the 3D mouse (flycam) on the number keys.
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

// While the flycam (the 3D mouse's camera) is on, the number keys 1-9 set how fast it moves and turns,
// and 0 puts it back to the normal speed. The digit is a step of PlusFlycamSensSteps (the multipliers,
// slowest first). Nothing is drawn: the camera's own speed shows it. With the flycam off the keys are
// not taken, so a digit does what it did before. A control that takes text keeps its digits.
// moveFlycam() (llviewerjoystick.cpp) scales its six move / turn axes by multiplier(); the zoom axis is
// left alone, and the user's own FlycamAxisScale* stay as they are under it.
namespace plus::flycamsens
{
    // The keyboard actions "flycam_sens_1" ... "flycam_sens_9" and "flycam_sens_reset".
    bool keySens(EKeystate state, S32 digit);

    // The factor on the flycam's move / turn axes (PlusFlycamSens, kept between sessions).
    F32 multiplier();

    // The keys 1-9 and 0 in every mode a flycam can be used in, on top of the user's own key bindings.
    // A digit the user's file gives to another command keeps it: bind puts keySens in front of that
    // command, which gets the key whenever keySens returns false (llviewerinput.cpp, loadBindingsXML).
    using BindKey = std::function<bool(S32 mode, KEY key, MASK mask, const char* command)>;
    void bindKeys(const BindKey& bind);
}

#endif // PLUS_FLYCAMSENS_H
