/**
 * @file updatecheck.cpp
 * @brief Update check: once a day at start, read a small JSON file from our own
 *        site and, when it names a newer Charis Viewer, say so in a small window.
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

// research/123-update-check.md in the sl-viewer repository has the file's
// fields, the order of versions and how to test.
//
// How it runs:
//   onUIReady (the login screen is being shown)
//     -> PlusUpdateCheck on, a day since PlusUpdateCheckLastTime, an hour
//        since PlusUpdateCheckLastTry, a web URL in PlusUpdateCheckURL
//        (https; http only to this computer): the time of the try is kept
//        and settings.xml written, then a coroutine GETs the file (LLCore
//        HTTP; 8 s, no retries, no redirects, the server's certificate and
//        its host name checked, at most 16 KiB taken in; the login screen
//        does not wait for it)
//     -> no answer, an HTTP error, a file that is not what we expect: a log
//        line and nothing else (a start an hour or more later tries again;
//        a crash while reading does not repeat at every start)
//     -> same or older version, or the version in PlusUpdateCheckSkipped: the
//        time is kept, nothing shown
//     -> newer: the window (floater_plus_update.xml) opens at the login screen,
//        or once the world is up when the login has already started; the time
//        is kept when it opens.
//
// What is sent: a GET of the URL with Accept, User-Agent ("Charis Viewer
// 0.1.0 alpha 1") and libcurl's own transport headers. No account, avatar,
// machine or setting goes with it; the site sees the IP address and the
// version, as with any web page. The download page opens in the browser
// only when the button is pressed (LLWeb::loadURLExternal), and only on a
// host of PlusUpdateCheckAllowedHosts (empty: the host of PlusUpdateCheckURL).
// research/130 has the review this follows (H1, M1 to M3, L1, L2).
//
// The official update service (VVM) stays off (plus/version, llvvmquery.cpp);
// this does not touch it. Official files changed for it: the check box in
// panel_preferences_setup.xml (en), and the reply limit in llcorehttp
// (HttpOptions::setMaxReplyBytes(), used in _httpoprequest.cpp).

#include "llviewerprecompiledheaders.h"

#include "updateversion.h"
#include "plus.h"
#include "version/version.h"

#include "llapp.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcorehttputil.h"
#include "lldate.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llhttpconstants.h"
#include "llpanel.h"
#include "llstartup.h"
#include "llstring.h"
#include "llui.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llweb.h"

#include <boost/json.hpp>

#include <cmath>
#include <optional>

namespace plus::updatecheck
{
    namespace
    {
        constexpr char FLOATER_NAME[]    = "plus_update";
        constexpr char SETTING_ENABLED[] = "PlusUpdateCheck";
        constexpr char SETTING_URL[]     = "PlusUpdateCheckURL";
        constexpr char SETTING_SKIPPED[] = "PlusUpdateCheckSkipped";
        constexpr char SETTING_LAST[]    = "PlusUpdateCheckLastTime";
        constexpr char SETTING_LAST_TRY[] = "PlusUpdateCheckLastTry";
        constexpr char SETTING_ALLOWED[] = "PlusUpdateCheckAllowedHosts";

        constexpr uint32_t CHECK_INTERVAL_SECONDS = 24 * 60 * 60;   // once a day at most
        constexpr uint32_t RETRY_INTERVAL_SECONDS = 60 * 60;        // after a try without an answer
        constexpr unsigned TIMEOUT_SECONDS        = 8;              // to connect, and for the whole transfer
        constexpr size_t   MAX_BODY_BYTES         = 16 * 1024;      // the file is a few hundred bytes
        constexpr S32      MAX_MESSAGE_BYTES      = 1000;

        // What the file says about a newer version.
        struct Offer
        {
            Version     version;
            bool        required = false;   // the running version is below min_version
            std::string download_url;       // a web URL or ""
            std::string notes_url;          // a web URL or ""
            std::string message;            // message_ja or message_en, as the UI language
        };

        // Found by the check, until the window has been opened with it.
        std::optional<Offer> sOffer;

        uint32_t nowSeconds()
        {
            const F64 seconds = LLDate::now().secondsSinceEpoch();
            return seconds > 0.0 ? static_cast<uint32_t>(seconds) : 0;
        }

        // An answer: the next check is a day later. The time of the try is
        // cleared, so setting PlusUpdateCheckLastTime to 0 alone checks again.
        void markChecked()
        {
            gSavedSettings.setU32(SETTING_LAST, nowSeconds());
            gSavedSettings.setU32(SETTING_LAST_TRY, 0);
        }

        Version runningVersion()
        {
            Version v;
            v.major        = plus::version::MAJOR;
            v.minor        = plus::version::MINOR;
            v.patch        = plus::version::PATCH;
            v.stage_rank   = stageRank(plus::version::STAGE);
            v.stage_number = plus::version::STAGE_NUMBER;
            v.build        = LLVersionInfo::instance().getBuild();
            return v;
        }

        // "Charis Viewer 0.1.0 alpha 1": the product and its version, nothing else.
        std::string userAgent()
        {
            std::string agent = std::string(plus::version::PRODUCT_NAME) + " " + plus::version::shortVersion();
            const std::string stage = plus::version::stage();
            if (!stage.empty())
            {
                agent += " " + stage;
            }
            return agent;
        }

        // ---- The file ------------------------------------------------------

        // A string member; "" when it is missing or not a string.
        std::string stringField(const boost::json::object& obj, const char* name)
        {
            const boost::json::value* value = obj.if_contains(name);
            if (value && value->is_string())
            {
                const boost::json::string& text = value->get_string();
                return std::string(text.data(), text.size());
            }
            return std::string();
        }

        // An optional whole number, not negative: a JSON number or a string of
        // digits. A missing member (or null) leaves out as it is; false when
        // the member is there but not such a number.
        bool numberField(const boost::json::object& obj, const char* name, uint64_t& out)
        {
            const boost::json::value* value = obj.if_contains(name);
            if (!value || value->is_null())
            {
                return true;
            }
            if (value->is_int64())
            {
                if (value->get_int64() < 0)
                {
                    return false;
                }
                out = static_cast<uint64_t>(value->get_int64());
                return true;
            }
            if (value->is_uint64())
            {
                out = value->get_uint64();
                return true;
            }
            if (value->is_double())
            {
                const double d = value->get_double();
                if (d < 0.0 || d > 1e15 || std::floor(d) != d)
                {
                    return false;
                }
                out = static_cast<uint64_t>(d);
                return true;
            }
            if (value->is_string())
            {
                const boost::json::string& text = value->get_string();
                if (text.empty() || text.size() > 15)
                {
                    return false;
                }
                uint64_t n = 0;
                for (char c : text)
                {
                    if (c < '0' || c > '9')
                    {
                        return false;
                    }
                    n = n * 10 + static_cast<uint64_t>(c - '0');
                }
                out = n;
                return true;
            }
            return false;
        }

        // A URL member that may be opened later: kept only when it is a web
        // URL (webURL(): https, no user name, ASCII only) on an allowed host.
        std::string urlField(const boost::json::object& obj, const char* name, const std::string& allowed_hosts)
        {
            const std::string text = stringField(obj, name);
            if (text.empty())
            {
                return text;
            }
            const std::string url = webURL(text);
            if (url.empty())
            {
                LL_WARNS("UpdateCheck") << "\"" << name << "\" is not an https URL that may be opened; left out" << LL_ENDL;
                return url;
            }
            if (!hostInList(urlHost(url), allowed_hosts))
            {
                LL_WARNS("UpdateCheck") << "\"" << name << "\" is on " << urlHost(url) << ", not on a host of "
                                        << SETTING_ALLOWED << " (" << allowed_hosts << "); left out" << LL_ENDL;
                return std::string();
            }
            return url;
        }

        bool readOffer(const std::string& body, const std::string& allowed_hosts, Offer& offer, std::string& why)
        {
            // A file saved with a UTF-8 byte order mark (Windows editors, PowerShell) is accepted.
            std::string_view text(body);
            if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
            {
                text.remove_prefix(3);
            }

            boost::system::error_code ec;
            const boost::json::value root = boost::json::parse(text, ec);
            if (ec || !root.is_object())
            {
                why = "not a JSON object";
                return false;
            }
            const boost::json::object& obj = root.get_object();

            Version& v = offer.version;
            if (!parseNumbers(stringField(obj, "version"), v.major, v.minor, v.patch))
            {
                why = "no \"version\" of the form 0.1.0";
                return false;
            }
            v.stage_rank = stageRank(stringField(obj, "stage"));
            if (v.stage_rank == STAGE_UNKNOWN)
            {
                why = "\"stage\" is not alpha, beta, rc or empty";
                return false;
            }
            uint64_t stage_number = 0;
            if (!numberField(obj, "stage_number", stage_number) || stage_number > 9999)
            {
                why = "\"stage_number\" is not a small whole number";
                return false;
            }
            if (v.stage_rank == STAGE_RELEASE && stage_number != 0)
            {
                // key() leaves a release's stage number out: "skip 0.2.0 (1)" would also skip 0.2.0 (2).
                why = "a release (\"stage\": \"\") has a \"stage_number\"";
                return false;
            }
            v.stage_number = static_cast<int>(stage_number);
            if (!numberField(obj, "build", v.build))
            {
                why = "\"build\" is not a whole number";
                return false;
            }

            const std::string min_version = stringField(obj, "min_version");
            int min_major = 0, min_minor = 0, min_patch = 0;
            if (!min_version.empty())
            {
                if (parseNumbers(min_version, min_major, min_minor, min_patch))
                {
                    offer.required = numbersBelow(runningVersion(), min_major, min_minor, min_patch);
                }
                else
                {
                    LL_WARNS("UpdateCheck") << "\"min_version\" is not of the form 0.1.0; ignored" << LL_ENDL;
                }
            }

            offer.download_url = urlField(obj, "download_url", allowed_hosts);
            offer.notes_url    = urlField(obj, "notes_url", allowed_hosts);

            const bool japanese = LLUI::getLanguage().rfind("ja", 0) == 0;
            std::string message = stringField(obj, japanese ? "message_ja" : "message_en");
            if (message.empty())
            {
                message = stringField(obj, japanese ? "message_en" : "message_ja");
            }
            offer.message = utf8str_truncate(message, MAX_MESSAGE_BYTES);
            return true;
        }

        // ---- Showing it ----------------------------------------------------

        // Runs every frame from the check until the window could be opened:
        // at the login screen (waiting for the user), or once the world is up
        // when the login had already started.
        bool showWhenReady()
        {
            if (!sOffer || LLApp::isExiting())
            {
                return true;
            }
            const EStartupState state = LLStartUp::getStartupState();
            if (state != STATE_LOGIN_WAIT && state != STATE_STARTED)
            {
                return false;
            }
            if (!gSavedSettings.getBOOL(SETTING_ENABLED))
            {
                LL_INFOS("UpdateCheck") << "Turned off before it was shown; not shown" << LL_ENDL;
                sOffer.reset();
                return true;
            }

            LLFloaterReg::showInstance(FLOATER_NAME);   // the window copies sOffer in onOpen()
            sOffer.reset();
            markChecked();
            return true;
        }

        void decide(Offer offer)
        {
            const Version current = runningVersion();
            const std::string newest = displayText(offer.version);

            if (compare(offer.version, current) <= 0)
            {
                LL_INFOS("UpdateCheck") << "Up to date: running " << displayText(current) << ", newest " << newest << LL_ENDL;
                markChecked();
                return;
            }
            if (!offer.required && gSavedSettings.getString(SETTING_SKIPPED) == key(offer.version))
            {
                LL_INFOS("UpdateCheck") << "Newer version " << newest << " is one the user asked not to hear about" << LL_ENDL;
                markChecked();
                return;
            }

            LL_INFOS("UpdateCheck") << "Newer version " << newest << (offer.required ? " (running version below min_version)" : "")
                                    << "; shown at the login screen or once the world is up" << LL_ENDL;
            const bool idle_running = sOffer.has_value();
            sOffer = std::move(offer);
            if (!idle_running)
            {
                doOnIdleRepeating(&showWhenReady);
            }
        }

        // ---- The check -----------------------------------------------------

        void checkCoro(std::string url, std::string allowed_hosts)
        {
            LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
            auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("PlusUpdateCheck", policy);
            auto request = std::make_shared<LLCore::HttpRequest>();

            auto options = std::make_shared<LLCore::HttpOptions>();
            options->setTimeout(TIMEOUT_SECONDS);
            options->setTransferTimeout(TIMEOUT_SECONDS);
            options->setRetries(0);
            // A static file: no redirects (none from https down to http either).
            options->setFollowRedirects(false);
            // The certificate and the host name it is for. By default libcurl
            // is told not to check the name, and the viewer's own check leaves
            // it to libcurl, so any certificate a trusted CA gave to any site
            // passed (research/130 H1). Asked for here whatever the defaults.
            // With NoVerifySSLCert on, the viewer's chain check (the OpenSSL
            // verify callback, LLBasicCertificateStore::validate) passes any
            // certificate, so the chain is NOT verified then: only libcurl's
            // host name match is left (a self-made certificate with the right
            // name would pass).
            options->setSSLVerifyPeer(true);
            options->setSSLVerifyHost(true);
            // At most 16 KiB taken in, counted as it arrives (after unpacking,
            // should the server send it packed anyway): more ends the transfer.
            options->setMaxReplyBytes(MAX_BODY_BYTES);

            // The adapter adds an LLSD Accept, an LLSD Content-Type and the
            // port of the simulator link unless they are given; libcurl leaves
            // out a header whose value is empty. No Accept-Encoding: the file
            // is small, and nothing packed is asked for.
            auto headers = std::make_shared<LLCore::HttpHeaders>();
            headers->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_JSON);
            headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "");
            headers->append("X-SecondLife-UDP-Listen-Port", "");
            headers->append("Accept-Encoding", "");
            headers->append(HTTP_OUT_HEADER_USER_AGENT, userAgent());

            LL_INFOS("UpdateCheck") << "Reading " << url << LL_ENDL;
            const LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
            if (LLApp::isExiting())
            {
                return;
            }

            const LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
            const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
            if (!status)
            {
                if (status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_WRITE_ERROR)
                    || status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_FILESIZE_EXCEEDED))
                {
                    LL_WARNS("UpdateCheck") << "The answer is more than " << MAX_BODY_BYTES << " bytes; ignored" << LL_ENDL;
                    return;
                }
                LL_INFOS("UpdateCheck") << "No answer (" << status.toString() << "); nothing shown, tried again at a start an hour or more from now" << LL_ENDL;
                return;
            }

            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            if (raw.size() > MAX_BODY_BYTES)
            {
                LL_WARNS("UpdateCheck") << "The answer is " << raw.size() << " bytes, more than " << MAX_BODY_BYTES << "; ignored" << LL_ENDL;
                return;
            }

            Offer offer;
            std::string why;
            if (!readOffer(std::string(raw.begin(), raw.end()), allowed_hosts, offer, why))
            {
                LL_WARNS("UpdateCheck") << "The answer is not an update file (" << why << "); ignored" << LL_ENDL;
                return;
            }
            decide(std::move(offer));
        }

        void startCheck()
        {
            if (!gSavedSettings.getBOOL(SETTING_ENABLED))
            {
                LL_INFOS("UpdateCheck") << "Off (" << SETTING_ENABLED << ")" << LL_ENDL;
                return;
            }
            const uint32_t last = gSavedSettings.getU32(SETTING_LAST);
            if (!dueForCheck(nowSeconds(), last, CHECK_INTERVAL_SECONDS))
            {
                LL_INFOS("UpdateCheck") << "Checked less than a day ago; not now" << LL_ENDL;
                return;
            }
            const uint32_t now = nowSeconds();
            if (!dueForCheck(now, gSavedSettings.getU32(SETTING_LAST_TRY), RETRY_INTERVAL_SECONDS))
            {
                LL_INFOS("UpdateCheck") << "Tried less than an hour ago without an answer; not now" << LL_ENDL;
                return;
            }
            const std::string url_text = gSavedSettings.getString(SETTING_URL);
            if (url_text.find_first_not_of(" \t") == std::string::npos)
            {
                // The default: no update site is set yet (research/123 §5).
                LL_INFOS("UpdateCheck") << SETTING_URL << " is empty (no update site set); not checked" << LL_ENDL;
                return;
            }
            const std::string url = webURL(url_text);
            if (url.empty())
            {
                LL_WARNS("UpdateCheck") << SETTING_URL << " is not an https URL (http only to this computer); not checked" << LL_ENDL;
                return;
            }
            // Pages the window may open: these hosts, or the update file's own.
            std::string allowed_hosts = gSavedSettings.getString(SETTING_ALLOWED);
            if (allowed_hosts.find_first_not_of(", ;\t") == std::string::npos)
            {
                allowed_hosts = urlHost(url);
            }

            // The try is on disk before anything is read: should reading the
            // answer end the viewer, the next start does not read it again at
            // once (research/130 M2).
            gSavedSettings.setU32(SETTING_LAST_TRY, now);
            const std::string settings_file = gSavedSettings.getString("ClientSettingsFile");
            if (!settings_file.empty())
            {
                plus::saveSettingsFile(gSavedSettings, settings_file, true);   // as llstartup.cpp does once connected
            }

            LLCoros::instance().launch("plus::updatecheck", [url, allowed_hosts]() { checkCoro(url, allowed_hosts); });
        }

        // ---- The window ----------------------------------------------------

        class UpdateFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                LLButton* page_button = getChild<LLButton>("btn_download");
                page_button->setCommitCallback([this](LLUICtrl*, const LLSD&) { openPage(); closeFloater(); });
                mDownloadLabel = page_button->getLabelUnselected();
                mDownloadTip   = page_button->getToolTip();
                getChild<LLButton>("btn_later")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
                getChild<LLButton>("btn_skip")->setCommitCallback([this](LLUICtrl*, const LLSD&) { skipThisVersion(); closeFloater(); });
                return true;
            }

            void onOpen(const LLSD&) override
            {
                if (sOffer)
                {
                    mOffer = *sOffer;
                }

                LLStringUtil::format_map_t args;
                args["[VERSION]"] = displayText(mOffer.version);
                args["[CURRENT]"] = displayText(runningVersion());
                std::string text = getString(mOffer.required ? "body_required" : "body", args);
                if (!mOffer.message.empty())
                {
                    text += "\n\n" + mOffer.message;
                }
                if (!mOffer.notes_url.empty())
                {
                    LLStringUtil::format_map_t url_args;
                    url_args["[URL]"] = mOffer.notes_url;
                    text += "\n\n" + getString("notes", url_args);
                }
                if (!mOffer.download_url.empty())
                {
                    LLStringUtil::format_map_t url_args;
                    url_args["[URL]"] = mOffer.download_url;
                    text += "\n" + getString("page", url_args);
                }
                getChild<LLUICtrl>("message")->setValue(text);

                // fix-round68 3: with only the notes named, the button says it opens them (the
                // text has no links: the button is the only way to a page).
                LLButton* page_button = getChild<LLButton>("btn_download");
                page_button->setEnabled(!pageURL().empty());
                const bool notes_only = mOffer.download_url.empty() && !mOffer.notes_url.empty();
                page_button->setLabel(notes_only ? getString("btn_notes") : mDownloadLabel);
                page_button->setToolTip(notes_only ? getString("btn_notes_tip") : mDownloadTip);
                getChild<LLButton>("btn_skip")->setVisible(!mOffer.required);
            }

        private:
            explicit UpdateFloater(const LLSD& key) : LLFloater(key) {}

            // The download page, or the notes when the file names no download page.
            const std::string& pageURL() const
            {
                return mOffer.download_url.empty() ? mOffer.notes_url : mOffer.download_url;
            }

            void openPage()
            {
                const std::string& url = pageURL();
                if (!url.empty())
                {
                    LL_INFOS("UpdateCheck") << "Opening " << url << LL_ENDL;
                    LLWeb::loadURLExternal(url);
                }
            }

            void skipThisVersion()
            {
                const std::string skipped = plus::updatecheck::key(mOffer.version);
                gSavedSettings.setString(SETTING_SKIPPED, skipped);
                LL_INFOS("UpdateCheck") << "Not told about " << skipped << " again" << LL_ENDL;
            }

            Offer mOffer;
            std::string mDownloadLabel;     // btn_download as the XUI has it (the download page)
            std::string mDownloadTip;
        };

        // ---- The on/off box in Preferences > Setup -------------------------
        // fix-round103 (2026-10-24): shown only once the update site is named (PlusUpdateCheckURL not
        // empty; it is empty in the releases until the site is decided: research/123). While it is
        // hidden, what is below it in the panel moves up into its place, so no empty row is left.
        // The box stays (hidden) and keeps PlusUpdateCheck; nothing about the check itself changes.
        class UpdateCheckPanel : public LLPanel
        {
        public:
            bool postBuild() override
            {
                mBox = getChild<LLView>("plus_update_check_enabled");
                if (LLControlVariable* url = gSavedSettings.getControl(SETTING_URL).get())
                {
                    mURLChanged = url->getCommitSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { applyLayout(); });
                }
                mBox->setVisible(siteNamed());  // the rest waits for the panels below to be built
                return true;
            }

            // Preferences' Setup tab shown: every sibling is built by now.
            void onVisibilityChange(bool new_visibility) override
            {
                LLPanel::onVisibilityChange(new_visibility);
                if (new_visibility)
                {
                    applyLayout();
                }
            }

        private:
            static bool siteNamed()
            {
                static LLCachedControl<std::string> url(gSavedSettings, SETTING_URL, "");
                std::string value = url();
                LLStringUtil::trim(value);
                return !value.empty();
            }

            // The views below this one in the panel, and how far the nearest of them would move up to
            // take this one's place (its top to this one's top). Once, in the places the XUI gave them.
            void findBelow()
            {
                mBelowFound = true;
                LLView* parent = getParent();
                if (!parent)
                {
                    return;
                }
                const LLRect mine = getRect();
                S32 nearest_top = S32_MIN;
                for (LLView* view : *parent->getChildList())
                {
                    if (view != this && view->getRect().mTop <= mine.mBottom)
                    {
                        mBelow.push_back(view->getHandle());
                        nearest_top = llmax(nearest_top, view->getRect().mTop);
                    }
                }
                mGap = mBelow.empty() ? 0 : mine.mTop - nearest_top;
            }

            // Only what changed is touched: the box's visibility, and the views below by the difference.
            void applyLayout()
            {
                const bool shown = siteNamed();
                if (mBox && mBox->getVisible() != shown)
                {
                    mBox->setVisible(shown);
                }
                if (!getParent())
                {
                    return;
                }
                if (!mBelowFound)
                {
                    findBelow();
                }
                const S32 shift = shown ? 0 : mGap;
                if (shift == mShift)
                {
                    return;
                }
                for (const LLHandle<LLView>& handle : mBelow)
                {
                    if (LLView* view = handle.get())
                    {
                        view->translate(0, shift - mShift);   // up: +y
                    }
                }
                mShift = shift;
            }

            LLView*                               mBox = nullptr;
            std::vector<LLHandle<LLView>>         mBelow;
            bool                                  mBelowFound = false;
            S32                                   mGap   = 0;  // how far the views below move up while hidden
            S32                                   mShift = 0;  // how far they are moved now
            boost::signals2::scoped_connection    mURLChanged;
        };

        // panel_preferences_setup.xml names this class on its <panel filename="panel_plus_update_check.xml">.
        LLPanelInjector<UpdateCheckPanel> sUpdateCheckPanel("plus_update_check_panel");

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_update.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<UpdateFloater>);
        }

        void onUIReady()
        {
            startCheck();
        }

        plus::FeatureRegistration sRegistration({ "updatecheck", &init, &onUIReady });
    }
}
