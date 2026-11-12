/**
 * @file photomode.cpp
 * @brief Photo mode: the look sliders and presets, the sky and water picker
 *        and the gaze hook-up (research/photo-mode-ui.md in the
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

// Why the presets are not the graphics presets of plus/presets: those set the
// picture's quality and cost (shadows, volumetrics, ... and some of them rebuild
// the pipeline, "the viewer stands still for a few seconds"). These set how the
// picture looks, through settings that are plain uniforms, and must switch
// without a hitch. So they are a separate set, with their own label
// (PlusPhotoPreset), and they leave the quality alone. That holds for the settings a
// preset sets as they are (Preset::settings) too: none that the quality levels decide
// (the "render" ones of plus/presets/official_mid_scope.txt, whose values come from
// featuretable.txt), so no shadow distance or penumbra, which also make the sun's
// shadow maps again (research/249 2.3, review of photo-preset-autumn).
//
// Nothing here is kept besides the label: the sliders read the viewer's own
// settings back (value()), so what the card shows is what the picture has.

#include "llviewerprecompiledheaders.h"

#include "photocamera.h"
#include "photoguide.h"
#include "photokeys.h"
#include "photomode.h"
#include "photorecipe.h"
#include "photosky.h"
#include "phototools.h"
#include "phototoolsfloater.h"

#include "plus.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "llagent.h"
#include "llapp.h"
#include "llassetstorage.h"
#include "llcircuit.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsettingssky.h"
#include "llsettingsvo.h"
#include "llsettingswater.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvirtualtrackball.h"

namespace plus::photomode
{
    namespace
    {
        constexpr char PRESETS_FILE[] = "plus/photomode/presets.xml"; // in app_settings/

        // The settings the sliders write.
        constexpr char SETTING_EXPOSURE[]  = "RenderExposure";
        constexpr char SETTING_FNUMBER[]   = "CameraFNumber";
        constexpr char SETTING_DOF[]       = "RenderDepthOfField";
        constexpr char SETTING_LOOK[]      = "PlusLook";
        constexpr char SETTING_LOOK_STR[]  = "PlusLookStrength";
        constexpr char SETTING_AMBIENT[]   = "RenderSkyAmbientScale";
        constexpr char SETTING_TONEMAP[]   = "RenderTonemapMix";
        constexpr char SETTING_GLARE[]     = "PlusGlare";
        constexpr char SETTING_PRESET[]    = "PlusPhotoPreset";
        constexpr char SETTING_GAZE[]      = "PlusGazeMode";  // plus/gaze
        constexpr char SETTING_GAZE_EVERYONE[] = "PlusGazeAllowEveryone"; // plus/gaze: shows "Everyone"

        // Tint: the shipped looks the slider blends from.
        constexpr char LOOK_COOL[] = "cool-morning";
        constexpr char LOOK_WARM[] = "warm-afternoon";

        constexpr float F_NUMBER_WIDE = 1.4f;  // blur 100
        constexpr float F_NUMBER_NARROW = 22.f; // blur 0
        constexpr float AMBIENT_DEFAULT = 1.0f; // RenderSkyAmbientScale's own default (settings.xml); shadow 50

        constexpr ParamRange RANGES[PARAM_COUNT] = {
            { -100.f, 100.f }, // brightness
            { 0.f, 100.f },    // blur
            { -100.f, 100.f }, // tint
            { 0.f, 360.f },    // sun
            { 0.f, 100.f },    // shadow
            { 0.f, 100.f },    // contrast
            { 0.f, 3.f },      // glare
        };

        constexpr const char* PARAM_NAMES[PARAM_COUNT] = {
            "brightness", "blur", "tint", "sun", "shadow", "contrast", "glare"
        };

        // Provisional values: they are to be tuned looking at the target picture
        // (backlit crossing, research/photo-mode-ui.md), and can be replaced
        // without a build by app_settings/plus/photomode/presets.xml.
        //                  brightness blur  tint  sun   shadow contrast glare
        const std::vector<Preset>& builtinPresets()
        {
            static const std::vector<Preset> presets = {
                { "portrait",  { 10.f,  75.f,  20.f,  30.f, 35.f, 45.f, 1.f } },
                { "backlight", { 15.f,  50.f,  10.f, 180.f, 40.f, 60.f, 3.f } },
                { "sunset",    {  0.f,  50.f,  60.f, 150.f, 55.f, 65.f, 2.f } },
                { "night",     { 25.f,  40.f, -40.f, UNSET, 60.f, 70.f, 3.f } },
                { "studio",    {  5.f,  85.f,   0.f,  60.f, 45.f, 55.f, 0.f } },
                { "natural",   {  0.f,  32.f,   0.f, UNSET, 50.f, 50.f, 1.f } },
                // research/249 2.3: the Autumn Alley look with clarity and texture, weak glare, halation and
                // vignette, shadows soft enough for dappled light. No tint (the look is set as it is), no sun (the
                // low afternoon sky is chosen from the sky list) and nothing of the quality (see the top).
                { "autumn",    {  0.f, UNSET, UNSET, UNSET, 55.f, 55.f, 1.f },
                  { { "PlusLook",           LLSD("autumn-street") },
                    { "PlusLookStrength",   LLSD(100) },
                    { "PlusFxPreset",       LLSD("") },     // of the sliders below, Halation and Vignette work only without a preset (clarity and texture do either way)
                    { "PlusFxStrength",     LLSD(100) },
                    { "PlusFxClarity",      LLSD(35) },
                    { "PlusFxTexture",      LLSD(20) },
                    { "PlusFxHalation",     LLSD(10) },
                    { "PlusFxVignette",     LLSD(15) },
                    { "PlusShadowSoftness", LLSD(2) } } },  // normal: the gaps between leaves stay (a taste, not a level)
            };
            return presets;
        }

        // The control is missing before plus' settings are declared, or when a branch
        // that declares it is not merged: leave such a setting alone.
        bool has(const char* name)
        {
            return gSavedSettings.controlExists(name);
        }

        // --- What the presets set besides the sliders (Preset::settings) -----

        // The settings of the presets' own lists as they were before the first preset wrote them (their defaults
        // for a preset of an earlier session, presetWrites()), and what was written. For this session only.
        SettingWrites sPresetWrites;

        // A preset's value and a setting's: the same once each is read as the setting's type would read it.
        bool sameValue(const LLSD& current, const LLSD& wanted)
        {
            if (current.isString() || wanted.isString())
            {
                return current.asString() == wanted.asString();
            }
            return std::fabs(current.asReal() - wanted.asReal()) < 1e-4;
        }

        // What the presets wrote. A preset still in use from an earlier session (PlusPhotoPreset is saved) wrote
        // its settings then, and what they were before is gone: each one it names that still holds its value goes
        // back to the setting's default. Read once, on first use.
        SettingWrites& presetWrites()
        {
            static bool sSeeded = false;
            if (!sSeeded)
            {
                sSeeded = true;
                if (const Preset* last = findPreset(currentPreset()))
                {
                    for (const auto& [key, value] : last->settings)
                    {
                        LLControlVariable* control = gSavedSettings.getControl(key);
                        if (control && sameValue(control->getValue(), value))
                        {
                            sPresetWrites[key] = { control->getDefault(), control->getValue() };
                        }
                    }
                }
            }
            return sPresetWrites;
        }

        // True when a preset wrote `value` into the setting and it is still so: `value` becomes what was before.
        bool presetPutThere(const char* key, LLSD& value)
        {
            const SettingWrites& writes = presetWrites();
            const auto found = writes.find(key);
            if (found == writes.end() || !sameValue(found->second.wrote, value))
            {
                return false;
            }
            value = found->second.before;
            return true;
        }

        // --- What the settings were before the card changed them -----------

        std::map<std::string, LLSD> sOriginals;
        bool sOriginalsTaken = false;

        void takeOriginals()
        {
            if (sOriginalsTaken)
            {
                return;
            }
            sOriginalsTaken = true;
            for (const char* name : { SETTING_EXPOSURE, SETTING_FNUMBER, SETTING_LOOK, SETTING_LOOK_STR, SETTING_AMBIENT,
                                      SETTING_TONEMAP, SETTING_GLARE })
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    sOriginals[name] = control->getValue();
                }
            }
        }

        // The tint's "none" is the look the user had. sOriginals has it as it was when the card first changed
        // something, but the look can be chosen since (the Charis menu, the look list of the photo tools):
        // a look the tint slider did not put there is the user's, and is what "none" goes back to.
        struct LookValue
        {
            std::string look;
            U32         strength = 0;
        };
        LookValue sTintWrote;          // what the tint slider last put on PlusLook / PlusLookStrength
        bool      sTintWroteAny = false;

        LookValue currentLook()
        {
            return { gSavedSettings.getString(SETTING_LOOK), gSavedSettings.getU32(SETTING_LOOK_STR) };
        }

        void writeTint(const LookValue& value)
        {
            gSavedSettings.setString(SETTING_LOOK, value.look);
            gSavedSettings.setU32(SETTING_LOOK_STR, value.strength);
            sTintWrote    = value;
            sTintWroteAny = true;
        }

        // Called before "none" is used (needs has(SETTING_LOOK) && has(SETTING_LOOK_STR), and takeOriginals()).
        void followUserLook()
        {
            const LookValue now = currentLook();
            if (sTintWroteAny && now.look == sTintWrote.look && now.strength == sTintWrote.strength)
            {
                return; // still the slider's
            }
            if (now.look == LOOK_COOL || now.look == LOOK_WARM)
            {
                return; // one of the tint's own looks, chosen by name: the slider shows it as a tint, not as "none"
            }
            // a look a preset put there (still as it put it) is no choice of the user's: what was before it is
            LLSD look     = now.look;
            LLSD strength = (LLSD::Integer)now.strength;
            if (presetPutThere(SETTING_LOOK, look))
            {
                presetPutThere(SETTING_LOOK_STR, strength);
            }
            sOriginals[SETTING_LOOK]     = look.asString();
            sOriginals[SETTING_LOOK_STR] = strength.asInteger();
        }

        LookValue originalLook()
        {
            return { sOriginals[SETTING_LOOK].asString(), (U32)sOriginals[SETTING_LOOK_STR].asInteger() };
        }

        // The settings the sliders write that need nothing else (not the tint, whose "none" is the look the
        // user had, and not the sun).
        void writePlain(Param param, float v)
        {
            switch (param)
            {
            case Param::Brightness:
                gSavedSettings.setF32(SETTING_EXPOSURE, exp2f(v / 100.f));
                break;
            case Param::Blur:
                // The aperture only; depth of field itself is the user's choice (blurAvailable()).
                gSavedSettings.setF32(SETTING_FNUMBER,
                                      expf(logf(F_NUMBER_NARROW) + (logf(F_NUMBER_WIDE) - logf(F_NUMBER_NARROW)) * v / 100.f));
                break;
            case Param::Shadow:
                gSavedSettings.setF32(SETTING_AMBIENT, AMBIENT_DEFAULT * exp2f((50.f - v) / 50.f));
                break;
            case Param::Contrast:
                gSavedSettings.setF32(SETTING_TONEMAP, 0.4f + 0.6f * v / 100.f);
                break;
            case Param::Glare:
                if (has(SETTING_GLARE))
                {
                    gSavedSettings.setS32(SETTING_GLARE, (S32)ll_round(v));
                }
                break;
            default:
                break;
            }
        }

        // The slider value that writes no change (research/272): fixed points of each formula above, not the
        // settings' own shipped defaults (CameraFNumber's, 9.0, is a plain camera default for outside photo
        // mode, not "no blur"; Brightness, Shadow and Contrast's defaults do land on the settings' shipped
        // values, but by the formula, not by reading them back).
        constexpr float PARAM_DEFAULTS[PARAM_COUNT] = {
            0.f,    // brightness: exp2(0) == 1 (RenderExposure's own default)
            0.f,    // blur: the narrowest aperture (no blur; blurAvailable() still gates it)
            0.f,    // tint: none
            UNSET,  // sun: a direction, no "no change" point
            50.f,   // shadow: AMBIENT_DEFAULT * exp2(0) == AMBIENT_DEFAULT (RenderSkyAmbientScale's own default)
            50.f,   // contrast: 0.4 + 0.6 * 0.5 == 0.7 (RenderTonemapMix's own default)
            0.f,    // glare: PlusGlare's own default (off)
        };

        // --- The sun: the personal (local) sky -----------------------------

        LLSettingsSky::ptr_t   sLiveSky;
        LLSettingsWater::ptr_t sLiveWater;
        float                  sSunAngle = UNSET;

        // The personal sky while it is the land's day cycle taken along by a water (copyLandDay()):
        // still that one means the user has not chosen or changed a sky since. Any sky put on, the sun
        // slider and the official editors put another object there.
        // Only on the land it was taken from: on another region or parcel the land's own sky is
        // another, and this one would be shown as the land's ("now: the land's sky") - daySky().
        std::weak_ptr<LLSettingsSky> sDaySky;

        struct LandPlace
        {
            U64 region = 0;
            S32 parcel = INVALID_PARCEL_ID;
            bool operator==(const LandPlace& other) const { return region == other.region && parcel == other.parcel; }
        };
        LandPlace sDaySkyPlace;

        LandPlace landPlace()
        {
            LandPlace place;
            if (const LLViewerRegion* region = gAgent.getRegion())
            {
                place.region = region->getHandle();
            }
            place.parcel = LLViewerParcelMgr::getInstance()->getAgentParcelId();
            return place;
        }

        void setDaySky(const LLSettingsSky::ptr_t& sky, const LandPlace& place)
        {
            sDaySky      = sky;
            sDaySkyPlace = place;
        }

        // sDaySky while the avatar is still on the land it was taken from; forgotten once it is not.
        LLSettingsSky::ptr_t daySky()
        {
            if (!(sDaySkyPlace == landPlace()))
            {
                sDaySky.reset();
            }
            return sDaySky.lock();
        }

        // The personal environment as it was before the sun was first turned (captureLive()), so that
        // "Reset All" of the photo tools can put it back (restoreSun()): a day cycle goes on moving again,
        // no personal environment is no personal environment again. Forgotten with the sun (forgetSun()).
        struct SunUndo
        {
            bool                           taken    = false;
            bool                           hadLocal = false;         // a personal environment existed
            LLSettingsDay::ptr_t           day;                      // it was a day cycle (else a fixed sky)
            LLSettingsDay::Seconds         dayLength;
            LLSettingsDay::Seconds         dayOffset;
            LLSettingsWater::ptr_t         waterOverDay;             // a water of an asset put over that day
            bool                           daySkyWasLand = false;    // the day was the land's (copyLandDay())
            LandPlace                      daySkyPlace;              // and the land it was taken from
            LLQuaternion                   sunRotation;              // the fixed sky's sun (turned in place)
            LLEnvironment::EnvSelection_t  selected = LLEnvironment::ENV_LOCAL;
            size_t                         liveHash = 0;             // sLiveSky as the sun left it (applySun())
        };
        SunUndo sSunUndo;

        // The same number the official "Adjust Environment" window passes with its
        // own sky (llfloaterenvironmentadjust.cpp, FLOATER_ENVIRONMENT_UPDATE).
        constexpr S32 LIVE_ENVIRONMENT_VERSION = -2;

        // A personal environment exists and is the one shown (the World menu can show the shared one
        // while a personal one is kept).
        bool localInUse()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            return environment.hasEnvironment(LLEnvironment::ENV_LOCAL) &&
                   environment.getSelectedEnvironment() == LLEnvironment::ENV_LOCAL;
        }

        // Makes the personal environment a sky and water that can be changed in
        // place: the same steps as LLFloaterEnvironmentAdjust::captureCurrentEnvironment().
        void captureLive()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            bool           update_local = false;
            LLSettingsSky::ptr_t   sky;
            LLSettingsWater::ptr_t water;
            SunUndo                undo;

            sLiveSky.reset();
            sLiveWater.reset();

            undo.taken    = true;
            undo.selected = environment.getSelectedEnvironment();
            // As applyEnvironmentItem(): a personal environment kept but not shown (the shared one is) has an
            // old sky and water in it. The sun turns the sky on the screen, the land's, as when there is none;
            // the old one is not brought back (restoreSun() clears the personal environment for it).
            undo.hadLocal = localInUse();
            if (undo.hadLocal)
            {
                if (const LLSettingsDay::ptr_t day = environment.getEnvironmentDay(LLEnvironment::ENV_LOCAL))
                {   // A day cycle: freeze its sky.
                    sky   = environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
                    water = environment.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
                    undo.day           = day;
                    undo.dayLength     = environment.getEnvironmentDayLength(LLEnvironment::ENV_LOCAL);
                    undo.dayOffset     = environment.getEnvironmentDayOffset(LLEnvironment::ENV_LOCAL);
                    undo.waterOverDay  = (water && water->getAssetId().notNull()) ? water : LLSettingsWater::ptr_t();
                    undo.daySkyWasLand = sky && sky == daySky();
                    undo.daySkyPlace   = sDaySkyPlace;
                    sky   = sky ? sky->buildClone() : sky;
                    // A water of an asset (the card's water list put it over the day) is not moved by the
                    // day: it stays itself, so the list still knows it by its asset.
                    water = (water && water->getAssetId().isNull()) ? water->buildClone() : water;
                    update_local = true;
                }
                else
                {
                    sky   = environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
                    water = environment.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
                    if (sky)
                    {
                        undo.sunRotation = sky->getSunRotation(); // this sky itself is turned
                    }
                }
            }
            else
            {
                sky   = environment.getEnvironmentFixedSky(LLEnvironment::ENV_PARCEL, true);
                water = environment.getEnvironmentFixedWater(LLEnvironment::ENV_PARCEL, true);
                sky   = sky ? sky->buildClone() : sky;
                water = water ? water->buildClone() : water;
                update_local = true;
            }
            if (!sky || !water)
            {
                return; // no environment yet (not in a region)
            }
            sLiveSky   = sky;
            sLiveWater = water;
            sSunUndo   = undo;

            if (update_local)
            {
                environment.setEnvironment(LLEnvironment::ENV_LOCAL, sLiveSky, LIVE_ENVIRONMENT_VERSION);
                environment.setEnvironment(LLEnvironment::ENV_LOCAL, sLiveWater, LIVE_ENVIRONMENT_VERSION);
            }
            environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        }

        // Still the sky the personal environment shows (a sky chosen elsewhere since replaces it).
        bool liveSkyUsable()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            return sLiveSky && environment.hasEnvironment(LLEnvironment::ENV_LOCAL) &&
                   environment.getSelectedEnvironment() == LLEnvironment::ENV_LOCAL &&
                   environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL) == sLiveSky;
        }

        void applySun(float degrees)
        {
            sSunAngle = degrees;

            if (!liveSkyUsable())
            {
                captureLive();
            }
            if (!sLiveSky)
            {
                return;
            }

            // Where the camera looks (horizontally), against the sun's heading.
            const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
            const float camera_heading =
                (fabsf(at.mV[VX]) + fabsf(at.mV[VY]) > 0.001f) ? atan2f(at.mV[VY], at.mV[VX]) : 0.f;
            const float sun_heading = camera_heading + F_PI - degrees * DEG_TO_RAD;

            // The official azimuth runs the other way round (llvirtualtrackball.cpp:
            // azimuth = atan2(x, y) - 90 degrees, so heading = -azimuth).
            float azimuth = fmodf(-sun_heading, F_TWO_PI);
            if (azimuth < 0.f)
            {
                azimuth += F_TWO_PI;
            }

            // The sun keeps its height.
            F32 old_azimuth = 0.f, old_elevation = 0.f;
            LLVirtualTrackball::getAzimuthAndElevationDeg(sLiveSky->getSunRotation(), old_azimuth, old_elevation);
            float elevation = old_elevation * DEG_TO_RAD;
            if (is_approx_zero(elevation))
            {
                elevation = F_APPROXIMATELY_ZERO;
            }

            // As LLFloaterEnvironmentAdjust::onSunAzimElevChanged().
            LLQuaternion rotation;
            rotation.setAngleAxis(-elevation, 0.f, 1.f, 0.f);
            LLQuaternion azimuth_rotation;
            azimuth_rotation.setAngleAxis(F_TWO_PI - azimuth, 0.f, 0.f, 1.f);
            rotation *= azimuth_rotation;

            sLiveSky->setSunRotation(rotation);
            sLiveSky->update();
            // The official editor of the personal sky (World > Personal Lighting) changes this same sky in
            // place: a sky that is no longer as the sun left it has the user's changes, which restoreSun() keeps.
            sSunUndo.liveHash = sLiveSky->getHash();
        }

        // --- Presets file --------------------------------------------------

        // A value of presets.xml for a setting: of the setting's own type, and within the photo tools' row of it
        // (a slider's min and max, a combo's choices) when it has one.
        bool fileValue(const std::string& key, const LLSD& entry, LLSD& out)
        {
            LLControlVariable* control = gSavedSettings.getControl(key);
            if (!control)
            {
                return false;
            }
            bool fits = false;
            switch (control->type())
            {
            case TYPE_U32:
            case TYPE_S32:     fits = entry.isInteger() && (control->isType(TYPE_S32) || entry.asInteger() >= 0); break;
            case TYPE_F32:     fits = entry.isReal() || entry.isInteger(); break;
            case TYPE_BOOLEAN: fits = entry.isBoolean(); break;
            case TYPE_STRING:  fits = entry.isString(); break;
            default:           break;
            }
            if (!fits)
            {
                return false;
            }
            out = entry;
            const Item* item = findItem(key);
            if (!item || control->isType(TYPE_STRING) || control->isType(TYPE_BOOLEAN))
            {
                return true;
            }
            const double v = entry.asReal();
            if (item->kind == Kind::Slider)
            {
                const double clamped = llclamp(v, (double)item->min, (double)item->max);
                out = control->isType(TYPE_F32) ? LLSD(clamped) : LLSD((LLSD::Integer)ll_round((F32)clamped));
                return true;
            }
            if (item->kind == Kind::Combo && item->composite.empty())
            {
                return std::any_of(item->options.begin(), item->options.end(),
                                   [v](const Option& option) { return std::fabs(option.value - v) < 1e-4; });
            }
            return true;
        }

        std::vector<Preset> loadPresets()
        {
            std::vector<Preset> presets = builtinPresets();

            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, PRESETS_FILE);
            llifstream        file(path.c_str());
            if (!file.is_open())
            {
                return presets;
            }
            LLSD data;
            if (LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(data, file) || !data.isMap())
            {
                LL_WARNS("Plus") << "Cannot parse " << path << "; the built-in photo presets are used" << LL_ENDL;
                return presets;
            }
            for (Preset& preset : presets)
            {
                if (!data.has(preset.id) || !data[preset.id].isMap())
                {
                    continue;
                }
                for (int i = 0; i < PARAM_COUNT; ++i)
                {
                    const LLSD& entry = data[preset.id][PARAM_NAMES[i]];
                    if (entry.isReal() || entry.isInteger())
                    {
                        preset.values[i] = (float)entry.asReal();
                    }
                    else if (entry.isString() && entry.asString().empty())
                    {
                        preset.values[i] = UNSET; // "" in the file: leave this one alone
                    }
                }
                // the values of the preset's own settings ("settings" map); only the ones it names
                const LLSD& settings = data[preset.id]["settings"];
                if (!settings.isMap())
                {
                    continue;
                }
                for (auto& [key, value] : preset.settings)
                {
                    LLSD entry;
                    if (fileValue(key, settings[key], entry))
                    {
                        value = entry; // a missing or unfit one keeps the built-in
                    }
                }
            }
            return presets;
        }
    }

    const ParamRange& range(Param param)
    {
        return RANGES[static_cast<int>(param)];
    }

    const std::vector<Preset>& presets()
    {
        static const std::vector<Preset> sPresets = loadPresets();
        return sPresets;
    }

    const Preset* findPreset(const std::string& id)
    {
        for (const Preset& preset : presets())
        {
            if (preset.id == id)
            {
                return &preset;
            }
        }
        return nullptr;
    }

    float value(Param param)
    {
        switch (param)
        {
        case Param::Brightness:
            if (has(SETTING_EXPOSURE))
            {
                const float exposure = llclamp(gSavedSettings.getF32(SETTING_EXPOSURE), 0.5f, 4.f);
                return llclamp(100.f * log2f(exposure), -100.f, 100.f);
            }
            return 0.f;
        case Param::Blur:
            if (has(SETTING_FNUMBER))
            {
                const float f = llclamp(gSavedSettings.getF32(SETTING_FNUMBER), F_NUMBER_WIDE, F_NUMBER_NARROW);
                return 100.f * (logf(F_NUMBER_NARROW) - logf(f)) / (logf(F_NUMBER_NARROW) - logf(F_NUMBER_WIDE));
            }
            return 0.f;
        case Param::Tint:
            if (has(SETTING_LOOK) && has(SETTING_LOOK_STR))
            {
                const std::string look = gSavedSettings.getString(SETTING_LOOK);
                const float strength = (float)gSavedSettings.getU32(SETTING_LOOK_STR);
                if (look == LOOK_WARM)
                {
                    return llclamp(strength, 0.f, 100.f);
                }
                if (look == LOOK_COOL)
                {
                    return -llclamp(strength, 0.f, 100.f);
                }
            }
            return 0.f;
        case Param::Sun:
            return sSunAngle;
        case Param::Shadow:
            if (has(SETTING_AMBIENT))
            {
                const float ambient = llmax(gSavedSettings.getF32(SETTING_AMBIENT), 0.01f);
                return llclamp(50.f - 50.f * log2f(ambient / AMBIENT_DEFAULT), 0.f, 100.f);
            }
            return 50.f;
        case Param::Contrast:
            if (has(SETTING_TONEMAP))
            {
                return llclamp((gSavedSettings.getF32(SETTING_TONEMAP) - 0.4f) / 0.6f * 100.f, 0.f, 100.f);
            }
            return 50.f;
        case Param::Glare:
            return has(SETTING_GLARE) ? (float)llclamp(gSavedSettings.getS32(SETTING_GLARE), 0, 3) : 0.f;
        default:
            return 0.f;
        }
    }

    bool isTintLook(const std::string& look)
    {
        return look == LOOK_COOL || look == LOOK_WARM;
    }

    void setValue(Param param, float v)
    {
        takeOriginals();
        const ParamRange& limits = range(param);
        v = llclamp(v, limits.min, limits.max);

        switch (param)
        {
        case Param::Tint:
            if (has(SETTING_LOOK) && has(SETTING_LOOK_STR))
            {
                followUserLook();
                if (fabsf(v) < 0.5f)
                {   // no tint: the look the user had
                    writeTint(originalLook());
                }
                else
                {
                    writeTint({ v > 0.f ? LOOK_WARM : LOOK_COOL, (U32)ll_round(fabsf(v)) });
                }
            }
            break;
        case Param::Sun:
            applySun(v);
            break;
        default:
            writePlain(param, v);
            break;
        }
    }

    float paramDefault(Param param)
    {
        const int index = static_cast<int>(param);
        return index >= 0 && index < PARAM_COUNT ? PARAM_DEFAULTS[index] : UNSET;
    }

    namespace
    {
        bool names(const Preset& preset, const std::string& key)
        {
            return std::any_of(preset.settings.begin(), preset.settings.end(),
                               [&key](const std::pair<std::string, LLSD>& setting) { return setting.first == key; });
        }

        // Gives back what the presets wrote that `keep` does not name (all with nullptr). A value changed since
        // (by hand, the Charis menu, the photo tools) is the user's and stays.
        void giveBackExcept(const Preset* keep)
        {
            SettingWrites& writes = presetWrites();
            for (auto it = writes.begin(); it != writes.end();)
            {
                if (keep && names(*keep, it->first))
                {
                    ++it;
                    continue;
                }
                giveBack(it->first, it->second);
                it = writes.erase(it);
            }
        }

        void writePresetSettings(const Preset& preset)
        {
            giveBackExcept(&preset);
            SettingWrites& writes = presetWrites();
            for (const auto& [key, value] : preset.settings)
            {
                writeRemembered(writes, key, value);
            }
        }
    }

    bool presetSettingsChanged(const Preset& preset)
    {
        const SettingWrites& writes = presetWrites();
        for (const auto& [key, value] : preset.settings)
        {
            const auto found = writes.find(key);
            if (found != writes.end() && changedSince(key, found->second))
            {
                return true;
            }
        }
        return false;
    }

    void givePresetSettingsBack()
    {
        giveBackExcept(nullptr);
    }

    void forgetPresetSettings()
    {
        presetWrites().clear(); // read first: a preset of an earlier session is forgotten too, not read later
    }

    bool applyPreset(const std::string& id)
    {
        const Preset* preset = findPreset(id);
        if (!preset)
        {
            return false;
        }
        // The look the user had is remembered before this preset's own settings can replace it (the tint's
        // "none"), and an earlier preset's settings go back first, so the sliders below start from the user's.
        takeOriginals();
        writePresetSettings(*preset);
        for (int i = 0; i < PARAM_COUNT; ++i)
        {
            if (preset->values[i] > UNSET)
            {
                setValue(static_cast<Param>(i), preset->values[i]);
            }
        }
        gSavedSettings.setString(SETTING_PRESET, id);
        return true;
    }

    std::string currentPreset()
    {
        return has(SETTING_PRESET) ? gSavedSettings.getString(SETTING_PRESET) : std::string();
    }

    bool blurAvailable()
    {
        return has(SETTING_DOF) && gSavedSettings.getBOOL(SETTING_DOF);
    }

    bool isEnvironmentItem(const LLUUID& item_id)
    {
        const LLInventoryItem* item = gInventory.getItem(item_id);
        return item && item->getInventoryType() == LLInventoryType::IT_SETTINGS;
    }

    namespace
    {
        // Makes the personal environment the land's day cycle as it is (the same day, length and offset:
        // it looks the same and goes on moving). False when the land has none yet.
        bool copyLandDay()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            for (LLEnvironment::EnvSelection_t source : { LLEnvironment::ENV_PARCEL, LLEnvironment::ENV_REGION })
            {
                if (const LLSettingsDay::ptr_t day = environment.getEnvironmentDay(source))
                {
                    environment.setEnvironment(LLEnvironment::ENV_LOCAL, day, environment.getEnvironmentDayLength(source),
                                               environment.getEnvironmentDayOffset(source));
                    setDaySky(environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL), landPlace());
                    return true;
                }
            }
            return false;
        }

        // The water the land shows now (the region's when the parcel has none of its own).
        LLSettingsWater::ptr_t landWater()
        {
            return LLEnvironment::instance().getEnvironmentFixedWater(LLEnvironment::ENV_PUSH, true);
        }
    }

    void fetchSettingsAsset(const LLUUID& asset, const void* owner, SettingsFetched done)
    {
        if (!gAssetStorage)
        {
            done(asset, LLSettingsBase::ptr_t(), LL_ERR_ASSET_REQUEST_FAILED);
            return;
        }
        gAssetStorage->getAssetData(asset, LLAssetType::AT_SETTINGS,
            [done](const LLUUID& asset_id, LLAssetType::EType, void*, S32 status, LLExtStat ext_status)
            {
                if (status)
                {
                    if (status != LL_ERR_CIRCUIT_GONE)  // not while quitting
                    {
                        // as LLSettingsVOBase::onAssetDownloadComplete() logs it (not reached on this way)
                        LL_WARNS("SETTINGS") << "Error retrieving asset " << asset_id << ". Status code=" << status << "("
                                             << LLAssetStorage::getErrorString(status) << ") ext_status=" << (U32)ext_status
                                             << LL_ENDL;
                    }
                    done(asset_id, LLSettingsBase::ptr_t(), status);
                    return;
                }
                // In the cache now: read as getSettingsAsset() reads it (from the cache, at once). When the cache cannot
                // give it after all (not written, or gone), that call asks again with no user data, and the asset
                // storage may drop it as a duplicate (another part asking for it): no answer would ever come. So the
                // answer must come here and now; else it is a failure, and a late answer is dropped (done once).
                auto answered = std::make_shared<bool>(false);
                LLSettingsVOBase::getSettingsAsset(asset_id,
                    [done, answered](LLUUID id, LLSettingsBase::ptr_t settings, S32 read_status, LLExtStat)
                    {
                        if (*answered)
                        {
                            return;
                        }
                        *answered = true;
                        done(id, settings, read_status);
                    });
                if (!*answered)
                {
                    *answered = true;
                    LL_WARNS("SETTINGS") << "Asset " << asset_id << " came but could not be read from the cache" << LL_ENDL;
                    done(asset_id, LLSettingsBase::ptr_t(), LL_ERR_ASSET_REQUEST_FAILED);
                }
            },
            const_cast<void*>(owner), true);
    }

    namespace
    {
        // What a layer (EnvLayer) of the personal environment wants. Main thread (the asset's callback comes there).
        struct LayerWant
        {
            LLUUID        asset;                            // the asset wanted (null: none, or dropped)
            EnvFetch      fetch      = EnvFetch::None;      // of that asset
            EnvTransition transition = EnvTransition::Instant;
            U32           order      = 0;                   // when it was asked for, of all the layers (sWantOrder)
            U32           changes    = 0;                   // asked for or dropped: localEnvironmentChanges()
        };
        LayerWant sWant[static_cast<size_t>(EnvLayer::Count)];
        U32       sWantOrder = 0;

        LayerWant& want(EnvLayer layer)
        {
            return sWant[static_cast<size_t>(layer)];
        }

        void fetchWanted(EnvLayer layer);

        boost::signals2::signal<void()>& failedSignal()
        {
            static boost::signals2::signal<void()> signal;
            return signal;
        }

        // The kind of settings a layer takes.
        bool layerTakes(EnvLayer layer, const LLSettingsBase& settings)
        {
            switch (layer)
            {
                case EnvLayer::Sky:   return settings.getSettingsTypeValue() == LLSettingsType::ST_SKY;
                case EnvLayer::Water: return settings.getSettingsTypeValue() == LLSettingsType::ST_WATER;
                case EnvLayer::Day:   return settings.getSettingsTypeValue() == LLSettingsType::ST_DAYCYCLE;
                default:              return false;
            }
        }

        // A layer's asset came, or could not be had. Put on only while it is still the one wanted and not come yet.
        // Matched by the asset, not by the request: the asset storage drops a second request for an asset still
        // coming, callback and all (LLAssetStorage::getAssetData), and only the first one's callback comes.
        void onWantedAsset(EnvLayer layer, const LLUUID& asset_id, const LLSettingsBase::ptr_t& settings, S32 status)
        {
            LayerWant& wanted = want(layer);
            if (LLApp::isQuitting() || wanted.fetch != EnvFetch::Pending || wanted.asset != asset_id)
            {
                return;     // another chosen since, or dropped, or put on already
            }
            // Not of the layer's kind (a sky asked for as a water by a wrong item): not put on, as one not had.
            if (!settings || status || !layerTakes(layer, *settings))
            {
                if (settings && !status)
                {
                    LL_WARNS("SETTINGS") << "Asset " << asset_id << " is a " << settings->getSettingsType()
                                         << ", not of the layer asked for" << LL_ENDL;
                }
                wanted.fetch = EnvFetch::Failed;
                LLNotificationsUtil::add("FailedToFindSettings", LLSD().with("NAME", asset_id.asString()));
                failedSignal()();
                return;
            }
            wanted.fetch = EnvFetch::Arrived;
            // As LLEnvironment::onSetEnvAssetLoaded().
            LLEnvironment& environment = LLEnvironment::instance();
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, settings);
            environment.updateEnvironment(wanted.transition == EnvTransition::Instant ? LLEnvironment::TRANSITION_INSTANT
                                                                                     : LLEnvironment::TRANSITION_DEFAULT);
            if (layer == EnvLayer::Day)
            {
                // A day cycle replaces the sky and the water: a sky or water chosen after it and come before it goes
                // on it again (from the cache). One still coming goes on it when it comes.
                for (EnvLayer over : { EnvLayer::Sky, EnvLayer::Water })
                {
                    const LayerWant& chosen = want(over);
                    if (chosen.fetch == EnvFetch::Arrived && chosen.asset.notNull() && chosen.order > wanted.order)
                    {
                        fetchWanted(over);
                    }
                }
            }
        }

        // Asks for the layer's wanted asset (again). The layer's own entry marks the request (fetchSettingsAsset()):
        // another part asking for the same asset does not take the answer away, and the layer's own second request
        // for an asset still coming is dropped (the first one's answer comes; matched by the asset).
        void fetchWanted(EnvLayer layer)
        {
            LayerWant& wanted = want(layer);
            wanted.fetch = EnvFetch::Pending;     // first: a cached asset comes at once
            fetchSettingsAsset(wanted.asset, &wanted,
                [layer](const LLUUID& asset_id, const LLSettingsBase::ptr_t& settings, S32 status)
                {
                    onWantedAsset(layer, asset_id, settings, status);
                });
        }

        // The personal environment cleared, without dropping what is still coming (restoreLand*(): the other
        // layer's choice still comes; clearEnvironment() drops all first).
        void clearLocalEnvironment()
        {
            LLEnvironment& environment = LLEnvironment::instance();
            environment.clearEnvironment(LLEnvironment::ENV_LOCAL);
            environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
            forgetSun();
        }
    }

    void forgetLocalEnvironmentAsset(EnvLayer layer)
    {
        LayerWant& wanted = want(layer);
        wanted.asset.setNull();
        wanted.fetch = EnvFetch::None;
        ++wanted.changes;
    }

    U32 localEnvironmentChanges(EnvLayer layer)
    {
        return want(layer).changes;
    }

    EnvFetch localEnvironmentFetch(EnvLayer layer)
    {
        return want(layer).fetch;
    }

    LLUUID localEnvironmentWanted(EnvLayer layer)
    {
        return want(layer).asset;
    }

    boost::signals2::connection setLocalEnvironmentFailedCallback(const std::function<void()>& cb)
    {
        return failedSignal().connect(cb);
    }

    void setLocalEnvironmentAsset(const LLUUID& asset, EnvLayer layer, EnvTransition transition)
    {
        if (layer == EnvLayer::Day)
        {
            // a day cycle replaces the sky and the water: those still coming would come over it
            forgetLocalEnvironmentAsset(EnvLayer::Sky);
            forgetLocalEnvironmentAsset(EnvLayer::Water);
        }
        LayerWant& wanted = want(layer);
        wanted.asset      = asset;
        wanted.transition = transition;
        wanted.order      = ++sWantOrder;
        ++wanted.changes;
        fetchWanted(layer);
    }

    bool applyEnvironmentItem(const LLUUID& item_id)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item || item->getInventoryType() != LLInventoryType::IT_SETTINGS)
        {
            return false;
        }
        const LLSettingsType::type_e type = item->getSettingsType();
        if (type != LLSettingsType::ST_SKY && type != LLSettingsType::ST_WATER)
        {
            return false;   // a day cycle: not put on here
        }
        LLEnvironment& environment = LLEnvironment::instance();
        const bool     water       = item->getSettingsType() == LLSettingsType::ST_WATER;
        if (!localInUse())
        {
            // A personal environment kept but not shown has an old sky and water in it: neither is wanted.
            if (water)
            {
                copyLandDay(); // the sky goes on as the land's
            }
            else
            {
                environment.clearEnvironment(LLEnvironment::ENV_LOCAL);
            }
        }
        // What the Apply Locally entry of the inventory does (LLSettingsBridge::performAction),
        // by the asset: an item of the library needs no copy. A sky replaces only the sky of the
        // personal environment and a water only its water (LLEnvironment::setEnvironment(env, settings)).
        setLocalEnvironmentAsset(item->getAssetUUID(), water ? EnvLayer::Water : EnvLayer::Sky, EnvTransition::Instant);
        environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        if (!water)
        {
            forgetSun(); // a water leaves the sky, and the sun the slider turned in it, alone
        }
        return true;
    }

    void clearEnvironment()
    {
        // As the Reset button of the official "Adjust Environment" window: nothing still coming is put on after it.
        for (EnvLayer layer : { EnvLayer::Sky, EnvLayer::Water, EnvLayer::Day })
        {
            forgetLocalEnvironmentAsset(layer);
        }
        clearLocalEnvironment();
    }

    void restoreLandSky()
    {
        // this layer's choice, made now: the sky and the day cycle still coming are dropped (the water is not)
        forgetLocalEnvironmentAsset(EnvLayer::Sky);
        forgetLocalEnvironmentAsset(EnvLayer::Day);
        LLUUID                 unused;
        LLSettingsWater::ptr_t keep;
        if (environmentSource(true, unused) != EnvSource::Land)
        {
            keep = LLEnvironment::instance().getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
        }
        LLEnvironment& environment = LLEnvironment::instance();
        if (!keep)
        {
            // Nothing of the water to keep: the land's as it was. A water still coming (chosen while no personal
            // environment was in use) gets the land's day under it, as applyEnvironmentItem() made it: cleared, the
            // water would come into an empty personal environment and stop the sky where it is.
            if (localEnvironmentFetch(EnvLayer::Water) == EnvFetch::Pending && copyLandDay())
            {
                environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
                forgetSun();
                return;
            }
            clearLocalEnvironment();
            return;
        }
        if (copyLandDay())
        {
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, keep);
        }
        else
        {
            // no day cycle to take: the land's sky as it is now
            const LLSettingsSky::ptr_t sky = environment.getEnvironmentFixedSky(LLEnvironment::ENV_PUSH, true);
            const LLSettingsSky::ptr_t copy = sky ? sky->buildClone() : LLSettingsSky::ptr_t();
            if (!copy)
            {
                clearLocalEnvironment();
                return;
            }
            environment.setEnvironment(LLEnvironment::ENV_LOCAL, copy, keep);
        }
        environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        forgetSun();
    }

    void restoreLandWater()
    {
        // as restoreLandSky(): the water and the day cycle still coming are dropped (the sky is not)
        forgetLocalEnvironmentAsset(EnvLayer::Water);
        forgetLocalEnvironmentAsset(EnvLayer::Day);
        if (!localInUse())
        {
            return; // the land's already
        }
        LLEnvironment&             environment = LLEnvironment::instance();
        const LLSettingsSky::ptr_t sky         = environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
        if (sky && sky == daySky())
        {
            // The personal environment is the land's day with a water over it, nothing else of the user's:
            // the land's environment as it was (it also follows the land again, as before the water). A sky
            // still coming still comes.
            clearLocalEnvironment();
            return;
        }
        const LLSettingsWater::ptr_t land = landWater();
        const LLSettingsWater::ptr_t copy = land ? land->buildClone() : LLSettingsWater::ptr_t();
        if (!copy)
        {
            return;
        }
        environment.setEnvironment(LLEnvironment::ENV_LOCAL, copy);
        environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    }

    EnvSource environmentSource(bool water, LLUUID& asset)
    {
        asset.setNull();
        // Not in a region: nothing to look at (and the land's environment would be asked for in vain).
        if (!gAgent.getRegion() || !localInUse())
        {
            return EnvSource::Land;
        }
        LLEnvironment& environment = LLEnvironment::instance();
        if (water)
        {
            const LLSettingsWater::ptr_t mine = environment.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
            if (!mine)
            {
                return EnvSource::Land;
            }
            if (mine->getAssetId().notNull())
            {
                asset = mine->getAssetId();
                return EnvSource::Asset;
            }
            // A sky put on alone takes the water shown at that moment along (the land's): the same values.
            const LLSettingsWater::ptr_t land = landWater();
            if (land && (mine == land || mine->getHash() == land->getHash()))
            {
                return EnvSource::Land;
            }
            return EnvSource::Other;
        }
        const LLSettingsSky::ptr_t mine = environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
        if (!mine || mine == daySky())
        {
            return EnvSource::Land;
        }
        if (mine->getAssetId().notNull())
        {
            asset = mine->getAssetId();
            return EnvSource::Asset;
        }
        return EnvSource::Other;
    }

    void forgetSun()
    {
        sLiveSky.reset();
        sLiveWater.reset();
        sSunAngle = UNSET;
        sSunUndo  = SunUndo();
    }

    bool sunChanged()
    {
        return sSunUndo.taken && liveSkyUsable();
    }

    void restoreSun()
    {
        // Only while the personal environment is still the one the sun was turned in, as the sun left it: a
        // sky put on since, or changes made in one of the official editors, are the user's choice and stay.
        if (sunChanged() && sLiveSky->getHash() == sSunUndo.liveHash)
        {
            // A sky or water still coming is not dropped: chosen since, it comes over what is set back here (the
            // contract above: nothing is put back when a sky was chosen since).
            LLEnvironment& environment = LLEnvironment::instance();
            const SunUndo  undo        = sSunUndo;
            // A water put on since the sun was turned (the water list: it leaves the sky, and the sun, as
            // they are) is the user's too, and stays over what comes back.
            const LLSettingsWater::ptr_t water_now    = environment.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL);
            const bool                   water_chosen = water_now && water_now != sLiveWater;
            LLEnvironment::EnvSelection_t selected    = undo.selected;
            if (!undo.hadLocal)
            {
                if (!water_chosen)
                {
                    // a water still coming: the land's day under it (as restoreLandSky())
                    if (localEnvironmentFetch(EnvLayer::Water) == EnvFetch::Pending && copyLandDay())
                    {
                        selected = LLEnvironment::ENV_LOCAL;
                    }
                    else
                    {
                        environment.clearEnvironment(LLEnvironment::ENV_LOCAL);
                    }
                }
                else
                {
                    // as a water put on without a personal environment: the land's day under it
                    if (copyLandDay())
                    {
                        environment.setEnvironment(LLEnvironment::ENV_LOCAL, water_now);
                    }
                    selected = LLEnvironment::ENV_LOCAL; // the water is the personal environment's
                }
            }
            else if (undo.day)
            {
                environment.setEnvironment(LLEnvironment::ENV_LOCAL, undo.day, undo.dayLength, undo.dayOffset);
                if (undo.daySkyWasLand)
                {
                    setDaySky(environment.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL), undo.daySkyPlace);
                }
                if (water_chosen)
                {
                    environment.setEnvironment(LLEnvironment::ENV_LOCAL, water_now);
                }
                else if (undo.waterOverDay)
                {
                    environment.setEnvironment(LLEnvironment::ENV_LOCAL, undo.waterOverDay);
                }
            }
            else
            {
                sLiveSky->setSunRotation(undo.sunRotation); // the sky was turned in place; its water is as it is
                sLiveSky->update();
            }
            environment.setSelectedEnvironment(selected, LLEnvironment::TRANSITION_INSTANT);
        }
        forgetSun();
    }

    bool gazeAvailable()
    {
        return has(SETTING_GAZE);
    }

    bool gazeAllowsEveryone()
    {
        return has(SETTING_GAZE_EVERYONE) && gSavedSettings.getBOOL(SETTING_GAZE_EVERYONE);
    }

    int gazeMode()
    {
        const int mode = gazeAvailable() ? llclamp(gSavedSettings.getS32(SETTING_GAZE), 0, 2) : 0;
        return (mode == 2 && !gazeAllowsEveryone()) ? 1 : mode; // plus/gaze treats it so, too
    }

    void setGazeMode(int mode)
    {
        if (gazeAvailable())
        {
            gSavedSettings.setS32(SETTING_GAZE, llclamp(mode, 0, gazeAllowsEveryone() ? 2 : 1));
        }
    }
}

namespace
{
    void init()
    {
        // The card: a plain floater, so the detached-window feature (plus/detach)
        // takes it like the conversations. Menu items: skins/default/xui/<lang>/menu_plus.xml.
        // Settings: app_settings/settings_plus.xml (PlusPhoto*).
        plus::photomode::declareSkySettings(); // the per-account lists of favourite skies
        plus::photomode::registerFloater();
        // The toolbar button (app_settings/commands_plus.xml "plus_photo_tools", its texts in
        // skins/default/xui/<lang>/strings_plus.xml): a camera with a sparkle, white like the official
        // icons (tools/make_phototools_icon.py in the sl-viewer repository).
        plus::declareUIImage("Plus_PhotoTools_Icon", "plus/phototools_18.png");
        // PlusIgnoreSimCameraConstraint (the hook is in llviewermessage.cpp)
        plus::photomode::watchCameraConstraintSetting();
    }

    // The guide's view goes into the window when the UI exists. The keys (photokeys.cpp, registered in
    // llviewerinput.cpp): their default keys into a key_bindings.xml the user saved before they existed.
    void onUIReady()
    {
        plus::photomode::attachGuide();
        plus::photomode::addDefaultKeys();
    }

    plus::FeatureRegistration sRegistration({ "photomode", &init, &onUIReady });
}
