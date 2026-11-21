/**
 * @file rmlime.cpp
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

#include "llviewerprecompiledheaders.h"

#include "rmlime.h"

#include "rmlhost.h"
#include "rmlparts.h"

#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "lllineeditor.h"
#include "lltexteditor.h"
#include "llthread.h"
#include "llview.h"
#include "llwindow.h"

#include <cmath>
#include <utility>

namespace plus::rmlui
{
    RmlPreeditor& RmlPreeditor::instance()
    {
        // Never deleted: the window may name it as its preeditor until allowLanguageTextInput lets it go, and its
        // window thread reads that pointer without a lock (WM_IME_REQUEST). It holds no GL or RmlUi resource.
        static RmlPreeditor* sInstance = new RmlPreeditor;
        return *sInstance;
    }

    LLRmlView* RmlPreeditor::activeHost() const
    {
        return mInput ? mHost.get() : nullptr;
    }

    void RmlPreeditor::OnActivate(Rml::TextInputContext* input)
    {
        LLRmlView* host = LLRmlView::current();
        if (!input || !host || !host->getContext())
        {
            return;
        }
        if (mInput && mInput != input)
        {
            deactivate(true);   // another field took the focus without the first one's blur
        }
        mInput = input;
        mHost = host->getRmlHandle();
        // The field that is getting the focus. RmlUi calls this inside the field's own focus event, before it sets the
        // context's focus (Context::OnFocusChange sends the events, then sets focus), so GetFocusElement() is still the
        // element that LOST the focus (never the one to use here): Parts noted the target of the latest focus event in
        // the capture pass of the document. That is this field's unless a window's focus listener moved the focus in
        // between, which the box check below catches. No note (nothing in a focus event reached Parts) leaves the
        // element empty: the IME stays off for this field and its copy is empty, which is safe.
        Rml::Element* element = host->parts().focusTarget();
        if (!element)
        {
            LL_WARNS_ONCE("RmlUi") << "RmlUi: a text field was activated outside a focus event; the IME stays off for it" << LL_ENDL;
        }
        else
        {
            Rml::Rectanglef field_box;
            Rml::Rectanglef element_box;
            const bool field_ok = input->GetBoundingBox(field_box);
            const bool element_ok = Rml::ElementUtilities::GetBoundingBox(element_box, element, Rml::BoxArea::Border);
            if (field_ok != element_ok || (field_ok && (std::fabs(field_box.Left() - element_box.Left()) > 0.5f ||
                                                         std::fabs(field_box.Top() - element_box.Top()) > 0.5f ||
                                                         std::fabs(field_box.Right() - element_box.Right()) > 0.5f ||
                                                         std::fabs(field_box.Bottom() - element_box.Bottom()) > 0.5f)))
            {
                LL_WARNS_ONCE("RmlUi") << "RmlUi: the element of a focus event is not the field that was activated "
                                       << "(a window's focus listener moved the focus); the IME stays off for it" << LL_ENDL;
                element = nullptr;
            }
        }
        mElement = element ? element->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        // The caret RmlUi reported just before (WidgetTextInput::Focus calls ActivateKeyboard, then OnActivate): at that
        // time no field was active here, so setCaret kept it aside.
        if (mLastCaretHost == host)
        {
            mCaretPx = mLastCaretPx;
            mLineHeightPx = mLastLineHeightPx;
        }
        mLastCaretHost = nullptr;
        mComposing = false;
        mCompositionStart = 0;
        mCompositionEnd = 0;
        dropReconvert();    // a mark made before this field was active is not for it
        syncWindow();
        if (host->hasFocus())
        {
            host->takeEditMenu();   // the edit menu's keys (Del, Ctrl+X/C/V/A) act on this field, not the last handler
        }
        refresh(*host);
    }

    bool RmlPreeditor::isPassword(const Rml::Element* element)
    {
        return element && element->GetTagName() == "input" && element->GetAttribute<Rml::String>("type", "") == "password";
    }

    bool RmlPreeditor::imeAllowed(const Rml::Element* element)
    {
        // ime="off" on the field itself (not looked up in its ancestors, as with the password type), read when the
        // field takes the focus: like the XUI line editor, a field of digits only takes no IME. A password field and a
        // read-only one are not FieldKind::Text (research/387 6.4).
        return fieldKind(element) == FieldKind::Text && element->GetAttribute<Rml::String>(IME_ATTRIBUTE, Rml::String()) != "off";
    }

    void RmlPreeditor::hostFocusChanged(const LLRmlView& host)
    {
        if (mInput && mHost.get() == &host)
        {
            syncWindow();
        }
    }

    void RmlPreeditor::dropReconvert()
    {
        LLMutexLock lock(&mMutex);
        mReconvert = Reconvert();
    }

    void RmlPreeditor::OnDeactivate(Rml::TextInputContext* input)
    {
        if (input && input == mInput)
        {
            deactivate(true);
        }
    }

    void RmlPreeditor::OnDestroy(Rml::TextInputContext* input)
    {
        if (input && input == mInput)
        {
            mInput = nullptr;   // going: not touched again
            deactivate(false);
        }
    }

    void RmlPreeditor::forgetHost(const LLRmlView* host)
    {
        if (mInput && (mHost.get() == host || mHost.isDead()))
        {
            deactivate(false);
        }
    }

    void RmlPreeditor::deactivate(bool finish)
    {
        if (finish)
        {
            finishComposition();
        }
        if (LLRmlView* host = mHost.get())
        {
            host->releaseEditMenu();
        }
        {
            LLMutexLock lock(&mMutex);
            mReconvert = Reconvert();
        }
        mInput = nullptr;
        mHost = LLHandle<LLRmlView>();
        mElement.reset();
        mComposing = false;
        mCompositionStart = 0;
        mCompositionEnd = 0;
        mLineHeightPx = 0.f;
        // Before syncWindow: letting the window go ends a composition left open (ImmNotifyIME CPS_COMPLETE), and the
        // window thread may ask the bridge meanwhile; an empty copy is the right answer then, not this field's.
        publish(Copy());
        syncWindow();
    }

    void RmlPreeditor::syncWindow()
    {
        if (mImeDepth == 0)
        {
            applyWindow();
        }
        else if (!mSyncPending)
        {
            // Inside the window's IME call: it goes on with its preeditor after this returns, so the state is
            // worked out at the next idle (not now: what is right now may be changed by the rest of the call). The
            // bridge is never deleted.
            mSyncPending = true;
            doOnIdleOneTime([]()
            {
                RmlPreeditor& bridge = RmlPreeditor::instance();
                bridge.mSyncPending = false;
                const bool was_allowed = bridge.mWindowAllowed;
                if (!bridge.applyWindow() && was_allowed)
                {
                    // The bridge let go just now (not before: then the XUI field's own allow went through). An XUI
                    // field's allow made during the wait was dropped by the window, as it repeated the state.
                    restoreEditorInput();
                }
            });
        }
    }

    bool RmlPreeditor::applyWindow()
    {
        LLWindow* window = LLView::getWindow();
        if (!window)
        {
            return false;
        }
        const LLRmlView* host = activeHost();
        // Without the keyboard focus the composition would go here while the keys go elsewhere; a password field
        // or an ime="off" field takes no IME (its text would be handed to the IME's queries), nor does a read-only one
        // (nothing is typed into it): imeAllowed() is the one place that says so.
        const bool allow = host && host->hasFocus() && imeAllowed(mElement.get());
        // (let go: the window ends a composition left open, its result comes to no preeditor)
        window->allowLanguageTextInput(this, allow);
        mWindowAllowed = allow;
        return allow;
    }

    void RmlPreeditor::restoreEditorInput()
    {
        LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
        // What the editor does when it gets the focus (its own updateAllowingLanguageInput is not public; it also
        // makes the editor the edit menu's handler, which it is as the focused one).
        if (LLLineEditor* line = dynamic_cast<LLLineEditor*>(focus))
        {
            line->onFocusReceived();
        }
        else if (LLTextEditor* text = dynamic_cast<LLTextEditor*>(focus))
        {
            text->onFocusReceived();
        }
    }

    void RmlPreeditor::cancelComposition()
    {
        if (!mInput || !mComposing)
        {
            return;
        }
        mInput->SetText(Rml::StringView(), mCompositionStart, mCompositionEnd);
        if (mInput)     // (a change event may have removed the field: OnDestroy clears it)
        {
            mInput->SetCursorPosition(mCompositionStart);
            mInput->SetCompositionRange(0, 0);
        }
        mComposing = false;
        mCompositionEnd = mCompositionStart;
        if (LLRmlView* host = mHost.get())
        {
            host->markDirty();
        }
    }

    void RmlPreeditor::setCaret(const LLRmlView* host, Rml::Vector2f caret_px, float line_height_px)
    {
        if (mInput && host && mHost.get() == host)
        {
            mCaretPx = caret_px;
            mLineHeightPx = llmax(0.f, line_height_px);
        }
        else if (host)
        {
            // No field is active here yet: a field that is just getting the focus reports its caret before OnActivate.
            // Kept for it (OnActivate takes it and forgets it; the pointer is only compared, never used).
            mLastCaretHost = host;
            mLastCaretPx = caret_px;
            mLastLineHeightPx = llmax(0.f, line_height_px);
        }
    }

    void RmlPreeditor::finishCompositionOf(const Rml::Element* element)
    {
        if (mInput && mComposing && element && mElement.get() == element)
        {
            finishComposition();
        }
    }

    void RmlPreeditor::keepComposition()
    {
        if (!mInput || !mComposing)
        {
            return;
        }
        mInput->SetCompositionRange(0, 0);      // the text stays: it is the field's own now
        mComposing = false;
        mCompositionEnd = mCompositionStart;
        if (LLRmlView* host = mHost.get())
        {
            host->markDirty();
        }
    }

    void RmlPreeditor::finishComposition()
    {
        const Rml::Element* element = mElement.get();
        if (element && element->GetAttribute<Rml::String>(IME_BLUR_ATTRIBUTE, Rml::String()) == "keep")
        {
            keepComposition();
        }
        else
        {
            cancelComposition();
        }
    }

    void RmlPreeditor::refresh(LLRmlView& host)
    {
        if (!mInput || mHost.get() != &host)
        {
            return;
        }
        // The active field became disabled (a data model's disabled attribute, a window's setting): RmlUi blurs it, but a
        // disabled field's WidgetTextInput does not run its Blur and so never calls OnDeactivate. The IME and the edit
        // menu are let go here, after the Update that disabled it.
        Rml::ElementFormControl* field = rmlui_dynamic_cast<Rml::ElementFormControl*>(mElement.get());
        if (field && field->IsDisabled())
        {
            deactivate(true);
            return;
        }
        Copy copy;
        // A password field (and a read-only one, whose log may be 64 KB: nothing is typed or reconverted in it) gives the
        // window no text, and then no position either: the window cuts the text at
        // these positions (find_context in llwindowwin32.cpp), so a caret past the empty text reads outside it.
        // The defaults (0, 0) stay.
        if (field && !isPassword(field) && fieldKind(field) != FieldKind::ReadOnly)
        {
            copy.text = utf8str_to_wstring(field->GetValue());
            int start = 0;
            int end = 0;
            mInput->GetSelectionRange(start, end);
            copy.selection_position = llmin(start, end);
            copy.selection_length = llabs(end - start);
            if (mComposing)
            {
                copy.preedit_position = mCompositionStart;
                copy.preedit_length = mCompositionEnd - mCompositionStart;
            }
            else
            {
                copy.preedit_position = end;    // no composition: the caret, length 0 (LLPreeditor)
                copy.preedit_length = 0;
            }
        }
        copy.caret = host.contextToGL(Rml::Vector2f(mCaretPx.x, mCaretPx.y + mLineHeightPx * 0.5f));
        copy.caret_box = host.contextRectToGL(Rml::Rectanglef::FromPositionSize(mCaretPx, Rml::Vector2f(1.f, mLineHeightPx)));
        Rml::Rectanglef box;
        copy.control = mInput->GetBoundingBox(box) ? host.contextRectToGL(box) : copy.caret_box;
        copy.font_size = ll_round(mLineHeightPx);
        copy.valid = true;
        publish(std::move(copy));
    }

    void RmlPreeditor::publish(Copy copy)
    {
        {
            LLMutexLock lock(&mMutex);
            std::swap(mCopy, copy);
        }
        // the old copy is freed here, outside the lock
    }

    void RmlPreeditor::updatePreedit(const LLWString& preedit_string, const segment_lengths_t& /*preedit_segment_lengths*/,
                                     const standouts_t& /*preedit_standouts*/, S32 caret_position)
    {
        if (!on_main_thread() || !mInput)
        {
            return;
        }
        ImeEntry entry(*this);
        LLRmlView* host = activeHost();
        if (!host)
        {
            deactivate(false);
            return;
        }
        LLRmlView::Scope scope(host);
        if (preedit_string.empty())
        {
            cancelComposition();    // cancelled (Esc), or after a result (resetPreedit took it out already)
            refreshActive();
            return;
        }
        if (!mComposing)
        {
            // The composition starts over the selection (RmlUi's TextInputMethodEditor_Win32::SetCompositionString),
            // or over the word a reconversion marked (markAsPreedit, on the window thread just before).
            Reconvert reconvert;
            {
                LLMutexLock lock(&mMutex);
                std::swap(reconvert, mReconvert);
            }
            int start = 0;
            int end = 0;
            mInput->GetSelectionRange(start, end);
            mCompositionStart = llmin(start, end);
            mCompositionEnd = llmax(start, end);
            const Rml::ElementFormControl* field = rmlui_dynamic_cast<Rml::ElementFormControl*>(mElement.get());
            if (reconvert.pending && field)
            {
                const S32 text_length = (S32)Rml::StringUtilities::LengthUTF8(field->GetValue());
                mCompositionStart = llclamp(reconvert.position, 0, text_length);
                // (the IME's length is not trusted: clamped before the sum, which cannot overflow then)
                mCompositionEnd = mCompositionStart + llclamp(reconvert.length, 0, text_length - mCompositionStart);
            }
            mComposing = true;
        }
        mInput->SetText(wstring_to_utf8str(preedit_string), mCompositionStart, mCompositionEnd);
        if (!mInput)
        {
            return;     // the field went in its change event
        }
        mCompositionEnd = mCompositionStart + (int)preedit_string.length();
        mInput->SetCompositionRange(mCompositionStart, mCompositionEnd);
        mInput->SetCursorPosition(mCompositionStart + llclamp(caret_position, 0, (S32)preedit_string.length()));
        if (LLRmlView* active = activeHost())   // looked up again after RmlUi's events
        {
            active->markDirty();
        }
        refreshActive();
    }

    void RmlPreeditor::refreshActive()
    {
        if (LLRmlView* host = activeHost())
        {
            refresh(*host);
        }
    }

    void RmlPreeditor::resetPreedit()
    {
        if (!on_main_thread())
        {
            return;     // IMR_RECONVERTSTRING asks on the window thread: RmlUi is not touched there
        }
        ImeEntry entry(*this);
        LLRmlView* host = activeHost();
        if (!host || !mComposing)
        {
            return;
        }
        LLRmlView::Scope scope(host);
        cancelComposition();
        refreshActive();
    }

    void RmlPreeditor::markAsPreedit(S32 position, S32 length)
    {
        // A reconversion (IMR_RECONVERTSTRING, on the window thread): the word the IME chose, kept under the lock
        // for the next updatePreedit (main thread), which composes over it instead of beside it. RmlUi is not
        // touched here.
        LLMutexLock lock(&mMutex);
        mReconvert.pending = true;
        mReconvert.position = position;
        mReconvert.length = length;
    }

    bool RmlPreeditor::handleUnicodeCharHere(llwchar uni_char)
    {
        if (!on_main_thread())
        {
            return false;
        }
        ImeEntry entry(*this);
        LLRmlView* host = activeHost();
        if (!host)
        {
            return false;
        }
        const bool typed = host->typeCharacter(uni_char);
        if (typed)
        {
            refreshActive();
        }
        return typed;
    }

    void RmlPreeditor::getPreeditRange(S32* position, S32* length) const
    {
        LLMutexLock lock(&mMutex);
        *position = llclamp(mCopy.preedit_position, 0, mGivenLength);
        *length = llclamp(mCopy.preedit_length, 0, mGivenLength - *position);
    }

    void RmlPreeditor::getSelectionRange(S32* position, S32* length) const
    {
        LLMutexLock lock(&mMutex);
        *position = llclamp(mCopy.selection_position, 0, mGivenLength);
        *length = llclamp(mCopy.selection_length, 0, mGivenLength - *position);
    }

    bool RmlPreeditor::getPreeditLocation(S32 /*query_position*/, LLCoordGL* coord, LLRect* bounds, LLRect* control) const
    {
        // Every position of the composition answers with the caret (RmlUi has no per-character position here).
        LLMutexLock lock(&mMutex);
        if (!mCopy.valid)
        {
            return false;
        }
        if (coord)
        {
            *coord = mCopy.caret;
        }
        if (bounds)
        {
            *bounds = mCopy.caret_box;
        }
        if (control)
        {
            *control = mCopy.control;
        }
        return true;
    }

    S32 RmlPreeditor::getPreeditFontSize() const
    {
        LLMutexLock lock(&mMutex);
        return mCopy.font_size;
    }

    LLWString RmlPreeditor::getPreeditString() const
    {
        LLMutexLock lock(&mMutex);
        mGivenLength = (S32)mCopy.text.length();
        return mCopy.text;
    }
}
