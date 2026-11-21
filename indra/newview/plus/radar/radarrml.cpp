/**
 * @file radarrml.cpp
 * @brief The radar window made with RmlUi (research/386).
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

// The same window as radarfloater.cpp's (the XUI one, frozen: safe mode and PlusRmlUiWindows off), with its content
// app_settings/plus/rmlui/radar.rml. What the cells say, how a row is styled, the sort keys, the filter and the actions
// are radarui.cpp's (shared, so that the two windows cannot differ). This class is the view: the list is a
// plus::rmlui::RmlList with columns (the columns shown are their visible flags; a click on a heading sorts), the filter a
// plus::rmlui::SearchBox, the settings tab's boxes a plus::rmlui::SettingsModel, the name format a <select> (its list is
// shown outside the window by RmlFloater's SelectPopup), the range the host's number box (.ps-spin: arrows, long press,
// wheel, Up/Down, Esc, Enter; the window gets its "commit" and writes PlusRadarRange). The names come from the name
// service (the engine's rows) and are put in as text (escaped by RmlList), never as RML; a name too long for its column
// ends with "..." (the host's, .ps-cell). The list's right-click menu is the XUI window's (menu_plus_radar.xml, opened with
// plus::rmlui::ContextMenu; Plus.Radar.Action and Plus.Radar.Enable of radarui.h).

#include "llviewerprecompiledheaders.h"

#include "radar.h"
#include "radarrml.h"
#include "radarui.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlmenu.h"
#include "plus/rmlui/rmlsearch.h"
#include "plus/rmlui/rmlsettings.h"

#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

#include <boost/signals2/connection.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace plus::radar
{
    namespace
    {
        using plus::rmlui::RmlList;
        using plus::rmlui::SettingsModel;

        constexpr char SETTING_COLUMNS[]  = "PlusRadarColumns";
        constexpr char SETTING_NAME_FMT[] = "PlusRadarNameFormat";
        constexpr char SETTING_RANGE[]    = "PlusRadarRange";
        // The XUI search editor's max_length_chars (256), in the bytes SearchBox counts (a character is up to 4 bytes of
        // UTF-8; a Japanese filter is cut at 256 characters, not about 85).
        constexpr int  FILTER_MAX_BYTES   = 256 * 4;
        // The XUI spinner's: PlusRadarRange between these (radar.rml's number box has the same min and max, step 10).
        constexpr S32  RANGE_MIN  = 10;
        constexpr S32  RANGE_MAX  = 4096;
        constexpr int  NAME_FORMATS = 4;

        unsigned channel(F32 value)
        {
            return (unsigned)(llclamp(value, 0.f, 1.f) * 255.f + 0.5f);
        }

        unsigned rgb(const LLColor4& colour)
        {
            return (channel(colour.mV[VRED]) << 16) | (channel(colour.mV[VGREEN]) << 8) | channel(colour.mV[VBLUE]);
        }

        class RadarRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~RadarRmlFloater() override
            {
                closeDocument();    // before the list, the model and the connections go
                if (Engine::instanceExists())
                {
                    Engine* engine = Engine::getInstance();
                    engine->setListener(nullptr);
                    engine->setWindowOpen(false);
                }
            }

            bool postBuild() override
            {
                // The marks before the document loads (its first update() makes the rows with them).
                mMarks.parcel = getString("mark_parcel");
                mMarks.region = getString("mark_region");
                mMarks.sitting = getString("mark_sitting");
                mMarks.age_hidden = getString("age_hidden");
                mMarks.age_unknown = getString("age_unknown");
                const bool built = RmlFloater::postBuild();
                Engine* engine = Engine::getInstance();
                engine->setListener([this]() { update(); });
                engine->setWindowOpen(true);
                return built;
            }

            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (mask == MASK_CONTROL && key == 'C')
                {
                    ui::copyToClipboard(selectedIds(), false);
                    return true;
                }
                if (mask == MASK_CONTROL && key == 'A')
                {
                    mList.selectEverything();
                    touch();
                    return true;
                }
                return LLFloater::handleKeyHere(key, mask);     // (Esc in the range field is the number box's: it puts the text back)
            }

        protected:
            void beforeLoad(Rml::Context& context) override
            {
                Rml::DataModelConstructor model = mModel.create(context, "radar");
                if (!model)
                {
                    return;
                }
                using Kind = SettingsModel::Kind;
                LLControlGroup* group = &gSavedSettings;
                const std::vector<SettingsModel::Entry> entries = {
                    { "limit_range",         group, "PlusRadarLimitRange",       Kind::Bool, 0 },
                    { "show_muted",          group, "PlusRadarShowMuted",        Kind::Bool, 0 },
                    { "report_chat_enter",   group, "PlusRadarReportChatEnter",  Kind::Bool, 0 },
                    { "report_chat_leave",   group, "PlusRadarReportChatLeave",  Kind::Bool, 0 },
                    { "report_draw_enter",   group, "PlusRadarReportDrawEnter",  Kind::Bool, 0 },
                    { "report_draw_leave",   group, "PlusRadarReportDrawLeave",  Kind::Bool, 0 },
                    { "report_region_enter", group, "PlusRadarReportRegionEnter", Kind::Bool, 0 },
                    { "report_region_leave", group, "PlusRadarReportRegionLeave", Kind::Bool, 0 },
                    { "range",               group, SETTING_RANGE,               Kind::Int,  0, RANGE_MIN, RANGE_MAX },   // shown in the number box
                };
                mModel.bind(model, entries);
            }

            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mList.attach(document, "list", true);
                mList.attachHead(document, "list_head");
                mList.setColumns(columns());
                mList.sortBy("range", true);
                fillNameFormats(document);      // before the listener: filling the box must not write the setting
                mList.setSearchColumn("name");      // (the XUI list's search_column="1" meant the name of Firestorm's list)
                for (const char* event : { "click", "keydown" })
                {
                    view->listen(document.GetElementById("list"), event);
                }
                view->listen(document.GetElementById("list"), "dblclick");
                view->listen(document.GetElementById("list"), "mousedown");     // the left press selects, the right one: Result::ContextMenu
                mList.setCopyHandled(true);         // Ctrl+C in the list copies the names (below), not the cells
                view->listen(document.GetElementById("list_head"), "click");
                mSearch.attach(*view, document, "filter", FILTER_MAX_BYTES);

                for (const char* id : { "tab_list", "tab_settings" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                view->listen(document.GetElementById("name_format"), "change");
                view->listen(document.GetElementById("spin"), "commit");    // the number box (rmlparts.h 5.)
                view->listen(document.GetElementById("range"), "focus");    // the model keeps a change from outside back while the
                view->listenCapture(document.GetElementById("range"), "blur");     // field is held (after the host's own commit on blur)
                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                for (const ui::ColumnDef& column : ui::COLUMNS)
                {
                    if (!column.bit)
                    {
                        continue;
                    }
                    Rml::Element* check = document.GetElementById(std::string("c_") + column.name);
                    if (check && (shown & column.bit))
                    {
                        check->SetAttribute("checked", Rml::String());
                    }
                    view->listen(check, "change");
                }
                // A change of who is listed shows at the next sweep; ask for it now, without reports (those who come into
                // the list by it did not come in). The XUI boxes did the same on their commit.
                for (int i = 0; i < 2; ++i)
                {
                    LLControlVariable* control = gSavedSettings.getControl(i == 0 ? "PlusRadarLimitRange" : "PlusRadarShowMuted");
                    if (control)
                    {
                        mSweepConnections[i] = control->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) {
                            Engine::getInstance()->sweepQuietly();
                        });
                    }
                }
                mCounterChanged = true;
                update();
            }

            void beforeUpdate() override
            {
                mModel.beforeUpdate();
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                if (mCounterChanged)
                {
                    mCounterChanged = false;
                    if (Rml::Element* counter = document->GetElementById("counter"))
                    {
                        counter->SetInnerRML(plus::rmlui::escapeRml(mCounter));
                    }
                }
                mList.apply();
                if (mFormatsRefill)
                {
                    mFormatsRefill = false;
                    fillNameFormats(*document);     // a real format was chosen: the empty stand-in goes
                }
            }

            void afterUpdate() override
            {
                mList.afterUpdate();
            }

            // In RmlUi's event: no element is removed here (the rows change in beforeUpdate).
            void onEvent(Rml::Event& event) override
            {
                if (mSearch.handleEvent(event))
                {
                    if (mSearch.takeChanged())
                    {
                        mFilter = mSearch.text();
                        LLStringUtil::toLower(mFilter);
                        update();
                    }
                    return;
                }
                const Rml::Element* element = event.GetCurrentElement();
                if (!element)
                {
                    return;
                }
                const std::string id = element->GetId();
                if (id == "list" || id == "list_head")
                {
                    const RmlList::Result result = mList.handleEvent(event);
                    if (result == RmlList::Result::Activate)
                    {
                        onDoubleClick();
                    }
                    else if (result == RmlList::Result::Copy)
                    {
                        ui::copyToClipboard(selectedIds(), false);      // (as the XUI window: the names only)
                    }
                    else if (result == RmlList::Result::ContextMenu)
                    {
                        event.StopPropagation();    // the menu is the window's (the row is selected by the press already)
                        showMenu(event);
                    }
                }
                else if (id == "tab_list" || id == "tab_settings")
                {
                    showTab(id == "tab_settings");
                }
                else if (id == "name_format")
                {
                    auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(event.GetCurrentElement());
                    if (select && !mFillingFormats)
                    {
                        const std::string value = select->GetValue();
                        if (!value.empty())     // (the empty stand-in for a value that is not among the formats is no choice)
                        {
                            gSavedSettings.setU32(SETTING_NAME_FMT, (U32)std::atoi(value.c_str()));
                            mFormatsRefill = mHasStandIn;   // the setting is a format now: the stand-in is taken out
                        }
                    }
                }
                else if (id == "range")
                {
                    // The field's focus and blur: the number shown stays what the user sees while the field is held; at the blur
                    // (after the host committed what was typed) a change from outside meanwhile shows now.
                    if (event.GetId() == Rml::EventId::Focus && event.GetTargetElement() == element)
                    {
                        mModel.beginEdit("range");
                    }
                    else if (event.GetId() == Rml::EventId::Blur && event.GetTargetElement() == element)
                    {
                        mModel.endEdit("range", std::string());
                    }
                }
                else if (id == "spin")
                {
                    // The number box committed (Enter, the focus left after typing, an arrow, the wheel, Up/Down): the model
                    // writes the setting and shows it in the box (a change from elsewhere shows when the field is not held,
                    // else at its blur).
                    setRange((F32)event.GetParameter<double>("value", (double)gSavedSettings.getF32(SETTING_RANGE)));
                }
                else if (id.compare(0, 2, "c_") == 0)
                {
                    onColumnBox(id.substr(2), event.GetCurrentElement());
                }
                touch();
            }

            void unbind() override
            {
                mMenu.hide();
                mSearch.detach();
                mList.detach();
                mModel.unbind();
                mSweepConnections[0].disconnect();
                mSweepConnections[1].disconnect();
            }

        private:
            explicit RadarRmlFloater(const LLSD& key)
            :   RmlFloater(key), mModel([this]() { touch(); })
            {
            }

            void touch()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            // The name formats into the box, the stored one selected. A stored value that is not one of them (the XUI combo
            // shows nothing then and writes nothing): an empty stand-in first, selected; the setting stays as it is.
            void fillNameFormats(Rml::ElementDocument& document)
            {
                auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document.GetElementById("name_format"));
                if (!select)
                {
                    return;
                }
                std::vector<plus::rmlui::SelectOption> options;
                const U32 stored = gSavedSettings.getU32(SETTING_NAME_FMT);
                mHasStandIn = stored >= (U32)NAME_FORMATS;
                if (mHasStandIn)
                {
                    options.push_back({ std::string(), std::string() });
                }
                for (int i = 0; i < NAME_FORMATS; ++i)
                {
                    options.push_back({ std::to_string(i), getString(std::string("fmt_") + std::to_string(i)) });
                }
                mFillingFormats = true;     // (filling the box gives change events: they are not the user's)
                plus::rmlui::fillSelect(*select, options, mHasStandIn ? std::string() : std::to_string(stored));
                mFillingFormats = false;
            }

            // The tabs: the list and the settings (only classes change, in the event).
            void showTab(bool settings)
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                const char* const panels[] = { "panel_list", "panel_settings" };
                const char* const tabs[] = { "tab_list", "tab_settings" };
                for (int i = 0; i < 2; ++i)
                {
                    const bool shown = (i == 1) == settings;
                    if (Rml::Element* panel = document->GetElementById(panels[i]))
                    {
                        panel->SetClass("hidden", !shown);
                    }
                    if (Rml::Element* tab = document->GetElementById(tabs[i]))
                    {
                        tab->SetClass("active", shown);
                    }
                }
            }

            // The spinner's value (the XUI spinner: 10 to 4096, whole numbers). The number box has clamped and rounded it.
            void setRange(F32 value)
            {
                mModel.commitInt("range", (S32)llclamp((F32)ll_round(value), (F32)RANGE_MIN, (F32)RANGE_MAX));
            }

            void onColumnBox(const std::string& name, Rml::Element* box)
            {
                for (const ui::ColumnDef& column : ui::COLUMNS)
                {
                    if (column.bit && name == column.name && box)
                    {
                        const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                        const bool show = box->HasAttribute("checked");
                        gSavedSettings.setU32(SETTING_COLUMNS, show ? (shown | column.bit) : (shown & ~column.bit));
                        mList.setColumns(columns());
                        return;
                    }
                }
            }

            uuid_vec_t selectedIds() const
            {
                uuid_vec_t ids;
                for (const std::string& key : mList.selectedKeys())
                {
                    ids.emplace_back(key);
                }
                return ids;
            }

            void onDoubleClick()
            {
                const uuid_vec_t ids = selectedIds();
                if (ids.size() == 1)
                {
                    tell(ui::zoomToId(ids.front()));
                }
            }

            void notify(const char* string_id)
            {
                LLSD args;
                args["MESSAGE"] = getString(string_id);
                LLNotificationsUtil::add("SystemMessageTip", args);
            }

            void tell(ui::Notice notice)
            {
                if (notice == ui::Notice::NoFocus)
                {
                    notify("no_focus");
                }
                else if (notice == ui::Notice::NoPosition)
                {
                    notify("no_position");
                }
            }

            // The list's right-click menu, as the XUI window's (radarfloater.cpp showMenu): menu_plus_radar.xml at the click,
            // its items enabled by ui::menuEnabled for the selected avatars (the row under the press was selected first).
            void showMenu(Rml::Event& event)
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mMenu.hide();
                // RLVa @shownames: no menu for an avatar whose name is hidden (FSRadarListCtrl::handleRightMouseDown()):
                // its profile, IM, map, copied id and the rest would tell who it is.
                for (const LLUUID& id : selectedIds())
                {
                    if (ui::nameHidden(id))
                    {
                        return;
                    }
                }
                LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
                LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
                registrar.add("Plus.Radar.Action", [this](LLUICtrl*, const LLSD& param) { onMenuAction(param.asString()); });
                enable_registrar.add("Plus.Radar.Enable", [this](LLUICtrl*, const LLSD& param) {
                    return ui::menuEnabled(param.asString(), selectedIds());
                });
                if (mMenu.create("menu_plus_radar.xml"))
                {
                    mMenu.open(*view, Rml::Vector2f(event.GetParameter<float>("mouse_x", 0.f),
                                                    event.GetParameter<float>("mouse_y", 0.f)));
                }
            }

            void onMenuAction(const std::string& action)
            {
                if (action == "select_all")
                {
                    mList.selectEverything();
                    touch();
                    return;
                }
                tell(ui::runAction(action, selectedIds()));
            }

            // The XUI list's columns: the widths, the titles and tool tips; the columns off in PlusRadarColumns are hidden.
            std::vector<RmlList::Column> columns()
            {
                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                std::vector<RmlList::Column> out;
                for (const ui::ColumnDef& def : ui::COLUMNS)
                {
                    RmlList::Column column;
                    column.id = def.name;
                    column.title = getString(std::string("col_") + def.name);
                    const std::string tip_name = std::string("tip_") + def.name;
                    if (hasString(tip_name))
                    {
                        column.tip = getString(tip_name);
                    }
                    column.width_dp = def.width;
                    column.visible = !def.bit || (shown & def.bit);
                    const std::string name = def.name;
                    column.sort = (name == "range" || name == "age" || name == "seen") ? RmlList::SortKind::Number
                                                                                       : RmlList::SortKind::Text;
                    out.push_back(column);
                }
                return out;
            }

            static RmlList::Cell makeCell(const std::string& text, const ui::Style* style, double sort_value)
            {
                RmlList::Cell cell;
                cell.text = text;
                cell.sort_value = sort_value;
                if (style)
                {
                    cell.bold = style->bold;
                    cell.italic = style->italic;
                    cell.has_colour = style->coloured;
                    cell.colour = rgb(style->colour);
                }
                return cell;
            }

            // After each sweep (and when the filter changes): the rows of those who are listed, the counter. The list
            // keeps the selection and the scroll over a change of rows.
            void update()
            {
                if (!Engine::instanceExists())
                {
                    return;
                }
                const Engine* engine = Engine::getInstance();
                std::vector<RmlList::Row> rows;
                for (const Entry* entry : engine->rows())
                {
                    if (!ui::matchesFilter(*entry, mFilter))
                    {
                        continue;
                    }
                    RmlList::Row& row = rows.emplace_back();
                    row.key = entry->id.asString();
                    const ui::Style name_style = ui::nameStyle(*entry);
                    const ui::Style range_style = ui::rangeStyle(*entry);
                    row.cells.reserve(ui::COLUMN_COUNT);
                    for (size_t column = 0; column < ui::COLUMN_COUNT; ++column)
                    {
                        const std::string text = ui::cellText(*entry, column, mMarks);
                        double sort_value = 0.0;
                        if (column == ui::COL_RANGE || column == ui::COL_AGE)
                        {
                            sort_value = ui::sortNumber(text);
                        }
                        else if (column == ui::COL_SEEN)
                        {
                            sort_value = ui::sortSeconds(text);
                        }
                        row.cells.push_back(makeCell(text, column == ui::COL_NAME ? &name_style
                                                           : (column == ui::COL_RANGE ? &range_style : nullptr), sort_value));
                    }
                }
                mList.setRows(std::move(rows));

                LLStringUtil::format_map_t args;
                ui::counterArgs(engine->stats(), args);
                const std::string counter = getString("counter", args);
                if (counter != mCounter)
                {
                    mCounter = counter;
                    mCounterChanged = true;
                }
                touch();
            }

            SettingsModel mModel;
            RmlList mList;
            plus::rmlui::SearchBox mSearch;
            ui::Marks mMarks;
            std::string mFilter;                    // lower case
            std::string mCounter;
            bool mCounterChanged = false;
            bool mHasStandIn = false;       // the name-format box has the empty stand-in (the setting is none of the formats)
            bool mFormatsRefill = false;    // the box is filled again at the next update (without the stand-in)
            bool mFillingFormats = false;   // the window fills the box: its change events are not the user's
            boost::signals2::scoped_connection mSweepConnections[2];
            plus::rmlui::ContextMenu mMenu;         // the list's menu (closed by unbind() and its own destructor)
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_radar_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<RadarRmlFloater>);
    }
}
