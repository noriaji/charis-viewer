/**
 * @file updatecheck.cpp
 * @brief Update check: once a day at start, read a small JSON file and, when it
 *        names a newer Charis Viewer, say so in a band at the bottom of the login
 *        screen (or in a small window once the world is up).
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
// fields, the order of versions and how to test; research/325 the band at the
// login screen and the copy of the answer kept between starts.
//
// How it runs:
//   onUIReady (the login screen is being shown), PlusUpdateCheck on and a web
//   URL in PlusUpdateCheckURL (https; http only to this computer):
//     -> the copy of the last newer answer (PlusUpdateCheckOffer) is read again
//        as an answer (readOfferFile(): size, version, addresses and their
//        hosts); still newer and not skipped: it is offered at once. Broken or
//        caught up with: the copy is cleared.
//     -> a day since PlusUpdateCheckLastTime and an hour since
//        PlusUpdateCheckLastTry: the time of the try is kept and settings.xml
//        written, then a coroutine GETs the file (LLCore HTTP; 8 s, no
//        retries, no redirects, the server's certificate and its host name
//        checked, at most 16 KiB taken in; the login screen does not wait)
//     -> the site answers 4xx (404 and the like; not 429): a log line, the
//        time kept (the next check a day later), the copy not touched
//     -> no answer, 429, 5xx, a redirect, a file that is not what we expect:
//        a log line and nothing else (the copy is not touched; a start an
//        hour or more later tries again; a crash while reading does not
//        repeat at every start)
//     -> same or older version: the copy is cleared, the time kept, nothing shown
//     -> the version in PlusUpdateCheckSkipped: the copy kept, the time kept,
//        nothing shown
//     -> newer: the copy kept, the time kept, and it is offered.
//   An offer is shown while the login screen waits for the user, as the band
//   (panel_plus_update_banner.xml) at its bottom (at its top in a window too
//   low for the login column and the band); when the answer comes once
//   the login has started, as the window (floater_plus_update.xml) once the
//   world is up. A version shown once in a run is not shown again in that run.
//   The band goes when the login starts, with x (this run only), "Skip this
//   version", "Stop notices" (PlusUpdateCheck off) or when the check is turned
//   off elsewhere.
//
// What is sent: a GET of the URL with Accept, User-Agent ("Charis Viewer
// 0.1.0 alpha 1") and libcurl's own transport headers. No account, avatar,
// machine or setting goes with it; the site sees the IP address and the
// version, as with any web page. The download page opens in the browser
// only when a button is pressed (LLWeb::loadURLExternal), and only on a
// host of PlusUpdateCheckAllowedHosts (empty: the host of PlusUpdateCheckURL).
// research/130 has the review this follows (H1, M1 to M3, L1, L2).
//
// Where updates can be installed (Setup.exe; plus/update, research/326), the band's and the
// window's first button downloads and installs instead of opening the page ("Download and
// update"; PlusUpdateInstall 2: no band or window, it starts at once), and while such an
// update is on its way, ready or failed, nothing is shown here (plus/update's chip tells of it).
//
// The official update service (VVM) stays off (plus/version, llvvmquery.cpp);
// this does not touch it. Official files changed for it: the check box in
// panel_preferences_setup.xml (en), and the reply limit in llcorehttp
// (HttpOptions::setMaxReplyBytes(), used in _httpoprequest.cpp).

#include "llviewerprecompiledheaders.h"

#include "updateoffer.h"
#include "updateversion.h"
#include "plus.h"
#include "photoscreen/psui/pswidgets.h"
#include "update/update.h"
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
#include "llviewerwindow.h"
#include "llweb.h"

#include <optional>

namespace plus::updatecheck
{
    namespace
    {
        constexpr char FLOATER_NAME[]     = "plus_update";
        constexpr char BANNER_FILE[]      = "panel_plus_update_banner.xml";
        constexpr char SETTING_ENABLED[]  = "PlusUpdateCheck";
        constexpr char SETTING_URL[]      = "PlusUpdateCheckURL";
        constexpr char SETTING_SKIPPED[]  = "PlusUpdateCheckSkipped";
        constexpr char SETTING_LAST[]     = "PlusUpdateCheckLastTime";
        constexpr char SETTING_LAST_TRY[] = "PlusUpdateCheckLastTry";
        constexpr char SETTING_ALLOWED[]  = "PlusUpdateCheckAllowedHosts";
        constexpr char SETTING_OFFER[]    = "PlusUpdateCheckOffer";

        constexpr uint32_t CHECK_INTERVAL_SECONDS = 24 * 60 * 60;   // once a day at most
        constexpr uint32_t RETRY_INTERVAL_SECONDS = 60 * 60;        // after a try without an answer
        constexpr unsigned TIMEOUT_SECONDS        = 8;              // to connect, and for the whole transfer

        constexpr S32 BANNER_SIDE     = 24;   // the band's left and right to the window's
        constexpr S32 BANNER_BOTTOM   = 12;   // the band's bottom (or top) to the window's
        constexpr S32 BANNER_MIN_TEXT = 160;  // less room for the words: only x and Open are kept

        // What is offered: an update file read for the running version and the UI language.
        struct Offer
        {
            Version     version;
            bool        required = false;   // the running version is below min_version
            std::string download_url;       // a web URL or ""
            std::string notes_url;          // a web URL or ""
            std::string message;            // message_ja or message_en, as the UI language
        };

        // Where an answer or the copy of one came from (consider()).
        enum class Origin
        {
            Saved,      // PlusUpdateCheckOffer, read again at start
            Answer,     // the update site, just now
        };

        // ---- State (research/325 section 5; all on the main thread) --------
        // The offer waiting to be shown, or shown in the band. Cleared when the band is closed with a
        // button, the login starts, the window has taken it, the check is turned off, or an answer says
        // there is nothing newer.
        std::optional<Offer> sOffer;
        U32 sOfferSerial = 0;               // counts the offers given to sOffer (the band refills on a change)
        bool sTickRunning = false;          // tick() is on the idle list
        std::string sShownKey;              // key() of the version shown in this run (band or window); "" none
        std::string sAutoKey;               // key() of the version started without asking in this run (PlusUpdateInstall 2); "" none
        bool sBannerFailed = false;         // the band's XUI could not be built: not tried again in this run

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

        // settings.xml written now (as llstartup.cpp does once connected): a try, or what the user asked
        // for at the login screen, is kept even when the viewer ends before it writes the file itself.
        void saveSettingsNow()
        {
            const std::string settings_file = gSavedSettings.getString("ClientSettingsFile");
            if (!settings_file.empty())
            {
                plus::saveSettingsFile(gSavedSettings, settings_file, true);
            }
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

        // The update file and the hosts its pages may be on: PlusUpdateCheckURL as a web URL, and
        // PlusUpdateCheckAllowedHosts (empty: the update file's own host). nullopt, with a log line,
        // when no update site is set or it is not such a URL.
        struct Source
        {
            std::string url;
            std::string allowed_hosts;
        };

        std::optional<Source> updateSource()
        {
            const std::string url_text = gSavedSettings.getString(SETTING_URL);
            if (url_text.find_first_not_of(" \t") == std::string::npos)
            {
                LL_INFOS("UpdateCheck") << SETTING_URL << " is empty (no update site set); not checked" << LL_ENDL;
                return std::nullopt;
            }
            Source source;
            source.url = webURL(url_text);
            if (source.url.empty())
            {
                LL_WARNS("UpdateCheck") << SETTING_URL << " is not an https URL (http only to this computer); not checked" << LL_ENDL;
                return std::nullopt;
            }
            source.allowed_hosts = gSavedSettings.getString(SETTING_ALLOWED);
            if (hostListEmpty(source.allowed_hosts))
            {
                source.allowed_hosts = urlHost(source.url);
            }
            return source;
        }

        // An update file (an answer, or the copy of one) read and checked; the fields left out are logged.
        bool readOffer(std::string_view body, const std::string& allowed_hosts, OfferFile& file, std::string& why)
        {
            std::vector<std::string> notes;
            const bool ok = readOfferFile(body, allowed_hosts, file, why, notes);
            for (const std::string& note : notes)
            {
                LL_WARNS("UpdateCheck") << note << LL_ENDL;
            }
            return ok;
        }

        // The file's offer for the running version and the UI language.
        Offer offerFor(const OfferFile& file)
        {
            Offer offer;
            offer.version      = file.version;
            offer.required     = file.has_min_version
                                 && numbersBelow(runningVersion(), file.min_major, file.min_minor, file.min_patch);
            offer.download_url = file.download_url;
            offer.notes_url    = file.notes_url;
            const bool japanese = LLUI::getLanguage().rfind("ja", 0) == 0;
            offer.message = japanese ? file.message_ja : file.message_en;
            if (offer.message.empty())
            {
                offer.message = japanese ? file.message_en : file.message_ja;
            }
            return offer;
        }

        // ---- What the band and the window share ----------------------------

        // The download page, or the notes when the file names no download page.
        const std::string& pageURL(const Offer& offer)
        {
            return offer.download_url.empty() ? offer.notes_url : offer.download_url;
        }

        // The file names only the notes: the page button says it opens them.
        bool notesOnly(const Offer& offer)
        {
            return offer.download_url.empty() && !offer.notes_url.empty();
        }

        void openPage(const Offer& offer)
        {
            const std::string& url = pageURL(offer);
            if (!url.empty())
            {
                LL_INFOS("UpdateCheck") << "Opening " << url << LL_ENDL;
                LLWeb::loadURLExternal(url);
            }
        }

        // The user agreed to install offer: plus/update takes over (its chip). Could it not start, the
        // page is opened instead.
        void installOrOpen(const Offer& offer)
        {
            if (plus::update::engaged())
            {
                return;     // already on its way or failed (its chip tells; the band and the window are not shown then)
            }
            if (!plus::update::begin(displayText(offer.version), packageVersion(offer.version)))
            {
                openPage(offer);
            }
        }

        void skipVersion(const Offer& offer)
        {
            const std::string skipped = key(offer.version);
            gSavedSettings.setString(SETTING_SKIPPED, skipped);
            LL_INFOS("UpdateCheck") << "Not told about " << skipped << " again" << LL_ENDL;
        }

        // The first sentence: "[VERSION] is out" or, below min_version, "no longer supported". The
        // strings are the panel's own ("body", "body_required").
        std::string headline(const LLPanel& strings, const Offer& offer)
        {
            LLStringUtil::format_map_t args;
            args["[VERSION]"] = displayText(offer.version);
            args["[CURRENT]"] = displayText(runningVersion());
            return strings.getString(offer.required ? "body_required" : "body", args);
        }

        // The sentence, the file's message and the pages as plain text (no links: research/130 M1).
        std::string bodyText(const LLPanel& strings, const Offer& offer)
        {
            std::string text = headline(strings, offer);
            if (!offer.message.empty())
            {
                text += "\n\n" + offer.message;
            }
            if (!offer.notes_url.empty())
            {
                LLStringUtil::format_map_t url_args;
                url_args["[URL]"] = offer.notes_url;
                text += "\n\n" + strings.getString("notes", url_args);
            }
            if (!offer.download_url.empty())
            {
                LLStringUtil::format_map_t url_args;
                url_args["[URL]"] = offer.download_url;
                text += "\n" + strings.getString("page", url_args);
            }
            return text;
        }

        // ---- The band at the login screen ----------------------------------

        class UpdateBanner : public LLPanel
        {
        public:
            // A band in holder (login_panel_holder); nullptr when the XUI cannot be built. Built from the
            // file alone (as flycamexit.cpp's panel), so postBuild() runs once, with the children there.
            static UpdateBanner* create(LLView* holder)
            {
                UpdateBanner* banner = new UpdateBanner();
                if (!banner->buildFromFile(BANNER_FILE))
                {
                    delete banner;
                    return nullptr;
                }
                holder->addChild(banner);
                banner->place(holder);
                return banner;
            }

            // At the bottom of holder; at its top when there the band would come up to the version line of
            // the login column (panel_login's version_text: a low window). Only when that changed: called
            // every frame while the band is up (the window and the login column can change any frame).
            void place(LLView* holder)
            {
                if (!mVersionLooked)
                {
                    mVersionLooked = true;
                    if (LLView* version = holder->findChild<LLView>("version_text"))
                    {
                        mVersionText = version->getHandle();
                    }
                }
                const LLRect area = holder->getLocalRect();
                const S32 height  = getRect().getHeight();
                const S32 left    = area.mLeft + BANNER_SIDE;
                const S32 right   = llmax(left + 1, area.mRight - BANNER_SIDE);
                LLRect wanted(left, area.mBottom + BANNER_BOTTOM + height, right, area.mBottom + BANNER_BOTTOM);
                bool at_top = false;
                if (const LLView* version = mVersionText.get(); version && version->getVisible())
                {
                    LLRect line;
                    version->localRectToOtherView(version->getLocalRect(), &line, holder);
                    at_top = wanted.mTop + psui::style::PANEL_GAP > line.mBottom;
                }
                if (at_top)
                {
                    wanted.set(left, area.mTop - BANNER_BOTTOM, right, area.mTop - BANNER_BOTTOM - height);
                }
                if (wanted != getRect())
                {
                    setFollows(FOLLOWS_LEFT | FOLLOWS_RIGHT | (at_top ? FOLLOWS_TOP : FOLLOWS_BOTTOM));
                    setShape(wanted);
                }
            }

            // Whether the band holds the offer numbered serial already.
            bool holds(U32 serial) const
            {
                return mFilled && serial == mSerial;
            }

            bool postBuild() override
            {
                mBox   = getChild<LLView>("banner_box");
                mText  = getChild<LLUICtrl>("banner_text");
                mOpen  = getChild<psui::PsButton>("banner_open");
                mSkip  = getChild<LLView>("banner_skip");
                mStop  = getChild<LLView>("banner_stop");
                mClose = getChild<LLView>("banner_close");
                mOpenLabel = mOpen->getLabelText();
                mOpenTip   = mOpen->getToolTip();
                // The spacing the XUI gives: the last button to the box's right, between the buttons, the
                // words to the first button.
                const LLRect stop = mStop->getRect(), close = mClose->getRect();
                mEdge     = mBox->getRect().getWidth() - close.mRight;
                mGap      = close.mLeft - stop.mRight;
                mTextGap  = mOpen->getRect().mLeft - mText->getRect().mRight;

                // The buttons only change the state; tick() takes the band away the next frame (a view does
                // not delete itself in its own callback).
                mOpen->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    if (mInstalls)
                    {
                        installOrOpen(mOffer);
                        closeBand();    // the chip at the bottom right takes over
                        return;
                    }
                    openPage(mOffer);
                });
                getChild<LLUICtrl>("banner_skip")->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    skipVersion(mOffer);
                    saveSettingsNow();
                    closeBand();
                });
                getChild<LLUICtrl>("banner_stop")->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    LL_INFOS("UpdateCheck") << "Turned off from the login screen (" << SETTING_ENABLED << ")" << LL_ENDL;
                    gSavedSettings.setBOOL(SETTING_ENABLED, false);
                    saveSettingsNow();
                    closeBand();
                });
                getChild<LLUICtrl>("banner_close")->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    LL_INFOS("UpdateCheck") << "The band was closed; shown again at the next start" << LL_ENDL;
                    closeBand();
                });
                return true;
            }

            void reshape(S32 width, S32 height, bool called_from_parent = true) override
            {
                LLPanel::reshape(width, height, called_from_parent);
                arrange();
            }

            // Fills the band with offer, numbered serial; nothing when it has that one already.
            void show(const Offer& offer, U32 serial)
            {
                if (holds(serial))
                {
                    return;
                }
                mFilled = true;
                mSerial = serial;
                mOffer  = offer;

                const std::string text = headline(*this, mOffer);
                const std::string tip  = bodyText(*this, mOffer);
                mText->setValue(text);
                mText->setToolTip(tip);
                setToolTip(tip);

                fillOpenButton(plus::update::canInstall());
                setVisible(true);
                arrange();
            }

            // Every frame while the band is up: the first button follows whether updates can be installed
            // now (a setting changed); only when that changed.
            void followInstall()
            {
                const bool installs = plus::update::canInstall();
                if (mFilled && installs != mInstalls)
                {
                    fillOpenButton(installs);
                }
            }

        protected:
            UpdateBanner() = default;

        private:
            // The first button: "Download and update" (plus/update) or the page.
            void fillOpenButton(bool installs)
            {
                mInstalls = installs;
                const bool notes_only = notesOnly(mOffer);
                if (mInstalls)
                {
                    mOpen->setLabelText(getString("btn_install"));
                    mOpen->setToolTip(getString("btn_install_tip"));
                }
                else
                {
                    mOpen->setLabelText(notes_only ? getString("btn_notes") : mOpenLabel);
                    mOpen->setToolTip(notes_only ? getString("btn_notes_tip") : mOpenTip);
                }
                mOpen->setEnabled(mInstalls || !pageURL(mOffer).empty());
            }

            // Hidden at once; tick() deletes the band the next frame (sOffer is gone).
            void closeBand()
            {
                setVisible(false);
                sOffer.reset();
            }

            // From the right, mGap apart, the buttons that are wanted and fit; the words take the rest.
            // Below min_version, "Skip this version" and x are not wanted (as in the window: it cannot be
            // skipped nor closed for the run). When the words would get less than BANNER_MIN_TEXT, "Stop
            // notices" and "Skip this version" are left out too (x and Open stay); a button that does not
            // fit at all is hidden, so nothing goes left of the words' start.
            void arrange()
            {
                if (!mBox)
                {
                    return;     // reshaped while buildFromFile() is still making the children
                }
                const bool required = mFilled && mOffer.required;
                struct Slot
                {
                    LLView* button;
                    bool    wanted;
                    bool    optional;   // left out when the band is narrow
                };
                const Slot slots[] = {
                    { mClose, !required, false },
                    { mStop,  true,      true  },
                    { mSkip,  !required, true  },
                    { mOpen,  true,      false },
                };
                const S32 text_left = mText->getRect().mLeft;
                const S32 width     = mBox->getRect().getWidth();

                S32 all_buttons = 0;
                for (const Slot& slot : slots)
                {
                    if (slot.wanted)
                    {
                        all_buttons += slot.button->getRect().getWidth() + mGap;
                    }
                }
                const bool narrow = width - mEdge - all_buttons + mGap - mTextGap - text_left < BANNER_MIN_TEXT;

                S32 x = width - mEdge;
                for (const Slot& slot : slots)
                {
                    const LLRect r = slot.button->getRect();
                    const bool shown = slot.wanted && !(narrow && slot.optional) && x - r.getWidth() >= text_left;
                    if (slot.button->getVisible() != shown)
                    {
                        slot.button->setVisible(shown);
                    }
                    if (!shown)
                    {
                        continue;
                    }
                    if (r.mRight != x)
                    {
                        slot.button->setShape(LLRect(x - r.getWidth(), r.mTop, x, r.mBottom));
                    }
                    x -= r.getWidth() + mGap;
                }
                const LLRect words = mText->getRect();
                const S32 right = llmax(words.mLeft, x + mGap - mTextGap);
                if (words.mRight != right)
                {
                    mText->setShape(LLRect(words.mLeft, words.mTop, right, words.mBottom));
                }
            }

            Offer         mOffer;
            U32           mSerial = 0;
            bool          mFilled = false;
            bool          mInstalls = false;    // banner_open downloads and installs (plus/update): fillOpenButton()
            LLView*       mBox   = nullptr;     // children of this band: they live as long as it does
            LLUICtrl*     mText  = nullptr;
            psui::PsButton* mOpen = nullptr;
            LLView*       mSkip  = nullptr;
            LLView*       mStop  = nullptr;
            LLView*       mClose = nullptr;
            std::string   mOpenLabel;           // banner_open as the XUI has it (the download page)
            std::string   mOpenTip;
            S32           mEdge = 0, mGap = 0, mTextGap = 0;
            LLHandle<LLView> mVersionText;      // panel_login's version_text, when there is one
            bool          mVersionLooked = false;
        };

        LLHandle<UpdateBanner> sBanner;

        // The band is up (made, and not closed with a button this frame).
        bool bannerShown()
        {
            const UpdateBanner* banner = sBanner.get();
            return banner && banner->getVisible();
        }

        void removeBanner()
        {
            if (UpdateBanner* banner = sBanner.get())
            {
                if (LLView* parent = banner->getParent())
                {
                    parent->removeChild(banner);
                }
                delete banner;
            }
            sBanner = LLHandle<UpdateBanner>();
        }

        // The band for sOffer: made the first time, placed again when the window changed, filled again
        // only when the offer changed (every frame at the login screen: nothing is allocated otherwise).
        void showBanner()
        {
            LLView* holder = gViewerWindow ? gViewerWindow->getLoginPanelHolder() : nullptr;
            if (!holder)
            {
                return;
            }
            UpdateBanner* banner = sBanner.get();
            if (!banner)
            {
                if (sBannerFailed)
                {
                    return;
                }
                banner = UpdateBanner::create(holder);
                if (!banner)
                {
                    LL_WARNS("UpdateCheck") << BANNER_FILE << " could not be built; no band in this run" << LL_ENDL;
                    sBannerFailed = true;
                    return;
                }
                sBanner = banner->getDerivedHandle<UpdateBanner>();
            }
            banner->place(holder);
            if (banner->holds(sOfferSerial))
            {
                banner->followInstall();
                return;
            }
            const std::string shown = key(sOffer->version);
            if (shown != sShownKey)
            {
                LL_INFOS("UpdateCheck") << "Shown at the login screen: " << displayText(sOffer->version) << LL_ENDL;
                sShownKey = shown;
            }
            banner->show(*sOffer, sOfferSerial);
        }

        // ---- Showing it ----------------------------------------------------

        // Every frame while there is an offer or a band: the band while the login screen waits for the
        // user, the window once the world is up when the login had started before the offer came.
        bool tick()
        {
            if (LLApp::isExiting())
            {
                sOffer.reset();         // the band goes with the views
                sTickRunning = false;
                return true;
            }
            static LLCachedControl<bool> enabled(gSavedSettings, SETTING_ENABLED, true);
            if (!enabled)
            {
                if (sOffer || sBanner.get())
                {
                    LL_INFOS("UpdateCheck") << "Turned off (" << SETTING_ENABLED << "); not shown" << LL_ENDL;
                }
                sOffer.reset();
                removeBanner();
                sTickRunning = false;
                return true;
            }

            if (plus::update::engaged())
            {
                // An update the user agreed to is on its way, ready or failed: its chip tells of it, not this.
                sOffer.reset();
                removeBanner();
                sTickRunning = false;
                return true;
            }

            const EStartupState state = LLStartUp::getStartupState();
            if (state == STATE_LOGIN_WAIT)
            {
                if (sOffer)
                {
                    showBanner();
                    return false;
                }
                removeBanner();         // closed with a button, or nothing newer after all
                sTickRunning = false;
                return true;
            }
            if (sBanner.get())
            {
                // The login has started with the band up: it has been seen, the window does not repeat it.
                removeBanner();
                sOffer.reset();
            }
            if (!sOffer)
            {
                sTickRunning = false;
                return true;
            }
            if (state != STATE_STARTED)
            {
                return false;
            }

            LLFloaterReg::showInstance(FLOATER_NAME);   // the window copies sOffer in onOpen()
            sShownKey = key(sOffer->version);
            sOffer.reset();
            sTickRunning = false;
            return true;
        }

        void setOffer(Offer offer)
        {
            sOffer = std::move(offer);
            ++sOfferSerial;
            if (!sTickRunning)
            {
                sTickRunning = true;
                doOnIdleRepeating(&tick);
            }
        }

        // An update file, from the site or the copy of the last answer, judged against the running version.
        void consider(const OfferFile& file, Origin origin)
        {
            const Offer found = offerFor(file);
            const Version current = runningVersion();
            const std::string newest = displayText(found.version);
            const bool answer = origin == Origin::Answer;

            if (compare(found.version, current) <= 0)
            {
                LL_INFOS("UpdateCheck") << (answer ? "Up to date" : "The saved offer is caught up with") << ": running "
                                        << displayText(current) << ", newest " << newest << LL_ENDL;
                gSavedSettings.setString(SETTING_OFFER, std::string());
                if (answer)
                {
                    markChecked();
                }
                sOffer.reset();         // a band up for an older copy goes the next frame
                return;
            }

            if (answer)
            {
                const std::string copy = writeOfferFile(file);
                if (copy.size() <= MAX_OFFER_BYTES)
                {
                    gSavedSettings.setString(SETTING_OFFER, copy);
                }
                else
                {
                    LL_WARNS("UpdateCheck") << "The offer is " << copy.size() << " bytes written out; not kept" << LL_ENDL;
                    gSavedSettings.setString(SETTING_OFFER, std::string());
                }
                markChecked();
            }

            if (!found.required && gSavedSettings.getString(SETTING_SKIPPED) == key(found.version))
            {
                LL_INFOS("UpdateCheck") << "Newer version " << newest << " is one the user asked not to hear about" << LL_ENDL;
                sOffer.reset();
                return;
            }
            if (plus::update::engaged())
            {
                LL_INFOS("UpdateCheck") << "Newer version " << newest << ": an update is on its way, ready or failed already; not shown" << LL_ENDL;
                sOffer.reset();
                return;
            }
            if (plus::update::installsWithoutAsking() && key(found.version) != sAutoKey)
            {
                // PlusUpdateInstall 2: the user chose to have it downloaded without being asked. Once a run for
                // a version: should it fail, the chip offers Retry, and the next start tries again.
                sAutoKey = key(found.version);
                LL_INFOS("UpdateCheck") << "Newer version " << newest << (answer ? "" : " (saved)")
                                        << "; downloaded without asking (PlusUpdateInstall 2)" << LL_ENDL;
                if (plus::update::begin(displayText(found.version), packageVersion(found.version)))
                {
                    sOffer.reset();
                    return;
                }
            }
            if (key(found.version) == sShownKey && !bannerShown())
            {
                LL_INFOS("UpdateCheck") << "Newer version " << newest << " was shown in this run already" << LL_ENDL;
                return;
            }

            LL_INFOS("UpdateCheck") << "Newer version " << newest << (found.required ? " (running version below min_version)" : "")
                                    << (answer ? "" : " (saved)") << "; shown at the login screen or once the world is up" << LL_ENDL;
            setOffer(found);
        }

        // The copy of the last newer answer, read again as an answer (it came from the server).
        void considerSaved(const Source& source)
        {
            const std::string saved = gSavedSettings.getString(SETTING_OFFER);
            if (saved.empty())
            {
                return;
            }
            OfferFile file;
            std::string why;
            if (!readOffer(saved, source.allowed_hosts, file, why))
            {
                LL_WARNS("UpdateCheck") << "The saved offer (" << SETTING_OFFER << ") is not an update file (" << why << "); cleared" << LL_ENDL;
                gSavedSettings.setString(SETTING_OFFER, std::string());
                return;
            }
            consider(file, Origin::Saved);
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
            options->setMaxReplyBytes(MAX_OFFER_BYTES);

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
            if (!gSavedSettings.getBOOL(SETTING_ENABLED))
            {
                LL_INFOS("UpdateCheck") << "Turned off while reading (" << SETTING_ENABLED << "); the answer is not used" << LL_ENDL;
                return;
            }

            const LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
            const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
            if (!status)
            {
                // The site answered "no" (404 before noriaji/charis-viewer is public, 403, 410 ...): that is an
                // answer, so the next check is a day later, not an hour. Not 429 (too many requests) and not
                // 5xx (the site in trouble): those, like no answer at all, are tried again an hour later.
                const int code = status.isHttpStatus() ? static_cast<int>(status.getType()) : 0;
                if (code >= 400 && code < 500 && code != 429)
                {
                    LL_INFOS("UpdateCheck") << "The site answered HTTP " << code << "; nothing shown, checked again a day from now" << LL_ENDL;
                    markChecked();
                    return;
                }
                if (status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_WRITE_ERROR)
                    || status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_FILESIZE_EXCEEDED))
                {
                    LL_WARNS("UpdateCheck") << "The answer is more than " << MAX_OFFER_BYTES << " bytes; ignored" << LL_ENDL;
                    return;
                }
                LL_INFOS("UpdateCheck") << "No answer (" << status.toString() << "); nothing shown, tried again at a start an hour or more from now" << LL_ENDL;
                return;
            }

            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            if (raw.size() > MAX_OFFER_BYTES)
            {
                LL_WARNS("UpdateCheck") << "The answer is " << raw.size() << " bytes, more than " << MAX_OFFER_BYTES << "; ignored" << LL_ENDL;
                return;
            }

            OfferFile file;
            std::string why;
            const std::string_view body(reinterpret_cast<const char*>(raw.data()), raw.size());
            if (!readOffer(body, allowed_hosts, file, why))
            {
                LL_WARNS("UpdateCheck") << "The answer is not an update file (" << why << "); ignored" << LL_ENDL;
                return;
            }
            consider(file, Origin::Answer);
        }

        // Once a day at most, and an hour after a try without an answer.
        void startCheck(const Source& source)
        {
            const uint32_t now = nowSeconds();
            if (!dueForCheck(now, gSavedSettings.getU32(SETTING_LAST), CHECK_INTERVAL_SECONDS))
            {
                LL_INFOS("UpdateCheck") << "Checked less than a day ago; not now" << LL_ENDL;
                return;
            }
            if (!dueForCheck(now, gSavedSettings.getU32(SETTING_LAST_TRY), RETRY_INTERVAL_SECONDS))
            {
                LL_INFOS("UpdateCheck") << "Tried less than an hour ago without an answer; not now" << LL_ENDL;
                return;
            }

            // The try is on disk before anything is read: should reading the
            // answer end the viewer, the next start does not read it again at
            // once (research/130 M2).
            gSavedSettings.setU32(SETTING_LAST_TRY, now);
            saveSettingsNow();

            LLCoros::instance().launch("plus::updatecheck",
                                       [url = source.url, hosts = source.allowed_hosts]() { checkCoro(url, hosts); });
        }

        // ---- The window ----------------------------------------------------

        class UpdateFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                LLButton* page_button = getChild<LLButton>("btn_download");
                page_button->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    // As the button says (onOpen()): nothing is downloaded from a button that read "Open download page"
                    if (mInstalls)
                    {
                        installOrOpen(mOffer);
                    }
                    else
                    {
                        openPage(mOffer);
                    }
                    closeFloater();
                });
                mDownloadLabel = page_button->getLabelUnselected();
                mDownloadTip   = page_button->getToolTip();
                getChild<LLButton>("btn_later")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
                getChild<LLButton>("btn_skip")->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    skipVersion(mOffer);
                    saveSettingsNow();
                    closeFloater();
                });
                return true;
            }

            void onOpen(const LLSD&) override
            {
                if (sOffer)
                {
                    mOffer = *sOffer;
                }
                getChild<LLUICtrl>("message")->setValue(bodyText(*this, mOffer));

                // fix-round68 3: with only the notes named, the button says it opens them (the
                // text has no links: the button is the only way to a page).
                LLButton* page_button = getChild<LLButton>("btn_download");
                mInstalls = plus::update::canInstall();
                page_button->setEnabled(mInstalls || !pageURL(mOffer).empty());
                const bool notes_only = notesOnly(mOffer);
                if (mInstalls)
                {
                    page_button->setLabel(getString("btn_install"));
                    page_button->setToolTip(getString("btn_install_tip"));
                }
                else
                {
                    page_button->setLabel(notes_only ? getString("btn_notes") : mDownloadLabel);
                    page_button->setToolTip(notes_only ? getString("btn_notes_tip") : mDownloadTip);
                }
                getChild<LLButton>("btn_skip")->setVisible(!mOffer.required);
            }

        private:
            explicit UpdateFloater(const LLSD& key) : LLFloater(key) {}

            Offer mOffer;
            bool mInstalls = false;         // btn_download says "Download" and installs (plus/update): set in onOpen()
            std::string mDownloadLabel;     // btn_download as the XUI has it (the download page)
            std::string mDownloadTip;
        };

        // ---- The on/off box in Preferences > Setup -------------------------
        // fix-round103 (2026-10-24): shown only once the update site is named (PlusUpdateCheckURL not
        // empty; research/123). While it is hidden, what is below it in the panel moves up into its
        // place, so no empty row is left. The box stays (hidden) and keeps PlusUpdateCheck; nothing
        // about the check itself changes.
        class UpdateCheckPanel : public LLPanel
        {
        public:
            bool postBuild() override
            {
                mBox  = getChild<LLView>("plus_update_check_enabled");
                mMode = getChild<LLView>("plus_update_install");
                // How far the rows below move up when only the second row is hidden: its bottom to the box's.
                mModeGap = mBox->getRect().mBottom - mMode->getRect().mBottom;
                if (LLControlVariable* url = gSavedSettings.getControl(SETTING_URL).get())
                {
                    mURLChanged = url->getCommitSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { applyLayout(); });
                }
                mBox->setVisible(siteNamed());  // the rest waits for the panels below to be built
                mMode->setVisible(siteNamed() && plus::update::hasInstaller());
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

            // Only what changed is touched: the rows' visibility, and the views below by the difference.
            // Without an update site both rows go (the views below move up by mGap); without an installer
            // (the zip) only the second (by mModeGap).
            void applyLayout()
            {
                const bool shown = siteNamed();
                const bool mode_shown = shown && plus::update::hasInstaller();
                if (mBox && mBox->getVisible() != shown)
                {
                    mBox->setVisible(shown);
                }
                if (mMode && mMode->getVisible() != mode_shown)
                {
                    mMode->setVisible(mode_shown);
                }
                if (!getParent())
                {
                    return;
                }
                if (!mBelowFound)
                {
                    findBelow();
                }
                const S32 shift = !shown ? mGap : (mode_shown ? 0 : mModeGap);
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
            LLView*                               mMode = nullptr;      // PlusUpdateInstall's drop-down
            S32                                   mModeGap = 0;         // how far the views below move up while only it is hidden
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

        // The login screen is being shown: the copy of the last answer first (shown at once), then the
        // check when it is due. Off: nothing is read, checked or shown.
        void onUIReady()
        {
            if (!gSavedSettings.getBOOL(SETTING_ENABLED))
            {
                LL_INFOS("UpdateCheck") << "Off (" << SETTING_ENABLED << ")" << LL_ENDL;
                return;
            }
            const std::optional<Source> source = updateSource();
            if (!source)
            {
                return;
            }
            considerSaved(*source);
            startCheck(*source);
        }

        plus::FeatureRegistration sRegistration({ "updatecheck", &init, &onUIReady });
    }
}
