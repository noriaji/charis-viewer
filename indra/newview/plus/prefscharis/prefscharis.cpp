/**
 * @file prefscharis.cpp
 * @brief Preferences > Charis (research/263): the class of panel_preferences_charis.xml.
 *
 * The tab holds the Charis settings that were in the Charis menu. What a setting drives is a control_name
 * there, and what a registered function does is a commit_callback there: the same functions the menu called,
 * nothing copied. Only reading a function's state needs code: the menu asked its on_check / on_enable each
 * time it opened, and a check box or a drop-down has no such hook. This panel asks those same functions
 * when the floater opens, right after a click, and every half second while the tab is drawn (the controls of
 * the sub-tab shown only), and writes a control only when its value changed.
 *
 * Sub-tabs (prefs-charis-tabs, research/266 prefs-frame): a plus_ps_tabs on top (charis_tabs, the official tab
 * container drawn in the psui style: the preferences' search opens the sub-tab of a match) with one page per tab
 * (PAGES). Every page is built with the panel, so Cancel and the saved values cover the hidden pages alike; this
 * class only picks the remembered page when built and remembers the one the user picks in PlusPrefsCharisTab.
 *
 * Cancel (and the floater's X, and the advanced graphics floater's X) puts the tab's controls back as
 * LLPanelPreference does, but for the switches that take effect when clicked (IMMEDIATE: the MCP server must
 * never come back on behind a closed window), and only while Preferences is shown (values remembered at an
 * earlier opening are stale). A quality level applied while Preferences is shown (the recommendation, the
 * comparison, the photo tools) is remembered again as the official preset loading does, so that Cancel does
 * not put back half of it. The Charis menu's "Charis Settings..." opens this tab (Plus.PrefsCharis.Show).
 *
 * The detailed graphics settings (research/267 gfxadv-ui): the official floater prefs_graphics_advanced (its class
 * LLFloaterPreferenceGraphicsAdvanced, untouched) is registered again with Charis's file in the psui look,
 * floater_plus_prefs_graphics_advanced.xml. LLFloaterReg::add() replaces the build entry of a name, and the features'
 * init() runs after the official registrations (plus::registerFloaters()); the official file is left as it is, to
 * compare with. The floater is still built while Preferences is (LLPanelPreferenceGraphics::postBuild()).
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

#include "plus.h"
#include "plus/photoscreen/psui/pstabs.h"
#include "plus/presets/presets.h"

#include "llcallbacklist.h"
#include "llcombobox.h"
#include "llfloaterpreference.h"
#include "llfloaterpreferencesgraphicsadvanced.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llscrolllistitem.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

#include <iterator>
#include <string>
#include <vector>

namespace plus::prefscharis
{
    namespace
    {
        constexpr char PANEL_CLASS[]   = "plus_panel_preference_charis"; // floater_preferences.xml, the Charis tab
        constexpr char FLOATER_NAME[]  = "preferences";
        constexpr char TAB_NAME[]      = "charis";
        constexpr F32  REFRESH_SECONDS = 0.5f;
        constexpr char SETTING_TAB[]   = "PlusPrefsCharisTab";              // the sub-tab picked last (PAGES' name)
        constexpr char TABS_NAME[]     = "charis_tabs";                     // the plus_ps_tabs of the sub-tabs

        // The detailed graphics settings: the official name (llviewerfloaterreg.cpp) and Charis's file.
        constexpr char ADVANCED_FLOATER_NAME[] = "prefs_graphics_advanced";
        constexpr char ADVANCED_FLOATER_FILE[] = "floater_plus_prefs_graphics_advanced.xml";

        // The sub-tabs' pages (panel_preferences_charis.xml) and the value of SETTING_TAB for each.
        struct Page
        {
            const char* name;   // the value of SETTING_TAB
            const char* page;   // the page's panel in charis_tabs
        };

        constexpr Page PAGES[] = {
            { "display",  "page_display" },
            { "graphics", "page_graphics" },
            { "controls", "page_controls" },
            { "ai",       "page_ai" },
            { "rlva",     "page_rlva" },
            { "other",    "page_other" },
        };

        // Settings of the tab that take effect when clicked, as in the menu before: not put back by Cancel / X.
        // The FPS display is turned on by its position too, which is not a control (it would stay alone).
        constexpr const char* IMMEDIATE[] = { "PlusPoserMcpEnabled", "PlusPoserMcpAllowUiSnapshot", "PlusFpsCornerOn" };

        // A control whose state comes from functions the features registered in the default registrar
        // (EnableCallbackRegistry). VALUE: a check box (is() says on) or a drop-down with integer values
        // (is(value) says that value is the current one). VISIBLE: shown while is() holds.
        enum class Use { VALUE, VISIBLE };

        struct Bound
        {
            const char* child;
            const char* is;
            const char* enabled;    // nullptr: always enabled
            Use         use;
        };

        constexpr Bound BOUND[] = {
            { "c_ao_enabled",       "Plus.AO.IsEnabled",          nullptr,                             Use::VALUE },   // plus/ao
            { "c_ao_stands",        "Plus.AO.IsStands",           nullptr,                             Use::VALUE },   // plus/ao
            { "c_safe_mode_next",   "Plus.SafeMode.NextStart.Is", "Plus.SafeMode.NextStart.Choosable", Use::VALUE },   // plus/safemode
            { "cb_fps_position",    "Plus.FpsCorner.IsPosition",  nullptr,                             Use::VALUE },   // plus/fpscorner
            { "cb_dgc",             "Plus.GpuDrivenDGC.Is",       "Plus.GpuDrivenDGC.Choosable",       Use::VALUE },   // plus/gpudgc
            { "note_comparison_on", "Plus.Presets.IsComparison",  nullptr,                             Use::VISIBLE }, // plus/presets
        };

        using Function = LLUICtrl::enable_callback_t;

        // The registered function, looked up once (the registry keeps it where it is); null when none is named
        // or it is not registered.
        const Function* lookUp(const char* name)
        {
            if (!name)
            {
                return nullptr;
            }
            const Function* fn = LLUICtrl::EnableCallbackRegistry::getValue(name);
            if (!fn || !*fn)
            {
                LL_WARNS("Plus") << "Preferences > Charis: " << name << " is not registered" << LL_ENDL;
                return nullptr;
            }
            return fn;
        }

        class CharisPrefsPanel : public LLPanelPreference
        {
        public:
            bool postBuild() override
            {
                LLPanelPreference::postBuild();
                plus::psui::PsTabs* tabs = findChild<plus::psui::PsTabs>(TABS_NAME);
                if (tabs)
                {
                    mTabs = tabs->getTabsHandle();
                }
                else
                {
                    LL_WARNS("Plus") << "Preferences > Charis: no " << TABS_NAME << LL_ENDL;
                }
                for (const Bound& bound : BOUND)
                {
                    LLUICtrl* ctrl = findChild<LLUICtrl>(bound.child);
                    if (!ctrl)
                    {
                        LL_WARNS("Plus") << "Preferences > Charis: no " << bound.child << LL_ENDL;
                        continue;
                    }
                    Item item;
                    item.use = bound.use;
                    item.ctrl = ctrl->getHandle();
                    item.page = tabs ? tabs->pageOf(ctrl) : -1;
                    item.is = lookUp(bound.is);
                    item.enabled = lookUp(bound.enabled);
                    if (LLComboBox* combo = dynamic_cast<LLComboBox*>(ctrl))
                    {
                        for (const LLScrollListItem* row : combo->getAllData())
                        {
                            item.values.push_back(row->getValue());
                        }
                    }
                    if (bound.use == Use::VALUE)
                    {
                        // After the XML's commit_callback (connected first): the state as the function sees it,
                        // e.g. the AO switch before login, which does nothing. The click changed the control
                        // itself, so what was shown is not known any more.
                        ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&)
                                                {
                                                    forgetShown();
                                                    readStates();
                                                });
                    }
                    mItems.push_back(std::move(item));
                }
                if (tabs)
                {
                    // The page remembered; a page shown later (by the user, the search or code) is read at once.
                    tabs->selectTabByName(pageNamed(gSavedSettings.getString(SETTING_TAB)));
                    tabs->setShownCallback([this](S32) { readStates(); });
                    tabs->setPickedCallback([this](S32 index) { remember(index); });
                }
                readStates();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLPanelPreference::onOpen(key);
                readStates();
            }

            // Drawn only while the tab is shown; the first draw after it was hidden reads at once.
            void draw() override
            {
                if (mRefreshTimer.getElapsedTimeF32() > REFRESH_SECONDS)
                {
                    readStates();
                }
                LLPanelPreference::draw();
            }

            void cancel(const std::vector<std::string> settings_to_skip = {}) override
            {
                // Preferences is not shown (the advanced graphics floater's X, opened from the performance
                // floater): what was remembered at an earlier opening is stale, nothing is put back.
                if (!LLFloaterReg::instanceVisible(FLOATER_NAME))
                {
                    return;
                }
                std::vector<std::string> skip(settings_to_skip);
                skip.insert(skip.end(), std::begin(IMMEDIATE), std::end(IMMEDIATE));
                LLPanelPreference::cancel(skip);
            }

        private:
            static constexpr S32 UNKNOWN = -2;

            struct Item
            {
                Use                use = Use::VALUE;
                LLHandle<LLUICtrl> ctrl;
                const Function*    is = nullptr;
                const Function*    enabled = nullptr;
                std::vector<LLSD>  values;              // a drop-down's values (empty: one state, on or off)
                S32                page = -1;           // the index in charis_tabs of the page it is on (-1: none)
                S32                shown = UNKNOWN;     // what the control shows: 0 / 1, or the drop-down's index
                S32                enabledShown = UNKNOWN;
            };

            // The page's panel of a SETTING_TAB value (an unknown one: the first page).
            static const char* pageNamed(const std::string& name)
            {
                for (const Page& page : PAGES)
                {
                    if (name == page.name)
                    {
                        return page.page;
                    }
                }
                return PAGES[0].page;
            }

            // The user picked the sub-tab of index: remembered for the next opening.
            void remember(S32 index)
            {
                const plus::psui::PsTabs* tabs = mTabs.get();
                const LLPanel* page = tabs ? tabs->getPanelByIndex(index) : nullptr;
                if (!page)
                {
                    return;
                }
                for (const Page& known : PAGES)
                {
                    if (page->getName() == known.page)
                    {
                        if (gSavedSettings.getString(SETTING_TAB) != known.name)
                        {
                            gSavedSettings.setString(SETTING_TAB, known.name);
                        }
                        return;
                    }
                }
            }

            // The index of the sub-tab shown (-1 without the sub-tabs).
            S32 shownPage() const
            {
                const plus::psui::PsTabs* tabs = mTabs.get();
                return tabs ? tabs->getCurrentPanelIndex() : -1;
            }

            static bool holds(const Function* fn, LLUICtrl* ctrl, const LLSD& param)
            {
                return fn && (*fn)(ctrl, param);
            }

            void forgetShown()
            {
                for (Item& item : mItems)
                {
                    item.shown = UNKNOWN;
                    item.enabledShown = UNKNOWN;
                }
            }

            // The state of each control on the page shown, as its functions say; the control is written only when
            // that changed. A hidden page's controls are read when it is shown (the sub-tabs' shown callback).
            void readStates()
            {
                mRefreshTimer.reset();
                const S32 shown = shownPage();
                for (Item& item : mItems)
                {
                    LLUICtrl* ctrl = item.ctrl.get();
                    if (!ctrl || item.page != shown)
                    {
                        continue;
                    }
                    if (item.enabled)
                    {
                        const S32 enabled = holds(item.enabled, ctrl, LLSD()) ? 1 : 0;
                        if (enabled != item.enabledShown)
                        {
                            item.enabledShown = enabled;
                            ctrl->setEnabled(enabled != 0);
                        }
                    }
                    S32 state = -1; // a drop-down whose functions name none of its values: left as it is
                    if (item.values.empty())
                    {
                        state = holds(item.is, ctrl, LLSD()) ? 1 : 0;
                    }
                    else
                    {
                        for (S32 i = 0; i < (S32)item.values.size(); ++i)
                        {
                            if (holds(item.is, ctrl, item.values[i]))
                            {
                                state = i;
                                break;
                            }
                        }
                    }
                    if (state < 0 || state == item.shown)
                    {
                        continue;
                    }
                    item.shown = state;
                    if (item.use == Use::VISIBLE)
                    {
                        ctrl->setVisible(state != 0);
                    }
                    else
                    {
                        ctrl->setValue(item.values.empty() ? LLSD(state != 0) : item.values[state]);
                    }
                }
            }

            std::vector<Item> mItems;
            LLFrameTimer      mRefreshTimer;
            LLHandle<plus::psui::PsTabs> mTabs;     // charis_tabs
        };

        LLPanelInjector<CharisPrefsPanel> sInjector(PANEL_CLASS);

        // A level applied while Preferences is shown: its values are what Cancel puts back from now on, as
        // after loading a graphics preset (LLPanelPreferenceGraphics::onPresetsListChange). At the next idle:
        // the recommendation's PlusSnapshotBoost and the comparison's draw distance and the rest are written
        // right after the level, in the same call.
        void rememberSettingsAgain()
        {
            if (!LLFloaterReg::instanceVisible(FLOATER_NAME))
            {
                return;
            }
            if (LLFloaterPreference* prefs = LLFloaterReg::findTypedInstance<LLFloaterPreference>(FLOATER_NAME))
            {
                prefs->saveSettings();
            }
        }

        // The Charis menu's "Charis Settings..." (menu_plus.xml, PrefsCharis).
        void showTab()
        {
            if (LLFloaterPreference* prefs = dynamic_cast<LLFloaterPreference*>(LLFloaterReg::showInstance(FLOATER_NAME)))
            {
                prefs->selectPanel(TAB_NAME);
            }
        }

        // The detailed graphics settings from Charis's file (the file comment above).
        void registerAdvancedGraphics()
        {
            LLFloaterReg::add(ADVANCED_FLOATER_NAME, ADVANCED_FLOATER_FILE,
                              (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreferenceGraphicsAdvanced>);
            LL_INFOS("Plus") << ADVANCED_FLOATER_NAME << ": " << ADVANCED_FLOATER_FILE << LL_ENDL;
        }

        void init()
        {
            registerAdvancedGraphics();
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PrefsCharis.Show",
                                                                     [](LLUICtrl*, const LLSD&) { showTab(); });
            plus::presets::addGraphicsLevelListener([](U32) { doOnIdleOneTime(&rememberSettingsAgain); });
        }

        plus::FeatureRegistration sRegistration({ "prefscharis", &init });
    }
}
