/**
 * @file timeconvrml.cpp
 * @brief Time converter: the window made with RmlUi (research/385).
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

// The same window as timeconvfloater.cpp's (the XUI one, frozen: used in safe mode and with PlusRmlUiWindows off), with
// its content app_settings/plus/rmlui/timeconv.rml. How it works is told there: one moment (mUtc), up to four rows
// showing it as a wall clock, a row's five fields typed, stepped (Up/Down keys, the wheel) or pasted, "follow the clock".
// The arithmetic is timeconvmath.h and the zone of a row timeconvshared.h (both shared with the XUI window).
//
// What is different from the XUI window in how it is made (not in what it does):
//  - the rows and their fields are the document's data-for (a static skeleton, made once); the texts of the fields, the
//    info texts, the note and the summary are written by this class straight to the elements, only when they differ
//    (the XUI window's setIfChanged), so a typed row is left alone and the caret stays. The data model holds the skeleton
//    and the "tick" box only.
//  - a write of this class to a select comes back as a "change" event (a text field sends none for SetValue, but a value
//    RmlUi has to fix): mWriting (RAII) makes the handlers skip both. The "tick" box's write comes back through the data
//    model's setter: it does nothing when the box shows what mTick already is.
//  - the number fields take digits only (the XUI validator): a "textinput" with any other character is dropped; the same is
//    checked again on "change" for what arrives without it (an IME's result, a paste RmlUi did itself): the field goes back
//    to its text before (as the XUI field, which refused the paste).
//  - the moment and the "follow the clock" state live as long as the session, not as the window (the XUI window was kept
//    when closed): unbind() keeps them in sSession, the next window starts from them.

#include "llviewerprecompiledheaders.h"

#include "timeconv.h"
#include "timeconvmath.h"
#include "timeconvrml.h"
#include "timeconvshared.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"

#include "llfloaterreg.h"
#include "lltimer.h"
#include "llviewercontrol.h"

#include <array>
#include <vector>

namespace plus::timeconv
{
    namespace
    {
        // Where the parts of a row are (floater_plus_time_conv.xml): left of the field and of its unit mark, the field's width
        // and the characters it takes.
        struct FieldGeometry
        {
            int left, width, unit_left, max_chars;
        };
        constexpr FieldGeometry GEOMETRY[F_COUNT] = {
            { 130, 44, 176, 4 }, { 192, 28, 222, 2 }, { 238, 28, 268, 2 }, { 286, 28, 316, 2 }, { 332, 28, 362, 2 },
        };
        constexpr int UNIT_WIDTH = 14;

        // The data model's rows (timeconv.rml): what a row's parts say.
        struct RowInfo
        {
            std::string label;
            bool is_zone = false;
            bool removable = false;
            std::string copy;
            std::string tip_copy;
            std::string tip_zone;
            std::string remove;
            std::string tip_remove;
        };
        // ... and the fields of a row (the same five in every row).
        struct FieldInfo
        {
            std::string style;      // the wrapper's place in the row
            std::string istyle;     // the field's width
            std::string ustyle;     // the unit mark's place in the wrapper
            int maxlen = 2;
            std::string tip;
            std::string unit;
        };

        struct ZoneChoice
        {
            std::string id;
            std::string label;
        };

        std::string dp(const char* name, int value)
        {
            return std::string(name) + ": " + std::to_string(value) + "dp";
        }

        bool digitsOnly(const std::string& text)
        {
            for (const char c : text)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }
            return true;
        }

        // What the XUI window kept by not being destroyed when closed: the moment and whether it follows the clock.
        struct Session
        {
            bool valid = false;
            Minutes utc = 0;
            bool tick = true;
        };
        Session sSession;

        class TimeConvRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~TimeConvRmlFloater() override
            {
                closeDocument();    // before the members the document points at go
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                if (mReady)
                {
                    LLRmlView::Scope scope(host());
                    if (mTick)
                    {
                        mUtc = now();
                    }
                    refreshAll(-1);
                }
            }

            // Each frame: one integer (the minute) when following the clock (as the XUI window did).
            void draw() override
            {
                if (mReady && mTick)
                {
                    const Minutes n = now();
                    if (n != mUtc)
                    {
                        LLRmlView::Scope scope(host());
                        mUtc = n;
                        refreshAll(-1);
                    }
                }
                RmlFloater::draw();
            }

        protected:
            void beforeLoad(Rml::Context& context) override
            {
                // The box shows mTick as the data model first updates (before afterLoad): it is set here.
                mUtc = sSession.valid ? sSession.utc : now();
                mTick = sSession.valid ? sSession.tick : gSavedSettings.getBOOL(SETTING_TICK);
                Rml::DataModelConstructor model = context.CreateDataModel("timeconv");
                if (!model)
                {
                    return;
                }
                fillSkeleton();
                if (auto row = model.RegisterStruct<RowInfo>())
                {
                    row.RegisterMember("label", &RowInfo::label);
                    row.RegisterMember("is_zone", &RowInfo::is_zone);
                    row.RegisterMember("removable", &RowInfo::removable);
                    row.RegisterMember("copy", &RowInfo::copy);
                    row.RegisterMember("tip_copy", &RowInfo::tip_copy);
                    row.RegisterMember("tip_zone", &RowInfo::tip_zone);
                    row.RegisterMember("remove", &RowInfo::remove);
                    row.RegisterMember("tip_remove", &RowInfo::tip_remove);
                }
                if (auto field = model.RegisterStruct<FieldInfo>())
                {
                    field.RegisterMember("style", &FieldInfo::style);
                    field.RegisterMember("istyle", &FieldInfo::istyle);
                    field.RegisterMember("ustyle", &FieldInfo::ustyle);
                    field.RegisterMember("maxlen", &FieldInfo::maxlen);
                    field.RegisterMember("tip", &FieldInfo::tip);
                    field.RegisterMember("unit", &FieldInfo::unit);
                }
                model.RegisterArray<std::vector<RowInfo>>();
                model.RegisterArray<std::vector<FieldInfo>>();
                model.Bind("rows", &mRowInfo);
                model.Bind("fields", &mFieldInfo);
                // The box is the window's own (not control_name): the setting is written only by followClock, as the XUI
                // window did (an edit turns following off without saving it).
                model.BindFunc("tick", [this](Rml::Variant& value) { value = mTick; },
                               [this](const Rml::Variant& value) {
                                   // The box's attribute changes while the document loads (before mReady) and when this class
                                   // shows mTick (setTickShown): only a user's click is a change of mTick.
                                   const bool on = value.Get<bool>(false);
                                   if (mReady && on != mTick)
                                   {
                                       followClock(on);
                                   }
                               });
                mHandle = model.GetModelHandle();
            }

            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                Rml::Element* tc = document.GetElementById("tc");
                // The template of a data-for stays in the document (display none, with the data-for attribute): not a row.
                Rml::ElementList found;
                std::vector<Rml::Element*> rows;
                document.GetElementsByClassName(found, "tcrow");
                for (Rml::Element* element : found)
                {
                    if (!element->HasAttribute("data-for"))
                    {
                        rows.push_back(element);
                    }
                }
                if (!view || !tc || rows.size() != size_t(MAX_ROWS))
                {
                    LL_WARNS("RmlUi") << "RmlUi: timeconv.rml: " << rows.size() << " rows (" << MAX_ROWS << " expected)" << LL_ENDL;
                    return;
                }
                mNote.element = document.GetElementById("note");
                mSummary.element = document.GetElementById("summary");
                mAddRow = document.GetElementById("add_row");
                if (!mNote.element || !mSummary.element || !mAddRow)
                {
                    LL_WARNS("RmlUi") << "RmlUi: timeconv.rml: note, summary or add_row missing" << LL_ENDL;
                    return;
                }
                for (int r = 0; r < MAX_ROWS; ++r)
                {
                    Row& row = mRow[r];
                    row.root = rows[r];
                    row.info.element = row.root->QuerySelector(".tcinfo");
                    row.select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(row.root->QuerySelector("select"));
                    Rml::ElementList all_fields;
                    std::vector<Rml::Element*> fields;
                    row.root->GetElementsByClassName(all_fields, "tcf");
                    for (Rml::Element* element : all_fields)
                    {
                        if (!element->HasAttribute("data-for"))
                        {
                            fields.push_back(element);
                        }
                    }
                    if (!row.info.element || fields.size() != size_t(F_COUNT) || (r >= 2 && !row.select))
                    {
                        LL_WARNS("RmlUi") << "RmlUi: timeconv.rml: row " << r << " has " << fields.size() << " fields, or lacks its info or select" << LL_ENDL;
                        return;
                    }
                    for (int f = 0; f < F_COUNT; ++f)
                    {
                        row.field[f] = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(fields[f]);
                        if (!row.field[f])
                        {
                            LL_WARNS("RmlUi") << "RmlUi: timeconv.rml: a field of row " << r << " is not an input" << LL_ENDL;
                            return;
                        }
                        view->listenCapture(fields[f], "focus");     // what the commit on losing the focus (and Esc) is compared with
                        view->listenCapture(fields[f], "blur");     // after RmlUi's own Blur ended an IME composition
                    }
                }
                // The window's own keys, wheel and typed characters before the field's (a capture listener on an ancestor
                // runs first, and a stop there keeps RmlUi's own action from running); the rest comes up from the parts.
                for (const char* type : { "keydown", "mousescroll", "textinput" })
                {
                    view->listenCapture(tc, type);
                }
                for (const char* type : { "change", "click" })
                {
                    view->listen(tc, type);
                }

                mRow[0].zone.zone = zones()[ZONE_SL];
                mRow[1].zone.zone = zones()[ZONE_UTC];
                fillZoneChoices();
                for (int r = 2; r < MAX_ROWS; ++r)
                {
                    Row& row = mRow[r];
                    for (const ZoneChoice& choice : mChoices)
                    {
                        row.select->Add(Rml::StringUtilities::EncodeRml(choice.label), choice.id);
                    }
                    const std::string saved = gSavedSettings.getString(r == 2 ? SETTING_ZONE2 : SETTING_ZONE3);
                    row.zoneId = hasChoice(saved) ? saved : (r == 2 ? "pc" : "et");
                    writeSelect(row, row.zoneId);
                    updateRowZone(r);
                }
                mReady = true;
                setRows(gSavedSettings.getS32(SETTING_ROWS) >= MAX_ROWS ? MAX_ROWS : 3);
            }

            void onEvent(Rml::Event& event) override
            {
                if (!mReady)
                {
                    return;
                }
                const Rml::EventId type = event.GetId();
                Rml::Element* target = event.GetTargetElement();
                if (type == Rml::EventId::Click)
                {
                    onClick(target);
                    return;
                }
                int r = -1;
                int f = -1;
                if (Rml::Element* field = enclosingField(target, r, f))
                {
                    onFieldEvent(event, type, r, f, field);
                    return;
                }
                if (type == Rml::EventId::Change && !mWriting)
                {
                    // a place chosen in a row's select
                    for (int row = 2; row < MAX_ROWS; ++row)
                    {
                        if (mRow[row].select && mRow[row].select == target)
                        {
                            onZoneChanged(row, mRow[row].select->GetValue());
                            break;
                        }
                    }
                }
            }

            // The window closes with a field being typed in: closeDocument() blurs the focused field first, whose listener
            // commits it (a field's blur commits what changed). Nothing else is waiting.
            void commitPending() override {}

            void unbind() override
            {
                if (mReady)
                {
                    sSession.valid = true;      // the next window goes on from here (as the XUI window, kept when closed)
                    sSession.utc = mUtc;
                    sSession.tick = mTick;
                }
                mReady = false;
                mFocusBox = nullptr;
                mHandle = Rml::DataModelHandle();
                for (Row& row : mRow)
                {
                    row = Row();
                }
                mNote = TextCell();
                mSummary = TextCell();
                mAddRow = nullptr;
            }

        private:
            struct TextCell
            {
                Rml::Element* element = nullptr;
                std::string shown;
            };
            struct Row
            {
                Rml::Element* root = nullptr;
                Rml::ElementFormControlSelect* select = nullptr;
                Rml::ElementFormControlInput* field[F_COUNT] = {};
                TextCell info;
                RowZone zone;
                std::string zoneId;
            };

            explicit TimeConvRmlFloater(const LLSD& key) : RmlFloater(key) {}

            // An own writes' guard: what this class writes to a field or select comes back as a change event.
            class Writing
            {
            public:
                explicit Writing(bool& flag) : mFlag(flag), mWas(flag) { mFlag = true; }
                ~Writing() { mFlag = mWas; }
                Writing(const Writing&) = delete;
                Writing& operator=(const Writing&) = delete;

            private:
                bool& mFlag;
                bool mWas;
            };

            static Minutes now() { return static_cast<Minutes>(time_corrected()) / 60; }

            // ---- the document's skeleton and the choices of a place ------------------------------------------------

            void fillSkeleton()
            {
                mRowInfo.assign(MAX_ROWS, RowInfo());
                for (int r = 0; r < MAX_ROWS; ++r)
                {
                    RowInfo& info = mRowInfo[r];
                    info.is_zone = r >= 2;
                    info.removable = r == 3;
                    info.label = r == 0 ? getString("label_sl") : (r == 1 ? getString("label_utc") : std::string());
                    info.copy = getString("copy");
                    info.tip_copy = getString("tip_copy");
                    info.tip_zone = getString("tip_zone");
                    info.remove = getString("remove");
                    info.tip_remove = getString("tip_remove");
                }
                mFieldInfo.assign(F_COUNT, FieldInfo());
                for (int f = 0; f < F_COUNT; ++f)
                {
                    const FieldGeometry& g = GEOMETRY[f];
                    FieldInfo& info = mFieldInfo[f];
                    info.style = dp("left", g.left) + "; " + dp("width", g.unit_left + UNIT_WIDTH - g.left);
                    info.istyle = dp("width", g.width);
                    info.ustyle = dp("left", g.unit_left - g.left) + "; top: 7dp";
                    info.maxlen = g.max_chars;
                    info.tip = getString(std::string("tip_") + FIELD_NAMES[f]);
                    info.unit = getString(std::string("u_") + FIELD_NAMES[f]);
                }
            }

            void fillZoneChoices()
            {
                mChoices.push_back({ "pc", getString("zone_pc") });
                for (size_t i = 2; i < zones().size(); ++i) // after SL time and UTC
                {
                    const std::string id = zones()[i].id;
                    mChoices.push_back({ id, getString("zone_" + id) });
                }
                for (int h = FIXED_HOURS_MIN; h <= FIXED_HOURS_MAX; ++h)
                {
                    mChoices.push_back({ "u" + std::to_string(h * 60), offsetText(h * 60) });
                }
            }

            bool hasChoice(const std::string& id) const
            {
                for (const ZoneChoice& choice : mChoices)
                {
                    if (choice.id == id)
                    {
                        return true;
                    }
                }
                return false;
            }

            std::string choiceLabel(const std::string& id) const
            {
                for (const ZoneChoice& choice : mChoices)
                {
                    if (choice.id == id)
                    {
                        return choice.label;
                    }
                }
                return std::string();
            }

            // ---- writing to the document (only what differs) --------------------------------------------------------

            // A write to a field that is not the window's own change (the field goes back to a text it had).
            void writeField(Rml::ElementFormControlInput* field, const std::string& text)
            {
                if (field->GetValue() != text)
                {
                    Writing writing(mWriting);
                    field->SetValue(text);
                    touched();
                }
            }

            // The window writes a field (a moment, a step, a paste, Esc): the text the field is compared with on leaving is the
            // new one too (the XUI line editor's setText made it its previous text).
            void setField(Rml::ElementFormControlInput* field, const std::string& text)
            {
                writeField(field, text);
                if (field == mFocusBox)
                {
                    mFocusText = text;
                }
                for (int r = 0; r < MAX_ROWS; ++r)
                {
                    for (int f = 0; f < F_COUNT; ++f)
                    {
                        if (mRow[r].field[f] == field)
                        {
                            mPrev[r][f] = text;
                        }
                    }
                }
            }

            void setText(TextCell& cell, const std::string& text)
            {
                if (cell.shown != text)
                {
                    cell.shown = text;
                    cell.element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
                    touched();
                }
            }

            void writeSelect(Row& row, const std::string& id)
            {
                if (row.select->GetValue() != id)
                {
                    Writing writing(mWriting);
                    row.select->SetValue(id);
                    touched();
                }
            }

            void touched()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            void setTickShown()
            {
                if (mHandle)
                {
                    mHandle.DirtyVariable("tick");
                    touched();
                }
            }

            // ---- the events ------------------------------------------------------------------------------------------

            // The number field an event's target is in (the field's own parts count: its text, its selection), and where
            // it is. Null: none.
            Rml::Element* enclosingField(Rml::Element* target, int& r, int& f)
            {
                for (Rml::Element* element = target; element; element = element->GetParentNode())
                {
                    for (int row = 0; row < MAX_ROWS; ++row)
                    {
                        for (int field = 0; field < F_COUNT; ++field)
                        {
                            if (mRow[row].field[field] == element)
                            {
                                r = row;
                                f = field;
                                return element;
                            }
                        }
                    }
                }
                return nullptr;
            }

            void onFieldEvent(Rml::Event& event, Rml::EventId type, int r, int f, Rml::Element* element)
            {
                Rml::ElementFormControlInput* box = mRow[r].field[f];
                if (type == Rml::EventId::Focus)
                {
                    if (event.GetTargetElement() == element)
                    {
                        mFocusBox = box;
                        mFocusText = box->GetValue();   // what a commit on losing the focus (and Esc) is compared with
                        mPrev[r][f] = mFocusText;
                        // (the whole text is selected by the host: select-on-focus in timeconv.rml)
                    }
                }
                else if (type == Rml::EventId::Blur)
                {
                    if (event.GetTargetElement() == element && !mWriting)
                    {
                        mFocusBox = nullptr;
                        const std::string text = committedText(*box);
                        if (text != mFocusText)
                        {
                            mFocusText = text;
                            onCommit(r);
                        }
                    }
                }
                else if (type == Rml::EventId::Change)
                {
                    if (mWriting || event.GetTargetElement() != element)
                    {
                        return;
                    }
                    const std::string text = box->GetValue();
                    if (!digitsOnly(text))
                    {
                        writeField(box, mPrev[r][f]);   // the XUI field refused such a text: it stays as it was
                        return;
                    }
                    mPrev[r][f] = text;
                    if (event.GetParameter<bool>("linebreak", false))
                    {
                        mFocusText = text;
                        onCommit(r);
                    }
                    else
                    {
                        onEdit(r);
                    }
                }
                else if (type == Rml::EventId::Keydown)
                {
                    onKey(event, r, f);
                }
                else if (type == Rml::EventId::Mousescroll)
                {
                    const float delta = event.GetParameter<float>("wheel_delta_y", 0.f);
                    if (delta != 0.f)
                    {
                        onStep(r, f, delta < 0.f ? 1 : -1);    // wheel up (negative) raises, as the XUI field
                        event.StopPropagation();
                    }
                }
                else if (type == Rml::EventId::Textinput)
                {
                    if (!digitsOnly(event.GetParameter("text", Rml::String())))
                    {
                        event.StopImmediatePropagation();       // the field does not take it
                    }
                }
            }

            void onKey(Rml::Event& event, int r, int f)
            {
                const auto key = static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
                const bool ctrl = event.GetParameter<int>("ctrl_key", 0) > 0;
                const bool alt = event.GetParameter<int>("alt_key", 0) > 0;
                const bool shift = event.GetParameter<int>("shift_key", 0) > 0;
                const bool meta = event.GetParameter<int>("meta_key", 0) > 0;
                if (!ctrl && !alt && !shift && !meta && (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN))
                {
                    onStep(r, f, key == Rml::Input::KI_UP ? 1 : -1);
                    event.StopPropagation();
                }
                else if (key == Rml::Input::KI_ESCAPE && !ctrl && !alt && !shift && !meta)
                {
                    // revert_on_esc of the XUI line editor: the text goes back to what it was when the field got the focus
                    // (the key goes on: the window's Esc stays as it was)
                    if (mRow[r].field[f]->GetValue() != mFocusText)
                    {
                        setField(mRow[r].field[f], mFocusText);
                    }
                }
                else if (!alt && !meta && ((ctrl && !shift && key == Rml::Input::KI_V) || (shift && !ctrl && key == Rml::Input::KI_INSERT)))
                {
                    LLWString text;
                    if (LLClipboard::instance().pasteFromClipboard(text) && onPasteText(r, wstring_to_utf8str(text)))
                    {
                        event.StopPropagation();    // RmlUi's own paste does not run
                    }
                }
            }

            void onClick(Rml::Element* target)
            {
                for (Rml::Element* element = target; element; element = element->GetParentNode())
                {
                    const std::string id = element->GetId();
                    if (id == "now")            { followClock(true); return; }
                    if (id == "plus_hour")      { shift(60); return; }
                    if (id == "minus_hour")     { shift(-60); return; }
                    if (id == "plus_day")       { shift(1440); return; }
                    if (id == "minus_day")      { shift(-1440); return; }
                    if (id == "copy_all")       { copyText(mSummary.shown); return; }
                    if (id == "add_row")        { setRows(4); return; }
                    const bool copy = element->IsClassSet("tccopy");
                    if (copy || element->IsClassSet("tcrm"))
                    {
                        for (int r = 0; r < MAX_ROWS; ++r)
                        {
                            if (mRow[r].root && element->GetParentNode() == mRow[r].root)
                            {
                                if (copy)
                                {
                                    copyText(rowText(r));
                                }
                                else
                                {
                                    setRows(3);
                                }
                                return;
                            }
                        }
                    }
                }
            }

            // ---- the window's state (the XUI window's, one for one) ------------------------------------------------

            void updateRowZone(int r)
            {
                Row& row = mRow[r];
                if (r < 2)
                {
                    return;
                }
                if (!resolveZone(row.zoneId, row.zone))
                {
                    resolveZone("pc", row.zone);
                }
            }

            void setRows(int rows)
            {
                if (rows < MAX_ROWS)
                {
                    // the fourth row goes: a field or button of it with the focus lets go first (its blur commits)
                    Rml::Context* context = host() ? host()->getContext() : nullptr;
                    Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
                    for (Rml::Element* element = focus; element; element = element->GetParentNode())
                    {
                        if (element == mRow[3].root)
                        {
                            focus->Blur();
                            break;
                        }
                    }
                }
                mRows = rows;
                mRow[3].root->SetClass("gone", rows < 4);
                mAddRow->SetProperty("display", rows < 4 ? "block" : "none");
                gSavedSettings.setS32(SETTING_ROWS, rows);
                refreshAll(-1);
                touched();
            }

            void followClock(bool on)
            {
                mTick = on;
                setTickShown();
                gSavedSettings.setBOOL(SETTING_TICK, on);
                if (on)
                {
                    mUtc = now();
                }
                setText(mNote, std::string());
                refreshAll(-1);
            }

            void stopFollowing()
            {
                if (mTick)
                {
                    mTick = false;
                    setTickShown();
                }
            }

            void shift(int minutes)
            {
                stopFollowing();
                mUtc += minutes;
                setText(mNote, std::string());
                refreshAll(-1);
            }

            std::string rowName(int r)
            {
                if (r == 0) return getString("name_sl");
                if (r == 1) return "UTC";
                return choiceLabel(mRow[r].zoneId);
            }

            // Reads the fields of a row. Returns whether they make a real date and time; marks the wrong fields red when asked.
            bool readRow(int r, Civil& c, bool mark)
            {
                Row& row = mRow[r];
                int v[F_COUNT] = {};
                bool ok[F_COUNT];
                for (int f = 0; f < F_COUNT; ++f)
                {
                    const std::string t = row.field[f]->GetValue();
                    ok[f] = !t.empty();
                    v[f]  = ok[f] ? std::atoi(t.c_str()) : 0;
                }
                ok[F_YEAR]   = ok[F_YEAR] && v[F_YEAR] >= YEAR_MIN && v[F_YEAR] <= YEAR_MAX;
                ok[F_MONTH]  = ok[F_MONTH] && v[F_MONTH] >= 1 && v[F_MONTH] <= 12;
                const int dim = (ok[F_YEAR] && ok[F_MONTH]) ? daysInMonth(v[F_YEAR], v[F_MONTH]) : 31;
                ok[F_DAY]    = ok[F_DAY] && v[F_DAY] >= 1 && v[F_DAY] <= dim;
                ok[F_HOUR]   = ok[F_HOUR] && v[F_HOUR] >= 0 && v[F_HOUR] <= 23;
                ok[F_MINUTE] = ok[F_MINUTE] && v[F_MINUTE] >= 0 && v[F_MINUTE] <= 59;
                bool all = true;
                for (int f = 0; f < F_COUNT; ++f)
                {
                    all = all && ok[f];
                    if (mark)
                    {
                        row.field[f]->SetClass("bad", !ok[f]);
                    }
                }
                if (all)
                {
                    c.year = v[F_YEAR];
                    c.month = v[F_MONTH];
                    c.day = v[F_DAY];
                    c.hour = v[F_HOUR];
                    c.minute = v[F_MINUTE];
                }
                return all;
            }

            // A wall clock in the row, as the moment. Turns the clock off and rewrites the other rows.
            void applyRow(int r, const Civil& c, bool rewriteThisRow)
            {
                stopFollowing();
                Note note = Note::None;
                mUtc = toUtc(mRow[r].zone, c, &note);
                setText(mNote, std::string());
                if (note != Note::None)
                {
                    LLStringUtil::format_map_t args;
                    args["[WHERE]"] = rowName(r);
                    args["[TIME]"]  = two(c.hour) + ":" + two(c.minute);
                    const Civil fixed = toLocal(mRow[r].zone, mUtc);
                    args["[FIXED]"] = two(fixed.hour) + ":" + two(fixed.minute);
                    setText(mNote, getString(note == Note::Gap ? "note_gap" : "note_overlap", args));
                }
                // While typing, the row stays as typed even when its clock does not exist (02:30 at the start of summer
                // time): the commit (leaving the field) writes it as 03:30.
                refreshAll(rewriteThisRow ? -1 : r);
            }

            void onEdit(int r)
            {
                Civil c;
                if (!readRow(r, c, true))
                {
                    return; // half typed, or wrong: the others stay as they are
                }
                applyRow(r, c, false);
            }

            void onCommit(int r)
            {
                Civil c;
                if (!readRow(r, c, true))
                {
                    setText(mNote, getString("note_range"));
                    return;
                }
                applyRow(r, c, true); // also writes this row whole ("7" becomes "07")
            }

            void onStep(int r, int field, int delta)
            {
                Civil c;
                if (!readRow(r, c, false))
                {
                    c = toLocal(mRow[r].zone, mUtc);
                }
                switch (field)
                {
                case F_YEAR:   c = addMonths(c, 12 * delta); break;
                case F_MONTH:  c = addMonths(c, delta); break;
                case F_DAY:    c = addWallMinutes(c, 1440LL * delta); break;
                case F_HOUR:   c = addWallMinutes(c, 60LL * delta); break;
                default:       c = addWallMinutes(c, delta); break;
                }
                if (!validCivil(c))
                {
                    return; // past 1970 or 2100
                }
                applyRow(r, c, true);
            }

            bool onPasteText(int r, const std::string& text)
            {
                Parsed p;
                if (!parseDateTime(text, p))
                {
                    return false;
                }
                if (!p.hasDate && p.numbers < 2 && !p.ampm)
                {
                    return false; // one plain number: an ordinary paste into the field
                }
                Civil c;
                if (!readRow(r, c, false))
                {
                    c = toLocal(mRow[r].zone, mUtc);
                }
                if (p.hasDate)
                {
                    c.year = p.year;
                    c.month = p.month;
                    c.day = p.day;
                }
                if (p.hasTime)
                {
                    c.hour = p.hour;
                    c.minute = p.minute;
                }
                if (!validCivil(c))
                {
                    return false;
                }
                applyRow(r, c, true);
                return true;
            }

            void onZoneChanged(int r, const std::string& id)
            {
                if (!hasChoice(id))
                {
                    return;
                }
                mRow[r].zoneId = id;
                updateRowZone(r);
                gSavedSettings.setString(r == 2 ? SETTING_ZONE2 : SETTING_ZONE3, id);
                setText(mNote, std::string());
                refreshAll(-1);
            }

            std::string infoText(int r)
            {
                const RowZone& rz = mRow[r].zone;
                if (r == 1)
                {
                    return "UTC";
                }
                const int off = offsetAt(rz, mUtc);
                if (rz.pc)
                {
                    return offsetText(off) + " (" + getString("info_pc") + ")";
                }
                const std::string abbr = abbrText(rz.zone, isDst(rz.zone, mUtc));
                const std::string ot   = offsetText(off);
                return abbr.empty() || abbr == ot ? ot : abbr + " " + ot;
            }

            // "2026-10-24 23:00 JST"
            std::string rowText(int r)
            {
                return civilText(toLocal(mRow[r].zone, mUtc)) + " " + zoneName(mRow[r].zone, mUtc);
            }

            void refreshRow(int r, bool writeFields)
            {
                Row& row = mRow[r];
                if (writeFields)
                {
                    const Civil c = toLocal(row.zone, mUtc);
                    setField(row.field[F_YEAR],   std::to_string(c.year));
                    setField(row.field[F_MONTH],  two(c.month));
                    setField(row.field[F_DAY],    two(c.day));
                    setField(row.field[F_HOUR],   two(c.hour));
                    setField(row.field[F_MINUTE], two(c.minute));
                    for (int f = 0; f < F_COUNT; ++f)
                    {
                        row.field[f]->SetClass("bad", false);
                    }
                }
                setText(row.info, infoText(r));
            }

            // "SL 2026/10/24 07:00 PDT = UTC 14:00 = JST 23:00" (a date after the time when it differs)
            void updateSummary()
            {
                std::string& s = mSummaryBuf; // kept, so the text is built in the same memory each time
                s.clear();
                int baseMonth = 0, baseDay = 0;
                for (int r = 0; r < mRows; ++r)
                {
                    const Civil c = toLocal(mRow[r].zone, mUtc);
                    if (r == 0)
                    {
                        baseMonth = c.month;
                        baseDay = c.day;
                        s = getString("name_sl") + " " + std::to_string(c.year) + "/" + two(c.month) + "/" + two(c.day) + " " +
                            two(c.hour) + ":" + two(c.minute) + " " + zoneName(mRow[r].zone, mUtc);
                        continue;
                    }
                    s += " = " + zoneName(mRow[r].zone, mUtc) + " " + two(c.hour) + ":" + two(c.minute);
                    if (c.month != baseMonth || c.day != baseDay)
                    {
                        s += " (" + std::to_string(c.month) + "/" + std::to_string(c.day) + ")";
                    }
                }
                setText(mSummary, s);
            }

            // skipRow: the row being typed in, whose fields are left alone.
            void refreshAll(int skipRow)
            {
                for (int r = 0; r < mRows; ++r)
                {
                    refreshRow(r, r != skipRow);
                }
                updateSummary();
            }

            std::vector<RowInfo> mRowInfo;
            std::vector<FieldInfo> mFieldInfo;
            std::vector<ZoneChoice> mChoices;
            Rml::DataModelHandle mHandle;
            Row mRow[MAX_ROWS];
            TextCell mNote;
            TextCell mSummary;
            Rml::Element* mAddRow = nullptr;
            std::string mSummaryBuf;
            std::string mFocusText;         // the focused field's text when it got the focus (commit and Esc compare with it)
            Rml::ElementFormControlInput* mFocusBox = nullptr;          // the field that has the focus (null: none of ours)
            std::string mPrev[MAX_ROWS][F_COUNT];                       // each field's text before the change being looked at
            int mRows = 3;
            Minutes mUtc = 0;
            bool mTick = true;
            bool mReady = false;            // the document is loaded and the pointers above are valid
            bool mWriting = false;          // this class is writing a field or select: its change event is not the user's
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_time_conv_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<TimeConvRmlFloater>);
    }
}
