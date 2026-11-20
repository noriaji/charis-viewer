/**
 * @file flycamsens.cpp
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

#include "llviewerprecompiledheaders.h"

#include "flycamsens.h"

#include "plus/plus.h"

#include "plus/flycammouse/flycammouse.h"

#include "llviewerinput.h"
#include "llviewerjoystick.h"

namespace plus::flycamsens
{
    namespace
    {
        constexpr S32 DIGITS = 9;

        // Only while the flycam is on (false lets the key through to whatever it did before). As the photo
        // tools' keys: the digits of a text control are its own, and the numeric keypad reaches the world
        // past it; Shift, Ctrl and Alt are other commands'.
        bool keyIsOurs()
        {
            if (!LLViewerJoystick::getInstance()->getOverrideCamera() || plus::textHasKeyboard())
            {
                return false;
            }
            return gKeyboard && gKeyboard->currentMask(false) == MASK_NONE;
        }
    }

    bool keySens(EKeystate state, S32 digit)
    {
        if (digit < 1 || digit > DIGITS || !keyIsOurs())
        {
            return false;
        }
        if (state == KEYSTATE_DOWN)
        {
            plus::flycammouse::setSpeedLevel(digit);
        }
        return true;
    }

    bool keyLevel(EKeystate state)
    {
        if (!keyIsOurs())
        {
            return false;
        }
        if (state == KEYSTATE_DOWN) // held, the key does not start the turn again
        {
            plus::flycammouse::levelNow();
        }
        return true;
    }

    bool isOwnCommand(const std::string& command)
    {
        return command.compare(0, 12, "flycam_sens_") == 0 || command == "flycam_level";
    }

    void bindKeys(const BindKey& bind)
    {
        static const char* const COMMANDS[] =
        {
            "flycam_level", "flycam_sens_1", "flycam_sens_2", "flycam_sens_3", "flycam_sens_4",
            "flycam_sens_5", "flycam_sens_6", "flycam_sens_7", "flycam_sens_8", "flycam_sens_9",
        };
        // While the flycam is on, the keys are read in its own mode (LLViewerInput::getMode(), since the
        // official 26.5 game control of 2026-10-22); the other modes never see the flycam on
        for (S32 mode : { (S32)MODE_FLYCAM })
        {
            for (S32 digit = 0; digit <= DIGITS; ++digit)
            {
                bind(mode, (KEY)('0' + digit), MASK_NONE, COMMANDS[digit]);
            }
        }
    }
}
