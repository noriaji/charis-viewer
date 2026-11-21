/**
 * @file importrml.cpp
 * @brief The bring-settings-over window made with RmlUi (research/386).
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

// The same window as importsettings.cpp's (the XUI one, frozen: safe mode and PlusRmlUiWindows off), with its content
// app_settings/plus/rmlui/import_settings.rml. What the window does is importcontroller.cpp's (shared, so that the two
// windows cannot differ); this class is the view. Everything the controller sets is kept here and put in the document in
// beforeUpdate() (never inside RmlUi's event). The viewers' names and folders come from the disk (the folder names of
// other viewers, a folder the user picked) and the status text from the importer: they are put in as text (RmlList and
// setLogText: never RML), and a name or a folder that is very long or holds control characters is cut / cleaned first
// (shownText()).
//
// The status is a <textarea readonly> (the host's read-only field: it can be selected and copied, takes no typing or IME).

#include "llviewerprecompiledheaders.h"

#include "importcontroller.h"
#include "importrml.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlparts.h"

#include "llfloaterreg.h"

#include <cstdlib>
#include <vector>

namespace plus::importsettings
{
    namespace
    {
        using plus::rmlui::RmlList;

        constexpr size_t STATUS_MAX_BYTES = 4096;       // the XUI text editor's max_length
        constexpr size_t SHOWN_MAX_CHARS = 300;         // a name or a folder in the list (a path is 260 at most on Windows)

        const char* const CHECKS[] = { "chk_general", "chk_graphics", "chk_keys", "chk_accounts", "chk_chat", "chk_files" };

        // A text from outside as one line of a list: control characters become spaces and a very long one is cut.
        std::string shownText(const std::string& text)
        {
            std::string out = text.size() > SHOWN_MAX_CHARS * 4 ? utf8str_truncate(text, (S32)(SHOWN_MAX_CHARS * 4)) : text;
            for (char& c : out)
            {
                if ((unsigned char)c < 0x20 || c == 0x7f)
                {
                    c = ' ';
                }
            }
            return out;
        }

        class ImportRmlFloater : public plus::rmlui::RmlFloater, public ImportController::View
        {
            friend class ::LLFloaterReg;

        public:
            ~ImportRmlFloater() override
            {
                closeDocument();    // before the list and the text go
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                mController.open();
                touch();
            }

            void onClose(bool app_quitting) override
            {
                mController.onClose(app_quitting);
                RmlFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mClose)
                {
                    mClose = false;
                    closeFloater();     // not inside RmlUi's event (the button only asks)
                    return;
                }
                RmlFloater::draw();
            }

            // ImportController::View (only values are kept here; the document is changed in beforeUpdate())
            std::string text(const char* id, const LLStringUtil::format_map_t& args) override
            {
                return getString(id, args);
            }
            void setIntro(const std::string& text) override
            {
                mIntro = text;
                mIntroChanged = true;
                touch();
            }
            void setCloseLabel(const std::string& text) override
            {
                mCloseLabel = text;
                mCloseLabelChanged = true;
                touch();
            }
            void setStatus(const std::string& text) override
            {
                if (text != mStatus)
                {
                    mStatus = text;
                    mStatusChanged = true;
                }
                touch();
            }
            void setSources(const std::vector<Source>& sources, size_t selected) override
            {
                std::vector<RmlList::Row> rows;
                rows.reserve(sources.size());
                for (size_t i = 0; i < sources.size(); ++i)
                {
                    RmlList::Row& row = rows.emplace_back();
                    row.key = std::to_string(i);
                    row.cells = { cell(shownText(sources[i].name)), cell(shownText(sources[i].root)),
                                  cell(ImportController::dateText(sources[i].last_used)) };
                }
                mList.setRows(std::move(rows));
                mSelected = sources.empty() ? -1 : (int)selected;
                if (mSelected >= 0)
                {
                    mList.selectKey(std::to_string(mSelected));
                }
                touch();
            }
            int selectedSource() const override { return mSelected; }
            void setImportEnabled(bool enabled) override
            {
                mImportEnabled = enabled;
                mImportEnabledChanged = true;
                touch();
            }
            void setGraphicsChecked(bool checked) override
            {
                mGraphicsChecked = checked;
                mGraphicsChanged = true;
                touch();
            }
            Choice choice() const override
            {
                Choice c;
                c.general = checked("chk_general");
                c.graphics = checked("chk_graphics");
                c.keys = checked("chk_keys");
                c.accounts = checked("chk_accounts");
                c.chat = checked("chk_chat");
                c.files = checked("chk_files");
                return c;
            }

        protected:
            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mList.attach(document, "sources", false);
                mList.attachHead(document, "sources_head");
                mList.setColumns(columns());
                for (const char* event : { "click", "keydown", "mousedown" })
                {
                    view->listen(document.GetElementById("sources"), event);
                }
                view->listen(document.GetElementById("sources_head"), "click");
                for (const char* id : { "btn_browse", "btn_import", "btn_close" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                mStatusChanged = true;
                mIntroChanged = !mIntro.empty();
                mCloseLabelChanged = !mCloseLabel.empty();
                mImportEnabledChanged = true;
            }

            void beforeUpdate() override
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                if (mIntroChanged)
                {
                    mIntroChanged = false;
                    setText(document->GetElementById("intro"), mIntro);
                }
                if (mCloseLabelChanged)
                {
                    mCloseLabelChanged = false;
                    setText(document->GetElementById("btn_close"), mCloseLabel);
                }
                mList.apply();
                if (mStatusChanged)
                {
                    mStatusChanged = false;
                    // The text is from the importer (other viewers' files): plain text, cut to the XUI editor's max_length.
                    plus::rmlui::setLogText(*view, document->GetElementById("status"),
                                            utf8str_truncate(mStatus, (S32)STATUS_MAX_BYTES),
                                            plus::rmlui::Follow::Keep);
                }
                if (mImportEnabledChanged)
                {
                    mImportEnabledChanged = false;
                    plus::rmlui::setElementDisabled(document->GetElementById("btn_import"), !mImportEnabled);
                }
                if (mGraphicsChanged)
                {
                    mGraphicsChanged = false;
                    if (Rml::Element* box = document->GetElementById("chk_graphics"))
                    {
                        if (mGraphicsChecked)
                        {
                            box->SetAttribute("checked", Rml::String());
                        }
                        else
                        {
                            box->RemoveAttribute("checked");
                        }
                    }
                }
            }

            void afterUpdate() override
            {
                mList.afterUpdate();
            }

            // In RmlUi's event: nothing is removed from the document here (the values change in beforeUpdate).
            void onEvent(Rml::Event& event) override
            {
                const Rml::Element* element = event.GetCurrentElement();
                if (!element)
                {
                    return;
                }
                if (event.GetId() == Rml::EventId::Click && plus::rmlui::isElementDisabled(element))
                {
                    return;     // a disabled button takes no click (RmlUi 6.3 does not stop it)
                }
                const std::string id = element->GetId();
                if (id == "sources" || id == "sources_head")
                {
                    if (mList.handleEvent(event) == RmlList::Result::Selection)
                    {
                        const std::vector<std::string> keys = mList.selectedKeys();
                        mSelected = keys.empty() ? -1 : std::atoi(keys.front().c_str());
                        mController.onSelect();
                    }
                }
                else if (id == "btn_browse")
                {
                    mController.browse();
                }
                else if (id == "btn_import")
                {
                    mController.onImport();
                }
                else if (id == "btn_close")
                {
                    mClose = true;
                }
            }

            void unbind() override
            {
                mList.detach();
            }

        private:
            explicit ImportRmlFloater(const LLSD& key)
            :   RmlFloater(key), mController(*this)
            {
            }

            static RmlList::Cell cell(const std::string& text)
            {
                RmlList::Cell out;
                out.text = text;
                return out;
            }

            static void setText(Rml::Element* element, const std::string& text)
            {
                if (element)
                {
                    element->SetInnerRML(plus::rmlui::escapeRml(text));
                }
            }

            void touch()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            // A check box of the document: the attribute "checked" is what the click changes.
            bool checked(const char* id) const
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                Rml::Element* box = document ? document->GetElementById(id) : nullptr;
                return box && box->HasAttribute("checked");
            }

            // The XUI list's columns (the widths of the XUI window; its heading sorts on a click).
            std::vector<RmlList::Column> columns()
            {
                auto column = [this](const char* id, int width) {
                    RmlList::Column out;
                    out.id = id;
                    out.title = getString(std::string("col_") + id);
                    out.width_dp = width;
                    return out;
                };
                return { column("name", 170), column("folder", 0), column("date", 110) };
            }

            ImportController mController;
            RmlList mList;
            std::string mStatus;                        // the status text; in the document when !mStatusChanged
            bool mStatusChanged = false;
            int mSelected = -1;                         // the row selected: an index into the sources given
            std::string mIntro, mCloseLabel;
            bool mIntroChanged = false;
            bool mCloseLabelChanged = false;
            bool mImportEnabled = false;
            bool mImportEnabledChanged = false;
            bool mGraphicsChecked = false;
            bool mGraphicsChanged = false;
            bool mClose = false;
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_import_settings_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<ImportRmlFloater>);
    }
}
