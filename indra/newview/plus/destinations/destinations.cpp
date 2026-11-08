/**
 * @file destinations.cpp
 * @brief Reload for the Destinations floater (plus/destinations)
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

// Why: the Destinations floater is single instance and stays alive while closed, so a guide
// page that failed once (a 502 from the server) is still on screen when the floater is opened
// again, and the official floater has no way to load it again.
//
// How the error is seen: the CEF plugin never sends "navigate_error_page" (that came from the
// old WebKit plugin), so MEDIA_EVENT_NAVIGATE_ERROR_PAGE and the GenericErrorPageURL never come.
// It sends the main frame's HTTP status with "navigate_complete" instead; the plugin keeps it
// (LLPluginClassMedia::getNavigateResultCode(), -1 until the first load completes).
//
// How the cache is skipped: LLMediaCtrl::clearCache() sends "clear_cache", which the CEF plugin
// does not handle. "browse_reload" does: it reloads with ignore_cache. The button reloads the page
// on screen that way (the guide, or a page the guide led to); only without a plugin or a page is
// the guide URL opened with navigateTo(). The reload on open goes back to the guide URL: with
// browse_reload(true) when the guide URL is the page on screen, else with navigateTo().
// navigateTo's clean_browser is not used: it would start a plugin with web security off.

#include "llviewerprecompiledheaders.h"

#include "plus/destinations/destinations.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "llmediactrl.h"
#include "llpluginclassmedia.h"
#include "llviewercontrol.h"

namespace
{
    const char* const BROWSER_NAME = "destination_guide_contents";
    const char* const BUTTON_NAME = "plus_reload_btn";

    // The result code is not reset when a load begins, so while a reload on open is still
    // loading the old failure is still read: opening the floater again within this time does
    // not start another reload. The floater is single instance, so one time is kept.
    constexpr F64 AUTO_RELOAD_HOLDOFF_SECONDS = 5.0;
    F64 sLastAutoReload = -AUTO_RELOAD_HOLDOFF_SECONDS;

    LLMediaCtrl* guideBrowser(LLFloater* floater)
    {
        return floater ? floater->findChild<LLMediaCtrl>(BROWSER_NAME) : nullptr;
    }

    // The guide URL again: reloaded without the cache when it is the page on screen, else opened.
    void openGuide(LLMediaCtrl* browser)
    {
        const std::string url = gSavedSettings.getString("DestinationGuideURL");
        if (!browser || url.empty())
        {
            return;
        }
        LLPluginClassMedia* plugin = browser->getMediaPlugin();
        if (plugin && plugin->getLocation() == url)
        {
            plugin->browse_reload(true); // ignore the cache
            return;
        }
        browser->navigateTo(url, HTTP_CONTENT_TEXT_HTML);
    }

    // The button: the page on screen again, without the cache.
    void reloadPage(LLMediaCtrl* browser)
    {
        LLPluginClassMedia* plugin = browser ? browser->getMediaPlugin() : nullptr;
        if (plugin && !plugin->getLocation().empty())
        {
            plugin->browse_reload(true); // ignore the cache
            return;
        }
        openGuide(browser);
    }

    // The last completed load of the main frame ended in an HTTP error. 0 is a load that never
    // got a response (no connection, DNS); -1 is "not completed yet" and is not an error.
    bool lastLoadFailed(LLMediaCtrl* browser)
    {
        LLPluginClassMedia* plugin = browser ? browser->getMediaPlugin() : nullptr;
        if (!plugin)
        {
            return false; // no plugin: it loads the page itself when it starts again
        }
        const S32 code = plugin->getNavigateResultCode();
        return code == 0 || code >= 400;
    }
}

namespace plus
{
namespace destinations
{
    void setupReloadButton(LLFloater* floater)
    {
        LLButton* button = floater ? floater->findChild<LLButton>(BUTTON_NAME) : nullptr;
        if (!button)
        {
            return;
        }
        // The button is the floater's child, so the floater is alive whenever this runs.
        button->setCommitCallback([floater](LLUICtrl*, const LLSD&) { reloadPage(guideBrowser(floater)); });
    }

    void reloadIfLastLoadFailed(LLFloater* floater)
    {
        LLMediaCtrl* browser = guideBrowser(floater);
        if (!lastLoadFailed(browser))
        {
            return;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - sLastAutoReload < AUTO_RELOAD_HOLDOFF_SECONDS)
        {
            return; // the last reload on open may still be loading
        }
        sLastAutoReload = now;
        LL_INFOS("Destinations") << "Last load of the guide failed (HTTP "
                                 << browser->getMediaPlugin()->getNavigateResultCode() << "), reloading" << LL_ENDL;
        openGuide(browser);
    }
}
}
