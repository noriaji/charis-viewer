/**
 * @file photorecipe.cpp
 * @brief Settings written and given back, and the one-press recipe "sunbeams
 *        through clouds" (research/253-cloud-light-shafts.md 9 in the
 *        sl-viewer repository).
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

// Why the recipe is a switch made again from the values and not a saved flag: it is shown in three places
// (the photo tools' row, the photo screen's switches, the check of Preferences > Charis) and its settings are changed
// in many more (the rows of each, the quality levels, Preferences, saved graphics presets, the compare
// button). A saved flag would drift from what the picture has; the values never do.
//
// Nothing is polled: the switch follows the signals of its settings, once on the next idle for a burst of
// changes (a quality level writes a hundred settings).

#include "llviewerprecompiledheaders.h"

#include "photorecipe.h"

#include "plus.h"
#include "plus/presets/presets.h"
#include "plus/safemode/safemode.h"
#include "plus/safemode/safemode_table.h"

#include <cmath>
#include <functional>
#include <iterator>

#include "llcallbacklist.h"
#include "llsdutil.h"
#include "llviewercontrol.h"

namespace plus::photomode
{
    bool writeRemembered(SettingWrites& writes, const std::string& key, const LLSD& value)
    {
        LLControlVariable* control = gSavedSettings.getControl(key);
        if (!control)
        {
            return false;
        }
        const auto found  = writes.find(key);
        const LLSD before = found != writes.end() ? found->second.before : control->getValue();
        control->setValue(value);
        writes[key] = { before, control->getValue() }; // as the control holds it
        return true;
    }

    bool stillAsWritten(const std::string& key, const SettingWrite& write)
    {
        LLControlVariable* control = gSavedSettings.getControl(key);
        return control && llsd_equals(control->getValue(), write.wrote);
    }

    bool changedSince(const std::string& key, const SettingWrite& write)
    {
        LLControlVariable* control = gSavedSettings.getControl(key);
        return control && !llsd_equals(control->getValue(), write.wrote);
    }

    void giveBack(const std::string& key, const SettingWrite& write)
    {
        if (stillAsWritten(key, write))
        {
            gSavedSettings.getControl(key)->setValue(write.before);
        }
    }

    namespace
    {
        constexpr char SETTING_SWITCH[]  = "PlusCloudShaftsScene";       // not saved: made again from the values
        constexpr char SETTING_BEFORE[]  = "PlusCloudShaftsSceneBefore"; // what the switch wrote, and what was before
        constexpr char SETTING_SHAFTS[]  = "PlusCloudShafts";            // cloud-shafts-core (research/253 7)
        constexpr char SETTING_LEVEL[]   = "RenderQualityPerformance";   // official: Preferences' quality slider

        // One setting of the recipe: on (a check box), or at least `least` (the larger of it and the value now).
        struct Step
        {
            const char* key;
            F64         least;
            bool        level;  // the quality levels decide it (featuretable.txt): after a level, the level's value stands
        };

        // research/253 9.2. A setting that does not exist (a branch not merged) is left out, written or tested.
        constexpr Step STEPS[] = {
            { "PlusVolumetricClouds",   1.0, true  },   // the clouds' shadow map comes from them
            { "PlusCloudShadow",        0.7, false },   // 0 makes no shadow map
            { SETTING_SHAFTS,           1.5, false },   // the sky's haze 1.5 times over, as shafts
            { "PlusVolumetric",         1.0, true  },   // the pass that draws them
            { "PlusVolumetricStrength", 1.5, false },   // the near shafts (between trees) shown too
            { "PlusVolumetricQuality",  2.0, true  },   // medium: low is too coarse for the far shafts
        };

        constexpr F64 SAME = 1e-4;  // a slider's F32 against the step's value

        SettingWrites sWrites;          // what the switch wrote that still holds it (PlusCloudShaftsSceneBefore)
        SettingWrites sWritesAsOpened;  // sWrites as the photo tools opened (rememberRecipesAsOpened)
        LLSD          sWritesAside;     // a safe start: the file's entries for what it sets (safeStartSets), written back as they were
        int           sHolds = 0;       // SwitchHold
        bool          sSyncing = false; // the switch is being set from the values
        bool          sSyncQueued = false;
        bool          sLevelQueued = false;

        const Step* findStep(const std::string& key)
        {
            for (const Step& step : STEPS)
            {
                if (key == step.key)
                {
                    return &step;
                }
            }
            return nullptr;
        }

        bool met(const Step& step)
        {
            LLControlVariable* control = gSavedSettings.getControl(step.key);
            return !control || control->getValue().asReal() >= step.least - SAME;
        }

        // All of the recipe is on; with `level_only`, the settings of it that the quality levels decide.
        bool stepsMet(bool level_only = false)
        {
            for (const Step& step : STEPS)
            {
                if ((step.level || !level_only) && !met(step))
                {
                    return false;
                }
            }
            return true;
        }

        LLSD valueOf(LLControlVariable& control, F64 value)
        {
            if (control.isType(TYPE_BOOLEAN))
            {
                return LLSD(value > 0.5);
            }
            if (control.isType(TYPE_F32))
            {
                return LLSD((LLSD::Real)value);
            }
            return LLSD((LLSD::Integer)ll_round(value));
        }

        // --- What the switch wrote, kept over restarts ------------------------------

        // A setting a safe start sets for its run only (safemode_table.h SETTINGS: the volumetric clouds, the stars).
        bool safeStartSets(const std::string& key)
        {
            const std::wstring name(key.begin(), key.end());     // the table's names are plain ASCII words
            for (const plus::safemode::Switch& setting : plus::safemode::SETTINGS)
            {
                if (name == setting.name)
                {
                    return true;
                }
            }
            return false;
        }

        // A saved value the switch could have written: a number (a check box, a slider, a choice).
        bool plainNumber(const LLSD& value)
        {
            return (value.isBoolean() || value.isInteger() || value.isReal()) && std::isfinite(value.asReal());
        }

        void saveWrites()
        {
            LLControlVariable* control = gSavedSettings.getControl(SETTING_BEFORE);
            if (!control)
            {
                return;
            }
            LLSD saved = LLSD::emptyMap();
            for (LLSD::map_const_iterator it = sWritesAside.beginMap(); it != sWritesAside.endMap(); ++it)
            {
                if (!sWrites.count(it->first))
                {
                    saved[it->first] = it->second;
                }
            }
            for (const auto& [key, write] : sWrites)
            {
                LLSD entry;
                entry["before"] = write.before;
                entry["wrote"]  = write.wrote;
                saved[key]      = entry;
            }
            control->setValue(saved);
        }

        // Only the recipe's own settings and plain numbers are taken from the file (a settings.xml edited by
        // hand names no other setting to write). One changed since it was written is the user's now.
        void loadWrites()
        {
            LLControlVariable* control = gSavedSettings.getControl(SETTING_BEFORE);
            const LLSD saved = control ? control->getValue() : LLSD();
            if (!saved.isMap())
            {
                return;
            }
            for (const Step& step : STEPS)
            {
                const LLSD entry = saved.get(step.key);
                LLControlVariable* setting = gSavedSettings.getControl(step.key);
                if (!setting || !entry.isMap() || !plainNumber(entry.get("before")) || !plainNumber(entry.get("wrote")))
                {
                    continue;
                }
                // `before` as the setting's own type (a check box gets a bool back, not a number)
                const SettingWrite write = { valueOf(*setting, entry.get("before").asReal()), entry.get("wrote") };
                if (stillAsWritten(step.key, write))
                {
                    sWrites[step.key] = write;
                }
                else if (plus::safemode::active() && safeStartSets(step.key))
                {
                    sWritesAside[step.key] = entry;     // not the user's change: the safe start's, for this run only
                }
            }
            // A safe start sets the volumetric clouds off for this run only (plus/safemode, not saved): what the
            // switch wrote for them is not dropped from the file, the next normal start finds it as written. Every
            // later save of this run (the switch off, "give all back") writes those entries back too. An entry the user
            // changed since is theirs, as in a normal start: the next save leaves it out.
            if (sWrites.size() != (size_t)saved.size() && !plus::safemode::active())
            {
                saveWrites();
            }
        }

        // Drops what the user has changed since it was written: theirs, not the switch's to give back.
        void forgetChangedSince()
        {
            for (auto it = sWrites.begin(); it != sWrites.end();)
            {
                it = changedSince(it->first, it->second) ? sWrites.erase(it) : std::next(it);
            }
        }

        void giveAllBack()
        {
            for (const auto& [key, write] : sWrites)
            {
                giveBack(key, write);
            }
            sWrites.clear();
            saveWrites();
        }

        // --- The switch ----------------------------------------------------------------

        void putOn()
        {
            forgetChangedSince();
            for (const Step& step : STEPS)
            {
                LLControlVariable* control = gSavedSettings.getControl(step.key);
                if (control && !met(step))
                {
                    writeRemembered(sWrites, step.key, valueOf(*control, step.least));
                }
            }
            saveWrites();
            LL_INFOS("Plus") << "Sunbeams through clouds on (" << sWrites.size() << " settings written)" << LL_ENDL;
        }

        void takeOff()
        {
            giveAllBack();
            // Nothing given back keeps it all on when it was all on before the switch: off is then the shafts off.
            if (stepsMet())
            {
                if (LLControlVariable* shafts = gSavedSettings.getControl(SETTING_SHAFTS))
                {
                    shafts->setValue(shafts->getDefault());
                }
            }
            LL_INFOS("Plus") << "Sunbeams through clouds off" << LL_ENDL;
        }

        void sync()
        {
            sSyncQueued = false;
            LLControlVariable* control = gSavedSettings.getControl(SETTING_SWITCH);
            const bool on = stepsMet();
            if (control && control->getValue().asBoolean() != on)
            {
                sSyncing = true;
                control->setValue(LLSD(on));
                sSyncing = false;
            }
        }

        void queueSync()
        {
            if (!sSyncQueued)
            {
                sSyncQueued = true;
                doOnIdleOneTime(&sync);
            }
        }

        // While one is held, changes of the switch are not taken as the user's.
        struct SwitchHold
        {
            SwitchHold() { ++sHolds; }
            ~SwitchHold()
            {
                --sHolds;
                queueSync();
            }

            SwitchHold(const SwitchHold&)            = delete;
            SwitchHold& operator=(const SwitchHold&) = delete;
        };

        void onSwitch(bool on)
        {
            if (sSyncing || sHolds > 0)
            {
                return;
            }
            if (on)
            {
                putOn();
            }
            else
            {
                takeOff();
            }
            queueSync();
        }

        // research/253 9.4: a quality level that turned off the recipe's quality settings is what the user asked
        // for. The rest the switch wrote (the shafts, the shadow, the density) goes back; the level's own stand.
        void checkAfterLevel()
        {
            sLevelQueued = false;
            if (sWrites.empty() || stepsMet(true))
            {
                return; // nothing written, or the level left the recipe's quality settings on
            }
            for (const auto& [key, write] : sWrites)
            {
                const Step* step = findStep(key);
                if (step && !step->level)
                {
                    giveBack(key, write);
                }
            }
            sWrites.clear();
            saveWrites();
            LL_INFOS("Plus") << "Sunbeams through clouds: off with the quality level" << LL_ENDL;
        }

        void queueLevelCheck()
        {
            if (!sLevelQueued)
            {
                sLevelQueued = true;
                doOnIdleOneTime(&checkAfterLevel); // after the level's values are all written
            }
        }

        void watch(const char* key, const std::function<void(const LLSD&)>& changed)
        {
            if (LLControlVariable* control = gSavedSettings.getControl(key))
            {
                control->getSignal()->connect([changed](LLControlVariable*, const LLSD& value, const LLSD&) { changed(value); });
            }
        }

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusCloudShaftsScene, PlusCloudShaftsSceneBefore); the switch's
            // rows: catalog.xml, photodetail_left.cpp / photodetail_right.cpp, panel_preferences_charis.xml.
            loadWrites();
            watch(SETTING_SWITCH, [](const LLSD& value) { onSwitch(value.asBoolean()); });
            for (const Step& step : STEPS)
            {
                watch(step.key, [](const LLSD&) { queueSync(); });
            }
            watch(SETTING_LEVEL, [](const LLSD&) { queueLevelCheck(); });
            plus::presets::addGraphicsLevelListener([](U32) { queueLevelCheck(); });
            sync();
        }

        plus::FeatureRegistration sRegistration({ "photorecipe", &init });
    }

    void rememberRecipesAsOpened()
    {
        sWritesAsOpened = sWrites;
    }

    void letGoOfRecipes(const std::function<void()>& putRowsBack)
    {
        // Written after the tools opened: back. What they opened with the rows put back (one that is not a
        // row holds it still).
        for (const auto& [key, write] : sWrites)
        {
            const auto opened = sWritesAsOpened.find(key);
            if (opened == sWritesAsOpened.end() || !llsd_equals(opened->second.wrote, write.wrote))
            {
                giveBack(key, write);
            }
        }
        sWrites.clear();
        {
            SwitchHold hold;
            putRowsBack();
        }
        // What the tools opened with and holds it now is the switch's still: off gives it back.
        for (const auto& [key, write] : sWritesAsOpened)
        {
            if (stillAsWritten(key, write))
            {
                sWrites[key] = write;
            }
        }
        saveWrites();
    }
}
