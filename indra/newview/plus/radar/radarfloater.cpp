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
// Left out of Firestorm's: the minimap above the list, the marks on the minimap, the voice
// level, notes and age alert columns, contact set colors, derendering and the actions for
// land and estate managers (the official viewer has none of these actions to call).

#include "llviewerprecompiledheaders.h"

#include "radar.h"
#include "plus.h"
#include "plus/camerashot/camerashot.h"
#include "plus/rlv/rlvhooks.h"

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

        // How near the camera comes to an avatar that is zoomed in on, in meters.
        constexpr F32 ZOOM_DISTANCE = 2.8f;
        // The avatar's position is its pelvis: a little above it is where one looks.
        constexpr F64 ZOOM_LOOK_HEIGHT = 0.3;

        struct ColumnDef
        {
            const char* name;   // also the strings col_<name> (label) and tip_<name> (tool tip)
            S32 width;
            U32 bit;
        };

        const ColumnDef COLUMNS[] = {
            { "name",    160, 0  },     // always shown
            { "region",  24,  1  },
            { "sitting", 24,  2  },
            { "age",     44,  4  },
            { "payment", 30,  8  },
            { "seen",    58,  16 },
            { "range",   56,  32 },
        };

        // The first number in a cell's text ("12.5", ">162"); the sort puts ">" (not known) last.
        F64 numberIn(const std::string& text)
        {
            if (!text.empty() && text[0] == '>')
            {
                return 1e12;
            }
            const size_t start = text.find_first_of("0123456789");
            return start == std::string::npos ? -1.0 : std::strtod(text.c_str() + start, nullptr);
        }

        // "h:mm:ss" in seconds.
        S32 secondsIn(const std::string& text)
        {
            S32 h = 0, m = 0, s = 0;
            if (sscanf(text.c_str(), "%d:%d:%d", &h, &m, &s) == 3)
            {
                return h * 3600 + m * 60 + s;
            }
            return 0;
        }

        // RLVa @shownames: the avatar's name may not be shown (plus/rlv, research/248 3.4).
        bool nameHidden(const LLUUID& id)
        {
            return plus::rlv::enabled() && !plus::rlv::canShowMenuName(id);
        }

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

        // The camera in front of an avatar, looking at it (Firestorm's LLAvatarActions::zoomIn()).
        void zoomTo(LLVOAvatar* avatar)
        {
            // A camera shot running would move the camera on from here the next frame.
            plus::camerashot::stop();
            // The flycam would override the move.
            if (LLViewerJoystick::getInstance()->getOverrideCamera())
            {
                gAgentCamera.toggleFlycam(); // off (the flycam of the 3D mouse and of game control, official 26.5)
            }
            LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);

            gAgentCamera.setFocusOnAvatar(false, ANIMATE);

            LLVector3d target = avatar->getPositionGlobal();
            target.mdV[VZ] += ZOOM_LOOK_HEIGHT;
            LLVector3d camera_dir = gAgentCamera.getCameraPositionGlobal() - target;
            if (camera_dir.normalize() == 0.0)
            {
                camera_dir = LLVector3d::x_axis;
            }
            gAgentCamera.setCameraPosAndFocusGlobal(target + camera_dir * ZOOM_DISTANCE, target, avatar->getID());
        }

        class RadarFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
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
                if (column == "name")
                {
                    return entry.name;
                }
                if (column == "region")
                {
                    return entry.onParcel ? getString("mark_parcel") : (entry.inRegion ? getString("mark_region") : std::string());
                }
                if (column == "sitting")
                {
                    return entry.sitting ? getString("mark_sitting") : std::string();
                }
                if (column == "age")
                {
                    if (nameHidden(entry.id))
                    {
                        return std::string();   // RLVa @shownames: no age either (FSRadar::updateRadarList())
                    }
                    if (entry.age >= 0)
                    {
                        return llformat("%d", entry.age);
                    }
                    return entry.age == -2 ? getString("age_hidden") : (entry.age == -3 ? getString("age_unknown") : std::string());
                }
                if (column == "payment")
                {
                    return (entry.flags & AVATAR_TRANSACTED) ? "$$" : ((entry.flags & AVATAR_IDENTIFIED) ? "$" : "");
                }
                if (column == "seen")
                {
                    const S32 seconds = (S32)difftime(time(nullptr), entry.firstSeen);
                    return llformat("%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
                }
                if (column == "range")
                {
                    // Not known: farther than the draw distance.
                    return entry.range > UNKNOWN_RANGE ? llformat("%.1f", entry.range)
                                                       : llformat(">%d", (S32)gSavedSettings.getF32("RenderFarClip"));
                }
                return std::string();
            }

            // Colors and styles of a row: the range by how far one is heard, friends bold, the muted in italic.
            void styleRow(LLScrollListItem* item, const Entry& entry)
            {
                static const LLColor4 say_color(1.f, 0.4f, 0.4f, 1.f);
                static const LLColor4 shout_color(1.f, 0.85f, 0.45f, 1.f);
                const LLColor4 other_color = LLUIColorTable::instance().getColor("DefaultListText", LLColor4::white).get();
                for (S32 i = 0; i < item->getNumColumns(); ++i)
                {
                    LLScrollListText* cell = static_cast<LLScrollListText*>(item->getColumn(i));
                    const std::string& name = mList->getColumn(i)->mName;
                    if (name == "name")
                    {
                        // RLVa @shownames: a friend is not told by the bold either (FSRadar::updateRadarList()).
                        const bool bold = entry.friendship && !nameHidden(entry.id);
                        cell->setFontStyle(static_cast<U8>((bold ? LLFontGL::BOLD : 0) | (entry.muted ? LLFontGL::ITALIC : 0)));
                    }
                    else if (name == "range")
                    {
                        const bool known = entry.range > UNKNOWN_RANGE;
                        cell->setColor(known && entry.range <= CHAT_RANGE_SAY ? say_color
                                       : (known && entry.range <= CHAT_RANGE_SHOUT ? shout_color : other_color));
                        // In bold when the avatar is drawn.
                        cell->setFontStyle(static_cast<U8>(entry.inDraw ? LLFontGL::BOLD : LLFontGL::NORMAL));
                    }
                }
            }

            bool matchesFilter(const Entry& entry) const
            {
                if (mFilter.empty())
                {
                    return true;
                }
                std::string name = entry.name;
                LLStringUtil::toLower(name);
                return name.find(mFilter) != std::string::npos;
            }

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
                args["[TOTAL]"]  = llformat("%d", engine->stats().total);
                args["[REGION]"] = llformat("%d", engine->stats().region);
                args["[CHAT]"]   = llformat("%d", engine->stats().chatRange);
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
                    const F64 number_a = numberIn(text_a);
                    const F64 number_b = numberIn(text_b);
                    return number_a < number_b ? -1 : (number_a > number_b ? 1 : 0);
                }
                if (def->mName == "seen")
                {
                    return secondsIn(text_a) - secondsIn(text_b);
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

            // The avatar's object when it is drawn (the camera needs it).
            LLVOAvatar* drawnAvatar(const LLUUID& id) const
            {
                LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(id));
                return avatar && !avatar->isDead() ? avatar : nullptr;
            }

            void onDoubleClick()
            {
                const uuid_vec_t ids = selectedIds();
                if (ids.size() != 1)
                {
                    return;
                }
                if (LLVOAvatar* avatar = drawnAvatar(ids.front()))
                {
                    zoomTo(avatar);
                }
                else
                {
                    notify("no_focus");
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
                enable_registrar.add("Plus.Radar.Enable", [this](LLUICtrl*, const LLSD& param) { return menuEnabled(param.asString()); });

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

            bool menuEnabled(const std::string& what) const
            {
                const uuid_vec_t ids = selectedIds();
                if (ids.empty())
                {
                    return false;
                }
                if (what == "any")
                {
                    return true;
                }
                if (what == "offer_teleport")
                {
                    return LLAvatarActions::canOfferTeleport(ids);
                }
                if (what == "call")
                {
                    return LLAvatarActions::canCall();     // one: a call; several: a conference call
                }
                if (ids.size() != 1)
                {
                    return false;
                }
                const LLUUID& id   = ids.front();
                const Entry* entry = Engine::getInstance()->find(id);
                if (what == "one")
                {
                    return true;
                }
                if (what == "add_friend")
                {
                    return !LLAvatarActions::isFriend(id);
                }
                if (what == "remove_friend")
                {
                    return LLAvatarActions::isFriend(id);
                }
                if (what == "map")
                {
                    return is_agent_mappable(id) || gAgent.isGodlike();
                }
                if (what == "position")
                {
                    return entry && entry->range > UNKNOWN_RANGE;
                }
                if (what == "zoom")
                {
                    return entry && entry->inDraw;
                }
                if (what == "block")
                {
                    return LLAvatarActions::canBlock(id);
                }
                return false;
            }

            void onMenuAction(const std::string& action)
            {
                const uuid_vec_t ids = selectedIds();
                if (ids.empty())
                {
                    return;
                }
                const LLUUID& id = ids.front();
                const bool one   = ids.size() == 1;
                if (action == "profile")
                {
                    LLAvatarActions::showProfile(id);
                }
                else if (action == "add_friend")
                {
                    LLAvatarActions::requestFriendshipDialog(id);
                }
                else if (action == "remove_friend")
                {
                    LLAvatarActions::removeFriendDialog(id);
                }
                else if (action == "im")
                {
                    one ? LLAvatarActions::startIM(id) : LLAvatarActions::startConference(ids);
                }
                else if (action == "call")
                {
                    one ? LLAvatarActions::startCall(id) : LLAvatarActions::startAdhocCall(ids);
                }
                else if (action == "map")
                {
                    LLAvatarActions::showOnMap(id);
                }
                else if (action == "track")
                {
                    Engine::getInstance()->startTracking(id);
                }
                else if (action == "teleport_to")
                {
                    const Entry* entry = Engine::getInstance()->find(id);
                    if (entry && entry->range > UNKNOWN_RANGE)
                    {
                        gAgent.teleportViaLocation(entry->position);
                    }
                    else
                    {
                        notify("no_position");
                    }
                }
                else if (action == "zoom")
                {
                    if (LLVOAvatar* avatar = drawnAvatar(id))
                    {
                        zoomTo(avatar);
                    }
                    else
                    {
                        notify("no_focus");
                    }
                }
                else if (action == "offer_teleport")
                {
                    LLAvatarActions::offerTeleport(ids);
                }
                else if (action == "request_teleport")
                {
                    LLAvatarActions::teleportRequest(id);
                }
                else if (action == "share")
                {
                    LLAvatarActions::share(id);
                }
                else if (action == "pay")
                {
                    LLAvatarActions::pay(id);
                }
                else if (action == "invite_group")
                {
                    LLAvatarActions::inviteToGroup(id);
                }
                else if (action == "block")
                {
                    LLAvatarActions::toggleBlock(id);
                }
                else if (action == "copy_name")
                {
                    copyToClipboard(false);
                }
                else if (action == "copy_id")
                {
                    copyToClipboard(true);
                }
                else if (action == "select_all")
                {
                    mList->selectAll();
                }
            }

            // The names (or the ids) of the selected rows, one to a line.
            void copyToClipboard(bool ids)
            {
                std::string text;
                for (const LLUUID& id : selectedIds())
                {
                    if (!text.empty())
                    {
                        text += "\n";
                    }
                    const Entry* entry = Engine::getInstance()->find(id);
                    text += ids ? id.asString() : (entry ? entry->name : id.asString());
                }
                if (text.empty())
                {
                    return;
                }
                const LLWString wide = utf8str_to_wstring(text);
                LLClipboard::instance().copyToClipboard(wide, 0, static_cast<S32>(wide.length()));
            }

            AvatarList*     mList        = nullptr;
            LLTextBox*      mCounter     = nullptr;
            LLSearchEditor* mQuickFilter = nullptr;
            std::string     mFilter;    // lower case
            LLHandle<LLContextMenu> mMenuHandle;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_radar.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<RadarFloater>);
        }

        // At login: the reports work with the window closed.
        void onUIReady()
        {
            Engine::getInstance()->applyRunState();
        }

        plus::FeatureRegistration sRegistration({ "radar", &init, &onUIReady });
    }
}
