/**
 * @file poser.cpp
 * @brief Registers the Poser (ported from Firestorm) with the plus foundation.
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

#include "plus.h"

#include "fsfloaterposer.h"
#include "llfloaterreg.h"

namespace
{
    void init()
    {
        // Firestorm: llviewerfloaterreg.cpp
        LLFloaterReg::add("fs_poser", "floater_fs_poser.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterPoser>);

        // Firestorm: textures.xml. Images the official textures.xml lacks;
        // the same names as in Firestorm so its XUI works unchanged.
        plus::declareUIImage("Plus_Poser_Visual_On",        "plus/visual_pose_enabled.png");
        plus::declareUIImage("Plus_Icon_Undock_Foreground", "plus/Icon_Undock_Foreground.png");
        plus::declareUIImage("Plus_Edit_Flip_X",            "plus/Edit_Flip_X.png");
        plus::declareUIImage("Plus_Script_Undo",            "plus/script_undo.png");
        plus::declareUIImage("Plus_Script_Redo",            "plus/script_redo.png");
        plus::declareUIImage("Plus_Script_Running",         "plus/green_square.png");
        plus::declareUIImage("Plus_Script_NotRunning",      "plus/yellow_square.png");
        plus::declareUIImage("Plus_Script_Error",           "plus/red_square.png");

        // Settings: app_settings/settings_plus.xml (FSPoser*, FSManip*).
        // Menu item: skins/default/xui/<lang>/menu_plus.xml ("Poser").
    }

    plus::FeatureRegistration sRegistration({ "poser", &init, {} });
}
