/**
 * @file rmlsearch.h
 * @brief SearchBox: the text field with a hint and a clear button (research/386).
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

// A search field of a window: the text field, the hint shown while it is empty and does not have the focus, and the clear
// button shown while there is text (LLSearchEditor's work; the style is charis.rcss's .ps-search). The ids of the document are
// a prefix and its three endings: <id> (the text field), <id>_hint (a span), <id>_clear (a button):
//     <div class="ps-search">
//         <input type="text" class="text" id="filter" maxlength="256" ime-blur="keep" select-on-focus/>
//         <span class="ps-hint" id="filter_hint">[[filter_hint]]</span>
//         <button id="filter_clear" class="clear hidden">x</button>
//     </div>
// In the window (the contract with the host: RmlFloater's hooks):
//     afterLoad:  mSearch.attach(*view, document, "filter", 256);
//     onEvent:    if (mSearch.handleEvent(event)) { if (mSearch.takeChanged()) { ...narrow the list... } return; }
// The field selects its whole text when it gets the focus by the attribute select-on-focus (the host's part, rmlparts.h
// 2.: a "focus" listener of the window cannot do it). Only classes and values change inside the
// events; the window changes its list in its own time (takeChanged()).
// The part holds the document and the host as raw pointers (they are used in the window's events only): the window calls
// detach() in unbind(), before its document goes, and they are not used after it.

#ifndef PLUS_RMLSEARCH_H
#define PLUS_RMLSEARCH_H

#include "rmlcommon.h"

#include <string>

class LLRmlView;

namespace plus::rmlui
{
    class SearchBox
    {
    public:
        void attach(LLRmlView& view, Rml::ElementDocument& document, const char* id, int max_bytes);
        void detach();
        // True when the event was for this field (its focus, blur, change, or the clear button).
        bool handleEvent(Rml::Event& event);
        // The text as it is now (cut to max_bytes bytes, not in the middle of a character; the field's own maxlength counts characters).
        const std::string& text() const { return mText; }
        // True once after the text changed.
        bool takeChanged();

    private:
        Rml::ElementFormControl* box() const;
        void changed();
        void showHint(bool shown);

        LLRmlView* mView = nullptr;
        Rml::ElementDocument* mDocument = nullptr;
        std::string mId;
        int mMaxBytes = 0;
        std::string mText;
        bool mChanged = false;
    };
}

#endif // PLUS_RMLSEARCH_H
