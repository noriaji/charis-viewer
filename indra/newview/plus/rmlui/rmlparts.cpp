/**
 * @file rmlparts.cpp
 * @brief The host's parts for RmlUi windows (research/387; see rmlparts.h for how a window uses them).
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

#include "rmlparts.h"

#include "rmlhost.h"
#include "rmlnumber.h"
#include "rmltextmath.h"

#include "llstring.h"
#include "lltimer.h"
#include "llui.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace plus::rmlui
{
    namespace
    {
        constexpr char READONLY_ATTRIBUTE[] = "readonly";
        constexpr char SELECT_ON_FOCUS_ATTRIBUTE[] = "select-on-focus";
        constexpr int SCROLL_END_TRIES = 2;
        constexpr int CLIP_LOOKUP_LEVELS = 3;       // the hit element and two parents

        // The number box (research/387 3.5).
        constexpr char SPIN_CLASS[] = "ps-spin";
        constexpr char SPIN_UP_CLASS[] = "up";
        constexpr char SPIN_DOWN_CLASS[] = "down";
        constexpr char SPIN_SHOWN_ATTRIBUTE[] = "spin-shown";     // the text the field showed when last committed or focused
        constexpr char SPIN_EDITED_ATTRIBUTE[] = "spin-edited";   // the user typed in the field since it got the focus or a commit
        constexpr char SPIN_COMMIT_EVENT[] = "commit";
        constexpr char SPIN_WHEEL_ATTRIBUTE[] = "wheel";          // wheel="focus": only while the field has the focus
        constexpr int SPIN_LOOKUP_LEVELS = 4;       // from a hit element up to its .ps-spin
        constexpr F64 SPIN_REPEAT_DELAY = 0.5;      // seconds held before the repeat starts
        constexpr F64 SPIN_REPEAT_RATE = 60.0;      // repeat steps a second
        constexpr S32 SPIN_MAX_STEPS_PER_FRAME = 4; // a stalled frame does not leap
        constexpr S32 SPIN_MAX_WHEEL_STEPS = 10;

        // The .ps-spin element a hit element is in (or is), or null.
        Rml::Element* spinOf(Rml::Element* element)
        {
            for (int level = 0; element && level < SPIN_LOOKUP_LEVELS; ++level, element = element->GetParentNode())
            {
                if (element->IsClassSet(SPIN_CLASS))
                {
                    return element;
                }
            }
            return nullptr;
        }

        Rml::ElementFormControl* findSpinField(Rml::Element* parent, int depth)
        {
            for (int i = 0; i < parent->GetNumChildren(); ++i)
            {
                Rml::Element* child = parent->GetChild(i);
                if (fieldKind(child) == FieldKind::Text && child->GetTagName() == "input")
                {
                    return rmlui_dynamic_cast<Rml::ElementFormControl*>(child);
                }
                if (depth > 0)
                {
                    if (Rml::ElementFormControl* field = findSpinField(child, depth - 1))
                    {
                        return field;
                    }
                }
            }
            return nullptr;
        }

        // The text field of a .ps-spin (a direct child, or one level down in a wrapper).
        Rml::ElementFormControl* spinField(Rml::Element* spin)
        {
            return spin ? findSpinField(spin, 1) : nullptr;
        }
    }

    FieldKind fieldKind(const Rml::Element* element)
    {
        if (!element)
        {
            return FieldKind::None;
        }
        const Rml::String& tag = element->GetTagName();
        const bool read_only = element->HasAttribute(READONLY_ATTRIBUTE);
        if (tag == "textarea")
        {
            return read_only ? FieldKind::ReadOnly : FieldKind::Text;
        }
        if (tag != "input")
        {
            return FieldKind::None;
        }
        // RmlUi makes a text field of every type it does not know (ElementFormControlInput::OnAttributeChange): so does this.
        const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
        if (type == "checkbox" || type == "radio" || type == "range" || type == "submit" || type == "button")
        {
            return FieldKind::None;
        }
        if (read_only)
        {
            return FieldKind::ReadOnly;
        }
        return type == "password" ? FieldKind::Password : FieldKind::Text;
    }

    void selectAll(Rml::Element* element)
    {
        // (RmlUi selects only while the field has the focus: WidgetTextInput::SetSelectionRange)
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element))
        {
            input->Select();
        }
        else if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element))
        {
            area->Select();
        }
    }

    bool selectionRange(Rml::Element* element, int& start, int& end)
    {
        start = 0;
        end = 0;
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element))
        {
            input->GetSelection(&start, &end, nullptr);
            return true;
        }
        if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element))
        {
            area->GetSelection(&start, &end, nullptr);
            return true;
        }
        return false;
    }

    void setSelectionRange(Rml::Element* element, int start, int end)
    {
        // (RmlUi sets a range only while the field has the focus: WidgetTextInput::SetSelectionRange)
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element))
        {
            input->SetSelectionRange(start, end);
        }
        else if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element))
        {
            area->SetSelectionRange(start, end);
        }
    }

    bool hasSelection(Rml::Element* element)
    {
        int start = 0;
        int end = 0;
        return selectionRange(element, start, end) && start != end;
    }

    Rml::Element* clippedTextBox(Rml::Element* hit, std::string& whole)
    {
        Rml::Element* element = hit;
        for (int level = 0; element && level < CLIP_LOOKUP_LEVELS; ++level, element = element->GetParentNode())
        {
            const Rml::ComputedValues& values = element->GetComputedValues();
            if (values.text_overflow() == Rml::Style::TextOverflow::Clip || values.overflow_x() == Rml::Style::Overflow::Visible)
            {
                continue;   // RmlUi cuts a text only inside a box that clips (ElementText.cpp ResolveTextOverflow)
            }
            // The width below is the raw text's (kerning and letter-spacing are the box's own: GetStringWidth). A box
            // that wraps, or changes the case of its text, shows another width: it is not measured (no wrong tool tip).
            if ((values.white_space() != Rml::Style::WhiteSpace::Nowrap && values.white_space() != Rml::Style::WhiteSpace::Pre) ||
                values.text_transform() != Rml::Style::TextTransform::None)
            {
                continue;
            }
            // Only text nodes inside (a <span> would be the text's parent, and the box would not cut it).
            // (this runs every frame the mouse is over the window: the measuring makes no string, the text is joined
            // only for a box that is cut)
            const int count = element->GetNumChildren();
            float text_width = 0.f;
            bool only_text = count > 0;
            for (int i = 0; i < count && only_text; ++i)
            {
                if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i)))
                {
                    text_width += (float)Rml::ElementUtilities::GetStringWidth(element, text->GetText());
                }
                else
                {
                    only_text = false;
                }
            }
            const float box_width = element->GetBox().GetSize(Rml::BoxArea::Content).x;
            if (!only_text || text_width <= 0.f || box_width <= 0.f || text_width <= box_width + 0.5f)
            {
                continue;   // not a text box, or it fits: nothing is cut, so no tool tip
            }
            whole.clear();
            for (int i = 0; i < count; ++i)
            {
                whole += static_cast<Rml::ElementText*>(element->GetChild(i))->GetText();    // (all of them are text: above)
            }
            return element;
        }
        return nullptr;
    }

    void Parts::attach(LLRmlView& host, Rml::ElementDocument& document)
    {
        detach();
        mHost = &host;
        mDocument = &document;
        document.AddEventListener("focus", this, true);
        document.AddEventListener("blur", this, true);
        document.AddEventListener("mousedown", this, true);
        document.AddEventListener("mousedown", this, false);    // the right press: still there when nobody stopped it
        document.AddEventListener("drag", this, true);
        document.AddEventListener("keydown", this, true);
        document.AddEventListener("mousescroll", this, true);
        document.AddEventListener("change", this, false);
    }

    void Parts::detach()
    {
        // (the document is still the host's here: unloadNow calls this before the context goes)
        if (mDocument && mHost && mHost->getDocument() == mDocument)
        {
            mDocument->RemoveEventListener("focus", this, true);
            mDocument->RemoveEventListener("blur", this, true);
            mDocument->RemoveEventListener("mousedown", this, true);
            mDocument->RemoveEventListener("mousedown", this, false);
            mDocument->RemoveEventListener("drag", this, true);
            mDocument->RemoveEventListener("keydown", this, true);
            mDocument->RemoveEventListener("mousescroll", this, true);
            mDocument->RemoveEventListener("change", this, false);
        }
        mHost = nullptr;
        mDocument = nullptr;
        mScrollEnd.clear();
        mScrollRun.clear();
        mJustFocused.reset();
        mJustBlurred.reset();
        mRightClickField.reset();
        mRightSelection.element.reset();
        mFocusTarget.reset();
        pressEnded();
    }

    void Parts::pressEnded()
    {
        mHeldSpin.reset();
        mHeldDirection = 0;
        mStepsDue = 0;
        mNoDrag.reset();
    }

    Rml::ObserverPtr<Rml::Element> Parts::takeRightClickField()
    {
        Rml::ObserverPtr<Rml::Element> field = mRightClickField;
        mRightClickField.reset();
        mRightSelection.element.reset();
        return field;
    }

    void Parts::ProcessEvent(Rml::Event& event)
    {
        switch (event.GetId())
        {
        case Rml::EventId::Focus:
            onFocus(event);
            break;
        case Rml::EventId::Blur:
            onBlur(event);
            break;
        case Rml::EventId::Mousedown:
            onMouseDown(event);
            break;
        case Rml::EventId::Drag:
            // The press that gave a select-on-focus field the focus keeps its selection: no drag selects from it.
            if (event.GetTargetElement() && event.GetTargetElement() == mNoDrag.get())
            {
                event.StopPropagation();
            }
            break;
        case Rml::EventId::Keydown:
            onKeyDown(event);
            break;
        case Rml::EventId::Mousescroll:
            onMouseScroll(event);
            break;
        case Rml::EventId::Change:
            onChange(event);
            break;
        default:
            break;
        }
    }

    void Parts::endInput()
    {
        mJustBlurred.reset();
        mFocusTarget.reset();
        selectJustFocused();
    }

    void Parts::selectJustFocused()
    {
        Rml::Element* field = mJustFocused.get();
        mJustFocused.reset();   // first: nothing below may come back here with it
        if (!field || !mHost || !mHost->getContext() || mHost->getContext()->GetFocusElement() != field)
        {
            return;     // gone, or the focus moved on within the same input
        }
        selectAll(field);
        mHost->markDirty();
    }

    void Parts::onFocus(Rml::Event& event)
    {
        // Every element of the new focus chain gets its own focus event (Context::OnFocusChange): the target is the one.
        Rml::Element* target = event.GetTargetElement();
        // Noted for the IME's bridge: Context::focus is still the element that LOST the focus until every focus event is
        // out, and the field's own listener calls OnActivate inside its event. This listener of the document runs in the
        // capture pass of that same event, so the note is the field's unless a listener of the window between the two
        // moves the focus again (the nested event overwrites it: OnActivate checks the note against the field's box and
        // lets the IME go when they differ, and a window must not move the focus in a focus listener).
        mFocusTarget = target ? target->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        if (target && target->HasAttribute(SELECT_ON_FOCUS_ATTRIBUTE) && isSelectable(fieldKind(target)))
        {
            mJustFocused = target->GetObserverPtr();
        }
        // The number box's field: what Esc puts back is what it showed when it got the focus (a window whose setting can
        // change from outside keeps the attribute at the setting's value: data-attr-spin-shown). Nothing is typed yet.
        Rml::Element* spin = spinOf(target);
        if (Rml::ElementFormControl* field = spinField(spin); field && field == target)
        {
            target->SetAttribute(SPIN_SHOWN_ATTRIBUTE, field->GetValue());
            target->RemoveAttribute(SPIN_EDITED_ATTRIBUTE);
        }
    }

    void Parts::onBlur(Rml::Event& event)
    {
        Rml::Element* target = event.GetTargetElement();
        // The element that had the focus (it still is the context's focus while the blur events go out; every element
        // of the old focus chain hears one, the focus itself is the one noted). An arrow press of a number box gives
        // it back (spinPress) with its selection.
        Rml::Context* context = mHost ? mHost->getContext() : nullptr;
        if (target && context && target == context->GetFocusElement() && target != target->GetOwnerDocument() &&
            target != context->GetRootElement())
        {
            mJustBlurred = target->GetObserverPtr();
            selectionRange(target, mBlurStart, mBlurEnd);
        }
        Rml::Element* spin = spinOf(target);
        if (spin && spinField(spin) == target)
        {
            // The field of a number box lost the focus: its text is committed when the user typed in it (the XUI spin
            // control's lost-focus: a field that was not edited commits nothing and shows the setting again, which a change
            // from outside meanwhile has made a new one).
            if (target->HasAttribute(SPIN_EDITED_ATTRIBUTE))
            {
                Rml::ElementFormControl* field = spinField(spin);
                if (field && field->IsDisabled())
                {
                    // A field made disabled while it was typed in (RmlUi blurs it; nothing is committed): the text the box
                    // showed last comes back.
                    field->SetValue(field->GetAttribute<Rml::String>(SPIN_SHOWN_ATTRIBUTE, field->GetValue()));
                }
                else
                {
                    spinCommit(spin);
                }
            }
            target->RemoveAttribute(SPIN_EDITED_ATTRIBUTE);
        }
    }

    void Parts::onMouseDown(Rml::Event& event)
    {
        const bool capture = event.GetPhase() == Rml::EventPhase::Capture;
        const int button = event.GetParameter<int>("button", 0);
        if (button == 1)
        {
            onRightMouseDown(event, capture);
            return;
        }
        if (!capture || button != 0)
        {
            return;
        }
        mNoDrag.reset();
        // An arrow of a number box.
        if (Rml::Element* target = event.GetTargetElement())
        {
            int direction = 0;
            Rml::Element* element = target;
            for (int level = 0; element && level < SPIN_LOOKUP_LEVELS && !element->IsClassSet(SPIN_CLASS);
                 ++level, element = element->GetParentNode())
            {
                if (direction == 0)
                {
                    direction = element->IsClassSet(SPIN_UP_CLASS) ? 1 : (element->IsClassSet(SPIN_DOWN_CLASS) ? -1 : 0);
                }
            }
            if (direction != 0 && element && element->IsClassSet(SPIN_CLASS))
            {
                spinPress(event, element, direction);
                return;
            }
        }
        // The press that gave a select-on-focus field the focus: endInput selects all once the press is done (after the
        // field put its caret under the mouse), and no drag of this press selects from the caret. The press goes on
        // as any other: a second one soon after is a double click, which selects the word (as the XUI's).
        if (Rml::Element* target = event.GetTargetElement(); target && target == mJustFocused.get())
        {
            mNoDrag = target->GetObserverPtr();
        }
    }

    void Parts::onRightMouseDown(Rml::Event& event, bool capture)
    {
        // The right press: the capture pass forgets the last one and notes the selection the field has (RmlUi puts the
        // caret under the mouse for any button, which clears it); one that comes back in the bubble pass was not
        // stopped by any element of the window (a window's own menu stops it), so the host may offer the edit menu.
        Rml::Element* target = event.GetTargetElement();
        if (capture)
        {
            mRightClickField.reset();
            mRightSelection.element.reset();
            Rml::Context* context = mHost ? mHost->getContext() : nullptr;
            if (target && context && context->GetFocusElement() == target && isSelectable(fieldKind(target)))
            {
                int start = 0;
                int end = 0;
                if (selectionRange(target, start, end) && start != end)
                {
                    mRightSelection = { target->GetObserverPtr(), start, end };
                }
            }
        }
        else if (target && isSelectable(fieldKind(target)))
        {
            mRightClickField = target->GetObserverPtr();
            // The field's own handler has put the caret at the point: a point inside the selection the field had keeps
            // that selection (the XUI field does the same), one outside leaves the caret there. (The host gives RmlUi
            // this press without Shift: with Shift RmlUi would grow the selection to the point.)
            if (mRightSelection.element.get() == target)
            {
                int caret = 0;
                int caret_end = 0;
                if (selectionRange(target, caret, caret_end) && caret == caret_end && caret >= mRightSelection.start &&
                    caret <= mRightSelection.end)
                {
                    setSelectionRange(target, mRightSelection.start, mRightSelection.end);
                    if (mHost)
                    {
                        mHost->markDirty();
                    }
                }
            }
        }
    }

    void Parts::afterUpdate()
    {
        if (mScrollEnd.empty())
        {
            return;
        }
        // (SetScrollTop sends a scroll event: a window's listener may call setLogText, which adds to mScrollEnd. The
        // list that is walked is a work list, so nothing is added to it during the walk.)
        mScrollRun.swap(mScrollEnd);
        for (PendingScroll& pending : mScrollRun)
        {
            if (Rml::Element* element = pending.element.get())
            {
                const float max_top = std::max(0.f, element->GetScrollHeight() - element->GetClientHeight());
                element->SetScrollTop(max_top);
            }
            --pending.left;
        }
        for (PendingScroll& pending : mScrollRun)
        {
            if (pending.left <= 0 || !pending.element.get())
            {
                continue;
            }
            auto again = std::find_if(mScrollEnd.begin(), mScrollEnd.end(),
                                      [&](const PendingScroll& other) { return other.element == pending.element; });
            if (again == mScrollEnd.end())
            {
                mScrollEnd.push_back(pending);      // the second try (a listener's own new one is the same or newer)
            }
        }
        mScrollRun.clear();
        if (mHost)
        {
            mHost->markDirty();     // the scroll bar's thumb, and the second try's update
        }
    }

    void Parts::scrollReadOnly(Rml::Element* field, KEY key)
    {
        if (!field)
        {
            return;
        }
        const float line = std::max(1.f, field->GetLineHeight());
        const float page = std::max(line, field->GetClientHeight() - line);
        const float max_top = std::max(0.f, field->GetScrollHeight() - field->GetClientHeight());
        float top = field->GetScrollTop();
        switch (key)
        {
        case KEY_UP: case KEY_PAD_UP:           top -= line; break;
        case KEY_DOWN: case KEY_PAD_DOWN:       top += line; break;
        case KEY_PAGE_UP: case KEY_PAD_PGUP:    top -= page; break;
        case KEY_PAGE_DOWN: case KEY_PAD_PGDN:  top += page; break;
        case KEY_HOME: case KEY_PAD_HOME:       top = 0.f; break;
        case KEY_END: case KEY_PAD_END:         top = max_top; break;
        default: return;
        }
        field->SetScrollTop(std::min(std::max(top, 0.f), max_top));
        if (mHost)
        {
            mHost->markDirty();
        }
    }

    void Parts::setLogText(Rml::Element* textarea, const std::string& text, Follow follow)
    {
        auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(textarea);
        if (!area || !mHost)
        {
            return;
        }
        // What RmlUi keeps of the text: no CR, no TAB (it erases them and sends a change event, so a text that has them
        // would never equal the field's value, and a compare-then-set loop would never settle), and not more than
        // MAX_LOG_BYTES (the start of it; a log that follows its end keeps the end).
        const bool plain = isPlainLogText(text, MAX_LOG_BYTES);
        std::string fixed;
        if (!plain)
        {
            fixed = logFieldText(text, MAX_LOG_BYTES, follow == Follow::End);
        }
        const std::string& value = plain ? text : fixed;
        if (area->GetValue() == value)
        {
            return;
        }
        area->SetValue(value);      // plain text: never read as RML (research/364 2.5)
        if (follow == Follow::End)
        {
            const Rml::ObserverPtr<Rml::Element> pointer = area->GetObserverPtr();
            auto same = std::find_if(mScrollEnd.begin(), mScrollEnd.end(),
                                     [&](const PendingScroll& pending) { return pending.element == pointer; });
            if (same != mScrollEnd.end())
            {
                same->left = SCROLL_END_TRIES;
            }
            else
            {
                mScrollEnd.push_back({ pointer, SCROLL_END_TRIES });
            }
        }
        mHost->markDirty();
    }

    void Parts::onKeyDown(Rml::Event& event)
    {
        // Up, Down and Esc in the field of a number box (the key the XUI spin control takes when its field has the focus).
        Rml::Element* target = event.GetTargetElement();
        Rml::Element* spin = spinOf(target);
        Rml::ElementFormControl* field = spinField(spin);
        if (!field || field != target || field->IsDisabled())
        {
            return;
        }
        const int key = event.GetParameter<int>("key_identifier", 0);
        const bool plain = event.GetParameter<int>("ctrl_key", 0) == 0 && event.GetParameter<int>("alt_key", 0) == 0;
        if (key == Rml::Input::KI_ESCAPE)
        {
            event.StopPropagation();    // not the window's: a window with a number box does not close on Esc in it
            field->SetValue(field->GetAttribute<Rml::String>(SPIN_SHOWN_ATTRIBUTE, field->GetValue()));
            field->RemoveAttribute(SPIN_EDITED_ATTRIBUTE);      // (the blur below commits nothing: the text is the shown one)
            field->Blur();
        }
        else if (plain && (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN))
        {
            event.StopPropagation();    // the caret does not move
            spinStep(spin, key == Rml::Input::KI_UP ? 1 : -1);
        }
    }

    void Parts::onMouseScroll(Rml::Event& event)
    {
        if (event.GetParameter<bool>("autoscroll", false))
        {
            return;     // the middle button's autoscroll, not a wheel
        }
        Rml::Element* spin = spinOf(event.GetTargetElement());
        Rml::ElementFormControl* field = spinField(spin);
        if (!field || field->IsDisabled())
        {
            return;     // (a disabled box lets the wheel scroll what is under it, as a disabled XUI control does)
        }
        if (spin->GetAttribute<Rml::String>(SPIN_WHEEL_ATTRIBUTE, "") == "focus" && mHost && mHost->getContext() &&
            mHost->getContext()->GetFocusElement() != field)
        {
            return;     // wheel="focus": the wheel steps only while the field has the focus; else the panel under it scrolls
        }
        event.StopPropagation();
        // The wheel's clicks: positive is down. One click is one step, whether or not the field has the focus.
        const float delta = event.GetParameter<float>("wheel_delta_y", 0.f);
        const int clicks = std::clamp((int)std::lround(std::fabs(delta)), 1, (int)SPIN_MAX_WHEEL_STEPS);
        const Rml::ObserverPtr<Rml::Element> keep = spin->GetObserverPtr();
        for (int i = 0; i < clicks && keep; ++i)
        {
            spinStep(keep.get(), delta > 0.f ? -1 : 1);
        }
    }

    void Parts::onChange(Rml::Event& event)
    {
        // A key typed in the field of a number box ("change" comes with every key; only an edit of the user's: RmlUi sends none for
        // a SetValue) marks it edited; Enter (linebreak) commits. As the XUI spin control (setSelectAllonCommit(false)) the
        // committed number is not selected.
        Rml::Element* target = event.GetTargetElement();
        Rml::Element* spin = spinOf(target);
        if (!spin || spinField(spin) != target)
        {
            return;
        }
        if (!event.GetParameter<bool>("linebreak", false))
        {
            target->SetAttribute(SPIN_EDITED_ATTRIBUTE, Rml::String());
            return;
        }
        spinCommit(spin);       // (the commit event runs the window's code: nothing is touched after it)
    }

    void Parts::spinPress(Rml::Event& event, Rml::Element* spin, int direction)
    {
        // The press is the box's: no caret, no double click, no drag, and no other listener's click.
        event.StopPropagation();
        Rml::ElementFormControl* field = spinField(spin);
        if (!field)
        {
            return;
        }
        const Rml::ObserverPtr<Rml::Element> keep = spin->GetObserverPtr();
        if (field->IsDisabled())
        {
            giveFocusBack(field);   // a disabled box takes no step, and no focus from what had it
            return;
        }
        // The hold is noted before anything runs the window's code (the focus events, the commit): code that ends the
        // press (a lost mouse capture) then ends this one too, and nothing sets the hold up again after it.
        mHeldSpin = keep;
        mHeldDirection = direction;
        mHeldSince = LLTimer::getTotalSeconds();
        mStepsDue = 0;
        giveFocusBack(field);
        if (!keep || !mHeldSpin)
        {
            pressEnded();   // (the box may be gone, or the press over)
            return;
        }
        spinStep(keep.get(), direction);
    }

    void Parts::giveFocusBack(Rml::ElementFormControl* spin_field)
    {
        // The press moved the focus off the element that had it (RmlUi focuses what is pressed: the box). The arrows of
        // the XUI spin control take no focus (tab_stop false), so the element gets it back, with its selection.
        Rml::Context* context = mHost ? mHost->getContext() : nullptr;
        const Rml::ObserverPtr<Rml::Element> prior = mJustBlurred;
        const int start = mBlurStart;
        const int end = mBlurEnd;
        if (!context || !prior || context->GetFocusElement() == prior.get())
        {
            return;
        }
        if (!prior->Focus() || !prior)
        {
            return;
        }
        if (mJustFocused.get() == prior.get())
        {
            mJustFocused.reset();   // the focus came back, it was not taken: select-on-focus does not select all again
        }
        mJustBlurred.reset();
        if (prior.get() != spin_field && start != end)
        {
            setSelectionRange(prior.get(), start, end);     // (the number box's own field shows a new number)
        }
    }

    bool Parts::tick(F64 now)
    {
        if (!mHeldSpin)
        {
            return false;
        }
        Rml::Element* spin = mHeldSpin.get();
        Rml::ElementFormControl* field = spinField(spin);
        if (!field || field->IsDisabled() || !mHost || !mHost->hasMouseCapture())
        {
            pressEnded();   // gone or disabled while held, or the press is over (the mouse capture was lost)
            return false;
        }
        const F64 held = now - mHeldSince;
        if (held < SPIN_REPEAT_DELAY)
        {
            return false;
        }
        const F64 due_steps = std::min((held - SPIN_REPEAT_DELAY) * SPIN_REPEAT_RATE, 1.0e6) + 1.0;
        const S32 due = (S32)due_steps;
        const S32 todo = std::min(due - mStepsDue, SPIN_MAX_STEPS_PER_FRAME);
        mStepsDue = due;    // steps skipped by a stalled frame are not made up later
        bool changed = false;
        for (S32 i = 0; i < todo && mHeldSpin; ++i)
        {
            // (the commit event runs the window's code: the box may be gone, or the press ended, after each step)
            changed = spinStep(mHeldSpin.get(), mHeldDirection) || changed;
        }
        return changed;
    }

    // Reads the field's text as a number and clamps and rounds it to the box's range, shows it and tells the window.
    // False (and the bad keystroke sound) when the text is no number: the text the box showed last comes back.
    bool Parts::spinCommit(Rml::Element* spin)
    {
        Rml::ElementFormControl* field = spinField(spin);
        if (!field || field->IsDisabled())
        {
            return false;
        }
        const SpinRange range = spinRange(spin);
        double value = 0.0;
        if (!evaluateNumber(normalizeNumberText(field->GetValue()), value))
        {
            make_ui_sound("UISndBadKeystroke");
            field->SetValue(field->GetAttribute<Rml::String>(SPIN_SHOWN_ATTRIBUTE, field->GetValue()));
            field->RemoveAttribute(SPIN_EDITED_ATTRIBUTE);
            return false;
        }
        value = std::clamp(roundTo(value, range.decimals), range.min, range.max);
        spinPublish(spin, field, value, formatNumber(value, range.decimals));
        return true;
    }

    // One step up (+1) or down (-1) from the number in the field (what was typed counts, as in the XUI). True when the
    // value changed (at the end of the range it does not, and nothing is told to the window).
    bool Parts::spinStep(Rml::Element* spin, int direction)
    {
        Rml::ElementFormControl* field = spinField(spin);
        if (!field || field->IsDisabled())
        {
            return false;
        }
        double base = 0.0;
        if (!parsePlainNumber(normalizeNumberText(field->GetValue()), base))
        {
            return false;   // no number in it (the XUI's arrows do nothing then)
        }
        const SpinRange range = spinRange(spin);
        const double next = std::clamp(roundTo(base + direction * range.step, range.decimals), range.min, range.max);
        const std::string shown = formatNumber(next, range.decimals);
        if (shown == formatNumber(roundTo(base, range.decimals), range.decimals) && shown == field->GetValue())
        {
            return false;
        }
        spinPublish(spin, field, next, shown);
        return true;
    }

    void Parts::spinPublish(Rml::Element* spin, Rml::ElementFormControl* field, double value, const std::string& text)
    {
        if (field->GetValue() != text)
        {
            field->SetValue(text);
        }
        field->SetAttribute(SPIN_SHOWN_ATTRIBUTE, text);
        field->RemoveAttribute(SPIN_EDITED_ATTRIBUTE);      // what is in the field is what was told
        Rml::Dictionary parameters;
        parameters["value"] = Rml::Variant(value);
        parameters["text"] = Rml::Variant(text);
        spin->DispatchEvent(SPIN_COMMIT_EVENT, parameters);     // the window's code runs: nothing is touched after this
        if (mHost)
        {
            mHost->markDirty();
        }
    }

    void setLogText(LLRmlView& host, Rml::Element* textarea, const std::string& text, Follow follow)
    {
        host.parts().setLogText(textarea, text, follow);
    }
}
