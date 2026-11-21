/**
 * @file subtitlerml.cpp
 * @brief The subtitles window made with RmlUi (research/386): the content is app_settings/plus/rmlui/subtitle.rml.
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

// What the window does is SubtitleCore (subtitlecore.cpp), the same as the XUI window's (subtitlefloater.cpp, frozen).
// This file is the view: the core says what to show (SubtitleView), and it is kept here and put into the document in
// beforeUpdate() (RmlUi's own turn, never inside an event or from the core's tick). The settings of the window are a
// SettingsModel (the XUI control_name). The words and the three setting changes that matter to the core (the way of
// translating, its language, on/off) are passed to the core on the next frame, after the model has written them.

#include "llviewerprecompiledheaders.h"

#include "subtitlerml.h"
#include "subtitlecore.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlparts.h"
#include "plus/rmlui/rmlsettings.h"

#include <cmath>

#include "llfloaterreg.h"
#include "llviewercontrol.h"

namespace plus::subtitle
{
    namespace
    {
        using plus::rmlui::SettingsModel;

        const char* const SET_THREADS = "PlusSubtitleThreads";
        constexpr S32 THREADS_MIN = 1;
        constexpr S32 THREADS_MAX = 16;
        constexpr S32 LOG_MAX_BYTES = 65536;     // the XUI text editor's max_length

        class SubtitleRmlFloater : public plus::rmlui::RmlFloater, public SubtitleView
        {
            friend class ::LLFloaterReg;

        public:
            ~SubtitleRmlFloater() override
            {
                closeDocument();    // before the members the document points at (the model, the log) go
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                mCore.open();
                rememberTranslateSettings();
            }

            void onClose(bool app_quitting) override
            {
                mCore.close();
                RmlFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mSettingsChanged)
                {
                    mSettingsChanged = false;
                    // A change event is not always the user's choice (a select that took an option at load says so too):
                    // the core is told only when what it reads changed since it was last told (it resets its notices).
                    if (rememberTranslateSettings())
                    {
                        mCore.onTranslateServiceChanged();     // after the model wrote the setting
                    }
                }
                mCore.tick();
                RmlFloater::draw();
            }

            // ---- SubtitleView: kept here, put into the document in beforeUpdate() ----
            std::string str(const std::string& name) override { return getString(name); }
            void setStatus(const std::string& text) override
            {
                if (text != mStatus)
                {
                    mStatus = text;
                    mUiChanged = true;
                    wake();
                }
            }
            void setLog(const std::string& text) override
            {
                if (text != mLogText)
                {
                    mLogText = text;
                    mLogChanged = true;
                    mUiChanged = true;
                    wake();
                }
            }
            S32 logMaxBytes() const override { return LOG_MAX_BYTES; }
            void setDownloadButton(bool visible, const std::string& label) override
            {
                if (!label.empty())
                {
                    mDownloadLabel = label;
                }
                mDownloadVisible = visible;
                mUiChanged = true;
                wake();
            }
            void setFetchButton(bool visible, const std::string& label) override
            {
                mFetchVisible = visible;
                mFetchLabel = label;
                mUiChanged = true;
                wake();
            }
            void setFetchCredits(const std::string& credits) override
            {
                mFetchCredits = credits;
                mUiChanged = true;
                wake();
            }

        protected:
            void beforeLoad(Rml::Context& context) override
            {
                Rml::DataModelConstructor model = mModel.create(context, "subtitle");
                if (!model)
                {
                    return;
                }
                using Kind = SettingsModel::Kind;
                LLControlGroup* group = &gSavedSettings;
                const std::vector<SettingsModel::Entry> entries = {
                    { "translate", group, "PlusSubtitleTranslate",        Kind::Bool,   0 },
                    { "radio",     group, "PlusSubtitleStream",           Kind::Bool,   0 },
                    { "service",   group, "PlusSubtitleTranslateService", Kind::Choice, 0 },
                    { "to",        group, "PlusSubtitleTranslateTo",      Kind::Choice, 0 },
                    { "dialect",   group, "PlusTranslateDialect",         Kind::Choice, 0 },
                    { "language",  group, "PlusSubtitleLanguage",         Kind::Choice, 0 },
                    { "threads",   group, SET_THREADS,                    Kind::Int,    0, THREADS_MIN, THREADS_MAX },
                };
                mModel.bind(model, entries);
            }

            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mLogChanged = true;
                // A setting that is none of a select's options (a slip in the settings file) shows an empty first option
                // that stands for it, as the XUI combo showed nothing; the setting is not written (SettingsModel's quiet).
                addUnknownValueOption(document, "service", "PlusSubtitleTranslateService");
                addUnknownValueOption(document, "to", "PlusSubtitleTranslateTo");
                addUnknownValueOption(document, "dialect", "PlusTranslateDialect");
                addUnknownValueOption(document, "language", "PlusSubtitleLanguage");
                for (const char* id : { "download_model", "download_translate_model" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                for (const char* id : { "service", "to" })
                {
                    view->listen(document.GetElementById(id), "change");
                }
                view->listen(document.GetElementById("c_translate"), "change");
                view->listen(document.GetElementById("threads_spin"), "commit");   // the host's number box (rmlparts.h 5.)
                view->listen(document.GetElementById("threads"), "focus");         // the model keeps a change from outside back while the
                view->listenCapture(document.GetElementById("threads"), "blur");   // field is held (after the host's own commit on blur)
                // The XUI window's own widths of the Threads label and box (ja is wider): from the window's strings.
                setWidth(document.GetElementById("spin_label"), getString("threads_label_w"));
                setWidth(document.GetElementById("spin"), getString("threads_w"));
                mUiChanged = true;
            }

            void beforeUpdate() override
            {
                mModel.beforeUpdate();
                if (!mUiChanged)
                {
                    return;
                }
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                mUiChanged = false;
                setText(document->GetElementById("status"), mStatus);
                if (mLogChanged)
                {
                    mLogChanged = false;
                    // The core keeps the log to logMaxBytes() (its old lines go first); the newest line is at the end.
                    plus::rmlui::setLogText(*view, document->GetElementById("log"), mLogText, plus::rmlui::Follow::End);
                }
                if (Rml::Element* button = document->GetElementById("download_model"))
                {
                    setText(button, mDownloadLabel);
                    button->SetClass("invis", !mDownloadVisible);
                }
                if (Rml::Element* button = document->GetElementById("download_translate_model"))
                {
                    setText(button, mFetchLabel);
                    button->SetClass("invis", !mFetchVisible);
                    const std::string tip = getString("tip_fetch");
                    button->SetAttribute("title", Rml::String(mFetchCredits.empty() ? tip : tip + "\n\n" + mFetchCredits));
                }
            }

            void afterUpdate() override
            {
                mModel.afterUpdate();       // the selects have made their first choice: from here on a change is the user's
            }

            // In RmlUi's event: no element is removed here.
            void onEvent(Rml::Event& event) override
            {
                const Rml::Element* element = event.GetCurrentElement();
                if (!element)
                {
                    return;
                }
                const std::string id = element->GetId();
                const Rml::EventId type = event.GetId();
                if (type == Rml::EventId::Click)
                {
                    if (id == "download_model")
                    {
                        mCore.onDownloadButton();
                    }
                    else if (id == "download_translate_model")
                    {
                        mCore.onFetchButton();
                    }
                }
                else if (id == "threads" && (type == Rml::EventId::Focus || type == Rml::EventId::Blur) &&
                         event.GetTargetElement() == element)
                {
                    // The field's focus and blur: the number shown stays what the user sees while the field is held; at the
                    // blur (after the host committed what was typed) a change from outside meanwhile shows now.
                    if (type == Rml::EventId::Focus)
                    {
                        mModel.beginEdit("threads");
                    }
                    else
                    {
                        mModel.endEdit("threads", std::string());
                    }
                }
                else if (event.GetType() == "commit" && id == "threads_spin")
                {
                    // The number box committed (Enter, the focus left after typing, an arrow, the wheel): the model writes the
                    // setting (clamped to its range) and shows it in the box.
                    const S32 now = gSavedSettings.getS32(SET_THREADS);
                    mModel.commitInt("threads", (S32)std::lround(event.GetParameter<double>("value", (double)now)));
                }
                else if (type == Rml::EventId::Change && (id == "service" || id == "to" || id == "c_translate"))
                {
                    mSettingsChanged = true;     // draw() tells the core (the model has written it by then)
                }
            }

            void unbind() override
            {
                mModel.unbind();
            }

        private:
            explicit SubtitleRmlFloater(const LLSD& key)
            :   RmlFloater(key),
                mModel([this]() { wake(); }),
                mCore(*this)
            {
            }

            void wake()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            // The select's options hold the setting's value, or an empty option with that value is put first.
            static void addUnknownValueOption(Rml::ElementDocument& document, const char* id, const char* setting)
            {
                auto* box = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document.GetElementById(id));
                if (!box)
                {
                    return;
                }
                const std::string value = gSavedSettings.getString(setting);
                for (int i = 0; i < box->GetNumOptions(); ++i)
                {
                    if (box->GetOption(i)->GetAttribute<Rml::String>("value", "") == value)
                    {
                        return;
                    }
                }
                box->Add("", value, 0);
            }

            // What the core reads of the three settings the window can change; true when they differ from what it was last told.
            bool rememberTranslateSettings()
            {
                const bool translate = gSavedSettings.getBOOL("PlusSubtitleTranslate");
                const std::string service = gSavedSettings.getString("PlusSubtitleTranslateService");
                const std::string to = gSavedSettings.getString("PlusSubtitleTranslateTo");
                const bool differ = translate != mSeenTranslate || service != mSeenService || to != mSeenTo;
                mSeenTranslate = translate;
                mSeenService = service;
                mSeenTo = to;
                return differ;
            }

            static void setText(Rml::Element* element, const std::string& text)
            {
                if (element && element->GetInnerRML() != plus::rmlui::escapeRml(text))
                {
                    element->SetInnerRML(plus::rmlui::escapeRml(text));
                }
            }

            static void setWidth(Rml::Element* element, const std::string& dp)
            {
                if (element && !dp.empty())
                {
                    element->SetProperty("width", dp + "dp");
                }
            }

            SettingsModel mModel;
            std::string mStatus;
            std::string mLogText;
            std::string mDownloadLabel;
            std::string mFetchLabel;
            std::string mFetchCredits;
            bool mDownloadVisible = false;
            bool mFetchVisible = false;
            bool mLogChanged = false;           // the log text is new: it goes into the area (and scrolls to its end)
            bool mUiChanged = true;             // something above is not in the document yet
            bool mSettingsChanged = false;
            bool mSeenTranslate = false;        // the translate settings as the core was last told them
            std::string mSeenService;
            std::string mSeenTo;
            SubtitleCore mCore;                 // last: it stops its helpers first when the window goes
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_subtitle_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<SubtitleRmlFloater>);
    }
}
