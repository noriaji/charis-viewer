/**
 * @file flycamsens.cpp
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

#include "llviewerprecompiledheaders.h"

#include "flycamsens.h"

#include "plus/plus.h"

#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewerjoystick.h"

#include <sstream>
#include <vector>

namespace plus::flycamsens
{
    namespace
    {
        constexpr char SETTING_SENS[]  = "PlusFlycamSens";
        constexpr char SETTING_STEPS[] = "PlusFlycamSensSteps";
        constexpr S32  DIGITS          = 9;
        constexpr F32  SENS_MIN        = 0.01f;
        constexpr F32  SENS_MAX        = 100.f;

        // The multipliers of the digits; a missing or bad entry falls back to the default list's, so a
        // hand-edited setting can not leave a digit without a speed.
        std::vector<F32> steps()
        {
            static const F32 DEFAULTS[DIGITS] = { 0.1f, 0.18f, 0.3f, 0.5f, 1.f, 1.6f, 2.5f, 4.f, 6.f };
            std::vector<F32> list(DEFAULTS, DEFAULTS + DIGITS);
            std::istringstream words(gSavedSettings.getString(SETTING_STEPS));
            for (S32 i = 0; i < DIGITS; ++i)
            {
                F32 value = 0.f;
                if (!(words >> value))
                {
                    break;
                }
                if (value >= SENS_MIN && value <= SENS_MAX)
                {
                    list[i] = value;
                }
            }
            return list;
        }

        // As the photo tools' keys: the digits of a text control are its own, and the numeric keypad
        // reaches the world past it; Shift, Ctrl and Alt are other commands'.
        bool keyIsOurs()
        {
            if (plus::textHasKeyboard())
            {
                return false;
            }
            return gKeyboard && gKeyboard->currentMask(false) == MASK_NONE;
        }
    }

    F32 multiplier()
    {
        static LLCachedControl<F32> sens(gSavedSettings, SETTING_SENS, 1.f);
        return llfinite((F32)sens) ? llclamp((F32)sens, SENS_MIN, SENS_MAX) : 1.f;   // NaN would pass llclamp()
    }

    bool keySens(EKeystate state, S32 digit)
    {
        // false lets the key through to whatever it did before
        if (!LLViewerJoystick::getInstance()->getOverrideCamera() || !keyIsOurs())
        {
            return false;
        }
        if (state == KEYSTATE_DOWN)
        {
            const std::vector<F32> list = steps();
            gSavedSettings.setF32(SETTING_SENS, digit >= 1 && digit <= DIGITS ? list[digit - 1] : 1.f);
        }
        return true;
    }

    void bindKeys(const BindKey& bind)
    {
        static const char* const COMMANDS[] =
        {
            "flycam_sens_reset", "flycam_sens_1", "flycam_sens_2", "flycam_sens_3", "flycam_sens_4",
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
