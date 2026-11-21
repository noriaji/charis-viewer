/**
 * @file rmlsearch.cpp
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

// See rmlsearch.h. What protectedfoldersrml.cpp's filter does, as a part.

#include "llviewerprecompiledheaders.h"

#include "rmlsearch.h"

#include "rmlhost.h"

namespace plus::rmlui
{
    void SearchBox::attach(LLRmlView& view, Rml::ElementDocument& document, const char* id, int max_bytes)
    {
        mView = &view;
        mDocument = &document;
        mId = id;
        mMaxBytes = max_bytes;
        mText.clear();
        mChanged = false;
        Rml::Element* field = document.GetElementById(id);
        for (const char* event : { "focus", "blur", "change" })
        {
            view.listen(field, event);
        }
        view.listen(document.GetElementById(mId + "_clear"), "click");
    }

    void SearchBox::detach()
    {
        mView = nullptr;
        mDocument = nullptr;
    }

    Rml::ElementFormControl* SearchBox::box() const
    {
        Rml::Element* field = mDocument ? mDocument->GetElementById(mId) : nullptr;
        return field ? rmlui_dynamic_cast<Rml::ElementFormControl*>(field) : nullptr;
    }

    bool SearchBox::takeChanged()
    {
        const bool changed = mChanged;
        mChanged = false;
        return changed;
    }

    void SearchBox::showHint(bool shown)
    {
        if (Rml::Element* hint = mDocument ? mDocument->GetElementById(mId + "_hint") : nullptr)
        {
            hint->SetClass("hidden", !shown);
        }
    }

    // The text changed (or the focus left): the clear button and the hint follow.
    void SearchBox::changed()
    {
        Rml::ElementFormControl* field = box();
        if (!field)
        {
            return;
        }
        std::string text = field->GetValue();
        if ((int)text.size() > mMaxBytes)
        {
            text.resize(mMaxBytes);
            while (!text.empty() && ((unsigned char)text.back() & 0xC0) == 0x80)
            {
                text.pop_back();    // not in the middle of a character
            }
            if (!text.empty() && (unsigned char)text.back() >= 0xC0)
            {
                text.pop_back();
            }
        }
        if (text != mText)
        {
            mText = text;
            mChanged = true;
        }
        if (Rml::Element* clear = mDocument->GetElementById(mId + "_clear"))
        {
            clear->SetClass("hidden", text.empty());
        }
        Rml::Context* context = mView ? mView->getContext() : nullptr;
        showHint(text.empty() && !(context && context->GetFocusElement() == field));
    }

    bool SearchBox::handleEvent(Rml::Event& event)
    {
        const Rml::Element* element = event.GetCurrentElement();
        if (!element || !mDocument)
        {
            return false;
        }
        const std::string id = element->GetId();
        if (id == mId + "_clear")
        {
            if (Rml::ElementFormControl* field = box())
            {
                field->SetValue("");
                field->Focus();
            }
            changed();
            return true;
        }
        if (id != mId)
        {
            return false;
        }
        if (event.GetId() == Rml::EventId::Focus)
        {
            showHint(false);        // (the whole text is selected by the host: the field's select-on-focus attribute)
        }
        else
        {
            changed();
            if (event.GetId() == Rml::EventId::Blur)
            {
                showHint(mText.empty());    // the focus has left the field (its element is still the context's focus at this event)
            }
        }
        return true;
    }
}
