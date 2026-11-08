/**
 * @file debugtools.cpp
 * @brief MCP tools for the render switches of development starts: cue_list,
 *        cue_toggle, env_switches (debugtools.h).
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

// Cues (vk-mcp2): a development start (tools/launch.ps1, daily.ps1) sets
// LLRHI_CUE_DIR; the viewer looks for <dir>/<name>.toggle once a second while
// it draws, deletes it and flips the switch (LLRHI::pollCueToggles and the
// features' own cues, OTHER_CUES below), writing a log line. Without
// LLRHI_CUE_DIR (the downloadable viewer) there are no cues and cue_toggle
// says so. cue_toggle writes the file of a known cue only (no other name, no
// other folder), waits for the viewer to take it, and takes the file back when
// the viewer did not in time, so nothing flips later by surprise.
//
// Environment switches: env_switches reads the LLRHI_* switches the viewer
// read at startup, as they are now (an auto mode turns itself on or off for the
// run), and the last log line of each that tells the decision and its counts
// (the mismatches are counted where they are compared and logged there).
//
// Threads: the switches are read in run() on the main thread (where the cues
// flip them); the cue file and the log are touched on the connection thread
// only. The log is read, never written, and only lines of the render switches'
// own messages are returned (the log holds other things too).

#include "llviewerprecompiledheaders.h"

#include "debugtools.h"
#include "mcpschema.h"

#include "llerrorcontrol.h"
#include "llfile.h"
#include "llrhi.h"
#include "llrhidrawlist.h"
#include "llrhipass.h"
#include "llrhiscene.h"
#include "llstring.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <thread>

namespace json = boost::json;
using plus::mcp::Tool;
using plus::mcp::ToolResult;
using plus::posermcp::JobProgress;
using plus::posermcp::MainThreadRunner;

namespace
{
    constexpr char        CUE_DIR_ENV[]    = "LLRHI_CUE_DIR";
    constexpr char        TOGGLE_SUFFIX[]  = ".toggle";
    constexpr std::size_t MAX_CUE_NAME     = 64;
    constexpr double      READ_TIMEOUT     = 15.0;      // seconds for a main thread read
    constexpr double      DEFAULT_WAIT     = 4.0;       // the viewer looks once a second while it draws
    constexpr double      MAX_WAIT         = 15.0;
    constexpr int         POLL_MS          = 100;
    constexpr std::size_t CUE_LOG_BYTES    = 256 * 1024; // cue_toggle: at most this much of the log written meanwhile
    constexpr std::size_t ENV_LOG_BYTES    = 2 * 1024 * 1024; // env_switches: the end of the log searched
    constexpr std::size_t MAX_LOG_LINE     = 400;
    constexpr std::size_t MAX_CUE_LOG_LINES = 12;

    // Log lines a cue's answer may be in (the tags of the switches' messages).
    const char* const CUE_LOG_TAGS[] = { "RHIProfile", "RHI", "Vulkan", "VKPerf", "LightTable", "Scene", "PerfMark" };

    // ---- cues ----

    // Cue files outside LLRHI::pollCueToggles: direct.toggle (llviewerdisplay.cpp) and the features'
    // own. setting: the setting the cue flips (not saved), whose value is shown; nullptr: the switch
    // is the feature's own (its value is in the log line only), except the two read below.
    struct OtherCue
    {
        const char* file;
        const char* label;
        const char* setting;
    };
    const OtherCue OTHER_CUES[] = {
        { "direct.toggle", "direct draws (LLRHI_DIRECT)", nullptr },
        { "nativepasses.toggle", "full-screen passes (research/137 N12, LLRHI_NATIVE_PASSES)", nullptr },
        { "nativepassesref.toggle", "full-screen passes' reference, counting only (LLRHI_NATIVE_PASSES=2; off: the barrier plan, =1)", nullptr },
        { "nativetree.toggle", "native tree shadows (research/137 N10)", nullptr },
        { "shadowbridge.toggle", "shadow bridge culls once per cascade set (research/178 C1b)", nullptr },
        { "shadowzhold.toggle", "shadow z hold (research/178 C1)", nullptr },
        { "shadowfarsmall.toggle", "far small shadow casters skipped (PlusShadowSkipFarSmall flipped, not saved)", "PlusShadowSkipFarSmall" },
        { "shadowshift.toggle", "shadow shift (PlusShadowShift flipped, not saved)", "PlusShadowShift" },
        { "realstars.toggle", "real star sky (PlusSkyStarsReal flipped, not saved)", "PlusSkyStarsReal" },
        { "realstarlines.toggle", "constellation lines (PlusSkyStarsLines flipped, not saved)", "PlusSkyStarsLines" },
        { "realstarart.toggle", "constellation pictures (PlusSkyStarsArt flipped, not saved)", "PlusSkyStarsArt" },
        { "startwinkle.toggle", "star twinkle (research/176 section 11)", nullptr },
        { "colornorebuild.toggle", "TE color without a group rebuild (research/81)", nullptr },
        { "alphanorebuild.toggle", "TE alpha within its class without a group rebuild (research/81)", nullptr },
        { "colorstats.toggle", "TE color change counters (research/81)", nullptr },
        { "texview.toggle", "textures on screen first (research/285, PlusTexViewPriority)", nullptr },
    };

    struct CueRow
    {
        std::string         file;
        std::string         label;
        std::optional<bool> on;          // unknown here: the log line tells
        bool                available = true;
        const char*         source = ""; // "llrhi" (LLRHI::pollCueToggles) or "viewer"
    };

    // Main thread.
    std::vector<CueRow> cueRows()
    {
        std::vector<LLRHI::CueToggleState> states;
        LLRHI::cueToggleStates(states);
        std::vector<CueRow> rows;
        rows.reserve(states.size() + std::size(OTHER_CUES));
        for (const LLRHI::CueToggleState& state : states)
            rows.push_back({ state.file, state.label, state.on, state.available, "llrhi" });
        for (const OtherCue& cue : OTHER_CUES)
        {
            CueRow row{ cue.file, cue.label, std::nullopt, true, "viewer" };
            if (!std::strcmp(cue.file, "direct.toggle"))
                row.on = LLRHI::sDirect;
            else if (!std::strcmp(cue.file, "nativepasses.toggle"))
                row.on = LLRHI::sNativePasses != 0;
            else if (!std::strcmp(cue.file, "nativepassesref.toggle"))
                row.on = LLRHI::sNativePasses == 2;
            else if (cue.setting)
            {
                if (LLControlVariablePtr control = gSavedSettings.getControl(cue.setting))
                    row.on = control->getValue().asBoolean();
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    json::object cueJson(const CueRow& row)
    {
        json::object o;
        o["file"]  = row.file;
        o["label"] = row.label;
        if (row.on)
            o["on"] = *row.on;
        else
            o["on"] = nullptr;
        if (!row.available)
            o["available"] = false; // the cue would leave it unchanged (not installed at startup)
        o["source"] = row.source;
        return o;
    }

    // "shadowcache" or "shadowcache.toggle" -> "shadowcache.toggle"; empty when not a cue file's name.
    std::string cueFileName(std::string name)
    {
        const std::size_t suffix = std::strlen(TOGGLE_SUFFIX);
        if (name.size() <= suffix || name.compare(name.size() - suffix, suffix, TOGGLE_SUFFIX) != 0)
            name += TOGGLE_SUFFIX;
        const std::size_t stem = name.size() - suffix;
        if (stem == 0 || name.size() > MAX_CUE_NAME)
            return std::string();
        for (std::size_t i = 0; i < stem; ++i)
        {
            const unsigned char c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return std::string(); // a cue's name only: no path, no other folder
        }
        return name;
    }

    std::string lowered(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    // ---- the log (connection thread; read only) ----

    // At most max_bytes of the file from offset on (the end when offset is past it), split into lines.
    std::vector<std::string> readLogLines(const std::string& path, S64 offset, std::size_t max_bytes, bool keep_end)
    {
        std::vector<std::string> lines;
        if (path.empty())
            return lines;
        llifstream file(path, std::ios::in | std::ios::binary);
        if (!file.is_open())
            return lines;
        file.seekg(0, std::ios::end);
        const S64 size = (S64)file.tellg();
        if (size <= 0)
            return lines;
        if (offset < 0 || offset > size)
            offset = 0; // a new log since
        S64 length = size - offset;
        if ((S64)max_bytes < length)
        {
            if (keep_end)
                offset = size - (S64)max_bytes;
            length = (S64)max_bytes;
        }
        std::string text((std::size_t)length, '\0');
        file.seekg(offset);
        file.read(text.data(), length);
        text.resize((std::size_t)std::max<std::streamsize>(0, file.gcount()));

        std::size_t start = 0;
        if (keep_end && offset > 0)
        {
            const std::size_t first = text.find('\n');
            start = first == std::string::npos ? text.size() : first + 1; // the first line is cut
        }
        while (start < text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                lines.push_back(std::move(line));
            start = end + 1;
        }
        return lines;
    }

    std::string shortLine(const std::string& line)
    {
        if (line.size() <= MAX_LOG_LINE)
            return line;
        std::size_t end = MAX_LOG_LINE;
        while (end > 0 && ((unsigned char)line[end] & 0xC0) == 0x80)
            --end;
        return line.substr(0, end) + "...";
    }

    bool hasCueTag(const std::string& line)
    {
        return std::any_of(std::begin(CUE_LOG_TAGS), std::end(CUE_LOG_TAGS),
                           [&line](const char* tag) { return line.find(tag) != std::string::npos; });
    }

    // ---- environment switches ----

    // A switch with an auto mode: verify first, then on once enough compared with no mismatch, off
    // for the run at the first. marker: the start of its log lines (the auto decisions and the counts).
    struct AutoSwitch
    {
        const char* env;
        const bool* on;
        const bool* verify;
        const bool* autoMode;
        const char* marker;
    };
    const AutoSwitch AUTO_SWITCHES[] = {
        { "LLRHI_TEX_HANDLES", &LLRHI::sTexHandles, &LLRHI::sTexHandlesVerify, &LLRHI::sTexHandlesAuto, "texture numbers" },
        { "LLRHI_VB_HANDLES", &LLRHI::sVBHandles, &LLRHI::sVBHandlesVerify, &LLRHI::sVBHandlesAuto, "vertex buffer numbers" },
        { "LLRHI_SLOT_PUBLISH", &LLRHI::sSlotPublish, &LLRHI::sSlotPublishVerify, &LLRHI::sSlotPublishAuto, "slot publish" },
        { "LLRHI_PACKETS", &LLRHI::sPackets, &LLRHI::sPacketsVerify, &LLRHI::sPacketsAuto, "draw packets" },
        { "LLRHI_CULL_PAR", &LLRHI::sCullPar, &LLRHI::sCullParVerify, &LLRHI::sCullParAuto, "cull par" },
        { "LLRHI_CULL_REUSE", &LLRHI::sCullReuse, &LLRHI::sCullReuseVerify, &LLRHI::sCullReuseAuto, "cull reuse" },
        { "LLRHI_LIGHT_TABLE", &LLRHI::sLightTable, &LLRHI::sLightTableVerify, &LLRHI::sLightTableAuto, "light table" },
        { "LLRHI_TEXANIM_GPU", &LLRHI::sTexAnimGpu, &LLRHI::sTexAnimGpuVerify, &LLRHI::sTexAnimGpuAuto, "texanim gpu" },
        { "LLRHI_VPARAM_MEMO", &LLRHI::sVParamMemo, &LLRHI::sVParamMemoVerify, &LLRHI::sVParamMemoAuto, "vparam memo" },
    };

    // Switches without an auto mode.
    struct PlainSwitch
    {
        const char* env;
        const bool* on;
    };
    const PlainSwitch PLAIN_SWITCHES[] = {
        { "LLRHI_DIRECT", &LLRHI::sDirect },
        { "LLRHI_OIT", &LLRHI::sOIT },
        { "LLRHI_CULL_THREADS", &LLRHI::sCullThreads },
        { "LLRHI_RENDER_THREAD", &LLRHI::sRenderThread },
        { "LLRHI_GPU_TIMING", &LLRHI::sGPUTiming },
        { "LLRHI_FRAME_COST", &LLRHI::sFrameCost },
        { "LLRHI_PERIODIC_LOGS", &LLRHI::sPeriodicLogs },
    };

    // The scene's log lines start so (llscenerecords.cpp).
    constexpr char SCENE_MARKER[] = "retained scene";

    json::value envValue(const char* env)
    {
        const char* value = std::getenv(env);
        if (!value)
            return nullptr; // unset: the switch's default
        std::string text(value);
        if (text.size() > 32)
            text = text.substr(0, 32) + "...";
        return json::string(text);
    }

    std::string autoState(bool on, bool verify, bool auto_mode)
    {
        if (auto_mode)
            return verify ? "auto: verifying (turns on after enough clean comparisons, off at a mismatch)" : (on ? "auto: on" : "auto: off");
        if (verify)
            return "verify";
        return on ? "on" : "off";
    }

    struct EnvRow
    {
        json::object row;
        std::string  marker; // empty: no log line
    };

    // Main thread.
    std::vector<EnvRow> envRows()
    {
        std::vector<EnvRow> rows;
        {
            json::object row;
            row["env"]  = "LLRHI_SCENE";
            row["set"]  = envValue("LLRHI_SCENE");
            row["mode"] = LLRHI::sceneModeName(LLRHI::sceneMode());
            if (LLRHI::sScene)
            {
                row["drawing"]  = LLRHI::sScene->drawing(); // auto: on once its clean frames are in
                row["problems"] = LLRHI::sScene->problems(); // the backend's own checks that found a difference
            }
            row["alpha"]  = LLRHI::sceneModeName(LLRHI::sceneAlphaMode());  // LLRHI_SCENE_ALPHA
            row["shadow"] = LLRHI::sceneModeName(LLRHI::sceneShadowMode()); // LLRHI_SCENE_SHADOW
            row["probe"]  = LLRHI::sceneModeName(LLRHI::sceneProbeMode());  // LLRHI_SCENE_PROBE
            rows.push_back({ std::move(row), SCENE_MARKER });
        }
        for (const AutoSwitch& s : AUTO_SWITCHES)
        {
            json::object row;
            row["env"]    = s.env;
            row["set"]    = envValue(s.env);
            row["state"]  = autoState(*s.on, *s.verify, *s.autoMode);
            row["on"]     = *s.on;
            row["verify"] = *s.verify;
            row["auto"]   = *s.autoMode;
            rows.push_back({ std::move(row), s.marker });
        }
        {
            json::object row;
            row["env"]  = "LLRHI_DRAW_LISTS";
            row["set"]  = envValue("LLRHI_DRAW_LISTS");
            row["mode"] = LLRHI::DrawList::sMode; // 0 off, 1 the light path, 2 the reference path
            rows.push_back({ std::move(row), std::string() });
        }
        {
            json::object row;
            row["env"]  = "LLRHI_NATIVE_PASSES";
            row["set"]  = envValue("LLRHI_NATIVE_PASSES");
            row["mode"] = LLRHI::sNativePasses; // 0 off, 1 the barrier plan, 2 counting only
            rows.push_back({ std::move(row), std::string() });
        }
        for (const PlainSwitch& s : PLAIN_SWITCHES)
        {
            json::object row;
            row["env"] = s.env;
            row["set"] = envValue(s.env);
            row["on"]  = *s.on;
            rows.push_back({ std::move(row), std::string() });
        }
        return rows;
    }

    // ---- what the main thread hands the connection thread ----

    struct CueState
    {
        std::string         dir;     // LLRHI_CUE_DIR (UTF-8), empty: no cues in this start
        std::string         log;     // the log file
        std::vector<CueRow> rows;
    };

    std::shared_ptr<CueState> readCueState(const MainThreadRunner& run, ToolResult& error)
    {
        auto state = std::make_shared<CueState>();
        ToolResult result = run(
            [state](const JobProgress&) -> std::optional<ToolResult>
            {
                state->dir  = LLStringUtil::getenv(CUE_DIR_ENV); // UTF-8, as LLFile takes it
                state->log  = LLError::logFileName();
                state->rows = cueRows();
                return ToolResult::text("ok");
            },
            READ_TIMEOUT);
        if (result.isError)
        {
            error = std::move(result);
            return nullptr;
        }
        return state;
    }

    const CueRow* findRow(const CueState& state, const std::string& file)
    {
        for (const CueRow& row : state.rows)
            if (row.file == file)
                return &row;
        return nullptr;
    }

    using plus::mcp::ToolError;      // mcpschema.h (research/275 L110)
    using plus::mcp::prop;
    using plus::mcp::objectSchema;

    std::optional<std::string> argString(const json::object& args, const char* key, std::size_t max_length)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        if (!v->is_string() || v->get_string().size() > max_length)
            throw ToolError(std::string("'") + key + "' must be a string of at most " + std::to_string(max_length) + " bytes");
        return std::string(v->get_string());
    }

    // ---- tools (connection thread, the reads through run) ----

    ToolResult cueList(const MainThreadRunner& run, const json::object& args)
    {
        const std::string filter = lowered(argString(args, "filter", 100).value_or(""));
        ToolResult        error;
        const std::shared_ptr<CueState> state = readCueState(run, error);
        if (!state)
            return error;

        json::array cues;
        for (const CueRow& row : state->rows)
        {
            if (!filter.empty() && row.file.find(filter) == std::string::npos &&
                lowered(row.label).find(filter) == std::string::npos)
                continue;
            cues.push_back(cueJson(row));
        }
        json::object result;
        result["cue_dir_set"] = !state->dir.empty();
        if (!state->dir.empty())
            result["cue_dir_exists"] = LLFile::isdir(state->dir);
        result["count"] = cues.size();
        result["cues"]  = std::move(cues);
        result["note"]  = state->dir.empty()
                              ? "This start has no LLRHI_CUE_DIR (the downloadable viewer, or not started by tools/launch.ps1 "
                                "or daily.ps1): cues cannot be used. on: the switch now (null: the feature's own, see its log line)."
                              : "cue_toggle flips one (the file is written into LLRHI_CUE_DIR; the viewer takes it within a "
                                "second while it draws). on: the switch now (null: the feature's own, see its log line). "
                                "available false: the cue would leave it unchanged (not installed at startup).";
        return ToolResult::json(result);
    }

    ToolResult cueToggle(const MainThreadRunner& run, const json::object& args)
    {
        const std::string name = argString(args, "name", MAX_CUE_NAME).value_or("");
        const std::string file = cueFileName(name);
        if (file.empty())
            throw ToolError("'name' must be a cue's name (lower case letters, digits, _; with or without .toggle): cue_list shows them");
        double wait = DEFAULT_WAIT;
        if (const json::value* v = args.if_contains("wait_seconds"); v && !v->is_null())
        {
            if (!v->is_number() || !std::isfinite(v->to_number<double>()) || v->to_number<double>() < 0.5 ||
                v->to_number<double>() > MAX_WAIT)
                throw ToolError("'wait_seconds' must be a number from 0.5 to " + std::to_string((int)MAX_WAIT));
            wait = v->to_number<double>();
        }

        ToolResult error;
        const std::shared_ptr<CueState> before = readCueState(run, error);
        if (!before)
            return error;
        const CueRow* row = findRow(*before, file);
        if (!row)
            throw ToolError(file + " is not a cue of this viewer: cue_list shows them");
        if (before->dir.empty())
            throw ToolError("This start has no LLRHI_CUE_DIR (the downloadable viewer, or not started by tools/launch.ps1 or "
                            "daily.ps1): cues cannot be used. Environment switches are read at startup only (env_switches).");
        if (!LLFile::isdir(before->dir))
            throw ToolError("LLRHI_CUE_DIR does not exist (the folder is not made here)");
        const std::string path = before->dir + "/" + file;
        if (LLFile::isfile(path))
            throw ToolError(file + " is already waiting in LLRHI_CUE_DIR (the viewer takes it while it draws); nothing written");
        if (!row->available)
            throw ToolError(file + ": its switch was not installed at startup, the cue would leave it unchanged");

        const S64 log_from = before->log.empty() ? -1 : LLFile::size(before->log);
        if (LLFILE* f = LLFile::fopen(path, LLFILE_MODE("wb")))
            LLFile::close(f);
        else
            throw ToolError("cannot write " + file + " into LLRHI_CUE_DIR");
        LL_INFOS("PlusPoserMcp") << "cue_toggle " << file << LL_ENDL;

        // The viewer deletes the file when it takes it.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(wait);
        bool taken = false;
        while (!(taken = !LLFile::isfile(path)) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        bool took_back = false;
        if (!taken)
        {
            // Not in time (no frames drawn, or a feature that looks only while it runs): taken back, so it
            // does not flip later by surprise. When the viewer was quicker, it is taken after all.
            if (LLFile::remove(path, ENOENT) == 0)
                took_back = true;
            else
                taken = !LLFile::isfile(path);
        }

        ToolResult      after_error;
        const std::shared_ptr<CueState> after = readCueState(run, after_error);

        json::object result;
        result["file"]     = file;
        result["label"]    = row->label;
        result["taken"]    = taken;
        result["before"]   = row->on ? json::value(*row->on) : json::value(nullptr);
        const CueRow* now  = after ? findRow(*after, file) : nullptr;
        result["after"]    = now && now->on ? json::value(*now->on) : json::value(nullptr);
        if (taken && row->on && now && now->on && *row->on == *now->on)
            result["unchanged"] = true; // the viewer kept it (the log line says why)

        json::array log;
        if (log_from > 0) // 0: no log file (LLFile::size's answer for a missing file)
        {
            const std::vector<std::string> lines = readLogLines(before->log, log_from, CUE_LOG_BYTES, false);
            const std::string stem = file.substr(0, file.size() - std::strlen(TOGGLE_SUFFIX));
            std::vector<std::string> picked;
            for (const std::string& line : lines)
            {
                if (line.find("PlusPoserMcp") != std::string::npos)
                    continue; // the tool's own line
                if (hasCueTag(line) || line.find(stem) != std::string::npos)
                    picked.push_back(shortLine(line));
            }
            const std::size_t first = picked.size() > MAX_CUE_LOG_LINES ? picked.size() - MAX_CUE_LOG_LINES : 0;
            for (std::size_t i = first; i < picked.size(); ++i)
                log.push_back(json::string(picked[i]));
        }
        const bool no_log = log.empty();
        result["log"] = std::move(log); // the switches' lines written since the cue (newest last)
        if (took_back)
            result["note"] = "The viewer did not take the cue in time (is it drawing frames? a feature's cue is looked for "
                             "only while the feature runs): the file was taken back, nothing changed.";
        else if (!taken)
            result["note"] = "The cue file is still there and could not be taken back: the viewer may take it later.";
        else if (no_log)
            result["note"] = "No log line yet: the log file may not be flushed yet (env_switches or cue_list show the "
                             "switches now).";
        return ToolResult::json(result);
    }

    ToolResult envSwitches(const MainThreadRunner& run, const json::object& args)
    {
        const bool with_log = [&args]()
        {
            const json::value* v = args.if_contains("log");
            if (!v || v->is_null())
                return true;
            if (!v->is_bool())
                throw ToolError("'log' must be true or false");
            return v->get_bool();
        }();

        struct Read
        {
            std::vector<EnvRow> rows;
            std::string         log;
        };
        auto read = std::make_shared<Read>();
        ToolResult result = run(
            [read](const JobProgress&) -> std::optional<ToolResult>
            {
                read->rows = envRows();
                read->log  = LLError::logFileName();
                return ToolResult::text("ok");
            },
            READ_TIMEOUT);
        if (result.isError)
            return result;

        std::vector<std::string> lines;
        if (with_log)
            lines = readLogLines(read->log, 0, ENV_LOG_BYTES, true);
        json::array switches;
        for (EnvRow& r : read->rows)
        {
            if (with_log && !r.marker.empty())
            {
                // The newest line of the switch's own messages: the auto decision and its counts.
                const auto it = std::find_if(lines.rbegin(), lines.rend(),
                                             [&r](const std::string& line) { return line.find(r.marker) != std::string::npos; });
                r.row["last_log"] = it == lines.rend() ? json::value(nullptr) : json::value(json::string(shortLine(*it)));
            }
            switches.push_back(std::move(r.row));
        }
        json::object out;
        out["switches"] = std::move(switches);
        out["note"] = "Read at startup from the environment (set: the variable's value, null: unset, the default) and as they "
                      "are now: an auto mode verifies first, turns on after enough clean comparisons and off for the run at "
                      "the first mismatch (state, on, verify, auto). last_log: the newest log line of the switch (its auto "
                      "decision with the counts, or its periodic counts; lines still in the log's buffer are not seen "
                      "yet). Changing an environment switch needs a restart; "
                      "many have a cue (cue_list) that flips them while the viewer runs.";
        return ToolResult::json(out);
    }

    // ToolError -> a tool error (the handlers run on the connection thread).
    template <class F> ToolResult guarded(F&& f)
    {
        try
        {
            return f();
        }
        catch (const ToolError& e)
        {
            return ToolResult::error(e.what());
        }
    }
}

namespace plus::posermcp
{
    void appendDebugTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        {
            Tool t;
            t.name        = "cue_list";
            t.title       = "List render cues";
            t.description = "Lists the cue files of this viewer (LLRHI_CUE_DIR/<name>.toggle flips a render switch while it "
                            "runs): file, label, on (the switch now; null when only its log line tells), available, and "
                            "whether this start has a cue folder (development starts only). filter: part of a name or label.";
            t.readOnly    = true;
            json::object p;
            p["filter"]   = prop("string", "Part of the file name or label (any case).");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = [run](const json::object& args) -> ToolResult { return guarded([&] { return cueList(run, args); }); };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "cue_toggle";
            t.title       = "Flip a render cue";
            t.description = "Flips one render switch through its cue file (a name from cue_list, e.g. \"shadowcache\" or "
                            "\"texhandles.toggle\"): writes it into LLRHI_CUE_DIR, waits until the viewer takes it (it looks "
                            "once a second while drawing), and returns the value before and after and the viewer's log lines "
                            "about it. Not taken in time: the file is taken back, nothing changes. Only in development starts "
                            "(LLRHI_CUE_DIR set). A cue is not saved: the next start has the switch as before.";
            json::object p;
            p["name"]         = prop("string", "The cue's name, with or without .toggle (cue_list).");
            p["wait_seconds"] = prop("number", "How long to wait for the viewer to take it, 0.5-15. Default 4.");
            t.inputSchema     = objectSchema(std::move(p), { "name" });
            t.handler         = [run](const json::object& args) -> ToolResult { return guarded([&] { return cueToggle(run, args); }); };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "env_switches";
            t.title       = "Read render environment switches";
            t.description = "The LLRHI_* switches read at startup (LLRHI_SCENE, LLRHI_TEX_HANDLES, LLRHI_VB_HANDLES, "
                            "LLRHI_LIGHT_TABLE, ...) as they are now: the variable's value, the mode, where an auto mode went "
                            "(verifying, on, or off after a mismatch), the scene's drawing and problem count, and the newest "
                            "log line of each (its auto decision with the mismatch counts). Read only.";
            t.readOnly    = true;
            json::object p;
            p["log"]      = prop("boolean", "Add each switch's newest log line (default true).");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = [run](const json::object& args) -> ToolResult { return guarded([&] { return envSwitches(run, args); }); };
            tools.push_back(std::move(t));
        }
    }

    std::string debugInstructions()
    {
        return "RENDER SWITCHES (development starts): env_switches reads the LLRHI_* environment switches as they are now "
               "(auto modes: verifying, on, or off after a mismatch; last_log has the counts). cue_list lists the cue files "
               "that flip render switches while the viewer runs; cue_toggle flips one and returns before/after and the "
               "viewer's log lines (only when the viewer was started with LLRHI_CUE_DIR). Cues are not saved.\n";
    }
}
