/**
 * @file release.cpp
 * @brief What the downloadable build does at startup that a developer build
 *        does not (release.h).
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

#include "release.h"

#include "llviewercontrol.h"

void plus::release::clearDeveloperQAModeOnce()
{
#ifdef LL_RELEASE_FOR_DOWNLOAD
    // Declared in settings_plus.xml, but that file is read right after this
    // (plus::declareSettings()). Here the control exists only when the user's
    // settings.xml holds it; plus::loadSettings() adopts the one declared below.
    constexpr char CLEARED[] = "PlusReleaseQAModeCleared";

    LLControlVariable* cleared = gSavedSettings.getControl(CLEARED);
    if (cleared && cleared->getValue().asBoolean())
    {
        return;
    }

    LLControlVariable* qa_mode = gSavedSettings.getControl("QAMode");
    if (qa_mode && qa_mode->getValue().asBoolean())
    {
        // Back to the default (off); being the default, it is no longer written to settings.xml.
        qa_mode->resetToDefault(true);
        LL_INFOS("Plus") << "QAMode was saved on (by a developer build, or in copied settings): "
                         << "turned off once for the downloadable build; Ctrl+Alt+Q turns it on again" << LL_ENDL;
    }

    if (!cleared)
    {
        cleared = gSavedSettings.declareBOOL(CLEARED, false,
            "The downloadable build has turned a saved QAMode off once (plus/release)",
            LLControlVariable::PERSIST_NONDFT);
    }
    if (cleared)
    {
        cleared->setValue(true);
    }
#endif
}
