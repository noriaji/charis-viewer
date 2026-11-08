/**
 * @file translatesettings.cpp
 * @brief Translation (plus/translate, research/228): the "this PC" part of the chat translation settings window.
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

#include "translatesettings.h"
#include "localtranslate.h"

#include "plus/subtitle/subtitledialect.h"
#include "plus/subtitle/subtitletranslate.h"
#include "plus/subtitle/translatefetch.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llpanel.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewercontrol.h"

#include <vector>

namespace plus
{
    namespace translate
    {
        namespace
        {
            // Where the local panel goes: under the radio item of "This PC", at the top of the group of the services.
            constexpr S32 PANEL_LEFT = 45;
            constexpr S32 PANEL_BELOW_GROUP_TOP = 20;   // the item's height 16 and a gap

            // The language the combo's value stands for: two letters. "default" and "" are the language of the viewer.
            std::string targetOf(const std::string& value)
            {
                std::string v = value;
                if (v.empty() || v == "default")
                {
                    v = LLUI::getLanguage();
                }
                return subtitle::primaryLanguage(v);
            }

            std::string megabytes(uint64_t bytes)
            {
                return llformat("%.0f", double(bytes) / 1.0e6);
            }
        }

        struct LocalSettings::Impl
        {
            LLFloater* floater = nullptr;
            std::function<void()> changed;
            subtitle::TranslateModelFetch& fetch = subtitle::sharedTranslateFetch();   // the one fetch of the viewer (the subtitle window uses it too)

            LLPanel* panel = nullptr;
            LLTextBox* status = nullptr;
            LLButton* fetchButton = nullptr;
            LLTextBox* koreanStatus = nullptr;
            LLButton* koreanButton = nullptr;
            LLTextBox* problem = nullptr;
            LLComboBox* dialect = nullptr;

            std::string service;
            std::string language;
            bool on = true;                 // translation is switched on in the window
            bool active = false;            // "This PC" is chosen and translation is on: the block is usable
            subtitle::TranslateModelFetch::State lastState = subtitle::TranslateModelFetch::State::Idle;   // what draw() saw last
            std::string failure;            // why the last fetch failed, for the window
            LLFrameTimer progressTimer;

            std::string text(const char* name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t()) const
            {
                return floater->getString(name, args);
            }

            bool busy() const
            {
                const subtitle::TranslateModelFetch::State st = fetch.state();
                return st == subtitle::TranslateModelFetch::State::Running || st == subtitle::TranslateModelFetch::State::Unpacking;
            }

            // The block of "This PC" is always there, like the services' blocks; its parts are usable when it is chosen.
            void showEnabled()
            {
                active = on && service == "local";
                status->setEnabled(active);
                koreanStatus->setEnabled(active);
                problem->setEnabled(active);
                dialect->setEnabled(on);   // the dialect belongs to every service
            }

            // The texts and buttons of the panel, for what is installed and what is being fetched.
            void showStatus()
            {
                const std::string target = targetOf(language);
                const bool fetching = busy();
                const bool unsupported = subtitle::translatePairsFor(target).empty();

                LLStringUtil::format_map_t args;
                args["[LANG]"] = target;
                std::string line;
                // the fetch is the Korean model's, for the Korean row (when Korean is the language chosen, the row above is that model)
                const bool koreanFetch = fetching && target != "ko" && fetch.currentArchive() == "m2m";
                if (fetching && !koreanFetch)
                {
                    args["[PERCENT]"] = llformat("%.0f", fetch.fraction() * 100.0);
                    line = text(fetch.state() == subtitle::TranslateModelFetch::State::Unpacking ? "local_unpacking" : "local_fetching", args);
                }
                else if (unsupported)
                {
                    line = text("local_unsupported", args);
                }
                else if (subtitle::translateModelsInstalledFor(target))
                {
                    line = text("local_ready", args);
                }
                else
                {
                    args["[SIZE]"] = megabytes(subtitle::translateModelBytesMissing(target, false));
                    line = text("local_missing", args);
                }
                if (status->getText() != line)
                {
                    status->setText(line);
                }

                const bool fetchable = !unsupported && !subtitle::translateModelsInstalledFor(target);
                setButton(fetchButton, fetching && !koreanFetch, fetchable && !(fetching && koreanFetch));

                // Korean: its own row, unless Korean is the language being translated into (then the row above is the Korean model)
                const bool korean_row = (target != "ko");
                koreanStatus->setVisible(korean_row);
                koreanButton->setVisible(korean_row);
                if (korean_row)
                {
                    std::string kline;
                    LLStringUtil::format_map_t kargs;
                    if (fetching && koreanFetch)
                    {
                        kargs["[PERCENT]"] = llformat("%.0f", fetch.fraction() * 100.0);
                        kline = text(fetch.state() == subtitle::TranslateModelFetch::State::Unpacking ? "local_unpacking" : "local_fetching", kargs);
                    }
                    else if (subtitle::translateKoreanInstalled())
                    {
                        kline = text("local_korean_ready");
                    }
                    else
                    {
                        kargs["[SIZE]"] = megabytes(subtitle::translateModelBytesMissing("ko", false));
                        kline = text("local_korean_missing", kargs);
                    }
                    if (koreanStatus->getText() != kline)
                    {
                        koreanStatus->setText(kline);
                    }
                    setButton(koreanButton, fetching && koreanFetch, !subtitle::translateKoreanInstalled() && !(fetching && !koreanFetch));
                }

                std::string note = failure;
                if (note.empty())
                {
                    note = localProblem();
                }
                if (problem->getText() != note)
                {
                    problem->setText(note);
                }
            }

            // A button reads "Get the models" or, while its fetch runs, "Cancel"; it is enabled when there is something it can do.
            void setButton(LLButton* button, bool cancelling, bool enabled)
            {
                const std::string label = text(cancelling ? "local_cancel_label" : "local_fetch_label");
                if (button->getLabelSelected() != label)
                {
                    button->setLabel(label);
                }
                button->setEnabled(cancelling || (active && enabled));   // a fetch can always be cancelled
            }

            // The button is the user's go-ahead for the download; pressed again while it runs, it cancels.
            void onFetch(bool korean)
            {
                if (busy())
                {
                    fetch.cancel();
                    lastState = fetch.state();
                    showStatus();
                    return;
                }
                failure.clear();
                const std::string base = subtitle::translateModelBaseUrl();
                std::string error;
                // the Korean row asks for the Korean model as the target ("ko"): that is the one archive
                const std::string target = korean ? std::string("ko") : targetOf(language);
                if (!fetch.start(base, target, error))
                {
                    LL_WARNS("Translate") << error << LL_ENDL;
                    failure = text(base.empty() ? "local_unpublished" : "local_cannot_start");
                    showStatus();
                    return;
                }
                LL_INFOS("Translate") << "fetching the translation models: " << subtitle::translateModelCredits(target) << LL_ENDL;
                lastState = fetch.state();
                progressTimer.reset();
                showStatus();
            }

            // Every frame the window is drawn: shows how a fetch goes (it is followed by the fetch's own timer, not by this window, so
            // it goes on while the window is closed; the window only reads the state).
            void follow()
            {
                const subtitle::TranslateModelFetch::State st = fetch.state();
                if (busy() && progressTimer.getElapsedTimeF32() >= 0.5f)
                {
                    progressTimer.reset();
                    showStatus();
                }
                if (st == lastState)
                {
                    return;
                }
                const subtitle::TranslateModelFetch::State before = lastState;
                lastState = st;
                if (st == subtitle::TranslateModelFetch::State::Failed && before != st)
                {
                    LL_WARNS("Translate") << fetch.failure() << LL_ENDL;
                    failure = text("local_failed");
                }
                showStatus();
                if (st == subtitle::TranslateModelFetch::State::Done)
                {
                    changed();   // the models are here: the window may enable OK
                }
            }
        };

        LocalSettings::LocalSettings(LLFloater* floater, std::function<void()> changed) : mImpl(new Impl)
        {
            mImpl->floater = floater;
            mImpl->changed = std::move(changed);
        }

        LocalSettings::~LocalSettings() {}

        void LocalSettings::postBuild()
        {
            Impl& m = *mImpl;
            m.panel = m.floater->getChild<LLPanel>("local_panel");
            m.status = m.panel->getChild<LLTextBox>("local_status");
            m.fetchButton = m.panel->getChild<LLButton>("local_fetch");
            m.koreanStatus = m.panel->getChild<LLTextBox>("local_korean_status");
            m.koreanButton = m.panel->getChild<LLButton>("local_korean_fetch");
            m.problem = m.panel->getChild<LLTextBox>("local_problem");
            m.dialect = m.floater->getChild<LLComboBox>("translate_dialect_combo");
            m.fetchButton->setClickedCallback([this](LLUICtrl*, const LLSD&) { mImpl->onFetch(false); });
            m.koreanButton->setClickedCallback([this](LLUICtrl*, const LLSD&) { mImpl->onFetch(true); });

            // the block goes under the item of "This PC", the first of the group (the XUI leaves the room for it)
            if (LLView* group = m.floater->findChildView("translation_service_rg"))
            {
                LLRect r = m.panel->getRect();
                r.setLeftTopAndSize(PANEL_LEFT, group->getRect().mTop - PANEL_BELOW_GROUP_TOP, r.getWidth(), r.getHeight());
                m.panel->setRect(r);
            }
        }

        void LocalSettings::onOpen()
        {
            Impl& m = *mImpl;
            m.dialect->setSelectedByValue(gSavedSettings.getString("PlusTranslateDialect"), true);
            // a fetch may have been started before (in this window, or in the subtitle window) and may have ended while it was closed
            m.lastState = m.fetch.state();
            m.failure = (m.lastState == subtitle::TranslateModelFetch::State::Failed) ? m.text("local_failed") : std::string();
        }

        void LocalSettings::refresh(const std::string& service, const std::string& language, bool on)
        {
            Impl& m = *mImpl;
            m.service = service;
            m.language = language;
            m.on = on;
            m.showEnabled();
            m.showStatus();
        }

        void LocalSettings::draw()
        {
            mImpl->follow();
        }

        void LocalSettings::apply()
        {
            const std::string id = mImpl->dialect->getSelectedValue().asString();
            gSavedSettings.setString("PlusTranslateDialect", subtitle::knownDialect(id) ? id : std::string());
            // what the window saves (OK): the log tells later whether the chat translation was switched on with it
            logState("settings saved", mImpl->on, mImpl->service, mImpl->language, targetOf(mImpl->language));
        }

        bool LocalSettings::ready(const std::string& language) const
        {
            return localReady(targetOf(language));   // some model that translates into it is here (or the Korean one alone)
        }
    }
}
