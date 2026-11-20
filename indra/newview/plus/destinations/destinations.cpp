/**
 * @file destinations.cpp
 * @brief Reload and page language for the Destinations floater (plus/destinations)
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
//
// Why the language flips to English: the guide page is English HTML that Weglot (a script in the
// page) translates in the browser. Weglot picks the language from the browser language only the
// first time; afterwards it follows its own choice kept in the page's localStorage ("wglang") and
// a cookie ("WG_CHOOSE_ORIGINAL"), so a reload can come back in English. localStorage cannot be
// written before the page has loaded, so once it has loaded the UI language is asked of Weglot
// with its own API (switchTo), which does not reload the page and is the surer way.
//
// When: the loads that follow the floater being opened, the Reload button, the first load of the
// session or a change of the UI language, for a few seconds ("settle window", see
// PageLanguageObserver): the page often loads two or three times in a row then, and the last one
// can come back in English. A link the user follows later is left alone, so a language they pick
// on the page itself is kept. PlusDestinationsPageLang off: nothing is done (the official behaviour).

#include "llviewerprecompiledheaders.h"

#include "plus/destinations/destinations.h"

#include "llbutton.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "llmediactrl.h"
#include "llpluginclassmedia.h"
#include "llui.h"
#include "llviewermediaobserver.h"
#include "llviewermedia.h"
#include <algorithm>
#include <memory>
#include "llviewercontrol.h"

namespace
{
    const char* const BROWSER_NAME = "destination_guide_contents";
    const char* const BUTTON_NAME = "plus_reload_btn";

    // Weglot's language codes on the Second Life site differ from the viewer's in one case (Brazilian
    // Portuguese); a language the site does not have is left alone by the script below. Only letters
    // and "-" get into the script.
    std::string pageLanguageFor(const std::string& ui_language)
    {
        for (char c : ui_language)
        {
            if (!((c >= 'a' && c <= 'z') || c == '-'))
            {
                return std::string();
            }
        }
        return ui_language == "pt" ? std::string("br") : ui_language;
    }

    // The page itself checks its host (the plugin runs the script in the main frame it has at that
    // moment, which may be another site if the page moved on). Weglot may be added after the load
    // event and may take long to start: the script looks for it every 500 ms for 5 seconds, and once
    // found it also waits for its "initialized" event (registered once). Switching twice is harmless
    // (the language is compared first). The whole step is inside try, also when run by the timer.
    const char* const SWITCH_SCRIPT_HEAD =
        "(function(t){var h=location.hostname;if(h!=='secondlife.com'&&h!=='go.secondlife.com')return;"
        "var n=0,hooked=false;var f=function(){try{var w=window.Weglot;"
        "if(!w||!w.initialized){"
        "if(w&&!hooked&&w.on){hooked=true;w.on('initialized',f);}"
        "if(++n<=10)setTimeout(f,500);return;}"
        "if(w.getCurrentLang()!==t&&((w.options&&t===w.options.language_from)||w.getAvailableLanguages().indexOf(t)>=0))"
        "w.switchTo(t);}catch(e){}};f();})('";

    class PageLanguageObserver final : public LLViewerMediaObserver
    {
    public:
        // Whichever of the browser and this object goes first removes the link (the base class
        // destructor and the emitter's destructor both do), so nothing is kept here.
        explicit PageLanguageObserver(LLMediaCtrl* browser)
        {
            browser->addObserver(this);
        }

        // A page that is still settling (the floater was opened, the page redirects, Weglot puts back
        // its own choice) loads two or three times in a row. A "wait" says that a load is due; the
        // first good load after it opens a "window" in which every good load is asked for the UI
        // language (the script is sent). A link the user follows after the window is left alone.
        // States (times are on the frame timer; only "now < t" is compared, no subtraction, so a large
        // jump of the clock can only close a wait or a window, never open one; 0 is in the past):
        //   mWaitUntil    waiting for the first good load; mWindowUntil / mWindowStart / mSent  the window.
        //   mAppliedLanguage   the language last asked for, or passed over while the setting was off.
        // Transitions:
        //   open the floater, Reload, reload of a failed guide (arm)
        //       setting on : mWaitUntil = now + WAIT (30 s); an open window is closed (mWindowUntil = 0)
        //       setting off: nothing
        //   good load, setting off                -> mWaitUntil = mWindowUntil = 0; mAppliedLanguage = lang
        //   good load, UI language differs        -> mAppliedLanguage = lang; mWaitUntil = now + WAIT
        //                                            (this load is the first one)
        //   good load, window open (now < mWindowUntil)
        //       mSent < MAX_SENT (4)  -> script sent; ++mSent;
        //                                mWindowUntil = min(max(mWindowUntil, now + CHAIN (3 s)),
        //                                                   mWindowStart + MAX_WINDOW (15 s))
        //       else                  -> nothing
        //   good load, waiting (now < mWaitUntil) -> window opens: mWindowStart = now,
        //                                            mWindowUntil = now + SETTLE (8 s), mSent = 0,
        //                                            mWaitUntil = 0; then as "window open"
        //   good load, neither                    -> nothing (a link the user followed)
        //   error load (result 0 or >= 400), a load of another site -> nothing changes
        //   floater closed -> nothing (the times run out by themselves)
        //   Reload pressed again and again -> each press starts a new wait (bounded by the user's presses)
        void arm()
        {
            if (!gSavedSettings.getBOOL("PlusDestinationsPageLang"))
            {
                return;
            }
            const F64 now = LLFrameTimer::getElapsedSeconds();
            mArmedAt = now;
            mWaitUntil = now + WAIT_SECONDS;
            mWindowUntil = 0.0;
        }

        void handleMediaEvent(LLPluginClassMedia* self, EMediaEvent event) override
        {
            if (event != MEDIA_EVENT_NAVIGATE_COMPLETE || !self ||
                !(isGuideHost(self->getNavigateURI()) && isGuideHost(self->getLocation())))
            {
                return;
            }
            const S32 result = self->getNavigateResultCode();
            if (result == 0 || result >= 400)
            {
                LL_DEBUGS("Destinations") << "load result " << result << ": error, ignored" << LL_ENDL;
                return; // an error page: nothing to translate, and the next good load is still due
            }
            const std::string lang = pageLanguageFor(LLUI::getLanguage());
            if (lang.empty())
            {
                return;
            }
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (!gSavedSettings.getBOOL("PlusDestinationsPageLang"))
            {
                // Off: nothing is asked, and what is due is dropped, so turning it on later does not
                // switch the language on the next link the user follows.
                mWaitUntil = 0.0;
                mWindowUntil = 0.0;
                mAppliedLanguage = lang;
                return;
            }
            if (lang != mAppliedLanguage)
            {
                mAppliedLanguage = lang; // the UI language changed since the last time
                mArmedAt = now;
                mWaitUntil = now + WAIT_SECONDS;
            }
            if (!(now < mWindowUntil) && now < mWaitUntil)
            {
                mWindowStart = now;
                mWindowUntil = now + SETTLE_SECONDS;
                mSent = 0;
                mWaitUntil = 0.0;
            }
            if (!(now < mWindowUntil))
            {
                LL_DEBUGS("Destinations") << "load result " << result << " " << (now - mArmedAt)
                                          << " s after the last arm: outside the window, left alone" << LL_ENDL;
                return;
            }
            if (mSent >= MAX_SENT)
            {
                LL_DEBUGS("Destinations") << "load result " << result << ": window full (" << mSent
                                          << " asks), left alone" << LL_ENDL;
                return;
            }
            ++mSent;
            // The next load of this chain still counts if it comes soon after this one, up to a limit.
            mWindowUntil = std::min(std::max(mWindowUntil, now + CHAIN_SECONDS), mWindowStart + MAX_WINDOW_SECONDS);
            LL_DEBUGS("Destinations") << "load result " << result << " " << (now - mArmedAt)
                                      << " s after the last arm: asked for '" << lang << "' (" << mSent
                                      << ")" << LL_ENDL;
            self->executeJavaScript(std::string(SWITCH_SCRIPT_HEAD) + lang + "');");
        }

    private:
        static bool isGuideHost(const std::string& url)
        {
            return url.rfind("https://secondlife.com/", 0) == 0 ||
                   url.rfind("https://go.secondlife.com/", 0) == 0;
        }

        static constexpr F64 WAIT_SECONDS = 30.0;       // how long a first good load is waited for
        static constexpr F64 SETTLE_SECONDS = 8.0;      // the window, from its first good load
        static constexpr F64 CHAIN_SECONDS = 3.0;       // a load that follows another within this stays inside
        static constexpr F64 MAX_WINDOW_SECONDS = 15.0; // the window never lasts longer than this
        static constexpr S32 MAX_SENT = 4;              // asks per window (a reload loop stops here)

        F64 mWaitUntil = 0.0;
        F64 mWindowStart = 0.0;
        F64 mWindowUntil = 0.0;
        F64 mArmedAt = 0.0; // for the log only
        S32 mSent = 0;
        std::string mAppliedLanguage;
    };

    // One floater, one observer: a second postBuild replaces it, and the viewer's end deletes it
    // with the statics (the browser has removed the link by then, so nothing is used after it is freed).
    std::unique_ptr<PageLanguageObserver> sPageLanguage;

    void armPageLanguage()
    {
        if (sPageLanguage)
        {
            sPageLanguage->arm();
        }
    }

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
        armPageLanguage();
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
        armPageLanguage();
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
    void setupPageLanguage(LLFloater* floater)
    {
        if (LLMediaCtrl* browser = guideBrowser(floater))
        {
            sPageLanguage.reset(new PageLanguageObserver(browser));
        }
    }

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

    void onFloaterOpen(LLFloater* floater)
    {
        armPageLanguage(); // the page may load again by itself: those loads are asked for the language
        reloadIfLastLoadFailed(floater);
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
