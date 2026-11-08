/**
 * @file shaderbake.cpp
 * @brief --bake-shaders: the viewer compiles the shaders of every graphics
 *        quality level and its neighbours into an empty cache folder, without
 *        logging in, and quits (research/159 section 3.1).
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

// The shader bundle (app_settings/vk_shader_bundle/shaders.vkb, research/159)
// holds the SPIR-V of the shaders a release is likely to meet, so a first
// start or a quality level met for the first time compiles nothing. Its keys are
// the hashes of the final sources: the official assembly (LLShaderMgr), the
// Charis rewrites and the global defines. Rather than rebuild those outside,
// the release exe makes them itself: this mode applies each set of settings
// of the grid (app_settings/plus/shaderbake/grid.xml: the quality levels of
// featuretable.txt, research/183, and each with one setting changed) and reloads the shaders into an empty
// cache folder (LLRHI::sBakeShaders: the parallel reload, every program's OIT
// variant, and once the backend's own passes). The same exe at run time
// builds the same sources, so it finds the same keys. Each set starts from the
// defaults of every setting the grid's levels and axes write (applySet), so the
// sets do not depend on the GPU's class the viewer started with or on the order.
//
// The GPU vendor shows in the sources (IS_AMD_CARD for AMD, NVIDIA's texture
// lookups: LLShaderMgr::loadShaderFile reads gGLManager's flags), and only
// there (glslang's output does not depend on the GPU): each set is made once
// for each vendor asked for, with the flags set to that vendor's for the
// reload only (VendorScope), on whichever GPU the bake runs.
//
// Nothing of the user's is changed: the settings are changed in memory only
// and saved (by the quit, as always) to bake_settings-<vendors>.xml in the
// bake folder, not to settings.xml; no login happens (AutoLogin off; the bake
// stops if a login starts all the same). The bake runs at the login screen,
// one set a call of the idle loop (the window keeps being drawn between
// them), and ends with the usual quit from the login screen.
//
// Written to the bake folder: cache/ (the SPIR-V and GL views, for
// tools/pack-shader-bundle.py), grid-<vendors>.txt (the sets, in the form
// tools/pack-shader-bundle.py --plan prints them), report-<vendors>.txt
// (status=ok when done, the counts), keylog-<vendors>.tsv (every build,
// LLRHI_VK_SHADER_KEYLOG).

#include "llviewerprecompiledheaders.h"

#include "shaderbake.h"

#include "plus.h"

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llfeaturemanager.h"
#include "llfile.h"
#include "llgl.h"
#include "llrhi.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "fsyspath.h"
#include "plus/shaderreload/shaderreload.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
extern char** environ;
#endif

namespace
{
    constexpr S32 GRID_VERSION = 2;     // grid.xml's "version" this reader knows (2: "presets" are quality levels)

    using Settings = std::map<std::string, LLSD>;   // setting -> value (as in the grid file)

    struct AxisValue
    {
        std::string label;
        Settings settings;
    };

    struct Axis
    {
        std::string name;
        S32 tier = 1;
        bool adjacent = false;
        std::vector<AxisValue> values;
    };

    struct BakeSet
    {
        std::string preset;     // its quality level in featuretable.txt ("High"), under a GPU mask ("High/Intel")
        std::string id;         // "High", "High+shadows=2"
        Settings overlay;       // set over the preset
        std::string key;        // its shader settings, "Name=value,..." (sorted): sets with the same are made once
    };

    struct Bake
    {
        bool active = false;
        std::string folder;     // UTF-8, absolute
        std::string tag;        // the vendors, joined by '-' (file names)
        std::vector<std::string> vendors;
        std::vector<std::string> env;           // LLRHI_* variables set before the start (not ours)

        std::vector<std::string> presets;
        std::vector<std::string> reset;         // with every axis's and compare's settings
        std::vector<std::string> level_names;   // every setting a level's file writes, but those of reset (applySet)
        std::vector<std::string> compare;
        std::vector<Axis> axes;
        std::map<std::string, LLSD> preset_files;
        std::vector<BakeSet> sets;

        bool started = false;
        bool finished = false;
        size_t next = 0;        // vendor * sets + set
        std::string error;
        LLRHI::ShaderBakeCounts total;
        U32 failed_steps = 0;
        std::chrono::steady_clock::time_point start;
    };

    Bake sBake;

    // ------------------------------------------------------------------
    // Files and the environment

    std::string pathIn(const std::string& name)
    {
        return sBake.folder + gDirUtilp->getDirDelimiter() + name;
    }

    void setEnv(const char* name, const std::string& value)
    {
#if LL_WINDOWS
        _wputenv_s(ll_convert_string_to_wide(name).c_str(), ll_convert_string_to_wide(value).c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    // The LLRHI_* switches the bake's environment has (a switch away from its
    // default changes the keys: the bundle would miss at run time).
    std::vector<std::string> rhiSwitches()
    {
        std::vector<std::string> found;
#if LL_WINDOWS
        wchar_t* block = GetEnvironmentStringsW();
        if (!block)
        {
            return found;
        }
        for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1)
        {
            const std::string text = ll_convert_wide_to_string(entry);
            if (text.rfind("LLRHI_", 0) == 0)
            {
                found.push_back(text);
            }
        }
        FreeEnvironmentStringsW(block);
#else
        for (char** entry = environ; entry && *entry; ++entry)
        {
            if (std::string(*entry).rfind("LLRHI_", 0) == 0)
            {
                found.push_back(*entry);
            }
        }
#endif
        return found;
    }

    bool readLLSD(const std::string& path, LLSD& out)
    {
        out = LLSD();
        if (!plus::xmlNestingOk(path))
        {
            return false;
        }
        llifstream file(path.c_str());
        return file.is_open() && LLSDSerialize::fromXML(out, file) != LLSDParser::PARSE_FAILURE && out.isMap();
    }

    // ------------------------------------------------------------------
    // Values: by the setting's type, so the grid file's numbers compare with
    // the presets' and the defaults whatever LLSD type they were written as.

    LLControlVariable* control(const std::string& name)
    {
        return gSavedSettings.getControl(name).get();
    }

    LLSD typed(LLControlVariable& variable, const LLSD& value)
    {
        switch (variable.type())
        {
        case TYPE_BOOLEAN:
            return LLSD(value.asBoolean());
        case TYPE_S32:
        case TYPE_U32:
            return LLSD(value.asInteger());
        case TYPE_F32:
            return LLSD(value.asReal());
        case TYPE_STRING:
            return LLSD(value.asString());
        default:
            return value;
        }
    }

    // research/183: a quality level's settings as LLFeatureManager::setGraphicsLevel() gives them
    // (the smaller of featuretable.txt's "all" and the level's; no GPU's masks unless gpu_mask names
    // one, such as "Intel": grid.xml's "masks"), in the official preset format ({name: {Value}}),
    // with the slider's position. Not the three settings the slider leaves alone
    // (LLFeatureManager::loadFeatureTables()).
    bool levelSettings(const std::string& level, const std::string& gpu_mask, LLSD& out)
    {
        LLFeatureManager* features = LLFeatureManager::getInstance();
        const S32 index = features->getGraphicsLevelForName(level);
        LLFeatureList* all = features->findMask("all");
        LLFeatureList* own = features->findMask(level);
        LLFeatureList* masked = gpu_mask.empty() ? nullptr : features->findMask(gpu_mask);
        if (index < 0 || !all || !own || (!gpu_mask.empty() && !masked))
        {
            return false;
        }
        LLFeatureList list("bake");
        list.getFeatures() = all->getFeatures();
        list.maskList(*own);
        if (masked)
        {
            list.maskList(*masked);     // the smaller again, as applyBaseMasks() then the level's mask
        }
        out = LLSD::emptyMap();
        for (const LLFeatureList::feature_map_t::value_type& feature : list.getFeatures())
        {
            const std::string& name = feature.first;
            LLControlVariable* variable = control(name);
            if (!variable || name == "RenderGamma" || name == "RenderVBOEnable" || name == "RenderFogRatio")
            {
                continue;
            }
            const F32 value = feature.second.mAvailable ? feature.second.mRecommendedLevel : 0.f;
            out[name]["Value"] = typed(*variable, LLSD((F64)value));
        }
        out["RenderQualityPerformance"]["Value"] = LLSD(index);
        return true;
    }

    // The value as the set lists and the key compare it (tools/pack-shader-bundle.py --plan writes the same).
    std::string canonical(LLControlVariable& variable, const LLSD& value)
    {
        switch (variable.type())
        {
        case TYPE_BOOLEAN:
            return value.asBoolean() ? "1" : "0";
        case TYPE_S32:
        case TYPE_U32:
            return std::to_string(value.asInteger());
        case TYPE_F32:
        {
            char text[32];
            snprintf(text, sizeof(text), "%.4f", value.asReal());
            return text;
        }
        default:
            return value.asString();
        }
    }

    // A setting's value in a set: the set's own, else the preset's, else its
    // default (every setting a key reads is reset to its default first).
    LLSD effective(const std::string& name, const std::string& preset, const Settings& overlay)
    {
        auto it = overlay.find(name);
        if (it != overlay.end())
        {
            return it->second;
        }
        const LLSD& file = sBake.preset_files[preset];
        if (file.has(name))
        {
            return file[name]["Value"];
        }
        LLControlVariable* variable = control(name);
        return variable ? variable->getDefault() : LLSD();
    }

    // ------------------------------------------------------------------
    // The grid

    bool fail(const std::string& why)
    {
        if (sBake.error.empty())
        {
            sBake.error = why;
        }
        LL_WARNS("Plus") << "Shader bake: " << why << LL_ENDL;
        return false;
    }

    void addName(std::vector<std::string>& names, const std::string& name)
    {
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            names.push_back(name);
        }
    }

    // Every setting a level's file writes that reset does not name (levelSettings() lists
    // existing settings only): applySet() puts them back to their defaults.
    void collectLevelNames()
    {
        for (const std::string& preset : sBake.presets)
        {
            const LLSD& file = sBake.preset_files[preset];
            for (LLSD::map_const_iterator it = file.beginMap(); it != file.endMap(); ++it)
            {
                if (std::find(sBake.reset.begin(), sBake.reset.end(), it->first) == sBake.reset.end())
                {
                    addName(sBake.level_names, it->first);
                }
            }
        }
    }

    bool readGrid()
    {
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", "shaderbake", "grid.xml");
        LLSD grid;
        if (!readLLSD(path, grid))
        {
            return fail("cannot read " + path);
        }
        if (grid["version"].asInteger() != GRID_VERSION)
        {
            return fail(path + " is version " + grid["version"].asString() + ", this viewer reads " + std::to_string(GRID_VERSION));
        }
        // Each level, then each level under each GPU mask of "masks" ("High/Intel": what a GPU of
        // that maker gets when it picks the level; a set equal to one before it is left out).
        std::vector<std::string> gpu_masks{ std::string() };
        for (const LLSD& mask : llsd::inArray(grid["masks"]))
        {
            gpu_masks.push_back(mask.asString());
        }
        for (const std::string& gpu_mask : gpu_masks)
        {
            for (const LLSD& id : llsd::inArray(grid["presets"]))
            {
                LLSD file;
                if (!levelSettings(id.asString(), gpu_mask, file))
                {
                    return fail("no quality level " + id.asString() + (gpu_mask.empty() ? "" : " or GPU mask " + gpu_mask) +
                                " in featuretable.txt");
                }
                const std::string preset = gpu_mask.empty() ? id.asString() : id.asString() + "/" + gpu_mask;
                sBake.presets.push_back(preset);
                sBake.preset_files[preset] = file;
            }
        }
        std::vector<std::string> names;     // every setting the grid names
        for (const LLSD& name : llsd::inArray(grid["reset"]))
        {
            addName(sBake.reset, name.asString());
        }
        for (const LLSD& name : llsd::inArray(grid["compare"]))
        {
            addName(sBake.compare, name.asString());
            addName(sBake.reset, name.asString());
        }
        for (const LLSD& entry : llsd::inArray(grid["axes"]))
        {
            Axis axis;
            axis.name = entry["name"].asString();
            axis.tier = entry["tier"].asInteger();
            axis.adjacent = entry["adjacent"].asBoolean();
            for (const LLSD& value : llsd::inArray(entry["values"]))
            {
                AxisValue v;
                v.label = value["label"].asString();
                for (LLSD::map_const_iterator it = value["settings"].beginMap(); it != value["settings"].endMap(); ++it)
                {
                    v.settings[it->first] = it->second;
                    addName(sBake.reset, it->first);
                }
                axis.values.push_back(v);
            }
            if (axis.name.empty() || axis.values.size() < 2)
            {
                return fail("an axis of " + path + " has no name or fewer than two values");
            }
            sBake.axes.push_back(axis);
        }
        for (const std::string& name : sBake.reset)
        {
            if (!control(name))
            {
                return fail("the setting " + name + " of " + path + " does not exist");
            }
        }
        collectLevelNames();
        if (sBake.presets.empty() || sBake.axes.empty())
        {
            return fail(path + " names no preset or no axis");
        }
        return true;
    }

    // Every setting a key compares: the axes' and compare's, sorted.
    std::vector<std::string> keyNames()
    {
        std::set<std::string> names(sBake.compare.begin(), sBake.compare.end());
        for (const Axis& axis : sBake.axes)
        {
            for (const AxisValue& value : axis.values)
            {
                for (const auto& setting : value.settings)
                {
                    names.insert(setting.first);
                }
            }
        }
        return std::vector<std::string>(names.begin(), names.end());
    }

    std::string keyOf(const std::string& preset, const Settings& overlay, const std::vector<std::string>& names)
    {
        std::string key;
        for (const std::string& name : names)
        {
            LLControlVariable* variable = control(name);
            key += (key.empty() ? "" : ",") + name + "=" + canonical(*variable, effective(name, preset, overlay));
        }
        return key;
    }

    // The value of the axis the preset has (all its settings match), or -1.
    S32 presetValue(const Axis& axis, const std::string& preset)
    {
        for (size_t i = 0; i < axis.values.size(); ++i)
        {
            bool same = true;
            for (const auto& setting : axis.values[i].settings)
            {
                LLControlVariable* variable = control(setting.first);
                same = same && canonical(*variable, setting.second) == canonical(*variable, effective(setting.first, preset, Settings()));
            }
            if (same)
            {
                return (S32)i;
            }
        }
        return -1;
    }

    // Its handler writes RenderEnableEmissiveBuffer (HAS_EMISSIVE, applySet), which no key compares.
    constexpr char VINTAGE_SETTING[] = "RenderDisableVintageMode";

    // False when two sets with the same key would make different shaders (the second is left out).
    bool makeSets()
    {
        const std::vector<std::string> names = keyNames();
        LLControlVariable* vintage = control(VINTAGE_SETTING);
        std::map<std::string, std::pair<std::string, std::string>> kept;    // key -> the set's id, its VINTAGE_SETTING
        bool ok = true;
        auto add = [&](const std::string& preset, const std::string& id, const Settings& overlay)
        {
            BakeSet set{ preset, id, overlay, keyOf(preset, overlay, names) };
            const std::string mode = vintage ? canonical(*vintage, effective(VINTAGE_SETTING, preset, overlay)) : std::string();
            const auto [it, added] = kept.emplace(set.key, std::make_pair(id, mode));
            if (added)
            {
                sBake.sets.push_back(set);
            }
            else if (it->second.second != mode)
            {
                ok = fail("the sets " + it->second.first + " and " + id + " have the same key but not the same " +
                          VINTAGE_SETTING + " (HAS_EMISSIVE): the second would not be baked");
            }
        };
        for (const std::string& preset : sBake.presets)
        {
            add(preset, preset, Settings());
            for (S32 tier = 1; tier <= 2; ++tier)
            {
                for (const Axis& axis : sBake.axes)
                {
                    if (axis.tier != tier)
                    {
                        continue;
                    }
                    const S32 own = presetValue(axis, preset);
                    for (S32 i = 0; i < (S32)axis.values.size(); ++i)
                    {
                        const bool next_to = !axis.adjacent || own < 0 || i == own - 1 || i == own + 1;
                        if (i != own && next_to)
                        {
                            add(preset, preset + "+" + axis.name + "=" + axis.values[i].label, axis.values[i].settings);
                        }
                    }
                }
            }
        }
        return ok;
    }

    void writeGrid()
    {
        llofstream out(pathIn("grid-" + sBake.tag + ".txt").c_str());
        for (size_t i = 0; i < sBake.sets.size(); ++i)
        {
            out << i + 1 << '\t' << sBake.sets[i].id << '\t' << sBake.sets[i].key << '\n';
        }
    }

    // ------------------------------------------------------------------
    // One set

    // The vendor's flags while the shaders reload (LLShaderMgr::loadShaderFile reads them).
    class VendorScope
    {
    public:
        explicit VendorScope(const std::string& vendor)
            : mAMD(gGLManager.mIsAMD), mNVIDIA(gGLManager.mIsNVIDIA), mIntel(gGLManager.mIsIntel)
        {
            gGLManager.mIsAMD = vendor == "amd";
            gGLManager.mIsNVIDIA = vendor == "nvidia";
            gGLManager.mIsIntel = vendor == "other";
        }
        ~VendorScope()
        {
            gGLManager.mIsAMD = mAMD;
            gGLManager.mIsNVIDIA = mNVIDIA;
            gGLManager.mIsIntel = mIntel;
        }
        VendorScope(const VendorScope&) = delete;
        VendorScope& operator=(const VendorScope&) = delete;

    private:
        const bool mAMD;
        const bool mNVIDIA;
        const bool mIntel;
    };

    // As LLFeatureManager::setGraphicsLevel(): the settings' own handlers run, the
    // reloads they ask for are held back (the bake reloads once).
    struct NoShaderReload
    {
        const bool was = LLViewerShaderMgr::sSkipReload;
        NoShaderReload() { LLViewerShaderMgr::sSkipReload = true; }
        ~NoShaderReload() { LLViewerShaderMgr::sSkipReload = was; }
        NoShaderReload(const NoShaderReload&) = delete;
        NoShaderReload& operator=(const NoShaderReload&) = delete;
    };

    // The setting holds its default (compared as its type, as the level's values are written).
    bool atDefault(LLControlVariable& variable)
    {
        const LLSD value = variable.getValue();
        const LLSD fallback = variable.getDefault();
        switch (variable.type())
        {
        case TYPE_BOOLEAN:
            return value.asBoolean() == fallback.asBoolean();
        case TYPE_S32:
        case TYPE_U32:
            return value.asInteger() == fallback.asInteger();
        case TYPE_F32:
            return value.asReal() == fallback.asReal();
        case TYPE_STRING:
            return value.asString() == fallback.asString();
        default:
            return false;   // reset it all the same
        }
    }

    // Every set starts from the same settings, whatever the GPU and the set before (research/312
    // section 10). A setting's handlers run when its value changes, and some write other settings
    // the shaders read: RenderDisableVintageMode writes RenderEnableEmissiveBuffer (HAS_EMISSIVE).
    // So a level's settings are put back to their defaults first (their handlers run where one was
    // not at its default), not only overwritten. Without it the first set followed the level the
    // viewer started at (picked by the GPU's benchmark class: a start at Low already had
    // RenderDisableVintageMode off, Low's set did not change it, its handler did not run, and
    // HAS_EMISSIVE was on where Low has it off), and every set followed the one before it (the sets
    // after Low's own kept HAS_EMISSIVE on). The defaults are those of settings.xml: the same on
    // every PC. Done at the start of each set, so no way out of a set has anything to put back.
    // The handlers' render target remakes come once a set: the Batch (declared after the hold, so
    // it ends first) makes them at its end, while the reloads are still held back.
    bool applySet(const BakeSet& set)
    {
        NoShaderReload hold;
        plus::shaderreload::Batch batch;
        for (const std::string& name : sBake.level_names)
        {
            LLControlVariable* variable = control(name);
            if (!atDefault(*variable))
            {
                variable->resetToDefault(true);
            }
        }
        for (const std::string& name : sBake.reset)
        {
            control(name)->resetToDefault(true);
        }
        const LLSD& level = sBake.preset_files[set.preset];
        for (LLSD::map_const_iterator it = level.beginMap(); it != level.endMap(); ++it)
        {
            LLControlVariable* variable = control(it->first);
            variable->setValue(typed(*variable, it->second["Value"]));
        }
        for (const auto& setting : set.overlay)
        {
            LLControlVariable* variable = control(setting.first);
            variable->setValue(typed(*variable, setting.second));
        }
        return true;
    }

    void setShaders()
    {
        LLViewerShaderMgr::instance()->setShaders();
    }

    void runNext()
    {
        const size_t count = sBake.sets.size();
        const std::string& vendor = sBake.vendors[sBake.next / count];
        const BakeSet& set = sBake.sets[sBake.next % count];
        if (!applySet(set))
        {
            return;
        }
        LLRHI::ShaderBakeCounts counts;
        bool made = false;
        {
            VendorScope scope(vendor);
            // The backend's own passes once (their sources do not depend on the vendor or the settings).
            made = LLRHI::sBakeShaders(&setShaders, /*oit*/ true, /*fixed*/ sBake.next == 0, counts);
        }
        LLPipeline::refreshCachedSettings();
        if (!made)
        {
            fail("the shaders could not be reloaded into the cache (no shader cache folder)");
            return;
        }
        LLRHI::ShaderBakeCounts& total = sBake.total;
        total.builds += counts.builds;
        total.compiled += counts.compiled;
        total.cache_hits += counts.cache_hits;
        total.failed_programs += counts.failed_programs;
        total.oit_variants += counts.oit_variants;
        total.fixed_passes += counts.fixed_passes;
        sBake.failed_steps += counts.failed_programs;
        ++sBake.next;
        LL_INFOS("Plus") << "Shader bake " << sBake.next << "/" << count * sBake.vendors.size() << ": " << vendor << ", " << set.id
                         << ": " << counts.builds << " builds, " << counts.compiled << " compiled, " << counts.oit_variants
                         << " OIT variants" << (counts.fixed_passes ? ", " + std::to_string(counts.fixed_passes) + " passes of the backend" : "")
                         << " in " << counts.seconds << " s" << LL_ENDL;
    }

    // ------------------------------------------------------------------
    // Start and end

    bool start()
    {
        sBake.started = true;
        sBake.start = std::chrono::steady_clock::now();
        if (!LLRHI::sVulkanGL || !LLRHI::sBakeShaders)
        {
            return fail("not drawing with Vulkan (the bake makes the Vulkan backend's SPIR-V)");
        }
        if (!readGrid())
        {
            return false;
        }
        if (sBake.vendors.empty())
        {
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", "shaderbake", "grid.xml");
            LLSD grid;
            readLLSD(path, grid);
            for (const LLSD& vendor : llsd::inArray(grid["vendors"]))
            {
                sBake.vendors.push_back(vendor.asString());
            }
        }
        for (const std::string& vendor : sBake.vendors)
        {
            if (vendor != "nvidia" && vendor != "amd" && vendor != "other")
            {
                return fail("unknown vendor " + vendor + " (nvidia, amd, other)");
            }
        }
        if (sBake.vendors.empty())
        {
            return fail("no vendor to bake for");
        }
        if (!makeSets())
        {
            return false;
        }
        writeGrid();
        LL_INFOS("Plus") << "Shader bake: " << sBake.sets.size() << " sets of settings x " << sBake.vendors.size() << " vendors into "
                         << sBake.folder << LL_ENDL;
        return true;
    }

    void finish()
    {
        sBake.finished = true;
        const F64 seconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - sBake.start).count();
        const size_t units = sBake.sets.size() * sBake.vendors.size();
        const bool ok = sBake.error.empty() && units && sBake.next == units;
        const LLRHI::ShaderBakeCounts& t = sBake.total;
        {
            llofstream out(pathIn("report-" + sBake.tag + ".txt").c_str());
            out << "status=" << (ok ? "ok" : "failed") << '\n'
                << "error=" << sBake.error << '\n'
                << "viewer=" << LLVersionInfo::instance().getChannelAndVersion() << '\n'
                << "gpu=" << gGLManager.mGLRenderer << '\n'
                << "vendors=" << sBake.tag << '\n'
                << "sets=" << sBake.sets.size() << '\n'
                << "done=" << sBake.next << '/' << units << '\n'
                << "builds=" << t.builds << '\n'
                << "compiled=" << t.compiled << '\n'
                << "cache_hits=" << t.cache_hits << '\n'
                << "oit_variants=" << t.oit_variants << '\n'
                << "fixed_passes=" << t.fixed_passes << '\n'
                << "failed_steps=" << sBake.failed_steps << '\n'
                << "seconds=" << (S64)std::lround(seconds) << '\n';
            for (const std::string& variable : sBake.env)
            {
                out << "env=" << variable << '\n';
            }
        }
        LL_INFOS("Plus") << "Bake: " << sBake.next << " of " << units << " (" << sBake.sets.size() << " settings sets x "
                         << sBake.vendors.size() << " vendors), " << t.builds << " builds (" << t.compiled << " compiled, "
                         << t.cache_hits << " already), " << t.oit_variants << " OIT variants, " << t.fixed_passes
                         << " passes of the backend (" << sBake.failed_steps << " not set up), " << seconds << " s"
                         << (ok ? "" : "; failed: " + sBake.error) << LL_ENDL;
        LLAppViewer::instance()->requestQuit();
    }

    // At each idle until it is done. The login screen first: the shaders were
    // loaded once with the user's settings by then, and every viewer system
    // the settings' handlers touch is up. One set a call.
    bool onIdle()
    {
        if (sBake.finished)
        {
            return true;
        }
        const EStartupState state = LLStartUp::getStartupState();
        if (state < STATE_LOGIN_WAIT)
        {
            return false;
        }
        if (state > STATE_LOGIN_WAIT)
        {
            fail("a login started (the bake runs at the login screen only)");
        }
        else if (!sBake.started)
        {
            start();
        }
        else
        {
            runNext();
        }
        if (!sBake.error.empty() || sBake.next >= sBake.sets.size() * sBake.vendors.size())
        {
            finish();
            return true;
        }
        return false;
    }
}

namespace plus::shaderbake
{
    void configure(const std::string& folder, const std::string& vendors)
    {
        if (sBake.active || folder.empty())
        {
            return;
        }
        fsyspath path(folder);
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (!ec)
        {
            path = absolute;
        }
        sBake.folder = path.string();
        std::filesystem::create_directories(path / "cache", ec);
        sBake.active = true;

        std::stringstream list(vendors);
        std::string vendor;
        while (std::getline(list, vendor, ','))
        {
            LLStringUtil::trim(vendor);
            if (!vendor.empty())
            {
                sBake.vendors.push_back(vendor);
            }
        }
        sBake.tag = sBake.vendors.empty() ? std::string("all") : vendors;
        LLStringUtil::replaceChar(sBake.tag, ',', '-');
        LLStringUtil::replaceChar(sBake.tag, ' ', '-');

        // Read before the Vulkan device is made (LLVKShader::cacheDir, the bundle at the first miss).
        sBake.env = rhiSwitches();
        setEnv("LLRHI_VK_SHADER_CACHE", pathIn("cache"));
        setEnv("LLRHI_VK_SHADER_BUNDLE", "0");      // every build compiles or reads the bake's cache
        if (LLStringUtil::getenv("LLRHI_VK_SHADER_KEYLOG").empty())
        {
            setEnv("LLRHI_VK_SHADER_KEYLOG", pathIn("keylog-" + sBake.tag + ".tsv"));
        }
        LLRHI::sShaderPrewarm = false;  // after login only anyway; the bake makes every OIT variant itself
        LLRHI::sOITSources = true;      // the programs keep the sources OIT variants are made from

        // The settings in memory only: the quit saves them here, not to settings.xml.
        gSavedSettings.setString("ClientSettingsFile", pathIn("bake_settings-" + sBake.tag + ".xml"));
        if (gSavedSettings.controlExists("AutoLogin"))
        {
            gSavedSettings.setBOOL("AutoLogin", false);
        }
        // A viewer of the user's may be running: the bake touches none of its files (its own cache
        // folder and settings file; a second instance logs apart), so it does not stop at "already running".
        if (gSavedSettings.controlExists("AllowMultipleViewers"))
        {
            gSavedSettings.setBOOL("AllowMultipleViewers", true);
        }
        LL_INFOS("Plus") << "Shader bake into " << sBake.folder << " (vendors: " << (vendors.empty() ? "all" : vendors)
                         << "): the viewer quits at the login screen once it is done" << LL_ENDL;
        for (const std::string& variable : sBake.env)
        {
            LL_WARNS("Plus") << "Shader bake with " << variable << " set: keys away from the defaults miss at run time" << LL_ENDL;
        }
        doOnIdleRepeating(&onIdle);
    }

    bool active()
    {
        return sBake.active;
    }
}
