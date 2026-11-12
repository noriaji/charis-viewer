/**
 * @file phototools_env.cpp
 * @brief The photo tools' sky, water and look lists (the Sky & Weather and Light & Color blocks; research/265:
 *        made over to psui by pt-ui-env, moved here as they were by pt-ui-frame).
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

// What the frame (phototoolsfloater.cpp) calls: buildEnv() from postBuild(), openEnv() from onOpen(), drawEnv()
// every frame and tickEnv() every REFRESH_SECONDS. The blocks are the XUI's (panel_plus_photo_tools_light.xml:
// sec_sky, sec_water; _color.xml: sec_tone_note, sec_look; _home.xml: the sky and water buttons of Home); a
// block that changes its height or is shown or hidden asks the frame to lay the page out again (requestLayout()).

#include "llviewerprecompiledheaders.h"

#include "phototoolsfloater_impl.h"
#include "plus/look/look.h"

#include <algorithm>
#include <map>
#include <set>

#include "llagent.h"
#include "llbutton.h"
#include "llenvironment.h"
#include "llfiltereditor.h"
#include "llflatlistview.h"
#include "llfloaterreg.h"
#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llfolderviewmodelinventory.h"
#include "llframetimer.h"
#include "llinventorybridge.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "llinventorysettings.h"
#include "llnotificationsutil.h"
#include "llrender2dutils.h"
#include "llscrollcontainer.h"
#include "llscrolllistctrl.h"
#include "llsettingssky.h"
#include "llsettingsvo.h"
#include "llsettingswater.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "lluiimage.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

namespace plus::photomode::tools
{
    namespace
    {
        // --- What the lists show of a sky or a water besides its name -------
        //
        // A sky: whether it is a "classic" one, whose settings have no reflection_probe_ambiance
        // (LLSettingsSky::canAutoAdjust(), llsettingssky.cpp): made before the HDR skies, or converted from
        // a Windlight one. A water: a dot of its fog colour, to tell waters apart when their names do not.
        // The inventory item says neither; the asset has to be read. So an item is looked at once, when
        // its row is drawn (a row off the screen costs nothing), a few at a time, and the answer is kept
        // for the session by the asset id. No answer yet, or none to be had (the asset could not be
        // fetched): no mark, no dot.
        struct EnvLook
        {
            enum class State
            {
                Unknown,
                Fetching,
                Done,
                Failed
            };
            State    state   = State::Unknown;
            bool     sky     = false;
            bool     classic = false; // a sky of the old format
            LLColor4 color;           // a water's fog colour, lifted to be seen (swatchColor())
        };

        enum class SkyKind
        {
            Unknown,
            Classic,
            Modern
        };

        std::map<LLUUID, EnvLook> sLooks;
        int                       sLookFetches = 0;
        constexpr int             MAX_LOOK_FETCHES = 3;   // at a time
        std::string               sMarkClassic = " (old format)";
        std::string               sMarkModern  = " (HDR)";

        // The fog colours are dark (the default water's is 4, 38, 64 of 255) and would all look black as
        // they are: lifted, keeping the hue and the order of brightness (the brightest channel goes from
        // 0..1 to 0.35..1).
        LLColor4 swatchColor(const LLColor3& fog)
        {
            const F32 top = llmax(fog.mV[VRED], llmax(fog.mV[VGREEN], fog.mV[VBLUE]));
            if (top <= 0.001f)
            {
                return LLColor4(0.35f, 0.35f, 0.35f, 1.f);
            }
            const F32 scale = (0.35f + 0.65f * llmin(top, 1.f)) / top;
            return LLColor4(fog.mV[VRED] * scale, fog.mV[VGREEN] * scale, fog.mV[VBLUE] * scale, 1.f);
        }

        const EnvLook& envLook(const LLUUID& asset_id)
        {
            EnvLook& look = sLooks[asset_id];
            if (look.state == EnvLook::State::Unknown && asset_id.notNull() && sLookFetches < MAX_LOOK_FETCHES)
            {
                look.state = EnvLook::State::Fetching;
                ++sLookFetches;
                // The viewer's asset cache first. The answer comes on the main thread, after the row may be gone: it
                // keeps nothing of it. Marked as the swatches' (fetchSettingsAsset()): a sky put on meanwhile does not
                // take the answer away (sLookFetches would stay up and stop the swatches after MAX_LOOK_FETCHES).
                fetchSettingsAsset(asset_id, &sLooks, [](const LLUUID& id, const LLSettingsBase::ptr_t& settings, S32 status) {
                    --sLookFetches;
                    EnvLook& found = sLooks[id];
                    found.state    = EnvLook::State::Failed;
                    if (status || !settings)
                    {
                        return;
                    }
                    if (const auto sky = std::dynamic_pointer_cast<LLSettingsSky>(settings))
                    {
                        found.state   = EnvLook::State::Done;
                        found.sky     = true;
                        found.classic = sky->canAutoAdjust();
                    }
                    else if (const auto water = std::dynamic_pointer_cast<LLSettingsWater>(settings))
                    {
                        found.state = EnvLook::State::Done;
                        found.color = swatchColor(water->getWaterFogColor());
                    }
                });
            }
            return look;
        }

        // The dot of a water, when it is known (false while it is not, and for a sky).
        bool waterSwatch(const LLUUID& asset_id, LLColor4& color)
        {
            const EnvLook& look = envLook(asset_id);
            if (look.state != EnvLook::State::Done || look.sky)
            {
                return false;
            }
            color = look.color;
            return true;
        }

        // The favourite skies and waters, for the stars of every row at every draw: made again only when
        // the lists change (favoritesGeneration()) or more of the inventory has come (an old list's link is
        // known by the item it points to only once the link is loaded), not read from the settings row by row.
        const std::set<LLUUID>& favoriteSet()
        {
            static std::set<LLUUID> sItems;
            static unsigned         sMadeFor = 0; // the generation starts at 1: the first call makes it
            static S32              sItemsAt = -1;
            const unsigned          now      = favoritesGeneration();
            const S32               loaded   = gInventory.getItemCount();
            if (now != sMadeFor || loaded != sItemsAt)
            {
                sItems   = favoriteItems();
                sMadeFor = now;
                sItemsAt = loaded;
            }
            return sItems;
        }

        // A row of the sky or water list: the mark of an old sky after its name, the dot of a water, the star.
        class PhotoSkyItem : public LLFolderViewItem
        {
            friend class LLUICtrlFactory;
            explicit PhotoSkyItem(const Params& p) : LLFolderViewItem(p) {}

        public:
            // The star at the right end of the row: filled for a favourite; pressing it adds or takes it out.
            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                const LLUUID id = environmentId();
                if (id.notNull() && starRect().pointInRect(x, y))
                {
                    if (plus::photomode::isFavorite(id))
                    {
                        removeFavorite(id);
                    }
                    else
                    {
                        // told as for the row's star: the list is full, or the item is gone
                        const AddFavorite added = addFavorite(id);
                        LLFloater* floater = LLFloaterReg::findInstance(FLOATER_NAME);
                        if (added != AddFavorite::Added && floater)
                        {
                            LLNotificationsUtil::add("SystemMessageTip",
                                                     LLSD().with("MESSAGE", floater->getString(added == AddFavorite::Full ? "fav_full" : "fav_not_found")));
                        }
                    }
                    return true;
                }
                return LLFolderViewItem::handleMouseDown(x, y, mask);
            }

            void draw() override
            {
                const SkyKind kind = currentKind();
                if (kind != mShownKind)
                {
                    mShownKind          = kind;
                    mSuffixNeedsRefresh = true; // LLFolderViewItem::draw() calls refreshSuffix()
                }
                LLFolderViewItem::draw();
                const LLUUID id = environmentId();
                if (id.notNull())
                {
                    const LLRect star = starRect();
                    const LLViewerInventoryItem* item = gInventory.getItem(id);
                    LLColor4 color;
                    if (item && item->getSettingsType() == LLSettingsType::ST_WATER && waterSwatch(item->getAssetUUID(), color))
                    {
                        gl_rect_2d(star.mLeft - 14, star.mTop - 3, star.mLeft - 4, star.mBottom + 3, color, true);
                    }
                    // Looked up once, as the official rows do (llinventorylistitem.cpp): not by name every frame.
                    static const LLUIImagePtr star_on  = LLUI::getUIImage("Favorite_Star_Active");
                    static const LLUIImagePtr star_off = LLUI::getUIImage("Favorite_Star_Off");
                    // a link's row: the favourites keep the item it points to
                    const std::set<LLUUID>& starred = favoriteSet();
                    const LLUIImagePtr& image = starred.count(id) || starred.count(gInventory.getLinkedItemID(id)) ? star_on : star_off;
                    if (image)
                    {
                        image->draw(star.mLeft, star.mBottom, star.getWidth(), star.getHeight());
                    }
                }
            }

            void refresh() override
            {
                LLFolderViewItem::refresh();
                addMark();
            }

            void refreshSuffix() override
            {
                LLFolderViewItem::refreshSuffix();
                addMark();
            }

        private:
            // The inventory item of this row when it is a sky or a water.
            LLUUID environmentId() const
            {
                const auto* model = dynamic_cast<LLFolderViewModelItemInventory*>(const_cast<PhotoSkyItem*>(this)->getViewModelItem());
                if (!model)
                {
                    return LLUUID::null;
                }
                return isEnvironmentItem(model->getUUID()) ? model->getUUID() : LLUUID::null;
            }

            LLRect starRect() const
            {
                constexpr S32 SIZE = 16;
                const S32 right = getRect().getWidth() - 4;
                const S32 y     = (getRect().getHeight() - SIZE) / 2;
                return LLRect(right - SIZE, y + SIZE, right, y);
            }

            // Unknown for what is not a sky (a water, a folder), and while it is not known.
            SkyKind currentKind()
            {
                const auto* model = dynamic_cast<LLFolderViewModelItemInventory*>(getViewModelItem());
                const LLViewerInventoryItem* item = model ? gInventory.getItem(model->getUUID()) : nullptr;
                if (!item || item->getInventoryType() != LLInventoryType::IT_SETTINGS ||
                    item->getSettingsType() != LLSettingsType::ST_SKY || item->getAssetUUID().isNull())
                {
                    return SkyKind::Unknown;
                }
                const EnvLook& look = envLook(item->getAssetUUID());
                if (look.state != EnvLook::State::Done || !look.sky)
                {
                    return SkyKind::Unknown;
                }
                return look.classic ? SkyKind::Classic : SkyKind::Modern;
            }

            void addMark()
            {
                if (!mRoot->useLabelSuffix())
                {
                    return;
                }
                if (mShownKind == SkyKind::Classic)
                {
                    mLabelSuffix += utf8str_to_wstring(sMarkClassic);
                }
                else if (mShownKind == SkyKind::Modern)
                {
                    mLabelSuffix += utf8str_to_wstring(sMarkModern);
                }
                mLabelWidthDirty = true;
            }

            SkyKind mShownKind = SkyKind::Unknown;
        };

        // The sky list and the water list: the official asset-filtered inventory panel, without the
        // two things the official use of it does that this card does not want: a double-click
        // that opens the editor of an item, and the item menu. What it lists is the settings types of
        // its filter (setFilterSettingsTypes(), from the card): the skies, or the waters.
        class PhotoSkyPanel : public LLAssetFilteredInventoryPanel
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLAssetFilteredInventoryPanel::Params>
            {
                Params() {}
            };

        protected:
            PhotoSkyPanel(const Params& p) : LLAssetFilteredInventoryPanel(p) {}
            friend class LLUICtrlFactory;

        public:
            bool handleDoubleClick(S32, S32, MASK) override { return true; }
            bool handleRightMouseDown(S32, S32, MASK) override { return true; }
            bool handleDragAndDrop(S32, S32, MASK, bool, EDragAndDropType, void*, EAcceptance*, std::string&) override
            {
                return false;
            }

            LLFolderViewItem* createFolderViewItem(LLInvFVBridge* bridge) override
            {
                // As LLInventoryPanel::createFolderViewItem(), with a row that can carry the mark.
                LLFolderViewItem::Params params(mParams.item);
                params.name          = "itm";
                params.creation_date = bridge->getCreationDate();
                params.root          = mFolderRoot.get();
                params.listener      = bridge;
                params.rect          = LLRect(0, 0, 0, 0);
                params.font_color           = (bridge->isLibraryItem() ? sLibraryColor : sDefaultColor);
                params.font_highlight_color = (bridge->isLibraryItem() ? sLibraryColor : sDefaultHighlightColor);
                return LLUICtrlFactory::create<PhotoSkyItem>(params);
            }

        protected:
            // No folder without an item of the kind in it. The official filter cannot do this here: it counts a
            // folder with no views under it as empty and shows it (LLFolderViewModelItemInventory::filter,
            // "Update the latest filter generation for empty folders"), and this panel builds no view for
            // an item of another kind, so every such folder looks empty. So the folders are chosen before
            // their views are built. A folder not fetched yet is kept (it may hold some); the ones that
            // turn out empty once fetched stay shown until the card is next built.
            bool typedViewsFilter(const LLUUID& id, LLInventoryObject const* objectp) override
            {
                if (objectp && objectp->getType() == LLAssetType::AT_CATEGORY)
                {
                    return objectp->getParentUUID().isNull() || mayHoldEnvironment(id);
                }
                return LLAssetFilteredInventoryPanel::typedViewsFilter(id, objectp);
            }

        private:
            // True when the folder, or a folder below it, holds an item of the kind, or has not been
            // fetched (then it is not known). Answers are kept for the frame: a build asks about every folder.
            bool mayHoldEnvironment(const LLUUID& folder_id)
            {
                if (mCacheFrame != LLFrameTimer::getFrameCount())
                {
                    mCacheFrame = LLFrameTimer::getFrameCount();
                    mMayHold.clear();
                }
                const auto known = mMayHold.find(folder_id);
                if (known != mMayHold.end())
                {
                    return known->second;
                }

                bool                            result = true;
                const LLViewerInventoryCategory* cat    = gInventory.getCategory(folder_id);
                if (cat && cat->getVersion() != LLViewerInventoryCategory::VERSION_UNKNOWN)
                {
                    LLInventoryModel::cat_array_t*  categories = nullptr;
                    LLInventoryModel::item_array_t* items      = nullptr;
                    gInventory.getDirectDescendentsOf(folder_id, categories, items);
                    if (categories || items)
                    {
                        result = false;
                        const U64 wanted = getFilter().getFilterSettingsTypes();
                        if (items)
                        {
                            for (const LLPointer<LLViewerInventoryItem>& item : *items)
                            {
                                if (item && item->getInventoryType() == LLInventoryType::IT_SETTINGS &&
                                    ((0x1ULL << item->getSettingsType()) & wanted))
                                {
                                    result = true;
                                    break;
                                }
                            }
                        }
                        if (!result && categories)
                        {
                            for (const LLPointer<LLViewerInventoryCategory>& sub : *categories)
                            {
                                if (sub && mayHoldEnvironment(sub->getUUID()))
                                {
                                    result = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                mMayHold[folder_id] = result;
                return result;
            }

            U32                  mCacheFrame = 0;
            std::map<LLUUID, bool> mMayHold;
        };

        LLDefaultChildRegistry::Register<PhotoSkyPanel> sSkyPanelRegistration("plus_photo_sky_panel");
    }

    // --- what the frame calls ------------------------------------------------------------

    void PhotoToolsFloater::buildEnv()
    {
        // --- sky and water ---
        sMarkClassic = " " + getString("sky_mark_classic");
        sMarkModern  = " " + getString("sky_mark_modern");
        setupPicker(mSky);
        setupPicker(mWater);
        // Both back to the land's (the button at the bottom of the sky's list).
        getChild<LLButton>("btn_sky_undo")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEnvReset(); });
        // "Personal Lighting..." of the World menu: the official editor of the sky in use.
        getChild<LLButton>("btn_sky_edit")->setCommitCallback([](LLUICtrl*, const LLSD&) {
            LLFloaterReg::showInstance("env_adjust_snapshot");
        });
        // Home's button to choose a sky: the Sky & Weather tab, the sky's list open and in sight (fix-round43 3).
        getChild<LLButton>("btn_home_sky_open")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            showTab(TAB_SKY);
            setEnvListOpen(mSky, true);
            scrollToEnvSection(mSky);
        });

        // --- looks (the Light & Color tab) ---
        mLookList = getChild<LLScrollListCtrl>("list_look");
        mLookList->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            const LLScrollListItem* picked = mLookList->getFirstSelected();
            if (picked && picked->getEnabled())
            {
                onLookChosen(picked->getValue().asString());
            }
        });
        getChild<LLFilterEditor>("flt_look")->setCommitCallback([this](LLUICtrl*, const LLSD& param) {
            std::string term = param.asString();
            LLStringUtil::trim(term);
            LLStringUtil::toLower(term);
            mLookFilter = term;
            rebuildLookList(false);
        });
        getChild<LLButton>("btn_looktab_all")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setLookTab(0); });
        getChild<LLButton>("btn_looktab_fav")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setLookTab(1); });
        getChild<LLButton>("btn_looktab_recent")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setLookTab(2); });
        getChild<LLButton>("btn_look_cur_star")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLookStar(); });
        getChild<LLButton>("btn_look_prev")->setCommitCallback([this](LLUICtrl*, const LLSD&) { stepLookList(-1); });
        getChild<LLButton>("btn_look_next")->setCommitCallback([this](LLUICtrl*, const LLSD&) { stepLookList(1); });
        getChild<LLButton>("btn_look_folder")->setCommitCallback([](LLUICtrl*, const LLSD&) { plus::look::openUserFolder(); });
        getChild<LLButton>("btn_look_reload")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLookReload(); });
        setLookTab(0);

        rebuildEnvLists(true);
        refreshCurrentEnv();
    }

    void PhotoToolsFloater::openEnv()
    {
        mToneNote = -1;
    }

    void PhotoToolsFloater::drawEnv()
    {
        if (favoritesGeneration() != mFavSeen)
        {
            rebuildEnvLists(false); // a star was pressed, a favorite moved: not from inside that click
        }
        else if (mInventoryTimer.checkExpirationAndReset(INVENTORY_REFRESH_SECONDS))
        {
            mSky.ofAssetGeneration   = 0; // itemOfAsset() looks again: an item may have arrived or gone
            mWater.ofAssetGeneration = 0;
            rebuildEnvLists(false); // the inventory: a name, an item that arrived or went; a water's dot
        }
    }

    void PhotoToolsFloater::tickEnv()
    {
        refreshCurrentEnv();    // the sky or water may have been changed elsewhere
        rebuildLookList(false); // the look folders read again (here or from the menu)
        refreshLookSection();
        refreshToneNote();
    }

    // Scrolls the Sky & Weather tab so that the section of the sky (or the water) is seen, from its top when it is
    // taller than the view. The page is laid out first: a list just opened has its new height only then.
    void PhotoToolsFloater::scrollToEnvSection(const Picker& picker)
    {
        layoutShownPage();
        LLScrollContainer* scroll  = pageScroll(TAB_SKY);
        const LLPanel*     section = findChild<LLPanel>(ctl(picker, "sec_", ""));
        if (!scroll || !section)
        {
            return;
        }
        // the scroll container takes the content's new height into its scroll range: scrollToShowRect() clamps
        // to the old one first, and a list just opened under the water's would be scrolled short (fix-round44 5)
        scroll->reshape(scroll->getRect().getWidth(), scroll->getRect().getHeight());
        scroll->scrollToShowRect(section->getRect());
    }

    // --- the sky's and the water's lists: favorites, recent ---------------------

    PhotoToolsFloater::Picker* PhotoToolsFloater::pickerFor(const LLUUID& item)
    {
        EnvKind kind = EnvKind::Sky;
        if (!environmentKind(item, kind))
        {
            return nullptr;
        }
        return kind == EnvKind::Water ? &mWater : &mSky;
    }

    // The controls of one list, from postBuild().
    void PhotoToolsFloater::setupPicker(Picker& picker)
    {
        Picker* p = &picker; // the floater owns it: it lives as long as the callbacks
        picker.list = getChild<LLInventoryPanel>(ctl(picker, "", "_list"));
        picker.list->setFilterSettingsTypes(0x1ULL << (picker.kind == EnvKind::Water ? LLSettingsType::ST_WATER : LLSettingsType::ST_SKY));
        picker.list->setShowFolderState(LLInventoryFilter::SHOW_NON_EMPTY_FOLDERS);
        picker.list->setSelectCallback([this, p](const std::deque<LLFolderViewItem*>&, bool user_action) { onEnvSelection(*p, user_action); });

        getChild<LLFilterEditor>(ctl(picker, "flt_", ""))->setCommitCallback([this, p](LLUICtrl*, const LLSD& param) {
            onEnvSearch(*p, param.asString());
        });
        for (const std::string& name : { ctl(picker, "btn_", "_land"), ctl(picker, "btn_home_", "_land") })
        {
            getChild<LLButton>(name)->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { onEnvLand(*p); });
        }
        for (const std::string& name : { ctl(picker, "btn_", "_prev"), ctl(picker, "btn_home_", "_prev") })
        {
            getChild<LLButton>(name)->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { stepEnv(*p, -1); });
        }
        for (const std::string& name : { ctl(picker, "btn_", "_next"), ctl(picker, "btn_home_", "_next") })
        {
            getChild<LLButton>(name)->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { stepEnv(*p, 1); });
        }
        getChild<LLButton>(ctl(picker, "btn_", "tab_all"))->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { setEnvTab(*p, 0); });
        getChild<LLButton>(ctl(picker, "btn_", "tab_fav"))->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { setEnvTab(*p, 1); });
        getChild<LLButton>(ctl(picker, "btn_", "tab_recent"))->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { setEnvTab(*p, 2); });
        const EnvKind kind = picker.kind;
        getChild<LLButton>(ctl(picker, "btn_", "_fav_sort"))->setCommitCallback([kind](LLUICtrl*, const LLSD&) { sortFavoritesByName(kind); });
        getChild<LLButton>(ctl(picker, "btn_", "_cur_star"))->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { onCurrentStar(*p); });
        getChild<LLFlatListView>(ctl(picker, "", "_fav_list"))->setReorderCallback([](const LLSD& value, S32 new_index) {
            moveFavorite(value.asUUID(), new_index); // the list looks again by itself (the generation)
        });
        for (int i = 0; i < picker.chips; ++i)
        {
            getChild<LLButton>(ctl(picker, "", "_chip_" + std::to_string(i)))->setCommitCallback([this, kind, i](LLUICtrl*, const LLSD&) {
                const std::vector<SkyEntry> fav = favorites(kind);
                if ((size_t)i < fav.size())
                {
                    applyEntry(fav[i].item);
                }
            });
        }
        getChild<LLButton>(ctl(picker, "btn_", "_list_toggle"))->setCommitCallback([this, p](LLUICtrl*, const LLSD&) {
            setEnvListOpen(*p, !p->listOpen);
        });
        picker.openHeight = getChild<LLPanel>(ctl(picker, "sec_", ""))->getRect().getHeight();
        picker.listOpen   = gSavedSettings.getBOOL(listSetting(picker));
        showEnvList(picker); // and the first tab, "All" (setEnvTab())
    }

    void PhotoToolsFloater::setEnvTab(Picker& picker, int tab)
    {
        picker.tab = tab;
        const char* names[3] = { "tab_all", "tab_fav", "tab_recent" };
        for (int i = 0; i < 3; ++i)
        {
            getChild<LLButton>(ctl(picker, "btn_", names[i]))->setToggleState(i == tab);
        }
        const bool open = picker.listOpen; // folded: none of the list's parts
        getChild<LLView>(ctl(picker, "", "_wrap"))->setVisible(open && tab == 0);
        getChild<LLView>(ctl(picker, "flt_", ""))->setVisible(open && tab == 0);
        getChild<LLView>(ctl(picker, "", "_fav_list"))->setVisible(open && tab == 1);
        getChild<LLView>(ctl(picker, "btn_", "_fav_sort"))->setVisible(open && tab == 1);
        getChild<LLView>(ctl(picker, "", "_recent_list"))->setVisible(open && tab == 2);
    }

    // The section of the sky (or the water) as picker.listOpen has it: folded, it keeps the title, "Now: ...",
    // "Back to the land's ...", the star and the row of the arrows, and gives its height to the parts under it
    // (the page is laid out again; the section's widgets follow its bottom as the XUI has them).
    void PhotoToolsFloater::showEnvList(Picker& picker)
    {
        const bool open = picker.listOpen;
        for (const char* name : { "tab_all", "tab_fav", "tab_recent" })
        {
            getChild<LLView>(ctl(picker, "btn_", name))->setVisible(open);
        }
        setEnvTab(picker, picker.tab);

        // the title row (20), "Back to the land's ..." (22), and the arrows at the bottom (4 + 22 + 4)
        constexpr S32 FOLDED_HEIGHT = 72;
        LLPanel*      section       = getChild<LLPanel>(ctl(picker, "sec_", ""));
        const S32     height        = open ? picker.openHeight : FOLDED_HEIGHT;
        if (section->getRect().getHeight() != height)
        {
            section->reshape(section->getRect().getWidth(), height);
            requestLayout();
        }

        LLButton* toggle = getChild<LLButton>(ctl(picker, "btn_", "_list_toggle"));
        toggle->setToggleState(open);
        toggle->setLabel(getString(open ? "env_list_close" : ctl(picker, "", "_list_open")));
    }

    // Folds or opens the list of the sky (or the water), and keeps it for the next time. Opened, the rows
    // folded away are made (once) and the tree shows the one in use.
    void PhotoToolsFloater::setEnvListOpen(Picker& picker, bool open)
    {
        if (open == picker.listOpen)
        {
            return;
        }
        picker.listOpen = open;
        gSavedSettings.setBOOL(listSetting(picker), open);
        if (!open)
        {
            // Folded, the tabs are not seen: the arrows (here and on the home tab) go through the folder
            // again, not the favorites of a tab chosen before.
            picker.tab = 0;
        }
        showEnvList(picker);
        if (!open)
        {
            return;
        }
        if (!picker.listsBuilt)
        {
            rebuildPicker(picker, true);
        }
        if (picker.lastApplied.notNull() && selectedId(picker) != picker.lastApplied && gInventory.getItem(picker.lastApplied))
        {
            picker.list->setSelectionByID(picker.lastApplied, false); // opens its folders and scrolls to it
        }
    }

    // A row of the favorites or the recent list.
    LLPanel* PhotoToolsFloater::makeEnvRow(const SkyEntry& entry, LLFlatListView* list, bool favorites_list)
    {
        LLPanel* row = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>("panel_plus_photo_sky_row.xml", nullptr,
                                                                              LLPanel::child_registry_t::instance());
        if (!row)
        {
            return nullptr;
        }
        const LLUUID item = entry.item;
        row->getChild<LLUICtrl>("grip")->setVisible(favorites_list);

        // a water's dot (rebuildPicker() makes the rows again when it becomes known)
        LLPanel* swatch = row->getChild<LLPanel>("swatch");
        LLColor4 color;
        const bool dot = entry.water && waterSwatch(entry.asset, color);
        swatch->setVisible(dot);
        if (dot)
        {
            swatch->setBackgroundColor(color);
            swatch->setTransparentColor(color);
        }

        LLButton* apply = row->getChild<LLButton>("apply");
        if (entry.exists)
        {
            apply->setLabel(entry.name);
        }
        else
        {
            LLStringUtil::format_map_t args;
            args["[NAME]"] = entry.name;
            apply->setLabel(getString("fav_missing", args));
        }
        apply->setEnabled(entry.exists);
        apply->setCommitCallback([this, item](LLUICtrl*, const LLSD&) { applyEntry(item); });

        row->getChild<LLTextBox>("folder")->setText(entry.folder);

        LLButton* star = row->getChild<LLButton>("star");
        star->setToggleState(isFavorite(item));
        star->setCommitCallback([this, item](LLUICtrl* ctrl, const LLSD&) {
            if (ctrl->getValue().asBoolean())
            {
                const AddFavorite added = addFavorite(item);
                if (added != AddFavorite::Added)
                {
                    // the list is full, or the item is gone (as in onCurrentStar()): the star must not stay lit
                    ctrl->setValue(false);
                    LLNotificationsUtil::add("SystemMessageTip",
                                             LLSD().with("MESSAGE", getString(added == AddFavorite::Full ? "fav_full" : "fav_not_found")));
                }
            }
            else
            {
                removeFavorite(item);
            }
        });

        LLButton* unpin = row->getChild<LLButton>("unpin");
        unpin->setLabel(getString("fav_remove"));
        unpin->setVisible(favorites_list && !entry.exists);
        unpin->setCommitCallback([item](LLUICtrl*, const LLSD&) { removeFavorite(item); });

        list->addItem(row, LLSD(entry.item.asString()));
        return row;
    }

    void PhotoToolsFloater::rebuildEnvLists(bool force)
    {
        rebuildPicker(mSky, force);
        rebuildPicker(mWater, force);
        mFavSeen = favoritesGeneration();
    }

    // The two lists of one kind and its chips of the home tab from the data; only when something changed.
    void PhotoToolsFloater::rebuildPicker(Picker& picker, bool force)
    {
        const std::vector<SkyEntry> fav = favorites(picker.kind);
        const std::vector<SkyEntry> rec = recents(picker.kind);

        const std::set<LLUUID> starred = favoriteItems();

        std::string sig;
        for (const std::vector<SkyEntry>* entries : { &fav, &rec })
        {
            for (const SkyEntry& entry : *entries)
            {
                LLColor4 color;
                const bool dot = entry.water && waterSwatch(entry.asset, color); // starts the fetch
                sig += entry.item.asString() + (entry.exists ? "+" : "-") + entry.name + "/" + entry.folder +
                       (starred.count(entry.item) ? "*" : ".") + (dot ? "o" : "") + ";";
            }
            sig += "|";
        }
        if (!force && sig == picker.favSig)
        {
            return;
        }
        picker.favSig = sig;

        // the rows of the lists only while they can be seen (setEnvListOpen() makes them when they open)
        picker.listsBuilt = picker.listOpen;
        if (picker.listOpen)
        {
            LLFlatListView* fav_list = getChild<LLFlatListView>(ctl(picker, "", "_fav_list"));
            LLFlatListView* rec_list = getChild<LLFlatListView>(ctl(picker, "", "_recent_list"));
            fav_list->clear();
            rec_list->clear();
            fav_list->setNoItemsCommentText(getString(ctl(picker, "", "_fav_empty")));
            rec_list->setNoItemsCommentText(getString(ctl(picker, "", "_recent_empty")));
            for (const SkyEntry& entry : fav)
            {
                makeEnvRow(entry, fav_list, true);
            }
            for (const SkyEntry& entry : rec)
            {
                makeEnvRow(entry, rec_list, false);
            }
        }

        // the first favorites as buttons on the home tab
        for (int i = 0; i < picker.chips; ++i)
        {
            LLButton* chip = getChild<LLButton>(ctl(picker, "", "_chip_" + std::to_string(i)));
            const bool shown = (size_t)i < fav.size();
            chip->setVisible(shown);
            if (shown)
            {
                chip->setLabel(fav[i].name);
                chip->setEnabled(fav[i].exists);
                chip->setToolTip(fav[i].name);
            }
        }
        getChild<LLButton>(ctl(picker, "btn_", "_cur_star"))->setToggleState(isFavorite(picker.lastApplied));
    }

    // Puts a sky or water on, from a list or a chip.
    void PhotoToolsFloater::applyEntry(const LLUUID& item)
    {
        Picker* picker = isEnvironmentItem(item) ? pickerFor(item) : nullptr;
        if (!picker)
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("fav_gone")));
            return;
        }
        putOn(*picker, item);
    }

    void PhotoToolsFloater::putOn(Picker& picker, const LLUUID& item)
    {
        picker.lastApplied = item;
        picker.appliedAt   = LLTimer::getTotalSeconds();
        if (applyEnvironmentItem(item))
        {
            noteApplied(item);
        }
        refreshRows();
        refreshStatus();
    }

    // The arrows on the Favorites tab: the next favorite of the list, round.
    void PhotoToolsFloater::stepFavorite(Picker& picker, int direction)
    {
        std::vector<SkyEntry> shown;
        for (const SkyEntry& entry : favorites(picker.kind))
        {
            if (entry.exists)
            {
                shown.push_back(entry);
            }
        }
        if (shown.empty())
        {
            return;
        }
        int at = -1;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            if (shown[i].item == picker.lastApplied)
            {
                at = (int)i;
            }
        }
        const int count = (int)shown.size();
        const int next  = at < 0 ? (direction > 0 ? 0 : count - 1) : (at + direction + count) % count;
        applyEntry(shown[next].item);
    }

    // The star beside the sky or water in use.
    void PhotoToolsFloater::onCurrentStar(Picker& picker)
    {
        if (picker.lastApplied.isNull() || !isEnvironmentItem(picker.lastApplied))
        {
            // the land's, or one made with Personal Lighting and not in the inventory
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString(ctl(picker, "", "_not_in_inventory"))));
            getChild<LLButton>(ctl(picker, "btn_", "_cur_star"))->setToggleState(false);
            return;
        }
        if (isFavorite(picker.lastApplied))
        {
            removeFavorite(picker.lastApplied);
        }
        else
        {
            const AddFavorite added = addFavorite(picker.lastApplied);
            if (added != AddFavorite::Added)
            {
                LLNotificationsUtil::add("SystemMessageTip",
                                         LLSD().with("MESSAGE", getString(added == AddFavorite::Full ? "fav_full" : "fav_not_found")));
            }
        }
        rebuildPicker(picker, false);
    }

    // The item of the list's kind that has the asset: the one put on last, else one of the recent
    // ones or the favorites (null when none of them has it: put on from the inventory window).
    LLUUID PhotoToolsFloater::itemOfAsset(Picker& picker, const LLUUID& asset)
    {
        const LLViewerInventoryItem* last = picker.lastApplied.notNull() ? gInventory.getItem(picker.lastApplied) : nullptr;
        if (last && last->getAssetUUID() == asset)
        {
            return picker.lastApplied;
        }
        // Asked every refresh while the sky was put on from elsewhere: the lists are not read again
        // until they or the inventory change.
        const unsigned generation = favoritesGeneration();
        if (picker.ofAssetGeneration == generation && picker.ofAssetAsset == asset)
        {
            return picker.ofAssetItem;
        }
        LLUUID found;
        for (const std::vector<SkyEntry>& entries : { recents(picker.kind), favorites(picker.kind) })
        {
            for (const SkyEntry& entry : entries)
            {
                if (entry.exists && entry.asset == asset)
                {
                    found = entry.item;
                    break;
                }
            }
            if (found.notNull())
            {
                break;
            }
        }
        // favorites() may adopt other accounts' library favourites and write the list (a new generation)
        picker.ofAssetGeneration = favoritesGeneration();
        picker.ofAssetAsset      = asset;
        picker.ofAssetItem       = found;
        return found;
    }

    // "Now: ..." of each list (and the home tab), and the star beside it. Also finds a sky or water put
    // back to the land's elsewhere (the World menu, the official windows): then none is "put on" here.
    void PhotoToolsFloater::refreshCurrentEnv()
    {
        for (Picker* picker : { &mSky, &mWater })
        {
            LLUUID          asset;
            const EnvSource source = environmentSource(picker->kind == EnvKind::Water, asset);
            std::string     name;
            // Just put on: the asset may still be on its way, and the old one still shown.
            const LLViewerInventoryItem* last = picker->lastApplied.notNull() ? gInventory.getItem(picker->lastApplied) : nullptr;
            const bool settling = last && LLTimer::getTotalSeconds() - picker->appliedAt < SETTLE_SECONDS &&
                                  !(source == EnvSource::Asset && asset == last->getAssetUUID());
            if (settling)
            {
                name = last->getName();
            }
            else if (source == EnvSource::Land)
            {
                picker->lastApplied.setNull();
                name = getString(ctl(*picker, "", "_now_land"));
            }
            else if (source == EnvSource::Asset)
            {
                picker->lastApplied            = itemOfAsset(*picker, asset);
                const LLViewerInventoryItem* item = picker->lastApplied.notNull() ? gInventory.getItem(picker->lastApplied) : nullptr;
                name = item ? item->getName() : getString(ctl(*picker, "", "_now_item"));
            }
            else
            {
                // changed after it was put on (the sun of a sky), or made in the official editors
                const LLViewerInventoryItem* item = picker->lastApplied.notNull() ? gInventory.getItem(picker->lastApplied) : nullptr;
                name = item ? item->getName() + " " + getString("now_adjusted") : getString(ctl(*picker, "", "_now_other"));
            }
            LLStringUtil::format_map_t args;
            args["[NAME]"] = name;
            const std::string line = getString("now_line", args);
            if (line != picker->currentShown)
            {
                picker->currentShown = line;
                getChild<LLTextBox>(ctl(*picker, "", "_current"))->setText(line);
                getChild<LLTextBox>(ctl(*picker, "", "_current"))->setToolTip(line);
                getChild<LLUICtrl>(ctl(*picker, "title_home_", ""))->setToolTip(line);
            }
            getChild<LLButton>(ctl(*picker, "btn_", "_cur_star"))->setToggleState(isFavorite(picker->lastApplied));
        }
    }

    void PhotoToolsFloater::setLookTab(int tab)
    {
        mLookTab = tab;
        const char* names[3] = { "btn_looktab_all", "btn_looktab_fav", "btn_looktab_recent" };
        for (int i = 0; i < 3; ++i)
        {
            getChild<LLButton>(names[i])->setToggleState(i == tab);
        }
        rebuildLookList(true);
    }

    // What the list of the Light & Color tab shows: all, the favorites or the recent ones, narrowed by the search.
    std::vector<LookChoice> PhotoToolsFloater::shownLooks() const
    {
        const std::vector<LookChoice> base = mLookTab == 1 ? favoriteLooks() : (mLookTab == 2 ? recentLooks() : allLooks());
        if (mLookFilter.empty())
        {
            return base;
        }
        std::vector<LookChoice> shown;
        for (const LookChoice& choice : base)
        {
            std::string name = lookName(choice);
            std::string id   = choice.id;
            LLStringUtil::toLower(name);
            LLStringUtil::toLower(id);
            if (name.find(mLookFilter) != std::string::npos || id.find(mLookFilter) != std::string::npos)
            {
                shown.push_back(choice);
            }
        }
        return shown;
    }

    // Fills the list again when what it shows changed (only then: the list keeps its scroll position).
    void PhotoToolsFloater::rebuildLookList(bool force)
    {
        if (!mLookList)
        {
            return;
        }
        LookListSource source;
        source.tab        = mLookTab;
        source.filter     = mLookFilter;
        source.generation = plus::look::generation();
        source.favorites  = favoriteLookIds();
        source.recent     = recentLookIds();
        if (!force && source == mLookSource)
        {
            return;
        }
        mLookSource = std::move(source);

        const std::vector<LookChoice> shown = shownLooks();
        std::set<std::string>         stars;
        for (const LookChoice& choice : favoriteLooks())
        {
            stars.insert(choice.id);
        }
        std::string sig = std::to_string(mLookTab) + "|" + mLookFilter + "|" + std::to_string(plus::look::generation());
        for (const LookChoice& choice : shown)
        {
            sig += "|" + choice.id + (choice.exists ? "+" : "-") + (stars.count(choice.id) ? "*" : ".");
        }
        if (!force && sig == mLookSig)
        {
            return;
        }
        mLookSig = sig;

        mLookList->clearRows();
        mLookList->setCommentText(getString(!mLookFilter.empty() ? "search_none"
                                            : mLookTab == 1      ? "look_fav_empty"
                                            : mLookTab == 2      ? "look_recent_empty"
                                                                 : "look_none_found"));
        for (const LookChoice& choice : shown)
        {
            const bool star = stars.count(choice.id) > 0;
            LLSD       row;
            row["value"]   = choice.id;
            row["enabled"] = choice.exists;
            row["columns"][0]["column"] = "star";
            row["columns"][0]["type"]   = star ? "icon" : "text";
            row["columns"][0]["value"]  = star ? "Favorite_Star_Active" : "";
            row["columns"][1]["column"] = "name";
            row["columns"][1]["value"]  = lookName(choice);
            row["columns"][2]["column"] = "kind";
            row["columns"][2]["value"]  = choice.id.empty() ? std::string() : getString(choice.user ? "look_kind_user" : "look_kind_shipped");
            row["columns"][2]["font"]["name"] = "SANSSERIF_SMALL";
            mLookList->addElement(row);
        }
        refreshLookSection();
    }

    // The look in use above the list, its star, and its row chosen in the list (shown when it changed).
    void PhotoToolsFloater::refreshLookSection()
    {
        if (!mLookList)
        {
            return;
        }
        const std::string current = currentLook();
        LookChoice        in_use;
        in_use.id     = current;
        in_use.label  = plus::look::labelOf(current);
        in_use.exists = current.empty() || !in_use.label.empty();
        LLStringUtil::format_map_t args;
        args["[NAME]"]   = lookName(in_use);
        std::string text = getString("look_current", args);
        if (!current.empty() && gSavedSettings.getBOOL("PlusLookSnapshotOnly"))
        {
            text += " " + getString("look_photo_only");
        }
        setTextIfChanged(getChild<LLTextBox>("look_current"), text);

        LLButton* star = getChild<LLButton>("btn_look_cur_star");
        star->setEnabled(!current.empty());
        star->setToggleState(isFavoriteLook(current));

        const LLScrollListItem* selected = mLookList->getFirstSelected();
        if (!selected || selected->getValue().asString() != current)
        {
            if (mLookList->selectByValue(LLSD(current)))
            {
                mLookList->scrollToShowSelected();
            }
            else
            {
                mLookList->deselectAllItems(true);
            }
        }
    }

    // The arrows under the list: the next look of what the list shows (all, favorites or recent, searched), round.
    void PhotoToolsFloater::stepLookList(int direction)
    {
        onLookChosen(stepLook(shownLooks(), direction));
    }

    // The star beside the look in use.
    void PhotoToolsFloater::onLookStar()
    {
        const std::string current = currentLook();
        if (current.empty())
        {
            return;
        }
        if (isFavoriteLook(current))
        {
            removeFavoriteLook(current);
        }
        else if (!addFavoriteLook(current))
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("look_fav_full")));
        }
        rebuildLookList(false);
        refreshLookSection();
    }

    // The folders read again (a LUT put in the look folder shows up), as the menu's "Reload the Looks".
    void PhotoToolsFloater::onLookReload()
    {
        plus::look::reload();
        size_t shipped = 0, user = 0;
        for (const plus::look::Entry& entry : plus::look::entries())
        {
            ++(entry.user ? user : shipped);
        }
        LLStringUtil::format_map_t args;
        args["[SHIPPED]"] = std::to_string(shipped);
        args["[USER]"]    = std::to_string(user);
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("look_reloaded", args)));
        rebuildLookList(false);
        refreshRows();
    }

    // --- choosing a sky or a water --------------------------------------------

    LLUUID PhotoToolsFloater::selectedId(const Picker& picker) const
    {
        LLFolderViewItem* item = picker.list->getRootFolder() ? picker.list->getRootFolder()->getCurSelectedItem() : nullptr;
        auto*             model = item ? dynamic_cast<LLFolderViewModelItemInventory*>(item->getViewModelItem()) : nullptr;
        return model ? model->getUUID() : LLUUID::null;
    }

    void PhotoToolsFloater::onEnvSelection(Picker& picker, bool user_action)
    {
        // Chosen by the user, and applied by the click itself: the arrows and the search select
        // too, and do their own applying.
        if (!user_action)
        {
            return;
        }
        const LLUUID id = selectedId(picker);
        if (id.isNull() || id == picker.lastApplied || !isEnvironmentItem(id))
        {
            return;
        }
        putOn(picker, id);
    }

    void PhotoToolsFloater::onEnvSearch(Picker& picker, const std::string& text)
    {
        if (text == picker.search)
        {
            return;
        }
        picker.search = text;
        picker.list->setFilterSubString(text);
        if (!text.empty() && picker.list->getRootFolder())
        {
            // open the folders the search found something in
            picker.list->getRootFolder()->setOpenArrangeRecursively(true, LLFolderViewFolder::RECURSE_DOWN);
        }
    }

    // The next sky (or water) of the same folder as the one chosen, in the order of their names, and
    // round; only of the list's kind. With nothing chosen in the list, from the one put on last.
    void PhotoToolsFloater::stepEnv(Picker& picker, int direction)
    {
        if (picker.tab == 1)
        {
            stepFavorite(picker, direction); // the order of the favorites, not the folder's
            return;
        }
        LLUUID current = selectedId(picker);
        if (current.isNull() || !isEnvironmentItem(current))
        {
            current = picker.lastApplied;
        }
        const LLViewerInventoryItem* current_item = current.notNull() ? gInventory.getItem(current) : nullptr;
        if (!current_item || !isEnvironmentItem(current))
        {
            return;
        }

        LLInventoryModel::cat_array_t*  categories = nullptr;
        LLInventoryModel::item_array_t* items      = nullptr;
        gInventory.getDirectDescendentsOf(current_item->getParentUUID(), categories, items);
        if (!items)
        {
            return;
        }

        const LLSettingsType::type_e wanted = picker.kind == EnvKind::Water ? LLSettingsType::ST_WATER : LLSettingsType::ST_SKY;
        std::vector<const LLViewerInventoryItem*> siblings;
        for (const LLPointer<LLViewerInventoryItem>& item : *items)
        {
            if (item && item->getInventoryType() == LLInventoryType::IT_SETTINGS && item->getSettingsType() == wanted)
            {
                siblings.push_back(item.get());
            }
        }
        if (siblings.size() < 2)
        {
            return;
        }
        std::sort(siblings.begin(), siblings.end(), [](const LLViewerInventoryItem* a, const LLViewerInventoryItem* b) {
            return LLStringUtil::compareDict(a->getName(), b->getName()) < 0;
        });

        size_t at = 0;
        for (size_t i = 0; i < siblings.size(); ++i)
        {
            if (siblings[i]->getUUID() == current)
            {
                at = i;
                break;
            }
        }
        const size_t count = siblings.size();
        const size_t next  = direction < 0 ? (at + count - 1) % count : (at + 1) % count;

        const LLUUID id = siblings[next]->getUUID();
        picker.list->setSelectionByID(id, false);
        putOn(picker, id);
    }

    // "Back to the land's sky" / "... water": only that layer; the other stays as it is.
    void PhotoToolsFloater::onEnvLand(Picker& picker)
    {
        if (picker.kind == EnvKind::Water)
        {
            restoreLandWater();
        }
        else
        {
            restoreLandSky();
        }
        picker.lastApplied.setNull();
        picker.appliedAt = 0.0;
        if (picker.list->getRootFolder())
        {
            picker.list->getRootFolder()->clearSelection();
        }
        refreshRows();
        refreshStatus();
        refreshCurrentEnv();
    }

    // Both back to the land's: the personal environment goes.
    void PhotoToolsFloater::onEnvReset()
    {
        clearEnvironment();
        for (Picker* picker : { &mSky, &mWater })
        {
            picker->lastApplied.setNull();
            picker->appliedAt = 0.0;
            if (picker->list->getRootFolder())
            {
                picker->list->getRootFolder()->clearSelection();
            }
        }
        refreshRows();
        refreshStatus();
        refreshCurrentEnv();
    }

    // A block the search found (the sky list, the water list): its tab, its list's search box.
    void PhotoToolsFloater::goToPlace(const std::string& id, const std::string& tab)
    {
        getChild<LLFilterEditor>("flt_search")->clear();
        hideResults();
        const int index = tabIndex(tab);
        if (index >= 0)
        {
            showTab(index);
        }
        Picker* picker = id == "water" ? &mWater : (id == "sky" ? &mSky : nullptr);
        if (picker)
        {
            setEnvListOpen(*picker, true); // folded, the search box is not there
            setEnvTab(*picker, 0);
            scrollToEnvSection(*picker);   // the tab may be scrolled down past it (fix-round43 3)
            getChild<LLFilterEditor>(ctl(*picker, "flt_", ""))->setFocus(true);
        }
    }

    // An old ("classic") sky ignores the tone mapper's mix (LLSettingsVOSky: classic_mode = canAutoAdjust and
    // not RenderSkyAutoAdjustLegacy): say so, and offer the setting that makes it work. The check box
    // stays while the sky is an old one (so it can be turned off again); the sentence only while it is
    // not adjusted. Looked at only while the Light & Color tab is the one shown.
    void PhotoToolsFloater::refreshToneNote()
    {
        if (shownTab() != TAB_COLOR)
        {
            return;
        }
        S32 state = 0;
        if (gAgent.getRegion())
        {
            const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
            if (sky && sky->canAutoAdjust())
            {
                state = gSavedSettings.getBOOL("RenderSkyAutoAdjustLegacy") ? 2 : 1;
            }
        }
        if (state != mToneNote)
        {
            mToneNote = state;
            getChild<LLTextBox>("tone_note")->setVisible(state == 1);
            getChild<LLPanel>("sec_tone_note")->setVisible(state != 0);
            requestLayout();
        }
    }
}
