/**
 * @file version.cpp
 * @brief Charis Viewer's own version number (see version.h).
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

#include "version.h"

#include "lltrans.h"
#include "llversioninfo.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "stringize.h"

namespace plus::version
{
    static_assert(MAJOR >= 0 && MINOR >= 0 && PATCH >= 0 && STAGE_NUMBER >= 0);

    std::string shortVersion()
    {
        return stringize(MAJOR, '.', MINOR, '.', PATCH);
    }

    std::string stage()
    {
        if (STAGE[0] == '\0')
        {
            return std::string();
        }
        if (STAGE_NUMBER > 0)
        {
            return stringize(STAGE, ' ', STAGE_NUMBER);
        }
        return std::string(STAGE);
    }

    std::string shortTitle(const std::string& name)
    {
        std::string text = name + " " + shortVersion();
        const std::string stage_text = stage();
        if (!stage_text.empty())
        {
            text += " " + stage_text;
        }
        text += stringize(" (rev ", LLVersionInfo::instance().getBuild(), ")");
        return text;
    }

    std::string displayVersion(const std::string& name)
    {
        return shortTitle(name);
    }

    std::string displayVersion()
    {
        return displayVersion(PRODUCT_NAME);
    }

    // ---- The main window's title ------------------------------------------

    namespace
    {
        std::string sTitlePlace;
        std::string sTitleStatus;
        std::string sTitleShown;    // what was last given to the window

        void applyTitle()
        {
            if (!gViewerWindow || !gViewerWindow->getWindow())
            {
                return;
            }
            std::string title = shortTitle(LLTrans::getString("APP_NAME"));
            if (!sTitlePlace.empty())
            {
                title += " - " + sTitlePlace;
            }
            if (!sTitleStatus.empty())
            {
                title += " - " + sTitleStatus;
            }
            title = utf8str_truncate(title, 255);   // as the start's title (llappviewer.cpp), not mid-character
            if (title != sTitleShown)
            {
                sTitleShown = title;
                gViewerWindow->getWindow()->setTitle(title);
            }
        }
    }

    void setTitlePlace(const std::string& place)
    {
        sTitlePlace = place;
        applyTitle();
    }

    void setTitleStatus(const std::string& status)
    {
        sTitleStatus = status;
        applyTitle();
    }
}
