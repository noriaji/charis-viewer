/**
 * @file windowmode.cpp
 * @brief Borderless fullscreen, switched while the viewer runs: PlusBorderless (Charis menu) takes the
 *        frame off the window and sets it over its monitor; off puts it back where it was. The old
 *        exclusive "FullScreen" (a change of the display mode, set at the start only) is not used any more:
 *        a session that has it set starts borderless instead.
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
#include "plus/windowmode/windowmode.h"

#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace plus::windowmode
{
    namespace
    {
        constexpr char SETTING_BORDERLESS[] = "PlusBorderless";

        void apply(bool borderless)
        {
            if (gViewerWindow && gViewerWindow->getWindow())
            {
                gViewerWindow->getWindow()->setBorderless(borderless);
            }
        }

        void init()
        {
            // Setting: app_settings/settings_plus.xml (PlusBorderless). Check box: panel_preferences_charis.xml
            // ("c_borderless", the control of the setting).
            if (LLControlVariable* control = gSavedSettings.getControl(SETTING_BORDERLESS))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                                              { apply(value.asBoolean()); });
            }
        }

        void onUIReady()
        {
            // The window is made from the saved position and size first (and maximized, if it was);
            // the placement kept here is that one.
            if (gSavedSettings.getBOOL(SETTING_BORDERLESS))
            {
                apply(true);
            }
        }

        plus::FeatureRegistration sRegistration({ "windowmode", &init, &onUIReady });

        // The old exclusive FullScreen start (llappviewerwin32.cpp, the Windows 11 24H2 workaround) saved
        // the screen resolution as WindowWidth/Height: a framed window that size would go past the screen
        // edges once borderless is turned off. Such a size becomes 80% of the work area, centred.
        void resetScreenSizedWindow()
        {
#if LL_WINDOWS
            DEVMODE dev_mode = {};
            dev_mode.dmSize = sizeof(DEVMODE);
            RECT work = {};
            if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dev_mode) ||
                !SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
            {
                return;
            }
            if (gSavedSettings.getU32("WindowWidth") < dev_mode.dmPelsWidth ||
                gSavedSettings.getU32("WindowHeight") < dev_mode.dmPelsHeight)
            {
                return;
            }
            const S32 work_width = work.right - work.left;
            const S32 work_height = work.bottom - work.top;
            const S32 width = work_width * 4 / 5;
            const S32 height = work_height * 4 / 5;
            gSavedSettings.setU32("WindowWidth", (U32)width);
            gSavedSettings.setU32("WindowHeight", (U32)height);
            gSavedSettings.setS32("WindowX", work.left + (work_width - width) / 2);
            gSavedSettings.setS32("WindowY", work.top + (work_height - height) / 2);
            LL_INFOS("Window") << "Screen-sized window from the old FullScreen start set to " << width << "x"
                               << height << LL_ENDL;
#endif
        }
    }

    void migrateExclusiveFullscreen()
    {
        LLControlVariable* fullscreen = gSavedSettings.getControl("FullScreen");
        if (!fullscreen || !fullscreen->getValue().asBoolean())
        {
            return;
        }
        // getSaveValue() is the settings.xml value; "--set FullScreen 1" (and the other command-line
        // options) sets an unsaved value over it (LLControlVariable::setValue(value, false)).
        if (fullscreen->getSaveValue().asBoolean())
        {
            // Saved: move it over for good.
            gSavedSettings.setBOOL("FullScreen", false);
            gSavedSettings.setBOOL(SETTING_BORDERLESS, true);
            resetScreenSizedWindow();
        }
        else
        {
            // This start only: borderless for this session, nothing saved.
            fullscreen->setValue(false, false);
            if (LLControlVariable* borderless = gSavedSettings.getControl(SETTING_BORDERLESS))
            {
                borderless->setValue(true, false);
            }
        }
    }

    bool isBorderless()
    {
        return gViewerWindow && gViewerWindow->getWindow() && gViewerWindow->getWindow()->getBorderless();
    }
}
