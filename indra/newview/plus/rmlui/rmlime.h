/**
 * @file rmlime.h
 * @brief The bridge between the window's IME (LLPreeditor) and RmlUi's text fields (research/364 2.2.3).
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

// On Windows the IME is IMM32 in llwindowwin32.cpp, through the one LLPreeditor the window holds
// (allowLanguageTextInput). RmlUi tells a text field's focus to its Rml::TextInputHandler, and edits the
// field through the Rml::TextInputContext it hands over. RmlPreeditor is both ends, one for the viewer:
//
//   RmlUi (main thread, inside a host's work)       the bridge
//     OnActivate(field)                              keeps the field, its host and element (the element is the
//                                                    target of the focus event, from Parts::focusTarget(): the
//                                                    context's focus is still the old one inside the event; checked
//                                                    against the field's box); asks the window for the IME (only for
//                                                    a plain text field without ime="off": imeAllowed; not a
//                                                    password or read-only one, nor without the host's keyboard
//                                                    focus); the host takes the edit menu (with the focus)
//     hostFocusChanged (the host's focus received    the same, for a field active before the host had the focus
//                       or lost)                     (allowed), or kept when the host's blur did not reach the field
//                                                    (let go)
//     OnDeactivate(field)                            ends a composition (drops it; keeps its text in a field with
//                                                    ime-blur="keep"), lets the IME and the edit menu go
//     OnDestroy(field)                               the same without touching the field (it is going)
//     SystemInterface::ActivateKeyboard              the caret (context pixels) and the line's height (a field that
//                                                    is just getting the focus reports it before OnActivate: kept
//                                                    aside, taken by OnActivate)
//   the window's IME (main thread, posted)
//     updatePreedit(text, ..., caret)                SetText over the composition (first: over the selection, or
//                                                    over a reconversion's word), SetCompositionRange,
//                                                    SetCursorPosition (RmlUi's Win32 way)
//     resetPreedit()                                 the composition removed, the caret at its start
//     handleUnicodeCharHere(c)                       the result, one character after resetPreedit: typed into
//                                                    the field (its maxlength applies)
//                                                    (the window's allow / let go made inside these three, or by what
//                                                    they run, waits for the next idle: its loop over a result still
//                                                    holds the bridge as its preeditor. The idle works out the state
//                                                    that is right then: allowed only for an active field of a host with
//                                                    the keyboard focus that imeAllowed() accepts)
//   the window thread (WM_IME_REQUEST, synchronous)
//     getPreeditRange, getSelectionRange, getPreeditString, getPreeditLocation, getPreeditFontSize
//                                                    answered from the copy the main thread wrote (under mMutex);
//                                                    RmlUi is never called there
//     resetPreedit (IMR_RECONVERTSTRING)             nothing (not the main thread)
//     markAsPreedit (IMR_RECONVERTSTRING)            the word to reconvert, kept under mMutex for the next
//                                                    updatePreedit (else its result went beside the word)
// A field's selected parts of a composition (the standout clauses) cannot be shown: RmlUi has one composition
// range, drawn underlined as a whole.
//
// The bridge is made once and never deleted (the window may hold it as its preeditor until it lets it go);
// it holds no RmlUi or GL resource of its own. Its states (research/364 6): no input, input (a field active),
// composing.

#ifndef PLUS_RMLIME_H
#define PLUS_RMLIME_H

#include "rmlcommon.h"

#include "llcoord.h"
#include "llhandle.h"
#include "llmutex.h"
#include "llpreeditor.h"
#include "llrect.h"

class LLRmlView;

namespace plus::rmlui
{
    class RmlPreeditor final : public LLPreeditor, public Rml::TextInputHandler
    {
    public:
        static RmlPreeditor& instance();

        // Rml::TextInputHandler (main thread, inside the host's LLRmlView::Scope)
        void OnActivate(Rml::TextInputContext* input) override;
        void OnDeactivate(Rml::TextInputContext* input) override;
        void OnDestroy(Rml::TextInputContext* input) override;

        // SystemInterface::ActivateKeyboard: the active field's caret (context pixels) and its line height. (A field
        // that is just getting the focus reports it before OnActivate: it is kept for that call.)
        void setCaret(const LLRmlView* host, Rml::Vector2f caret_px, float line_height_px);
        // After the host's Context::Update (and after an edit): the copy the window thread reads.
        void refresh(LLRmlView& host);
        // An <input type="password"> (no IME, no copy or cut, no text in the window's copy).
        static bool isPassword(const Rml::Element* element);
        // Whether the window's IME is allowed for this field: a text field (fieldKind: not a password or read-only one)
        // without the attribute ime="off" (a field of digits only; the XUI line editor turns the IME off for a field
        // with a prevalidator).
        // Read when the field takes the focus; changing the attribute while it is focused acts at the next focus.
        static bool imeAllowed(const Rml::Element* element);
        static constexpr char IME_ATTRIBUTE[] = "ime";
        // The host unloads or goes away: the IME is let go, nothing of its document is touched.
        void forgetHost(const LLRmlView* host);
        // The host's keyboard focus changed (onFocusReceived / onFocusLost) while a text field of it is active: the
        // window's IME is allowed or let go to match.
        void hostFocusChanged(const LLRmlView& host);
        // The host took an input of its own (a character or an editing key, a mouse press, a field's activation): a
        // reconversion's word marked before it is stale (the IME did not follow up with a composition).
        void dropReconvert();
        // What a field does with a composition still open when it loses the focus (its blur, its being disabled, the
        // window closing): by default the composition's text is taken out of it (the field keeps what was confirmed).
        // A field with the attribute ime-blur="keep" keeps the text and only ends the composition (as the XUI line
        // editor does: the typed characters stay and are committed with the rest). Other ends of a composition (the
        // IME's reset, a result, Esc) are not affected.
        static constexpr char IME_BLUR_ATTRIBUTE[] = "ime-blur";
        // The same for this field when it is the active one and composing, done now (WidgetTextInput's Blur is not
        // run for a disabled field): a window reads the field's text after this.
        void finishCompositionOf(const Rml::Element* element);

        // LLPreeditor
        void resetPreedit() override;
        void updatePreedit(const LLWString& preedit_string, const segment_lengths_t& preedit_segment_lengths,
                           const standouts_t& preedit_standouts, S32 caret_position) override;
        void markAsPreedit(S32 position, S32 length) override;
        void getPreeditRange(S32* position, S32* length) const override;
        void getSelectionRange(S32* position, S32* length) const override;
        bool getPreeditLocation(S32 query_position, LLCoordGL* coord, LLRect* bounds, LLRect* control) const override;
        S32 getPreeditFontSize() const override;
        LLWString getPreeditString() const override;
        bool handleUnicodeCharHere(llwchar uni_char) override;

    private:
        RmlPreeditor() = default;

        // What the window thread may ask, as the main thread last saw it.
        struct Copy
        {
            LLWString text;
            S32 preedit_position = 0;
            S32 preedit_length = 0;
            S32 selection_position = 0;
            S32 selection_length = 0;
            LLCoordGL caret;        // the caret's middle (GL coordinates)
            LLRect caret_box;       // the caret's line box
            LLRect control;         // the field
            S32 font_size = 0;      // window pixels
            bool valid = false;     // false: no field (the queries answer "nothing")
        };

        LLRmlView* activeHost() const;
        // The one place the window's IME is allowed or let go: allowed when a field is active, its host has the
        // keyboard focus and it is not a password field, else let go. Deferred to the next idle while the window's
        // IME calls run in the bridge (mImeDepth): the window's loop over a result still holds the bridge as its
        // preeditor, and an allow / let go there would swap it (NULL) under the loop.
        void syncWindow();
        bool applyWindow();     // true: allowed
        // After a deferred let go: a focused XUI text field takes the IME again (the window drops an allow that
        // repeats the state it is in, so the field's own allow during the deferred time was lost). Only from the
        // idle: elsewhere the focus change is in the middle of its switch (LLFocusMgr writes the new focus before
        // the old one's onFocusLost) and the field's own onFocusReceived has not been called yet.
        static void restoreEditorInput();
        // One entry of the window's IME into the bridge (main thread); counted so syncWindow knows.
        class ImeEntry
        {
        public:
            explicit ImeEntry(RmlPreeditor& bridge) : mBridge(bridge) { ++mBridge.mImeDepth; }
            ~ImeEntry() { --mBridge.mImeDepth; }
            ImeEntry(const ImeEntry&) = delete;
            ImeEntry& operator=(const ImeEntry&) = delete;

        private:
            RmlPreeditor& mBridge;
        };
        // The field gone or let go: the IME goes too. finish: a composition still open in the field is ended first, as
        // the field's attribute ime-blur says (kept in it, or taken out: finishComposition); false: the field is not
        // touched (it is going).
        void deactivate(bool finish);
        // The composition taken out of the field, the caret at its start.
        void cancelComposition();
        // The composition ends and its text stays in the field (ime-blur="keep"); the caret stays where it is.
        void keepComposition();
        // A composition left open as its field loses the focus: kept or taken out, by the field's attribute.
        void finishComposition();
        // refresh() for the active host, looked up again (after RmlUi ran events).
        void refreshActive();
        void publish(Copy copy);

        // main thread
        Rml::TextInputContext* mInput = nullptr;    // the active field's (RmlUi's, until OnDestroy)
        LLHandle<LLRmlView> mHost;
        Rml::ObserverPtr<Rml::Element> mElement;
        bool mComposing = false;
        int mCompositionStart = 0;                  // characters
        int mCompositionEnd = 0;
        Rml::Vector2f mCaretPx;
        float mLineHeightPx = 0.f;
        // The caret a field reported before it was active here (see setCaret and OnActivate); host is compared only.
        const LLRmlView* mLastCaretHost = nullptr;
        Rml::Vector2f mLastCaretPx;
        float mLastLineHeightPx = 0.f;
        int mImeDepth = 0;                          // open ImeEntry objects
        bool mSyncPending = false;                  // a deferred syncWindow waits for the next idle
        bool mWindowAllowed = false;                // what applyWindow last gave the window

        // A reconversion's word (markAsPreedit, window thread), taken by the next updatePreedit (main thread).
        struct Reconvert
        {
            bool pending = false;
            S32 position = 0;       // characters
            S32 length = 0;
        };

        // under mMutex: the copy (written on the main thread, read on the window thread) and the reconversion
        // (written on the window thread, taken on the main thread)
        mutable LLMutex mMutex;
        Copy mCopy;
        // The length of the text getPreeditString last gave out. The window asks the text first, then the ranges, in
        // separate locks; a publish between them must not leave a range past the text it got (it cuts the text at
        // the range: find_context in llwindowwin32.cpp), so the ranges are cut to this.
        mutable S32 mGivenLength = 0;
        Reconvert mReconvert;
    };
}

#endif // PLUS_RMLIME_H
