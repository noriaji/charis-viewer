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
#include "llnotificationsutil.h"
#include "llscrollcontainer.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "lluictrlfactory.h"

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

        void backToLand()
        {
            if (isWater())
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
            // Chosen with the keyboard (Tab, then Return or Space): the new card takes the focus back.
            const LLHandle<LLView>& own = kind == pm::EnvKind::Water ? sW.waterCard : sW.skyCard;
            LLView* card = own.get();
            const bool had_focus = card && gFocusMgr.childHasKeyboardFocus(card);
            later([kind, had_focus]() {     // not from the card's own click: it is deleted by the refill
                fillLeft();
                if (had_focus)
                {
                    const LLHandle<LLView>& made = kind == pm::EnvKind::Water ? sW.waterCard : sW.skyCard;
                    if (LLUICtrl* again = live<LLUICtrl>(made))
                    {
                        again->setFocus(true);
                    }
                }
            });
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

            content->deleteAllChildren();
            sW.rows.clear();
            sW.landRow = LLHandle<LLView>();
            sW.chosenSource = -1;       // marked again below
            sW.listBuilt = true;
            sW.builtKind = sW.kind;
            sW.builtTab  = sW.tab;

            const S32 width = content->getRect().getWidth();
            const bool land = sW.tab == ListTab::All;
            const bool more = land && sW.more > 0;   // a last line: how many were left out
            const S32 count = (S32)sW.shown.size() + (land ? 1 : 0) + (more ? 1 : 0);
            const S32 height = llmax(visible, count * psui::style::LIST_ROW_HEIGHT);
            content->reshape(width, height);
            S32 top = height;

            if (land)
            {
                if (psui::PsListRow* row = makeRow(ls(isWater() ? "WaterBackLand" : "SkyBackLand"), ls("LandTip"), false, top, width))
                {
                    row->setCommitCallback([](LLUICtrl*, const LLSD&) { backToLand(); });
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
                row->setCommitCallback([item](LLUICtrl*, const LLSD&) { putOn(item); });
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
                if (view_changed || !sameOrder(sW.next, sW.shown) || sW.nextMore != sW.more)
                {
                    sW.more = sW.nextMore;
                    sW.shown.swap(sW.next);
                    buildRows();
                }
                else
                {
                    // the same rows: only the stars that changed (a click on a star, the photo tools)
                    for (size_t i = 0; i < sW.shown.size(); ++i)
                    {
                        if (sW.shown[i].starred == sW.next[i].starred)
                        {
                            continue;
                        }
                        sW.shown[i].starred = sW.next[i].starred;
                        if (i < sW.rows.size())
                        {
                            if (psui::PsListRow* row = live<psui::PsListRow>(sW.rows[i]))
                            {
                                row->setStarred(sW.shown[i].starred);
                            }
                        }
                    }
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
            sW.skyCard = card->getHandle();
        }
        stack.label(ls("Water"));
        if (psui::PsCard* card = stack.card("thumb", "water", std::string(), std::string(), ls("WaterTip"),
                                            []() { onCard(pm::EnvKind::Water); }))
        {
            card->setChosen(sW.open && isWater());
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
