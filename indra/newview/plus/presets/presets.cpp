/**
 * @file presets.cpp
 * @brief The Charis features on the official quality levels (featuretable.txt),
 *        the one-time move from the retired Charis presets, the comparison
 *        with the official viewer and the snapshot boost. Registers with the
 *        plus foundation (research/183, research/151).
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


// How it fits the official viewer (research/183)
// ----------------------------------------------
// The Charis features that draw or work each frame are set by the official
// quality slider (RenderQualityPerformance, 0 Low .. 6 Ultra): their values
// for each level are lines of featuretable.txt (in <plus> blocks), next to the
// official ones, so moving the slider (Preferences, the photo
// tools' Quality tab) sets them with the rest, through
// LLFeatureManager::setGraphicsLevel(). The matters of taste (the look, the
// weather, lens flares, the shadows' softness, the stars...) are not there:
// the slider leaves them alone. The heavy features are capped for the "safe"
// mode, unknown GPUs and Intel ones (featuretable.txt's masks: the values are
// the smallest of all the lists applied).
//
// The Charis presets of before (light, standard, beautiful, and official_mid
// for comparisons; copies in the account's presets/graphic folder, a backup
// for "Undo") are retired. Once, after a login (PlusPresetMigrated), a preset
// that was in use moves to its level: light Mid, official_mid MidHigh,
// standard High, beautiful Ultra (the draw distance and the avatar limits as
// they were). Settings of the user's own ("Custom") are left as they are. The
// copies are removed at every login (another account may still have them), and
// so is the backup.
//
// Here too: the levels next to the one in use get their shaders compiled in
// the background after a login or a change of level (research/111 B-PW); the
// official preset saving takes the Charis settings of the levels
// (addPresetControlNames(), a <plus> hook in LLPresetsManager::savePreset); the
// automatic level picks Ultra for a fast GPU (recommendedGraphicsLevel(), a
// <plus> hook in LLFeatureManager); the comparison with the official viewer
// (research/151, Preferences > Charis > Development) is MidHigh, whose Charis
// features are all off but full OIT, with the official MidHigh's own LOD and
// anisotropic filtering (featuretable.txt has Firestorm's), partial OIT, the official
// night sky, no TAA (off at every level too) and a 256 m draw distance.
//
// The snapshot boost (PlusSnapshotBoost, one check box, on by default) is the
// second axis: whatever the level, the frames of a snapshot are drawn with
// every Charis enhancement at its best quality (GTAO, contact shadows, sun
// shafts, the shadow distance, the glare and the softer face shadows at least
// medium) and full OIT
// (RenderOITSnapshotFull, which the box drives). No
// setting changes; features ask snapshotBoostActive(). Not for the 360
// snapshot's faces (simpleSnapshot: screen-space effects would seam at their
// edges) nor the pictures the viewer saves for itself (saveSnapshot: the login
// screen's last and home locations): SnapshotBoostWithheld.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "presets.h"

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfeaturemanager.h"
#include "llfile.h"
#include "llfloaterpreference.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llnotificationsutil.h"
#include "llpresetsmanager.h"
#include "llrhi.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "lltexturefetch.h"
#include "lltimer.h"
#include "lluictrl.h"
#include "lluictrlfactory.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "fsyspath.h"
#include "plus/photomode/phototools.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/shaderreload/shaderreload.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    constexpr char SETTING_PRESET[]         = "PlusGraphicPreset";          // the retired preset applied last (its id)
    constexpr char SETTING_REVISION[]       = "PlusGraphicPresetRevision";  // in the retired presets' copies: marks ours
    constexpr char SETTING_MIGRATED[]       = "PlusPresetMigrated";         // the move to the levels is done
    constexpr char SETTING_BOOST[]          = "PlusSnapshotBoost";
    constexpr char SETTING_OIT_SNAPSHOT[]   = "RenderOITSnapshotFull";      // full OIT in snapshots (research/41)
    constexpr char SETTING_ACTIVE[]         = "PresetGraphicActive";        // official: the saved preset in use
    constexpr char SETTING_QUALITY[]        = "RenderQualityPerformance";   // official: the quality slider's level
    constexpr char SETTING_DEBUG_QUALITY[]  = "DebugQualityPerformance";    // official: written with it (onChangeQuality)
    constexpr char SETTING_FAR_CLIP[]       = "RenderFarClip";
    constexpr char SETTING_VOLUME_LOD[]     = "RenderVolumeLODFactor";
    constexpr char SETTING_ANISOTROPIC[]    = "RenderAnisotropicLevel";
    constexpr char SETTING_SKY_STARS[]      = "PlusSkyStars";               // plus/starfield: 0 the official night sky
    constexpr char SETTING_OIT[]            = "RenderOITMode";              // 1: OIT only where avatar alpha is (hybrid)
    constexpr char SETTING_DYNAMIC_LOD[]    = "RenderDynamicLOD";           // the detail by distance (on at every level)

    constexpr char DISPLAY_NAME_PREFIX[]    = "Charis ";                    // the retired presets' copies were named so
    constexpr char STRINGS_FILE[]           = "panel_plus_presets.xml";     // the texts (localized)
    // The retired presets' backup for "Undo" (user_settings; the accounts' folders before fix-round9b).
    constexpr char BACKUP_FILE[]            = "charis_graphics_before_preset.xml";
    // Frames drawn between the "applying" notice and the shader reload, so the
    // notice is on screen while the viewer stands still.
    constexpr S32  NOTICE_FRAMES = 3;
    // The shader prewarm starts this long after login or a change of level (research/111 B-PW).
    constexpr F32  PREWARM_DELAY_SECONDS = 20.f;

    constexpr U32 LEVEL_COUNT = 7;          // Low, LowMid, Mid, MidHigh, High, HighUltra, Ultra
    constexpr U32 LEVEL_MID_HIGH = 3;
    constexpr U32 LEVEL_ULTRA = 6;

    // research/183: the automatic level is Ultra for a GPU of the official benchmark's
    // fastest class with this much VRAM (an 8 GB card reports a little less).
    constexpr S32 ULTRA_GPU_CLASS = GPU_CLASS_5;
    constexpr U32 ULTRA_MIN_VRAM_MB = 7 * 1024;

    // The comparison with the official viewer (research/151): MidHigh (no Charis feature on),
    // with the official MidHigh's values where featuretable.txt has Firestorm's, partial OIT
    // (the MidHigh level has full OIT, 2026-10-25; the comparison is "about the official one"),
    // the official night sky (the stars are a matter of taste, not in the levels; the retired comparison
    // preset had them off), no TAA (the official viewer has none; every level has it off too, 2026-11-09)
    // and 256 m. tools/dummy-sim/level_preset.py writes the same.
    constexpr U32 COMPARISON_LEVEL = LEVEL_MID_HIGH;
    constexpr F32 COMPARISON_FAR_CLIP = 256.f;
    constexpr F32 COMPARISON_VOLUME_LOD = 1.375f;
    constexpr U32 COMPARISON_ANISOTROPIC = 4;
    constexpr U32 COMPARISON_OIT = 1;          // partial (hybrid)
    constexpr char SETTING_TAA[] = "PlusTAA";   // plus/taa: off in every level and in the comparison

    // Values read back from F32 settings are compared with this tolerance.
    constexpr F64 F32_TOLERANCE = 1e-4;

    // The retired presets and the level each moves to (research/183 section 3.3). official_mid
    // was the comparison: all its settings counted for "in use", no matter of taste.
    struct RetiredPreset
    {
        const char* id;
        U32 level;
        bool comparison;
    };
    constexpr RetiredPreset RETIRED_PRESETS[] =
    {
        { "light",        2,              false },
        { "official_mid", LEVEL_MID_HIGH, true },
        { "standard",     4,              false },
        { "beautiful",    LEVEL_ULTRA,    false },
    };

    // The retired everyday presets' matters of taste (fix-round109, research/162): set by them,
    // but changing them afterwards left the preset in use. The move to the levels goes by the same.
    constexpr const char* TASTE_SETTINGS[] =
    {
        "PlusShadowSoftness",
        "PlusLensFlare", "PlusCrossFilter",                         // photographic effects
        "PlusSkyStars", "PlusSkyStarGain", "PlusNightCloudLift", "PlusSkyStarsReal", "PlusSkyStarsLines", // the night sky's look
        "PlusLook", "PlusPaintStyle", "PlusFxStrength",             // the look
        "PlusGazeMode", "PlusFpsLimit",                             // the user's own
        "PlusHideOtherLandObjects", "PlusHideOtherParcelObjects",
        "PlusRain", "PlusSnow", "PlusSnowGround", "PlusWetLook",    // the viewer's weather
        "PlusGroundFog", "PlusFireflies",
        "RenderDepthOfField", "RenderTonemapType", "RenderTonemapMix", "RenderExposure",
    };

    // What the move to the levels leaves as it was (research/183 section 3.3: the draw distance,
    // the avatar limits): the level sets them too.
    constexpr const char* KEPT_BY_MIGRATION[] =
    {
        "RenderFarClip", "RenderAvatarMaxNonImpostors", "RenderAvatarMaxComplexity",
    };

    bool sCopiesRemoved = false;    // the login's removal of the retired copies has run
    S32 sSnapshotDepth = 0;     // snapshots being rendered (nested)
    U32 sSnapshotSerial = 0;    // snapshotSerial: outermost snapshots begun
    S32 sBoostDepth = 0;        // of them, those with the boost on
    S32 sWithheldDepth = 0;     // SnapshotBoostWithheld

    // research/109: the texture budget's hold around snapshots, and the log after them.
    constexpr F64 BUDGET_HOLD_SECONDS = 5.0;    // at most, after a snapshot
    constexpr U64 BUDGET_HOLD_ROOM_MB = 512;    // VRAM back within this of before
    constexpr F64 AFTER_LOG_SECONDS = 60.0;
    constexpr F64 AFTER_LOG_PERIOD = 5.0;
    F64 sSnapshotEnded = 0.0;           // the outermost snapshot's end (0: none yet)
    U64 sSnapshotVRAMBefore = 0;        // VRAM in use before the outermost snapshot, MB
    struct BudgetFigures
    {
        F32 used_mb = 0.f;
        F32 limit_mb = 0.f;
        F32 discard_bias = 0.f;
        S32 pressure = 0;
        bool held = false;
    };
    BudgetFigures sBudget;
    // The counters at the snapshot's end, for the log's "since" figures.
    struct AfterCounters
    {
        U64 made = 0, released = 0, bc7 = 0;
        U32 http = 0;
    };
    AfterCounters sAfterStart;
    U32 sAfterLogId = 0;                // a newer snapshot's log replaces the running one
    U32 sScreenTargetSets = 0;          // made since the outermost snapshot began

    std::vector<std::function<void(bool)>>& boostListeners()
    {
        static std::vector<std::function<void(bool)>> listeners;
        return listeners;
    }

    void notifyBoost(bool active)
    {
        for (const std::function<void(bool)>& listener : boostListeners())
        {
            listener(active);
        }
    }

    // addSnapshotListener's (research/165): the outermost snapshot's begin and end, boost or not.
    std::vector<std::function<void(bool)>>& snapshotListeners()
    {
        static std::vector<std::function<void(bool)>> listeners;
        return listeners;
    }

    void notifySnapshot(bool in_progress)
    {
        for (const std::function<void(bool)>& listener : snapshotListeners())
        {
            listener(in_progress);
        }
    }

    // addGraphicsLevelListener's (research/253 9.4): after a level was applied here.
    std::vector<std::function<void(U32)>>& levelListeners()
    {
        static std::vector<std::function<void(U32)>> listeners;
        return listeners;
    }

    // ------------------------------------------------------------------
    // The retired presets' copies

    // fix-round101: a file nested deeper than a preset is (the official LLSD reader has no limit, and freeing
    // or comparing a deep result overflowed the stack: the login reads every preset of the account's folder)
    // is not read; a setting of a type this viewer does not know is left out.
    bool readPresetFile(const std::string& path, LLSD& out)
    {
        out = LLSD();
        if (!plus::xmlNestingOk(path))
        {
            LL_WARNS("Plus") << "Preset " << path << " is nested too deep; not read" << LL_ENDL;
            return false;
        }
        llifstream infile(path.c_str());
        if (!infile.is_open())
        {
            return false;
        }
        if (LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(out, infile) || !out.isMap())
        {
            return false;
        }
        std::vector<std::string> unknown;
        for (LLSD::map_const_iterator it = out.beginMap(); it != out.endMap(); ++it)
        {
            if ((U32)LLControlGroup::typeStringToEnum(it->second["Type"].asString()) >= (U32)TYPE_COUNT)
            {
                unknown.push_back(it->first);
            }
        }
        for (const std::string& name : unknown)
        {
            out.erase(name);
        }
        return true;
    }

    bool isMarker(const std::string& name)
    {
        return name == SETTING_PRESET || name == SETTING_REVISION;
    }

    bool isTaste(const std::string& name)
    {
        return std::any_of(std::begin(TASTE_SETTINGS), std::end(TASTE_SETTINGS),
                           [&name](const char* taste) { return name == taste; });
    }

    // A copy of a retired preset: the official "Save preset" never writes the revision.
    bool isRetiredCopy(const LLSD& file)
    {
        return file.has(SETTING_REVISION) && file[SETTING_REVISION]["Value"].asInteger() >= 0;
    }

    const RetiredPreset* findRetired(const std::string& id)
    {
        for (const RetiredPreset& preset : RETIRED_PRESETS)
        {
            if (id == preset.id)
            {
                return &preset;
            }
        }
        return nullptr;
    }

    // The saved value (getSaveValue()), as the retired presets' "in use" had it (fix-round87: an
    // unsaved test value of the MCP tools does not count).
    bool sameSavedValue(LLControlVariable& control, const LLSD& value)
    {
        const LLSD current = control.getSaveValue();
        switch (control.type())
        {
        case TYPE_BOOLEAN:
            return current.asBoolean() == value.asBoolean();
        case TYPE_S32:
        case TYPE_U32:
            return current.asInteger() == value.asInteger();
        case TYPE_F32:
            return std::fabs(current.asReal() - value.asReal()) <= F32_TOLERANCE;
        case TYPE_STRING:
            return current.asString() == value.asString();
        default:
            return llsd_equals(current, value);
        }
    }

    // A retired preset's copy whose every setting has its value (the matters of taste of an
    // everyday one aside): the preset was in use.
    bool copyInUse(const LLSD& copy, const RetiredPreset& preset)
    {
        for (LLSD::map_const_iterator it = copy.beginMap(); it != copy.endMap(); ++it)
        {
            if (isMarker(it->first) || (!preset.comparison && isTaste(it->first)))
            {
                continue;
            }
            LLControlVariable* control = gSavedSettings.getControl(it->first).get();
            if (!control || !sameSavedValue(*control, it->second["Value"]))
            {
                return false;
            }
        }
        return true;
    }

    // The retired copies in the account's graphics presets folder (path, content).
    std::vector<std::pair<std::string, LLSD>> retiredCopies(const std::string& dir)
    {
        std::vector<std::pair<std::string, LLSD>> copies;
        if (dir.empty())
        {
            return copies;
        }
        LLDirIterator iter(dir, "*.xml");
        std::string file;
        while (iter.next(file))
        {
            const std::string path = gDirUtilp->add(dir, file);
            LLSD content;
            if (readPresetFile(path, content) && isRetiredCopy(content))
            {
                copies.emplace_back(path, content);
            }
        }
        return copies;
    }

    // ------------------------------------------------------------------
    // The quality levels (featuretable.txt)

    std::string levelName(U32 level)
    {
        return LLFeatureManager::getInstance()->getNameForGraphicsLevel(level);
    }

    // A setting's value at a level as LLFeatureManager::setGraphicsLevel() gives it on this GPU: the
    // smaller of "all" with this GPU's masks (getBaseFeatures(): Intel, Unknown, safe...) and the
    // level's, 0 where a mask makes it unavailable (getRecommendedValue()); the fallback when "all"
    // does not have it.
    F32 levelValue(U32 level, const std::string& name, F32 fallback)
    {
        LLFeatureManager* features = LLFeatureManager::getInstance();
        const LLFeatureList::feature_map_t& base = features->getBaseFeatures();
        LLFeatureList::feature_map_t::const_iterator top = base.find(name);
        if (top == base.end())
        {
            return fallback;
        }
        bool available = top->second.mAvailable;
        F32 value = top->second.mRecommendedLevel;
        if (LLFeatureList* list = features->findMask(levelName(level)))
        {
            LLFeatureList::feature_map_t::const_iterator own = list->getFeatures().find(name);
            if (own != list->getFeatures().end())
            {
                available = available && own->second.mAvailable;
                value = llmin(value, own->second.mRecommendedLevel);
            }
        }
        return available ? value : 0.f;
    }

    // What the official slider does (LLFloaterPreference::onChangeQuality), from the Charis
    // menu and the photo tools: the level's values, the shaders reloaded once. The settings are
    // a level's now, not a saved preset's: "Preset in use" names none.
    void applyLevel(U32 level)
    {
        level = llmin(level, LEVEL_COUNT - 1);
        LL_INFOS("Plus") << "Graphics quality level " << level << " (" << levelName(level) << ")" << LL_ENDL;
        // fix-round22: a compare held in the photo tools ends first, so the level starts from the
        // user's own values (its window sees the end in draw()).
        plus::photomode::compareEnd();
        LLTimer timer;
        LLFeatureManager::getInstance()->setGraphicsLevel(level, true);
        // Changing the level turns the depth of field off (a matter of taste, so the levels do not set it): it is switched on again
        // where the focus is wanted (the photo tools, the photo screen). Only when it is on: a write rebuilds the pipeline.
        if (gSavedSettings.getBOOL("RenderDepthOfField"))
        {
            gSavedSettings.setBOOL("RenderDepthOfField", false);
        }
        gSavedSettings.setU32(SETTING_QUALITY, level);
        gSavedSettings.setU32(SETTING_DEBUG_QUALITY, level);
        if (!gSavedSettings.getString(SETTING_ACTIVE).empty())
        {
            gSavedSettings.setString(SETTING_ACTIVE, "");
        }
        LL_INFOS("Plus") << "Graphics quality level " << level << " applied in " << timer.getElapsedTimeF32() << " s" << LL_ENDL;
        LLFloaterPreference::refreshEnabledGraphics();
        LLPresetsManager::getInstance()->triggerChangeSignal();
        for (const std::function<void(U32)>& listener : levelListeners())
        {
            listener(level);
        }
    }

    void applyComparison()
    {
        applyLevel(COMPARISON_LEVEL);
        gSavedSettings.setF32(SETTING_FAR_CLIP, COMPARISON_FAR_CLIP);
        gSavedSettings.setF32(SETTING_VOLUME_LOD, COMPARISON_VOLUME_LOD);
        gSavedSettings.setU32(SETTING_ANISOTROPIC, COMPARISON_ANISOTROPIC);
        gSavedSettings.setS32(SETTING_SKY_STARS, 0);
        gSavedSettings.setU32(SETTING_OIT, COMPARISON_OIT);
        gSavedSettings.setBOOL(SETTING_TAA, false);
    }

    // Asked by the menu's checks and the photo tools' periodic refresh: cached controls, not looked up by name.
    bool isComparison()
    {
        static LLCachedControl<F32> far_clip(gSavedSettings, SETTING_FAR_CLIP, 0.f);
        static LLCachedControl<F32> volume_lod(gSavedSettings, SETTING_VOLUME_LOD, 0.f);
        static LLCachedControl<U32> anisotropic(gSavedSettings, SETTING_ANISOTROPIC, 0);
        static LLCachedControl<S32> sky_stars(gSavedSettings, SETTING_SKY_STARS, 0);
        static LLCachedControl<U32> oit(gSavedSettings, SETTING_OIT, 0);
        static LLCachedControl<bool> taa(gSavedSettings, SETTING_TAA, false);
        static LLCachedControl<bool> dynamic_lod(gSavedSettings, SETTING_DYNAMIC_LOD, true);
        return plus::presets::graphicsLevel() == COMPARISON_LEVEL
               && std::fabs(far_clip() - COMPARISON_FAR_CLIP) <= F32_TOLERANCE
               && std::fabs(volume_lod() - COMPARISON_VOLUME_LOD) <= F32_TOLERANCE
               && anisotropic() == COMPARISON_ANISOTROPIC
               && sky_stars() == 0
               && oit() == COMPARISON_OIT
               && !taa()
               && dynamic_lod();   // "not by distance" is not the official MidHigh (research/275 L059)
    }

    // The level the official viewer would pick for this PC, with Ultra for a fast GPU.
    U32 recommendedLevel()
    {
        LLFeatureManager* features = LLFeatureManager::getInstance();
        const S32 gpu_class = features->getGPUClass();
        const U32 level = (U32)llclamp(gpu_class, (S32)GPU_CLASS_0, (S32)GPU_CLASS_5);
        return plus::presets::recommendedGraphicsLevel(level, gpu_class);
    }

    // ------------------------------------------------------------------
    // Shaders: the prewarm of the levels next to the one in use (research/111 B-PW)

    // The global shader defines (LLGLSLShader::sGlobalDefines, which every
    // feature library file gets) with a level's settings, from the ones in
    // use: what LLViewerShaderMgr::loadBasicShaders() makes of the settings a
    // level changes (the sun and spot shadows, the probes' level, the official
    // SSR with plus/ssr's define, and since fix-round109 the water mirror's and
    // the avatar reflection's defines, plus/avatarrefl: on Vulkan, where the
    // prewarm runs, they follow their mode alone). Trusted only while it gives
    // the defines in use from the settings in use.
    struct DefineSettings
    {
        S32 shadow_detail = 0;
        S32 probe_level = 0;
        bool ssr = false;
        bool water_mirror = false;
        bool avatar_reflection = false;
    };

    void setDefine(LLGLSLShader::defines_map_t& defines, const char* name, bool on)
    {
        if (on)
        {
            defines[name] = "1";
        }
        else
        {
            defines.erase(name);
        }
    }

    LLGLSLShader::defines_map_t levelDefines(LLGLSLShader::defines_map_t defines, const DefineSettings& settings)
    {
        setDefine(defines, "SUN_SHADOW", settings.shadow_detail >= 1);
        setDefine(defines, "SPOT_SHADOW", settings.shadow_detail >= 2);
        if (defines.count("REFMAP_LEVEL"))     // reflection probes on
        {
            defines["REFMAP_LEVEL"] = std::to_string(llclamp(settings.probe_level, 0, 3));
        }
        setDefine(defines, "SSR", settings.ssr);
        setDefine(defines, "PLUS_SSR", settings.ssr);  // plus::ssr::addShaderDefines: always with SSR
        setDefine(defines, "PLUS_WATER_MIRROR", settings.water_mirror);
        setDefine(defines, "PLUS_AVATAR_REFL", settings.avatar_reflection);
        return defines;
    }

    // The settings the defines follow: in use (level < 0), or a level's (featuretable.txt).
    DefineSettings defineSettings(S32 level)
    {
        DefineSettings in_use;
        in_use.shadow_detail = gSavedSettings.getS32("RenderShadowDetail");
        in_use.probe_level = gSavedSettings.getS32("RenderReflectionProbeLevel");
        in_use.ssr = gSavedSettings.getBOOL("RenderScreenSpaceReflections");
        in_use.water_mirror = gSavedSettings.getU32("PlusWaterMirror") != 0;
        in_use.avatar_reflection = gSavedSettings.getU32("PlusAvatarReflection") != 0;
        if (level < 0)
        {
            return in_use;
        }
        DefineSettings settings;
        settings.shadow_detail = (S32)levelValue(level, "RenderShadowDetail", (F32)in_use.shadow_detail);
        settings.probe_level = (S32)levelValue(level, "RenderReflectionProbeLevel", (F32)in_use.probe_level);
        settings.ssr = levelValue(level, "RenderScreenSpaceReflections", in_use.ssr ? 1.f : 0.f) != 0.f;
        settings.water_mirror = levelValue(level, "PlusWaterMirror", in_use.water_mirror ? 1.f : 0.f) != 0.f;
        settings.avatar_reflection = levelValue(level, "PlusAvatarReflection", in_use.avatar_reflection ? 1.f : 0.f) != 0.f;
        return settings;
    }

    // The levels next to the one in use, compiled on low priority threads into the shader
    // cache: a step of the slider then takes what a second switch takes.
    void prewarmShaders()
    {
        if (!LLRHI::sPrewarmShaders || !LLRHI::sShaderPrewarm)
        {
            return;
        }
        const LLGLSLShader::defines_map_t& current = LLGLSLShader::sGlobalDefines;
        if (levelDefines(current, defineSettings(-1)) != current)
        {
            LL_WARNS("Plus") << "The shader defines in use do not follow from the settings as expected "
                             << "(LLViewerShaderMgr::loadBasicShaders changed, or a reload is pending): no shader prewarm" << LL_ENDL;
            return;
        }
        const S32 level = (S32)plus::presets::graphicsLevel();
        std::vector<LLGLSLShader::defines_map_t> targets;
        for (S32 next : { level - 1, level + 1 })
        {
            if (next < 0 || next >= (S32)LEVEL_COUNT)
            {
                continue;
            }
            const LLGLSLShader::defines_map_t target = levelDefines(current, defineSettings(next));
            if (target != current && std::find(targets.begin(), targets.end(), target) == targets.end())
            {
                targets.push_back(target);
            }
        }
        LLRHI::sPrewarmShaders(current, targets);
    }

    void schedulePrewarm()
    {
        auto timer = std::make_shared<LLTimer>();
        doOnIdleRepeating([timer]()
                          {
                              // research/250: after a reload made in the background, not while it is on its way.
                              if (timer->getElapsedTimeF32() < PREWARM_DELAY_SECONDS || plus::shaderreload::pending())
                              {
                                  return false;
                              }
                              prewarmShaders();
                              return true;
                          });
    }

    // ------------------------------------------------------------------
    // Texts (panel_plus_presets.xml, localized)

    constinit plus::psui::StringsPanel sStrings(STRINGS_FILE);

    std::string text(const std::string& name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
    {
        return sStrings.get(name, args);
    }

    // The reload stops the viewer for a while (seconds; up to about 20 s the
    // first time a combination of settings is used, while its shaders are
    // compiled: research/106). A notice says so, and the work starts once it
    // has been drawn. A second request while the notice is up replaces the
    // first, which has not started: the last one asked for is the one done
    // (fix-round9b 1). research/250: when the reload is made in the background
    // (in_background: the viewer does not stand still; plus/shaderreload shows
    // how far it is), there is no notice and the work is done at the next idle.
    std::function<void()> sPendingWork;     // empty: no notice up

    // research/250: plus/shaderreload makes a level's reload in the background unless the level
    // changes the vintage mode (the emissive buffer and HDR change what the programs write to:
    // made at once then, so the notice above is up).
    bool levelReloadsInBackground(U32 level)
    {
        const F32 vintage_off = gSavedSettings.getBOOL("RenderDisableVintageMode") ? 1.f : 0.f;
        return plus::shaderreload::inBackground() && levelValue(level, "RenderDisableVintageMode", vintage_off) == vintage_off;
    }

    void runAfterNotice(const std::function<void()>& work, bool in_background)
    {
        const bool notice_up = static_cast<bool>(sPendingWork);
        sPendingWork = work;
        if (notice_up)
        {
            return;
        }
        // In the background: at the next idle (not inside the menu's or the photo tools' commit).
        if (!in_background)
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", text("applying")));
        }
        auto frames_left = std::make_shared<S32>(in_background ? 1 : NOTICE_FRAMES);
        doOnIdleRepeating([frames_left]()
                          {
                              if (--*frames_left > 0)
                              {
                                  return false;
                              }
                              // Taken out first: a request made while it runs gets a notice of its own.
                              std::function<void()> latest;
                              latest.swap(sPendingWork);
                              if (latest)
                              {
                                  latest();
                                  schedulePrewarm();  // the levels next to this one now
                              }
                              return true;
                          });
    }

    // ------------------------------------------------------------------
    // The move from the retired presets (once), and their leftovers (every login)

    // The retired presets' backups for "Undo": the one in user_settings, and the old ones left
    // in the accounts' folders (fix-round9b moved them; fix-round103 removed them all after an Undo).
    void removeBackups()
    {
        namespace fs = std::filesystem;
        std::error_code e;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, BACKUP_FILE);
        if (!path.empty() && fs::exists(fsyspath(path), e) && fs::remove(fsyspath(path), e))
        {
            LL_INFOS("Plus") << "Removed the retired graphics presets' backup " << path << LL_ENDL;
        }
        const std::string app_dir = gDirUtilp->getOSUserAppDir();
        if (app_dir.empty())
        {
            return;
        }
        std::error_code ec;
        for (fs::directory_iterator it(fsyspath(app_dir), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code d;
            if (!it->is_directory(d))
            {
                continue;
            }
            const fs::path old = it->path() / BACKUP_FILE;
            if (fs::is_regular_file(old, d) && fs::remove(old, d))
            {
                LL_INFOS("Plus") << "Removed an old copy of the retired graphics presets' backup (" << fsyspath(old).string() << ")" << LL_ENDL;
            }
        }
    }

    // research/183 section 3.3: the retired preset in use (PlusGraphicPreset, and its copy's
    // values still the settings') goes to its level; the user's own settings are kept.
    void migrate(const std::vector<std::pair<std::string, LLSD>>& copies)
    {
        const std::string id = gSavedSettings.getString(SETTING_PRESET);
        const RetiredPreset* preset = findRetired(id);
        bool in_use = false;
        for (const std::pair<std::string, LLSD>& copy : copies)
        {
            if (preset && copy.second[SETTING_PRESET]["Value"].asString() == id && copyInUse(copy.second, *preset))
            {
                in_use = true;
                break;
            }
        }
        removeBackups();
        gSavedSettings.setBOOL(SETTING_MIGRATED, true);
        if (in_use)
        {
            std::vector<std::pair<std::string, LLSD>> kept;
            for (const char* name : KEPT_BY_MIGRATION)
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name).get())
                {
                    kept.emplace_back(name, control->getValue());
                }
            }
            LL_INFOS("Plus") << "The retired Charis graphics preset " << id << " was in use: moved to the quality level "
                             << preset->level << LL_ENDL;
            applyLevel(preset->level);
            for (const std::pair<std::string, LLSD>& setting : kept)
            {
                gSavedSettings.setUntypedValue(setting.first, setting.second);
            }
            schedulePrewarm();
        }
        else
        {
            LL_INFOS("Plus") << "No retired Charis graphics preset in use (" << (id.empty() ? std::string("none applied") : id)
                             << "): the settings are kept" << LL_ENDL;
        }
        if (in_use || !id.empty() || !copies.empty())
        {
            LLStringUtil::format_map_t args;
            args["[LEVEL]"] = in_use ? text("level_" + std::to_string(preset->level)) : std::string();
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", text(in_use ? "migrated" : "migrated_custom", args)));
        }
    }

    // The official "Preset in use" naming a retired copy that is not there names none.
    void clearRetiredActiveName(const std::string& dir)
    {
        const std::string active = gSavedSettings.getString(SETTING_ACTIVE);
        if (active.rfind(DISPLAY_NAME_PREFIX, 0) == 0
            && !gDirUtilp->fileExists(gDirUtilp->add(dir, LLURI::escape(active) + ".xml")))
        {
            gSavedSettings.setString(SETTING_ACTIVE, "");
        }
    }

    void onLoggedIn()
    {
        const std::string dir = LLPresetsManager::getPresetsDir(PRESETS_GRAPHIC);
        const std::vector<std::pair<std::string, LLSD>> copies = retiredCopies(dir);
        if (!gSavedSettings.getBOOL(SETTING_MIGRATED))
        {
            migrate(copies);
        }
        for (const std::pair<std::string, LLSD>& copy : copies)
        {
            LL_INFOS("Plus") << "Removing the copy of a retired Charis graphics preset: " << copy.first << LL_ENDL;
            LLFile::remove(copy.first);
        }
        sCopiesRemoved = true;
        clearRetiredActiveName(dir);
        LLPresetsManager::getInstance()->triggerChangeSignal();
    }

    // Runs every frame until the login is done. The account's folder is named
    // when a login is tried and stays named after a failed one (another name
    // may be tried next): the move waits for STATE_STARTED, when the folder is
    // the account's. The shader prewarm does not depend on the account and is
    // scheduled as soon as a folder is named.
    bool onIdleUntilLoggedIn()
    {
        static bool prewarm_scheduled = false;
        if (gDirUtilp->getLindenUserDir().empty())
        {
            return false;
        }
        if (!prewarm_scheduled)
        {
            prewarm_scheduled = true;
            schedulePrewarm();
        }
        if (LLStartUp::getStartupState() < STATE_STARTED)
        {
            return false;
        }
        onLoggedIn();
        return true;
    }

    // ------------------------------------------------------------------

    // The box drives full OIT in snapshots too: it is part of the boost.
    void syncSnapshotOIT(bool boost)
    {
        if (gSavedSettings.getBOOL(SETTING_OIT_SNAPSHOT) != boost)
        {
            gSavedSettings.setBOOL(SETTING_OIT_SNAPSHOT, boost);
        }
    }

    U32 levelParam(const LLSD& param)
    {
        return (U32)llclamp(param.asInteger(), 0, (S32)LEVEL_COUNT - 1);
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusPresetMigrated, PlusSnapshotBoost; PlusGraphicPreset
        // read once). Levels: featuretable.txt. Preferences > Charis: skins/default/xui/<lang>/panel_preferences_charis.xml
        // ("b_gl_recommended", "c_snapshot_boost", "b_comparison"); the buttons of the photo tools' Quality
        // tab ("btn_gl_*") and its "PlusSnapshotBoost" row.

        // Before the window: the OIT handler only notes that OIT sources are needed.
        if (gSavedSettings.controlExists(SETTING_BOOST) && gSavedSettings.getBOOL(SETTING_BOOST))
        {
            syncSnapshotOIT(true);
        }
        if (LLControlVariable* boost = gSavedSettings.getControl(SETTING_BOOST).get())
        {
            boost->getCommitSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&) { syncSnapshotOIT(value.asBoolean()); });
        }

        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Presets.SetLevel",
            [](LLUICtrl*, const LLSD& param) { plus::presets::requestGraphicsLevel(levelParam(param)); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Presets.SetRecommended",
            [](LLUICtrl*, const LLSD&)
            {
                runAfterNotice([]()
                               {
                                   applyLevel(recommendedLevel());
                                   gSavedSettings.setBOOL(SETTING_BOOST, true);
                               },
                               levelReloadsInBackground(recommendedLevel()));
            });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Presets.ApplyComparison",
                                                                 [](LLUICtrl*, const LLSD&) { runAfterNotice(&applyComparison, levelReloadsInBackground(COMPARISON_LEVEL)); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Presets.IsLevel",
            [](LLUICtrl*, const LLSD& param) { return plus::presets::graphicsLevel() == levelParam(param) && !isComparison(); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Presets.IsComparison",
                                                                 [](LLUICtrl*, const LLSD&) { return isComparison(); });
        // The "Graphics Quality" heading is not a command.
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Presets.Never",
                                                                 [](LLUICtrl*, const LLSD&) { return false; });
    }

    void onUIReady()
    {
        // igpu-safe: with the level in use (a new GPU gets the recommended one: LLViewerWindow's
        // LastGPUString check, "Applying Recommended Features" in the log).
        LL_INFOS("Plus") << "Recommended graphics quality level for this PC (GPU class "
                         << (S32)LLFeatureManager::getInstance()->getGPUClass() << ", " << gGLManager.mVRAM
                         << " MB VRAM): " << recommendedLevel() << "; in use: " << plus::presets::graphicsLevel() << LL_ENDL;
        doOnIdleRepeating(&onIdleUntilLoggedIn);
    }

    plus::FeatureRegistration sRegistration({ "presets", &init, &onUIReady });
}

namespace plus::presets
{
    U32 graphicsLevel()
    {
        static LLCachedControl<U32> quality(gSavedSettings, SETTING_QUALITY, 0);
        return quality;
    }

    F32 graphicsLevelValue(const std::string& name, F32 fallback)
    {
        return levelValue(graphicsLevel(), name, fallback);
    }

    bool comparisonInUse()
    {
        return isComparison();
    }

    void requestGraphicsLevel(U32 level)
    {
        level = llmin(level, LEVEL_COUNT - 1);
        runAfterNotice([level]() { applyLevel(level); }, levelReloadsInBackground(level));
    }

    U32 recommendedGraphicsLevel(U32 level, S32 gpu_class)
    {
        if (gpu_class >= ULTRA_GPU_CLASS && gGLManager.mVRAM >= ULTRA_MIN_VRAM_MB)
        {
            return LEVEL_ULTRA;
        }
        return level;
    }

    void addPresetControlNames(std::vector<std::string>& names)
    {
        LLFeatureList* all = LLFeatureManager::getInstance()->findMask("all");
        if (!all)
        {
            return;
        }
        for (const LLFeatureList::feature_map_t::value_type& feature : all->getFeatures())
        {
            const std::string& name = feature.first;
            if ((name.rfind("Plus", 0) == 0 || name == "RenderOITMode")
                && std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(name);
            }
        }
    }

    void recheckActivePresetName()
    {
        if (!sCopiesRemoved)
        {
            return;     // onLoggedIn() checks it at the login
        }
        clearRetiredActiveName(LLPresetsManager::getPresetsDir(PRESETS_GRAPHIC));
        LLPresetsManager::getInstance()->triggerChangeSignal();
    }

    bool snapshotBoostActive()
    {
        return sBoostDepth > 0;
    }

    bool snapshotInProgress()
    {
        return sSnapshotDepth > 0;
    }

    U32 snapshotSerial()
    {
        return sSnapshotSerial;
    }

    void addSnapshotBoostListener(const std::function<void(bool active)>& listener)
    {
        boostListeners().push_back(listener);
    }

    void addSnapshotListener(const std::function<void(bool in_progress)>& listener)
    {
        snapshotListeners().push_back(listener);
    }

    void addGraphicsLevelListener(const std::function<void(U32 level)>& listener)
    {
        levelListeners().push_back(listener);
    }

    // research/109: device-local memory in use, MB (0: unknown).
    static U64 vramInUseMB()
    {
        U64 textures = 0, usage = 0, budget = 0;
        return LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(textures, usage, budget) ? usage >> 20 : 0;
    }

    static AfterCounters afterCounters()
    {
        AfterCounters c;
        if (LLRHI::sResourceCounts)
        {
            LLRHI::sResourceCounts(c.made, c.released, c.bc7);
        }
        if (LLTextureFetch* fetch = LLAppViewer::getTextureFetch())
        {
            c.http = fetch->getTotalNumHTTPRequests();
        }
        return c;
    }

    // One line every AFTER_LOG_PERIOD for AFTER_LOG_SECONDS after a snapshot
    // (research/109): how long the texture system takes to settle.
    static void startAfterLog()
    {
        const U32 id = ++sAfterLogId;
        sAfterStart = afterCounters();
        const F64 start = LLTimer::getTotalSeconds();
        auto next = std::make_shared<F64>(start);
        doOnIdleRepeating([id, start, next]()
        {
            if (id != sAfterLogId)
            {
                return true;    // a newer snapshot logs from now on
            }
            const F64 now = LLTimer::getTotalSeconds();
            if (now < *next)
            {
                return false;
            }
            *next += AFTER_LOG_PERIOD;
            U64 textures = 0, usage = 0, budget = 0;
            if (LLRHI::sVRAMBudget)
            {
                LLRHI::sVRAMBudget(textures, usage, budget);
            }
            const AfterCounters c = afterCounters();
            LL_INFOS("Plus") << llformat("after the snapshot +%.0f s: VRAM %llu of %llu MB; textures %.0f MB of a limit of %.0f MB%s, "
                                         "discard bias %.2f, pressure %d; since the snapshot: images made %llu, released %llu, "
                                         "BC7 encoded %llu, HTTP fetches %u, fetch queue %d",
                                         now - start, usage >> 20, budget >> 20, sBudget.used_mb, sBudget.limit_mb,
                                         sBudget.held ? " (held)" : "", sBudget.discard_bias, sBudget.pressure,
                                         c.made - sAfterStart.made, c.released - sAfterStart.released, c.bc7 - sAfterStart.bc7,
                                         c.http - sAfterStart.http,
                                         LLAppViewer::getTextureFetch() ? LLAppViewer::getTextureFetch()->getNumRequests() : 0)
                             << LL_ENDL;
            return now - start >= AFTER_LOG_SECONDS;
        });
    }

    bool textureBudgetHeld()
    {
        if (sSnapshotDepth > 0)
        {
            return true;
        }
        if (sSnapshotEnded <= 0.0 || LLTimer::getTotalSeconds() - sSnapshotEnded > BUDGET_HOLD_SECONDS)
        {
            return false;
        }
        const U64 now = vramInUseMB();
        return now && sSnapshotVRAMBefore && now > sSnapshotVRAMBefore + BUDGET_HOLD_ROOM_MB;
    }

    void noteScreenTargets()
    {
        ++sScreenTargetSets;
    }

    void noteTextureBudget(float used_mb, float limit_mb, float discard_bias, int pressure)
    {
        sBudget.used_mb = used_mb;
        sBudget.limit_mb = limit_mb;
        sBudget.discard_bias = discard_bias;
        sBudget.pressure = pressure;
        sBudget.held = textureBudgetHeld();
    }

    SnapshotBoost::SnapshotBoost()
    {
        static LLCachedControl<bool> boost(gSavedSettings, SETTING_BOOST, true);
        mActive = boost && sWithheldDepth == 0;
        mStart = LLTimer::getTotalSeconds();
        mVRAMBefore = vramInUseMB();
        if (sSnapshotDepth++ == 0)
        {
            ++sSnapshotSerial;
            sSnapshotVRAMBefore = mVRAMBefore;  // research/109: the texture budget's hold
            sScreenTargetSets = 0;
        }
        if (LLRHI::sNoteEvent)
        {
            LLRHI::sNoteEvent(llformat("snapshot begins (boost %s), VRAM %llu MB", mActive ? "on" : "off", mVRAMBefore).c_str());
        }
        if (mActive && ++sBoostDepth == 1)
        {
            notifyBoost(true);
        }
        if (sSnapshotDepth == 1)
        {
            notifySnapshot(true);   // the outermost one only (360 snapshots nest)
        }
    }

    SnapshotBoost::~SnapshotBoost()
    {
        if (mActive && --sBoostDepth == 0)
        {
            notifyBoost(false);
        }
        --sSnapshotDepth;
        // research/109: one line a snapshot (the GPU fault report has the same in its frame events).
        const F64 now = LLTimer::getTotalSeconds();
        // "After" is about the peak: nothing a snapshot releases is freed before it ends.
        const std::string line = llformat("snapshot rendered in %.0f ms (boost %s); VRAM in use %llu MB before, %llu MB after; "
                                          "screen and shadow target sets made %u",
                                          (now - mStart) * 1000.0, mActive ? "on" : "off", mVRAMBefore, vramInUseMB(), sScreenTargetSets);
        LL_INFOS("Plus") << line << LL_ENDL;
        if (LLRHI::sNoteEvent)
        {
            LLRHI::sNoteEvent(line.c_str());
        }
        if (sSnapshotDepth == 0)
        {
            notifySnapshot(false);
            sSnapshotEnded = now;
            if (now - mStart >= 1.0)
            {
                startAfterLog();
            }
        }
    }

    SnapshotBoostWithheld::SnapshotBoostWithheld()
    {
        ++sWithheldDepth;
    }

    SnapshotBoostWithheld::~SnapshotBoostWithheld()
    {
        --sWithheldDepth;
    }
}
