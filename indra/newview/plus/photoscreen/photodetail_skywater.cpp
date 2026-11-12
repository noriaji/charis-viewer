/**
 * @file photodetail_skywater.cpp
 * @brief The sky and the water of the detailed photo screen's scene tab (research/233, ps-sky-water).
 *
 * What is where: a card for the sky and one for the water (the one in use, as the target picture's
 * "Windlight" card). The cards choose the list: a click on one opens its list under them (the card is
 * drawn chosen), a click on the chosen one closes it. The list has the tabs All / Favorites / Recent
 * and a scrolled list of rows (psui::PsListRow). A click on a row puts it on (the
 * photo tools' way: applyEnvironmentItem, then noteApplied, which makes the recent list); the star of a
 * row adds it to or takes it out of the favourites. All starts with "back to the land's".
 *
 * The lists are the photo tools' (photosky.h): the favourites and the recent ones are kept per account,
 * as their item ids are the account's own, and the photo tools window shows the same stars.
 *
 * Speed: the rows are made again only when what they show changed (the kind, the tab, the lists'
 * generation, and every LIST_SECONDS the inventory's skies for All). Every tick only the cards' names
 * and the row in use are compared with what is shown.
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

#include "photodetail_skywater.h"

#include "photodetail_left.h"
#include "photodetail_state.h"
#include "plus/photomode/photomode.h"
#include "plus/photomode/photosky.h"
#include "plus/photoscreen/psui/psstack.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/photoscreen/psui/pswidgets.h"

#include <set>

#include "llenvironment.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llscrollcontainer.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "lluictrlfactory.h"
#include "llviewerinput.h"

namespace plus::photoscreen::detail::skywater
{
    namespace
    {
        using psui::Stack;

        enum class ListTab { All, Favorites, Recent, Count };
        constexpr int LIST_TABS = static_cast<int>(ListTab::Count);
        const char* const LIST_TAB_WORDS[LIST_TABS] = { "ListAll", "ListFav", "ListRecent" };

        constexpr S32    LIST_ROWS    = 7;          // rows seen at once; more scroll
        constexpr S32    TABS_H       = 34;
        constexpr F64    LIST_SECONDS = 3.0;        // the inventory looked at again (All), as the simple view does
        constexpr size_t LIST_MAX     = 1000;
        constexpr F64    STEP_REPEAT_SECONDS = 0.12;    // a held arrow key: about 8 rows a second (as the drop-downs)

        enum class CursorAt { None, Land, Item };   // the row of the arrow keys (State::cursorAt)

        // A row the arrow keys go through: the land's or an item that can be put on.
        struct Stop
        {
            bool   land = false;
            LLUUID item, asset;
        };

        // What a row shows. The rows are made again only when the order changed (sameRow); a star alone is
        // set on the row that is there.
        struct Shown
        {
            LLUUID      item, asset;
            std::string name;
            std::vector<LLUUID> same;               // the copies folded into this row (environmentItems)
            bool        exists  = true;
            bool        starred = false;
            bool sameRow(const Shown& o) const
            {
                return item == o.item && asset == o.asset && name == o.name && exists == o.exists && same == o.same;
            }
        };

        bool sameOrder(const std::vector<Shown>& a, const std::vector<Shown>& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (!a[i].sameRow(b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        struct State
        {
            // kept while the photo screen is used (not a setting)
            bool        open = false;               // the list is shown
            pm::EnvKind kind = pm::EnvKind::Sky;
            ListTab     tab  = ListTab::All;

            // the widgets of the column shown (null when another tab is shown)
            LLHandle<LLView> skyCard, waterCard, scroll, content;
            std::vector<LLHandle<LLView>> rows;     // by index of shown (the land's row is landRow)
            LLHandle<LLView> landRow;

            // what they show
            std::string skyName, waterName;
            const void* skyObject = nullptr;        // compared only, never used
            const void* waterObject = nullptr;
            pm::EnvSource skySource = pm::EnvSource::Other, waterSource = pm::EnvSource::Other;
            bool        namesSet = false;

            std::vector<Shown> shown, next;         // the rows shown / read again (reused)
            size_t      more = 0, nextMore = 0;     // the All tab: the entries past LIST_MAX (shown / read again)
            std::vector<pm::SkyEntry> entries;      // reused
            bool        listBuilt = false;
            pm::EnvKind builtKind = pm::EnvKind::Sky;
            ListTab     builtTab  = ListTab::All;
            unsigned    generation = ~0u;
            F64         nextCollect = 0.0;
            int         chosenSource = -1;          // the row in use was marked for this source and asset
            LLUUID      chosenAsset;

            // the row the arrow keys are on: the last one put on with the keys or a click (combo-arrow). Not the
            // one in use: that changes only when the sky has come, and the next key must not wait for it.
            CursorAt    cursorAt   = CursorAt::None;
            pm::EnvKind cursorKind = pm::EnvKind::Sky;
            LLUUID      cursorItem;                 // CursorAt::Item
            U32         cursorChanges = 0;          // the layer's count of changes just after it was put on
            // what the layer showed just after it was put on (the one before, while it comes): a failed one counts
            // while it still shows that (nothing outside, an official editor, changed it since)
            pm::EnvSource cursorSource = pm::EnvSource::Other;
            LLUUID      cursorSourceAsset;
            bool        steppedByKeys = false;      // the last row was put on with an arrow key (not a click)
            F64         lastStepSeconds = 0.0;      // LLTimer::getTotalSeconds() when the last arrow key's step ended
        };
        State sW;

        std::string ls(const std::string& name)
        {
            LLPanel* left = sS.left.get();
            return left ? left->getString(name) : name;
        }

        std::string ls(const std::string& name, const LLStringUtil::format_map_t& args)
        {
            LLPanel* left = sS.left.get();
            return left ? left->getString(name, args) : name;
        }

        template <class T>
        T* live(const LLHandle<LLView>& handle)
        {
            return static_cast<T*>(handle.get());
        }

        bool isWater()
        {
            return sW.kind == pm::EnvKind::Water;
        }

        void tell(const std::string& message)
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
        }

        // ---- acting -------------------------------------------------------------------------------

        // Puts a sky or water on; noted in the recent list only when it could be put on.
        void putOn(const LLUUID& item)
        {
            pm::EnvKind kind = pm::EnvKind::Sky;
            if (item.isNull() || !pm::environmentKind(item, kind) || !pm::applyEnvironmentItem(item))
            {
                tell(ls("ItemGone"));
                return;
            }
            pm::noteApplied(item);
        }

        void backToLand(pm::EnvKind kind)
        {
            if (kind == pm::EnvKind::Water)
            {
                pm::restoreLandWater();
            }
            else
            {
                pm::restoreLandSky();
            }
        }

        // The star of a row. A row of All may stand for copies of the same sky too (same): a star of any of
        // them lights the row, and taking it off takes them all off, so it does not light again.
        void toggleFavorite(const LLUUID& item, const std::vector<LLUUID>& same)
        {
            bool starred = pm::isFavorite(item);
            for (const LLUUID& other : same)
            {
                starred = starred || pm::isFavorite(other);
            }
            if (starred)
            {
                pm::removeFavorite(item);
                for (const LLUUID& other : same)
                {
                    pm::removeFavorite(other);
                }
                return;
            }
            switch (pm::addFavorite(item))
            {
                case pm::AddFavorite::Full:     tell(ls("FavFull"));     break;
                case pm::AddFavorite::NotFound: tell(ls("ItemGone"));    break;
                default: break;
            }
        }

        // ---- the cards ----------------------------------------------------------------------------

        // The column made again (later: not from a widget's own callback, the refill deletes it), then the card of
        // the kind takes the keyboard.
        void refillAndFocusCard(pm::EnvKind kind)
        {
            later([kind]() {
                fillLeft();
                const LLHandle<LLView>& made = kind == pm::EnvKind::Water ? sW.waterCard : sW.skyCard;
                if (LLUICtrl* card = live<LLUICtrl>(made))
                {
                    card->setFocus(true);
                }
            });
        }

        // The card of a kind: choosing the other kind shows its list; the same kind again opens or
        // closes the list.
        void onCard(pm::EnvKind kind)
        {
            if (sW.open && sW.kind == kind)
            {
                sW.open = false;
            }
            else
            {
                sW.open = true;
                sW.kind = kind;
            }
            // The arrow keys start again from the row in use (the list opened or closed by the user).
            sW.cursorAt = CursorAt::None;
            sW.steppedByKeys = false;
            // With the keyboard (Tab, then Return or Space) or a click: the new card takes the keyboard, so the
            // arrow keys go through the list from it (onListKey()).
            refillAndFocusCard(kind);
        }

        std::string nameOf(bool water, pm::EnvSource source)
        {
            if (source == pm::EnvSource::Land)
            {
                return ls(water ? "WaterLand" : "SkyLand");
            }
            LLEnvironment& environment = LLEnvironment::instance();
            const std::string name = water ? (environment.getCurrentWater() ? environment.getCurrentWater()->getName() : std::string())
                                           : (environment.getCurrentSky() ? environment.getCurrentSky()->getName() : std::string());
            return name.empty() ? ls(water ? "WaterCustom" : "SkyCustom") : name;
        }

        void refreshCards()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            LLUUID asset;
            const pm::EnvSource sky_source   = pm::environmentSource(false, asset);
            const pm::EnvSource water_source = pm::environmentSource(true, asset);
            const void* sky   = environment.getCurrentSky().get();
            const void* water = environment.getCurrentWater().get();
            if (!sW.namesSet || sky != sW.skyObject || sky_source != sW.skySource)
            {
                sW.skyObject = sky;
                sW.skySource = sky_source;
                sW.skyName   = nameOf(false, sky_source);
                if (psui::PsCard* card = live<psui::PsCard>(sW.skyCard))
                {
                    card->setLabelText(sW.skyName);
                }
            }
            if (!sW.namesSet || water != sW.waterObject || water_source != sW.waterSource)
            {
                sW.waterObject = water;
                sW.waterSource = water_source;
                sW.waterName   = nameOf(true, water_source);
                if (psui::PsCard* card = live<psui::PsCard>(sW.waterCard))
                {
                    card->setLabelText(sW.waterName);
                }
            }
            sW.namesSet = true;
        }

        // ---- the arrow keys (combo-arrow) ---------------------------------------------------------
        // On a card or a row of the list: Up / Down put on the row before / after, as a click on it does; Esc
        // closes the list. The keyboard is there after a click on a card or a row (or Tab); Left / Right and the
        // other keys go on as before (the world's).

        // The keyboard is on a card or a row of the list.
        bool keysInList()
        {
            for (const LLHandle<LLView>* handle : { &sW.skyCard, &sW.waterCard, &sW.content })
            {
                LLView* view = handle->get();
                if (view && gFocusMgr.childHasKeyboardFocus(view))
                {
                    return true;
                }
            }
            return false;
        }

        bool listShown(pm::EnvKind kind)
        {
            return sW.open && sW.kind == kind && sW.listBuilt;
        }

        // The rows the keys go through, in their order: the land's first (the All tab), then the items that can be
        // put on (a gone one is passed over). The list shown goes as it is shown, in its tab; a card whose list is
        // closed goes through the All tab of its kind.
        void collectStops(pm::EnvKind kind, std::vector<Stop>& out)
        {
            out.clear();
            const bool shown = listShown(kind);
            if (!shown || sW.tab == ListTab::All)
            {
                Stop land;
                land.land = true;
                out.push_back(land);
            }
            auto add = [&out](bool exists, const LLUUID& item, const LLUUID& asset) {
                if (exists)
                {
                    Stop stop;
                    stop.item  = item;
                    stop.asset = asset;
                    out.push_back(stop);
                }
            };
            if (shown)
            {
                for (const Shown& row : sW.shown)
                {
                    add(row.exists, row.item, row.asset);
                }
                return;
            }
            std::vector<pm::SkyEntry> entries;
            pm::environmentItems(kind, entries, LIST_MAX);
            for (const pm::SkyEntry& entry : entries)
            {
                add(entry.exists, entry.item, entry.asset);
            }
        }

        pm::EnvLayer layerOf(pm::EnvKind kind)
        {
            return kind == pm::EnvKind::Water ? pm::EnvLayer::Water : pm::EnvLayer::Sky;
        }

        // Puts on a row (the land's or an item), as a click on it does, and sets the cursor on it.
        void putOnRow(pm::EnvKind kind, CursorAt at, const LLUUID& item)
        {
            if (at == CursorAt::Land)
            {
                backToLand(kind);
            }
            else
            {
                putOn(item);
            }
            sW.cursorKind    = kind;
            sW.cursorAt      = at;
            sW.cursorItem    = at == CursorAt::Item ? item : LLUUID::null;
            sW.cursorChanges = pm::localEnvironmentChanges(layerOf(kind));   // after it: the count it made
            sW.cursorSource  = pm::environmentSource(kind == pm::EnvKind::Water, sW.cursorSourceAsset);
        }

        // The stop the keys go on from: the cursor, else the row in use; -1: none (a sky of no row). The cursor counts
        // while nothing changed the layer since (its number) and its sky is still coming, could not be had while the
        // layer still shows what it showed when it was asked for (the keys go on past it), or is in use (the row
        // itself: another row of the same sky is not it); else (the time slider, the photo tools, the quick
        // preferences, an official editor) it is forgotten and the row in use counts.
        int currentStop(pm::EnvKind kind, const std::vector<Stop>& stops)
        {
            LLUUID asset;
            const pm::EnvSource source = pm::environmentSource(kind == pm::EnvKind::Water, asset);
            auto in_use = [&](const Stop& stop) {
                return stop.land ? source == pm::EnvSource::Land : source == pm::EnvSource::Asset && stop.asset == asset;
            };
            if (sW.cursorAt != CursorAt::None && sW.cursorKind == kind)
            {
                const pm::EnvLayer layer = layerOf(kind);
                const pm::EnvFetch fetch = pm::localEnvironmentFetch(layer);
                const bool failed_here = fetch == pm::EnvFetch::Failed && source == sW.cursorSource
                                         && asset == sW.cursorSourceAsset;
                for (size_t i = 0; i < stops.size(); ++i)
                {
                    const bool here = stops[i].land ? sW.cursorAt == CursorAt::Land
                                                    : sW.cursorAt == CursorAt::Item && stops[i].item == sW.cursorItem;
                    if (here)
                    {
                        if (sW.cursorChanges == pm::localEnvironmentChanges(layer)
                            && (fetch == pm::EnvFetch::Pending || failed_here || in_use(stops[i])))
                        {
                            return (int)i;
                        }
                        break;
                    }
                }
                sW.cursorAt = CursorAt::None;
            }
            for (size_t i = 0; i < stops.size(); ++i)
            {
                if (in_use(stops[i]))
                {
                    return (int)i;
                }
            }
            return -1;
        }

        // The keyboard and the view to the cursor's row of the list shown; false: it has no row there.
        bool focusCursorRow()
        {
            LLScrollContainer* scroll = live<LLScrollContainer>(sW.scroll);
            if (!scroll || sW.cursorKind != sW.kind)
            {
                return false;
            }
            LLUICtrl* row = nullptr;
            if (sW.cursorAt == CursorAt::Land)
            {
                row = live<LLUICtrl>(sW.landRow);
            }
            else if (sW.cursorAt == CursorAt::Item)
            {
                for (size_t i = 0; i < sW.shown.size() && i < sW.rows.size() && !row; ++i)
                {
                    if (sW.shown[i].item == sW.cursorItem)
                    {
                        row = live<LLUICtrl>(sW.rows[i]);
                    }
                }
            }
            if (!row)
            {
                return false;
            }
            row->setFocus(true);
            scroll->scrollToShowRect(row->getRect());
            return true;
        }

        // Up (-1) or Down (+1): puts on the row before or after the cursor; at either end it stays (no wrap). In
        // the list shown the keyboard follows the row. A held key goes on, a row STEP_REPEAT_SECONDS at the soonest
        // after the last one was put on (each row put on fetches its sky and writes the recent list: not at the
        // keyboard's repeat rate); the repeats in between are taken and do nothing. A press always steps.
        bool step(pm::EnvKind kind, int dir)
        {
            const F64 now_seconds = LLTimer::getTotalSeconds();
            if (gKeyboard && gKeyboard->getKeyRepeated(dir > 0 ? KEY_DOWN : KEY_UP)
                && now_seconds - sW.lastStepSeconds < STEP_REPEAT_SECONDS)
            {
                return true;
            }
            sW.lastStepSeconds = now_seconds;
            std::vector<Stop> stops;
            collectStops(kind, stops);
            const int current = currentStop(kind, stops);
            const int next = current < 0 ? (dir > 0 ? 0 : (int)stops.size() - 1) : current + dir;
            if (next < 0 || next >= (int)stops.size())
            {
                return true;
            }
            const Stop stop = stops[next];
            putOnRow(kind, stop.land ? CursorAt::Land : CursorAt::Item, stop.item);
            sW.lastStepSeconds = LLTimer::getTotalSeconds();    // after putting on: a slow one counts in the wait
            sW.steppedByKeys = true;
            if (listShown(kind))
            {
                focusCursorRow();
            }
            return true;
        }

        // The keys of a card and of the rows of its kind's list. Return / Space are the widget's own (a click; with
        // Shift on a row: the star).
        bool onListKey(pm::EnvKind kind, KEY key, MASK mask)
        {
            if (mask == MASK_NONE)
            {
                switch (key)
                {
                    case KEY_UP:   return step(kind, -1);
                    case KEY_DOWN: return step(kind, 1);
                    case KEY_ESCAPE:
                        if (!sW.open || sW.kind != kind)
                        {
                            return false;
                        }
                        sW.open = false;
                        sW.cursorAt = CursorAt::None;   // as a close by the card (onCard)
                        sW.steppedByKeys = false;
                        refillAndFocusCard(kind);
                        return true;
                    case KEY_RETURN:
                    case ' ':
                        return false;
                    default:
                        break;
                }
            }
            // The keyboard came with a click, not to keep the world's keys (WASD, Left / Right, the photo keys): a key
            // bound in the mode now lets it go and goes on to the world, as before the click. Not with Ctrl or Alt
            // (the menus' and the floaters' shortcuts; as flycammouse::releaseFocusForKey()).
            if (!(mask & (MASK_CONTROL | MASK_ALT)) && gViewerInput.isKeyBound((S32)gViewerInput.getMode(), key, mask))
            {
                gFocusMgr.setKeyboardFocus(nullptr);
            }
            return false;
        }

        // ---- the list -----------------------------------------------------------------------------

        // The rows of the kind and tab as they are now, into sW.next.
        void collect()
        {
            sW.entries.clear();
            sW.nextMore = 0;
            switch (sW.tab)
            {
                case ListTab::All:
                {
                    const size_t total = pm::environmentItems(sW.kind, sW.entries, LIST_MAX);
                    sW.nextMore = total > sW.entries.size() ? total - sW.entries.size() : 0;
                    break;
                }
                case ListTab::Favorites: sW.entries = pm::favorites(sW.kind);                 break;
                default:                 sW.entries = pm::recents(sW.kind);                   break;
            }
            const std::set<LLUUID> starred = pm::favoriteItems();
            sW.next.clear();
            for (const pm::SkyEntry& entry : sW.entries)
            {
                Shown row;
                row.item    = entry.item;
                row.asset   = entry.asset;
                row.name    = entry.name;
                row.same    = entry.same;
                row.exists  = entry.exists;
                row.starred = starred.count(entry.item) > 0;
                for (const LLUUID& other : entry.same)
                {
                    row.starred = row.starred || starred.count(other) > 0;
                }
                sW.next.push_back(std::move(row));
            }
        }

        psui::PsListRow* makeRow(const std::string& label, const std::string& tip, bool starrable, S32 top, S32 width)
        {
            psui::PsListRow::Params p(LLUICtrlFactory::getDefaultParams<psui::PsListRow>());
            p.name          = "row";
            p.rect          = LLRect(0, top, width, top - psui::style::LIST_ROW_HEIGHT);
            p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
            p.label         = label;
            p.tool_tip      = tip;
            p.starrable     = starrable;
            return LLUICtrlFactory::create<psui::PsListRow>(p);
        }

        // Makes the rows of sW.shown in the list's panel (outside of any of their own callbacks).
        void buildRows()
        {
            LLScrollContainer* scroll = live<LLScrollContainer>(sW.scroll);
            LLPanel* content = live<LLPanel>(sW.content);
            if (!scroll || !content)
            {
                return;
            }
            const bool same_view = sW.listBuilt && sW.builtKind == sW.kind && sW.builtTab == sW.tab;
            const S32  doc_pos   = scroll->getDocPosVertical();
            const S32  visible   = scroll->getRect().getHeight();
            const bool had_keys  = gFocusMgr.childHasKeyboardFocus(content);   // a row had the keyboard (it is deleted)

            content->deleteAllChildren();
            sW.rows.clear();
            sW.landRow = LLHandle<LLView>();
            sW.chosenSource = -1;       // marked again below
            sW.listBuilt = true;
            sW.builtKind = sW.kind;
            sW.builtTab  = sW.tab;

            const S32 width = content->getRect().getWidth();
            const bool land = sW.tab == ListTab::All;
            const pm::EnvKind kind = sW.kind;
            const bool more = land && sW.more > 0;   // a last line: how many were left out
            const S32 count = (S32)sW.shown.size() + (land ? 1 : 0) + (more ? 1 : 0);
            const S32 height = llmax(visible, count * psui::style::LIST_ROW_HEIGHT);
            content->reshape(width, height);
            S32 top = height;

            if (land)
            {
                if (psui::PsListRow* row = makeRow(ls(isWater() ? "WaterBackLand" : "SkyBackLand"), ls("LandTip"), false, top, width))
                {
                    row->setCommitCallback([kind](LLUICtrl* ctrl, const LLSD&) {
                        putOnRow(kind, CursorAt::Land, LLUUID::null);
                        sW.steppedByKeys = false;
                        ctrl->setFocus(true);   // the arrow keys go on from here
                    });
                    row->setKeyCallback([kind](KEY key, MASK mask) { return onListKey(kind, key, mask); });
                    content->addChild(row);
                    sW.landRow = row->getHandle();
                }
                top -= psui::style::LIST_ROW_HEIGHT;
            }
            for (const Shown& shown : sW.shown)
            {
                LLStringUtil::format_map_t args;
                args["[NAME]"] = shown.name;
                const std::string label = shown.exists ? shown.name : ls("Missing", args);
                // A gone item of the recent list has no star: it cannot be added, and only newer ones push it out.
                const bool recent_gone = !shown.exists && sW.tab == ListTab::Recent;
                const std::string tip = shown.exists ? ls("RowTip") : ls(recent_gone ? "MissingTipRecent" : "MissingTip");
                psui::PsListRow* row = makeRow(label, tip, !recent_gone, top, width);
                top -= psui::style::LIST_ROW_HEIGHT;
                if (!row)
                {
                    sW.rows.emplace_back();
                    continue;
                }
                row->setStarred(shown.starred);
                row->setMissing(!shown.exists);
                const LLUUID item = shown.item;
                row->setCommitCallback([kind, item](LLUICtrl* ctrl, const LLSD&) {
                    putOnRow(kind, CursorAt::Item, item);
                    sW.steppedByKeys = false;
                    ctrl->setFocus(true);       // the arrow keys go on from here
                });
                row->setKeyCallback([kind](KEY key, MASK mask) { return onListKey(kind, key, mask); });
                row->setStarCallback([item, same = shown.same](psui::PsListRow*) { toggleFavorite(item, same); });
                content->addChild(row);
                sW.rows.push_back(row->getHandle());
            }
            if (more)
            {
                LLStringUtil::format_map_t args;
                args["[COUNT]"] = std::to_string(sW.more);
                content->addChild(psui::makeNote(
                    "more", LLRect(psui::style::LIST_ROW_PAD, top, width - psui::style::LIST_ROW_PAD, top - psui::style::LIST_ROW_HEIGHT),
                    ls("ListMore", args), psui::font(psui::style::FONT_SMALL), psui::style::TEXT_SUB));
            }
            if (sW.shown.empty() && !land)
            {
                content->addChild(psui::makeNote(
                    "empty", LLRect(psui::style::LIST_ROW_PAD, height - 6, width - psui::style::LIST_ROW_PAD, height - 60),
                    ls(sW.tab == ListTab::Favorites ? "EmptyFav" : "EmptyRecent"), psui::font(psui::style::FONT_SMALL),
                    psui::style::TEXT_SUB));
            }

            if (same_view && doc_pos > 0)
            {
                const S32 shown_top = llmax(0, height - doc_pos);
                scroll->scrollToShowRect(LLRect(0, shown_top, width, llmax(0, shown_top - visible)));
            }
            else
            {
                scroll->goToTop();
            }

            // Made again under the keys (the inventory changed): the keyboard stays on the cursor's row, or goes to
            // the card when that row is gone (not later: the card is not made again here).
            if (had_keys && !focusCursorRow())
            {
                if (LLUICtrl* card = live<LLUICtrl>(isWater() ? sW.waterCard : sW.skyCard))
                {
                    card->setFocus(true);
                }
            }
        }

        // Marks the row in use (only when the sky or water in use changed, or the rows were made again).
        void refreshChosen()
        {
            LLUUID asset;
            const pm::EnvSource source = pm::environmentSource(isWater(), asset);
            const int source_id = static_cast<int>(source);
            if (source_id == sW.chosenSource && (source != pm::EnvSource::Asset || asset == sW.chosenAsset))
            {
                return;
            }
            sW.chosenSource = source_id;
            sW.chosenAsset  = asset;
            if (LLUICtrl* land = live<LLUICtrl>(sW.landRow))
            {
                land->setValue(LLSD(source == pm::EnvSource::Land));
            }
            for (size_t i = 0; i < sW.rows.size() && i < sW.shown.size(); ++i)
            {
                if (LLUICtrl* row = live<LLUICtrl>(sW.rows[i]))
                {
                    row->setValue(LLSD(source == pm::EnvSource::Asset && sW.shown[i].exists && sW.shown[i].asset == asset));
                }
            }
        }

        enum class StarMatch { ByIndex, ByItem };

        // The stars of the rows shown, from those read again (sW.next) where they changed: by index when the rows
        // are the same, by item when they are the same ones in another order (the Recent tab kept).
        void copyStars(StarMatch match)
        {
            for (size_t i = 0; i < sW.shown.size(); ++i)
            {
                const Shown* fresh = nullptr;
                if (match == StarMatch::ByIndex)
                {
                    fresh = i < sW.next.size() ? &sW.next[i] : nullptr;
                }
                else
                {
                    for (const Shown& row : sW.next)
                    {
                        if (row.item == sW.shown[i].item)
                        {
                            fresh = &row;
                            break;
                        }
                    }
                }
                if (!fresh || fresh->starred == sW.shown[i].starred)
                {
                    continue;
                }
                sW.shown[i].starred = fresh->starred;
                if (i < sW.rows.size())
                {
                    if (psui::PsListRow* row = live<psui::PsListRow>(sW.rows[i]))
                    {
                        row->setStarred(sW.shown[i].starred);
                    }
                }
            }
        }

        void refreshList()
        {
            if (!live<LLPanel>(sW.content))
            {
                return;
            }
            const F64 now = LLTimer::getTotalSeconds();
            const bool view_changed = !sW.listBuilt || sW.builtKind != sW.kind || sW.builtTab != sW.tab;
            if (view_changed || pm::favoritesGeneration() != sW.generation || now >= sW.nextCollect)
            {
                // favorites() may write the list (other accounts' library favourites): the generation after it
                collect();
                sW.generation  = pm::favoritesGeneration();
                sW.nextCollect = now + LIST_SECONDS;
                // The Recent tab keeps its order while the arrow keys go through it: the one put on goes to its top,
                // and the next key would go back to the one before. Made again at the next look (LIST_SECONDS, or a
                // change of the favourites) after a click or once the keyboard has left; the stars still change.
                const bool keep_order = !view_changed && sW.tab == ListTab::Recent && sW.steppedByKeys && keysInList();
                if (keep_order)
                {
                    copyStars(StarMatch::ByItem);
                }
                else if (view_changed || !sameOrder(sW.next, sW.shown) || sW.nextMore != sW.more)
                {
                    sW.more = sW.nextMore;
                    sW.shown.swap(sW.next);
                    buildRows();
                }
                else
                {
                    copyStars(StarMatch::ByIndex);  // the same rows (a click on a star, the photo tools)
                }
            }
            refreshChosen();
        }

        void chooseTab(ListTab tab)
        {
            if (sW.tab != tab)
            {
                sW.tab = tab;
                refreshList();
            }
        }
    }

    void fill(Stack& stack)
    {
        // The cards choose the list: the one whose list is open is drawn chosen (the column is made again
        // on every choice, so this is set only here).
        stack.label(ls("Sky"));
        if (psui::PsCard* card = stack.card("thumb", "sun", std::string(), std::string(), ls("SkyTip"),
                                            []() { onCard(pm::EnvKind::Sky); }))
        {
            card->setChosen(sW.open && !isWater());
            card->setKeyCallback([](KEY key, MASK mask) { return onListKey(pm::EnvKind::Sky, key, mask); });
            sW.skyCard = card->getHandle();
        }
        stack.label(ls("Water"));
        if (psui::PsCard* card = stack.card("thumb", "water", std::string(), std::string(), ls("WaterTip"),
                                            []() { onCard(pm::EnvKind::Water); }))
        {
            card->setChosen(sW.open && isWater());
            card->setKeyCallback([](KEY key, MASK mask) { return onListKey(pm::EnvKind::Water, key, mask); });
            sW.waterCard = card->getHandle();
        }
        if (!sW.open)
        {
            return;
        }
        const S32 width = stack.innerWidth();

        // All / Favorites / Recent
        {
            psui::PsRow::Params p(LLUICtrlFactory::getDefaultParams<psui::PsRow>());
            p.name          = "list_tabs";
            p.rect          = LLRect(0, TABS_H, width, 0);
            p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
            p.kind          = "tabs";
            p.spacing       = 0;
            psui::PsRow* row = LLUICtrlFactory::create<psui::PsRow>(p);
            if (row)
            {
                for (int i = 0; i < LIST_TABS; ++i)
                {
                    psui::PsTab::Params t(LLUICtrlFactory::getDefaultParams<psui::PsTab>());
                    t.name          = LIST_TAB_WORDS[i];
                    t.rect          = LLRect(i * 80, TABS_H, i * 80 + 80, 0);
                    t.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
                    t.label         = ls(LIST_TAB_WORDS[i]);
                    t.initial_value = i == static_cast<int>(sW.tab);
                    if (psui::PsTab* tab = LLUICtrlFactory::create<psui::PsTab>(t))
                    {
                        const ListTab which = static_cast<ListTab>(i);
                        tab->setCommitCallback([which](LLUICtrl*, const LLSD&) { chooseTab(which); });
                        row->addChild(tab);
                    }
                }
                row->arrange();
            }
            stack.view(row, TABS_H);
        }

        // the list
        {
            const S32 height = LIST_ROWS * psui::style::LIST_ROW_HEIGHT;
            LLScrollContainer::Params p;
            p.name                  = "list";
            p.rect                  = LLRect(0, height, width, 0);
            p.follows.flags         = FOLLOWS_LEFT | FOLLOWS_TOP;
            p.border_visible        = false;
            p.is_opaque             = false;
            p.reserve_scroll_corner = false;
            p.tab_stop              = false;
            // the rows move with the keys themselves (onListKey, scrollToShowRect); a key let go to the world
            // (Home, Shift+Up, Page Down) goes up through here and must not scroll the list on its way
            p.ignore_arrow_keys     = true;
            LLScrollContainer* scroll = LLUICtrlFactory::create<LLScrollContainer>(p);
            if (scroll)
            {
                static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);
                LLPanel::Params c;
                c.name               = "list_content";
                c.rect               = LLRect(0, height, llmax(1, width - (S32)scrollbar_size), 0);
                c.follows.flags      = FOLLOWS_LEFT | FOLLOWS_TOP;
                c.background_visible = false;
                c.mouse_opaque       = false;
                LLPanel* content = LLUICtrlFactory::create<LLPanel>(c);
                if (content)
                {
                    scroll->addChild(content);
                    sW.content = content->getHandle();
                }
                sW.scroll = scroll->getHandle();
            }
            stack.view(scroll, height, 4);
        }
        sW.listBuilt = false;   // made at the first refresh, after the column is placed
    }

    void refresh()
    {
        refreshCards();
        if (sW.open)
        {
            refreshList();
        }
    }

    void clear()
    {
        sW.skyCard = sW.waterCard = sW.scroll = sW.content = sW.landRow = LLHandle<LLView>();
        sW.rows.clear();
        sW.namesSet = false;
        sW.listBuilt = false;
        sW.shown.clear();
        sW.chosenSource = -1;
    }
}
