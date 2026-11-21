/**
 * @file rmlfloater.cpp
 * @brief RmlFloater: the base of a window whose content is one RmlUi document (research/380 1).
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

#include "rmlfloater.h"

#include "rmlhost.h"
#include "rmlime.h"
#include "rmlpopup.h"

namespace plus::rmlui
{
    RmlFloater::RmlFloater(const LLSD& key)
    :   LLFloater(key)
    {
    }

    RmlFloater::~RmlFloater()
    {
        closeDocument();    // (a derived class that holds model state has done it already)
    }

    LLRmlView* RmlFloater::host() const
    {
        return mHost.get();
    }

    bool RmlFloater::postBuild()
    {
        LLRmlView* view = findChild<LLRmlView>("body");
        if (!view)
        {
            return true;    // no view in the XML: an empty window
        }
        mHost = view->getRmlHandle();
        mSelectPopup = std::make_unique<SelectPopup>();
        LLRmlView::Hooks hooks;
        hooks.before_load = [this](Rml::Context& context) { beforeLoad(context); };
        hooks.after_load = [this](Rml::ElementDocument& document)
        {
            mSelectPopup->attach(mHost.get(), document);
            afterLoad(document);
        };
        hooks.before_update = [this]() { beforeUpdate(); };
        hooks.after_update = [this]()
        {
            afterUpdate();
            mSelectPopup->ownerUpdated();
        };
        hooks.on_event = [this](Rml::Event& event) { onEvent(event); };
        view->setHooks(std::move(hooks));
        view->load();
        return true;
    }

    void RmlFloater::onClose(bool app_quitting)
    {
        closeDocument();
        LLFloater::onClose(app_quitting);
    }

    bool RmlFloater::handleUnicodeCharHere(llwchar uni_char)
    {
        // (what the host did not use reaches here: the host takes a character only for a text field)
        return (mSelectPopup && mSelectPopup->handleChar(uni_char)) || LLFloater::handleUnicodeCharHere(uni_char);
    }

    bool RmlFloater::handleScrollWheel(S32 x, S32 y, S32 clicks)
    {
        // A closed <select> under the mouse takes the wheel first when PlusComboWheelSelect is on (the XUI combo box takes
        // it before anything that scrolls under it); else LLFloater::handleScrollWheel's: the children, and always used
        // (the wheel does not reach the world over a window).
        LLRmlView* view = mHost.get();
        S32 host_x = x;
        S32 host_y = y;
        if (mSelectPopup && view && localPointToOtherView(x, y, &host_x, &host_y, view) &&
            mSelectPopup->handleWheel(host_x, host_y, clicks))
        {
            return true;
        }
        LLView::handleScrollWheel(x, y, clicks);
        return true;
    }

    std::string RmlFloater::committedText(Rml::ElementFormControl& box)
    {
        RmlPreeditor::instance().finishCompositionOf(&box);
        return box.GetValue();
    }

    void RmlFloater::closeDocument()
    {
        if (mDocumentClosed)
        {
            return;
        }
        mDocumentClosed = true;
        LLRmlView* view = mHost.get();
        {
            LLRmlView::Scope scope(view);   // RmlUi works for the view (a null view: no scope)
            // A field with the focus is blurred while the window's listeners still stand: RmlUi's Blur ends an IME
            // composition, and the listener then commits the field (as a click on another part would).
            Rml::Context* context = view ? view->getContext() : nullptr;
            if (Rml::Element* focus = context ? context->GetFocusElement() : nullptr)
            {
                focus->Blur();
            }
            commitPending();
        }
        if (mSelectPopup)
        {
            mSelectPopup->detach();     // the open list, its listeners and its context go before the document does
        }
        unbind();
        if (view)
        {
            view->unload();     // (waits until RmlUi's work for it ends: rmlhost.h)
        }
    }
}
