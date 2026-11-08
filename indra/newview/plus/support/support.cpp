/**
 * @file support.cpp
 * @brief Charis's own place for bug reports and release notes.
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

#include "support.h"
#include "plus.h"

#include "llstring.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llweb.h"

namespace
{
    constexpr char SUPPORT_URL[] = "PlusSupportURL";
    constexpr char RELEASE_NOTES_URL[] = "PlusReleaseNotesURL";

    // The setting's value without surrounding spaces; "" when it is missing.
    std::string urlSetting(const char* name)
    {
        LLControlVariable* control = gSavedSettings.getControl(name);
        if (!control)
        {
            return std::string();
        }
        std::string url = control->getValue().asString();
        LLStringUtil::trim(url);
        return url;
    }

    void init()
    {
        // The "Report Bug" items of the Help menus (menu_viewer.xml, menu_login.xml).
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Support.HasReportURL",
            [](LLUICtrl*, const LLSD&) { return !plus::support::supportURL().empty(); });
    }

    plus::FeatureRegistration sRegistration({ "support", &init, {} });
}

namespace plus::support
{
    std::string supportURL()
    {
        return urlSetting(SUPPORT_URL);
    }

    void reportBug()
    {
        const std::string url = supportURL();
        if (url.empty())
        {
            LL_INFOS("Plus") << "Report Bug: no place for reports (" << SUPPORT_URL << " is empty)" << LL_ENDL;
            return;
        }
        LLWeb::loadURLExternal(url);
    }

    std::string releaseNotesURL()
    {
        return urlSetting(RELEASE_NOTES_URL);
    }

    std::string releaseNotesLine(const std::string& url, const std::string& label)
    {
        if (url.empty())
        {
            return std::string();
        }
        // The text widget makes "[url label]" a link (as AboutHeader's old
        // [[VIEWER_RELEASE_NOTES_URL] [ReleaseNotes]] did).
        return "\n[" + LLWeb::escapeURL(url) + " " + label + "]";
    }
}
