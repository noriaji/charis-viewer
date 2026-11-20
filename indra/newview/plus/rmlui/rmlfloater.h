/**
 * @file rmlfloater.h
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

// A window of a plus_rml_view (rmlhost.h) named "body" in its XML, the XUI floater being only the frame and the
// words (<string>s). What every such window shares is here, so each one does not write it again (the pattern of
// the keywords window, research/380; the weather and the time-conversion windows follow it):
//   - the host's hooks are set and the document loaded from postBuild(); the hooks call the virtuals below,
//   - the document goes in one order: the focused element is blurred (a text field's own blur ends an IME
//     composition and the window's blur listener commits the field), commitPending() (text typed and not yet
//     committed), unbind() (the window's data model and settings signals), then the host's unload(). closeDocument() does it, once: from onClose()
//     (the window closed) and from the destructor of the base. A derived class that holds things the document
//     points at (a data model, listeners) calls closeDocument() first in its own destructor, as its members go
//     before the base's destructor runs (this base cannot call the derived virtuals then).
//   - set reuse_instance="false" in the XML: a closed window is destroyed, and then costs nothing (no context,
//     no signals); RmlUi itself ends 60 s after the last window (research/364 6).
//     The document is closed once for good (a window shown again without being destroyed would stay empty).

#ifndef PLUS_RMLFLOATER_H
#define PLUS_RMLFLOATER_H

#include "rmlcommon.h"

#include "llfloater.h"

class LLRmlView;

namespace plus::rmlui
{
    class RmlFloater : public LLFloater
    {
    public:
        explicit RmlFloater(const LLSD& key);
        ~RmlFloater() override;
        bool postBuild() override;
        void onClose(bool app_quitting) override;

    protected:
        // Called on the main thread inside RmlUi's work for the host (rmlhost.h Hooks). The defaults do nothing.
        virtual void beforeLoad(Rml::Context& context) { (void)context; }
        virtual void afterLoad(Rml::ElementDocument& document) { (void)document; }
        virtual void beforeUpdate() {}
        virtual void afterUpdate() {}
        virtual void onEvent(Rml::Event& event) { (void)event; }
        // The window closes or goes: a text typed and not committed is committed (the focus may not leave first;
        // closeDocument blurs the focused field before this, so its blur listener commits it).
        virtual void commitPending() {}
        // The window's data model, signals and listeners go (before the document does). Idempotent.
        virtual void unbind() {}

        // The document's name as the "document" attribute of the XML's plus_rml_view: nothing to set here.
        LLRmlView* host() const;
        // A text field's text after an IME composition still open in it has ended as its blur ends it (taken out of the
        // field, or kept in it with ime-blur="keep": rmlime.h). For a listener that commits a field when it loses the
        // focus: listen to "blur" with LLRmlView::listenCapture, so RmlUi's own Blur ran before, and read the text
        // with this.
        static std::string committedText(Rml::ElementFormControl& box);
        // For the derived class's destructor (see above). Idempotent.
        void closeDocument();

    private:
        LLHandle<LLRmlView> mHost;
        bool mDocumentClosed = false;
    };
}

#endif // PLUS_RMLFLOATER_H
