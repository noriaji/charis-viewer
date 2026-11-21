/**
 * @file attachrml.cpp
 * @brief The attachments window made with RmlUi (research/386).
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

// The same window as avatarattach.cpp's (the XUI one, frozen: safe mode and PlusRmlUiWindows off), with its content
// app_settings/plus/rmlui/attachments.rml. What the window does is attachmodel.cpp's (shared, so that the two windows
// cannot differ); this class is the view: the list is a plus::rmlui::RmlList with columns, the status line a text, the
// buttons enabled from the selection. The names and creators are the simulator's and the name service's: they are put in
// as text (escaped by RmlList and escapeRml), never as RML.

#include "llviewerprecompiledheaders.h"

#include "attachmodel.h"
#include "attachrml.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"

#include "llfloaterreg.h"

#include <vector>

namespace plus::avatarattach
{
    namespace
    {
        using plus::rmlui::RmlList;

        // A HUD attachment has no distance: it sorts after the others (as the text "HUD" did in the XUI list).
        constexpr double HUD_SORT_VALUE = 1e9;

        RmlList::Cell makeCell(const std::string& text, double sort_value = 0.0)
        {
            RmlList::Cell cell;
            cell.text = text;
            cell.sort_value = sort_value;
            return cell;
        }

        class AttachmentsRmlFloater : public plus::rmlui::RmlFloater, public AttachmentsModel::View
        {
            friend class ::LLFloaterReg;

        public:
            ~AttachmentsRmlFloater() override
            {
                closeDocument();    // before the list goes
                mModel.stopAsking();
            }

            bool postBuild() override
            {
                const bool built = RmlFloater::postBuild();
                mModel.init();
                return built;
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                mModel.open(key.asUUID());
                mButtons = true;
            }

            void onClose(bool app_quitting) override
            {
                mModel.stopAsking();
                RmlFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mModel.tick())
                {
                    mButtons = true;
                    touch();
                }
                RmlFloater::draw();
            }

            // AttachmentsModel::View (called from open() and tick(), outside RmlUi's work: only values are kept here,
            // the document is changed in beforeUpdate())
            std::string text(const char* id, const LLStringUtil::format_map_t& args) override
            {
                return getString(id, args);
            }
            void setTitle(const std::string& title) override { LLFloater::setTitle(title); }
            void setStatus(const std::string& text) override
            {
                mStatus = text;
                mStatusChanged = true;
                touch();
            }
            void setEmptyText(const std::string& text) override
            {
                mList.setEmptyText(text);
                touch();
            }
            void clearRows() override
            {
                mList.setRows({});
                touch();
            }
            void showRows(const std::vector<Row>& now, const std::vector<Row>&, bool) override
            {
                std::vector<RmlList::Row> rows;
                rows.reserve(now.size());
                for (const Row& row : now)
                {
                    RmlList::Row& out = rows.emplace_back();
                    out.key = row.id.asString();
                    out.cells = { makeCell(row.point), makeCell(row.name), makeCell(row.creator),
                                  makeCell(std::to_string(row.prims), row.prims),
                                  makeCell(mModel.distanceText(row), row.distance < 0 ? HUD_SORT_VALUE : row.distance) };
                }
                mList.setRows(std::move(rows));
                touch();
            }

        protected:
            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mList.attach(document, "list", false);
                mList.attachHead(document, "list_head");
                mList.setColumns(columns());
                for (const char* event : { "click", "dblclick", "keydown", "mousedown" })
                {
                    view->listen(document.GetElementById("list"), event);
                }
                view->listen(document.GetElementById("list_head"), "click");
                for (const char* id : { "copy_name", "copy_id", "inspect", "refresh" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                mStatusChanged = true;
                mButtons = true;
            }

            void beforeUpdate() override
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                if (mStatusChanged)
                {
                    mStatusChanged = false;
                    if (Rml::Element* status = document->GetElementById("status"))
                    {
                        status->SetInnerRML(plus::rmlui::escapeRml(mStatus));
                    }
                }
                mList.apply();
                if (mButtons || mButtonVersion != mList.selectionVersion())
                {
                    mButtons = false;
                    mButtonVersion = mList.selectionVersion();
                    const AttachmentsModel::Buttons buttons = mModel.buttons(selectedId());
                    plus::rmlui::setElementDisabled(document->GetElementById("copy_name"), !buttons.copy_name);
                    plus::rmlui::setElementDisabled(document->GetElementById("copy_id"), !buttons.copy_id);
                    plus::rmlui::setElementDisabled(document->GetElementById("inspect"), !buttons.inspect);
                }
            }

            void afterUpdate() override
            {
                mList.afterUpdate();
            }

            // In RmlUi's event: no element is removed here (the rows change in beforeUpdate).
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
                if (id == "list" || id == "list_head")
                {
                    if (mList.handleEvent(event) == RmlList::Result::Activate)
                    {
                        mModel.inspect(LLUUID(mList.activatedKey()));
                    }
                }
                else if (id == "copy_name")
                {
                    mModel.copyName(selectedId());
                }
                else if (id == "copy_id")
                {
                    mModel.copyId(selectedId());
                }
                else if (id == "inspect")
                {
                    mModel.inspect(selectedId());
                }
                else if (id == "refresh")
                {
                    mModel.askAgain();
                }
                mButtons = true;    // follows at the next update
            }

            void unbind() override
            {
                mList.detach();
            }

        private:
            explicit AttachmentsRmlFloater(const LLSD& key)
            :   RmlFloater(key), mModel(*this)
            {
            }

            // The change is looked at at the next frame (RmlUi works only when something changed).
            void touch()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            LLUUID selectedId() const
            {
                const std::vector<std::string> keys = mList.selectedKeys();
                return keys.empty() ? LLUUID::null : LLUUID(keys.front());
            }

            // The XUI list's columns: the widths, the titles and tool tips (its heading sorts on a click; the prims
            // and the distance by their numbers).
            std::vector<RmlList::Column> columns()
            {
                auto column = [this](const char* id, int width, RmlList::SortKind sort, bool tip) {
                    RmlList::Column out;
                    out.id = id;
                    out.title = getString(std::string("col_") + id);
                    if (tip)
                    {
                        out.tip = getString(std::string("tip_") + id);
                    }
                    out.width_dp = width;
                    out.sort = sort;
                    return out;
                };
                return { column("point", 130, RmlList::SortKind::Text, false),
                         column("name", 0, RmlList::SortKind::Text, false),
                         column("creator", 120, RmlList::SortKind::Text, true),
                         column("prims", 56, RmlList::SortKind::Number, true),
                         column("distance", 64, RmlList::SortKind::Number, true) };
            }

            RmlList mList;
            AttachmentsModel mModel;
            std::string mStatus;
            bool mStatusChanged = false;
            bool mButtons = false;
            unsigned mButtonVersion = 0;
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_avatar_attachments_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<AttachmentsRmlFloater>);
    }
}
