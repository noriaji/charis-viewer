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
        LLRmlView::Hooks hooks;
        hooks.before_load = [this](Rml::Context& context) { beforeLoad(context); };
        hooks.after_load = [this](Rml::ElementDocument& document) { afterLoad(document); };
        hooks.before_update = [this]() { beforeUpdate(); };
        hooks.after_update = [this]() { afterUpdate(); };
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
        unbind();
        if (view)
        {
            view->unload();     // (waits until RmlUi's work for it ends: rmlhost.h)
        }
    }
}
