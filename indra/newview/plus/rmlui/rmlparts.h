/**
 * @file rmlparts.h
 * @brief The host's parts for RmlUi windows: what RmlUi 6.3 lacks and the XUI's fields have (research/387).
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

// HOW A WINDOW USES THE PARTS (nothing of this needs C++ in the window, except where it says so)
//
// The host (LLRmlView) owns one Parts. It puts a few listeners on the whole document when it is loaded and picks
// what to do from the attributes and classes below, so a field made later (a row of a list, a data-for) works too.
//
// 1. A read-only text that can be selected and copied (a log, a long message):
//        <textarea id="log" class="ps-readonly" readonly></textarea>
//    It takes no characters, no BackSpace / Delete / Enter, no cut, paste or delete (the edit menu either), no IME;
//    the arrows, Page Up / Down, Home / End scroll it, Shift+arrows select, Ctrl+A / Ctrl+C work, any other key goes on
//    to the viewer (W walks; Esc closes the floater, F1 to F12 are the viewer's), as the XUI's read-only text editor.
//    The window puts the text in with
//        plus::rmlui::setLogText(*host, textarea, text, Follow::End);    // End: scroll to the end; Keep: do not move
//    which does nothing when the text is the same (call it when the log changed, not for free every frame: it
//    compares the whole text). The text is plain text (never RML); a CR is dropped and a TAB is a space (RmlUi's
//    field takes neither), and a text over MAX_LOG_BYTES is cut (the start; Follow::End keeps the end). Any <input>
//    or <textarea> with the attribute "readonly" is read-only (the presence of the attribute counts, as in HTML;
//    data-attrif-readonly switches it; the IME's allowance follows at the field's next focus).
//
// 2. Select all when the field gets the focus (a search or filter field, as the XUI's select_on_focus):
//        <input type="text" class="text" select-on-focus/>
//    The text is selected when the field gets the focus by a click, Tab, the window coming back or the window's
//    own Focus(); the click that gives the focus puts no caret and starts no drag. A click on a field that already has
//    the focus is an ordinary click, and a second click soon after the first selects the word (a double click, as the
//    XUI's). (A window's own "focus" listener that calls Select() does not work: RmlUi's field has not taken the
//    focus yet when it runs, and the click's mouse down puts the caret over it.)
//
// 3. A long name that is cut with "..." (the RCSS text-overflow) and shows whole in a tool tip, only when it is cut:
//        .ps-ellipsis { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }     (charis.rcss)
//        <div class="ps-ellipsis">{{ name }}</div>
//    The text must be the box's direct child (not inside a <span>: RmlUi cuts the text by its parent's box), and
//    the box needs a width (a block in a column, a fixed or flexible width). Colour, bold and italic go on the box.
//    Only a box with white-space nowrap (or pre) and no text-transform is looked at (the width is measured from the
//    text as it is); give another kind of box a title="" of its own.
//    The tool tip comes from the host with no code in the window; a title="" on the box or above it comes first.
//    (The full text is read back from the document: nothing is read as RML.)
//
// 4. A right click on any field (not only the read-only one) opens the official text editor menu (Cut, Copy, Paste,
//    Delete, Select All; what the field cannot do is greyed), as the XUI's fields do. A point inside the selection
//    the field has keeps it (Cut and Copy work on it); a point outside, or a field with no selection, puts the caret
//    there (RmlUi does that for any button; the selection is put back when the point is inside it). The host gives
//    RmlUi a right press without Shift (the XUI menu does not look at Shift), so Shift does not grow the selection.
//    A field that had no focus takes it (a select-on-focus one selects all first). The window needs no code. A window with a menu of its own for the right press (a list
//    row) listens to "mousedown" (button 1) and calls event.StopPropagation() - then the host leaves the press to
//    it; the menu is opened with rmlmenu.h.
//
// 5. A number box (the XUI's LLSpinCtrl): a text field with two arrows. The look is charis.rcss's .ps-spin.
//        <div class="ps-spin" id="range" min="10" max="4096" step="10" decimals="0">
//            <input type="text" class="text" maxlength="16" ime="off" data-attr-value="range"/>
//            <div class="arrows"><button class="up"/><button class="down"/></div>
//        </div>
//    The arrows step once when pressed, and after 0.5 s held 60 steps a second (not tied to the frame rate), until the
//    mouse is released anywhere. The wheel over the box (a box with wheel="focus": only while its field has the
//    focus, else the panel under it scrolls, as the photo screen's PsSpinner) and Up / Down in the field step by one;
//    Esc puts back the text the field showed when it got the focus or was last committed (and leaves the field, the
//    window stays open: the attribute spin-shown of the field, which a window whose setting can change from outside keeps
//    at the setting's value with data-attr-spin-shown); Enter commits, and so does leaving the field when the user typed
//    in it (a field not typed in commits nothing: it shows the setting again, see rmlsettings.h Int): the text is
//    evaluated (an expression such as 10*3 works, full-width digits too), clamped to min and max,
//    rounded to decimals and shown (not selected, as the XUI's); a text that is no number plays the bad keystroke sound and is put back. A press on
//    an arrow does not take the focus from the field that has it (that field, or any other: it gets the focus back,
//    with its selection, after the press has moved it). A disabled field takes no step.
//    The window gets the "commit" event on the .ps-spin element (it bubbles; event.GetParameter<double>("value"),
//    <std::string>("text")), on every commit and every step that changed the value, and writes its setting then:
//        host->listen(document.GetElementById("range"), "commit");
//    Show the setting with data-attr-value on the field (never data-value: it writes at every key typed). The window
//    keeps the setting's own range; min, max, step and decimals are read at each step. A setting that can change from
//    outside: a SettingsModel Int entry (rmlsettings.h) with data-attr-value="x" data-attr-spin-shown="x_now" on the field and
//    the window's focus and blur listeners on the field calling the model's beginEdit / endEdit, so that the field being
//    typed in keeps its text until it is left, as the XUI spin control.
//
// 6. Every field takes the kind of field it is from fieldKind(): <textarea>, and <input> of any type but checkbox,
//    radio, range, submit, button (so type="number" and unknown types are text fields, as RmlUi makes them).
//
// 7. A window's "focus" listener (host->listen(field, "focus")) must not move the focus (field->Focus() of another
//    element): the IME's bridge learns which field got the focus from the event, and a nested move in the middle of it
//    leaves the IME off for that field. If the window must move the focus, it does it after the input (the next
//    frame's update, or the window's own idle), not inside the event.

#ifndef PLUS_RMLPARTS_H
#define PLUS_RMLPARTS_H

#include "rmlcommon.h"

#include "indra_constants.h"
#include "rmlkeyroute.h"

#include <string>
#include <vector>

class LLRmlView;

namespace plus::rmlui
{
    // What an element is to the keyboard and the edit menu.
    enum class FieldKind
    {
        None,       // not a field (a button, a check box, a div ...)
        Text,       // takes characters
        Password,   // takes characters; no copy or cut out of it, no IME, no text in the IME's copy
        ReadOnly,   // the attribute readonly: selects and copies, takes no characters
    };
    // The one place that tells a text field from the rest (the host, the IME's bridge and the clipboard ask here).
    FieldKind fieldKind(const Rml::Element* element);
    inline bool isTypable(FieldKind kind) { return kind == FieldKind::Text || kind == FieldKind::Password; }
    inline bool isSelectable(FieldKind kind) { return kind != FieldKind::None; }
    // The field's selection (character offsets; start == end is a caret). False, and 0 and 0, for a non-field.
    bool selectionRange(Rml::Element* element, int& start, int& end);
    // Sets the selection of a focused field (RmlUi ignores it for a field without the focus).
    void setSelectionRange(Rml::Element* element, int start, int end);
    // The field has a selection (not a caret only). False for a non-field.
    bool hasSelection(Rml::Element* element);
    // Selects all the text of a focused field (nothing for another element, or a field without the focus).
    void selectAll(Rml::Element* element);

    // The element (the hit element or one of its two parents) whose text is cut short by text-overflow, with the
    // whole text in whole; null when the text under the mouse is not cut or the box does not qualify (see 3. above).
    Rml::Element* clippedTextBox(Rml::Element* hit, std::string& whole);

    // KeyRoute and readOnlyKeyRoute(key, mask): rmlkeyroute.h.

    // Where setLogText leaves the scroll position.
    enum class Follow
    {
        Keep,   // where it is
        End,    // at the end of the text, after the next layout
    };

    // The host's listeners and work. Events are heard once, on the whole document (capture phase, which runs before
    // the field's own listeners: Rml::EventDispatcher), so fields made later are covered.
    class Parts final : public Rml::EventListener
    {
    public:
        // The host loaded its document / is about to remove the context. detach() is safe to call twice.
        void attach(LLRmlView& host, Rml::ElementDocument& document);
        void detach();
        // The end of the host's outermost Scope (one input, one frame's update): what waits for the input to be
        // complete is done now (select on focus: the field's own focus handling has run by now). Inside the Scope.
        void endInput();
        // The select-all a select-on-focus field is waiting for (what endInput does for it), now: a caller that needs the
        // field selected before the input is over (the right-click menu decides its items at once).
        void selectJustFocused();
        // The element the latest focus event was sent to: when a field's own focus listener runs (the IME's OnActivate)
        // that is the field that is getting the focus (Context::GetFocusElement() is still the old one), as long as no
        // listener of a window between the two moved the focus again (the IME's bridge checks it against the field's box).
        // Null outside an input.
        Rml::Element* focusTarget() const { return mFocusTarget.get(); }
        // Each frame the press is held (the host's handleHover, which the viewer calls every frame for the view that has
        // the mouse capture, as for an XUI button; inside the host's Scope): the held arrow's repeating steps. True when a
        // step changed a value (the host updates). A press whose mouse capture is gone ends here.
        bool tick(F64 now);
        // The mouse button went up, the mouse capture was lost, the window hid: no arrow is held any more.
        void pressEnded();
        // After the host's Context::Update (inside its Scope): the "scroll to the end" of setLogText.
        void afterUpdate();
        // A read-only field's scrolling key (readOnlyKeyRoute said Scroll). The host's Scope is open.
        void scrollReadOnly(Rml::Element* field, KEY key);
        // setLogText()'s work.
        void setLogText(Rml::Element* textarea, const std::string& text, Follow follow);

        // The field a right press landed on that no listener of the window stopped (the host then offers the edit
        // menu); null otherwise. Taking it clears it.
        Rml::ObserverPtr<Rml::Element> takeRightClickField();

        void ProcessEvent(Rml::Event& event) override;

    private:
        void onFocus(Rml::Event& event);
        void onBlur(Rml::Event& event);
        void onMouseDown(Rml::Event& event);
        void onRightMouseDown(Rml::Event& event, bool capture);
        void onKeyDown(Rml::Event& event);
        void onMouseScroll(Rml::Event& event);
        void onChange(Rml::Event& event);
        // The number box's work (research/387 3.5). spin is the .ps-spin element.
        void spinPress(Rml::Event& event, Rml::Element* spin, int direction);
        void giveFocusBack(Rml::ElementFormControl* spin_field);
        bool spinCommit(Rml::Element* spin);
        bool spinStep(Rml::Element* spin, int direction);
        void spinPublish(Rml::Element* spin, Rml::ElementFormControl* field, double value, const std::string& text);

        // A scroll to the end waiting for the layout (done twice: the first update may not have the new height yet).
        struct PendingScroll
        {
            Rml::ObserverPtr<Rml::Element> element;
            int left = 0;
        };

        LLRmlView* mHost = nullptr;
        Rml::ElementDocument* mDocument = nullptr;
        std::vector<PendingScroll> mScrollEnd;
        std::vector<PendingScroll> mScrollRun;      // afterUpdate's work list (a window's listener may add to mScrollEnd)
        // The select-on-focus field that took the focus in the input being processed (cleared at endInput).
        Rml::ObserverPtr<Rml::Element> mJustFocused;
        // The target of the latest focus event of the input being processed (see focusTarget()).
        Rml::ObserverPtr<Rml::Element> mFocusTarget;
        // The element that lost the focus in the input being processed, with its selection then (an arrow press of a
        // number box gives it back).
        Rml::ObserverPtr<Rml::Element> mJustBlurred;
        int mBlurStart = 0;
        int mBlurEnd = 0;
        // The select-on-focus field whose focusing press is held: no drag of it selects (cleared when the press ends).
        Rml::ObserverPtr<Rml::Element> mNoDrag;
        // The arrow held down: its .ps-spin, the direction (+1 up, -1 down), when it went down (LLTimer seconds), how
        // many repeat steps were due so far.
        Rml::ObserverPtr<Rml::Element> mHeldSpin;
        int mHeldDirection = 0;
        F64 mHeldSince = 0.0;
        S32 mStepsDue = 0;
        // The last right press's field, set when the press reached the document's bubble phase unstopped.
        Rml::ObserverPtr<Rml::Element> mRightClickField;
        // The selection the focused field had when a right press began (RmlUi's own press clears it).
        struct RightSelection
        {
            Rml::ObserverPtr<Rml::Element> element;
            int start = 0;
            int end = 0;
        };
        RightSelection mRightSelection;
    };

    // The window's call: textarea is an element of the window's document (ids come from RML; null and non-text-areas
    // do nothing). Text over MAX_LOG_BYTES is cut (at a character).
    constexpr size_t MAX_LOG_BYTES = 1u << 20;
    void setLogText(LLRmlView& host, Rml::Element* textarea, const std::string& text, Follow follow);
}

#endif // PLUS_RMLPARTS_H
