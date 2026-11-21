/**
 * @file radarfloater.cpp
 * @brief Radar: the window (the list of the avatars around, the settings) and the list's menu.
 *
 * Ported from Firestorm's fspanelradar.cpp, fsfloaterradar.cpp, fsradarmenu.cpp and
 * fsradarlistctrl.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Copyright (c) 2011 Arrehn Oberlander
 * Copyright (c) 2013 Ansariel Hiller @ Second Life
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Menu: "Radar" in menu_plus.xml (Floater.Toggle plus_radar). The window is destroyed when
// closed (reuse_instance false); the engine keeps sweeping afterwards only while a report is
// switched on (radar.cpp).
//
// Settings (gSavedSettings, shared by every account):
//   PlusRadarColumns           the columns shown (bits, ColumnDef::bit); the name is always shown
//   PlusRadarNameFormat, PlusRadarLimitRange, PlusRadarRange, PlusRadarShowMuted,
//   PlusRadarReport*           see radar.cpp; the settings tab has a control for each
//
// The name is in its contact set's colour with FSContactSetsColorizeRadar on (plus/contactsethooks,
// research/333). Left out of Firestorm's: the minimap above the list, the marks on the minimap, the
// voice level, notes and age alert columns, derendering and the actions for land and estate managers
// (the official viewer has none of these actions to call).

#include "llviewerprecompiledheaders.h"

#include "radar.h"
#include "radarui.h"
#include "radarrml.h"
#include "plus.h"
#include "plus/safemode/safemode.h"

#include <cstdlib>
#include <unordered_map>

#include "llagent.h"
#include "llagentcamera.h"
#include "llavataractions.h"
#include "llcallingcard.h"
#include "llcheckboxctrl.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsearcheditor.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

namespace plus::radar
{
    namespace
    {
        constexpr char FLOATER_NAME[]     = "plus_radar";
        constexpr char SETTING_COLUMNS[]  = "PlusRadarColumns";
        constexpr char SETTING_NAME_FMT[] = "PlusRadarNameFormat";

        using ui::COLUMNS;
        using ui::ColumnDef;
        using ui::nameHidden;

        // The list, with a menu of the floater's on a right click.
        class AvatarList : public LLScrollListCtrl
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLScrollListCtrl::Params> {};

            void setRightClickCallback(std::function<void(S32, S32)> callback) { mRightClick = std::move(callback); }

            bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
            {
                LLScrollListItem* item = hitItem(x, y);
                if (!item)
                {
                    return false;
                }
                // A row outside the selection becomes the selection.
                if (!item->getSelected())
                {
                    deselectAllItems(true);
                    selectByID(item->getUUID());
                }
                if (mRightClick)
                {
                    mRightClick(x, y);
                }
                return true;
            }

        protected:
            friend class ::LLUICtrlFactory;
            explicit AvatarList(const Params& p) : LLScrollListCtrl(p) {}

        private:
            std::function<void(S32, S32)> mRightClick;
        };

        LLDefaultChildRegistry::Register<AvatarList> sAvatarListRegistration("plus_radar_list");

        class RadarFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                mMarks.parcel = getString("mark_parcel");
                mMarks.region = getString("mark_region");
                mMarks.sitting = getString("mark_sitting");
                mMarks.age_hidden = getString("age_hidden");
                mMarks.age_unknown = getString("age_unknown");
                mList    = getChild<AvatarList>("radar_list");
                mCounter = getChild<LLTextBox>("counter");

                mList->setDoubleClickCallback([this]() { onDoubleClick(); });
                mList->setRightClickCallback([this](S32 x, S32 y) { showMenu(x, y); });
                mList->setSortCallback([this](S32 column, const LLScrollListItem* a, const LLScrollListItem* b) {
                    return compareCells(column, a, b);
                });
                buildColumns();
                mList->sortByColumn("range", true);

                mQuickFilter = getChild<LLSearchEditor>("quick_filter");
                mQuickFilter->setKeystrokeCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    mFilter = ctrl->getValue().asString();
                    LLStringUtil::toLower(mFilter);
                    update();
                });

                // The name format, and the columns.
                LLComboBox* format = getChild<LLComboBox>("name_format");
                format->selectByValue(LLSD(std::to_string(gSavedSettings.getU32(SETTING_NAME_FMT))));
                format->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    gSavedSettings.setU32(SETTING_NAME_FMT, (U32)ctrl->getValue().asInteger());
                });
                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                for (const ColumnDef& column : COLUMNS)
                {
                    if (!column.bit)
                    {
                        continue;
                    }
                    LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(std::string("col_") + column.name);
                    check->set((shown & column.bit) != 0);
                    check->setCommitCallback([this, bit = column.bit](LLUICtrl* ctrl, const LLSD&) {
                        showColumn(bit, ctrl->getValue().asBoolean());
                    });
                }
                // A change of the list's range or of who is listed shows at the next sweep; ask for it
                // now, without reports (those who come into the list by it did not come in).
                for (const char* name : { "limit_range", "show_muted" })
                {
                    getChild<LLUICtrl>(name)->setCommitCallback([](LLUICtrl*, const LLSD&) {
                        Engine::getInstance()->sweepQuietly();
                    });
                }

                Engine* engine = Engine::getInstance();
                engine->setListener([this]() { update(); });
                engine->setWindowOpen(true);
                return true;
            }

            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (mask == MASK_CONTROL && key == 'C')
                {
                    copyToClipboard(false);
                    return true;
                }
                if (mask == MASK_CONTROL && key == 'A')
                {
                    mList->selectAll();
                    return true;
                }
                return LLFloater::handleKeyHere(key, mask);
            }

        private:
            explicit RadarFloater(const LLSD& key) : LLFloater(key) {}

            ~RadarFloater() override
            {
                if (LLContextMenu* menu = mMenuHandle.get())
                {
                    menu->die();
                }
                if (Engine::instanceExists())
                {
                    Engine* engine = Engine::getInstance();
                    engine->setListener(nullptr);
                    engine->setWindowOpen(false);
                }
            }

            // --- the list ---

            void buildColumns()
            {
                const std::string sort_column = mList->getSortColumnName();
                const bool sort_ascending     = mList->getSortAscending();
                mList->deleteAllItems();
                mSetColoredRows.clear();
                mList->clearColumns();

                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                for (const ColumnDef& column : COLUMNS)
                {
                    if (column.bit && !(shown & column.bit))
                    {
                        continue;
                    }
                    LLScrollListColumn::Params params;
                    params.name                = column.name;
                    params.header.label        = getString(std::string("col_") + column.name);
                    params.width.pixel_width   = column.width;
                    const std::string tip_name = std::string("tip_") + column.name;
                    if (hasString(tip_name))
                    {
                        params.tool_tip = getString(tip_name);
                    }
                    mList->addColumn(params);
                }
                if (!sort_column.empty() && mList->getColumn(sort_column))
                {
                    mList->sortByColumn(sort_column, sort_ascending);
                }
                mList->dirtyColumns();
                mList->updateColumns(true);
            }

            void showColumn(U32 bit, bool show)
            {
                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                gSavedSettings.setU32(SETTING_COLUMNS, show ? (shown | bit) : (shown & ~bit));
                buildColumns();
                update();
            }

            std::string cellText(const Entry& entry, const std::string& column) const
            {
                return ui::cellText(entry, column, mMarks);
            }

            // Colors and styles of a row: the range by how far one is heard, friends bold, the muted in italic.
            void styleRow(LLScrollListItem* item, const Entry& entry)
            {
                const LLColor4 other_color = ui::listTextColour();
                for (S32 i = 0; i < item->getNumColumns(); ++i)
                {
                    LLScrollListText* cell = static_cast<LLScrollListText*>(item->getColumn(i));
                    const std::string& name = mList->getColumn(i)->mName;
                    if (name == "name")
                    {
                        const ui::Style style = ui::nameStyle(entry);
                        cell->setFontStyle(static_cast<U8>((style.bold ? LLFontGL::BOLD : 0) | (style.italic ? LLFontGL::ITALIC : 0)));
                        // A cell keeps a colour once given one (LLScrollListText), so a row that had a set colour and has none
                        // now gets the list's text colour; the others keep the list's own (selected rows differ).
                        if (style.coloured)
                        {
                            cell->setColor(style.colour);
                            mSetColoredRows.insert(entry.id);
                        }
                        else if (mSetColoredRows.erase(entry.id))
                        {
                            cell->setColor(other_color);
                        }
                    }
                    else if (name == "range")
                    {
                        const ui::Style style = ui::rangeStyle(entry);
                        cell->setColor(style.coloured ? style.colour : other_color);
                        cell->setFontStyle(static_cast<U8>(style.bold ? LLFontGL::BOLD : LLFontGL::NORMAL));
                    }
                }
            }

            bool matchesFilter(const Entry& entry) const { return ui::matchesFilter(entry, mFilter); }

            // After each sweep: rows of those who left go, new ones come, the others change in
            // place (so the selection and the scroll stay).
            void update()
            {
                if (!mList || !Engine::instanceExists())
                {
                    return;
                }
                const Engine* engine = Engine::getInstance();

                uuid_set_t listed;
                for (const Entry* entry : engine->rows())
                {
                    if (matchesFilter(*entry))
                    {
                        listed.insert(entry->id);
                    }
                }
                // The rows that stay, by id (the list's own lookup goes through every row,
                // comparing the ids as text: a pass for every avatar).
                std::unordered_map<LLUUID, LLScrollListItem*> shown;
                std::vector<LLUUID> gone;
                for (LLScrollListItem* item : mList->getAllData())
                {
                    if (listed.count(item->getUUID()))
                    {
                        shown.emplace(item->getUUID(), item);
                    }
                    else
                    {
                        gone.push_back(item->getUUID());
                    }
                }
                for (const LLUUID& id : gone)
                {
                    const S32 index = mList->getItemIndex(id);
                    if (index >= 0)
                    {
                        mList->deleteSingleItem(index);
                    }
                    mSetColoredRows.erase(id);
                }

                bool changed = !gone.empty();
                for (const Entry* entry : engine->rows())
                {
                    if (!listed.count(entry->id))
                    {
                        continue;
                    }
                    const auto found       = shown.find(entry->id);
                    LLScrollListItem* item = found != shown.end() ? found->second : nullptr;
                    if (!item)
                    {
                        LLScrollListItem::Params params;
                        params.value = entry->id;
                        LLScrollListCell::Params cell;
                        cell.font = LLFontGL::getFontSansSerif();
                        for (S32 i = 0; i < mList->getNumColumns(); ++i)
                        {
                            cell.column = mList->getColumn(i)->mName;
                            cell.value  = cellText(*entry, mList->getColumn(i)->mName);
                            params.columns.add(cell);
                        }
                        item    = mList->addRow(params);
                        changed = true;
                    }
                    else
                    {
                        for (S32 i = 0; i < item->getNumColumns(); ++i)
                        {
                            const std::string text = cellText(*entry, mList->getColumn(i)->mName);
                            LLScrollListCell* cell = item->getColumn(i);
                            if (cell && cell->getValue().asString() != text)
                            {
                                cell->setValue(text);
                                changed = true;
                            }
                        }
                    }
                    if (item)
                    {
                        styleRow(item, *entry);
                    }
                }
                if (changed)
                {
                    mList->setNeedsSort();
                }

                LLStringUtil::format_map_t args;
                ui::counterArgs(engine->stats(), args);
                mCounter->setText(getString("counter", args));
            }

            S32 compareCells(S32 column, const LLScrollListItem* a, const LLScrollListItem* b) const
            {
                const LLScrollListCell* cell_a = a->getColumn(column);
                const LLScrollListCell* cell_b = b->getColumn(column);
                const LLScrollListColumn* def  = mList->getColumn(column);
                if (!cell_a || !cell_b || !def)
                {
                    return 0;
                }
                const std::string text_a = cell_a->getValue().asString();
                const std::string text_b = cell_b->getValue().asString();
                if (def->mName == "range" || def->mName == "age")
                {
                    const F64 number_a = ui::sortNumber(text_a);
                    const F64 number_b = ui::sortNumber(text_b);
                    return number_a < number_b ? -1 : (number_a > number_b ? 1 : 0);
                }
                if (def->mName == "seen")
                {
                    return ui::sortSeconds(text_a) - ui::sortSeconds(text_b);
                }
                return LLStringUtil::compareDict(text_a, text_b);
            }

            // --- the actions ---

            uuid_vec_t selectedIds() const
            {
                uuid_vec_t ids;
                for (const LLScrollListItem* item : mList->getAllSelected())
                {
                    ids.push_back(item->getUUID());
                }
                return ids;
            }

            void notify(const char* string_id)
            {
                LLSD args;
                args["MESSAGE"] = getString(string_id);
                LLNotificationsUtil::add("SystemMessageTip", args);
            }

            void onDoubleClick()
            {
                const uuid_vec_t ids = selectedIds();
                if (ids.size() != 1)
                {
                    return;
                }
                tell(ui::zoomToId(ids.front()));
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

            void showMenu(S32 x, S32 y)
            {
                if (LLContextMenu* old_menu = mMenuHandle.get())
                {
                    old_menu->die();
                    mMenuHandle.markDead();
                }
                // RLVa @shownames: no menu for an avatar whose name is hidden (FSRadarListCtrl::handleRightMouseDown()):
                // its profile, IM, map, copied id and the rest would tell who it is.
                for (const LLUUID& id : selectedIds())
                {
                    if (nameHidden(id))
                    {
                        return;
                    }
                }
                LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
                LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
                registrar.add("Plus.Radar.Action", [this](LLUICtrl*, const LLSD& param) { onMenuAction(param.asString()); });
                enable_registrar.add("Plus.Radar.Enable", [this](LLUICtrl*, const LLSD& param) { return ui::menuEnabled(param.asString(), selectedIds()); });

                LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
                    "menu_plus_radar.xml", LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
                if (!menu)
                {
                    return;
                }
                mMenuHandle = menu->getHandle();
                menu->show(x, y);
                LLMenuGL::showPopup(mList, menu, x, y);
            }

            void onMenuAction(const std::string& action)
            {
                if (action == "select_all")
                {
                    mList->selectAll();
                    return;
                }
                tell(ui::runAction(action, selectedIds()));
            }

            void copyToClipboard(bool ids) { ui::copyToClipboard(selectedIds(), ids); }

            ui::Marks       mMarks;     // the marks of the cells (the window's strings, read once)
            AvatarList*     mList        = nullptr;
            LLTextBox*      mCounter     = nullptr;
            LLSearchEditor* mQuickFilter = nullptr;
            std::string     mFilter;    // lower case
            LLHandle<LLContextMenu> mMenuHandle;
            uuid_set_t      mSetColoredRows;    // rows whose name cell has a contact set colour (styleRow())
        };

        void init()
        {
            // The window is the RmlUi one (radarrml.cpp, research/386). This XUI version is frozen (not changed any
            // more: a setting added to the window goes to both) and is used in safe mode, where RmlUi is not started,
            // and with PlusRmlUiWindows off (the choice is made once, here: it takes effect at the next start).
            if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
            {
                LLFloaterReg::add(FLOATER_NAME, "floater_plus_radar.xml",
                                  (LLFloaterBuildFunc)&LLFloaterReg::build<RadarFloater>);
            }
            else
            {
                addRmlFloater(FLOATER_NAME);
            }
        }

        // At login: the reports work with the window closed.
        void onUIReady()
        {
            Engine::getInstance()->applyRunState();
        }

        plus::FeatureRegistration sRegistration({ "radar", &init, &onUIReady });
    }
}
