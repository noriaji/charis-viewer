/**
 * @file grouptitlesrml.cpp
 * @brief The group titles window made with RmlUi (research/386): the content is app_settings/plus/rmlui/grouptitles.rml.
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

// The same window as grouptitlesfloater.cpp (frozen: safe mode and PlusRmlUiWindows off). The asking of the titles and the
// rows are GroupTitlesData's (grouptitlescore.cpp); this is the view: the rows into an RmlList (three columns, the active
// title bold, the heading sorts), the filter, the buttons that follow the selected row. The rows are built in
// beforeUpdate() (RmlUi's own turn, once however many replies came in the frame), never inside an event.

#include "llviewerprecompiledheaders.h"

#include "grouptitles.h"
#include "grouptitlescore.h"
#include "grouptitlesrml.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlsearch.h"

#include "llgroupactions.h"
#include "llfloaterreg.h"
#include "llmenugl.h"
#include "lltrans.h"

namespace plus::grouptitles
{
    namespace
    {
        using plus::rmlui::RmlList;

        constexpr int FILTER_MAX_BYTES = 60 * 4;    // grouptitles.rml's maxlength is 60 characters: at most 4 bytes each
        class GroupTitlesRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~GroupTitlesRmlFloater() override
            {
                closeDocument();    // before the list and the data go
                deleteClearMenu(mClearMenu);
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                mSyncNone = true;
                mSelectActive = true;
                mFocusList = true;
                mData.requestAll();
                wake();
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
                mList.setColumns({
                    // the title and the group both take what the region column leaves, equally (the XUI's dynamic_width)
                    { "title",  getString("col_title"),  std::string(), 0,   true, RmlList::SortKind::Text },
                    { "group",  getString("col_group"),  std::string(), 0,   true, RmlList::SortKind::Text },
                    { "region", getString("col_region"), getString("tip_col_region"), 150, true, RmlList::SortKind::Text },
                });
                mList.sortBy("title", true);
                for (const char* event : { "click", "dblclick", "keydown", "mousedown" })
                {
                    view->listen(document.GetElementById("list"), event);
                }
                view->listen(mList.headElement(), "click");
                view->listen(&document, "keydown");     // Ctrl+F: the filter
                mSearch.attach(*view, document, "filter", FILTER_MAX_BYTES);
                for (const char* id : { "btnSetRegion", "btnSetRegionManual", "btnClearRegion", "btnInfo",
                                        "btnActivate", "btnRefresh" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                view->listen(document.GetElementById("none"), "change");
                for (const char* id : { "btnSetRegion", "btnSetRegionManual", "btnClearRegion", "btnInfo", "btnActivate" })
                {
                    plus::rmlui::setElementDisabled(document.GetElementById(id), true);
                }
                mRebuild = true;
                mSyncNone = true;
                mButtonVersion = ~mList.selectionVersion();     // the first frame sets the buttons
                mButtonsDirty = true;
            }

            void beforeUpdate() override
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                if (mSyncNone)
                {
                    mSyncNone = false;
                    if (Rml::Element* none = document->GetElementById("none"))
                    {
                        if (noneOnUnassigned())
                        {
                            none->SetAttribute("checked", true);
                        }
                        else
                        {
                            none->RemoveAttribute("checked");
                        }
                    }
                }
                if (mRebuild)
                {
                    mRebuild = false;
                    rebuildRows();
                }
                mList.apply();
                updateButtons(*document);
                if (mFocusList)
                {
                    mFocusList = false;
                    if (Rml::Element* list = document->GetElementById("list"))
                    {
                        list->Focus();
                    }
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
                if (mSearch.handleEvent(event))
                {
                    if (mSearch.takeChanged())
                    {
                        filterTextChanged();
                    }
                    return;
                }
                const std::string id = element->GetId();
                const Rml::EventId type = event.GetId();
                if (id == "list" || id == "list_head")
                {
                    switch (mList.handleEvent(event))
                    {
                    case RmlList::Result::Selection:
                        mSelectActive = false;      // the user chose: kept over rebuilds
                        break;
                    case RmlList::Result::Activate:
                        if (const TitleRow* row = selectedRow(); row && GroupTitlesData::usable(row->group_id))
                        {
                            activateTitle(row->group_id, row->role_id);
                        }
                        break;
                    default:
                        break;
                    }
                }
                else if (id == "none")
                {
                    setNoneOnUnassigned(element->HasAttribute("checked"));
                }
                else if (type == Rml::EventId::Keydown)
                {
                    // Ctrl+F (anywhere in the window): the filter, as in Firestorm
                    if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_F && event.GetParameter<bool>("ctrl_key", false) &&
                        !event.GetParameter<bool>("alt_key", false) && !event.GetParameter<bool>("shift_key", false))
                    {
                        focusFilter();
                        event.StopPropagation();
                    }
                }
                else if (type == Rml::EventId::Click)
                {
                    if (plus::rmlui::isElementDisabled(element))
                    {
                        return;
                    }
                    onButton(id);
                }
            }

            void unbind() override
            {
                mList.detach();
                mSearch.detach();
            }

            // Ctrl+F also when the window has the focus but the document does not (its frame): the filter gets the
            // focus, as in Firestorm.
            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (key == 'F' && mask == MASK_CONTROL)
                {
                    focusFilter();
                    return true;
                }
                return RmlFloater::handleKeyHere(key, mask);
            }
            bool hasAccelerators() const override { return true; }

        private:
            explicit GroupTitlesRmlFloater(const LLSD& key)
            :   RmlFloater(key),
                mData([handle = getDerivedHandle<GroupTitlesRmlFloater>()]()
                      {
                          if (GroupTitlesRmlFloater* floater = handle.get())
                          {
                              floater->rebuildLater();
                          }
                      })
            {
            }

            void wake()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            void rebuildLater()
            {
                mRebuild = true;
                wake();
            }

            // The filter's text changed (SearchBox: change, the clear button): the list narrows.
            void filterTextChanged()
            {
                std::string upper = mSearch.text();
                LLStringUtil::trimHead(upper);
                LLStringUtil::toUpper(upper);
                if (upper != mFilterUpper)
                {
                    mFilterUpper = upper;
                    rebuildLater();
                }
            }

            // Focuses the filter field: the view gets the keyboard focus first, then the field (Ctrl+F).
            void focusFilter()
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                Rml::Element* filter = document ? document->GetElementById("filter") : nullptr;
                if (!filter)
                {
                    return;
                }
                view->setFocus(true);
                {
                    LLRmlView::Scope scope(view);
                    filter->Focus();
                }
                view->markDirty();
            }

            void onButton(const std::string& id)
            {
                if (id == "btnRefresh")
                {
                    mData.requestAll();
                    return;
                }
                const TitleRow* row = selectedRow();
                if (!row)
                {
                    return;
                }
                const LLUUID group_id = row->group_id;      // copies: a rebuild may come before the action ends
                const LLUUID role_id = row->role_id;
                if (id == "btnActivate")
                {
                    activateTitle(group_id, role_id);
                }
                else if (id == "btnInfo")
                {
                    if (group_id.notNull())
                    {
                        LLGroupActions::show(group_id);
                    }
                }
                else if (id == "btnSetRegion")
                {
                    setAssignmentForCurrentRegion(group_id, role_id);
                }
                else if (id == "btnSetRegionManual")
                {
                    askRegionName(group_id, role_id);
                }
                else if (id == "btnClearRegion")
                {
                    // One region: taken away at once. More: a menu of them and "all" (an XUI menu) opening where the XUI window's
                    // did: localPointToScreen(0, height) of the Clear Region button, its top-left corner, read from the button.
                    LLRmlView* view = host();
                    Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                    Rml::Element* button = document ? document->GetElementById("btnClearRegion") : nullptr;
                    if (!button)
                    {
                        return;
                    }
                    S32 local_x = 0, local_y = 0;
                    view->contextToLocal(Rml::Vector2f(button->GetAbsoluteLeft(), button->GetAbsoluteTop()), local_x, local_y);
                    S32 screen_x = 0, screen_y = 0;
                    view->localPointToScreen(local_x, local_y, &screen_x, &screen_y);
                    clearRegionsOf(group_id, role_id, getString("ClearAllRegions"), screen_x, screen_y, this, mClearMenu);
                }
            }

            const TitleRow* selectedRow() const
            {
                const std::vector<std::string> keys = mList.selectedKeys();
                if (keys.empty())
                {
                    return nullptr;
                }
                for (const TitleRow& row : mRows)
                {
                    if (row.key == keys.front())
                    {
                        return &row;
                    }
                }
                return nullptr;
            }

            void rebuildRows()
            {
                std::string keep;   // the row selected before, unless the active one is to be selected
                if (!mSelectActive)
                {
                    const std::vector<std::string> keys = mList.selectedKeys();
                    if (!keys.empty())
                    {
                        keep = keys.front();
                    }
                }
                TitleLabels labels;
                labels.no_title = getString("NoGroupTitle");
                labels.none_group = LLTrans::getString("GroupsNone");
                labels.unknown_title = getString("UnknownTitle");
                labels.not_member = getString("NotMember");
                std::string active_key;
                mRows = mData.rows(labels, mFilterUpper, active_key);

                std::vector<RmlList::Row> rows;
                rows.reserve(mRows.size());
                bool keep_there = false;
                for (const TitleRow& title : mRows)
                {
                    keep_there |= !keep.empty() && title.key == keep;
                    RmlList::Row row;
                    row.key = title.key;
                    row.pinned_first = !title.is_group;     // "<No group title>" stays first in either order (as the XUI's key "0")
                    row.cells.resize(3);
                    row.cells[0].text = title.title;
                    row.cells[1].text = title.group_name;
                    row.cells[2].text = title.regions;
                    for (RmlList::Cell& cell : row.cells)
                    {
                        cell.bold = title.active;
                    }
                    rows.push_back(std::move(row));
                }
                mList.setRows(std::move(rows));

                // Else the active title, once its row is there; the user's choice is kept from then on.
                std::string show = keep;
                if (!keep_there && !active_key.empty())
                {
                    mList.setSelectedKeys({ active_key });
                    mSelectActive = false;
                    show = active_key;
                }
                if (!show.empty())
                {
                    mList.ensureVisible(show);
                }
                mButtonsDirty = true;
            }

            void updateButtons(Rml::ElementDocument& document)
            {
                if (!mButtonsDirty && mButtonVersion == mList.selectionVersion())
                {
                    return;
                }
                mButtonsDirty = false;
                mButtonVersion = mList.selectionVersion();
                const TitleRow* row = selectedRow();
                const bool any = row != nullptr;
                // A row of a group the agent is not in can only have its regions cleared.
                const bool usable = any && GroupTitlesData::usable(row->group_id);
                auto set = [&](const char* id, bool enabled)
                {
                    plus::rmlui::setElementDisabled(document.GetElementById(id), !enabled);
                };
                set("btnActivate", usable);
                set("btnInfo", any && row->group_id.notNull());
                set("btnSetRegion", usable);
                set("btnSetRegionManual", usable);
                set("btnClearRegion", any && !regionsForTitle(row->group_id, row->role_id).empty());
            }

            plus::rmlui::RmlList mList;
            std::vector<TitleRow> mRows;        // what the list shows (for the selected row's ids)
            plus::rmlui::SearchBox mSearch;
            std::string mFilterUpper;           // the filter in upper case (empty: all rows)
            bool mSelectActive = true;          // select the active title once its row is there (opening)
            bool mRebuild = false;
            bool mSyncNone = false;
            bool mFocusList = false;
            bool mButtonsDirty = false;
            unsigned mButtonVersion = 0;
            LLHandle<LLContextMenu> mClearMenu;
            GroupTitlesData mData;              // last: its signals go first when the window goes
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_group_titles_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<GroupTitlesRmlFloater>);
    }
}
