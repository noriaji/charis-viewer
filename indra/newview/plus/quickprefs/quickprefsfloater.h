/**
 * @file quickprefsfloater.h
 * @brief Quick Preferences (ported from Firestorm, plus/quickprefs): a small
 *        window with the settings the user picks, and the environment presets.
 *
 * Based on Firestorm's quickprefs.h.
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

#ifndef PLUS_QUICKPREFS_QUICKPREFSFLOATER_H
#define PLUS_QUICKPREFS_QUICKPREFSFLOATER_H

#include "llfloater.h"
#include "llsd.h"

#include <boost/signals2.hpp>
#include <list>
#include <map>
#include <string>

class LLButton;
class LLColorSwatchCtrl;
class LLComboBox;
class LLCheckBoxCtrl;
class LLControlVariable;
class LLFloaterReg;
class LLLayoutPanel;
class LLLayoutStack;
class LLLineEditor;
class LLPanel;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;

namespace plus::quickprefs
{
    // The widget a setting is shown with. The numbers are what the list file stores.
    enum ControlType
    {
        ControlTypeCheckbox = 0,
        ControlTypeText,
        ControlTypeSpinner,
        ControlTypeSlider,
        ControlTypeRadio,
        ControlTypeColor3,
        ControlTypeColor4
    };

    class QuickPrefsFloater : public LLFloater
    {
        friend class ::LLFloaterReg;

    public:
        static constexpr const char* NAME = "plus_quickprefs";

        bool postBuild() override;
        void onOpen(const LLSD& key) override;
        void onClose(bool app_quitting) override;

    private:
        explicit QuickPrefsFloater(const LLSD& key);
        ~QuickPrefsFloater() override;

        // One row of the list.
        struct ControlEntry
        {
            LLPanel*  panel          = nullptr; // option_ordering_panel inside the row's layout panel
            LLUICtrl* widget         = nullptr; // the widget of the chosen type
            LLTextBox* label_textbox = nullptr;
            std::string label;
            std::string translation_id; // the label's string in the floater, kept so the saved list follows the UI language
            ControlType type         = ControlTypeRadio;
            bool        integer      = false;
            F32         min_value    = -1000000.0f;
            F32         max_value    = 1000000.0f;
            F32         increment    = 0.0f;
            boost::signals2::connection signal_connection;
        };

        // The list (settings)
        std::string getSettingsPath(bool save_mode) const;
        void        loadList(const std::string& path);
        void        saveList();
        void        clearList();
        static LLControlVariable* findSetting(const std::string& name);

        LLUICtrl* addControl(const std::string& name, const std::string& label, LLView* slot = nullptr, ControlType type = ControlTypeRadio,
                             bool integer = false, F32 min_value = -1000000.0f, F32 max_value = 1000000.0f, F32 increment = 0.0f);
        void removeControl(const std::string& name, bool remove_slot = true);
        void deleteSlot(LLLayoutPanel* slot, S32 height);
        void updateControl(const std::string& name, ControlEntry& entry);
        void selectControl(const std::string& name);
        void swapControls(const std::string& control1, const std::string& control2);
        bool hasControl(const std::string& name) const { return mControlsList.find(name) != mControlsList.end(); }

        void onDoubleClickLabel(LLPanel* panel);
        void onClickLabel(LLPanel* panel);
        void onEditModeChanged();
        void onValuesChanged();
        void onAddNewClicked();
        void onRemoveClicked(LLPanel* panel);
        void onAlphaChanged(LLUICtrl* ctrl, LLColorSwatchCtrl* color_swatch);
        void onMoveUpClicked();
        void onMoveDownClicked();
        void onClickRestoreDefaults();
        void setEditMode(bool edit);

        // Environment presets
        void loadPresets();
        void fillPresetCombo(LLComboBox* combo, const std::multimap<std::string, LLUUID>& presets, bool day_cycle);
        void setSelectedEnvironment();
        static bool isValidPreset(const LLSD& preset);
        static bool stepComboBox(LLComboBox* combo, bool forward);
        void onPresetCombo(LLComboBox* combo);
        void onPresetStep(LLComboBox* combo, bool forward);
        void onClickResetToRegionDefault();

        // Hover height
        void onAvatarZOffsetSliderMoved();
        void onAvatarZOffsetFinalCommit();
        void updateAvatarZOffsetEditEnabled();
        void onRegionChanged();
        void syncAvatarZOffsetFromPreferenceSetting();

        // IndirectMaxNonImpostors (a list entry) drives RenderAvatarMaxNonImpostors.
        static void updateMaxNonImpostors(const LLSD& newvalue);

        LLComboBox*   mWLPresetsCombo        = nullptr;
        LLComboBox*   mWaterPresetsCombo     = nullptr;
        LLComboBox*   mDayCyclePresetsCombo  = nullptr;
        LLSliderCtrl* mAvatarZOffsetSlider   = nullptr;
        LLButton*     mBtnResetDefaults      = nullptr;

        LLLayoutStack* mOptionsStack = nullptr; // where the rows go

        // The rows by setting name, their order on the screen, and the layout panels they are in.
        std::map<std::string, ControlEntry> mControlsList;
        std::list<std::string>              mControlsOrder;
        std::list<LLLayoutPanel*>           mOrderingSlots;

        std::string mSelectedControl; // the row being edited
        bool        mDirty = false;   // the list changed since it was read or saved

        // Editor widgets
        LLLineEditor*   mControlLabelEdit        = nullptr;
        LLComboBox*     mControlNameCombo        = nullptr;
        LLComboBox*     mControlTypeCombo        = nullptr;
        LLCheckBoxCtrl* mControlIntegerCheckbox  = nullptr;
        LLSpinCtrl*     mControlMinSpinner       = nullptr;
        LLSpinCtrl*     mControlMaxSpinner       = nullptr;
        LLSpinCtrl*     mControlIncrementSpinner = nullptr;

        boost::signals2::scoped_connection mEditModeConnection;
        boost::signals2::scoped_connection mEnvChangedConnection;
        boost::signals2::scoped_connection mRegionChangedConnection;
        boost::signals2::scoped_connection mHoverSettingConnection;
        boost::signals2::scoped_connection mNonImpostorsConnection;
        boost::signals2::scoped_connection mRealNonImpostorsConnection;
    };
}

#endif // PLUS_QUICKPREFS_QUICKPREFSFLOATER_H
