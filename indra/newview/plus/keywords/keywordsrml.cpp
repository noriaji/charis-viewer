/**
 * @file keywordsrml.cpp
 * @brief Chat keyword alerts: the settings window made with RmlUi (research/380).
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

// The same window as keywordsfloater.cpp's (the XUI one, frozen: used in safe mode and with PlusRmlUiWindows off),
// with its content app_settings/plus/rmlui/keywords.rml. The boxes and fields are the document's, bound to the
// PlusKeywords* settings through a plus::rmlui::SettingsModel (what control_name was); this class is the data
// model's entries, the try-out field, the sound's two buttons and the colour square's picker (plus::rmlui::ColourLink).

#include "llviewerprecompiledheaders.h"

#include "keywords.h"
#include "keywordsrml.h"

#include "plus/rmlui/rmlcolour.h"
#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmlsettings.h"

#include "llfloaterreg.h"
#include "llui.h"
#include "llviewercontrol.h"

namespace plus::keywords
{
    namespace
    {
        using plus::rmlui::SettingsModel;

        const char* const SET_ON    = "PlusKeywordsOn";
        const char* const SET_SOUND = "PlusKeywordsSound";
        const char* const SET_COLOR = "PlusKeywordsColor";

        // keywords.rml's text fields: the ids are the data model's variable names, and the maxlength there
        constexpr S32 LIST_MAX_CHARS = 2048;
        constexpr S32 SOUND_MAX_CHARS = 64;
        // Where the colour square is in the window (the XUI window's color_swatch: left 170, top 248 under the window's
        // top, 30 x 16; keywords.rml's change_color row puts the square there), for the picker's cone.
        constexpr S32 SWATCH_LEFT = 170;
        constexpr S32 SWATCH_TOP = 248;
        constexpr S32 SWATCH_WIDTH = 30;
        constexpr S32 SWATCH_HEIGHT = 16;

        bool isCommittedField(const std::string& id)
        {
            return id == "list" || id == "sound";
        }

        class KeywordsRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~KeywordsRmlFloater() override
            {
                closeDocument();    // before the members the document points at (the model, the colour link) go
                mColour.shutdown();
            }

            bool postBuild() override
            {
                const bool built = RmlFloater::postBuild();
                const S32 top = getRect().getHeight() - SWATCH_TOP;
                mColour.create(this, host(), gSavedPerAccountSettings, SET_COLOR, SET_ON,
                               LLRect(SWATCH_LEFT, top, SWATCH_LEFT + SWATCH_WIDTH, top - SWATCH_HEIGHT));
                return built;
            }

            void draw() override
            {
                updateResult();
                RmlFloater::draw();
            }

        protected:
            void beforeLoad(Rml::Context& context) override
            {
                Rml::DataModelConstructor model = mModel.create(context, "keywords");
                if (!model)
                {
                    return;
                }
                using Kind = SettingsModel::Kind;
                LLControlGroup* group = &gSavedPerAccountSettings;
                const std::vector<SettingsModel::Entry> entries = {
                    { "on",           group, SET_ON,                       Kind::Bool,   0 },
                    { "in_chat",      group, "PlusKeywordsInChat",         Kind::Bool,   0 },
                    { "in_im",        group, "PlusKeywordsInIM",           Kind::Bool,   0 },
                    { "own_name",     group, "PlusKeywordsOwnName",        Kind::Bool,   0 },
                    { "speaker",      group, "PlusKeywordsSpeakerName",    Kind::Bool,   0 },
                    { "match_case",   group, "PlusKeywordsCaseSensitive",  Kind::Bool,   0 },
                    { "whole",        group, "PlusKeywordsWholeWords",     Kind::Bool,   0 },
                    { "fold",         group, "PlusKeywordsFoldWidth",      Kind::Bool,   0 },
                    { "change_color", group, "PlusKeywordsChangeColor",    Kind::Bool,   0 },
                    { "play_sound",   group, "PlusKeywordsPlaySound",      Kind::Bool,   0 },
                    { "toast",        group, "PlusKeywordsToast",          Kind::Bool,   0 },
                    { "list",         group, "PlusKeywordsList",           Kind::String, LIST_MAX_CHARS },
                    { "sound",        group, SET_SOUND,                    Kind::String, SOUND_MAX_CHARS },
                    { "colour",       group, SET_COLOR,                    Kind::Colour, 0 },
                };
                mModel.bind(model, entries);
                model.Bind("try_result", &mShown);
            }

            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                for (const char* id : { "list", "sound", "try" })
                {
                    Rml::Element* field = document.GetElementById(id);
                    view->listen(field, "focus");
                    view->listenCapture(field, "blur");     // after RmlUi's own Blur ended an IME composition
                    view->listen(field, "change");
                    view->listen(field, "keydown");         // Esc: back to the value the field was given
                }
                for (const char* id : { "colour", "sound_preview", "sound_default" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                mJudgedGeneration = 0;      // the first frame judges (the hint)
            }

            void beforeUpdate() override
            {
                mModel.beforeUpdate();
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
                    if (id == "colour")
                    {
                        mColour.open();
                    }
                    else if (id == "sound_preview")
                    {
                        previewSound();
                    }
                    else if (id == "sound_default")
                    {
                        resetSound();
                    }
                    return;
                }
                auto* box = rmlui_dynamic_cast<Rml::ElementFormControl*>(event.GetCurrentElement());
                if (!box)
                {
                    return;
                }
                if (id == "try")
                {
                    if (type == Rml::EventId::Change)
                    {
                        mTryText = box->GetValue();     // judged in draw() when it or the list changed
                    }
                    return;
                }
                if (!isCommittedField(id))
                {
                    return;
                }
                // The list and the sound are committed as the XUI's line editor does: Enter, and the focus leaving
                // (the model writes only a text the user changed).
                if (type == Rml::EventId::Focus)
                {
                    mModel.beginEdit(id);
                }
                else if (type == Rml::EventId::Blur)
                {
                    mModel.endEdit(id, committedText(*box));
                }
                else if (type == Rml::EventId::Keydown)
                {
                    // as the XUI line editor's revert_on_esc: the text goes back without being committed (the key goes
                    // on: the window's Esc and the focus leaving stay as they were)
                    std::string text;
                    if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE && mModel.revertText(id, text) &&
                        box->GetValue() != text)
                    {
                        box->SetValue(text);
                    }
                }
                else if (type == Rml::EventId::Change && event.GetParameter<bool>("linebreak", false))
                {
                    mModel.commitText(id, box->GetValue());
                }
            }

            // The window closes with a text typed in a field: it counts (closeDocument blurs the focused field first,
            // whose listener commits it; this is for a focus that did not blur).
            void commitPending() override
            {
                LLRmlView* view = host();
                Rml::Context* context = view ? view->getContext() : nullptr;
                Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
                if (!focus || !isCommittedField(focus->GetId()))
                {
                    return;
                }
                if (auto* box = rmlui_dynamic_cast<Rml::ElementFormControl*>(focus))
                {
                    mModel.endEdit(focus->GetId(), committedText(*box));
                }
            }

            void unbind() override
            {
                mModel.unbind();
            }

        private:
            explicit KeywordsRmlFloater(const LLSD& key)
            :   RmlFloater(key),
                mModel([this]() {
                    if (LLRmlView* view = host())
                    {
                        view->markDirty();
                    }
                })
            {
            }

            // The list may be edited while text stands in the try-out field: the text is judged again when it
            // or the list changed (looked at every frame, a whole number and a string compare; the answer is
            // given to the document only when it changes).
            void updateResult()
            {
                const unsigned generation = listGeneration();
                if (generation == mJudgedGeneration && mTryText == mJudgedText)
                {
                    return;
                }
                mJudgedGeneration = generation;
                mJudgedText = mTryText;

                std::string result;
                if (mTryText.empty())
                {
                    result = getString("try_hint");
                }
                else
                {
                    std::string word;
                    if (testText(mTryText, word))
                    {
                        LLStringUtil::format_map_t args;
                        args["[WORD]"] = word;
                        result = getString("try_hit");
                        LLStringUtil::format(result, args);
                    }
                    else
                    {
                        result = getString("try_miss");
                    }
                }
                if (result != mShown)
                {
                    mShown = std::move(result);
                    if (Rml::DataModelHandle model = mModel.handle())
                    {
                        model.DirtyVariable("try_result");
                        if (LLRmlView* view = host())
                        {
                            view->markDirty();
                        }
                    }
                }
            }

            void previewSound()
            {
                const LLUUID sound(gSavedPerAccountSettings.getString(SET_SOUND));
                if (sound.notNull() && LLUI::getInstance()->mAudioCallback)
                {
                    LLUI::getInstance()->mAudioCallback(sound);
                }
            }

            void resetSound()
            {
                if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SET_SOUND))
                {
                    control->resetToDefault(true);
                }
            }

            SettingsModel mModel;
            plus::rmlui::ColourLink mColour;
            std::string mShown;                 // the try-out's answer, the model's try_result
            std::string mTryText;               // the try-out field's text
            std::string mJudgedText;            // the text the answer is for
            unsigned mJudgedGeneration = 0;     // ... and the list's (listGeneration(), from 1: 0 is none yet)
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_keywords_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<KeywordsRmlFloater>);
    }
}
