/**
 * @file protectedfoldersrml.cpp
 * @brief Protected folders: the window made with RmlUi (research/386).
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

// The same window as protectedfoldersfloater.cpp's (the XUI one, frozen: used in safe mode and with PlusRmlUiWindows
// off), with its content app_settings/plus/rmlui/protected_folders.rml. The list is a plus::rmlui::RmlList; this class
// is what the XUI floater did: the rows from the folders and the filter, the selection kept over a rebuild, a
// double click showing the folder in the inventory, the remove button's enabling, and the look again every
// RETRY_SECONDS for folders the inventory lacked (shown in italics).

#include "llviewerprecompiledheaders.h"

#include "protectedfolders.h"
#include "protectedfoldersrml.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlsearch.h"

#include "llfloaterreg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lltimer.h"

namespace plus::protectedfolders
{
    namespace
    {
        constexpr F32 RETRY_SECONDS = 2.f;          // how often to look again for folders the inventory lacked
        constexpr int FILTER_MAX_BYTES = 256 * 4;   // protected_folders.rml's maxlength (256 characters, up to 4 bytes each: SearchBox counts bytes)

        class ProtectedFoldersRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~ProtectedFoldersRmlFloater() override
            {
                closeDocument();    // before the list and the connection go
                mChangedConnection.disconnect();
            }

            bool postBuild() override
            {
                mUnknownText = getString("unknown_folder");
                mChangedConnection = onChanged([this]() { requestRefresh(); });
                return RmlFloater::postBuild();
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                requestRefresh();
            }

            void draw() override
            {
                // Folders the inventory did not have yet: look again now and then.
                if (mHasUnknown && mRetryTimer.getElapsedTimeF32() >= RETRY_SECONDS)
                {
                    requestRefresh();
                }
                RmlFloater::draw();
            }

        protected:
            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mList.attach(document, "list", true);
                mList.attachHead(document, "list_head");
                mList.setColumns({ { "name", getString("folder"), std::string(), 0, true, plus::rmlui::RmlList::SortKind::Text } });
                mList.sortBy("name", true);     // sort_column="0" sort_ascending="true"; a click on the heading reverses
                for (const char* event : { "click", "dblclick", "keydown", "mousedown" })
                {
                    view->listen(document.GetElementById("list"), event);
                }
                view->listen(mList.headElement(), "click");
                mSearch.attach(*view, document, "filter", FILTER_MAX_BYTES);
                view->listen(document.GetElementById("remove"), "click");
                mButtonVersion = ~mList.selectionVersion();     // the first frame sets the button
                mRefresh = true;
            }

            void beforeUpdate() override
            {
                if (mRefresh)
                {
                    mRefresh = false;
                    rebuildRows();
                }
                mList.apply();
                updateRemoveButton();
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
                if (mSearch.handleEvent(event))
                {
                    if (mSearch.takeChanged())
                    {
                        requestRefresh();       // the list narrows
                    }
                    return;
                }
                const std::string id = element->GetId();
                const Rml::EventId type = event.GetId();
                if (type == Rml::EventId::Click && plus::rmlui::isElementDisabled(element))
                {
                    return;     // a disabled button takes no click (RmlUi 6.3 does not stop it)
                }
                if (id == "list" || id == "list_head")
                {
                    if (mList.handleEvent(event) == plus::rmlui::RmlList::Result::Activate)
                    {
                        const LLUUID folder(mList.activatedKey());
                        if (folder.notNull() && gInventory.getCategory(folder))
                        {
                            show_item_original(folder);
                        }
                    }
                }
                else if (id == "remove")
                {
                    std::vector<LLUUID> ids;
                    for (const std::string& key : mList.selectedKeys())
                    {
                        ids.emplace_back(key);
                    }
                    unprotect(ids);     // the list is made again by the change
                }
            }

            void unbind() override
            {
                mChangedConnection.disconnect();
                mSearch.detach();
                mList.detach();
            }

        private:
            explicit ProtectedFoldersRmlFloater(const LLSD& key)
            :   RmlFloater(key)
            {
            }

            void requestRefresh()
            {
                mRefresh = true;
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            void rebuildRows()
            {
                mRetryTimer.reset();
                mHasUnknown = false;

                std::string needle = mSearch.text();
                LLStringUtil::trim(needle);
                LLStringUtil::toLower(needle);

                std::vector<plus::rmlui::RmlList::Row> rows;
                for (const LLUUID& id : folders())
                {
                    LLViewerInventoryCategory* cat = gInventory.getCategory(id);
                    const std::string name = cat ? cat->getName() : mUnknownText;
                    if (!cat)
                    {
                        mHasUnknown = true;
                    }
                    if (!needle.empty())
                    {
                        std::string lower = name;
                        LLStringUtil::toLower(lower);
                        if (lower.find(needle) == std::string::npos)
                        {
                            continue;
                        }
                    }
                    plus::rmlui::RmlList::Row row;
                    row.key = id.asString();
                    row.cells.resize(1);
                    row.cells[0].text = name;
                    row.cells[0].italic = !cat;
                    rows.push_back(std::move(row));
                }
                mList.setRows(std::move(rows));
            }

            void updateRemoveButton()
            {
                if (mButtonVersion == mList.selectionVersion())
                {
                    return;
                }
                mButtonVersion = mList.selectionVersion();
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                plus::rmlui::setElementDisabled(document ? document->GetElementById("remove") : nullptr,
                                                mList.selectedCount() == 0);
            }

            plus::rmlui::RmlList mList;
            plus::rmlui::SearchBox mSearch;     // the filter field, its hint and clear button
            std::string mUnknownText;
            bool mRefresh = false;
            bool mHasUnknown = false;
            unsigned mButtonVersion = 0;
            LLTimer mRetryTimer;
            boost::signals2::connection mChangedConnection;
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_protected_folders_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<ProtectedFoldersRmlFloater>);
    }
}
