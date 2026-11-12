/**
 * @file quickprefsfloater.cpp
 * @brief Quick Preferences (ported from Firestorm, plus/quickprefs).
 *
 * Based on Firestorm's quickprefs.cpp. Left out: Phototools (the photo mode
 * of this viewer does that), RLVa, OpenSim legacy Windlight, the toolbar
 * button and docking, the fixed bandwidth and complexity rows (bandwidth is in
 * the standard list; see SOURCE.md).
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2011, WoLf Loonie @ Second Life
 * Copyright (C) 2013, Zi Ree @ Second Life
 * Copyright (C) 2013, Ansariel Hiller @ Second Life
 * Copyright (C) 2013, Cinder Biscuits @ Me too
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "quickprefsfloater.h"
#include "plus.h"
#include "plus/photomode/photomode.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "llenvironment.h"
#include "llfloaterpreference.h" // LLAvatarComplexityControls
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorysettings.h"
#include "lllayoutstack.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llxmlnode.h"
#include "llxuiparser.h"

#include <cmath>

namespace plus::quickprefs
{
    namespace
    {
        constexpr char ITEM_PANEL_FILE[]       = "panel_plus_quickprefs_item.xml";
        constexpr char USER_LIST_FILE[]        = "plus_quick_preferences.xml"; // in the user settings folder
        constexpr char SETTING_EDIT_MODE[]     = "PlusQuickPrefsEditMode";
        constexpr char SETTING_SELECTED[]      = "PlusQuickPrefsSelectedControl";
        constexpr char PRESET_NAME_REGION[]    = "__Regiondefault__";
        constexpr char PRESET_NAME_DAY_CYCLE[] = "__Day_Cycle__";
        constexpr char PRESET_NAME_NONE[]      = "__None__";
        constexpr LLFile::fopen_flags_t WRITE_MODE[] = { 'w', 0 }; // "w", for a char or a wchar_t

        // The settings items in the inventory, one per asset (not those in the marketplace listings).
        class SettingsCollector : public LLInventoryCollectFunctor
        {
        public:
            SettingsCollector() { mMarketplaceFolderUUID = gInventory.getMarketplaceListingsUUID(); }

            bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
            {
                if (item && item->getType() == LLAssetType::AT_SETTINGS && !gInventory.isObjectDescendentOf(item->getUUID(), mMarketplaceFolderUUID) &&
                    mSeen.find(item->getAssetUUID()) == mSeen.end())
                {
                    mSeen.insert(item->getAssetUUID());
                    return true;
                }
                return false;
            }

        private:
            LLUUID      mMarketplaceFolderUUID;
            uuid_set_t  mSeen;
        };

        // The list file, one <entries> per setting.
        struct QuickPrefsXMLEntry : public LLInitParam::Block<QuickPrefsXMLEntry>
        {
            Mandatory<std::string> control_name;
            Mandatory<std::string> label;
            Optional<std::string>  translation_id;
            Mandatory<U32>         control_type;
            Mandatory<bool>        integer;
            Mandatory<F32>         min_value; // "min" and "max" are macros in the Windows headers
            Mandatory<F32>         max_value;
            Mandatory<F32>         increment;

            QuickPrefsXMLEntry() :
                control_name("control_name"),
                label("label"),
                translation_id("translation_id"),
                control_type("control_type"),
                integer("integer"),
                min_value("min"),
                max_value("max"),
                increment("increment")
            {
            }
        };

        struct QuickPrefsXML : public LLInitParam::Block<QuickPrefsXML>
        {
            Multiple<QuickPrefsXMLEntry> entries;

            QuickPrefsXML() : entries("entries") {}
        };

        // Which widget of panel_plus_quickprefs_item.xml shows a type.
        const char* widgetName(ControlType type)
        {
            switch (type)
            {
                case ControlTypeCheckbox: return "option_checkbox_control";
                case ControlTypeText:     return "option_text_control";
                case ControlTypeSpinner:  return "option_spinner_control";
                case ControlTypeSlider:   return "option_slider_control";
                case ControlTypeRadio:    return "option_radio_control";
                case ControlTypeColor3:   return "option_color3_control";
                case ControlTypeColor4:   return "option_color4_control";
            }
            return "option_radio_control";
        }

        constexpr ControlType ALL_TYPES[] = { ControlTypeCheckbox, ControlTypeText,   ControlTypeSpinner, ControlTypeSlider,
                                              ControlTypeRadio,    ControlTypeColor3, ControlTypeColor4 };

        // Both kinds of the official preference settings are meant.
        LLControlVariable* findAnySetting(const std::string& name)
        {
            LLControlVariablePtr var = gSavedSettings.getControl(name);
            return var ? var.get() : gSavedPerAccountSettings.getControl(name).get();
        }

        // The settings object a layer shows now (compared only, never used): another one when anything changed it.
        const void* inUseOf(plus::photomode::EnvLayer layer)
        {
            LLEnvironment& env = LLEnvironment::instance();
            switch (layer)
            {
                case plus::photomode::EnvLayer::Water: return env.getCurrentWater().get();
                case plus::photomode::EnvLayer::Day:   return env.getCurrentDay().get();
                default:                               return env.getCurrentSky().get();
            }
        }

        // The layer of a preset drop-down (by its name in floater_plus_quickprefs.xml).
        plus::photomode::EnvLayer layerOf(const LLComboBox* combo)
        {
            const std::string& name = combo->getName();
            return name == "WaterPresetsCombo" ? plus::photomode::EnvLayer::Water
                 : name == "DCPresetsCombo"    ? plus::photomode::EnvLayer::Day
                                               : plus::photomode::EnvLayer::Sky;
        }

        // The preset of a drop-down on: the last one chosen of its layer wins (chosen fast with the arrow keys, an
        // older one fetched longer would come after it).
        void selectEnvironmentAsset(LLComboBox* combo)
        {
            LLEnvironment& env = LLEnvironment::instance();
            env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
            plus::photomode::setLocalEnvironmentAsset(combo->getSelectedValue().asUUID(), layerOf(combo),
                                                      plus::photomode::EnvTransition::Default);
        }
    }

    QuickPrefsFloater::QuickPrefsFloater(const LLSD& key) :
        LLFloater(key)
    {
    }

    QuickPrefsFloater::~QuickPrefsFloater() = default;

    LLControlVariable* QuickPrefsFloater::findSetting(const std::string& name)
    {
        return findAnySetting(name);
    }

    bool QuickPrefsFloater::postBuild()
    {
        mBtnResetDefaults    = getChild<LLButton>("Restore_Btn");
        mAvatarZOffsetSlider = getChild<LLSliderCtrl>("HoverHeightSlider");
        mAvatarZOffsetSlider->setMinValue(MIN_HOVER_Z);
        mAvatarZOffsetSlider->setMaxValue(MAX_HOVER_Z);

        mWaterPresetsCombo    = getChild<LLComboBox>("WaterPresetsCombo");
        mWLPresetsCombo       = getChild<LLComboBox>("WLPresetsCombo");
        mDayCyclePresetsCombo = getChild<LLComboBox>("DCPresetsCombo");

        // Environment presets
        mWLPresetsCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetCombo(mWLPresetsCombo); });
        mWaterPresetsCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetCombo(mWaterPresetsCombo); });
        mDayCyclePresetsCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetCombo(mDayCyclePresetsCombo); });
        getChild<LLUICtrl>("WLPrevPreset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetStep(mWLPresetsCombo, false); });
        getChild<LLUICtrl>("WLNextPreset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetStep(mWLPresetsCombo, true); });
        getChild<LLUICtrl>("WWPrevPreset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetStep(mWaterPresetsCombo, false); });
        getChild<LLUICtrl>("WWNextPreset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetStep(mWaterPresetsCombo, true); });
        getChild<LLUICtrl>("DCPrevPreset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetStep(mDayCyclePresetsCombo, false); });
        getChild<LLUICtrl>("DCNextPreset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetStep(mDayCyclePresetsCombo, true); });
        getChild<LLUICtrl>("ResetToRegionDefault")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickResetToRegionDefault(); });

        mEnvChangedConnection = LLEnvironment::instance().setEnvironmentChanged([this](LLEnvironment::EnvSelection_t, S32) { setSelectedEnvironment(); });
        // a preset that could not be had leaves the environment as it was: the drop-down goes back to the one in use
        mEnvFailedConnection = plus::photomode::setLocalEnvironmentFailedCallback([this]() { setSelectedEnvironment(); });

        // Hover height
        mAvatarZOffsetSlider->setSliderMouseUpCallback([this](LLUICtrl*, const LLSD&) { onAvatarZOffsetFinalCommit(); });
        mAvatarZOffsetSlider->setSliderEditorCommitCallback([this](LLUICtrl*, const LLSD&) { onAvatarZOffsetFinalCommit(); });
        mAvatarZOffsetSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAvatarZOffsetSliderMoved(); });
        if (LLControlVariable* hover = gSavedPerAccountSettings.getControl("AvatarHoverOffsetZ"))
        {
            mHoverSettingConnection = hover->getCommitSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { syncAvatarZOffsetFromPreferenceSetting(); });
        }
        else
        {
            LL_WARNS("Plus") << "Quick Preferences: no AvatarHoverOffsetZ setting" << LL_ENDL;
        }
        mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });

        // The list
        mOptionsStack = getChild<LLLayoutStack>("options_stack");

        mControlLabelEdit        = getChild<LLLineEditor>("label_edit");
        mControlNameCombo        = getChild<LLComboBox>("control_name_combo");
        mControlTypeCombo        = getChild<LLComboBox>("control_type_combo_box");
        mControlIntegerCheckbox  = getChild<LLCheckBoxCtrl>("control_integer_checkbox");
        mControlMinSpinner       = getChild<LLSpinCtrl>("control_min_edit");
        mControlMaxSpinner       = getChild<LLSpinCtrl>("control_max_edit");
        mControlIncrementSpinner = getChild<LLSpinCtrl>("control_increment_edit");

        auto changed = [this](LLUICtrl*, const LLSD&) { onValuesChanged(); };
        mControlLabelEdit->setCommitCallback(changed);
        mControlNameCombo->setCommitCallback(changed);
        mControlTypeCombo->setCommitCallback(changed);
        mControlIntegerCheckbox->setCommitCallback(changed);
        mControlMinSpinner->setCommitCallback(changed);
        mControlMaxSpinner->setCommitCallback(changed);
        mControlIncrementSpinner->setCommitCallback(changed);

        getChild<LLButton>("move_up_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMoveUpClicked(); });
        getChild<LLButton>("move_down_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMoveDownClicked(); });
        getChild<LLButton>("add_new_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddNewClicked(); });
        mBtnResetDefaults->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRestoreDefaults(); });

        if (LLControlVariable* edit_mode = gSavedSettings.getControl(SETTING_EDIT_MODE))
        {
            mEditModeConnection = edit_mode->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { onEditModeChanged(); });
        }
        else
        {
            LL_WARNS("Plus") << "Quick Preferences: no " << SETTING_EDIT_MODE << " setting" << LL_ENDL;
        }

        // IndirectMaxNonImpostors is a helper setting that the standard list uses.
        if (LLControlVariable* indirect = gSavedSettings.getControl("IndirectMaxNonImpostors"))
        {
            mNonImpostorsConnection = indirect->getCommitSignal()->connect([](LLControlVariable*, const LLSD& newvalue, const LLSD&) { updateMaxNonImpostors(newvalue); });
        }
        // And back: the real setting changed elsewhere (a graphics preset, the performance window's auto-tune) while this
        // is open. Its default 0 is "not made yet", outside the slider's 1..66 (66 = no limit): made again from the real
        // setting, which never gives 0. No loop: the way back writes the same real value, which fires nothing.
        if (LLControlVariable* real = gSavedSettings.getControl("RenderAvatarMaxNonImpostors"))
        {
            mRealNonImpostorsConnection = real->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) {
                LLAvatarComplexityControls::setIndirectMaxNonImpostors();
            });
        }

        loadList(getSettingsPath(false));

        // Every debug setting can be chosen, except those hidden in the debug settings window
        // and the floaters' positions, sizes and visibility.
        struct AddName : public LLControlGroup::ApplyFunctor
        {
            LLComboBox* combo;
            explicit AddName(LLComboBox* c) : combo(c) {}
            void apply(const std::string& name, LLControlVariable* control) override
            {
                if (!control->isHiddenFromSettingsEditor() && name.find("floater_") != 0)
                {
                    combo->addSimpleElement(name);
                }
            }
        } add_name(mControlNameCombo);
        gSavedSettings.applyToAll(&add_name);
        gSavedPerAccountSettings.applyToAll(&add_name);
        mControlNameCombo->sortByName();

        updateAvatarZOffsetEditEnabled();
        onRegionChanged();

        return LLFloater::postBuild();
    }

    void QuickPrefsFloater::onOpen(const LLSD& key)
    {
        loadPresets();
        setSelectedEnvironment();

        // IndirectMaxNonImpostors is made from the real setting only when the preferences open.
        LLAvatarComplexityControls::setIndirectMaxNonImpostors();

        setEditMode(false);

        // Sliders and some other widgets do not follow their setting while hidden: show the values again.
        for (const auto& [name, entry] : mControlsList)
        {
            if (!entry.widget)
            {
                continue;
            }
            if (LLControlVariable* var = entry.widget->getControlVariable() ? entry.widget->getControlVariable() : findSetting(name))
            {
                LLSD val = var->getValue();
                if (entry.type == ControlTypeRadio && val.isBoolean())
                {
                    val = LLSD(val.asInteger());
                }
                entry.widget->setValue(val);
            }
        }
        LLFloater::onOpen(key);
    }

    void QuickPrefsFloater::onClose(bool app_quitting)
    {
        // Leaving the edit mode saves the list.
        setEditMode(false);
        LLFloater::onClose(app_quitting);
    }

    void QuickPrefsFloater::setEditMode(bool edit)
    {
        gSavedSettings.setBOOL(SETTING_EDIT_MODE, edit);
    }

    // ---- The list ---------------------------------------------------------------------------

    std::string QuickPrefsFloater::getSettingsPath(bool save_mode) const
    {
        std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, USER_LIST_FILE);
        // Without an own list the standard one is read.
        if (!save_mode && !LLFile::isfile(path))
        {
            path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", "quickprefs", "quick_preferences.xml");
        }
        return path;
    }

    void QuickPrefsFloater::loadList(const std::string& path)
    {
        QuickPrefsXML xml;
        LLXMLNodePtr  root;

        // fix-round101: the XUI reader recurses (a list nested about 900 deep overflowed the stack)
        if (!plus::xmlNestingOk(path))
        {
            LL_WARNS("Plus") << "Quick Preferences: " << path << " is nested too deep; not read" << LL_ENDL;
            return;
        }
        if (!LLXMLNode::parseFile(path, root, nullptr))
        {
            LL_WARNS("Plus") << "Quick Preferences: cannot read " << path << LL_ENDL;
            return;
        }
        if (!root->hasName("quickprefs"))
        {
            LL_WARNS("Plus") << "Quick Preferences: " << path << " is not a list file" << LL_ENDL;
            return;
        }

        LLXUIParser parser;
        parser.readXUI(root, xml, path);
        if (!xml.validateBlock())
        {
            LL_WARNS("Plus") << "Quick Preferences: cannot validate " << path << LL_ENDL;
            return;
        }

        for (const QuickPrefsXMLEntry& xml_entry : xml.entries)
        {
            const std::string name = xml_entry.control_name;
            if (hasControl(name))
            {
                continue; // twice in the file
            }
            if (!findSetting(name))
            {
                // Settings of another viewer, or removed ones: nothing to show.
                LL_INFOS("Plus") << "Quick Preferences: no setting " << name << ", left out" << LL_ENDL;
                continue;
            }

            std::string label = xml_entry.label;
            std::string translation_id;
            if (xml_entry.translation_id.isProvided())
            {
                // Kept even when this language has no such string, so that saving does not lose it.
                translation_id = xml_entry.translation_id();
                if (hasString(translation_id))
                {
                    label = getString(translation_id);
                }
            }

            const U32 type = xml_entry.control_type;
            if (addControl(name, label, nullptr, type <= ControlTypeColor4 ? (ControlType)type : ControlTypeText,
                           xml_entry.integer.isProvided() && xml_entry.integer(),
                           xml_entry.min_value, xml_entry.max_value, xml_entry.increment))
            {
                mControlsList[name].translation_id = translation_id;
                mControlsOrder.push_back(name);
            }
        }
        mDirty = false;
    }

    void QuickPrefsFloater::saveList()
    {
        QuickPrefsXML xml;
        for (const std::string& name : mControlsOrder)
        {
            auto it = mControlsList.find(name);
            if (it == mControlsList.end() || !findSetting(name))
            {
                continue; // a new row that was never given a setting
            }
            const ControlEntry& entry = it->second;
            QuickPrefsXMLEntry  xml_entry;
            xml_entry.control_name = name;
            xml_entry.label        = entry.label;
            if (!entry.translation_id.empty())
            {
                xml_entry.translation_id = entry.translation_id;
            }
            xml_entry.control_type = (U32)entry.type;
            xml_entry.integer      = entry.integer;
            xml_entry.min_value    = entry.min_value;
            xml_entry.max_value    = entry.max_value;
            xml_entry.increment    = entry.increment;
            xml.entries.add(xml_entry);
        }

        LLXMLNodePtr output_node = new LLXMLNode("quickprefs", false);
        LLXUIParser  parser;
        parser.writeXUI(output_node, xml);

        const std::string path = getSettingsPath(true);
        if (output_node->isNull())
        {
            return;
        }
        // Written next to the file, then moved over it: a crash while saving leaves the old list.
        const plus::FileWrite wrote = plus::replaceFile(path, [&](const std::string& temp)
        {
            LLFILE* fp = LLFile::fopen(temp, WRITE_MODE);
            if (!fp)
            {
                return plus::FileWrite::cannot_open;
            }
            LLXMLNode::writeHeaderToFile(fp);
            output_node->writeToFile(fp);
            const bool written = !ferror(fp);
            return fclose(fp) != 0 || !written ? plus::FileWrite::cannot_write : plus::FileWrite::written;
        });
        if (wrote == plus::FileWrite::cannot_replace)
        {
            LL_WARNS("Plus") << "Quick Preferences: cannot replace " << path << LL_ENDL;
            return;
        }
        if (wrote != plus::FileWrite::written)
        {
            LL_WARNS("Plus") << "Quick Preferences: cannot write " << plus::tempPathFor(path) << LL_ENDL;
            return;
        }
        mDirty = false;
    }

    void QuickPrefsFloater::clearList()
    {
        selectControl("");
        const std::list<std::string> names = mControlsOrder;
        for (const std::string& name : names)
        {
            removeControl(name);
        }
        mControlsOrder.clear();
    }

    void QuickPrefsFloater::updateControl(const std::string& name, ControlEntry& entry)
    {
        // The row's panel is named after the setting, to find it again from the callbacks.
        entry.panel->setName(name);

        // Only the widget of the chosen type is used; the others stay hidden.
        for (ControlType type : ALL_TYPES)
        {
            if (entry.type != type)
            {
                if (LLUICtrl* other = entry.panel->findChild<LLUICtrl>(widgetName(type)))
                {
                    other->setControlName(SETTING_EDIT_MODE); // detaches it from the old setting
                    other->setVisible(false);
                    other->setEnabled(false);
                }
            }
        }

        LLUICtrl* widget = entry.panel->getChild<LLUICtrl>(widgetName(entry.type));
        entry.widget     = widget;
        widget->setControlName(name);
        widget->setVisible(true);
        widget->setEnabled(true);

        // Bounds a hand-edited list made infinite ("1e39") or not a number: within +-1e9 (NaN: 0), so the
        // step, the rounding and the widgets get finite numbers.
        constexpr F32 BOUND = 1.0e9f;
        entry.min_value = std::isnan(entry.min_value) ? 0.f : llclamp(entry.min_value, -BOUND, BOUND);
        entry.max_value = std::isnan(entry.max_value) ? 0.f : llclamp(entry.max_value, -BOUND, BOUND);
        // A range the wrong way round (typed in the editor, or a hand-edited list) becomes empty.
        entry.max_value = llmax(entry.max_value, entry.min_value);

        // No step given: slider 1/100 of the range, spinner 1/20.
        const bool stepped = entry.type == ControlTypeSlider || entry.type == ControlTypeSpinner;
        if (stepped && (entry.increment <= 0.0f || !std::isfinite(entry.increment)))
        {
            const F32 range = entry.max_value - entry.min_value;
            entry.increment = range / (entry.type == ControlTypeSlider ? 100.0f : 20.0f);
            if (entry.increment <= 0.0f || !std::isfinite(entry.increment))
            {
                // An empty range still needs a step: LLSlider::setValue() takes fmod() by it,
                // and a zero step would write NaN into the setting.
                entry.increment = 1.0f;
            }
        }

        S32 decimals = 3;
        if (entry.integer)
        {
            // rounded as floats: ll_round() goes through S32, so a step or bound past 2^31 in a hand-edited
            // list became -2147483648 (a negative step, or a range of that one value written to the setting)
            entry.min_value = std::round(entry.min_value);
            entry.max_value = std::round(entry.max_value);
            entry.increment = std::round(entry.increment);
            if (entry.increment == 0.f)
            {
                entry.increment = 1.f;
            }
            entry.max_value = llmax(entry.max_value, entry.min_value);
            decimals = 0;
        }

        LLUICtrl* alpha_widget = entry.panel->getChild<LLUICtrl>("option_color_alpha_control");
        alpha_widget->setVisible(false);

        if (entry.type == ControlTypeSpinner)
        {
            LLSpinCtrl* spinner = static_cast<LLSpinCtrl*>(widget);
            spinner->setPrecision(decimals);
            spinner->setMinValue(entry.min_value);
            spinner->setMaxValue(entry.max_value);
            spinner->setIncrement(entry.increment);
        }
        else if (entry.type == ControlTypeSlider)
        {
            LLSliderCtrl* slider = static_cast<LLSliderCtrl*>(widget);
            slider->setPrecision(decimals);
            slider->setMinValue(entry.min_value);
            slider->setMaxValue(entry.max_value);
            slider->setIncrement(entry.increment);
        }
        else if (entry.type == ControlTypeColor4)
        {
            LLColorSwatchCtrl* color_widget = static_cast<LLColorSwatchCtrl*>(widget);
            alpha_widget->setVisible(true);
            alpha_widget->setValue(color_widget->get().mV[VALPHA]);
        }

        // A new row gets its label and buttons wired once.
        LLTextBox* label_textbox = entry.label_textbox;
        if (!label_textbox)
        {
            label_textbox = entry.panel->getChild<LLTextBox>("option_label");
            LLPanel* panel = entry.panel;
            label_textbox->setDoubleClickCallback([this, panel](LLUICtrl*, S32, S32, MASK) { onDoubleClickLabel(panel); });
            label_textbox->setMouseUpCallback([this, panel](LLUICtrl*, S32, S32, MASK) { onClickLabel(panel); });

            entry.panel->getChild<LLButton>("remove_button")->setCommitCallback([this, panel](LLUICtrl*, const LLSD&) { onRemoveClicked(panel); });

            LLColorSwatchCtrl* color4 = entry.panel->getChild<LLColorSwatchCtrl>("option_color4_control");
            alpha_widget->setCommitCallback([this, color4](LLUICtrl* ctrl, const LLSD&) { onAlphaChanged(ctrl, color4); });

            entry.label_textbox = label_textbox;
        }
        label_textbox->setValue(entry.label + ":");

        if (LLControlVariable* var = findSetting(name))
        {
            LLSD val = var->getValue();
            if (entry.type == ControlTypeRadio && val.isBoolean())
            {
                val = LLSD(val.asInteger());
            }
            widget->setValue(val);

            // A radio pair works with 0 and 1: follow a Boolean setting by hand.
            entry.signal_connection.disconnect();
            if (entry.type == ControlTypeRadio && var->type() == TYPE_BOOLEAN)
            {
                entry.signal_connection = var->getSignal()->connect(
                    [handle = widget->getHandle()](LLControlVariable*, const LLSD& newvalue, const LLSD&)
                    {
                        if (LLUICtrl* w = handle.get())
                        {
                            w->setValue(LLSD(newvalue.asInteger()));
                        }
                    });
            }
        }
        else
        {
            LL_WARNS("Plus") << "Quick Preferences: no setting " << name << LL_ENDL;
        }
    }

    LLUICtrl* QuickPrefsFloater::addControl(const std::string& name, const std::string& label, LLView* slot, ControlType type, bool integer,
                                            F32 min_value, F32 max_value, F32 increment)
    {
        LLLayoutPanel* panel = LLUICtrlFactory::createFromFile<LLLayoutPanel>(ITEM_PANEL_FILE, nullptr, LLLayoutStack::child_registry_t::instance());
        if (!panel)
        {
            LL_WARNS("Plus") << "Quick Preferences: cannot build " << ITEM_PANEL_FILE << LL_ENDL;
            return nullptr;
        }

        if (max_value < min_value)
        {
            max_value = min_value;
        }
        if (increment < 0.0f)
        {
            increment = 0.0f; // updateControl() works one out
        }

        ControlEntry entry;
        entry.panel     = panel->getChild<LLPanel>("option_ordering_panel");
        entry.label     = label;
        entry.type      = type;
        entry.integer   = integer;
        entry.min_value = min_value;
        entry.max_value = max_value;
        entry.increment = increment;

        updateControl(name, entry);
        mControlsList[name] = entry;

        if (slot)
        {
            // An old row's place: the new ordering panel goes in, the new layout panel is not needed.
            slot->addChild(entry.panel);
            entry.panel->setOrigin(0, 0);
            entry.panel->reshape(slot->getRect().getWidth(), slot->getRect().getHeight());
            delete panel;
        }
        else
        {
            mOptionsStack->addPanel(panel, LLLayoutStack::NO_ANIMATE);
            mOrderingSlots.push_back(panel);
            reshape(getRect().getWidth(), getRect().getHeight() + panel->getRect().getHeight());
            panel->setVisible(true);
        }

        entry.panel->setBorderVisible(false);
        mDirty = true;
        return entry.widget;
    }

    void QuickPrefsFloater::removeControl(const std::string& name, bool remove_slot)
    {
        auto it = mControlsList.find(name);
        if (it == mControlsList.end())
        {
            LL_WARNS("Plus") << "Quick Preferences: no row " << name << LL_ENDL;
            return;
        }

        LLPanel* panel  = it->second.panel;
        const S32 height = panel->getRect().getHeight();

        it->second.signal_connection.disconnect();
        mControlsList.erase(it);

        LLLayoutPanel* slot = static_cast<LLLayoutPanel*>(panel->getParent());
        slot->removeChild(panel);
        delete panel;

        if (remove_slot)
        {
            deleteSlot(slot, height);
        }
        mDirty = true;
    }

    // Takes a row's place out of the stack and frees it with its resize bar (LLLayoutStack::removeChild()
    // only unlinks both). Called from the row's own remove button too: that is safe, as the button holds
    // the mouse capture (the up event comes straight to it, not down the view tree) and LLButton commits
    // last, touching nothing after; the ordering panel holding the button was deleted that way before.
    void QuickPrefsFloater::deleteSlot(LLLayoutPanel* slot, S32 height)
    {
        mOrderingSlots.remove(slot);
        LLResizeBar* bar = slot->getResizeBar();
        mOptionsStack->removeChild(slot); // unlinks the bar too and clears the panel's pointer to it
        delete bar;
        delete slot;
        reshape(getRect().getWidth(), getRect().getHeight() - height);
    }

    void QuickPrefsFloater::selectControl(const std::string& name)
    {
        // Take the mark off the row that was selected.
        if (!mSelectedControl.empty() && hasControl(mSelectedControl))
        {
            mControlsList[mSelectedControl].panel->setBorderVisible(false);
        }

        // The setting tells the editor widgets (enabled_control) whether a row is selected.
        mSelectedControl = name;
        gSavedSettings.setString(SETTING_SELECTED, name);

        if (!mSelectedControl.empty() && !hasControl(mSelectedControl))
        {
            mSelectedControl.clear();
            return;
        }
        if (!gSavedSettings.getBOOL(SETTING_EDIT_MODE))
        {
            return;
        }

        mControlNameCombo->selectNthItem(0);
        bool enable_floating_point = false;

        if (!mSelectedControl.empty())
        {
            const ControlEntry& entry = mControlsList[mSelectedControl];
            entry.panel->setBorderVisible(true);

            mControlLabelEdit->setValue(LLSD(entry.label));
            mControlNameCombo->setValue(LLSD(mSelectedControl));
            mControlTypeCombo->setValue(LLSD((S32)entry.type));
            mControlIntegerCheckbox->setValue(LLSD(entry.integer));
            mControlMinSpinner->setValue(LLSD(entry.min_value));
            mControlMaxSpinner->setValue(LLSD(entry.max_value));
            mControlIncrementSpinner->setValue(LLSD(entry.increment));

            if (entry.type == ControlTypeSpinner || entry.type == ControlTypeSlider)
            {
                enable_floating_point = true;
                mControlIncrementSpinner->setIncrement(entry.integer ? 1.0f : 0.1f);
                const S32 decimals = entry.integer ? 0 : 3;
                mControlMinSpinner->setPrecision(decimals);
                mControlMaxSpinner->setPrecision(decimals);
                mControlIncrementSpinner->setPrecision(decimals);
            }
        }

        mControlMinSpinner->setEnabled(enable_floating_point);
        mControlMaxSpinner->setEnabled(enable_floating_point);
        mControlIntegerCheckbox->setEnabled(enable_floating_point);
        mControlIncrementSpinner->setEnabled(enable_floating_point);
    }

    void QuickPrefsFloater::onClickLabel(LLPanel* panel)
    {
        if (gSavedSettings.getBOOL(SETTING_EDIT_MODE))
        {
            selectControl(panel->getName());
        }
    }

    void QuickPrefsFloater::onDoubleClickLabel(LLPanel* panel)
    {
        const bool edit = !gSavedSettings.getBOOL(SETTING_EDIT_MODE);
        setEditMode(edit);
        if (edit)
        {
            selectControl(panel->getName());
        }
    }

    void QuickPrefsFloater::onEditModeChanged()
    {
        if (gSavedSettings.getBOOL(SETTING_EDIT_MODE))
        {
            return;
        }
        // Leaving the edit mode: no row selected any more, and the list is saved if it changed.
        selectControl("");
        if (mDirty)
        {
            saveList();
        }
    }

    void QuickPrefsFloater::onValuesChanged()
    {
        if (!gSavedSettings.getBOOL(SETTING_EDIT_MODE) || mSelectedControl.empty())
        {
            return;
        }

        const std::string old_control_name = mSelectedControl;
        const std::string new_control_name = mControlNameCombo->getValue().asString();

        if (!new_control_name.empty() && old_control_name != new_control_name)
        {
            // Another setting for the row: build the row again.
            if (hasControl(new_control_name))
            {
                LL_WARNS("Plus") << "Quick Preferences: " << new_control_name << " is in the list already" << LL_ENDL;
                LLStringUtil::format_map_t args;
                args["[NAME]"] = new_control_name;
                LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", getString("qp_duplicate_control", args)));
                mControlNameCombo->setValue(LLSD(old_control_name)); // back to the row's own setting
                return;
            }

            const ControlEntry old_parameters = mControlsList[mSelectedControl];
            selectControl(""); // the border would point to a row that is gone
            for (std::string& name : mControlsOrder)
            {
                if (name == old_control_name)
                {
                    name = new_control_name;
                    break;
                }
            }

            LLView* slot = old_parameters.panel->getParent();
            removeControl(old_control_name, false);
            if (!addControl(new_control_name, new_control_name, slot))
            {
                // The row could not be built again: it goes, with its empty place.
                mControlsOrder.remove(new_control_name);
                if (LLLayoutPanel* layout = dynamic_cast<LLLayoutPanel*>(slot))
                {
                    deleteSlot(layout, layout->getRect().getHeight());
                }
                return;
            }
            selectControl(new_control_name);
            mControlsList[mSelectedControl].label          = old_parameters.label;
            mControlsList[mSelectedControl].translation_id = old_parameters.translation_id;

            if (LLControlVariable* var = findSetting(mSelectedControl); var && hasControl(mSelectedControl))
            {
                // Range and widget that suit the setting, so that its value is not destroyed.
                F32       min_value = 0.0f;
                F32       max_value = 1.0f;
                const F32 value     = (F32)var->getValue().asReal();
                if (value < 0.0f)
                {
                    min_value = value * 2.0f;
                }
                else if (value > 0.0f)
                {
                    max_value = value * 2.0f;
                }

                ControlType type;
                switch (var->type())
                {
                    case TYPE_BOOLEAN:
                        min_value = 0.0f;
                        max_value = 1.0f;
                        type      = ControlTypeRadio;
                        break;
                    case TYPE_COL3:
                        type = ControlTypeColor3;
                        break;
                    case TYPE_COL4:
                        type = ControlTypeColor4;
                        break;
                    case TYPE_U32:
                        min_value = 0.0f;
                        type      = ControlTypeSlider;
                        break;
                    case TYPE_S32:
                    case TYPE_F32:
                        type = ControlTypeSlider;
                        break;
                    default:
                        type = ControlTypeText;
                        break;
                }

                ControlEntry& entry = mControlsList[mSelectedControl];
                F32 increment = 0.1f;
                if (type == ControlTypeSlider)
                {
                    increment = (max_value - min_value) / 100.0f;
                }
                else if (type == ControlTypeSpinner)
                {
                    increment = (max_value - min_value) / 20.0f;
                }
                if (increment < 0.1f)
                {
                    increment = 0.1f;
                }

                entry.min_value = min_value;
                entry.max_value = max_value;
                entry.increment = increment;
                entry.type      = type;
                entry.integer   = var->type() == TYPE_S32 || var->type() == TYPE_U32;
            }

            updateControl(mSelectedControl, mControlsList[mSelectedControl]);
        }
        else if (hasControl(mSelectedControl))
        {
            // The same setting: only the values changed.
            ControlEntry& entry = mControlsList[mSelectedControl];
            const std::string label = mControlLabelEdit->getValue().asString();
            if (label != entry.label)
            {
                // A label of the user's own: it no longer follows the UI language.
                entry.label = label;
                entry.translation_id.clear();
            }
            entry.type      = (ControlType)mControlTypeCombo->getValue().asInteger();
            entry.integer   = mControlIntegerCheckbox->getValue().asBoolean();
            entry.min_value = (F32)mControlMinSpinner->getValue().asReal();
            entry.max_value = llmax((F32)mControlMaxSpinner->getValue().asReal(), entry.min_value);
            entry.increment = (F32)mControlIncrementSpinner->getValue().asReal();
            updateControl(mSelectedControl, entry);
        }

        mDirty = true;
        selectControl(mSelectedControl);
    }

    void QuickPrefsFloater::onAddNewClicked()
    {
        // A name that is not a setting: the row waits for its setting to be chosen.
        static S32 sCount = 0;
        const std::string new_control_name = "NewControl" + llformat("%d", sCount++);
        if (!addControl(new_control_name, getString("qp_new_label")))
        {
            return;
        }
        mControlsOrder.push_back(new_control_name);
        selectControl(new_control_name);
    }

    void QuickPrefsFloater::onRemoveClicked(LLPanel* panel)
    {
        const std::string name = panel->getName(); // the panel is deleted below
        selectControl("");
        mControlsOrder.remove(name);
        removeControl(name);
        setFocus(true);
    }

    void QuickPrefsFloater::onAlphaChanged(LLUICtrl* ctrl, LLColorSwatchCtrl* color_swatch)
    {
        LLColor4 color = color_swatch->get();
        color.setAlpha((F32)ctrl->getValue().asReal());
        if (LLControlVariable* var = color_swatch->getControlVariable())
        {
            var->setValue(color.getValue());
        }
    }

    void QuickPrefsFloater::swapControls(const std::string& control1, const std::string& control2)
    {
        const ControlEntry entry_1 = mControlsList[control1];
        const ControlEntry entry_2 = mControlsList[control2];

        LLView* slot_1 = entry_1.panel->getParent();
        LLView* slot_2 = entry_2.panel->getParent();

        slot_1->addChild(entry_2.panel);
        slot_2->addChild(entry_1.panel);
        mDirty = true;
    }

    void QuickPrefsFloater::onMoveUpClicked()
    {
        for (auto it = mControlsOrder.begin(); it != mControlsOrder.end(); ++it)
        {
            if (*it == mSelectedControl)
            {
                if (it == mControlsOrder.begin())
                {
                    return;
                }
                auto previous = it;
                --previous;
                *it       = *previous;
                *previous = mSelectedControl;
                swapControls(mSelectedControl, *it);
                return;
            }
        }
    }

    void QuickPrefsFloater::onMoveDownClicked()
    {
        for (auto it = mControlsOrder.begin(); it != mControlsOrder.end(); ++it)
        {
            if (*it == mSelectedControl)
            {
                auto next = it;
                ++next;
                if (next == mControlsOrder.end())
                {
                    return;
                }
                *it   = *next;
                *next = mSelectedControl;
                swapControls(mSelectedControl, *it);
                return;
            }
        }
    }

    void QuickPrefsFloater::onClickRestoreDefaults()
    {
        auto handle = getDerivedHandle<QuickPrefsFloater>();
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", getString("qp_restore_confirm")), LLSD(),
                                 [handle](const LLSD& notification, const LLSD& response)
                                 {
                                     QuickPrefsFloater* self = handle.get();
                                     if (!self || LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                                     {
                                         return;
                                     }
                                     self->clearList();
                                     LLFile::remove(self->getSettingsPath(true));
                                     self->loadList(self->getSettingsPath(false));
                                     self->setEditMode(false);
                                 });
    }

    // ---- Environment presets ----------------------------------------------------------------

    bool QuickPrefsFloater::isValidPreset(const LLSD& preset)
    {
        if (preset.isUUID())
        {
            return preset.asUUID().notNull();
        }
        if (preset.isString())
        {
            const std::string name = preset.asString();
            return !name.empty() && name != PRESET_NAME_REGION && name != PRESET_NAME_DAY_CYCLE && name != PRESET_NAME_NONE;
        }
        return false;
    }

    // Moves the selection to the next (or previous) entry that can be chosen, wrapping round; the
    // entries that only show the state (disabled) and the separators are passed. False if there is none.
    bool QuickPrefsFloater::stepComboBox(LLComboBox* combo, bool forward)
    {
        const S32 count = combo->getItemCount();
        const S32 start = combo->getCurrentIndex();
        const LLSD before = combo->getSelectedValue();
        S32 index = start;

        for (S32 n = 0; n < count; ++n)
        {
            index += forward ? 1 : -1;
            if (index < 0)
            {
                index = count - 1;
            }
            else if (index >= count)
            {
                index = 0;
            }
            if (combo->setCurrentByIndex(index) && isValidPreset(combo->getSelectedValue()))
            {
                return true;
            }
        }
        combo->selectByValue(before);
        return false;
    }

    void QuickPrefsFloater::fillPresetCombo(LLComboBox* combo, const std::multimap<std::string, LLUUID>& presets, bool day_cycle)
    {
        combo->operateOnAll(LLComboBox::OP_DELETE);
        combo->add(getString("qp_region_default"), LLSD(PRESET_NAME_REGION), EAddPosition::ADD_BOTTOM, false);
        if (day_cycle)
        {
            combo->add(getString("qp_none"), LLSD(PRESET_NAME_NONE), EAddPosition::ADD_BOTTOM, false);
        }
        else
        {
            combo->add(getString("qp_day_cycle_based"), LLSD(PRESET_NAME_DAY_CYCLE), EAddPosition::ADD_BOTTOM, false);
        }
        combo->addSeparator();

        for (const auto& [preset_name, asset_id] : presets)
        {
            if (!preset_name.empty())
            {
                combo->add(preset_name, LLSD(asset_id));
            }
        }
    }

    void QuickPrefsFloater::loadPresets()
    {
        LLInventoryModel::cat_array_t  cats;
        LLInventoryModel::item_array_t items;
        SettingsCollector              collector;
        gInventory.collectDescendentsIf(LLUUID::null, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);

        std::multimap<std::string, LLUUID> sky_map;
        std::multimap<std::string, LLUUID> water_map;
        std::multimap<std::string, LLUUID> daycycle_map;

        for (const auto& item : items)
        {
            switch (LLSettingsType::fromInventoryFlags(item->getFlags()))
            {
                case LLSettingsType::ST_SKY:
                    sky_map.emplace(item->getName(), item->getAssetUUID());
                    break;
                case LLSettingsType::ST_WATER:
                    water_map.emplace(item->getName(), item->getAssetUUID());
                    break;
                case LLSettingsType::ST_DAYCYCLE:
                    daycycle_map.emplace(item->getName(), item->getAssetUUID());
                    break;
                default:
                    LL_WARNS("Plus") << "Quick Preferences: unknown kind of settings item " << item->getName() << LL_ENDL;
                    break;
            }
        }

        fillPresetCombo(mWaterPresetsCombo, water_map, false);
        fillPresetCombo(mWLPresetsCombo, sky_map, false);
        fillPresetCombo(mDayCyclePresetsCombo, daycycle_map, true);
    }

    void QuickPrefsFloater::setSelectedEnvironment()
    {
        LLEnvironment& env = LLEnvironment::instance();

        // The entries that only show the state (shared environment, day cycle based, none) are disabled
        // so they cannot be clicked, and a disabled entry cannot be selected: enabled while this selects.
        auto set_state_entries_enabled = [this](bool enabled) {
            for (LLComboBox* combo : { mWLPresetsCombo, mWaterPresetsCombo, mDayCyclePresetsCombo })
            {
                for (const char* name : { PRESET_NAME_REGION, PRESET_NAME_DAY_CYCLE, PRESET_NAME_NONE })
                {
                    combo->setEnabledByValue(LLSD(name), enabled);
                }
            }
        };
        set_state_entries_enabled(true);

        // The row each shows now: two rows of the same preset (a copy of another name) would go back to the first
        // one when selected by value, and the next step would go from there.
        constexpr size_t COMBOS = static_cast<size_t>(plus::photomode::EnvLayer::Count);   // one a layer
        LLComboBox* const combos[COMBOS] = { mWLPresetsCombo, mWaterPresetsCombo, mDayCyclePresetsCombo };
        S32  shown_index[COMBOS];
        LLSD shown_value[COMBOS];
        for (size_t i = 0; i < COMBOS; ++i)
        {
            shown_index[i] = combos[i]->getCurrentIndex();
            shown_value[i] = combos[i]->getSelectedValue();
        }

        mWLPresetsCombo->selectByValue(LLSD(PRESET_NAME_REGION));
        mWaterPresetsCombo->selectByValue(LLSD(PRESET_NAME_REGION));
        mDayCyclePresetsCombo->selectByValue(LLSD(PRESET_NAME_REGION));

        if (env.getSelectedEnvironment() == LLEnvironment::ENV_LOCAL)
        {
            // A day cycle, a fixed sky and a fixed water may all be set. A fixed sky or water
            // overrides the one of the day cycle, so the day cycle is checked first.
            if (LLSettingsDay::ptr_t day = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL))
            {
                if (day->getAssetId().notNull())
                {
                    mDayCyclePresetsCombo->selectByValue(LLSD(day->getAssetId()));
                    mWLPresetsCombo->selectByValue(LLSD(PRESET_NAME_DAY_CYCLE));
                    mWaterPresetsCombo->selectByValue(LLSD(PRESET_NAME_DAY_CYCLE));
                }
            }
            else
            {
                mDayCyclePresetsCombo->selectByValue(LLSD(PRESET_NAME_NONE));
            }

            if (LLSettingsSky::ptr_t sky = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL))
            {
                if (sky->getAssetId().notNull())
                {
                    mWLPresetsCombo->selectByValue(LLSD(sky->getAssetId()));
                }
            }
            if (LLSettingsWater::ptr_t water = env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL))
            {
                if (water->getAssetId().notNull())
                {
                    mWaterPresetsCombo->selectByValue(LLSD(water->getAssetId()));
                }
            }
        }

        // A preset chosen and still coming stays chosen (the environment changes meanwhile, and the arrow keys go on
        // from it); one that could not be had is not (the one in use, above).
        for (LLComboBox* combo : { mWLPresetsCombo, mWaterPresetsCombo, mDayCyclePresetsCombo })
        {
            const plus::photomode::EnvLayer layer = layerOf(combo);
            const LLUUID wanted = plus::photomode::localEnvironmentWanted(layer);
            // only when it has a row here (chosen elsewhere, e.g. the photo screen, it may have none: a miss would
            // leave the drop-down with nothing chosen)
            if (plus::photomode::localEnvironmentFetch(layer) == plus::photomode::EnvFetch::Pending
                && combo->findItemByValue(wanted.asString()))
            {
                combo->selectByValue(LLSD(wanted));
            }
        }
        for (size_t i = 0; i < COMBOS; ++i)
        {
            if (shown_index[i] >= 0 && combos[i]->getCurrentIndex() != shown_index[i]
                && combos[i]->getSelectedValue().asString() == shown_value[i].asString())
            {
                combos[i]->setCurrentByIndex(shown_index[i]);
            }
        }
        set_state_entries_enabled(false);
    }

    void QuickPrefsFloater::putOnPreset(LLComboBox* combo)
    {
        // the row before asking: a preset that cannot be had may fail at once, and the drop-down goes back to the
        // one in use (setSelectedEnvironment()) before this goes on
        const S32 index = combo->getCurrentIndex();
        selectEnvironmentAsset(combo);
        const plus::photomode::EnvLayer layer = layerOf(combo);
        AskedPreset& asked = mAsked[static_cast<size_t>(layer)];
        asked.changes = plus::photomode::localEnvironmentChanges(layer);   // after it: the count it made
        asked.index   = index;
        asked.inUse   = inUseOf(layer);     // after it: the one before while it comes (the same when it failed)
    }

    void QuickPrefsFloater::placeAtFailedPreset(LLComboBox* combo, bool forward)
    {
        const plus::photomode::EnvLayer layer = layerOf(combo);
        const AskedPreset& asked = mAsked[static_cast<size_t>(layer)];
        if (!forward || plus::photomode::localEnvironmentFetch(layer) != plus::photomode::EnvFetch::Failed
            || asked.changes != plus::photomode::localEnvironmentChanges(layer) || asked.inUse != inUseOf(layer)
            || asked.index < 0 || asked.index >= combo->getItemCount())
        {
            return;
        }
        // the row asked for, while it still holds the preset that could not be had
        const std::vector<LLScrollListItem*> rows = combo->getAllData();
        if (rows[asked.index]->getValue().asString() == plus::photomode::localEnvironmentWanted(layer).asString())
        {
            combo->setCurrentByIndex(asked.index);
        }
    }

    void QuickPrefsFloater::onPresetCombo(LLComboBox* combo)
    {
        // The entries that only show the state cannot be clicked; a typed or scrolled-to one is passed on. The arrow
        // keys of the drop-down move it before this (LLComboBox commit_on_arrow): they go on from the row shown, the
        // one in use, so after a preset that could not be had they ask for it again (as a click on it does); the
        // step buttons go on past it (onPresetStep()).
        if (isValidPreset(combo->getSelectedValue()) || stepComboBox(combo, true))
        {
            putOnPreset(combo);
        }
        else
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", getString("qp_no_valid_env")));
        }
    }

    void QuickPrefsFloater::onPresetStep(LLComboBox* combo, bool forward)
    {
        placeAtFailedPreset(combo, forward);    // forward: go on past a preset that could not be had
        if (stepComboBox(combo, forward))
        {
            putOnPreset(combo);
        }
        else
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", getString("qp_no_valid_env")));
        }
    }

    void QuickPrefsFloater::onClickResetToRegionDefault()
    {
        mWLPresetsCombo->setValue(LLSD(PRESET_NAME_REGION));
        mWaterPresetsCombo->setValue(LLSD(PRESET_NAME_REGION));
        for (plus::photomode::EnvLayer layer : { plus::photomode::EnvLayer::Sky, plus::photomode::EnvLayer::Water,
                                                 plus::photomode::EnvLayer::Day })
        {
            plus::photomode::forgetLocalEnvironmentAsset(layer);   // a preset still coming would not show anyway
        }
        LLEnvironment::instance().setSharedEnvironment();
    }

    // ---- Hover height -----------------------------------------------------------------------

    void QuickPrefsFloater::onAvatarZOffsetSliderMoved()
    {
        if (!isAgentAvatarValid())
        {
            return;
        }
        const F32 value = mAvatarZOffsetSlider->getValueF32();
        if (mAvatarZOffsetSlider->isMouseHeldDown())
        {
            // Dragging: the avatar follows at once; the setting is written at the end.
            gAgentAvatarp->setHoverOffset(LLVector3(0.0f, 0.0f, llclamp(value, MIN_HOVER_Z, MAX_HOVER_Z)), false);
        }
        else
        {
            gSavedPerAccountSettings.setF32("AvatarHoverOffsetZ", value);
        }
    }

    void QuickPrefsFloater::onAvatarZOffsetFinalCommit()
    {
        if (gSavedPerAccountSettings.controlExists("AvatarHoverOffsetZ"))
        {
            gSavedPerAccountSettings.setF32("AvatarHoverOffsetZ", mAvatarZOffsetSlider->getValueF32());
        }
    }

    void QuickPrefsFloater::updateAvatarZOffsetEditEnabled()
    {
        const bool enabled = gAgent.getRegion() && gAgent.getRegion()->avatarHoverHeightEnabled();
        mAvatarZOffsetSlider->setEnabled(enabled);
        if (enabled)
        {
            syncAvatarZOffsetFromPreferenceSetting();
        }
    }

    void QuickPrefsFloater::onRegionChanged()
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (region && region->simulatorFeaturesReceived())
        {
            updateAvatarZOffsetEditEnabled();
        }
        else if (region)
        {
            auto handle = getDerivedHandle<QuickPrefsFloater>();
            region->setSimulatorFeaturesReceivedCallback(
                [handle](const LLUUID& region_id, LLViewerRegion*)
                {
                    LLViewerRegion* current = gAgent.getRegion();
                    if (QuickPrefsFloater* self = handle.get(); self && current && current->getRegionID() == region_id)
                    {
                        self->updateAvatarZOffsetEditEnabled();
                    }
                });
        }
    }

    void QuickPrefsFloater::syncAvatarZOffsetFromPreferenceSetting()
    {
        if (gSavedPerAccountSettings.controlExists("AvatarHoverOffsetZ"))
        {
            mAvatarZOffsetSlider->setValue(gSavedPerAccountSettings.getF32("AvatarHoverOffsetZ"), false);
        }
    }

    // ---- IndirectMaxNonImpostors ------------------------------------------------------------

    void QuickPrefsFloater::updateMaxNonImpostors(const LLSD& newvalue)
    {
        // Makes RenderAvatarMaxNonImpostors from the slider's number (0 = no limit at the top end).
        U32 value = newvalue.asInteger();
        if (0 == value || LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER <= value)
        {
            value = 0;
        }
        gSavedSettings.setU32("RenderAvatarMaxNonImpostors", value);
        LLVOAvatar::updateImpostorRendering(value); // effective at once
    }
}
