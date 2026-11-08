/**
 * @file phototoolsfloater_impl.h
 * @brief The photo tools window's class, shared by its three files (research/265 pt-ui-frame):
 *        the frame, the rows, the search and the compare (phototoolsfloater.cpp), the sky, water and look
 *        lists (phototools_env.cpp), and Home, Quality, Camera, Avatar and Shoot's blocks (phototools_blocks.cpp).
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

// Not a public header: only the three files of the window include it. Other features open the window by its
// registered name (PHOTO_TOOLS_FLOATER, phototoolsfloater.h).
//
// How the window is made (floater_plus_photo_tools.xml and panel_plus_photo_tools_<tab>.xml):
//   - on a plus_ps_box: the search and Compare on top, a row of plus_ps_tab with one page per tab (TabPages),
//     "N changed", "See Changes" and "Undo All" at the bottom; the search and the changes have a page of their
//     own (page_results) over the tabs and the pages.
//   - a page is a scroll_container whose content panel holds its parts from the top, in the order of their
//     tops in the XUI: blocks (a panel of the XUI: the lists, the camera positions ...) and columns (a panel
//     the frame fills with psui::Stack: the rows of settings of catalog.xml, made the first time the page is
//     shown). layoutPage() puts the visible parts under each other; a block that changes its height or is
//     shown or hidden calls requestLayout().
//   - the 0.25 s tick reads the rows of the page shown only (refreshRows()); the blocks have their own hooks
//     (buildEnv / openEnv / drawEnv / tickEnv and the same for the blocks), called by the frame.

#ifndef PLUS_PHOTOMODE_PHOTOTOOLSFLOATER_IMPL_H
#define PLUS_PHOTOMODE_PHOTOTOOLSFLOATER_IMPL_H

#include "photolook.h"
#include "photomode.h"
#include "photoshot.h"
#include "photosky.h"
#include "phototools.h"
#include "phototoolsfloater.h"
#include "plus/focuslock/focuslock.h"
#include "plus/photoscreen/psui/psstack.h"

#include "llfloater.h"
#include "llhandle.h"
#include "lltimer.h"

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class LLComboBox;
class LLFlatListView;
class LLInventoryPanel;
class LLLineEditor;
class LLScrollContainer;
class LLScrollListCtrl;
class LLTextBox;

namespace plus::psui
{
    class PsRow;
}

namespace plus::photomode::tools
{
    constexpr const char* FLOATER_NAME = PHOTO_TOOLS_FLOATER;

    // The tabs in the order of the row of tabs (the XUI's "tab_<tab>" over the page "page_<tab>", checked in
    // postBuild()); each page has a tip "tab_tip_<tab>". Code that means one tab takes its number from
    // tabIndex(), never a number of its own, so the order can change here and in the XUI. These are ids (also
    // the "tab" of catalog.xml), kept when the tabs were made over (2026-10-22); the names shown are
    // tabName()'s: "light" is Sky & Weather, "color" Light & Color, "subject" Avatar.
    constexpr const char* TABS[]    = { "home", "quality", "light", "color", "camera", "subject", "shoot" };
    constexpr int         TAB_COUNT = (int)(sizeof(TABS) / sizeof(TABS[0]));
    constexpr int tabIndex(std::string_view tab)
    {
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            if (tab == TABS[i])
            {
                return i;
            }
        }
        return -1;
    }
    constexpr int TAB_HOME    = tabIndex("home");
    constexpr int TAB_QUALITY = tabIndex("quality");
    constexpr int TAB_SHOOT   = tabIndex("shoot");
    constexpr int TAB_CAMERA  = tabIndex("camera");
    constexpr int TAB_COLOR   = tabIndex("color");   // Light & Color
    constexpr int TAB_SUBJECT = tabIndex("subject"); // Avatar
    constexpr int TAB_SKY     = tabIndex("light");   // Sky & Weather: the sky and water lists
    static_assert(TAB_HOME >= 0 && TAB_QUALITY >= 0 && TAB_SHOOT >= 0 && TAB_CAMERA >= 0 && TAB_COLOR >= 0 &&
                      TAB_SUBJECT >= 0 && TAB_SKY >= 0,
                  "a tab named in the code is not in TABS");
    // The page of the search and the list of changes ("page_results"), after the tabs' pages.
    constexpr int PAGE_RESULTS = TAB_COUNT;
    constexpr int PAGE_COUNT   = TAB_COUNT + 1;

    constexpr char  SETTING_SMOOTH[] = "PlusPhotoCameraSmooth";
    constexpr F32   REFRESH_SECONDS = 0.25f;
    // The favorite and recent skies and waters as the inventory has them (names, folders, gone or
    // not): looked at less often. A change of the lists themselves shows at once (favoritesGeneration()).
    constexpr F32   INVENTORY_REFRESH_SECONDS = 1.f;
    constexpr F64   SETTLE_SECONDS  = 3.0;   // a sky or water just put on is taken as the one in use for this long
    constexpr size_t MAX_RESULTS = 60;
    constexpr int    SHOT_LABEL_UNSET = -2; // the photo buttons are labelled at the next refreshShotState()

    // A text box laid out again only when its text is another (the periodic refresh writes the same mostly).
    void setTextIfChanged(LLTextBox* box, const std::string& text);

    class PhotoToolsFloater : public LLFloater
    {
        friend class LLFloaterReg;
        explicit PhotoToolsFloater(const LLSD& key);

    public:
        ~PhotoToolsFloater() override;
        bool postBuild() override;
        void onOpen(const LLSD& key) override;
        void onClose(bool app_quitting) override;
        void draw() override;
        bool handleKeyHere(KEY key, MASK mask) override;

        // =====================================================================================
        // The frame (phototoolsfloater.cpp): what the blocks use

        // Runs work on this window at the next idle: a widget cannot be removed from inside its own click.
        void later(std::function<void(PhotoToolsFloater*)> work);
        // The rows of the page shown, from the settings now (each widget written only when its value changed).
        void refreshRows();
        // "N changed" at the bottom, and the mark of the tabs with changes.
        void refreshStatus();
        // After many settings changed at once (a preset, Undo All): the blocks of the tab shown, the rows and the
        // status at once (not at the next tick). Not from inside a click of the look list (it is filled again).
        void refreshNow();
        // Back to the user's settings, from any way out of the compare (the button, an edit, closing).
        void endCompare();
        // The depth of field was switched: its rows stay off until the frames are ordinary again (draw()).
        void startDofSwitch();
        // The page of a tab (an index of TABS), shown, made and laid out at once (the search is closed first).
        void showTab(int tab);
        // The index of the tab shown (TABS); its page is behind the results while they are shown.
        int  shownTab() const;
        // A block changed its height or was shown or hidden: the page shown is laid out again before it is drawn.
        void requestLayout();
        // ... and at once (to scroll to a block of it).
        void layoutShownPage();
        LLScrollContainer* pageScroll(int page) const;
        // Back to the tabs from the search or the list of changes.
        void hideResults();

    private:
        // =====================================================================================
        // The frame: pages, columns of rows, search, changes, compare (phototoolsfloater.cpp)

        enum class Mode
        {
            Tabs,
            Search,
            Changes
        };

        // A part of a page's content: a block of the XUI or a column, placed under the one before it.
        struct Part
        {
            LLHandle<LLView> view;
            S32 left  = 0;      // its inset from the content's edges, as the XUI has it
            S32 right = 0;
        };
        struct Page
        {
            LLHandle<LLScrollContainer> scroll;  // "scroll_<tab>"
            LLHandle<LLPanel> content;  // "content_<tab>"
            std::vector<Part> parts;    // in the order of their tops in the XUI
            S32 laidWidth = -1;         // the view's size it was laid out for
            S32 laidHeight = -1;
        };

        // The columns of rows (psui::Stack): their panels in the pages' contents.
        enum ColumnId
        {
            COL_EASY,       // Home: "Adjust"
            COL_PINS,       // Home: the settings starred
            COL_QUALITY,
            COL_LIGHT,
            COL_COLOR,
            COL_CAMERA,
            COL_SUBJECT,
            COL_SHOOT,
            COL_RESULTS,    // the search's or the list of changes
            COLUMN_COUNT
        };
        // The marks before a row of an item: the star (shown on Home) and "changed".
        struct Mark
        {
            const Item*        item = nullptr;
            LLHandle<LLUICtrl> star;
            LLHandle<LLUICtrl> changed;
            int                changedShown = -1;
        };
        // A look row's drop-down (composite "look"), and what it was filled for (lookComboKey()).
        struct LookRow
        {
            LLHandle<LLUICtrl> combo;
            std::string        filled;
        };
        // A row whose label says more than its item's name, or that stays off while the depth of field switches.
        struct Special
        {
            enum class Kind
            {
                Dof,    // "Depth of Field (switching...)", off meanwhile
                Blur,   // off while the depth of field switches
                Sun     // "Light Direction (as the sky has it)" while unset
            };
            size_t row;
            Kind   kind;
            int    shown = -1;
        };
        struct Column
        {
            int                            page = -1;
            LLHandle<LLPanel>              panel;
            std::vector<psui::Stack::Row>  rows;    // the rows' commits point into it: the column does not move
            std::vector<Mark>              marks;
            std::vector<LookRow>           looks;
            std::vector<Special>           specials;
            std::vector<LLHandle<LLUICtrl>> blurNotes;  // "Turn on Depth of Field above to use this."
            int                            blurNoteShown = -1;
            S32                            builtWidth = -1;
            bool                           dirty = true;   // made again when its page is next laid out
            unsigned                       pinsShown = 0;  // mPinsGeneration its stars show
        };

        void buildFrame();
        void onPageShown(int page);
        int  pageShown() const;     // the page drawn: a tab's, or PAGE_RESULTS
        // The columns of the page shown made (when new, marked dirty or of another width) and the page laid out.
        void preparePage();
        void layoutPage(int page);
        S32  contentWidth(int page) const;
        void fitTabs();

        void buildColumn(Column& column, S32 width);
        void clearColumn(Column& column);
        psui::Stack emptyColumn(Column& column, LLPanel& panel, S32 width);
        void fillColumn(ColumnId id, psui::Stack& stack);
        void fillTab(psui::Stack& stack, const std::string& tab);
        void fillResults(psui::Stack& stack);
        void addItemRow(psui::Stack& stack, const Item& item);
        void addBlurNote(psui::Stack& stack);
        void addLookRow(psui::Stack& stack, const Item& item);
        void refreshColumn(Column& column);
        // The ids starred, read once for each mPinsGeneration.
        const std::set<std::string>& pinsNow();
        void onStar(const std::string& id, bool starred);
        void writeItem(const Item& item, float value);
        bool writeItemText(const Item& item, const std::string& text);

        std::string changedText(int count) const;   // "N changed"

        void onSearch(const std::string& text);
        void showResults(Mode mode);
        void onChanges();
        void onResetAll();

        // compare: held while the button is down, or kept by a short click (a second click ends it)
        void onCompareDown();
        void onCompareUp();
        void onCompareKey();
        void showCompareState();

        // looks (plus/look, photolook.h): the look rows of the columns (the list of Light & Color is env's)
        std::string lookName(const LookChoice& choice) const;
        std::string lookComboKey() const;
        void        fillLookCombo(LLComboBox* combo, std::string& filled);
        void        refreshLookRow(LookRow& row);
        void        onLookChosen(const std::string& id);

        psui::TabPages               mPages;
        std::array<Page, PAGE_COUNT> mPageInfo;
        std::array<Column, COLUMN_COUNT> mColumns;  // fixed places: the rows' callbacks point into them
        std::array<LLHandle<LLUICtrl>, TAB_COUNT> mTabs;
        std::array<int, TAB_COUNT>   mTabCounts {};     // the changes the tabs show (-1: none yet, from buildFrame())
        std::array<std::string, TAB_COUNT> mTabTipBase; // tab_tip_<tab>
        int                          mStatusShown = -2; // what the status line says: -1 the compare, else the count
        LLHandle<LLView>             mTabRow;           // the plus_ps_row of the tabs
        S32                          mTabsFitFor = -1;  // the row's width the tabs were fitted to
        LLHandle<LLView>             mResultsPage;
        LLHandle<LLUICtrl>           mSearch;           // the LLFilterEditor
        LLHandle<LLUICtrl>           mCompareButton;
        LLHandle<LLUICtrl>           mChangesButton;
        LLHandle<LLUICtrl>           mStatus;
        Mode                         mMode = Mode::Tabs;
        std::vector<const Item*>     mFound;            // what the results show
        std::vector<const Place*>    mFoundPlaces;
        std::map<std::string, bool>  mDetailOpen;       // "Details (N)" of each tab
        unsigned                     mPinsGeneration = 1;  // a star pressed here: the stars are read again
        std::set<std::string>        mPins;                // pinsNow()
        unsigned                     mPinsRead = 0;        // the generation mPins was read for
        Column*                      mBuilding = nullptr;  // the column being made (the decorator's)
        bool                         mRebuilding = false;  // widgets of a column are being deleted
        bool                         mRefreshSoon = false; // refreshRows() asked meanwhile: at the next draw
        bool                         mReady = false;       // drawn once: the window has its size
        bool                         mLayoutDirty = true;
        LLTimer                      mRefreshTimer;
        bool                         mDofBusy = false;     // the depth of field was switched: the pipeline is being rebuilt
        int                          mDofCalmFrames = 0;   // ... and the ordinary frames drawn since the last that stood still
        LLTimer                      mDofTimer;

        enum class Compare
        {
            Off,
            Held,   // the button is down
            Latched // a short click kept it
        };
        Compare mCompare = Compare::Off;
        bool    mCompareSwallowUp = false; // the release of the click that ended a latched compare
        LLTimer mCompareTimer;

        // =====================================================================================
        // The sky, water and look lists (phototools_env.cpp; the Sky & Weather and Light & Color blocks)

        void buildEnv();    // postBuild()
        void openEnv();     // onOpen()
        void drawEnv();     // every frame while the window is drawn
        void tickEnv();     // every REFRESH_SECONDS

        // One of the two lists of the Sky & Weather tab, the sky's or the water's. Its controls are
        // named after the prefix (panel_plus_photo_tools_light.xml, _home.xml): "<p>_list", "flt_<p>",
        // "btn_<p>tab_all", "btn_<p>_fav_sort", "<p>_fav_list", "<p>_recent_list", "btn_<p>_cur_star",
        // "<p>_current", "btn_<p>_prev", "btn_<p>_land", "btn_home_<p>_prev", "<p>_chip_N", "sec_<p>",
        // "btn_<p>_list_toggle" ...
        struct Picker
        {
            EnvKind           kind;
            std::string       prefix;
            int               chips;            // favorites shown as buttons on the home tab
            LLInventoryPanel* list = nullptr;
            LLUUID            lastApplied;      // the item put on last (null: none, or the land's again)
            std::string       search;
            int               tab = 0;          // 0 all, 1 favorites, 2 recent: what the arrows follow
            std::string       favSig;
            std::string       currentShown;
            F64               appliedAt = 0.0;  // when lastApplied was put on (its asset may still be on the way)
            // What itemOfAsset() found last, kept while the lists (favoritesGeneration()) and the asset are
            // the same; forgotten with each look at the inventory (0: none kept; the generation starts at 1).
            LLUUID            ofAssetAsset;
            LLUUID            ofAssetItem;
            unsigned          ofAssetGeneration = 0;
            // The list part (the tabs, the search, the tree, the favorites and the recent ones) can be
            // folded away: then the section is only its title, "Now: ...", the buttons and the arrows
            // (PlusPhotoSkyListOpen / PlusPhotoWaterListOpen). The rows of the favorites and the recent
            // ones are not made while it is folded (listsBuilt), only when it opens.
            bool              listOpen = false;
            bool              listsBuilt = false;
            S32               openHeight = 0;   // the section's height as the XUI has it, for when it opens
        };
        static std::string ctl(const Picker& picker, const std::string& before, const std::string& after)
        {
            return before + picker.prefix + after;
        }
        static const char* listSetting(const Picker& picker)
        {
            return picker.kind == EnvKind::Water ? "PlusPhotoWaterListOpen" : "PlusPhotoSkyListOpen";
        }
        Picker* pickerFor(const LLUUID& item);

        // the two lists: favorite and recent skies and waters
        void     setupPicker(Picker& picker);
        void     setEnvTab(Picker& picker, int tab);
        void     showEnvList(Picker& picker);
        void     setEnvListOpen(Picker& picker, bool open);
        void     rebuildEnvLists(bool force);
        void     rebuildPicker(Picker& picker, bool force);
        LLPanel* makeEnvRow(const SkyEntry& entry, LLFlatListView* list, bool favorites_list);
        void     applyEntry(const LLUUID& item);
        void     putOn(Picker& picker, const LLUUID& item);
        void     stepFavorite(Picker& picker, int direction);
        void     onCurrentStar(Picker& picker);
        void     refreshCurrentEnv();
        LLUUID   itemOfAsset(Picker& picker, const LLUUID& asset);
        void     scrollToEnvSection(const Picker& picker);

        // choosing in a list, the arrows, back to the land's
        LLUUID selectedId(const Picker& picker) const;
        void   onEnvSelection(Picker& picker, bool user_action);
        void   onEnvSearch(Picker& picker, const std::string& text);
        void   stepEnv(Picker& picker, int direction);
        void   onEnvLand(Picker& picker);
        void   onEnvReset();
        // A block the search found (the sky list, the water list): its tab, its list's search box.
        void   goToPlace(const std::string& id, const std::string& tab);

        // the note for old skies (Light & Color)
        void refreshToneNote();

        // the look list of the Light & Color tab
        void        setLookTab(int tab);
        std::vector<LookChoice> shownLooks() const;
        void        rebuildLookList(bool force);
        void        refreshLookSection();
        void        stepLookList(int direction);
        void        onLookStar();
        void        onLookReload();

        // What the look list of the Light & Color tab is made of, compared before the list itself (cheap):
        // when this is the same, so is the list.
        struct LookListSource
        {
            int                      tab = -1;
            std::string              filter;
            unsigned                 generation = 0; // of the look folders (plus::look::generation())
            std::vector<std::string> favorites;
            std::vector<std::string> recent;

            bool operator==(const LookListSource& other) const
            {
                return tab == other.tab && filter == other.filter && generation == other.generation &&
                       favorites == other.favorites && recent == other.recent;
            }
        };

        Picker            mSky{ EnvKind::Sky, "sky", 6 };
        Picker            mWater{ EnvKind::Water, "water", 4 };
        LLScrollListCtrl* mLookList = nullptr;
        int               mLookTab = 0; // 0 all, 1 favorites, 2 recent: what the list shows and its arrows follow
        std::string       mLookFilter;  // the search of the look list, lower case
        std::string       mLookSig;     // what the look list was filled for
        LookListSource    mLookSource;  // ... and what that was made of
        LLTimer           mInventoryTimer;
        unsigned          mFavSeen = 0;
        S32               mToneNote = -1; // 0 nothing, 1 sentence and check box, 2 check box only

        // =====================================================================================
        // Home, Quality, Camera, Avatar and Shoot's blocks (phototools_blocks.cpp)

        void buildBlocks(); // postBuild()
        void openBlocks();  // onOpen()
        void drawBlocks();  // every frame while the window is drawn
        void tickBlocks();  // every REFRESH_SECONDS

        // home and quality: the presets, the quality levels, the user's presets, the home's switches
        void updatePresetButtons();
        void refreshMyPresets();
        void refreshHomeControls();
        void onPreset(const std::string& id);

        // camera
        void        refreshCameraList();
        void        refreshCameraButtons();
        std::string autoName() const;
        void        onCameraSave();
        void        onCameraGo();
        void        onCameraRename();
        void        onCameraDelete();
        S32         selectedCamera() const;
        void        toggleSection(const char* body, const char* header, const char* label_string);

        // shutter, gaze, focus
        void        onShoot();
        void        refreshShotMenus();
        void        refreshSaveDir(); // the folder line of the Shoot tab, written when it changed
        void        refreshShotState();
        void        showShotResult();
        std::string whyNot(const ShotDims& dims, const ShotMemory& memory);
        void        refreshGaze();
        void        refreshFocus(); // the focus lock (plus/focuslock): the Avatar tab's Focus section

        LLScrollListCtrl* mCameraList = nullptr;
        LLLineEditor*     mCameraName = nullptr;
        std::string       mRegionShown;
        bool              mLocationHidden = false; // namedViewsHidden() (RLVa @showloc) when the camera list was made
        S32               mShownGraphicLevel = -1; // the quality level the buttons show (-1: none yet)
        bool              mEveryoneShown = false;
        std::vector<plus::focuslock::Nearby> mFocusNearby; // the people in the Focus section's list, reused
        std::string       mFocusPeopleKey;                 // what that list was filled for
        unsigned          mShotSeen = 0;
        std::string       mShotMenuKey;
        std::string       mSaveDirShown;   // the folder line as written (with its 'default' note)
        // what the photo buttons say: made again only when the state or the seconds of the wait change
        ShotState         mShotStateShown = ShotState::Idle;
        int               mShotSecondsShown = SHOT_LABEL_UNSET;
        bool              mFrozenShown = false;
        S32               mFrozenSecondsShown = -1;
    };
}

#endif // PLUS_PHOTOMODE_PHOTOTOOLSFLOATER_IMPL_H
