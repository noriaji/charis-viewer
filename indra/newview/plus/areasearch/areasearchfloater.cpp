/**
 * @file areasearchfloater.cpp
 * @brief Area search: the window (list, find, filter and column tabs, the list's menu).
 *
 * Ported from Firestorm's fsareasearch.cpp (the window) and fsareasearchmenu.cpp (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2012 Techwolf Lupindo
 * Copyright (c) 2014 Ansariel Hiller
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

// Menu: "Area Search" in menu_plus.xml (Floater.Toggle plus_area_search). The window is
// destroyed when closed (reuse_instance false): the search, its interest list and its
// cache of properties go with it.
//
// Settings (gSavedSettings, shared by every account):
//   PlusAreaSearchColumns      the columns shown (bits, ColumnDef::bit)
//   PlusAreaSearchFilters      the filter tab as saved by "Save as default" (control name -> value)
//   PlusAreaSearchDoubleClick  what a double click does besides showing the object on the map
//                              (bits: 1 touch, 2 buy, 4 sit, 8 zoom the camera in)
//   PlusAreaSearchIncludeAttached  list what avatars wear too, the agent's and everyone's (off:
//                              left out; the filter tab's check box, Criteria::exceptAttached)
//   PlusAreaSearchRange        the Options tab's spinner (control_name): only what is this near is
//                              listed, and the simulator is asked to send it (0: no limit to the
//                              list, the draw distance for the simulator); read by the engine
//                              (Engine::withinRange(), farRange())
//
// Changed from Firestorm's, to make it easier to use: the list is made as the window opens
// (with the saved filters; Firestorm shows an empty Find tab until Search is pressed), a quick
// filter over the list narrows it as one types (any of the texts; the Find tab still has a
// field for each), and a double click can zoom the camera in on the object.
// Left out of Firestorm's: the script info of the LSL bridge and the asset blacklist (neither
// exists here). RLVa's @shownames: areasearch.cpp (nameOf()) and refreshNames() below.

#include "llviewerprecompiledheaders.h"

#include "areasearch.h"
#include "plus.h"
#include "plus/camerashot/camerashot.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "llagent.h"
#include "llagentcamera.h"
#include "llagentdata.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "lleventtimer.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llmenugl.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsearcheditor.h"
#include "llselectmgr.h"
#include "llspinctrl.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltoolgrab.h"
#include "lltracker.h"
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewermediafocus.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"

namespace plus::areasearch
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_area_search";

        constexpr char SETTING_COLUMNS[]      = "PlusAreaSearchColumns";
        constexpr char SETTING_FILTERS[]      = "PlusAreaSearchFilters";
        constexpr char SETTING_DOUBLE_CLICK[] = "PlusAreaSearchDoubleClick";
        constexpr char SETTING_INCLUDE_ATTACHED[] = "PlusAreaSearchIncludeAttached";

        constexpr U32 DOUBLE_CLICK_TOUCH = 1, DOUBLE_CLICK_BUY = 2, DOUBLE_CLICK_SIT = 4, DOUBLE_CLICK_ZOOM = 8;

        // The quick filter waits this long after the last key, so a word is one search, not one a letter.
        constexpr F32 QUICK_FILTER_DELAY = 0.3f;

        // Seconds between the touches of several objects, so they arrive in order.
        constexpr F32 TOUCH_INTERVAL = 0.2f;

        struct ColumnDef
        {
            const char* name;   // also the strings col_<name> (label) and tip_<name> (tool tip)
            S32 width;
            U32 bit;
            bool numeric;       // sorted by the number in the text
        };

        const ColumnDef COLUMNS[] = {
            { "distance",    48,  1,   true  },
            { "name",        170, 2,   false },
            { "description", 160, 4,   false },
            { "price",       64,  8,   true  },
            { "land_impact", 40,  16,  true  },
            { "prim_count",  44,  32,  true  },
            { "owner",       130, 64,  false },
            { "group",       120, 128, false },
            { "creator",     130, 256, false },
            { "last_owner",  130, 512, false },
        };

        U32 allColumnBits()
        {
            U32 bits = 0;
            for (const ColumnDef& column : COLUMNS)
            {
                bits |= column.bit;
            }
            return bits;
        }

        // The filter tab's check boxes; with the spinners and the combo below, what "Save as
        // default" keeps (by control name).
        const char* const FILTER_CHECKS[] = {
            "filter_locked", "filter_physical", "filter_phantom", "filter_temporary", "filter_attachment",
            "filter_moap", "filter_perm_copy", "filter_perm_modify", "filter_perm_transfer",
            "filter_reflection_probe", "filter_agent_parcel_only", "filter_for_sale", "filter_distance",
            "exclude_attachment", "exclude_physical", "exclude_temporary", "exclude_reflection_probes",
            "exclude_childprim", "exclude_neighbor_region",
        };
        const char* const FILTER_SPINNERS[] = { "min_price", "max_price", "min_distance", "max_distance" };
        constexpr char FILTER_CLICK_ACTION[] = "click_action";

        // Excluding a kind of object leaves nothing to list "only" of it.
        struct ExcludePair
        {
            const char* except;
            const char* only;
        };
        const ExcludePair EXCLUDE_PAIRS[] = {
            { "exclude_attachment", "filter_attachment" },
            { "exclude_physical", "filter_physical" },
            { "exclude_temporary", "filter_temporary" },
            { "exclude_reflection_probes", "filter_reflection_probe" },
        };

        // The first number in a cell's text ("12 m", "L$ 150"); -1 without one ("...", blank).
        F64 numberIn(const std::string& text)
        {
            const size_t start = text.find_first_of("0123456789");
            return start == std::string::npos ? -1.0 : std::strtod(text.c_str() + start, nullptr);
        }

        // The list, with a menu of the floater's on a right click.
        class ObjectList : public LLScrollListCtrl
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

            // Deletes these rows in one pass over the list (deleteSingleItem() would look each
            // one up, a pass apiece). The list's own order is kept.
            void deleteRows(const std::vector<LLScrollListItem*>& rows)
            {
                if (rows.empty())
                {
                    return;
                }
                std::unordered_set<LLScrollListItem*> doomed(rows.begin(), rows.end());
                // The list remembers the last selected row: that one goes its own way.
                if (LLScrollListItem* last = getLastSelectedItem(); last && doomed.erase(last))
                {
                    deleteSingleItem(getItemIndex(last));
                }
                item_list& items = getItemList();
                items.erase(std::remove_if(items.begin(), items.end(),
                                           [&doomed](LLScrollListItem* item) {
                                               if (!doomed.count(item))
                                               {
                                                   return false;
                                               }
                                               delete item;
                                               return true;
                                           }),
                            items.end());
                dirtyColumns();
            }

        protected:
            friend class ::LLUICtrlFactory;
            explicit ObjectList(const Params& p) : LLScrollListCtrl(p) {}

        private:
            std::function<void(S32, S32)> mRightClick;
        };

        LLDefaultChildRegistry::Register<ObjectList> sObjectListRegistration("plus_area_search_list");

        void touchObject(LLViewerObject* object)
        {
            LLPickInfo pick; // the defaults: a touch of the object as a whole
            send_ObjectGrab_message(object, pick, LLVector3::zero);
            send_ObjectDeGrab_message(object, pick);
        }

        void sitOn(LLViewerObject* object)
        {
            LLViewerRegion* region = object->getRegion();
            if (!region)
            {
                return; // its region is gone (just left)
            }
            gMessageSystem->newMessageFast(_PREHASH_AgentRequestSit);
            gMessageSystem->nextBlockFast(_PREHASH_AgentData);
            gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
            gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
            gMessageSystem->nextBlockFast(_PREHASH_TargetObject);
            gMessageSystem->addUUIDFast(_PREHASH_TargetID, object->getID());
            gMessageSystem->addVector3Fast(_PREHASH_Offset, LLVector3::zero);
            region->sendReliableMessage();
        }

        // The selection node of an object the search has the properties of, made valid with
        // them (what buying and returning need; the simulator sent them to the search, not
        // to the selection).
        LLSelectNode* selectWithProperties(LLViewerObject* object, const Entry& entry)
        {
            LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(object);
            if (!node)
            {
                return nullptr;
            }
            node->mValid = true;
            node->mPermissions->init(entry.creatorId, entry.ownerId, entry.lastOwnerId, entry.groupId);
            node->mPermissions->initMasks(entry.permissions.getMaskBase(), entry.permissions.getMaskOwner(),
                                          entry.permissions.getMaskEveryone(), entry.permissions.getMaskGroup(),
                                          entry.permissions.getMaskNextOwner());
            node->mAggregatePerm = entry.aggregatePerms;
            return node;
        }

        void buyObject(LLViewerObject* object, const Entry& entry)
        {
            LLSelectMgr::getInstance()->deselectAll();
            LLSelectMgr::getInstance()->selectObjectAndFamily(object);
            if (LLSelectNode* node = selectWithProperties(object, entry))
            {
                node->mSaleInfo    = entry.saleInfo;
                node->mCategory    = entry.category;
                node->mName        = entry.name;
                node->mDescription = entry.description;
                handle_buy();
            }
        }

        // The camera looks at the object, far enough for it to fill about three quarters of
        // the view and not so close that it crosses the near clip (from Firestorm, which took
        // it from LLViewerMediaFocus::setCameraZoom()).
        void zoomTo(LLViewerObject* object)
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
            gAgentCamera.setLookAt(LOOKAT_TARGET_SELECT, object);

            const LLBBox bbox = object->getBoundingBoxAgent();
            const LLVector3d target(gAgent.getPosGlobalFromAgent(bbox.getCenterAgent()));
            LLVector3d camera_dir(gAgentCamera.getCameraPositionGlobal() - target);
            camera_dir.normalize();

            F32 height = 0.f, width = 0.f, depth = 0.f;
            const F32 aspect_ratio  = LLViewerMediaFocus::getBBoxAspectRatio(bbox, LLVector3(camera_dir), &height, &width, &depth);
            LLViewerCamera* camera  = LLViewerCamera::getInstance();
            const F32 camera_aspect = camera->getAspect();
            // The object's side along the screen's shorter side, unless the object is more
            // elongated than the screen.
            const bool invert = (camera_aspect > 1.f && aspect_ratio > camera_aspect) || (camera_aspect < 1.f && aspect_ratio < camera_aspect);
            F32 distance = 0.f;
            if (camera_aspect < 1.f || invert)
            {
                const F32 angle = llmax(0.1f, camera->getView() * camera_aspect);
                distance = width * 0.5f * 1.1f / tanf(angle * 0.5f);
            }
            else
            {
                const F32 angle = llmax(0.1f, camera->getView());
                distance = height * 0.5f * 1.1f / tanf(angle * 0.5f);
            }
            distance += depth * 0.5f;

            // Not inside the near clip: the bounding box's extent along the view direction.
            const LLVector3d extents(bbox.getExtentLocal());
            const LLVector3d axis_x = LLVector3d(1, 0, 0) * bbox.getRotation();
            const LLVector3d axis_y = LLVector3d(0, 1, 0) * bbox.getRotation();
            const LLVector3d axis_z = LLVector3d(0, 0, 1) * bbox.getRotation();
            const F32 near_extent = (F32)(extents.mdV[0] * (camera_dir * axis_x) + extents.mdV[1] * (camera_dir * axis_y)
                                          + extents.mdV[2] * (camera_dir * axis_z));
            distance = llmax(distance, near_extent + camera->getNear());

            LLVector3d camera_pos(target + camera_dir * distance);
            if (camera_dir == LLVector3d::z_axis || camera_dir == LLVector3d::z_axis_neg)
            {
                // Straight up or down the camera would flip over: a little toward where it was.
                LLVector3d delta = gAgentCamera.getCameraPositionGlobal() - camera_pos;
                const F64 length = delta.length();
                delta.normalize();
                camera_pos += 0.01 * length * delta;
            }
            gAgentCamera.setCameraPosAndFocusGlobal(camera_pos, target, object->getID());
        }

        class AreaSearchFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                mTabs    = getChild<LLTabContainer>("area_search_tabs");
                mList    = getChild<ObjectList>("result_list");
                mCounter = getChild<LLTextBox>("counter");
                mStatus  = getChild<LLTextBox>("find_status");

                mList->setDoubleClickCallback([this]() { onDoubleClick(); });
                mList->setRightClickCallback([this](S32 x, S32 y) { showMenu(x, y); });
                mList->setSortCallback([this](S32 column, const LLScrollListItem* a, const LLScrollListItem* b) {
                    return compareCells(column, a, b);
                });
                buildColumns();
                mList->sortByColumn("name", true);

                getChild<LLButton>("btn_refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { search(true); });
                getChild<LLButton>("btn_search")->setCommitCallback([this](LLUICtrl*, const LLSD&) { search(false); });
                getChild<LLButton>("btn_clear")->setCommitCallback([this](LLUICtrl*, const LLSD&) { clearFindFields(); });
                getChild<LLButton>("btn_apply")->setCommitCallback([this](LLUICtrl*, const LLSD&) { search(false); });
                getChild<LLButton>("btn_save_default")->setCommitCallback([this](LLUICtrl*, const LLSD&) { saveFilters(); });
                getChild<LLCheckBoxCtrl>("beacons")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    mBeacons = ctrl->getValue().asBoolean();
                });

                // Enter in a field searches.
                for (const char* field : FIND_FIELDS)
                {
                    getChild<LLLineEditor>(field)->setCommitCallback([this](LLUICtrl*, const LLSD&) { search(false); });
                }
                getChild<LLLineEditor>("owner_search")->setKeystrokeCallback(
                    [this](LLLineEditor* editor, void*) {
                        if (mOnlyMine && !editor->getText().empty())
                        {
                            clearOnlyMine();
                        }
                    },
                    nullptr);

                loadFilters();
                for (const ExcludePair& pair : EXCLUDE_PAIRS)
                {
                    getChild<LLCheckBoxCtrl>(pair.except)->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateFilterControls(); });
                }
                getChild<LLCheckBoxCtrl>("filter_for_sale")->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateFilterControls(); });
                getChild<LLCheckBoxCtrl>("filter_distance")->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateFilterControls(); });
                updateFilterControls();
                // Bound to PlusAreaSearchIncludeAttached (control_name). Takes effect at once, with
                // the conditions of the last search (not the other choices of the tab before Apply).
                getChild<LLCheckBoxCtrl>("include_attached")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    updateFilterControls();
                    mEngine.setExceptAttached(!ctrl->getValue().asBoolean());
                });

                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                for (const ColumnDef& column : COLUMNS)
                {
                    LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(std::string("col_") + column.name);
                    check->set((shown & column.bit) != 0);
                    check->setCommitCallback([this, bit = column.bit](LLUICtrl* ctrl, const LLSD&) {
                        showColumn(ctrl, bit, ctrl->getValue().asBoolean());
                    });
                }

                const U32 double_click = gSavedSettings.getU32(SETTING_DOUBLE_CLICK);
                struct DoubleClickCheck { const char* name; U32 bit; };
                for (const DoubleClickCheck& spec : { DoubleClickCheck{ "double_click_zoom", DOUBLE_CLICK_ZOOM },
                                                      DoubleClickCheck{ "double_click_touch", DOUBLE_CLICK_TOUCH },
                                                      DoubleClickCheck{ "double_click_buy", DOUBLE_CLICK_BUY },
                                                      DoubleClickCheck{ "double_click_sit", DOUBLE_CLICK_SIT } })
                {
                    LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(spec.name);
                    check->set((double_click & spec.bit) != 0);
                    check->setCommitCallback([bit = spec.bit](LLUICtrl* ctrl, const LLSD&) {
                        const U32 value = gSavedSettings.getU32(SETTING_DOUBLE_CLICK);
                        gSavedSettings.setU32(SETTING_DOUBLE_CLICK, ctrl->getValue().asBoolean() ? (value | bit) : (value & ~bit));
                    });
                }

                mQuickFilter = getChild<LLSearchEditor>("quick_filter");
                mQuickFilter->setKeystrokeCallback([this](LLUICtrl*, const LLSD&) {
                    mQuickFilterPending = true;
                    mQuickFilterTimer.start();
                });

                mCounter->setText(getString("counter_blank"));
                mEngine.start();
                // The list at once, with the saved filters.
                search(false);
                return true;
            }

            void draw() override
            {
                if (mQuickFilterPending && mQuickFilterTimer.getElapsedTimeF32() > QUICK_FILTER_DELAY)
                {
                    mQuickFilterPending = false;
                    search(false, false);
                }
                LLFloater::draw();
                if (!mBeacons)
                {
                    return;
                }
                static LLCachedControl<S32> beacon_width(gSavedSettings, "DebugBeaconLineWidth");
                const LLColor4 beacon_color(1.f, 0.5f, 0.f, 0.5f);
                const LLColor4 text_color(1.f, 1.f, 1.f, 1.f);
                for (LLScrollListItem* item : mList->getAllData())
                {
                    LLViewerObject* object = gObjectList.findObject(item->getUUID());
                    const Entry* entry     = mEngine.find(item->getUUID());
                    if (object && entry)
                    {
                        gObjectList.addDebugBeacon(object->getPositionAgent(), entry->name, beacon_color, text_color, beacon_width);
                    }
                    else if (entry && entry->cacheOnly)
                    {
                        // No object in the viewer: where the cache record said it is.
                        gObjectList.addDebugBeacon(gAgent.getPosAgentFromGlobal(entry->position), entry->name, beacon_color, text_color, beacon_width);
                    }
                }
            }

            // The rows again with the names as they may be shown now (refreshNames(): RLVa's @shownames switched),
            // from what the search knows and with the conditions of the last search (as showColumn()).
            void relistNames()
            {
                if (mSearched && mEngine.running())
                {
                    mEngine.relist();
                }
            }

            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (mask == MASK_CONTROL && key == 'C')
                {
                    copySelection();
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
            static constexpr const char* FIND_FIELDS[] = { "name_search", "description_search", "owner_search",
                                                           "group_search", "creator_search", "last_owner_search" };

            explicit AreaSearchFloater(const LLSD& key)
                : LLFloater(key),
                  mEngine(Callbacks{ [this](const Row& row) { addRow(row); },
                                     [this](const Row& row) { changeRow(row); },
                                     [this](const std::vector<LLUUID>& ids) { removeRows(ids); },
                                     [this]() { clearList(); },
                                     [this](S32 listed, S32 pending, S32 total) { showCounts(listed, pending, total); } })
            {
            }

            ~AreaSearchFloater() override
            {
                if (LLContextMenu* menu = mMenuHandle.get())
                {
                    menu->die();
                }
                mEngine.stop();
            }

            // --- searching ---

            Criteria criteriaFromControls()
            {
                Criteria c;
                c.name        = getChild<LLLineEditor>("name_search")->getText();
                c.description = getChild<LLLineEditor>("description_search")->getText();
                c.owner       = getChild<LLLineEditor>("owner_search")->getText();
                c.group       = getChild<LLLineEditor>("group_search")->getText();
                c.creator     = getChild<LLLineEditor>("creator_search")->getText();
                c.lastOwner   = getChild<LLLineEditor>("last_owner_search")->getText();
                c.regex       = checked("regular_expression");
                if (mOnlyMine)
                {
                    c.ownerId = gAgentID;
                }
                c.anyText     = mQuickFilter->getText();

                c.onlyLocked     = checked("filter_locked");
                c.onlyPhysical   = checked("filter_physical");
                c.onlyTemporary  = checked("filter_temporary");
                c.onlyPhantom    = checked("filter_phantom");
                c.onlyAttachment = checked("filter_attachment");
                c.onlyMedia      = checked("filter_moap");
                c.onlyProbe      = checked("filter_reflection_probe");
                c.onlyCopy       = checked("filter_perm_copy");
                c.onlyModify     = checked("filter_perm_modify");
                c.onlyTransfer   = checked("filter_perm_transfer");
                c.onlyParcel     = checked("filter_agent_parcel_only");
                c.forSale        = checked("filter_for_sale");
                c.priceMin       = getChild<LLSpinCtrl>("min_price")->getValue().asInteger();
                c.priceMax       = getChild<LLSpinCtrl>("max_price")->getValue().asInteger();
                c.distance       = checked("filter_distance");
                c.distanceMin    = getChild<LLSpinCtrl>("min_distance")->getValue().asInteger();
                c.distanceMax    = getChild<LLSpinCtrl>("max_distance")->getValue().asInteger();
                c.clickAction    = getChild<LLComboBox>(FILTER_CLICK_ACTION)->getCurrentIndex();

                c.exceptAttachment = checked("exclude_attachment");
                c.exceptPhysical   = checked("exclude_physical");
                c.exceptTemporary  = checked("exclude_temporary");
                c.exceptProbe      = checked("exclude_reflection_probes");
                c.exceptChild      = checked("exclude_childprim");
                c.exceptNeighbor   = checked("exclude_neighbor_region");
                static LLCachedControl<bool> include_attached(gSavedSettings, SETTING_INCLUDE_ATTACHED, false);
                c.exceptAttached   = !include_attached();
                return c;
            }

            // To the list tab when the search is made (a change of the columns only lists again:
            // showColumn()). The quick filter (moveTab false) leaves the tab as it is: it searches
            // as the user types in the list tab, and a broken Find pattern would pull them away at
            // every key (the error still goes to the Find tab's status).
            void search(bool cacheClear, bool moveTab = true)
            {
                const std::string error = mEngine.search(criteriaFromControls(), cacheClear);
                if (!error.empty())
                {
                    LLStringUtil::format_map_t args;
                    args["[ERROR]"] = error;
                    mStatus->setText(getString("regex_error", args));
                    if (moveTab)
                    {
                        mTabs->selectTabByName("find_panel");
                    }
                    return;
                }
                mStatus->setText(LLStringUtil::null);
                mSearched = true;
                if (moveTab)
                {
                    mTabs->selectTabByName("list_panel");
                }
            }

            void clearFindFields()
            {
                for (const char* field : FIND_FIELDS)
                {
                    getChild<LLLineEditor>(field)->clear();
                }
                clearOnlyMine();
                mStatus->setText(LLStringUtil::null);
            }

            // "Mine only" ends when the owner field is typed in or the fields are cleared.
            void clearOnlyMine()
            {
                mOnlyMine = false;
                getChild<LLLineEditor>("owner_search")->setLabel(LLStringUtil::null);
            }

            bool checked(const char* name) { return getChild<LLCheckBoxCtrl>(name)->get(); }

            void updateFilterControls()
            {
                for (const ExcludePair& pair : EXCLUDE_PAIRS)
                {
                    LLCheckBoxCtrl* only = getChild<LLCheckBoxCtrl>(pair.only);
                    const bool excluded  = checked(pair.except);
                    if (excluded)
                    {
                        only->set(false);
                    }
                    only->setEnabled(!excluded);
                }
                // With what avatars wear left out (PlusAreaSearchIncludeAttached off), neither
                // "Attachment" box has anything to act on.
                if (!checked("include_attached"))
                {
                    getChild<LLCheckBoxCtrl>("filter_attachment")->set(false);
                    getChild<LLCheckBoxCtrl>("filter_attachment")->setEnabled(false);
                    getChild<LLCheckBoxCtrl>("exclude_attachment")->setEnabled(false);
                }
                else
                {
                    getChild<LLCheckBoxCtrl>("exclude_attachment")->setEnabled(true);
                }
                const bool for_sale = checked("filter_for_sale");
                getChild<LLSpinCtrl>("min_price")->setEnabled(for_sale);
                getChild<LLSpinCtrl>("max_price")->setEnabled(for_sale);
                const bool distance = checked("filter_distance");
                getChild<LLSpinCtrl>("min_distance")->setEnabled(distance);
                getChild<LLSpinCtrl>("max_distance")->setEnabled(distance);
            }

            // The saved defaults over the window's own (a control missing from the setting
            // keeps the window's).
            void loadFilters()
            {
                const LLSD saved = gSavedSettings.getLLSD(SETTING_FILTERS);
                if (!saved.isMap())
                {
                    return;
                }
                for (const char* name : FILTER_CHECKS)
                {
                    if (saved.has(name))
                    {
                        getChild<LLCheckBoxCtrl>(name)->set(saved[name].asBoolean());
                    }
                }
                for (const char* name : FILTER_SPINNERS)
                {
                    if (saved.has(name))
                    {
                        getChild<LLSpinCtrl>(name)->set((F32)saved[name].asInteger());
                    }
                }
                if (saved.has(FILTER_CLICK_ACTION))
                {
                    getChild<LLComboBox>(FILTER_CLICK_ACTION)->selectNthItem(saved[FILTER_CLICK_ACTION].asInteger());
                }
            }

            void saveFilters()
            {
                LLSD saved = LLSD::emptyMap();
                for (const char* name : FILTER_CHECKS)
                {
                    saved[name] = checked(name);
                }
                for (const char* name : FILTER_SPINNERS)
                {
                    saved[name] = getChild<LLSpinCtrl>(name)->getValue().asInteger();
                }
                saved[FILTER_CLICK_ACTION] = getChild<LLComboBox>(FILTER_CLICK_ACTION)->getCurrentIndex();
                gSavedSettings.setLLSD(SETTING_FILTERS, saved);
            }

            // --- the list ---

            // The shown columns, in the order of COLUMNS. The rows are made again by the caller.
            void buildColumns()
            {
                const std::string sort_column = mList->getSortColumnName();
                const bool sort_ascending     = mList->getSortAscending();
                clearList();
                mList->clearColumns();

                U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                if (!(shown & allColumnBits()))
                {
                    // None saved (by hand in the setting): the name at least, as showColumn() keeps one.
                    shown |= COLUMNS[1].bit;
                    gSavedSettings.setU32(SETTING_COLUMNS, shown);
                }
                for (const ColumnDef& column : COLUMNS)
                {
                    if (!(shown & column.bit))
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

            void showColumn(LLUICtrl* check, U32 bit, bool show)
            {
                const U32 shown = gSavedSettings.getU32(SETTING_COLUMNS);
                const U32 next  = show ? (shown | bit) : (shown & ~bit);
                if (!(next & allColumnBits()))
                {
                    // The last column stays: with none, the list makes one of its own (the row's id).
                    check->setValue(true);
                    return;
                }
                gSavedSettings.setU32(SETTING_COLUMNS, next);
                buildColumns();
                if (mSearched)
                {
                    // The rows again, from what the search knows already, with the conditions of
                    // the last search (not the fields as they are now: a wrong regular expression
                    // typed since would list nothing, fix-round11 3).
                    mEngine.relist();
                }
            }

            std::string cellText(const Row& row, const std::string& column) const
            {
                if (column == "distance")    return llformat("%d m", row.distance);
                if (column == "name")        return row.name;
                if (column == "description") return row.description;
                if (column == "price")
                {
                    if (row.price < 0)
                    {
                        return std::string();
                    }
                    LLStringUtil::format_map_t args;
                    args["[COST]"] = llformat("%d", row.price);
                    return getString("cost_label", args);
                }
                if (column == "land_impact") return row.landImpact > 0.f ? llformat("%d", ll_round(row.landImpact)) : std::string("...");
                if (column == "prim_count")  return llformat("%d", row.prims);
                if (column == "owner")       return row.owner;
                if (column == "group")       return row.group;
                if (column == "creator")     return row.creator;
                if (column == "last_owner")  return row.lastOwner;
                return std::string();
            }

            void addRow(const Row& row)
            {
                if (!mList)
                {
                    return;
                }
                LLScrollListItem::Params item;
                item.value = row.id;
                LLScrollListCell::Params cell;
                cell.font = LLFontGL::getFontSansSerif();
                for (S32 i = 0; i < mList->getNumColumns(); ++i)
                {
                    const LLScrollListColumn* column = mList->getColumn(i);
                    cell.column = column->mName;
                    cell.value  = cellText(row, column->mName);
                    item.columns.add(cell);
                }
                LLScrollListItem* added = mList->addRow(item);
                if (!added)
                {
                    return;
                }
                mRows[row.id] = added;
                // Temporary objects in italics, physical ones in bold.
                if (row.temporary || row.physical)
                {
                    const U8 style = (row.temporary ? LLFontGL::ITALIC : 0) | (row.physical ? LLFontGL::BOLD : 0);
                    for (S32 i = 0; i < added->getNumColumns(); ++i)
                    {
                        static_cast<LLScrollListText*>(added->getColumn(i))->setFontStyle(style);
                    }
                }
            }

            // By the table of rows: the list's own lookup goes through every row, comparing the
            // ids as text, and this is called for every row each second while the agent walks.
            void changeRow(const Row& row)
            {
                const auto found = mRows.find(row.id);
                if (!mList || found == mRows.end())
                {
                    return;
                }
                LLScrollListItem* item = found->second;
                bool changed = false;
                for (S32 i = 0; i < mList->getNumColumns(); ++i)
                {
                    const std::string text = cellText(row, mList->getColumn(i)->mName);
                    LLScrollListCell* cell = item->getColumn(i);
                    if (cell && cell->getValue().asString() != text)
                    {
                        cell->setValue(text);
                        changed = true;
                    }
                }
                if (changed)
                {
                    mList->setNeedsSort();
                }
            }

            void removeRows(const std::vector<LLUUID>& ids)
            {
                if (!mList)
                {
                    return;
                }
                std::vector<LLScrollListItem*> rows;
                for (const LLUUID& id : ids)
                {
                    const auto found = mRows.find(id);
                    if (found != mRows.end())
                    {
                        rows.push_back(found->second);
                        mRows.erase(found);
                    }
                }
                mList->deleteRows(rows);
            }

            // Every row goes (and with them the table that finds them).
            void clearList()
            {
                mRows.clear();
                if (mList)
                {
                    mList->deleteAllItems();
                }
            }

            void showCounts(S32 listed, S32 pending, S32 total)
            {
                if (!mCounter)
                {
                    return;
                }
                LLStringUtil::format_map_t args;
                args["[LISTED]"]  = llformat("%d", listed);
                args["[PENDING]"] = llformat("%d", pending);
                args["[TOTAL]"]   = llformat("%d", total);
                mCounter->setText(getString("counter_filled", args));
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
                for (const ColumnDef& column_def : COLUMNS)
                {
                    if (def->mName == column_def.name && column_def.numeric)
                    {
                        const F64 number_a = numberIn(text_a);
                        const F64 number_b = numberIn(text_b);
                        return number_a < number_b ? -1 : (number_a > number_b ? 1 : 0);
                    }
                }
                return LLStringUtil::compareDict(text_a, text_b);
            }

            // Tab separated, a line of column titles first (for a spreadsheet).
            void copySelection()
            {
                const std::vector<LLScrollListItem*> selected = mList->getAllSelected();
                if (selected.empty())
                {
                    return;
                }
                std::string text;
                const S32 columns = mList->getNumColumns();
                for (S32 i = 0; i < columns; ++i)
                {
                    text += getString(std::string("col_") + mList->getColumn(i)->mName);
                    text += (i + 1 < columns) ? "\t" : "\n";
                }
                for (const LLScrollListItem* item : selected)
                {
                    for (S32 i = 0; i < columns; ++i)
                    {
                        const LLScrollListCell* cell = item->getColumn(i);
                        text += cell ? cell->getValue().asString() : std::string();
                        text += (i + 1 < columns) ? "\t" : "\n";
                    }
                }
                text.pop_back();
                const LLWString wide = utf8str_to_wstring(text);
                LLClipboard::instance().copyToClipboard(wide, 0, static_cast<S32>(wide.length()));
            }

            // The one selected object, with what the search knows of it (both or neither).
            bool selectedObject(LLViewerObject*& object, const Entry*& entry) const
            {
                if (mList->getNumSelected() != 1)
                {
                    return false;
                }
                const LLUUID id = mList->getFirstSelected()->getUUID();
                object = gObjectList.findObject(id);
                entry  = mEngine.find(id);
                return object && entry;
            }

            // Shows it on the map (the tracking beacon), and touches, buys or sits as chosen.
            void onDoubleClick()
            {
                LLViewerObject* object = nullptr;
                const Entry* entry     = nullptr;
                if (!selectedObject(object, entry))
                {
                    // An object the viewer has not made (research/218): the map beacon is all there is to do.
                    if (mList->getNumSelected() == 1)
                    {
                        const Entry* known = mEngine.find(mList->getFirstSelected()->getUUID());
                        if (known && known->cacheOnly)
                        {
                            LLTracker::trackLocation(known->position, known->name, "", LLTracker::LOCATION_ITEM);
                        }
                    }
                    return;
                }
                LLTracker::trackLocation(object->getPositionGlobal(), entry->name, "", LLTracker::LOCATION_ITEM);
                const U32 double_click = gSavedSettings.getU32(SETTING_DOUBLE_CLICK);
                if (double_click & DOUBLE_CLICK_BUY)
                {
                    buyObject(object, *entry);
                }
                if (double_click & DOUBLE_CLICK_TOUCH)
                {
                    touchObject(object);
                }
                if (double_click & DOUBLE_CLICK_SIT)
                {
                    sitOn(object);
                }
                if (double_click & DOUBLE_CLICK_ZOOM)
                {
                    zoomTo(object);
                }
            }

            // --- the list's menu (menu_plus_area_search.xml) ---

            void showMenu(S32 x, S32 y)
            {
                if (LLContextMenu* old_menu = mMenuHandle.get())
                {
                    old_menu->die();
                    mMenuHandle.markDead();
                }
                LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
                LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
                registrar.add("Plus.AreaSearch.Action", [this](LLUICtrl*, const LLSD& param) { onMenuAction(param.asString()); });
                enable_registrar.add("Plus.AreaSearch.Enable", [this](LLUICtrl*, const LLSD& param) { return menuEnabled(param.asString()); });

                LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
                    "menu_plus_area_search.xml", LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
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
                if (what == "one")
                {
                    return mList->getNumSelected() == 1;
                }
                if (what == "zoom")
                {
                    // Within the draw distance, or there is nothing to look at.
                    LLViewerObject* object = nullptr;
                    const Entry* entry     = nullptr;
                    return selectedObject(object, entry)
                        && dist_vec(gAgent.getPositionGlobal(), object->getPositionGlobal()) < gAgentCamera.mDrawDistance;
                }
                return mList->getNumSelected() > 0;
            }

            void onMenuAction(const std::string& action)
            {
                if (action == "select_all")
                {
                    mList->selectAll();
                }
                else if (action == "clear_selection")
                {
                    mList->deselectAllItems();
                }
                else if (action == "copy")
                {
                    copySelection();
                }
                else if (action == "filter_mine")
                {
                    // By the agent's id: the owner column may show a display name, and the
                    // account name is no regular expression for the whole text either.
                    LLLineEditor* owner = getChild<LLLineEditor>("owner_search");
                    owner->clear();
                    owner->setLabel(getString("owner_mine"));
                    mOnlyMine = true;
                    search(false);
                }
                else if (action == "touch")
                {
                    // One after another, so they arrive in order.
                    F32 delay = 0.f;
                    for (const LLScrollListItem* item : mList->getAllSelected())
                    {
                        const LLUUID id = item->getUUID();
                        LLEventTimer::run_after(delay, [id]() {
                            if (LLViewerObject* object = gObjectList.findObject(id))
                            {
                                touchObject(object);
                            }
                        });
                        delay += TOUCH_INTERVAL;
                    }
                }
                else if (action == "sit" || action == "teleport" || action == "zoom" || action == "buy")
                {
                    LLViewerObject* object = nullptr;
                    const Entry* entry     = nullptr;
                    if (!selectedObject(object, entry))
                    {
                        return;
                    }
                    if (action == "sit")
                    {
                        sitOn(object);
                    }
                    else if (action == "teleport")
                    {
                        gAgent.teleportViaLocation(object->getPositionGlobal());
                    }
                    else if (action == "zoom")
                    {
                        zoomTo(object);
                    }
                    else
                    {
                        buyObject(object, *entry);
                    }
                }
                else if (action == "inspect" || action == "edit" || action == "delete" || action == "return")
                {
                    actOnSelection(action);
                }
            }

            // The official tools on the selected objects, selected in the world first.
            void actOnSelection(const std::string& action)
            {
                LLSelectMgr::getInstance()->deselectAll();
                for (const LLScrollListItem* item : mList->getAllSelected())
                {
                    LLViewerObject* object = gObjectList.findObject(item->getUUID());
                    if (!object)
                    {
                        continue;
                    }
                    LLSelectMgr::getInstance()->selectObjectAndFamily(object);
                    if (action == "return")
                    {
                        // Returning checks the permissions of the selection.
                        if (const Entry* entry = mEngine.find(item->getUUID()))
                        {
                            selectWithProperties(object, *entry);
                        }
                    }
                }
                if (action == "inspect")
                {
                    LLFloaterReg::showInstance("inspect");
                }
                else if (action == "edit")
                {
                    handle_object_edit();
                }
                else if (action == "delete")
                {
                    handle_object_delete();
                }
                else
                {
                    handle_object_return();
                }
            }

            // The rows of the list by object id. Rows are made and deleted only through addRow(),
            // removeRows() and clearList(), which keep it.
            std::unordered_map<LLUUID, LLScrollListItem*> mRows;
            Engine mEngine;
            LLTabContainer* mTabs    = nullptr;
            ObjectList*     mList    = nullptr;
            LLTextBox*      mCounter = nullptr;
            LLTextBox*      mStatus  = nullptr;
            LLSearchEditor* mQuickFilter = nullptr;
            LLFrameTimer    mQuickFilterTimer;
            bool            mQuickFilterPending = false;
            LLHandle<LLContextMenu> mMenuHandle;
            bool mSearched = false;
            bool mOnlyMine = false; // the menu's "mine only": by the agent's id, not by a name
            bool mBeacons  = false;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_area_search.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<AreaSearchFloater>);
        }

        plus::FeatureRegistration sRegistration({ "areasearch", &init, {} });
    }

    void refreshNames()
    {
        if (AreaSearchFloater* floater = LLFloaterReg::findTypedInstance<AreaSearchFloater>(FLOATER_NAME))
        {
            floater->relistNames();
        }
    }
}
