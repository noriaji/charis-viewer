/**
 * @file phototools_blocks.cpp
 * @brief The photo tools' blocks of Home, Quality, Camera, Avatar and Shoot: the presets and the user's own,
 *        the quality levels, the camera positions, the focus and the gaze, the shutter (research/265: made over
 *        to psui by pt-ui-blocks, moved here as they were by pt-ui-frame).
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

// What the frame (phototoolsfloater.cpp) calls: buildBlocks() from postBuild(), openBlocks() from onOpen(),
// drawBlocks() every frame and tickBlocks() every REFRESH_SECONDS. The blocks are the XUI's
// (panel_plus_photo_tools_home.xml, _quality.xml, _camera.xml, _subject.xml, _shoot.xml); a block that changes
// its height or is shown or hidden asks the frame to lay the page out again (requestLayout()).

#include "llviewerprecompiledheaders.h"

#include "phototoolsfloater_impl.h"
#include "plus/camroll/camroll.h"
#include "plus/freeze/freeze.h"
#include "plus/photoscreen/photoscreen.h"
#include "plus/presets/presets.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

namespace plus::photomode::tools
{
    using psui::setComboIfChanged;

    namespace
    {
        struct PresetButton
        {
            const char* button;
            const char* id;
        };
        constexpr PresetButton PRESET_BUTTONS[] = {
            { "preset_portrait",  "portrait" },
            { "preset_backlight", "backlight" },
            { "preset_sunset",    "sunset" },
            { "preset_night",     "night" },
            { "preset_studio",    "studio" },
            { "preset_natural",   "natural" },
            { "preset_autumn",    "autumn" },
        };
        // The official quality levels of the quality tab, Low to Ultra (plus/presets, research/183): they apply
        // through "Plus.Presets.SetLevel" (the XUI), and show the one in use (the official slider's).
        constexpr const char* GRAPHIC_LEVEL_BUTTONS[] = {
            "btn_gl_low", "btn_gl_lowmid", "btn_gl_mid", "btn_gl_midhigh", "btn_gl_high", "btn_gl_highultra", "btn_gl_ultra",
        };
        constexpr S32 GRAPHIC_LEVEL_COUNT = (S32)(sizeof(GRAPHIC_LEVEL_BUTTONS) / sizeof(GRAPHIC_LEVEL_BUTTONS[0]));
    }

    // --- what the frame calls ------------------------------------------------------------

    void PhotoToolsFloater::buildBlocks()
    {
        getChild<LLButton>("btn_home_camera")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showTab(TAB_CAMERA); });
        // the photo screen (plus/photoscreen) hides this window with the others; later it comes back
        getChild<LLButton>("btn_home_photoscreen")->setCommitCallback([](LLUICtrl*, const LLSD&) { plus::photoscreen::enter(); });

        // --- home ---
        for (const PresetButton& entry : PRESET_BUTTONS)
        {
            const std::string id = entry.id;
            getChild<LLButton>(entry.button)->setCommitCallback([this, id](LLUICtrl*, const LLSD&) { onPreset(id); });
        }
        getChild<LLComboBox>("my_preset_list")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
            endCompare();
            applyMyPreset(ctrl->getValue().asString());
            refreshNow();
        });
        getChild<LLButton>("btn_my_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            endCompare(); // the user's values go into the preset, not the compare's
            LLLineEditor* editor = getChild<LLLineEditor>("ed_my_name");
            std::string   name   = editor->getText();
            LLStringUtil::trim(name);
            if (name.empty())
            {
                const std::vector<std::string> taken = myPresetNames();
                for (int n = 1; n < 100 && name.empty(); ++n)
                {
                    LLStringUtil::format_map_t args;
                    args["[N]"] = std::to_string(n);
                    const std::string candidate = getString("my_name_auto", args);
                    if (std::find(taken.begin(), taken.end(), candidate) == taken.end())
                    {
                        name = candidate;
                    }
                }
            }
            if (saveMyPreset(name))
            {
                editor->clear();
            }
            else if (!name.empty())
            {
                // the only other reason: a new name while the list is full
                LLStringUtil::format_map_t args;
                args["[MAX]"] = std::to_string(MAX_MY_PRESETS);
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("my_presets_full", args)));
            }
            refreshMyPresets();
        });
        getChild<LLButton>("btn_my_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            const std::string name = getChild<LLComboBox>("my_preset_list")->getValue().asString();
            if (!name.empty())
            {
                deleteMyPreset(name);
            }
            refreshMyPresets();
        });
        getChild<LLButton>("btn_home_shoot")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShoot(); });
        for (const char* name : { "home_shot_size", "shot_size" })
        {
            getChild<LLComboBox>(name)->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                gSavedSettings.setS32("PlusPhotoShotSize", ctrl->getValue().asInteger());
                later([](PhotoToolsFloater* floater) { floater->refreshShotMenus(); }); // not inside the combo's own click
            });
        }
        for (const char* name : { "home_shot_aspect", "shot_aspect" })
        {
            LLComboBox* combo = getChild<LLComboBox>(name);
            static const char* ASPECT_STRINGS[SHOT_ASPECT_COUNT] = { "aspect_window", "aspect_16x9", "aspect_3x2", "aspect_4x3",
                                                                      "aspect_1x1", "aspect_9x16", "aspect_2x3" };
            for (int i = 0; i < SHOT_ASPECT_COUNT; ++i)
            {
                combo->add(getString(ASPECT_STRINGS[i]), LLSD(i));
            }
            combo->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                gSavedSettings.setS32("PlusPhotoShotAspect", ctrl->getValue().asInteger());
                later([](PhotoToolsFloater* floater) { floater->refreshShotMenus(); });
            });
        }
        // The one-click switch of the home tab: checked = no level of detail by distance (RenderDynamicLOD off).
        getChild<LLCheckBoxCtrl>("chk_home_lod")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
            endCompare();
            gSavedSettings.setBOOL("RenderDynamicLOD", !ctrl->getValue().asBoolean());
            refreshRows();
            refreshStatus();
        });
        getChild<LLButton>("btn_roll_reset")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            endCompare();
            plus::camroll::setDegrees(0.f);
            refreshRows();
            refreshStatus();
        });
        // The composition guide (plus/photomode/photoguide.cpp): one click on the home tab.
        getChild<LLButton>("btn_home_guide")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
            endCompare();
            gSavedSettings.setBOOL("PlusPhotoGuideOn", ctrl->getValue().asBoolean());
            refreshRows();
            refreshStatus();
        });
        {
            LLComboBox* kinds = getChild<LLComboBox>("home_guide_kind");
            if (const Item* item = findItem("PlusPhotoGuideKind"))
            {
                for (const Option& option : item->options)
                {
                    kinds->add(option.label, LLSD((LLSD::Integer)ll_round(option.value)));
                }
            }
            kinds->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                endCompare();
                gSavedSettings.setS32("PlusPhotoGuideKind", ctrl->getValue().asInteger());
                later([](PhotoToolsFloater* floater) { floater->refreshRows(); floater->refreshStatus(); });
            });
        }
        getChild<LLButton>("btn_shot_open")->setCommitCallback([](LLUICtrl*, const LLSD&) { openLastShot(); });
        getChild<LLButton>("btn_shot_folder")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            if (!openSaveDir())
            {
                LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("save_dir_missing")));
            }
        });
        getChild<LLButton>("btn_shot_dir")->setCommitCallback([](LLUICtrl*, const LLSD&) { chooseSaveDir(); });
        getChild<LLButton>("btn_shot_window")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("snapshot"); });

        // --- camera ---
        mCameraList = getChild<LLScrollListCtrl>("list_cam");
        mCameraName = getChild<LLLineEditor>("ed_cam_name");
        mCameraList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshCameraButtons(); });
        mCameraList->setDoubleClickCallback([this]() { onCameraGo(); });
        getChild<LLButton>("btn_cam_temp_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            saveTempView();
            refreshCameraButtons();
        });
        getChild<LLButton>("btn_cam_temp_go")->setCommitCallback([](LLUICtrl*, const LLSD&) {
            goToTempView(gSavedSettings.getBOOL(SETTING_SMOOTH));
        });
        getChild<LLButton>("btn_cam_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCameraSave(); });
        getChild<LLButton>("btn_cam_go")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCameraGo(); });
        getChild<LLButton>("btn_cam_rename")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCameraRename(); });
        getChild<LLButton>("btn_cam_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCameraDelete(); });

        // --- shutter, gaze, freeze ---
        getChild<LLButton>("btn_shoot")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShoot(); });
        getChild<LLRadioGroup>("gaze_mode")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
            setGazeMode(static_cast<LLRadioGroup*>(ctrl)->getSelectedIndex());
        });
        // The world freeze (plus/freeze): one press stops, the next resumes. The label follows
        // PlusWorldFreeze (draw()), which also goes off by itself at a teleport.
        getChild<LLButton>("btn_freeze")->setCommitCallback([](LLUICtrl*, const LLSD&) {
            plus::freeze::setFrozen(!plus::freeze::isFrozen());
        });
        getChild<LLButton>("btn_toggle_gaze")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            toggleSection("body_gaze_adv", "btn_toggle_gaze", "gaze");
        });

        // --- focus lock (plus/focuslock) ---
        getChild<LLRadioGroup>("focus_mode")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
            const auto mode = static_cast<plus::focuslock::Mode>(
                llclamp(static_cast<LLRadioGroup*>(ctrl)->getSelectedIndex(), 0, plus::focuslock::MODE_COUNT - 1));
            if (mode == plus::focuslock::MODE_OTHER && plus::focuslock::getAvatar().isNull() &&
                plus::focuslock::listNearby(mFocusNearby) > 0)
            {
                plus::focuslock::setAvatar(mFocusNearby.front().id); // the nearest, until someone is chosen
            }
            plus::focuslock::setMode(mode);
            refreshFocus();
        });
        getChild<LLComboBox>("focus_who")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
            const LLUUID id = ctrl->getValue().asUUID();
            if (id.notNull())
            {
                plus::focuslock::setAvatar(id);
                plus::focuslock::setMode(plus::focuslock::MODE_OTHER); // choosing someone means to follow them
            }
        });
        LLComboBox* focus_part = getChild<LLComboBox>("focus_part");
        focus_part->add(getString("focus_part_eyes"), LLSD((S32)plus::focuslock::PART_EYES));
        focus_part->add(getString("focus_part_chest"), LLSD((S32)plus::focuslock::PART_CHEST));
        focus_part->add(getString("focus_part_body"), LLSD((S32)plus::focuslock::PART_BODY));
        focus_part->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
            plus::focuslock::setPart(static_cast<plus::focuslock::Part>(ctrl->getValue().asInteger()));
        });
        getChild<LLButton>("btn_focus_blur")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
            if (const Item* item = findItem("RenderDepthOfField"))
            {
                setItemValue(*item, 1.f);
                startDofSwitch(); // the pipeline is rebuilt: the viewer stands still for seconds
                refreshRows();
                refreshStatus();
            }
            refreshFocus();
        });
        getChild<LLPanel>("body_gaze_adv")->setVisible(false);
    }

    void PhotoToolsFloater::openBlocks()
    {
        mRegionShown.clear();
        updatePresetButtons();
        refreshMyPresets();
        refreshHomeControls();
        refreshCameraList();
        refreshGaze();
        mFrozenSecondsShown = -2; // label again
        mShotSeen = lastShot().number; // an old result is not announced again
        mShotMenuKey.clear();
        mShotSecondsShown = SHOT_LABEL_UNSET;
        refreshShotMenus();
        refreshSaveDir();
        refreshShotState();
        showShotResult();
    }

    void PhotoToolsFloater::drawBlocks()
    {
        // The few things that change without this window: the region (the list of cameras is
        // the region's), the shutter, the world freeze.
        static const std::string NO_REGION;
        const LLViewerRegion*    region = gAgent.getRegion();
        if ((region ? region->getName() : NO_REGION) != mRegionShown // as regionName(), without a copy every frame
            || namedViewsHidden() != mLocationHidden)
        {
            refreshCameraList();
        }
        refreshShotState();
        const bool frozen  = plus::freeze::isFrozen();
        const S32  seconds = frozen ? (S32)plus::freeze::frozenSeconds() : -1;
        if (frozen != mFrozenShown || seconds != mFrozenSecondsShown)
        {
            mFrozenShown        = frozen;
            mFrozenSecondsShown = seconds;
            LLStringUtil::format_map_t args;
            args["[SECONDS]"] = std::to_string(llmax(seconds, 0));
            getChild<LLButton>("btn_freeze")->setLabel(getString(frozen ? "freeze_resume" : "freeze_start", args));
        }
    }

    // The blocks of the tab shown only (the frame calls this too when a tab is shown).
    void PhotoToolsFloater::tickBlocks()
    {
        const int tab = shownTab();
        if (tab == TAB_HOME || tab == TAB_QUALITY)
        {
            updatePresetButtons();
        }
        if (tab == TAB_HOME)
        {
            refreshHomeControls();
        }
        if (tab == TAB_HOME || tab == TAB_SHOOT)
        {
            refreshShotMenus();
        }
        if (tab == TAB_SHOOT)
        {
            refreshSaveDir();
        }
        if (tab == TAB_SUBJECT)
        {
            refreshFocus(); // the people around, the target's distance
        }
        if (gazeAllowsEveryone() != mEveryoneShown)
        {
            refreshGaze(); // a debug setting changed: the third choice comes or goes
        }
    }

    // The switches of Home from the settings (they can change elsewhere: the photo screen, the menu): written only
    // when they differ.
    void PhotoToolsFloater::refreshHomeControls()
    {
        static LLCachedControl<bool> dynamic_lod(gSavedSettings, "RenderDynamicLOD", true);
        static LLCachedControl<bool> guide_on(gSavedSettings, "PlusPhotoGuideOn", false);
        static LLCachedControl<S32>  guide_kind(gSavedSettings, "PlusPhotoGuideKind", 1);
        LLCheckBoxCtrl* home_lod = getChild<LLCheckBoxCtrl>("chk_home_lod");
        if (home_lod->get() != !dynamic_lod())
        {
            home_lod->set(!dynamic_lod());
        }
        LLButton* home_guide = getChild<LLButton>("btn_home_guide");
        if (home_guide->getToggleState() != guide_on())
        {
            home_guide->setToggleState(guide_on());
        }
        setComboIfChanged(getChild<LLComboBox>("home_guide_kind"), LLSD((LLSD::Integer)guide_kind()));
    }

    // The photo presets' buttons and the quality levels' from the settings. Also on the periodic refresh: a
    // level is applied a few frames after its click (a notice first), and either can change from elsewhere
    // (the Preferences). The levels' buttons are set only when the level is another.
    void PhotoToolsFloater::updatePresetButtons()
    {
        const std::string current = currentPreset();
        for (const PresetButton& entry : PRESET_BUTTONS)
        {
            getChild<LLButton>(entry.button)->setToggleState(current == entry.id);
        }
        // in the comparison with the official viewer no level is lit, as on the photo screen (bugloop round 1)
        const S32 level = plus::presets::comparisonInUse() ? GRAPHIC_LEVEL_COUNT : (S32)plus::presets::graphicsLevel();
        if (level != mShownGraphicLevel)
        {
            mShownGraphicLevel = level;
            for (S32 i = 0; i < GRAPHIC_LEVEL_COUNT; ++i)
            {
                getChild<LLButton>(GRAPHIC_LEVEL_BUTTONS[i])->setToggleState(i == level);
            }
        }
    }

    void PhotoToolsFloater::onPreset(const std::string& id)
    {
        endCompare();
        applyPreset(id);
        refreshNow();
    }

    void PhotoToolsFloater::refreshMyPresets()
    {
        LLComboBox* combo = getChild<LLComboBox>("my_preset_list");
        combo->removeall();
        const std::vector<std::string> names = myPresetNames();
        for (const std::string& name : names)
        {
            combo->add(name, LLSD(name));
        }
        // nothing is chosen in it (a preset is put on when it is chosen): the name of the list, never blank
        combo->setLabel(getString(names.empty() ? "my_presets_none" : "my_presets_label"));
    }

    void PhotoToolsFloater::toggleSection(const char* body, const char* header, const char* label_string)
    {
        LLPanel*   panel = getChild<LLPanel>(body);
        const bool open  = !panel->getVisible();
        panel->setVisible(open);
        getChild<LLButton>(header)->setLabel(getString(std::string(label_string) + (open ? "_open" : "_closed")));
        requestLayout();
    }

    // --- camera --------------------------------------------------------

    S32 PhotoToolsFloater::selectedCamera() const
    {
        return mCameraList->getFirstSelectedIndex();
    }

    std::string PhotoToolsFloater::autoName() const
    {
        const std::vector<CameraView> views = namedViews();
        for (int n = 1; n <= MAX_NAMED_VIEWS + 1; ++n)
        {
            LLStringUtil::format_map_t args;
            args["[N]"] = std::to_string(n);
            const std::string name = getString("auto_name", args);
            bool taken = false;
            for (const CameraView& view : views)
            {
                taken = taken || view.name == name;
            }
            if (!taken)
            {
                return name;
            }
        }
        return getString("auto_name", LLStringUtil::format_map_t{ { "[N]", "9" } });
    }

    void PhotoToolsFloater::refreshCameraList()
    {
        // The list is the region's: in another region the same row number is another position (a
        // delete or rename would hit it), so the selection goes with the region.
        // RLVa @showloc (namedViewsHidden()): no positions (namedViews() is empty) and no region name.
        const bool hidden   = namedViewsHidden();
        const S32  selected = (!hidden && !mLocationHidden && regionName() == mRegionShown) ? selectedCamera() : -1;
        mCameraList->clearRows();
        mLocationHidden = hidden;

        const std::vector<CameraView> views = namedViews();
        for (const CameraView& view : views)
        {
            LLSD row;
            row["columns"][0]["column"] = "name";
            row["columns"][0]["value"]  = view.name;
            mCameraList->addElement(row);
        }
        if (selected >= 0 && (size_t)selected < views.size())
        {
            mCameraList->selectNthItem(selected);
        }

        mRegionShown = regionName();
        LLStringUtil::format_map_t args;
        args["[REGION]"] = hidden ? plus::rlv::regionNameForPlus(mRegionShown)
                                  : (mRegionShown.empty() ? getString("no_region") : mRegionShown);
        args["[COUNT]"]  = std::to_string(views.size());
        args["[MAX]"]    = std::to_string(MAX_NAMED_VIEWS);
        getChild<LLTextBox>("cam_region")->setText(getString("region_label", args));
        refreshCameraButtons();
    }

    void PhotoToolsFloater::refreshCameraButtons()
    {
        const bool picked = selectedCamera() >= 0;
        getChild<LLButton>("btn_cam_go")->setEnabled(picked);
        getChild<LLButton>("btn_cam_delete")->setEnabled(picked);
        getChild<LLButton>("btn_cam_rename")->setEnabled(picked);
        getChild<LLButton>("btn_cam_save")->setEnabled(!mLocationHidden && namedViews().size() < (size_t)MAX_NAMED_VIEWS
                                                       && !regionName().empty());
        getChild<LLButton>("btn_cam_temp_go")->setEnabled(hasTempView());
    }

    void PhotoToolsFloater::onCameraSave()
    {
        std::string name = mCameraName->getText();
        LLStringUtil::trim(name);
        if (name.empty())
        {
            name = autoName();
        }
        if (saveNamedView(name))
        {
            mCameraName->clear();
        }
        else if (allNamedViewsFull())
        {
            // The positions of every region together are kept up to a number; the list shows only this region's.
            LLStringUtil::format_map_t args;
            args["[MAX]"] = std::to_string(MAX_VIEWS_ALL);
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getString("cam_all_full", args)));
        }
        refreshCameraList();
    }

    void PhotoToolsFloater::onCameraGo()
    {
        const S32 index = selectedCamera();
        if (index >= 0)
        {
            goToNamedView((size_t)index, gSavedSettings.getBOOL(SETTING_SMOOTH));
        }
    }

    void PhotoToolsFloater::onCameraRename()
    {
        const S32   index = selectedCamera();
        std::string name  = mCameraName->getText();
        LLStringUtil::trim(name);
        if (index >= 0 && !name.empty() && renameNamedView((size_t)index, name))
        {
            mCameraName->clear();
        }
        refreshCameraList();
    }

    void PhotoToolsFloater::onCameraDelete()
    {
        const S32 index = selectedCamera();
        if (index >= 0)
        {
            deleteNamedView((size_t)index);
        }
        // The row number would now be the next position: a second click must not delete that one too.
        // With nothing selected, refreshCameraList() turns the delete button off.
        mCameraList->deselectAllItems();
        refreshCameraList();
    }

    // --- shutter, gaze -------------------------------------------------

    // Why a size cannot be taken, in the language of the viewer ("" when it can).
    std::string PhotoToolsFloater::whyNot(const ShotDims& dims, const ShotMemory& memory)
    {
        std::string why;
        double      need = 0.0, room = 0.0;
        if (shotFits(dims, memory, why, &need, &room))
        {
            return std::string();
        }
        LLStringUtil::format_map_t args;
        if (dims.w > 7680 || dims.h > 7680)
        {
            return getString("why_side");
        }
        if (need <= 0.0)
        {
            return getString("why_unknown");
        }
        args["[NEED]"] = llformat("%.1f", need);
        args["[FREE]"] = llformat("%.1f", room);
        return getString("why_memory", args);
    }

    // The photo button: one picture, taken a few frames later, with the size and shape of the menus.
    void PhotoToolsFloater::onShoot()
    {
        const ShotDims dims = shotDims(gSavedSettings.getS32("PlusPhotoShotSize"), gSavedSettings.getS32("PlusPhotoShotAspect"));
        const std::string why = whyNot(dims, shotMemory());
        if (!why.empty())
        {
            LLStringUtil::format_map_t args;
            args["[WHY]"] = why;
            const std::string text = getString("shot_fail", args);
            getChild<LLTextBox>("shot_status")->setText(text);
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", text));
            return;
        }
        takePhoto();
        refreshShotState();
    }

    // The size menu with the sizes the memory does not take switched off (and why), the shape menu, the line of numbers.
    void PhotoToolsFloater::refreshShotMenus()
    {
        static const char* SIZE_STRINGS[SHOT_SIZE_COUNT] = { "size_window", "size_2k", "size_4k", "size_6k", "size_8k" };
        static LLCachedControl<S32> shot_size(gSavedSettings, "PlusPhotoShotSize", 2);
        static LLCachedControl<S32> shot_aspect(gSavedSettings, "PlusPhotoShotAspect", 0);
        const int size   = llclamp((int)shot_size(), 0, SHOT_SIZE_COUNT - 1);
        const int aspect = llclamp((int)shot_aspect(), 0, SHOT_ASPECT_COUNT - 1);
        const ShotMemory memory = shotMemory();

        std::vector<std::string> labels, reasons;
        std::vector<bool>        enabled;
        std::string              key = std::to_string(aspect);
        std::string              tip;
        for (int i = 0; i < SHOT_SIZE_COUNT; ++i)
        {
            const ShotDims    dims = shotDims(i, aspect);
            const std::string why  = whyNot(dims, memory); // the driver asked once for all the sizes
            labels.push_back(getString(SIZE_STRINGS[i]) + "  " + std::to_string(dims.w) + " x " + std::to_string(dims.h));
            reasons.push_back(why);
            enabled.push_back(why.empty());
            key += "|" + labels.back() + (why.empty() ? "+" : "-");
            if (!why.empty())
            {
                tip += (tip.empty() ? "" : "\n") + getString(SIZE_STRINGS[i]) + ": " + why;
            }
        }
        if (key != mShotMenuKey)
        {
            mShotMenuKey = key;
            for (const char* name : { "home_shot_size", "shot_size" })
            {
                LLComboBox* combo = getChild<LLComboBox>(name);
                combo->removeall();
                for (int i = 0; i < SHOT_SIZE_COUNT; ++i)
                {
                    combo->add(labels[i], LLSD(i), ADD_BOTTOM, enabled[i]);
                }
                combo->setToolTip(tip.empty() ? getString("shot_size_tip") : tip);
            }
        }
        for (const char* name : { "home_shot_size", "shot_size" })
        {
            setComboIfChanged(getChild<LLComboBox>(name), LLSD(size));
        }
        for (const char* name : { "home_shot_aspect", "shot_aspect" })
        {
            setComboIfChanged(getChild<LLComboBox>(name), LLSD(aspect));
        }
        const ShotDims dims = shotDims(size, aspect);
        LLStringUtil::format_map_t args;
        args["[W]"]   = std::to_string(dims.w);
        args["[H]"]   = std::to_string(dims.h);
        args["[MP]"]  = llformat("%.1f", dims.megapixels());
        args["[WHY]"] = reasons[size];
        const std::string info = getString(enabled[size] ? "shot_dims" : "shot_dims_no", args);
        setTextIfChanged(getChild<LLTextBox>("home_shot_info"), info);
        setTextIfChanged(getChild<LLTextBox>("shot_info"), info);
    }

    // The folder line: written when the folder changed (also by the snapshot window); the tip is the whole line.
    void PhotoToolsFloater::refreshSaveDir()
    {
        const SaveDirView view = saveDirView();
        LLStringUtil::format_map_t args;
        args["[DIR]"] = view.dir;
        const std::string text = getString(view.key, args);
        if (text != mSaveDirShown)
        {
            mSaveDirShown = text;
            LLTextBox* box = getChild<LLTextBox>("shot_dir");
            box->setText(text);
            box->setToolTip(text);
        }
    }

    // The button says what is going on and cannot be pressed meanwhile; a new result is announced.
    void PhotoToolsFloater::refreshShotState()
    {
        const ShotState state = shotState();
        // the seconds of the wait (0: waiting for the memory), -1 when there is no wait
        int seconds = -1;
        if (state == ShotState::Cooling)
        {
            const float left = shotCoolingLeft();
            seconds          = left > 0.f ? (int)ceilf(left) : 0;
        }
        if (state != mShotStateShown || seconds != mShotSecondsShown)
        {
            mShotStateShown   = state;
            mShotSecondsShown = seconds;
            std::string label;
            bool        can = false;
            switch (state)
            {
            case ShotState::Idle:
                label = getString("shot_btn");
                can   = true;
                break;
            case ShotState::Pending:
            case ShotState::Busy:
                label = getString("shot_btn_taking");
                break;
            case ShotState::Cooling:
            {
                LLStringUtil::format_map_t args;
                args["[S]"] = std::to_string(seconds);
                label       = seconds > 0 ? getString("shot_btn_wait", args) : getString("shot_btn_wait_memory");
                break;
            }
            }
            for (const char* name : { "btn_shoot", "btn_home_shoot" })
            {
                LLButton* button = getChild<LLButton>(name);
                button->setLabel(label);
                button->setEnabled(can);
            }
        }
        if (lastShot().number != mShotSeen)
        {
            mShotSeen = lastShot().number;
            showShotResult();
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", getChild<LLTextBox>("shot_status")->getText()));
        }
    }

    void PhotoToolsFloater::showShotResult()
    {
        const ShotResult& result = lastShot();
        std::string       text;
        if (result.number == 0)
        {
            text = getString("shot_none");
        }
        else if (result.ok)
        {
            LLStringUtil::format_map_t args;
            args["[FILE]"] = gDirUtilp->getBaseFileName(result.path);
            args["[W]"]    = std::to_string(result.w);
            args["[H]"]    = std::to_string(result.h);
            args["[KB]"]   = std::to_string(result.bytes);
            args["[S]"]    = llformat("%.1f", result.ms / 1000.f);
            text           = getString("shot_ok", args);
        }
        else
        {
            LLStringUtil::format_map_t args;
            args["[WHY]"] = result.error;
            text          = getString("shot_fail", args);
        }
        getChild<LLTextBox>("shot_status")->setText(text);
        getChild<LLButton>("btn_shot_open")->setEnabled(result.ok);
    }

    // The Focus section of the Avatar tab: the mode, the people near the camera, the part, and one line saying
    // what the lock does now. Written only where something changed. Looked at while the tab is shown.
    void PhotoToolsFloater::refreshFocus()
    {
        namespace fl = plus::focuslock;
        const fl::Mode mode  = fl::getMode();
        LLRadioGroup*  group = getChild<LLRadioGroup>("focus_mode");
        if (group->getSelectedIndex() != (S32)mode)
        {
            group->setSelectedIndex((S32)mode);
        }

        // The people: the combo is filled again only when the list (who, in which order) is another.
        LLComboBox*  who    = getChild<LLComboBox>("focus_who");
        const LLUUID target = fl::getAvatar();
        fl::listNearby(mFocusNearby);
        std::string key;
        for (const fl::Nearby& person : mFocusNearby)
        {
            key += person.id.asString() + person.name + '|';
        }
        if (key != mFocusPeopleKey)
        {
            mFocusPeopleKey = key;
            who->removeall();
            for (const fl::Nearby& person : mFocusNearby)
            {
                who->add(person.name, LLSD(person.id));
            }
            who->setLabel(getString("focus_nobody"));
        }
        bool listed = false;
        for (const fl::Nearby& person : mFocusNearby)
        {
            listed = listed || person.id == target;
        }
        if (listed)
        {
            setComboIfChanged(who, LLSD(target));
        }
        else if (who->getCurrentIndex() >= 0)
        {
            who->clear(); // the one chosen went out of range: none is shown as chosen
            who->setLabel(getString("focus_nobody"));
        }
        LLComboBox* part = getChild<LLComboBox>("focus_part");
        setComboIfChanged(part, LLSD((S32)fl::getPart()));
        part->setEnabled(mode == fl::MODE_SELF || mode == fl::MODE_OTHER);

        // The line: what the lock does, or why it does not.
        const bool  blur_off = !blurAvailable();
        std::string text;
        if (blur_off)
        {
            text = getString(mDofBusy ? "focus_st_switching" : "focus_st_blur_off");
        }
        else
        {
            LLStringUtil::format_map_t args;
            args["[WHO]"]  = fl::focusName();
            args["[PART]"] = getString(std::string("focus_part_") + fl::partKey(fl::getPart()));
            F32 meters = 0.f;
            if (fl::focusDistance(meters))
            {
                args["[DIST]"] = llformat("%.1f", meters);
            }
            // the place clicked (research/264): whether a click sets it now, and whether it is set
            const bool picking = fl::isPicking();
            switch (fl::state())
            {
                case fl::State::Off:
                    text = getString("focus_st_off");
                    break;
                case fl::State::Locked:
                    if (mode == fl::MODE_CLICK)
                    {
                        text = getString(picking ? "focus_st_click_picking" : "focus_st_click_locked", args);
                    }
                    else
                    {
                        text = getString(mode == fl::MODE_CURSOR ? "focus_st_cursor" : mode == fl::MODE_SELF ? "focus_st_locked_self" : "focus_st_locked", args);
                    }
                    break;
                default:
                    if (mode == fl::MODE_CLICK)
                    {
                        text = getString(picking ? "focus_st_click_wait" : "focus_st_click_none");
                    }
                    else if (mode == fl::MODE_CURSOR)
                    {
                        text = getString("focus_st_lost_cursor");
                    }
                    else if (target.isNull() && mode == fl::MODE_OTHER)
                    {
                        text = getString("focus_st_nobody");
                    }
                    else
                    {
                        text = getString("focus_st_lost", args);
                    }
                    break;
            }
        }
        setTextIfChanged(getChild<LLTextBox>("focus_status"), text);
        LLButton* blur = getChild<LLButton>("btn_focus_blur");
        if (blur->getVisible() != blur_off)
        {
            blur->setVisible(blur_off);
        }
        blur->setEnabled(!mDofBusy);
    }

    void PhotoToolsFloater::refreshGaze()
    {
        // plus/gaze is merged: PlusGazeMode exists (disabled only with settings that lack it).
        LLRadioGroup* group = getChild<LLRadioGroup>("gaze_mode");
        group->setEnabled(gazeAvailable());
        group->setSelectedIndex(gazeMode());
        // "Everyone" is hidden (not just off) until PlusGazeAllowEveryone is on.
        mEveryoneShown = gazeAllowsEveryone();
        getChild<LLUICtrl>("gaze_everyone")->setVisible(mEveryoneShown);
        // The tuning sliders are bound to the settings of plus/gaze (control_name).
        for (const char* name : { "sl_gz_eye", "sl_gz_head", "sl_gz_conv", "sl_gz_biasl", "sl_gz_biasr", "chk_gz_fov", "sl_gz_headabs" })
        {
            getChild<LLUICtrl>(name)->setEnabled(gazeAvailable());
        }
        getChild<LLTextBox>("gaze_note")->setVisible(!gazeAvailable());
    }
}
