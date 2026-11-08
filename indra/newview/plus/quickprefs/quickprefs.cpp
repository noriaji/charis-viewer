/**
 * @file quickprefs.cpp
 * @brief Quick Preferences (ported from Firestorm, plus/quickprefs):
 *        registration with the plus foundation.
 *
 * Where Firestorm registers the floater in llviewerfloaterreg.cpp and adds its
 * toolbar command to commands.xml, this feature registers itself; the window is
 * opened from the Plus menu (menu_plus.xml, "QuickPreferences") or from the
 * toolbar button "Quick prefs" (commands_plus.xml).
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

#include "quickprefsfloater.h"

#include "plus.h"

#include "llfloaterreg.h"

namespace
{
    void init()
    {
        // Firestorm: llviewerfloaterreg.cpp. Settings: app_settings/settings_plus.xml (PlusQuickPrefs*);
        // the list's standard file: app_settings/plus/quickprefs/quick_preferences.xml.
        LLFloaterReg::add(plus::quickprefs::QuickPrefsFloater::NAME, "floater_plus_quickprefs.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<plus::quickprefs::QuickPrefsFloater>);

        // The toolbar button: app_settings/commands_plus.xml ("plus_quickprefs"; Firestorm: commands.xml),
        // its texts skins/default/xui/<lang>/strings_plus.xml. The icon is the Charis mark as a white
        // silhouette (tools/make_quickprefs_icon.py in the sl-viewer repository).
        plus::declareUIImage("Plus_Quickprefs_Icon", "plus/quickprefs_18.png");
    }

    plus::FeatureRegistration sRegistration({ "quickprefs", &init, {} });
}
