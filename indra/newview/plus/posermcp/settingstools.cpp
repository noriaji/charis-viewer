/**
 * @file settingstools.cpp
 * @brief MCP tools that read and change the viewer's settings: settings_list,
 *        settings_get, settings_set, settings_revert (settingstools.h).
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

// Which settings: every setting of the photo tools' catalog
// (app_settings/plus/phototools/catalog.xml, with its labels and ranges), the
// short list EXTRA_SETTINGS below (render settings for measurements, with
// ranges), and (vk-mcp2, "the debug settings") every other setting whose name
// starts with Plus, Render or Debug (OPEN_PREFIXES), of a type settings_set
// checks and not hidden from the Debug Settings window: checked by type, and
// by range where LIMITS names one (sizes, counts that allocate, divisors). A
// name that looks like anything private or dangerous (isDenied(): the MCP
// server's own settings, login, accounts, paths, URLs, network, crash
// switches) is refused even when a list names it. Nothing else can be read or
// written.
//
// Saving: settings_set without persist writes the control's unsaved value
// (LLControlVariable::setValue(value, false)): the value the user saved stays
// the control's "save value", which is what every save of settings.xml writes
// (LLControlGroup::saveToFile writes getSaveValue()), at quit and in between.
// So a test value is not written to the user's file, even after a crash, and
// nothing has to be put back before a save. When the server stops, such values
// go back to the ones from before (settingsServerStopped()). persist true
// writes the saved value, as the viewer's own windows do.
//
// That holds while nothing turns an unsaved value into the saved one. The
// places that remember a setting and write it back later keep the two apart
// (fix-round87/90): Preferences' cancel (LLPanelPreference, also run by OK and
// by closing the window), the photo tools' compare and reset, and the graphics
// presets' backup. A window the user works in that writes the value it shows
// (a slider moved, "save my preset") saves what it shows, as for any value.
//
// Threads: the handlers parse their arguments on the connection thread and do
// everything else in run(), on the main thread. The table, the history and the
// list of unsaved values are only touched there, so they need no lock.
// Nothing runs per frame.

#include "llviewerprecompiledheaders.h"

#include "settingstools.h"

#include "photomode/phototools.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lltimer.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>

namespace json = boost::json;
using plus::mcp::Tool;
using plus::mcp::ToolResult;
using plus::posermcp::JobProgress;
using plus::posermcp::MainThreadRunner;

namespace
{
    constexpr char        CATALOG_FILE[]     = "plus/phototools/catalog.xml"; // in app_settings/
    constexpr char        EXTRA_TAB[]        = "render";  // the tab id given to EXTRA_SETTINGS
    constexpr std::size_t HISTORY_CAPACITY   = 200;       // settings_set calls kept for settings_revert
    constexpr std::size_t MAX_NAMES_PER_CALL = 200;
    constexpr std::size_t MAX_LIST_ROWS      = 200;       // settings_list rows per call (offset pages on)
    constexpr std::size_t MAX_COMMENT_LENGTH = 300;       // of a setting's comment in settings_list
    constexpr std::size_t MAX_STRING_LENGTH  = 256;
    constexpr double      LIST_TIMEOUT       = 15.0;
    constexpr double      SET_TIMEOUT        = 30.0;      // a setting may rebuild the pipeline (seconds)

    // Render settings besides the catalog, for measurements and A/B comparisons. Ranges are what
    // the viewer's own windows offer (a little wider where a test needs it).
    struct Extra
    {
        const char* name;
        bool        ranged;
        double      min;
        double      max;
    };
    const Extra EXTRA_SETTINGS[] = {
        { "RenderFarClip", true, 32, 1024 },
        { "RenderShadowDetail", true, 0, 2 },
        { "RenderReflectionProbeDetail", true, -1, 2 },
        { "RenderReflectionProbeLevel", true, 0, 3 },
        { "RenderOITMode", true, 0, 2 },
        { "RenderVolumeLODFactor", true, 0.5, 8 },
        { "RenderTerrainDetail", true, 0, 1 },
        { "RenderTerrainLODFactor", true, 1, 8 },
        { "RenderTerrainPBRDetail", true, -4, 0 },
        { "RenderTreeLODFactor", true, 0, 2 },
        { "RenderAvatarLODFactor", true, 0, 1 },
        { "RenderAvatarMaxNonImpostors", true, 0, 66 },
        { "RenderAvatarMaxComplexity", true, 0, 10000000 },
        { "RenderMaxNodeSize", true, 4096, 262144 },
        { "RenderMaxPartCount", true, 0, 8192 },
        { "RenderMaxTextureResolution", true, 512, 2048 },
        { "RenderFSAAType", true, 0, 2 },
        { "RenderHeroProbeResolution", true, 256, 2048 },
        { "RenderShadowResolutionScale", true, 0.25, 1 },
        { "RenderCASSharpness", true, 0, 1 },
        { "RenderDeferredSSAO", false, 0, 0 },
        { "RenderScreenSpaceReflections", false, 0, 0 },
        { "RenderTransparentWater", false, 0, 0 },
        { "RenderMirrors", false, 0, 0 },
        { "RenderGlow", false, 0, 0 },
        { "RenderDynamicLOD", false, 0, 0 },
        { "RenderVSyncEnable", false, 0, 0 },
    };

    // ---- the open set (vk-mcp2) ----

    // Settings of these prefixes besides the two lists above (the top of the file), with their tab.
    struct OpenPrefix
    {
        const char* prefix;
        const char* tab;
    };
    const OpenPrefix OPEN_PREFIXES[] = { { "Plus", "plus" }, { "Render", "render" }, { "Debug", "debug" } };

    // Ranges for settings of the open set whose wrong value costs more than a wrong look: the sizes of
    // render targets, caches and pictures, counts that allocate or start threads, divisors and shifts.
    // Each takes in its default and what the viewer's own windows offer. The rest of the open set is
    // checked by type only.
    const Extra LIMITS[] = {
        { "RenderAnisotropicLevel", true, 0, 16 },
        { "RenderLocalLightCount", true, 0, 1024 },
        { "RenderScreenSpaceReflectionIterations", true, 0, 256 },
        { "RenderScreenSpaceReflectionGlossySamples", true, 0, 64 },
        { "RenderScreenSpaceReflectionsResolutionMultiplier", true, 0.1, 1 },
        { "RenderMaxVRAMBudget", true, 0, 131072 },
        { "RenderTextureVRAMDivisor", true, 1, 16 },
        { "RenderMaxTextureIndex", true, 1, 32 },
        { "RenderSpecularResX", true, 64, 4096 },
        { "RenderSpecularResY", true, 64, 4096 },
        { "RenderShadowBlurSamples", true, 1, 32 },
        { "RenderFSAASamples", true, 0, 16 },
        { "RenderGlowIterations", true, 0, 16 },
        { "RenderGlowResolutionPow", true, 6, 11 },
        { "RenderMaxVBOSize", true, 16, 16384 },
        { "RenderReflectionProbeCount", true, 1, 512 },
        { "RenderReflectionProbeResolution", true, 32, 256 },
        { "RenderReflectionRes", true, 16, 2048 },
        { "RenderResolutionDivisor", true, 1, 8 },
        { "RenderWaterRefResolution", true, 64, 2048 },
        { "RenderTerrainPBRPlanarSampleCount", true, 1, 3 },
        { "PlusVolumetricSamples", true, 0, 128 },
        { "PlusSSRIterations", true, 1, 256 },
        { "PlusUpscaleScale", true, 0.5, 1 },
        { "PlusAvatarReflectionScale", true, 0.25, 1 },
        { "PlusCullThreads", true, 0, 4 },
        { "PlusJ2CDecoderThreads", true, 1, 8 },
        { "PlusSubtitleThreads", true, 1, 16 },
        { "PlusDetachedMaxWindows", true, 1, 8 },
        { "PlusDetachedMaxFps", true, 0, 240 },
        { "PlusFpsLimit", true, 0, 1000 },
        { "PlusPhotoShotSize", true, 0, 4 },
        { "PlusWorldFreezeMaxPending", true, 0, 1000000 },
        { "PlusChatConsoleMaxLines", true, 1, 200 },
    };

    // The open set's tab of a name (nullptr: not of the open set).
    const char* openTab(const std::string& name)
    {
        for (const OpenPrefix& open : OPEN_PREFIXES)
            if (name.compare(0, std::strlen(open.prefix), open.prefix) == 0)
                return open.tab;
        return nullptr;
    }

    const Extra* findLimit(const std::string& name)
    {
        for (const Extra& limit : LIMITS)
            if (name == limit.name)
                return &limit;
        return nullptr;
    }

    // A free text value of the open set (not one of the catalog's choices): no piece of a path or a
    // URL, as some settings name a preset or a file to load.
    bool isPlainText(const std::string& text)
    {
        return text.find_first_of("/\\:") == std::string::npos && text.find("..") == std::string::npos;
    }

    // ---- refused names ----

    // Anywhere in the name (lower case).
    const char* const DENIED_PARTS[] = { "token",   "password", "passwd",   "secret",   "login",    "logoff",
                                         "cert",    "credential", "proxy",  "socks",    "mcp",      "curl",
                                         "crash",   "sandbox",  "security", "account",  "email",    "username",
                                         "firstname", "lastname" };
    // Whole words of the name (CamelCase split, lower case): "dir" refuses "CacheDir" but not "Direction", "url"
    // refuses "SupportURL" but not "RenderDeferredBlurLight".
    const char* const DENIED_WORDS[] = { "key", "keys", "path", "paths", "dir", "dirs", "directory", "file", "files", "folder",
                                         "url", "urls", "uri", "slurl" };
    // Leading part of the name.
    // (Not "Debug" or "RenderDebug": the Debug Settings' display switches are for measurements; the
    // dangerous ones are named below.)
    const char* const DENIED_PREFIXES[] = { "PlusPoserMcp", "Update", "Crash", "Http", "Browser", "Voice", "Media", "QAMode",
                                            "UserLogin", "NoVerifySSL", "Cache", "InstallLanguage",
                                            // the update check and the statistics talk to servers; the radar's
                                            // reports speak in the chat; keywords and protected folders are the
                                            // user's own lists
                                            "PlusUpdateCheck", "PlusSend", "PlusRadarReport", "PlusKeywords",
                                            "PlusProtectedFolders",
                                            // RLVa: remote scripted viewer controls are switched on by the user, never through the server
                                            "RestrainedLove", "RLVa",
                                            // appearance requests to the server, plugin watchdogs, forced failures
                                            "DebugForce", "DebugPlugin", "DebugAvatarExperimental", "DebugAvatarAppearanceService" };
    const char* const DENIED_NAMES[] = { "UserLoginInfo", "NoVerifySSLCert", "CurrentGrid", "CmdLineGridChoice",
                                         "ClientSettingsFile", "PerAccountSettingsFile",
                                         // kept by the viewer (a crash guard, a one-time move of the user's
                                         // settings): not to be set by hand
                                         "PlusGpuDrivenDGCLostCount", "PlusPresetMigrated",
                                         // other avatars' ids
                                         "PlusTtsOverrides",
                                         // a GL debug context (none under Vulkan), the session's own switches
                                         "RenderDebugGLSession", "DebugSession", "DebugWindowProc" };

    std::string lowered(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    std::vector<std::string> camelWords(const std::string& name)
    {
        std::vector<std::string> words;
        std::string              word;
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            const unsigned char c = name[i];
            if (!std::isalnum(c))
            {
                if (!word.empty())
                    words.push_back(lowered(word));
                word.clear();
                continue;
            }
            if (!word.empty())
            {
                const unsigned char prev  = name[i - 1];
                const bool          next_lower = i + 1 < name.size() && std::islower((unsigned char)name[i + 1]);
                const bool          starts = (std::isupper(c) && (std::islower(prev) || std::isdigit(prev) || (std::isupper(prev) && next_lower))) ||
                                    (std::isdigit(c) != 0) != (std::isdigit(prev) != 0);
                if (starts)
                {
                    words.push_back(lowered(word));
                    word.clear();
                }
            }
            word += (char)c;
        }
        if (!word.empty())
            words.push_back(lowered(word));
        return words;
    }

    bool isDenied(const std::string& name)
    {
        for (const char* denied : DENIED_NAMES)
            if (name == denied)
                return true;
        for (const char* prefix : DENIED_PREFIXES)
            if (name.compare(0, std::strlen(prefix), prefix) == 0)
                return true;
        const std::string lower = lowered(name);
        for (const char* part : DENIED_PARTS)
            if (lower.find(part) != std::string::npos)
                return true;
        for (const std::string& word : camelWords(name))
            for (const char* denied : DENIED_WORDS)
                if (word == denied)
                    return true;
        return false;
    }

    const char DENIED_RULE[] =
        "Settings of the photo tools and settings whose names start with Plus, Render or Debug can be read or "
        "changed (settings_list shows them). Always refused: the MCP server's own settings (PlusPoserMcp*), names "
        "containing token, password, secret, login, cert, proxy, crash, account or email, names with the word "
        "key, path, dir, file, folder or url, names starting with Update, Crash, Cache, Http, Browser, Voice, Media, QAMode, "
        "PlusUpdateCheck, PlusSend, PlusRadarReport, PlusKeywords, DebugForce, DebugPlugin, settings hidden from the "
        "Debug Settings window, and a few named ones (PlusGpuDrivenDGCLostCount, PlusTtsOverrides, DebugSession, ...).";

    // ---- the table of allowed settings ----

    struct Option
    {
        double      value = 0.0;
        std::string labelEn, labelJa;
    };

    struct Allowed
    {
        std::string         name;
        std::string         tab;  // the catalog's tab id, or EXTRA_TAB
        std::string         kind; // the catalog's kind (slider, check, combo, look); empty for EXTRA_SETTINGS
        std::string         labelEn, labelJa;
        bool                ranged = false;
        double              min = 0.0, max = 0.0;
        std::vector<Option> options;
        int                 rebuild = 0;
        bool                open = false; // of the open set (OPEN_PREFIXES): a string is free text (isPlainText)
    };

    std::vector<Allowed>          sAllowed;     // catalog order, then EXTRA_SETTINGS, then the open set by name
    std::map<std::string, size_t> sAllowedIndex;
    bool                          sTableBuilt = false;

    bool isSupportedType(eControlType type, bool strings)
    {
        switch (type)
        {
            case TYPE_BOOLEAN:
            case TYPE_S32:
            case TYPE_U32:
            case TYPE_F32:
            case TYPE_COL3:
            case TYPE_COL4:
            case TYPE_VEC3:
                return true;
            case TYPE_STRING:
                return strings; // the catalog's look list (PlusLook) and the open set; not the extra list
            default:
                return false;   // LLSD, rectangles, quaternions, double vectors: no check for them
        }
    }

    void addAllowed(Allowed allowed, bool strings)
    {
        if (allowed.name.empty() || sAllowedIndex.count(allowed.name) || isDenied(allowed.name) ||
            !gSavedSettings.controlExists(allowed.name))
            return;
        LLControlVariablePtr control = gSavedSettings.getControl(allowed.name);
        if (!control || !isSupportedType(control->type(), strings))
            return;
        sAllowedIndex[allowed.name] = sAllowed.size();
        sAllowed.push_back(std::move(allowed));
    }

    // Main thread, once (the catalog does not change while the viewer runs).
    void buildTable()
    {
        if (sTableBuilt)
            return;
        sTableBuilt = true;

        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, CATALOG_FILE);
        llifstream        file(path.c_str());
        LLSD              catalog;
        if (!file.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(catalog, file) || !catalog.isArray())
        {
            LL_WARNS("PlusPoserMcp") << "Cannot read " << path << "; the settings tools offer the render settings only" << LL_ENDL;
        }
        else
        {
            for (const LLSD& entry : llsd::inArray(catalog))
            {
                if (!entry.has("setting"))
                    continue; // a heading or a place
                Allowed a;
                a.name    = entry["setting"].asString();
                a.tab     = entry["tab"].asString();
                a.kind    = entry["kind"].asString();
                a.labelEn = entry["label_en"].asString();
                a.labelJa = entry["label_ja"].asString();
                a.rebuild = entry["rebuild"].asInteger();
                if (entry.has("min") || entry.has("max"))
                {
                    // As the photo tools read them (phototools.cpp): min 0 and max 1 when left out.
                    a.ranged = true;
                    a.min    = entry["min"].asReal();
                    a.max    = entry.has("max") ? entry["max"].asReal() : 1.0;
                }
                else if (a.kind == "check")
                {
                    a.ranged = true; // a check box of a number setting: 0 or 1
                    a.min    = 0.0;
                    a.max    = 1.0;
                }
                for (const LLSD& option : llsd::inArray(entry["options"]))
                {
                    Option o;
                    o.value   = std::atof(option["value"].asString().c_str());
                    o.labelEn = option["label_en"].asString();
                    o.labelJa = option["label_ja"].asString();
                    a.options.push_back(std::move(o));
                }
                addAllowed(std::move(a), true);
            }
        }

        for (const Extra& extra : EXTRA_SETTINGS)
        {
            Allowed a;
            a.name   = extra.name;
            a.tab    = EXTRA_TAB;
            a.ranged = extra.ranged;
            a.min    = extra.min;
            a.max    = extra.max;
            addAllowed(std::move(a), false);
        }

        // The open set, in name order (the control table is a hash map).
        struct Collect : public LLControlGroup::ApplyFunctor
        {
            std::vector<std::string> names;
            void apply(const std::string& name, LLControlVariable* control) override
            {
                if (control && !control->isHiddenFromSettingsEditor() && openTab(name))
                    names.push_back(name);
            }
        } collect;
        gSavedSettings.applyToAll(&collect);
        std::sort(collect.names.begin(), collect.names.end());
        for (const std::string& name : collect.names)
        {
            Allowed a;
            a.name = name;
            a.tab  = openTab(name);
            a.open = true;
            if (const Extra* limit = findLimit(name))
            {
                a.ranged = true;
                a.min    = limit->min;
                a.max    = limit->max;
            }
            addAllowed(std::move(a), true);
        }
        LL_INFOS("PlusPoserMcp") << "Settings tools: " << sAllowed.size() << " settings can be read and changed" << LL_ENDL;
    }

    const Allowed* findAllowed(const std::string& name)
    {
        const auto it = sAllowedIndex.find(name);
        return it == sAllowedIndex.end() ? nullptr : &sAllowed[it->second];
    }

    // ---- the session's changes ----

    struct Change
    {
        std::string name;
        LLSD        before;      // the value in use before the call
        LLSD        beforeSaved; // the value that would have been saved before the call
        LLSD        after;       // the value in use after the call
        bool        persist = false;
    };

    struct Entry
    {
        std::uint64_t       id = 0;
        std::vector<Change> changes;
    };

    std::deque<Entry> sHistory; // oldest first
    std::uint64_t     sNextEntryId = 1;

    // Settings whose value in use is an unsaved one written by settings_set (persist false):
    // the value from before, the value the tool left (put back only while it is still that), and
    // the saved value then (fix-round100: the user saving since - even the value shown, as a
    // checkbox clicked twice does - is a change too, and their saved value is not taken back).
    struct Unsaved
    {
        LLSD original;
        LLSD written;
        LLSD saved;
    };
    std::map<std::string, Unsaved> sUnsaved;

    LLSD savedValueOf(const std::string& name)
    {
        LLControlVariablePtr control = gSavedSettings.getControl(name);
        return control ? control->getSaveValue() : LLSD();
    }

    // original: by value (a caller may pass the entry's own, which this may erase).
    void noteUnsaved(const std::string& name, LLSD original, const LLSD& now)
    {
        const auto it = sUnsaved.find(name);
        if (it == sUnsaved.end())
        {
            if (!llsd_equals(original, now))
                sUnsaved[name] = { original, now, savedValueOf(name) };
        }
        else if (llsd_equals(it->second.original, now))
            sUnsaved.erase(it); // back where it started
        else
        {
            it->second.written = now;
            it->second.saved   = savedValueOf(name);
        }
    }

    // The record's value is still in use and nobody saved over it since.
    bool stillAsWritten(const std::string& name, const Unsaved& unsaved, const LLSD& current)
    {
        return llsd_equals(current, unsaved.written) && llsd_equals(savedValueOf(name), unsaved.saved);
    }

    // fix-round93: true when somebody else (the user, a preset) changed a setting since the tool last
    // wrote it: its record (the original from before) is out of date and is dropped. A value equal to
    // the one the tool wrote, with the saved value as it was then, is taken as unchanged (no guessing
    // beyond that).
    bool forgetIfChangedSince(const std::string& name, const LLSD& current)
    {
        const auto it = sUnsaved.find(name);
        if (it == sUnsaved.end() || stillAsWritten(name, it->second, current))
            return false;
        sUnsaved.erase(it);
        return true;
    }

    // ---- values ----

    struct ToolError : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    double niceFloat(double value)
    {
        if (!std::isfinite(value))
            return 0.0;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        return std::strtod(buffer, nullptr);
    }

    std::string typeName(eControlType type)
    {
        return LLControlGroup::typeEnumToString(type);
    }

    json::value toJson(const LLSD& value, eControlType type)
    {
        switch (type)
        {
            case TYPE_BOOLEAN:
                return value.asBoolean();
            case TYPE_S32:
                return (std::int64_t)value.asInteger();
            case TYPE_U32:
                return (std::uint64_t)(U32)value.asInteger();
            case TYPE_F32:
                return niceFloat(value.asReal());
            case TYPE_STRING:
                return json::string(value.asString());
            case TYPE_COL3:
            case TYPE_COL4:
            case TYPE_VEC3:
            {
                json::array list;
                for (const LLSD& component : llsd::inArray(value))
                    list.push_back(niceFloat(component.asReal()));
                return list;
            }
            default:
                return nullptr;
        }
    }

    // In the value's own words for the log line (an allowed setting: nothing private).
    std::string logText(const LLSD& value, eControlType type)
    {
        return json::serialize(toJson(value, type));
    }

    bool numberOf(const json::value& value, double& out)
    {
        if (value.is_int64())
            out = (double)value.get_int64();
        else if (value.is_uint64())
            out = (double)value.get_uint64();
        else if (value.is_double())
            out = value.get_double();
        else
            return false;
        return true;
    }

    std::string rangeText(const Allowed& a)
    {
        return llformat("%g to %g", a.min, a.max);
    }

    // The value for the control, checked against the setting's type, the catalog's range or
    // choices, and EXTRA_SETTINGS' range. Throws ToolError with the reason.
    LLSD parseValue(const json::value& value, const Allowed& a, eControlType type)
    {
        const std::string& name = a.name;
        if (type == TYPE_BOOLEAN)
        {
            if (value.is_bool())
                return LLSD::Boolean(value.get_bool());
            double d = 0.0;
            if (numberOf(value, d) && (d == 0.0 || d == 1.0))
                return LLSD::Boolean(d != 0.0);
            throw ToolError(name + ": expected true or false (Boolean)");
        }
        if (type == TYPE_STRING)
        {
            if (!value.is_string())
                throw ToolError(name + ": expected a string");
            const std::string text(value.get_string());
            if (text.size() > MAX_STRING_LENGTH)
                throw ToolError(name + ": the string is too long (at most " + std::to_string(MAX_STRING_LENGTH) + " bytes)");
            if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
                throw ToolError(name + ": the string must not contain control characters");
            if (a.open && !isPlainText(text))
                throw ToolError(name + ": the string must not contain / \\ : or .. (no paths or URLs)");
            return LLSD::String(text);
        }
        if (type == TYPE_COL3 || type == TYPE_COL4 || type == TYPE_VEC3)
        {
            const std::size_t count = type == TYPE_COL4 ? 4 : 3;
            if (!value.is_array() || value.get_array().size() != count)
                throw ToolError(name + ": expected an array of " + std::to_string(count) + " numbers (" + typeName(type) + ")");
            LLSD list = LLSD::emptyArray();
            for (const json::value& component : value.get_array())
            {
                double d = 0.0;
                if (!numberOf(component, d) || !std::isfinite(d) || std::fabs(d) > FLT_MAX)
                    throw ToolError(name + ": every component must be a finite number");
                list.append(LLSD::Real((F32)d));
            }
            return list;
        }

        // S32, U32, F32
        double d = 0.0;
        if (!numberOf(value, d))
            throw ToolError(name + ": expected a number (" + typeName(type) + ")");
        if (!std::isfinite(d))
            throw ToolError(name + ": the value must be a finite number (not NaN or infinity)");
        if (type != TYPE_F32 && d != std::floor(d))
            throw ToolError(name + ": expected a whole number (" + typeName(type) + ")");
        if (type == TYPE_S32 && (d < (double)std::numeric_limits<S32>::min() || d > (double)std::numeric_limits<S32>::max()))
            throw ToolError(name + ": out of the range of S32");
        if (type == TYPE_U32 && (d < 0.0 || d > (double)std::numeric_limits<U32>::max()))
            throw ToolError(name + ": out of the range of U32 (0 or more)");
        if (type == TYPE_F32 && std::fabs(d) > FLT_MAX)
            throw ToolError(name + ": out of the range of F32");
        if (a.ranged && (d < a.min - 1e-6 || d > a.max + 1e-6))
            throw ToolError(name + ": " + llformat("%g", d) + " is out of range (" + rangeText(a) + ")");
        if (!a.options.empty())
        {
            const bool listed = std::any_of(a.options.begin(), a.options.end(),
                                            [d](const Option& o) { return std::fabs(o.value - d) < 1e-4; });
            if (!listed)
            {
                std::string choices;
                for (const Option& o : a.options)
                    choices += (choices.empty() ? "" : ", ") + llformat("%g", o.value);
                throw ToolError(name + ": " + llformat("%g", d) + " is not one of the choices (" + choices + ")");
            }
        }
        switch (type)
        {
            case TYPE_S32:
                return LLSD::Integer((S32)d);
            case TYPE_U32:
                return LLSD::Integer((S32)(U32)d); // as LLControlGroup::setU32 stores it
            default:
                return LLSD::Real((F32)d); // as LLControlGroup::setF32 stores it
        }
    }

    // ---- arguments ----

    std::optional<bool> argBool(const json::object& args, const char* key)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        if (!v->is_bool())
            throw ToolError(std::string("'") + key + "' must be true or false");
        return v->get_bool();
    }

    std::optional<std::string> argString(const json::object& args, const char* key, std::size_t max_length)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        if (!v->is_string())
            throw ToolError(std::string("'") + key + "' must be a string");
        if (v->get_string().size() > max_length)
            throw ToolError(std::string("'") + key + "' is too long");
        return std::string(v->get_string());
    }

    std::optional<int> argInt(const json::object& args, const char* key, int min_value, int max_value)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        double d = 0.0;
        if (!numberOf(*v, d) || !std::isfinite(d) || d != std::floor(d) || d < min_value || d > max_value)
            throw ToolError(std::string("'") + key + "' must be a whole number from " + std::to_string(min_value) + " to " +
                            std::to_string(max_value));
        return (int)d;
    }

    // ---- tools (main thread) ----

    const char* const TAB_IDS[] = { "home", "quality", "light", "color", "camera", "subject", "shoot", EXTRA_TAB, "plus", "debug" };

    // The comment of the settings file, cut at a whole UTF-8 character.
    std::string shortComment(const std::string& comment)
    {
        if (comment.size() <= MAX_COMMENT_LENGTH)
            return comment;
        std::size_t end = MAX_COMMENT_LENGTH;
        while (end > 0 && ((unsigned char)comment[end] & 0xC0) == 0x80)
            --end;
        return comment.substr(0, end) + "...";
    }

    json::object settingRow(const Allowed& a, LLControlVariable& control)
    {
        const eControlType type  = control.type();
        const LLSD         value = control.getValue();
        const LLSD         saved = control.getSaveValue();
        json::object       row;
        row["name"]    = a.name;
        row["type"]    = typeName(type);
        row["value"]   = toJson(value, type);
        row["default"] = toJson(control.getDefault(), type);
        if (!llsd_equals(value, saved))
            row["saved_value"] = toJson(saved, type); // what settings.xml keeps (the value in use is not saved)
        if (a.ranged)
        {
            row["min"] = niceFloat(a.min);
            row["max"] = niceFloat(a.max);
        }
        if (!a.options.empty())
        {
            json::array options;
            for (const Option& o : a.options)
                options.push_back(json::object{ { "value", niceFloat(o.value) }, { "label_en", o.labelEn }, { "label_ja", o.labelJa } });
            row["options"] = std::move(options);
        }
        if (!a.labelEn.empty())
            row["label_en"] = a.labelEn;
        if (!a.labelJa.empty())
            row["label_ja"] = a.labelJa;
        if (a.labelEn.empty() && !control.getComment().empty())
            row["comment"] = shortComment(control.getComment()); // what the setting does, from the settings file
        row["tab"] = a.tab;
        if (a.rebuild > 0)
            row["rebuild"] = a.rebuild; // 1: the pipeline or its shaders are rebuilt (a pause of seconds)
        row["persist"] = control.isPersisted();
        if (sUnsaved.count(a.name))
            row["mcp_unsaved"] = true;
        return row;
    }

    ToolResult listStep(const json::object& args)
    {
        const std::string filter = lowered(argString(args, "filter", 100).value_or(""));
        const std::string tab    = lowered(argString(args, "tab", 20).value_or(""));
        const bool        changed_only = argBool(args, "changed_only").value_or(false);
        const std::size_t offset       = (std::size_t)argInt(args, "offset", 0, 1000000).value_or(0);
        const std::size_t limit        = (std::size_t)argInt(args, "limit", 1, (int)MAX_LIST_ROWS).value_or((int)MAX_LIST_ROWS);
        if (!tab.empty() && std::none_of(std::begin(TAB_IDS), std::end(TAB_IDS), [&tab](const char* id) { return tab == id; }))
            throw ToolError("'tab' must be one of home, quality, light, color, camera, subject, shoot, render, plus, debug");

        buildTable();
        std::set<std::string> pinned;
        if (tab == "home")
            pinned = plus::photomode::pinnedIds(); // the home tab shows the pinned rows of the other tabs

        json::array rows;
        std::size_t matched = 0; // rows of every page
        for (const Allowed& a : sAllowed)
        {
            if (tab == "home" ? !pinned.count(a.name) : (!tab.empty() && a.tab != tab))
                continue;
            if (!filter.empty() && lowered(a.name).find(filter) == std::string::npos &&
                lowered(a.labelEn).find(filter) == std::string::npos && a.labelJa.find(filter) == std::string::npos)
                continue;
            LLControlVariablePtr control = gSavedSettings.getControl(a.name);
            if (!control)
                continue;
            if (changed_only && llsd_equals(control->getValue(), control->getDefault()) && !sUnsaved.count(a.name))
                continue;
            if (matched++ >= offset && rows.size() < limit)
                rows.push_back(settingRow(a, *control));
        }

        json::object result;
        result["count"]  = rows.size();
        result["total"]  = matched;
        result["offset"] = offset;
        if (offset + rows.size() < matched)
            result["next_offset"] = offset + rows.size(); // more rows: call again with this offset
        result["settings"] = std::move(rows);
        result["note"] = "tab: the photo tools' tab id (home = pinned rows), render = render settings, plus / debug = "
                         "the other Plus* / Debug* settings. min/max and options are enforced by settings_set; a setting "
                         "without them is checked by type only. rebuild 1: changing it rebuilds the pipeline (a pause of "
                         "seconds). saved_value: what the user's settings file keeps while the value in use is a test "
                         "value (mcp_unsaved: set by settings_set without persist).";
        return ToolResult::json(result);
    }

    ToolResult getStep(const std::vector<std::string>& names)
    {
        buildTable();
        json::object values, saved;
        json::array  refused;
        for (const std::string& name : names)
        {
            LLControlVariablePtr control = findAllowed(name) ? gSavedSettings.getControl(name) : LLControlVariablePtr();
            if (!control)
            {
                refused.push_back(json::string(name));
                continue;
            }
            values[name] = toJson(control->getValue(), control->type());
            if (!llsd_equals(control->getValue(), control->getSaveValue()))
                saved[name] = toJson(control->getSaveValue(), control->type());
        }
        json::object result;
        result["values"] = std::move(values);
        if (!saved.empty())
            result["saved_values"] = std::move(saved); // differ from the value in use (a test value is in use)
        if (!refused.empty())
        {
            result["not_allowed"] = std::move(refused);
            result["note"]        = std::string("Unknown or not allowed names were left out. ") + DENIED_RULE;
        }
        return ToolResult::json(result);
    }

    ToolResult setStep(const json::object& values, bool persist)
    {
        buildTable();

        // Everything is checked first: one bad value changes nothing.
        struct Planned
        {
            const Allowed*       allowed;
            LLControlVariablePtr control;
            LLSD                 value;
        };
        std::vector<Planned> plan;
        std::string          errors;
        for (const json::key_value_pair& item : values)
        {
            const std::string    name(item.key());
            const json::value&   value   = item.value();
            const Allowed*       a       = findAllowed(name);
            LLControlVariablePtr control = a ? gSavedSettings.getControl(name) : LLControlVariablePtr();
            if (!control)
            {
                errors += (errors.empty() ? "" : "; ") + name + ": unknown or not allowed";
                continue;
            }
            try
            {
                plan.push_back({ a, control, parseValue(value, *a, control->type()) });
            }
            catch (const ToolError& e)
            {
                errors += (errors.empty() ? "" : "; ") + std::string(e.what());
            }
        }
        if (!errors.empty())
            throw ToolError("Nothing was changed. " + errors + (errors.find("not allowed") != std::string::npos ? std::string(". ") + DENIED_RULE : std::string()));

        // fix-round100: a compare held in the photo tools ends first (as a graphics preset does), so the
        // values from before are the user's own, not the compare's, and the compare never puts its
        // remembered values back over the tool's.
        plus::photomode::compareEnd();

        Entry        entry;
        json::array  changed, unchanged;
        for (Planned& p : plan)
        {
            LLControlVariable& control = *p.control;
            const eControlType type    = control.type();
            Change             change;
            change.name        = p.allowed->name;
            change.before      = control.getValue();
            change.beforeSaved = control.getSaveValue();
            change.persist     = persist;
            if (llsd_equals(change.before, p.value) && (!persist || llsd_equals(change.beforeSaved, p.value)))
            {
                unchanged.push_back(json::string(change.name));
                continue;
            }

            // persist false: the unsaved value (see the top of the file); true: the saved one.
            control.setValue(p.value, persist);
            change.after = control.getValue();
            if (llsd_equals(change.after, change.before) && llsd_equals(control.getSaveValue(), change.beforeSaved))
            {
                // A validator of the setting refused it.
                unchanged.push_back(json::string(change.name));
                continue;
            }
            if (persist)
                sUnsaved.erase(change.name); // the value is the user's saved one now
            else
            {
                forgetIfChangedSince(change.name, change.before); // the stop goes back to the newer value then
                noteUnsaved(change.name, change.before, change.after);
            }

            LL_INFOS("PlusPoserMcp") << "settings_set " << change.name << ": " << logText(change.before, type) << " -> "
                                     << logText(change.after, type) << (persist ? " (saved)" : " (not saved)") << LL_ENDL;

            json::object row;
            row["name"]   = change.name;
            row["before"] = toJson(change.before, type);
            row["after"]  = toJson(change.after, type);
            if (!llsd_equals(change.after, p.value))
                row["adjusted_by_viewer"] = true; // a listener of the setting changed it again
            changed.push_back(std::move(row));
            entry.changes.push_back(std::move(change));
        }

        json::object result;
        result["changed"] = std::move(changed);
        if (!unchanged.empty())
            result["unchanged"] = std::move(unchanged);
        result["persist"] = persist;
        if (!entry.changes.empty())
        {
            entry.id            = sNextEntryId++;
            result["history_id"] = entry.id;
            sHistory.push_back(std::move(entry));
            while (sHistory.size() > HISTORY_CAPACITY)
                sHistory.pop_front();
        }
        result["note"] = persist ? "Saved: the values are the user's settings now (settings_revert still undoes them)."
                                 : "Not saved: the user's settings file keeps the old values; they come back when the MCP "
                                   "server stops. settings_revert undoes this call now.";
        return ToolResult::json(result);
    }

    void restore(LLControlVariable& control, const Change& change)
    {
        if (change.persist)
        {
            control.setValue(change.beforeSaved, true);
            if (!llsd_equals(change.before, change.beforeSaved))
                control.setValue(change.before, false); // an unsaved value was in use before
        }
        else
        {
            control.setValue(change.before, false);
        }
    }

    ToolResult revertStep(const json::object& args)
    {
        const std::optional<int>  steps = argInt(args, "steps", 1, (int)HISTORY_CAPACITY);
        const bool                all   = argBool(args, "all").value_or(false);
        if (steps && all)
            throw ToolError("give either steps or all, not both");
        if (sHistory.empty())
            throw ToolError("Nothing to revert: no settings_set change of this MCP session is left.");

        plus::photomode::compareEnd(); // fix-round100: as in setStep()

        const std::size_t count = all ? sHistory.size() : std::min<std::size_t>(steps.value_or(1), sHistory.size());
        json::array       reverted, skipped;
        for (std::size_t i = 0; i < count; ++i)
        {
            const Entry  entry = std::move(sHistory.back());
            sHistory.pop_back();
            json::object restored;
            for (auto it = entry.changes.rbegin(); it != entry.changes.rend(); ++it)
            {
                const Change&        change  = *it;
                LLControlVariablePtr control = gSavedSettings.getControl(change.name);
                if (!control)
                    continue;
                const eControlType type = control->type();
                // Changed since the tool's last write of it, even if back to this change's value: not undone.
                const bool changed_since = forgetIfChangedSince(change.name, control->getValue());
                // fix-round100: a test call is not undone over a saved value written since (the record may
                // be gone already: a persist call since, or the user's saving dropped it).
                const bool saved_since = !change.persist && !llsd_equals(control->getSaveValue(), change.beforeSaved);
                if (changed_since || saved_since || !llsd_equals(control->getValue(), change.after))
                {
                    // Changed since by somebody else (the user, a preset): theirs stays.
                    skipped.push_back(json::object{ { "name", change.name }, { "history_id", entry.id },
                                                    { "value", toJson(control->getValue(), type) } });
                    // fix-round100: and the older calls' changes of it too (all from before that change): a later
                    // revert would otherwise take a value that only happens to equal an older call's for the tool's.
                    for (Entry& older : sHistory)
                    {
                        std::erase_if(older.changes, [&change](const Change& c) { return c.name == change.name; });
                    }
                    continue;
                }
                const LLSD from = control->getValue();
                restore(*control, change);
                const LLSD now = control->getValue();
                if (llsd_equals(now, control->getSaveValue()))
                    sUnsaved.erase(change.name); // fix-round100: the saved value itself again: nothing for the stop to put back
                else if (const auto unsaved = sUnsaved.find(change.name); unsaved != sUnsaved.end())
                    noteUnsaved(change.name, unsaved->second.original, now);
                else if (change.persist && !llsd_equals(now, control->getSaveValue()))
                    noteUnsaved(change.name, control->getSaveValue(), now); // an unsaved value is in use again
                LL_INFOS("PlusPoserMcp") << "settings_revert " << change.name << ": " << logText(from, type) << " -> "
                                         << logText(now, type) << LL_ENDL;
                restored[change.name] = toJson(now, type);
            }
            reverted.push_back(json::object{ { "history_id", entry.id }, { "values", std::move(restored) } });
        }

        json::object result;
        result["reverted"] = std::move(reverted); // newest first
        if (!skipped.empty())
        {
            result["skipped"] = std::move(skipped);
            result["note"]    = "skipped: changed since by something else (the user, a preset); left as it is.";
        }
        result["remaining"] = sHistory.size();
        return ToolResult::json(result);
    }

    // ---- schema helpers ----

    json::object prop(const char* type, const std::string& description)
    {
        return json::object{ { "type", type }, { "description", description } };
    }

    json::object objectSchema(json::object properties, std::initializer_list<const char*> required = {})
    {
        json::object schema;
        schema["type"]                 = "object";
        schema["properties"]           = std::move(properties);
        schema["additionalProperties"] = false;
        if (required.size())
        {
            json::array list;
            for (const char* r : required)
                list.push_back(json::string(r));
            schema["required"] = std::move(list);
        }
        return schema;
    }
}

namespace plus::posermcp
{
    void appendSettingsTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        {
            Tool t;
            t.name        = "settings_list";
            t.title       = "List viewer settings";
            t.description = "Lists the viewer settings that settings_get/settings_set may use (the photo tools' settings and "
                            "the debug settings whose names start with Plus, Render or Debug, except private or dangerous "
                            "ones): name, type, value, default, min/max or options, label_en/label_ja or comment, tab, "
                            "rebuild (1 = changing it rebuilds the pipeline), persist. Filter by part of a name or label "
                            "(e.g. \"shadow\", \"TAA\", \"Volumetric\"), by tab (home, quality, light, color, camera, subject, "
                            "shoot, render, plus, debug), or changed_only. At most 200 rows a call: total is the number "
                            "matching, next_offset the offset of the next page.";
            t.readOnly    = true;
            json::object p;
            p["filter"]       = prop("string", "Part of the name or label (any case).");
            p["tab"]          = prop("string", "home, quality, light, color, camera, subject, shoot, render, plus or debug.");
            p["changed_only"] = prop("boolean", "Only settings that differ from their default or hold a test value.");
            p["offset"]       = prop("integer", "Rows to skip (the next_offset of the call before). Default 0.");
            p["limit"]        = prop("integer", "Rows to return, 1-200. Default 200.");
            t.inputSchema     = objectSchema(std::move(p));
            t.handler         = [run](const json::object& args) -> ToolResult
            {
                return run([args](const JobProgress&) -> std::optional<ToolResult> { return listStep(args); }, LIST_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "settings_get";
            t.title       = "Read viewer settings";
            t.description = "Current values of the named settings (only those settings_list offers). saved_values shows "
                            "what the user's settings file keeps when a test value is in use.";
            t.readOnly    = true;
            json::object p;
            json::object names = prop("array", "Setting names (exact, case-sensitive).");
            names["items"]     = json::object{ { "type", "string" } };
            p["names"]         = std::move(names);
            t.inputSchema      = objectSchema(std::move(p), { "names" });
            t.handler          = [run](const json::object& args) -> ToolResult
            {
                const json::value* v = args.if_contains("names");
                if (!v || !v->is_array() || v->get_array().empty() || v->get_array().size() > MAX_NAMES_PER_CALL)
                    return ToolResult::error("'names' must be an array of 1 to " + std::to_string(MAX_NAMES_PER_CALL) + " setting names");
                std::vector<std::string> names;
                for (const json::value& name : v->get_array())
                {
                    if (!name.is_string() || name.get_string().size() > 100)
                        return ToolResult::error("every entry of 'names' must be a setting name (a string)");
                    names.emplace_back(name.get_string());
                }
                return run([names](const JobProgress&) -> std::optional<ToolResult> { return getStep(names); }, LIST_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "settings_set";
            t.title       = "Change viewer settings";
            t.description = "Changes settings: values = {name: value}. Each value is checked against the type and the "
                            "min/max or options of settings_list (a string of a Plus/Render/Debug setting must not contain "
                            "/ \\ : or ..); one bad value changes nothing. By default (persist false) "
                            "the change is a test: the user's settings file keeps the old value, and it comes back when the "
                            "MCP server stops. persist true saves it as the user's setting. Returns the values before and "
                            "after and a history_id; settings_revert undoes it.";
            json::object p;
            json::object vals = prop("object", "Setting name -> new value (true/false, number, string, or [r,g,b(,a)]).");
            p["values"]       = std::move(vals);
            p["persist"]      = prop("boolean", "Save as the user's setting (default false: a test value, not saved).");
            t.inputSchema     = objectSchema(std::move(p), { "values" });
            t.handler         = [run](const json::object& args) -> ToolResult
            {
                const json::value* v = args.if_contains("values");
                if (!v || !v->is_object() || v->get_object().empty() || v->get_object().size() > MAX_NAMES_PER_CALL)
                    return ToolResult::error("'values' must be an object of 1 to " + std::to_string(MAX_NAMES_PER_CALL) + " settings ({name: value})");
                bool persist = false;
                if (const json::value* pv = args.if_contains("persist"); pv && !pv->is_null())
                {
                    if (!pv->is_bool())
                        return ToolResult::error("'persist' must be true or false");
                    persist = pv->get_bool();
                }
                const json::object values = v->get_object();
                return run([values, persist](const JobProgress&) -> std::optional<ToolResult> { return setStep(values, persist); },
                           SET_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "settings_revert";
            t.title       = "Revert setting changes";
            t.description = "Undoes settings_set calls of this MCP session, newest first: steps=N calls (default 1), or "
                            "all=true. A setting changed since by someone else is left as it is (listed as skipped).";
            json::object p;
            p["steps"]    = prop("integer", "How many settings_set calls to undo (1-200). Default 1.");
            p["all"]      = prop("boolean", "Undo every settings_set call of this session.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = [run](const json::object& args) -> ToolResult
            {
                return run([args](const JobProgress&) -> std::optional<ToolResult> { return revertStep(args); }, SET_TIMEOUT);
            };
            tools.push_back(std::move(t));
        }
    }

    std::string settingsInstructions()
    {
        return "VIEWER SETTINGS (for tests and A/B measurements): settings_list shows the settings you may read and change "
               "(the photo tools' settings and the debug settings named Plus*, Render* and Debug*, 200 rows a page: use "
               "filter, tab or offset) with their ranges; settings_get reads values; "
               "settings_set changes them (checked; by default a test value that is not saved and goes back when the MCP "
               "server stops; persist=true saves it); settings_revert undoes settings_set calls (steps, or all). For an "
               "A/B test: settings_set the A value, measure, settings_set the B value, measure, then settings_revert "
               "all=true.\n";
    }

    void settingsServerStopped(bool restore_values)
    {
        if (restore_values)
        {
            for (const auto& [name, unsaved] : sUnsaved)
            {
                LLControlVariablePtr control = gSavedSettings.getControl(name);
                if (!control || !stillAsWritten(name, unsaved, control->getValue()))
                    continue; // changed (or saved, fix-round100) since by the user: theirs stays
                control->setValue(unsaved.original, false);
                LL_INFOS("PlusPoserMcp") << "MCP stopped: " << name << " back to " << logText(control->getValue(), control->type())
                                         << LL_ENDL;
            }
        }
        sUnsaved.clear();
        sHistory.clear();
    }

    bool isTestValue(const std::string& name, const LLSD& value)
    {
        const auto it = sUnsaved.find(name);
        return it != sUnsaved.end() && llsd_equals(value, it->second.written) && llsd_equals(savedValueOf(name), it->second.saved);
    }
}
