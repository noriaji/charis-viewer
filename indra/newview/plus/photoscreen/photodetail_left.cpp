/**
 * @file photodetail_left.cpp
 * @brief The left of the detailed photo screen: the rail of eight tabs and the tab's column (research/221,
 *        ps-left). Quality (ps-quality: the photo tools' Quality tab, first), then Scene, Avatar, Environment,
 *        Lighting, Pose & Animation, Camera and Effects, as the target picture shows them; each tab's column is
 *        headings that open and close over the rows of the photo tools' items and of the photo screen's own (the
 *        quality level, the place, the time of day, the sky and water, the focus).
 *        The picture's eighth tab, Presets, was taken out (ps-sky-water): the strip under the world has the
 *        moods, the user's presets and the camera positions.
 *        Under the eight, kept at the rail's bottom out of its scrolling: Conversations (ps-im, research/247), the
 *        IM and group chats with the unread count on the tab; its panel (photoconv_panel.cpp) takes the body's
 *        place while it is chosen, and the same tab again goes back to the tab before.
 *
 * Where the six tabs of before went: Focus -> Camera (its first section), Camera -> Camera, Light & Color ->
 * Lighting and Effects (the brightness and colour rows are the right panel's), Sky & Water -> Scene (the sky
 * and water cards and their list: photodetail_skywater.cpp), Avatar -> Avatar, Save -> the right panel's Output.
 *
 * Only what works is shown (research/221 (3)): the rows of the picture without a function behind them yet
 * (other avatars and attachments hidden, head tracking, the animation list, a pose applied from the list) are
 * left out until the branches that make them.
 *
 * Speed: the column is made when a tab, a heading or the panel's width changes. The tick (4 Hz, while the
 * panels are shown) reads the rows' values and the few live parts (the parcel, the sky and water, the avatar's
 * complexity, the time of day, the focus lock), each written only when it changed.
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

#include "photodetail_left.h"

#include "photoconv.h"
#include "photoconv_panel.h"
#include "photocamctl.h"
#include "photodetail_layout.h"
#include "photodetail_rows.h"
#include "photodetail_skywater.h"
#include "photodetail_state.h"
#include "photoscreen_items.h"
#include "plus/flycammouse/flycammouse.h"
#include "plus/freeze/freeze.h"
#include "plus/photomode/photomode.h"
#include "plus/presets/presets.h"
#include "plus/photoscreen/psui/psstack.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/photoscreen/psui/pswidgets.h"

#include <algorithm>
#include <map>

#include "llagent.h"
#include "llapp.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfloaterreg.h"
#include "llparcel.h"
#include "llresmgr.h"
#include "llscrollcontainer.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "pipeline.h"
#include "plus/rlv/rlvhooks.h"

namespace plus::photoscreen::detail
{
    namespace
    {
        using psui::Stack;
        using Row = psui::Stack::Row;
        using Heading = psui::Stack::Heading;
        using namespace psui::stack;     // the column's sizes

        // ---- the rail ---------------------------------------------------------------------------

        enum class LeftTab
        {
            Quality,
            Scene,
            Avatar,
            Environment,
            Lighting,
            Pose,
            Camera,
            Effects,
            Count
        };
        constexpr int LEFT_TABS = static_cast<int>(LeftTab::Count);
        const char* const LEFT_TAB_IDS[LEFT_TABS] = { "quality", "scene", "avatar", "environment", "lighting", "pose",
                                                      "camera", "effects" };
        constexpr S32 QUALITY_LEVELS = 7;   // the official slider's levels, 0 Low .. 6 Ultra (plus/presets)

        constexpr S32 RAIL_TOP = 4;     // over the first tab (the picture: the chosen tab almost at the top)
        constexpr S32 RAIL_BOTTOM = 4;  // under the conversations tab
        constexpr S32 BODY_INSET_TOP    = 4;    // the scrolled column inside the panel's rounded corners
        constexpr S32 BODY_INSET_BOTTOM = 8;
        constexpr char SETTING_AO[] = "PlusAOEnabled";  // per account

        // A tab of the left rail that also commits when it is chosen already: the folded panel (a narrow
        // window) closes with the same tab again. Otherwise the rail tab of psui.
        class LeftRailTab : public psui::PsRailTab
        {
        public:
            struct Params : public LLInitParam::Block<Params, psui::PsRailTab::Params> { Params() {} };

        protected:
            friend class LLUICtrlFactory;
            explicit LeftRailTab(const Params& p) : psui::PsRailTab(p) {}

            void onClick(S32 x, S32 y) override
            {
                if (getValue().asBoolean())
                {
                    commitChange();
                }
                else
                {
                    psui::PsRailTab::onClick(x, y);
                }
            }
        };
        LLDefaultChildRegistry::Register<LeftRailTab> sLeftRailTabRegistration("plus_ps_left_rail_tab");

        // ---- what the column shows --------------------------------------------------------------

        struct LeftState
        {
            LeftTab tab      = LeftTab::Scene;
            LeftTab builtTab = LeftTab::Count;
            bool    conv     = false;   // the conversations tab is chosen: its panel is shown instead of the column
            LLHandle<LLPanel> convPanel;
            boost::signals2::scoped_connection convChanged;     // the tab's badge
            S32     builtWidth = -1;
            std::vector<Row> rows;
            std::map<std::string, bool> open;   // "<tab>/<section>" -> open (only for this session)

            // the live parts of the column shown (null when another tab is shown; the sky and water are
            // photodetail_skywater.cpp's)
            LLHandle<LLView> location, avatar, time, level;
            // what they show
            bool        locationSet = false;
            std::string parcelName, regionName;
            bool        avatarNamed = false;
            LLUUID      snapshot;
            U32         complexity = ~0u;
            int         timeShown = -2;
            int         levelShown = -2;    // the quality level the drop-down shows (-1: none, the comparison)

            S32  lastShadow = 0;            // the shadows' level before "show shadows" was cleared
        };
        LeftState sL;

        // Particles hidden here come back when the photo screen is left (as the HUD does by its scope).
        bool sParticlesHidden = false;
        bool sParticlesWatch  = false;

        bool watchParticles()
        {
            if (!sParticlesHidden || LLApp::isExiting())
            {
                sParticlesWatch = false;
                return true;
            }
            if (plus::photoscreen::isActive())
            {
                return false;
            }
            sParticlesHidden = false;
            sParticlesWatch  = false;
            if (!LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_PARTICLES))
            {
                LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_PARTICLES);
            }
            return true;
        }

        void showParticles(bool show)
        {
            if (LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_PARTICLES) == show)
            {
                return;
            }
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_PARTICLES);
            sParticlesHidden = !show;
            if (!show && !sParticlesWatch)
            {
                sParticlesWatch = true;
                doOnIdleRepeating(&watchParticles);
            }
        }

        // ---- small helpers ----------------------------------------------------------------------

        // A word of the left panel's XML (en / ja).
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

        bool isOpen(const std::string& key, bool by_default)
        {
            const auto it = sL.open.find(key);
            return it == sL.open.end() ? by_default : it->second;
        }

        // A heading of the tab shown; true when its rows are to be added.
        bool section(Stack& stack, const char* id, const std::string& label, const char* icon, bool by_default = true)
        {
            const std::string key = std::string(LEFT_TAB_IDS[static_cast<int>(sL.tab)]) + "/" + id;
            const bool open = isOpen(key, by_default);
            Heading heading;
            heading.id    = key;
            heading.label = label;
            heading.icon  = icon;
            return stack.header(heading, open, [key, open]() {
                sL.open[key] = !open;
                later([]() { fillLeft(); });    // not from the heading's own click: it is deleted by the refill
            });
        }

        // ---- the quality tab: the photo tools' Quality tab (ps-quality) ----------------------------
        // The same settings as the photo tools' tab (catalog.xml, tab "quality") and the same level command
        // (plus::presets::requestGraphicsLevel, as "Plus.Presets.SetLevel"): both windows change one thing.
        // The main rows under the level; the photo tools' "Details" in four sections, closed at first.

        enum QualityGroup
        {
            QgMain,
            QgAA,
            QgShading,
            QgReflect,
            QgDetail
        };

        bool startsWith(const std::string& text, const char* prefix)
        {
            return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
        }

        // The rows of a section that places the photo tools' items by hand (photoscreen_items.h), in the table's
        // order; an id the catalog does not have is left out (Stack::itemById).
        void addPlaced(Stack& stack, items::Spot spot)
        {
            items::forEachRow(spot, [&stack](const items::Placement& placement) { stack.itemById(placement.id); });
        }

        // Which section a row of the Quality tab goes in (a detail the catalog adds later goes in the last).
        QualityGroup qualityGroup(const pm::Item& item)
        {
            if (!item.detail)
            {
                return QgMain;
            }
            const std::string& id = item.id;
            if (startsWith(id, "RenderFSAA") || startsWith(id, "PlusTAA"))
            {
                return QgAA;
            }
            if (startsWith(id, "PlusGtao") || startsWith(id, "RenderDeferredSSAO") || startsWith(id, "RenderGlow"))
            {
                return QgShading;
            }
            if (startsWith(id, "RenderReflection") || startsWith(id, "RenderMirrors") || startsWith(id, "PlusMirror") ||
                startsWith(id, "RenderScreenSpaceReflections") || startsWith(id, "PlusSSR") ||
                startsWith(id, "RenderTransparentWater"))
            {
                return QgReflect;
            }
            return QgDetail;
        }

        void addQualityRows(Stack& stack, QualityGroup group)
        {
            for (const pm::Item& item : pm::items())
            {
                if (item.tab == "quality" && qualityGroup(item) == group)
                {
                    stack.item(item);
                }
            }
        }

        void fillQuality(Stack& stack)
        {
            if (section(stack, "quality", ls("SecQuality"), "quality"))
            {
                if (psui::PsCombo* level = stack.plainCombo(ls("QualityLevel"), ls("QualityLevelTip")))
                {
                    for (S32 i = 0; i < QUALITY_LEVELS; ++i)
                    {
                        level->add(ls("QualityLevel" + std::to_string(i)), LLSD(i));
                    }
                    level->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                        const LLSD value = ctrl->getValue();
                        if (value.isInteger())
                        {
                            plus::presets::requestGraphicsLevel((U32)llclamp(value.asInteger(), 0, QUALITY_LEVELS - 1));
                        }
                    });
                    sL.level = level->getHandle();
                }
                addQualityRows(stack, QgMain);
                stack.endSection();
            }
            struct Sec
            {
                QualityGroup group;
                const char*  id;
                const char*  label;
                const char*  icon;
            };
            constexpr Sec SECTIONS[] = {
                { QgAA,      "aa",      "SecQualityAA",      "resolution" },
                { QgShading, "shading", "SecQualityShading", "sun" },
                { QgReflect, "reflect", "SecQualityReflect", "water" },
                { QgDetail,  "detail",  "SecQualityDetail",  "sliders" },
            };
            for (const Sec& sec : SECTIONS)
            {
                if (section(stack, sec.id, ls(sec.label), sec.icon, false))
                {
                    addQualityRows(stack, sec.group);
                    stack.endSection();
                }
            }
        }

        // The level in use (none shown in the comparison with the official viewer, as the Charis menu and the
        // photo tools). Written only when it changed: a level just chosen stays shown while its notice is up.
        void refreshQuality()
        {
            LLComboBox* combo = live<LLComboBox>(sL.level);
            if (!combo)
            {
                return;
            }
            const int level = plus::presets::comparisonInUse() ? -1 : (int)plus::presets::graphicsLevel();
            if (level == sL.levelShown)
            {
                return;
            }
            sL.levelShown = level;
            if (level < 0 || level >= QUALITY_LEVELS)
            {
                combo->clear();
            }
            else
            {
                setComboIfChanged(combo, LLSD(level));
            }
        }

        // ---- the scene tab ----------------------------------------------------------------------

        const LLUUID& knownSky(int index)
        {
            switch (index)
            {
                case 0:  return LLEnvironment::KNOWN_SKY_SUNRISE;
                case 1:  return LLEnvironment::KNOWN_SKY_MIDDAY;
                case 2:  return LLEnvironment::KNOWN_SKY_SUNSET;
                default: return LLEnvironment::KNOWN_SKY_MIDNIGHT;
            }
        }

        // Which of the four times of World > Environment the personal sky is (-1: none of them).
        int timeOfDay()
        {
            LLSettingsSky::ptr_t sky = LLEnvironment::instance().getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
            if (!sky)
            {
                return -1;
            }
            const LLUUID id = sky->getAssetId();
            for (int i = 0; i < 4; ++i)
            {
                if (id == knownSky(i))
                {
                    return i;
                }
            }
            return -1;
        }

        void setTimeOfDay(int index)
        {
            // as the official World > Environment > Sunrise ... (llviewermenu.cpp LLWorldEnvSettings); the last sky
            // chosen wins (a sky of the list still coming is dropped, and this one is if another comes after it)
            pm::setLocalEnvironmentAsset(knownSky(llclamp(index, 0, 3)), pm::EnvLayer::Sky, pm::EnvTransition::Instant);
            LLEnvironment::instance().setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
            pm::forgetSun();
        }

        void openAboutLand()
        {
            LLViewerParcelMgr::getInstance()->selectParcelAt(gAgent.getPositionGlobal());
            LLFloaterReg::showInstance("about_land");
        }

        void fillScene(Stack& stack)
        {
            if (section(stack, "scene", ls("SecScene"), "clapperboard"))
            {
                stack.label(ls("Location"));
                if (psui::PsCard* card = stack.card("thumb", "scene", std::string(), std::string(), ls("LocationTip"), &openAboutLand))
                {
                    sL.location = card->getHandle();
                }

                stack.label(ls("TimeOfDay"), ls("TimeTip"));
                stack.picture("dusk_night", PICTURE_H);
                if (psui::PsSlider* time = stack.slider(std::string(), ls("TimeTip"), 0.f, 3.f, 1.f, 0, 1.f,
                                                        []() { return (float)llmax(0, timeOfDay()); },
                                                        [](float v) { setTimeOfDay(ll_round(v)); }, 4))
                {
                    sL.time = time->getHandle();
                }

                skywater::fill(stack);
                stack.endSection();
            }

            if (section(stack, "display", ls("SecDisplay"), "sliders"))
            {
                stack.check(ls("ShowHud"), ls("ShowHudTip"),
                            []() { return LLPipeline::sShowHUDAttachments ? 1.f : 0.f; },
                            [](float v) {
                                // RLVa: a locked HUD cannot be hidden (research/248 W10, 3.4)
                                if (v <= 0.5f && plus::rlv::enabled() && plus::rlv::hasLockedHUD())
                                    return;
                                LLPipeline::sShowHUDAttachments = v > 0.5f;
                            });
                stack.check(ls("ShowParticles"), ls("ShowParticlesTip"),
                            []() { return LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_PARTICLES) ? 1.f : 0.f; },
                            [](float v) { showParticles(v > 0.5f); });
                if (const pm::Item* shadows = pm::findItem(items::ITEM_SHADOWS))
                {
                    stack.check(ls("ShowShadows"), shadows->note,
                                [shadows]() { return pm::itemValue(*shadows) > 0.5f ? 1.f : 0.f; },
                                [shadows](float v) {
                                    if (v > 0.5f)
                                    {
                                        float on = 1.f;
                                        for (const pm::Option& option : shadows->options)
                                        {
                                            on = llmax(on, option.value);   // the most shadows
                                        }
                                        if (sL.lastShadow > 0)
                                        {
                                            on = (float)sL.lastShadow;
                                        }
                                        pm::setItemValue(*shadows, on);
                                    }
                                    else
                                    {
                                        sL.lastShadow = ll_round(pm::itemValue(*shadows));
                                        pm::setItemValue(*shadows, 0.f);
                                    }
                                });
                }
                if (const pm::Item* lod = pm::findItem(items::ITEM_LOD))
                {
                    stack.check(ls("MaxLod"), lod->note,
                                [lod]() { return ll_round(pm::itemValue(*lod)) == 1 ? 1.f : 0.f; },  // 1: not by distance (2 is "gentle")
                                [lod](float v) { pm::setItemValue(*lod, v > 0.5f ? 1.f : 0.f); });
                }
                addPlaced(stack, items::Spot::LeftSceneDisplay);   // the temporal anti-aliasing, switched at once
                stack.endSection();
            }
        }

        // ---- the avatar tab ---------------------------------------------------------------------

        // The AO was on, and "Stop the AO" here turned it off ("Reset the avatar" brings back only that).
        bool sAOStoppedHere = false;

        LLControlVariable* aoControl()
        {
            return gSavedPerAccountSettings.getControl(SETTING_AO);
        }

        void addGaze(Stack& stack)
        {
            if (!pm::gazeAvailable())
            {
                return;
            }
            std::vector<pm::Option> options = { { 0.f, str("Gaze0") }, { 1.f, str("Gaze1") } };
            if (pm::gazeAllowsEveryone())
            {
                options.push_back({ 2.f, str("Gaze2") });
            }
            stack.combo(str("Gaze"), std::string(), options, []() { return (float)pm::gazeMode(); },
                        [](float v) { pm::setGazeMode(ll_round(v)); });
        }

        void addFreeze(Stack& stack)
        {
            stack.check(str("Freeze"), str("FreezeTip"), []() { return plus::freeze::isFrozen() ? 1.f : 0.f; },
                        [](float v) { plus::freeze::setFrozen(v > 0.5f); });
        }

        void addStopAO(Stack& stack)
        {
            LLControlVariable* control = aoControl();
            if (!control)
            {
                return;
            }
            // The AO on again from anywhere (the AO's own window, a gesture) is the user's: "Reset the avatar" then
            // leaves it as the user sets it later (off too). Connected once; the control lives as long as the account's
            // settings.
            static boost::signals2::connection ao_watch;
            if (!ao_watch.connected())
            {
                ao_watch = control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&) {
                    if (value.asBoolean())
                    {
                        sAOStoppedHere = false;
                    }
                });
            }
            stack.check(ls("StopAO"), ls("StopAOTip"),
                        []() {
                            LLControlVariable* ao = aoControl();
                            return ao && !ao->getValue().asBoolean() ? 1.f : 0.f;
                        },
                        [](float v) {
                            if (LLControlVariable* ao = aoControl())
                            {
                                const bool on = v < 0.5f;
                                if (!on && ao->getValue().asBoolean())
                                {
                                    sAOStoppedHere = true;
                                }
                                else if (on)
                                {
                                    sAOStoppedHere = false;
                                }
                                ao->setValue(LLSD(on));
                            }
                        });
        }

        void resetAvatar()
        {
            plus::freeze::setFrozen(false);
            // only an AO that was stopped here comes back on (one the user does not use stays off)
            if (sAOStoppedHere)
            {
                sAOStoppedHere = false;
                if (LLControlVariable* ao = aoControl())
                {
                    ao->setValue(LLSD(true));
                }
            }
            if (pm::gazeAvailable())
            {
                pm::setGazeMode(0);
            }
        }

        void fillAvatar(Stack& stack)
        {
            if (section(stack, "avatar", ls("SecAvatar"), "user"))
            {
                if (psui::PsCard* card = stack.card("avatar", "avatar", std::string(), std::string(), std::string(), nullptr))
                {
                    sL.avatar = card->getHandle();
                }
                addStopAO(stack);
                addFreeze(stack);
                addGaze(stack);
                stack.button(ls("ResetAvatar"), ls("ResetAvatarTip"), &resetAvatar);
                stack.endSection();
            }
            if (section(stack, "look", ls("SecLook"), "sliders"))
            {
                for (const pm::Item& item : pm::items())
                {
                    if (item.tab == "subject" && !item.detail)
                    {
                        stack.item(item);
                    }
                }
                stack.endSection();
            }
        }

        // ---- the environment tab: the photo tools' "sky and weather" by their headings -----------

        const char* headingIcon(const std::string& before)
        {
            if (before.find("Rain") != std::string::npos || before.find("Water") != std::string::npos)
            {
                return "environment";
            }
            return "sun";
        }

        void fillEnvironment(Stack& stack)
        {
            struct Group
            {
                std::string id, label;
                std::vector<const pm::Item*> items;
            };
            std::vector<Group> groups;
            for (const pm::Item& item : pm::items())
            {
                if (item.tab != "light")
                {
                    continue;
                }
                for (const pm::Heading& heading : pm::headings())
                {
                    if (heading.tab == "light" && heading.before == item.id)
                    {
                        groups.push_back({ heading.before, heading.label, {} });
                    }
                }
                if (item.detail || item.kind == pm::Kind::Text)
                {
                    continue;
                }
                if (groups.empty())
                {
                    groups.push_back({ "light", pm::tabName("light"), {} });
                }
                groups.back().items.push_back(&item);
            }
            bool first = true;
            for (const Group& group : groups)
            {
                if (group.items.empty())
                {
                    continue;
                }
                if (section(stack, group.id.c_str(), group.label, headingIcon(group.id), first))
                {
                    for (const pm::Item* item : group.items)
                    {
                        stack.item(*item);
                    }
                    stack.endSection();
                }
                first = false;
            }
        }

        // ---- lighting and effects: the photo tools' items of photoscreen_items.h ------------------

        void fillLighting(Stack& stack)
        {
            if (section(stack, "light", ls("SecLight"), "lighting"))
            {
                addPlaced(stack, items::Spot::LeftLightingLight);
                stack.endSection();
            }
            if (section(stack, "shadow", ls("SecShadow"), "sun", false))
            {
                addPlaced(stack, items::Spot::LeftLightingShadow);
                stack.endSection();
            }
        }

        void fillEffects(Stack& stack)
        {
            if (section(stack, "lens", ls("SecLensFx"), "effects"))
            {
                addPlaced(stack, items::Spot::LeftEffectsLens);
                stack.endSection();
            }
            if (section(stack, "air", ls("SecAir"), "sun"))
            {
                addPlaced(stack, items::Spot::LeftEffectsAir);
                stack.endSection();
            }
        }

        // ---- pose and animation -----------------------------------------------------------------

        void openPoser()
        {
            LLFloaterReg::showInstance("fs_poser");
        }

        void fillPose(Stack& stack)
        {
            if (section(stack, "pose", ls("SecPose"), "pose"))
            {
                stack.button(ls("OpenPoser"), ls("OpenPoserTip"), &openPoser);
                // The saved poses: a click opens the poser (loading one from here needs the poser's loader,
                // which only its window and the MCP tools may call: research/221 (3): later).
                const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "poses");
                std::vector<std::string> names;
                if (gDirUtilp->fileExists(dir))
                {
                    for (const std::string& file : gDirUtilp->getFilesInDir(dir))
                    {
                        if (file.size() > 4 && file.compare(file.size() - 4, 4, ".xml") == 0)
                        {
                            names.push_back(file.substr(0, file.size() - 4));
                        }
                    }
                }
                std::sort(names.begin(), names.end());
                if (names.empty())
                {
                    stack.note(ls("NoPoses"));
                }
                else
                {
                    stack.label(ls("SavedPoses"));
                    for (const std::string& name : names)
                    {
                        stack.card("thumb", "pose", name, std::string(), ls("OpenPoserTip"), &openPoser);
                    }
                }
                stack.endSection();
            }
            if (section(stack, "motion", ls("SecMotion"), "avatar"))
            {
                addFreeze(stack);
                addStopAO(stack);
                stack.endSection();
            }
        }

        // ---- the camera tab: the lens, the flycam, the guide (the focus settings moved to the right column,
        //      photodetail_right.cpp addCamera(), research/264) ------------------------------------------

        // The flycam on the keyboard and the mouse (photo-flycam): the menu bar and the flycam's leave button are hidden
        // in here, so it is switched here (and by the icon over the world, photodetail_center.cpp). The row follows the
        // flycam however it was switched; the note says the keys for the way the mouse looks (PlusFlycamMouseMode).
        void fillFlycam(Stack& stack)
        {
            LLStringUtil::format_map_t args;
            args["[KEY]"] = plus::flycammouse::switchKey();     // as bound when the tab is made
            stack.check(ls("Flycam"), ls("FlycamTip", args), []() { return plus::flycammouse::isOn() ? 1.f : 0.f; },
                        [](float v) {
                            if ((v > 0.5f) != plus::flycammouse::isOn())
                            {
                                plus::flycammouse::toggleFromUi();
                            }
                        });
            static LLCachedControl<S32> look(gSavedSettings, "PlusFlycamMouseMode", 1);
            stack.note(ls("FlycamKeys" + std::to_string(llclamp((S32)look, 0, 2))));
        }

        // The photo tools' camera rows in the camera section: those of the catalog's camera tab but for the ones the
        // section leaves out (photoscreen_items.h, LeftCameraLens: photo-camera, research/260). The guide has its own
        // section.
        bool inCameraSection(const pm::Item& item)
        {
            return item.tab == "camera" && !item.detail && !startsWith(item.id, "PlusPhotoGuide") &&
                   !items::omits(items::Spot::LeftCameraLens, item.id);
        }

        void openCameraPosition()
        {
            // as the official camera controls' "Use precise controls" (LLFloaterCamera::postBuild)
            LLFloaterReg::showInstance("prefs_view_advanced", LLSD(), false);
        }

        // The official camera controls (floater_camera.xml) in the photo screen's look: orbit, zoom and pan, the tilt
        // row and the camera position typed in (photocamctl.cpp).
        void fillCameraControls(Stack& stack)
        {
            camctl::Words words;
            words.orbit          = ls("CamOrbit");
            words.orbit_tip      = ls("CamOrbitTip");
            words.pan            = ls("CamPan");
            words.pan_tip        = ls("CamPanTip");
            words.zoom           = ls("CamZoom");
            words.zoom_tip       = ls("CamZoomTip");
            words.zoom_in_tip    = ls("CamZoomInTip");
            words.zoom_out_tip   = ls("CamZoomOutTip");
            words.tilt_left_tip  = ls("TiltLeftTip");
            words.tilt_right_tip = ls("TiltRightTip");
            words.tilt_value_tip = ls("TiltValueTip");
            words.level          = ls("TiltLevel");
            words.level_tip      = ls("TiltLevelTip");

            const S32 width = stack.innerWidth();
            stack.view(camctl::makePads(width, words), camctl::padsHeight(width), ROW_GAP);
            stack.label(ls("Tilt"), ls("TiltTip"));
            stack.view(camctl::makeTiltRow(width, words), camctl::TILT_H, ROW_GAP / 2);
            stack.button(ls("PreciseCamera"), ls("PreciseCameraTip"), &openCameraPosition);
        }

        void fillCamera(Stack& stack)
        {
            // The focus settings (合わせ方・対象・部位・クリックで指定) moved to the right column's camera
            // section (research/264, photodetail_right.cpp addCamera()): not repeated here any more.
            if (section(stack, "lens", ls("SecLens"), "camera"))
            {
                fillCameraControls(stack);
                for (const pm::Item& item : pm::items())
                {
                    if (inCameraSection(item))
                    {
                        stack.item(item);
                    }
                }
                stack.endSection();
            }
            if (section(stack, "flycam", ls("SecFlycam"), "flycam"))
            {
                fillFlycam(stack);
                stack.endSection();
            }
            if (section(stack, "guide", ls("SecGuide"), "grid", false))
            {
                addPlaced(stack, items::Spot::LeftCameraGuide);
                stack.endSection();
            }
        }

        // ---- the live parts ---------------------------------------------------------------------

        void refreshLocation()
        {
            psui::PsCard* card = live<psui::PsCard>(sL.location);
            if (!card)
            {
                return;
            }
            LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
            LLViewerRegion* region = gAgent.getRegion();
            static const std::string none;
            const std::string& parcel_name = parcel ? parcel->getName() : none;
            const std::string& region_name = region ? region->getName() : none;
            // <plus> rlv: @showloc - the card shows no parcel, region or parcel picture (research/248 M1)
            const bool rlv_hidden = plus::rlv::enabled() && !plus::rlv::canShowMenuLocation();
            if (rlv_hidden)
            {
                const std::string& hidden_parcel = plus::rlv::hiddenParcelText();
                const std::string& hidden_region = plus::rlv::hiddenRegionText();
                if (!sL.locationSet || hidden_parcel != sL.parcelName || hidden_region != sL.regionName || !sL.snapshot.isNull())
                {
                    sL.locationSet = true;
                    sL.parcelName  = hidden_parcel;
                    sL.regionName  = hidden_region;
                    sL.snapshot    = LLUUID::null;
                    card->setLabelText(hidden_parcel);
                    card->setSubtitle(hidden_region);
                    card->setTexture(LLPointer<LLTexture>());
                }
                return;
            }
            // </plus>
            if (!sL.locationSet || parcel_name != sL.parcelName || region_name != sL.regionName)
            {
                sL.locationSet = true;
                sL.parcelName  = parcel_name;
                sL.regionName  = region_name;
                card->setLabelText(parcel_name.empty() ? ls("LocationNone") : parcel_name);
                card->setSubtitle(region_name);
            }
            const LLUUID snapshot = parcel ? parcel->getSnapshotID() : LLUUID::null;
            if (snapshot != sL.snapshot)
            {
                sL.snapshot = snapshot;
                if (snapshot.isNull())
                {
                    card->setTexture(LLPointer<LLTexture>());
                }
                else
                {
                    LLPointer<LLViewerFetchedTexture> texture =
                        LLViewerTextureManager::getFetchedTexture(snapshot, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                    texture->setKnownDrawSize(128, 128);    // a small picture: no more is loaded than shown
                    card->setTexture(LLPointer<LLTexture>(texture.get()));
                }
            }
        }

        void refreshAvatar()
        {
            psui::PsCard* card = live<psui::PsCard>(sL.avatar);
            if (!card || !isAgentAvatarValid())
            {
                return;
            }
            if (!sL.avatarNamed)
            {
                // the display name once it is known (the full name meanwhile)
                LLAvatarName av_name;
                sL.avatarNamed = LLAvatarNameCache::get(gAgent.getID(), &av_name);
                card->setLabelText(sL.avatarNamed ? av_name.getDisplayName() : gAgentAvatarp->getFullname());
            }
            const U32 complexity = gAgentAvatarp->getVisualComplexity();
            if (complexity != sL.complexity)
            {
                sL.complexity = complexity;
                std::string number;
                LLResMgr::getInstance()->getIntegerString(number, (S32)llmin(complexity, (U32)S32_MAX));
                LLStringUtil::format_map_t args;
                args["[N]"] = number;
                card->setSubtitle(ls("Complexity", args));
            }
        }

        void refreshScene()
        {
            refreshLocation();
            skywater::refresh();
            if (psui::PsSlider* time = live<psui::PsSlider>(sL.time))
            {
                const int index = timeOfDay();
                if (index != sL.timeShown)
                {
                    sL.timeShown = index;
                    time->setDisplayText(ls(index < 0 ? std::string("TimeOther") : "Time" + std::to_string(index)));
                }
            }
        }

        // The focus section: the people near, the part and one line saying what the lock does (as the
        // Avatar tab of the photo tools). Written only where something changed.
        // The parts of the column shown that are not rows (once after it is made, then every tick).
        void refreshLive()
        {
            switch (sL.tab)
            {
                case LeftTab::Quality: refreshQuality(); break;
                case LeftTab::Scene:   refreshScene();   break;
                case LeftTab::Avatar:  refreshAvatar();  break;
                default: break;
            }
        }

        // ---- the panel --------------------------------------------------------------------------

        // A tab of before (photodetail_state.h, still set by the other areas: the focus button over the world
        // asks for TabFocus) as the tab of now.
        LeftTab fromOldTab(Tab tab)
        {
            switch (tab)
            {
                case TabFocus:
                case TabCamera: return LeftTab::Camera;
                case TabColor:  return LeftTab::Lighting;
                case TabSky:    return LeftTab::Scene;
                case TabAvatar: return LeftTab::Avatar;
                default:        return LeftTab::Camera;     // the save tab's rows are the right panel's output now
            }
        }

        void selectTab(LeftTab tab)
        {
            if (!sS.wide)
            {
                // the folded panel: the same tab again closes it
                sS.flyout = !(sS.flyout && sL.tab == tab && !sL.conv);
            }
            else if (tab == sL.tab && !sL.conv)
            {
                return;
            }
            sL.conv = false;
            sL.tab  = tab;
            fillLeft();
            relayout();
        }

        void setValueIfChanged(LLUICtrl* ctrl, bool on)
        {
            if (ctrl && ctrl->getValue().asBoolean() != on)
            {
                ctrl->setValue(LLSD(on));
            }
        }

        void showRailSelection(LLPanel* left)
        {
            for (int i = 0; i < LEFT_TABS; ++i)
            {
                setValueIfChanged(left->findChild<LLUICtrl>(std::string("tab_") + LEFT_TAB_IDS[i]),
                                  !sL.conv && i == static_cast<int>(sL.tab));
            }
            setValueIfChanged(left->findChild<LLUICtrl>("tab_conv"), sL.conv);
        }

        // The body shows the tab's column, or the conversations while that tab is chosen (the column if their panel
        // did not load).
        void showBodyPage(LLPanel* left)
        {
            if (!sL.convPanel.get())
            {
                sL.conv = false;
            }
            LLView* body = left->findChildView("body", false);
            if (LLView* scroll = body ? body->findChildView("scroll", false) : nullptr)
            {
                scroll->setVisible(!sL.conv);
            }
            if (LLPanel* conv = sL.convPanel.get())
            {
                conv->setVisible(sL.conv);
            }
        }

        // The conversations tab: chosen, or (chosen already) back to the tab before; folded, it opens and closes the
        // panel as the other tabs do.
        void selectConv()
        {
            LLPanel* left = sS.left.get();
            if (!left)
            {
                return;
            }
            const bool was = sL.conv;
            if (!sL.convPanel.get())
            {
                showRailSelection(left);    // nothing to show: the tab chosen stays
                return;
            }
            if (!sS.wide)
            {
                sS.flyout = !(sS.flyout && was);
                sL.conv   = true;
            }
            else
            {
                sL.conv = !was;
            }
            showRailSelection(left);
            showBodyPage(left);
            relayout();
            if (sL.conv && (sS.wide || sS.flyout))
            {
                conv::openPanel(sL.convPanel.get());
            }
        }

        void showConvBadge()
        {
            LLPanel* left = sS.left.get();
            if (psui::PsRailTab* tab = left ? left->findChild<psui::PsRailTab>("tab_conv") : nullptr)
            {
                tab->setBadge(conv::badgeCount());
            }
        }

        void clearLive()
        {
            sL.location = sL.avatar = sL.time = sL.level = LLHandle<LLView>();
            skywater::clear();
            sL.locationSet = false;
            sL.parcelName.clear();
            sL.regionName.clear();
            sL.avatarNamed = false;
            sL.snapshot.setNull();
            sL.complexity = ~0u;
            sL.timeShown = -2;
            sL.levelShown = -2;
        }
    }

    void fillLeft()
    {
        LLPanel* left = sS.left.get();
        if (!left)
        {
            return;
        }
        if (sS.tab != TabCount)
        {
            // another area asked for a tab of before (the focus button): show where its rows are now (the
            // focus settings themselves are the right column's camera section, research/264)
            sL.tab  = fromOldTab(sS.tab);
            sL.conv = false;
            sS.tab = TabCount;
        }
        LLScrollContainer* scroll = left->findChild<LLScrollContainer>("scroll");
        LLPanel* content = left->findChild<LLPanel>("content");
        if (!scroll || !content)
        {
            return;
        }
        static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);

        // the same tab made again keeps where it was scrolled to
        const bool same_tab = sL.builtTab == sL.tab;
        const S32  old_height = content->getRect().getHeight();
        const S32  doc_pos    = scroll->getDocPosVertical();
        const S32  visible    = scroll->getRect().getHeight();

        clearLive();
        const S32 width = llmax(1, scroll->getRect().getWidth() - (S32)scrollbar_size);
        Stack stack(content, width, sL.rows);
        switch (sL.tab)
        {
            case LeftTab::Quality:     fillQuality(stack);     break;
            case LeftTab::Scene:       fillScene(stack);       break;
            case LeftTab::Avatar:      fillAvatar(stack);      break;
            case LeftTab::Environment: fillEnvironment(stack); break;
            case LeftTab::Lighting:    fillLighting(stack);    break;
            case LeftTab::Pose:        fillPose(stack);        break;
            case LeftTab::Camera:      fillCamera(stack);      break;
            case LeftTab::Effects:     fillEffects(stack);     break;
            default: break;
        }
        const S32 height = stack.finish();
        if (same_tab && doc_pos > 0 && old_height > 0)
        {
            // the window that was shown (doc_pos pixels under the top), in the new column's coordinates
            const S32 top = llmax(0, height - doc_pos);
            scroll->scrollToShowRect(LLRect(0, top, width, llmax(0, top - visible)));
        }
        else
        {
            scroll->goToTop();
        }
        sL.builtTab   = sL.tab;
        sL.builtWidth = scroll->getRect().getWidth();
        sS.leftRows.clear();    // the rows of before (photodetail_rows.cpp) are not used on the left any more
        showRailSelection(left);
        showBodyPage(left);
        Stack::refresh(sL.rows);
        refreshLive();
    }

    void bindLeftPanel()
    {
        LLPanel* left = sS.left.get();
        if (!left)
        {
            return;
        }
        sS.tab = TabCount;      // no tab of before asked for: the column starts at the scene, as the picture (the
                                // Quality tab over it is opened by the user; sL.tab is kept for the session)
        for (int i = 0; i < LEFT_TABS; ++i)
        {
            const LeftTab tab = static_cast<LeftTab>(i);
            if (LLUICtrl* button = left->findChild<LLUICtrl>(std::string("tab_") + LEFT_TAB_IDS[i]))
            {
                button->setCommitCallback([tab](LLUICtrl*, const LLSD&) { selectTab(tab); });
            }
        }
        if (LLUICtrl* button = left->findChild<LLUICtrl>("tab_conv"))
        {
            button->setCommitCallback([](LLUICtrl*, const LLSD&) { selectConv(); });
        }
        if (LLView* body = left->findChildView("body", false))
        {
            if (LLPanel* panel = conv::createPanel(body))
            {
                sL.convPanel = panel->getHandle();
            }
        }
        showConvBadge();
        sL.convChanged = conv::onChanged(&showConvBadge);
    }

    void layoutLeft(const Margins& m, bool open)
    {
        LLPanel* left = sS.left.get();
        if (!left)
        {
            return;
        }
        // Always the full width: the panel itself is see-through and lets the mouse through, the rail and
        // the body are the opaque parts. Folded, only the body is hidden (the widgets are never squeezed to
        // nothing by a narrower panel). The whole height of the window, as the picture: the preset strip is
        // between the side panels (photodetail_bottom.cpp, layoutBottom()).
        const S32 height = llmax(1, sS.h);
        left->setShape(LLRect(0, sS.h, m.rail + m.left_panel_full, 0));

        if (LLView* rail = left->findChildView("rail", false))
        {
            rail->setShape(LLRect(0, height, m.rail, 0));
            // the conversations tab at the bottom, out of the scrolled tabs (its badge is always in sight)
            S32 conv_top = 0;
            if (LLView* conv = rail->findChildView("tab_conv", false))
            {
                conv_top = RAIL_BOTTOM + psui::style::RAIL_TAB_HEIGHT;
                conv->setShape(LLRect(0, conv_top, m.rail, RAIL_BOTTOM));
            }
            if (LLView* scroll = rail->findChildView("rail_scroll", false))
            {
                scroll->setShape(LLRect(0, height, m.rail, llmin(height, conv_top)));
            }
            if (LLView* tabs = rail->findChildView("rail_tabs", true))
            {
                const S32 tabs_h = RAIL_TOP + LEFT_TABS * psui::style::RAIL_TAB_HEIGHT;
                tabs->reshape(m.rail, tabs_h);
                for (int i = 0; i < LEFT_TABS; ++i)
                {
                    if (LLView* tab = tabs->findChildView(std::string("tab_") + LEFT_TAB_IDS[i], false))
                    {
                        const S32 top = tabs_h - RAIL_TOP - i * psui::style::RAIL_TAB_HEIGHT;
                        tab->setShape(LLRect(0, top, m.rail, top - psui::style::RAIL_TAB_HEIGHT));
                    }
                }
            }
        }

        if (LLView* body = left->findChildView("body", false))
        {
            // the panel floats with a gap around it (research/221, the design part 0.)
            const S32 gap = psui::style::PANEL_GAP;
            const LLRect shape(m.rail + gap, height - gap, llmax(m.rail + gap + 1, m.rail + m.left_panel_full - gap), gap);
            body->setShape(shape);
            // inside the rounded corners: the column, or the conversations in its place
            const LLRect inner(0, shape.getHeight() - BODY_INSET_TOP, shape.getWidth(), BODY_INSET_BOTTOM);
            if (LLView* scroll = body->findChildView("scroll", false))
            {
                scroll->setShape(inner);
            }
            if (LLPanel* conv = sL.convPanel.get())
            {
                conv->setShape(inner);
            }
            body->setVisible(open);
            if (open && shape.getWidth() != sL.builtWidth)
            {
                fillLeft();     // the rows are made with the width of the panel
            }
        }
    }

    void tickLeft()
    {
        if (sL.conv)
        {
            return;     // the column is hidden; the conversations follow their own signal (photoconv.cpp)
        }
        Stack::refresh(sL.rows);
        refreshLive();
    }
}
