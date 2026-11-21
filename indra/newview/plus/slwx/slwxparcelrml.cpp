/**
 * @file slwxparcelrml.cpp
 * @brief Parcel weather: the window made with RmlUi (research/389).
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

// The same window as slwxfloater.cpp's (the XUI one, frozen: used in safe mode and with PlusRmlUiWindows off), with
// its content app_settings/plus/rmlui/weather_parcel.rml. What the two share (the sliders' ranges and rounding, the tag,
// the parcel, the two questions) is slwxparceledit.h. The sliders and the numbers beside them are the document's
// (data-value / data-attr-value of the data model "weather"); this class keeps the values, makes the XUI slider
// control's rounding, number format and typing rules for the numbers, and the four texts and the two buttons' state.

#include "llviewerprecompiledheaders.h"

#include "slwxparcelrml.h"
#include "slwx.h"
#include "slwxparceledit.h"
#include "slwxtag.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"

#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "lltextvalidate.h"
#include "llui.h"

#include <cmath>
#include <cstdlib>

namespace plus::slwx
{
    namespace
    {
        using namespace parceledit;
        using plus::rmlui::RmlFloater;

        // The two names of a slider in the document's data model.
        const std::string& valueVar(int i)
        {
            static const std::string NAMES[SLIDER_COUNT] = { "v_rain", "v_snow", "v_wind_dir", "v_wind_speed", "v_gust", "v_storm" };
            return NAMES[i];
        }
        const std::string& textVar(int i)
        {
            static const std::string NAMES[SLIDER_COUNT] = { "t_rain", "t_snow", "t_wind_dir", "t_wind_speed", "t_gust", "t_storm" };
            return NAMES[i];
        }

        // The slider an element id (s_<name> or t_<name>) belongs to, or -1.
        int sliderOf(const std::string& id, char kind)
        {
            if (id.size() < 3 || id[0] != kind || id[1] != '_')
            {
                return -1;
            }
            for (int i = 0; i < SLIDER_COUNT; ++i)
            {
                if (id.compare(2, std::string::npos, spec(i).name) == 0)
                {
                    return i;
                }
            }
            return -1;
        }

        // Two values of a slider that are the same step (the document's float arithmetic can be off by a little).
        bool sameStep(int i, F32 a, F32 b)
        {
            return std::fabs(a - b) < spec(i).increment * 0.01f;
        }

        // LLSliderCtrl::onEditorCommit: the text must pass LLLineEditor::postvalidateFloat (a digit), is read as a
        // number, and must be inside the slider's range (else the old value stays).
        bool numberOf(int i, const std::string& text, F32& out)
        {
            if (!LLLineEditor::postvalidateFloat(text))
            {
                return false;
            }
            const F32 value = static_cast<F32>(std::atof(text.c_str()));
            if (!(value >= spec(i).min_value && value <= spec(i).max_value))
            {
                return false;
            }
            out = value;
            return true;
        }

        class ParcelWeatherRmlFloater : public RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~ParcelWeatherRmlFloater() override
            {
                closeDocument();    // before the members the document points at go
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                loadFromParcel();
                mDirty = true;
            }

            void draw() override
            {
                refresh();
                RmlFloater::draw();
            }

        protected:
            void beforeLoad(Rml::Context& context) override
            {
                Rml::DataModelConstructor model = context.CreateDataModel("weather");
                if (!model)
                {
                    return;
                }
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    model.Bind(valueVar(i), &mValue[i]);    // data-value: the slider writes it
                    model.Bind(textVar(i), &mText[i]);
                }
                model.Bind("reason", &mReason);
                model.Bind("preview", &mPreview);
                model.Bind("bytes", &mBytes);
                model.Bind("warning", &mWarning);
                model.Bind("apply_ok", &mApplyOk);
                model.Bind("clear_ok", &mClearOk);
                mModel = model.GetModelHandle();
            }

            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    const std::string name = spec(i).name;
                    view->listen(document.GetElementById("s_" + name), "change");
                    Rml::Element* box = document.GetElementById("t_" + name);
                    view->listen(box, "focus");
                    view->listenCapture(box, "blur");   // after RmlUi's own Blur ended an IME composition
                    view->listen(box, "change");
                    view->listen(box, "keydown");       // Esc: back to the value
                }
                view->listen(document.GetElementById("apply"), "click");
                view->listen(document.GetElementById("clear"), "click");
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
                    if (id == "apply" && mApplyOk)
                    {
                        askApply(this, mTag);
                    }
                    else if (id == "clear" && mClearOk)
                    {
                        askClear(this);
                    }
                    return;
                }
                if (const int slider = sliderOf(id, 's'); slider >= 0)
                {
                    // The bar moved (by the user or by a value given to it): the value, rounded as the XUI slider does.
                    setValue(slider, event.GetParameter<float>("value", mValue[slider]), false);
                    return;
                }
                const int number = sliderOf(id, 't');
                auto* box = rmlui_dynamic_cast<Rml::ElementFormControl*>(event.GetCurrentElement());
                if (number < 0 || !box)
                {
                    return;
                }
                if (type == Rml::EventId::Focus)
                {
                    mHeld[number]     = true;
                    mFocusText[number] = box->GetValue();
                    mLastGood[number]  = mFocusText[number];
                }
                else if (type == Rml::EventId::Blur)
                {
                    mHeld[number] = false;
                    commitNumber(number, *box, committedText(*box));
                }
                else if (type == Rml::EventId::Keydown)
                {
                    // the caret and selection before the key's text arrives, for putting them back if it is refused
                    if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(box))
                    {
                        input->GetSelection(&mSelBefore[number][0], &mSelBefore[number][1], nullptr);
                    }
                    // as the XUI line editor's revert_on_esc: the number goes back to the slider's value
                    if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE && box->GetValue() != mText[number])
                    {
                        box->SetValue(mText[number]);
                    }
                }
                else if (type == Rml::EventId::Change)
                {
                    if (event.GetParameter<bool>("linebreak", false))
                    {
                        commitNumber(number, *box, box->GetValue());
                    }
                    else
                    {
                        // The XUI number box's prevalidator (LLTextValidate::validateFloat): a key that would make
                        // the text something else than a number is refused. The text, the caret and the selection go
                        // back as LLLineEditorRollback does, and the explanation shows (once in 30 s).
                        const std::string text = box->GetValue();
                        if (LLTextValidate::validateFloat.validate(text))
                        {
                            mLastGood[number] = text;
                        }
                        else
                        {
                            LLTextValidate::validateFloat.showLastErrorUsingTimeout();
                            LLUI::getInstance()->reportBadKeystroke();
                            box->SetValue(mLastGood[number]);
                            const int length = static_cast<int>(mLastGood[number].size());
                            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(box))
                            {
                                input->SetSelectionRange(llmin(mSelBefore[number][0], length), llmin(mSelBefore[number][1], length));
                            }
                        }
                    }
                }
            }

            void unbind() override
            {
                mModel = Rml::DataModelHandle();
            }

        private:
            explicit ParcelWeatherRmlFloater(const LLSD& key) : RmlFloater(key) {}

            void wake(const std::string& variable)
            {
                if (mModel)
                {
                    mModel.DirtyVariable(variable);
                    if (LLRmlView* view = host())
                    {
                        view->markDirty();
                    }
                }
            }

            // The number beside the slider: what the document shows is `mText` (a box being typed in is left alone:
            // its text is read when it is committed).
            void setText(int i, const std::string& text)
            {
                if (mText[i] == text)
                {
                    return;
                }
                mText[i] = text;
                if (!mHeld[i])
                {
                    wake(textVar(i));
                }
            }

            // A slider's value, rounded to its step as LLSlider::setValue does. `tellBar`: the bar did not make
            // this value itself (a number typed, the parcel read), so it is given to it.
            void setValue(int i, F32 raw, bool tellBar)
            {
                const F32 value = snap(i, raw);
                const bool off  = std::fabs(raw - value) > spec(i).increment * 0.001f;
                mValue[i] = value;
                if (tellBar || off)
                {
                    wake(valueVar(i));
                }
                setText(i, valueText(i, value));
            }

            // Enter, or the focus leaving, in a number box: LLSliderCtrl::onEditorCommit. A text that is no number or
            // is outside the range leaves the value and the box shows the value again.
            void commitNumber(int i, Rml::ElementFormControl& box, const std::string& text)
            {
                if (text != mFocusText[i])
                {
                    F32 number = 0.f;
                    if (numberOf(i, text, number))
                    {
                        setValue(i, number, true);
                    }
                    else
                    {
                        LLUI::getInstance()->reportBadKeystroke();
                    }
                }
                if (box.GetValue() != mText[i])
                {
                    box.SetValue(mText[i]);
                }
                mFocusText[i] = mText[i];
                mLastGood[i]  = mText[i];
                wake(textVar(i));       // (a held box is not told by setText)
            }

            // The sliders start from the tag that stands in the parcel now, if there is one.
            void loadFromParcel()
            {
                F32 wanted[SLIDER_COUNT];
                valuesOf(parcelDescription(), wanted);
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    setValue(i, wanted[i], true);
                    // A slider rounds to its step: what it holds now is what "not touched" means below.
                    mBase[i] = mValue[i];
                }
            }

            bool sliderTouched() const
            {
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    if (!sameStep(i, mValue[i], mBase[i]))
                    {
                        return true;
                    }
                }
                return false;
            }

            void setString(std::string& member, const std::string& text, const char* variable)
            {
                if (member != text)
                {
                    member = text;
                    wake(variable);
                }
            }

            void setFlag(bool& member, bool value, const char* variable)
            {
                if (member != value)
                {
                    member = value;
                    wake(variable);
                }
            }

            // Looked at every frame; the texts are set only when something they depend on changed.
            void refresh()
            {
                const std::string& description = parcelDescription();
                const bool         canEdit     = canEditCurrentParcel();
                bool same = !mDirty && canEdit == mCanEdit && description == mDescription;
                for (int i = 0; same && i < SLIDER_COUNT; ++i)
                {
                    same = sameStep(i, mValue[i], mSeen[i]);
                }
                if (same)
                {
                    return;
                }
                // The description changed under an untouched window (another parcel, or a new tag): follow it.
                if (!mDirty && description != mDescription && !sliderTouched())
                {
                    loadFromParcel();
                }
                mDirty       = false;
                mCanEdit     = canEdit;
                mDescription = description;
                F32 values[SLIDER_COUNT];
                for (int i = 0; i < SLIDER_COUNT; ++i)
                {
                    mSeen[i] = mValue[i];
                    // (a slider not moved is exactly what it was loaded as)
                    values[i] = sameStep(i, mValue[i], mBase[i]) ? mBase[i] : snap(i, mValue[i]);
                }

                const tag::ParseResult existing = tag::parse(description);
                const bool unclosed = tag::hasUnclosedOpen(description);
                mTag = parceledit::buildTag(description, existing, values, mBase);
                const int  left = bytesLeftWith(description, mTag);
                const bool fits = left >= 0 && !unclosed;

                setString(mPreview, mTag, "preview");
                LLStringUtil::format_map_t args;
                args["[LEFT]"] = std::to_string(left);
                setString(mBytes, getString(left >= 0 ? "bytes_left" : "bytes_over", args), "bytes");
                setString(mReason, canEdit ? getString("can_edit") : getString("cannot_edit"), "reason");
                LLStringUtil::format_map_t bad;
                bad["[N]"] = std::to_string(existing.bad_items);
                setString(mWarning, unclosed ? getString("tag_unclosed")
                                             : (existing.bad_items > 0 ? getString("tag_warning", bad) : std::string()),
                          "warning");
                setFlag(mApplyOk, canEdit && fits, "apply_ok");
                setFlag(mClearOk, canEdit && existing.found, "clear_ok");
            }

            Rml::DataModelHandle mModel;
            F32         mValue[SLIDER_COUNT] = {};      // the sliders (data-value)
            std::string mText[SLIDER_COUNT];            // the numbers beside them (data-attr-value)
            bool        mHeld[SLIDER_COUNT]  = {};      // the number box has the focus
            std::string mFocusText[SLIDER_COUNT];       // its text when it got the focus / was last committed
            std::string mLastGood[SLIDER_COUNT];        // its last text that the typing rule allowed
            int         mSelBefore[SLIDER_COUNT][2] = {};   // its selection at the last key down
            F32         mBase[SLIDER_COUNT]  = {};      // the sliders as loaded from the parcel
            F32         mSeen[SLIDER_COUNT]  = {};      // what the texts were last made from ...
            bool        mDirty    = true;
            bool        mCanEdit  = false;
            std::string mDescription;
            std::string mTag;
            std::string mReason, mPreview, mBytes, mWarning;    // the four texts
            bool        mApplyOk = false;
            bool        mClearOk = false;
        };
    }

    void addParcelRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_weather_parcel_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<ParcelWeatherRmlFloater>);
    }
}
