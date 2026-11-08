/**
 * @file posehistorytools.cpp
 * @brief The viewer side of the poser MCP's pose history. See posehistorytools.h.
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

// Everything here runs on the main thread (inside MainThreadSteps and the idle
// callback); the tool handlers only parse arguments on the connection thread
// and hand the work over with run(). So the history needs no lock.
//
// Only the own avatar has a history: its folder is per agent
// (user_settings/plus_poser_history/<agent id>/), and animesh are left out
// (they come and go, and their UUID changes when rezzed again).

#include "llviewerprecompiledheaders.h"

#include "posehistorytools.h"
#include "mcpschema.h"

#include "llagent.h"
#include "lltimer.h"
#include "llvoavatarself.h"

#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>

namespace json = boost::json;
using plus::mcp::Content;
using plus::mcp::Tool;
using plus::mcp::ToolResult;
using plus::posermcp::applySelfPose;
using plus::posermcp::captureSelfPose;
using plus::posermcp::JobProgress;
using plus::posermcp::MainThreadRunner;
using plus::posermcp::MainThreadStep;
namespace history = plus::posermcp::history;

namespace
{
    constexpr char        HISTORY_DIRECTORY[] = "plus_poser_history"; // in user_settings, next to "poses"
    constexpr std::size_t HISTORY_CAPACITY    = 100;
    constexpr double      SETTLE_SECONDS      = 1.5;  // a window edit is one entry once the pose was still this long
    constexpr F64         POLL_SECONDS        = 0.5;  // how often the idle callback looks at the pose (~0.5 ms each)
    constexpr F64         HOLD_SECONDS        = 3.0;  // see Tracker::holdIdle
    constexpr int         MAX_LIST            = 100;

    using plus::mcp::ToolError;      // mcpschema.h (research/275 L110)
    using plus::mcp::prop;
    using plus::mcp::objectSchema;

    double unixNow()
    {
        using namespace std::chrono;
        return duration<double>(system_clock::now().time_since_epoch()).count();
    }

    std::string localTime(double unix_seconds)
    {
        const std::time_t t = static_cast<std::time_t>(unix_seconds);
        std::tm           tm{};
#if LL_WINDOWS
        if (localtime_s(&tm, &t) != 0)
            return "?";
#else
        if (!localtime_r(&t, &tm))
            return "?";
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
        return buffer;
    }

    std::string agoText(double seconds)
    {
        if (seconds < 0)
            seconds = 0;
        if (seconds < 90)
            return std::to_string((long long)std::llround(seconds)) + " s ago";
        if (seconds < 90 * 60)
            return std::to_string((long long)std::llround(seconds / 60)) + " min ago";
        if (seconds < 36 * 3600)
            return std::to_string((long long)std::llround(seconds / 3600)) + " h ago";
        return std::to_string((long long)std::llround(seconds / 86400)) + " days ago";
    }

    double rounded(double value, int decimals)
    {
        const double factor = std::pow(10.0, decimals);
        const double r      = std::round(value * factor) / factor;
        return r == 0.0 ? 0.0 : r;
    }

    // ---- arguments ----

    std::optional<std::string> argString(const json::object& args, const char* key)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        if (v->is_string())
            return std::string(v->get_string());
        if (v->is_int64() || v->is_uint64())
            return json::serialize(*v); // an id given as a number
        throw ToolError(std::string("'") + key + "' must be a string");
    }

    std::optional<int> argInt(const json::object& args, const char* key, int min_value, int max_value)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        double d = 0;
        if (v->is_int64())
            d = (double)v->get_int64();
        else if (v->is_uint64())
            d = (double)v->get_uint64();
        else if (v->is_double())
            d = v->get_double();
        else
            throw ToolError(std::string("'") + key + "' must be a whole number");
        if (!std::isfinite(d) || d != std::floor(d) || d < min_value || d > max_value)
            throw ToolError(std::string("'") + key + "' must be a whole number from " + std::to_string(min_value) + " to " +
                            std::to_string(max_value));
        return (int)d;
    }

    std::optional<double> argNumber(const json::object& args, const char* key)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        double d = 0.0;
        if (v->is_double())
            d = v->get_double();
        else if (v->is_int64())
            d = (double)v->get_int64();
        else if (v->is_uint64())
            d = (double)v->get_uint64();
        else
            throw ToolError(std::string("'") + key + "' must be a number");
        // Finite and within F32: the poser keeps F32, where 1e39 would become inf.
        if (!std::isfinite(d) || std::fabs(d) > std::numeric_limits<F32>::max())
            throw ToolError(std::string("'") + key + "' must be a finite number");
        return d;
    }

    // The own avatar ("self", empty, or its UUID); animesh have no history.
    bool targetsSelf(const json::object& args)
    {
        const json::value* v = args.if_contains("avatar");
        if (!v || v->is_null())
            return true;
        if (!v->is_string())
            return false; // the tool reports the bad argument itself
        const std::string which(v->get_string());
        return which.empty() || boost::iequals(which, "self") || (gAgentID.notNull() && boost::iequals(which, gAgentID.asString()));
    }

    // ---- what a tool call was about (the entry's summary) ----

    std::string joinNames(const std::vector<std::string>& names, std::size_t shown)
    {
        std::string text;
        for (std::size_t i = 0; i < names.size() && i < shown; ++i)
            text += (i ? ", " : "") + names[i];
        if (names.size() > shown)
            text += ", ... (" + std::to_string(names.size()) + ")";
        return text;
    }

    std::vector<std::string> namesIn(const json::object& args, const char* list, const char* field)
    {
        std::vector<std::string> names;
        const json::value*       v = args.if_contains(list);
        if (!v)
            return names;
        if (v->is_string())
            names.emplace_back(v->get_string());
        else if (v->is_array())
        {
            for (const json::value& item : v->get_array())
            {
                if (item.is_string())
                    names.emplace_back(item.get_string());
                else if (field && item.is_object())
                {
                    const json::value* name = item.get_object().if_contains(field);
                    if (name && name->is_string())
                        names.emplace_back(name->get_string());
                }
            }
        }
        return names;
    }

    std::string summarize(const std::string& tool, const json::object& args)
    {
        std::string text = tool;
        if (tool == "set_joints")
        {
            const json::value* mode = args.if_contains("mode");
            text += std::string(" ") + (mode && mode->is_string() ? mode->get_string().c_str() : "absolute") + ": " +
                    joinNames(namesIn(args, "joints", "joint"), 6);
        }
        else if (tool == "aim")
        {
            const std::vector<std::string> bones = namesIn(args, "bones", "joint");
            text += " " + std::to_string(bones.size()) + " bones: " + joinNames(bones, 6);
        }
        else if (tool == "reach")
            text += " " + joinNames(namesIn(args, "limbs", "limb"), 4);
        else if (tool == "reset_joint")
            text += " " + joinNames(namesIn(args, "joints", nullptr), 6);
        else if (tool == "solve_pose")
        {
            // "solve_pose 7 goals: contact, on_floor, torso_lean, ..." (the kinds, each once)
            std::vector<std::string> kinds;
            const std::vector<std::string> types = namesIn(args, "goals", "type");
            for (const std::string& t : types)
            {
                if (std::find(kinds.begin(), kinds.end(), t) == kinds.end())
                    kinds.push_back(t);
            }
            text += " " + std::to_string(types.size()) + " goals: " + joinNames(kinds, 6);
        }
        else
        {
            // load_pose, reset_all, start_posing, and tools added later: the arguments, short
            json::object shown = args;
            shown.erase("avatar");
            if (!shown.empty())
            {
                std::string compact = json::serialize(shown);
                if (compact.size() > 160)
                    compact = utf8str_truncate(compact, 157) + "...";   // not mid-character
                text += " " + compact;
            }
        }
        return text;
    }

    // ---- the tracker ----

    class Tracker
    {
    public:
        // The own avatar's history, loaded from its folder on first use. Null
        // before login.
        history::PoseHistory* history()
        {
            if (gAgentID.isNull() || !isAgentAvatarValid())
                return nullptr;
            if (!mHistory || mAgent != gAgentID)
            {
                mAgent   = gAgentID;
                mHistory = std::make_unique<history::PoseHistory>(HISTORY_CAPACITY);
                const std::string folder =
                    gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, HISTORY_DIRECTORY, mAgent.asString());
                mLoad = mHistory->attachDirectory(folder, mAgent.asString());
                LL_INFOS("PlusPoserMcp") << "Pose history: " << mLoad.loaded << " entries from " << folder << ", "
                                         << mLoad.skipped << " files skipped" << LL_ENDL;
                for (std::size_t i = 0; i < mLoad.problems.size() && i < 5; ++i)
                    LL_WARNS("PlusPoserMcp") << "Pose history: " << mLoad.problems[i] << LL_ENDL;
                forget();
            }
            return mHistory.get();
        }

        const history::PoseHistory::LoadReport& loadReport() const { return mLoad; }

        void forget()
        {
            mHaveSeen = false;
            mSeen     = history::Pose();
            mDebouncer.reset();
        }

        // The pose as last recorded or restored: later differences are edits.
        void setSeen(const history::Pose& pose)
        {
            mSeen     = pose;
            mHaveSeen = true;
            mDebouncer.setBaseline(history::poseHash(pose));
        }

        // Records an edit not recorded yet (the window, before it settled).
        void flushEdit(history::PoseHistory& h, const history::Pose& live)
        {
            if (mHaveSeen && history::poseHash(live) != mDebouncer.baseline() && !history::samePose(mSeen, live))
                recordEdit(h, live);
            setSeen(live);
        }

        // While a pose-changing tool runs over several frames, its changes are
        // not window edits. The hold runs out by itself in case the job is
        // abandoned (timed out) and never finishes; whatever it changed is
        // then recorded as an edit.
        void holdIdle() { mHoldUntil = LLTimer::getTotalSeconds() + HOLD_SECONDS; }
        void releaseIdle() { mHoldUntil = 0.0; }

        void idle()
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (now < mNextPoll || now < mHoldUntil)
                return;
            mNextPoll = now + POLL_SECONDS;

            history::Pose live;
            if (!captureSelfPose(live))
            {
                forget(); // not posing: start again from the pose found next time
                return;
            }
            history::PoseHistory* h = history();
            if (!h)
                return;
            if (!mHaveSeen)
            {
                setSeen(live);
                return;
            }
            if (mDebouncer.observe(history::poseHash(live), now))
            {
                if (!history::samePose(mSeen, live))
                    recordEdit(*h, live);
                setSeen(live);
            }
        }

    private:
        void recordEdit(history::PoseHistory& h, const history::Pose& live)
        {
            const std::string what = history::describeChange(history::changeStats(mSeen, live));
            h.record(mSeen, live, "manual", "poser window: " + what, unixNow());
        }

        LLUUID                                mAgent;
        std::unique_ptr<history::PoseHistory> mHistory;
        history::PoseHistory::LoadReport      mLoad;
        history::Pose                         mSeen;
        bool                                  mHaveSeen = false;
        history::ChangeDebouncer              mDebouncer{ SETTLE_SECONDS };
        F64                                   mNextPoll = 0.0;
        F64                                   mHoldUntil = 0.0;
    };

    Tracker& tracker()
    {
        static Tracker sTracker;
        return sTracker;
    }

    // The history and the live pose, for the history tools.
    history::PoseHistory& requireHistory()
    {
        history::PoseHistory* h = tracker().history();
        if (!h)
            throw ToolError("The viewer is not logged in yet.");
        return *h;
    }

    history::Pose requireLivePose()
    {
        history::Pose live;
        if (!captureSelfPose(live))
            throw ToolError("Your avatar is not being posed; call start_posing first (the history is kept).");
        return live;
    }

    // ---- results ----

    std::size_t basePosition(const history::PoseHistory& h)
    {
        return h.position() < h.size() ? h.position() : h.size();
    }

    json::object entryJson(const history::PoseHistory& h, const history::Entry& e, double now)
    {
        std::size_t index = 0;
        while (index < h.size() && h.entries()[index].id != e.id)
            ++index;

        json::object o;
        o["id"]   = e.id;
        o["time"] = localTime(e.time);
        o["ago"]  = agoText(now - e.time);
        if (!e.label.empty())
            o["mark"] = e.label;
        if (e.isChange())
        {
            o["source"]  = e.source;
            o["summary"] = e.summary;
            o["changed_joints"] = e.change.joints;
            if (e.change.joints > 0)
            {
                o["max_deg"] = rounded(e.change.maxDeg, 1);
                json::array top;
                for (const std::string& item : e.change.top)
                    top.push_back(json::string(item));
                o["changes"] = std::move(top);
            }
        }
        else
        {
            o["source"]  = "saved";
            o["summary"] = e.summary;
        }
        // undo steps=N reaches this entry (negative: redo -N)
        o["steps_back"] = (std::int64_t)basePosition(h) - (std::int64_t)index;
        if (index == h.position())
            o["current"] = true;
        return o;
    }

    void addPosition(json::object& result, const history::PoseHistory& h)
    {
        result["undo_available"] = h.undoAvailable();
        result["redo_available"] = h.redoAvailable();
    }

    void annotate(ToolResult& result, std::uint64_t id)
    {
        if (result.isError || result.content.empty() || result.content.front().type != Content::Type::Text)
            return;
        boost::system::error_code ec;
        json::value               value = json::parse(result.content.front().text, ec);
        if (!ec && value.is_object())
        {
            value.get_object()["history_id"] = id;
            result.content.front().text      = json::serialize(value);
        }
        else
        {
            result.content.push_back(Content::makeText("history_id " + std::to_string(id) + " (undo returns to the pose "
                                                                                             "before this call)"));
        }
    }

    // ---- the moves ----

    ToolResult moveResult(history::PoseHistory& h, const history::PoseHistory::Move& move, double now)
    {
        if (!move.entry)
            throw ToolError("The history entry went missing.");
        const history::Pose target  = move.entry->pose; // copy: the list may change below
        const std::uint64_t id      = move.entry->id;
        const std::size_t   written = applySelfPose(target);

        history::Pose after;
        if (captureSelfPose(after))
            tracker().setSeen(after);

        json::object result;
        result["now_at"]         = entryJson(h, *h.findById(id), now);
        result["steps"]          = move.steps;
        result["joints_written"] = written;
        addPosition(result, h);

        json::array not_restored;
        for (const history::JointDiff& d : history::diffPoses(target, after))
        {
            if (!d.missing)
                not_restored.push_back(json::string(d.joint));
        }
        if (!not_restored.empty())
        {
            result["not_restored"] = std::move(not_restored);
            result["note"] = "These joints did not take the saved values (another animation may hold them); check with get_pose.";
        }
        result["next"] = move.steps == 0 ? "Already there." : "Take a snapshot to check. redo goes forward again until the next change.";
        return ToolResult::json(result);
    }

    ToolResult undoStep(const json::object& args)
    {
        const std::optional<int>         steps = argInt(args, "steps", 1, MAX_LIST);
        const std::optional<std::string> to    = argString(args, "to");
        if (steps && to)
            throw ToolError("give either steps or to, not both");

        history::PoseHistory& h    = requireHistory();
        const history::Pose   live = requireLivePose();
        tracker().flushEdit(h, live);
        const double now = unixNow();
        const auto   move = to ? h.undoTo(*to, live, now) : h.undo(steps.value_or(1), live, now);
        return moveResult(h, move, now);
    }

    ToolResult redoStep(const json::object& args)
    {
        const int             steps = argInt(args, "steps", 1, MAX_LIST).value_or(1);
        history::PoseHistory& h     = requireHistory();
        const history::Pose   live  = requireLivePose();
        tracker().flushEdit(h, live);
        const double now = unixNow();
        return moveResult(h, h.redo(steps, live, now), now);
    }

    ToolResult markStep(const json::object& args)
    {
        const std::optional<std::string> name = argString(args, "name");
        if (!name)
            throw ToolError("'name' is required");
        history::PoseHistory& h    = requireHistory();
        const history::Pose   live = requireLivePose();
        tracker().flushEdit(h, live);
        const double         now   = unixNow();
        const history::Entry& entry = h.mark(*name, live, now);

        json::object result;
        result["marked"] = entryJson(h, entry, now);
        result["next"]   = "undo to=\"" + *name + "\" returns to this pose; compare from=\"" + *name + "\" shows what changed since.";
        return ToolResult::json(result);
    }

    ToolResult historyStep(const json::object& args)
    {
        const int             limit = argInt(args, "limit", 1, MAX_LIST).value_or(20);
        history::PoseHistory& h     = requireHistory();

        // Record a pending window edit so the list is up to date.
        history::Pose live;
        const bool    posing = captureSelfPose(live);
        if (posing)
            tracker().flushEdit(h, live);

        const double now = unixNow();
        json::array  list;
        for (std::size_t i = h.size(); i > 0 && list.size() < (std::size_t)limit; --i)
            list.push_back(entryJson(h, h.entries()[i - 1], now));

        json::object result;
        result["entries"] = std::move(list); // newest first
        result["total"]   = h.size();
        result["capacity"] = h.capacity();
        if (h.position() >= h.size())
            result["position"] = "the live pose is newer than every entry (undo 1 returns to the newest entry)";
        addPosition(result, h);
        if (!posing)
            result["posing"] = false;
        result["folder"] = h.directory();
        const history::PoseHistory::LoadReport& load = tracker().loadReport();
        if (load.loaded)
            result["loaded_from_disk"] = load.loaded;
        if (load.skipped)
            result["damaged_files_skipped"] = load.skipped;
        if (!h.lastFileError().empty())
            result["file_error"] = h.lastFileError();
        result["note"] = "Own avatar only (animesh are not recorded). source mcp:<tool> = before that tool call, manual = "
                         "before an edit in the poser window, saved = a pose kept by undo or mark. steps_back: undo "
                         "steps=N reaches the entry (negative: redo).";
        return ToolResult::json(result);
    }

    ToolResult compareStep(const json::object& args)
    {
        const std::optional<std::string> from  = argString(args, "from");
        const std::optional<int>         steps = argInt(args, "steps", 0, MAX_LIST);
        const std::string                to    = argString(args, "to").value_or("now");
        const double                     min_deg = argNumber(args, "min_deg").value_or(1.0);
        const int                        limit   = argInt(args, "limit", 1, 300).value_or(30);
        if (from && steps)
            throw ToolError("give either from or steps, not both");
        if (min_deg < 0)
            throw ToolError("'min_deg' must not be negative");

        history::PoseHistory& h = requireHistory();
        history::Pose         live;
        const bool            posing = captureSelfPose(live);
        if (posing)
            tracker().flushEdit(h, live);

        const history::Entry& a = from ? h.find(*from) : h.stepsBack(steps.value_or(1));
        const std::uint64_t   a_id = a.id;
        history::Pose         b;
        std::string           b_name;
        if (boost::iequals(to, "now"))
        {
            if (!posing)
                throw ToolError("Your avatar is not being posed, so there is no current pose; give 'to' an id or mark.");
            b      = live;
            b_name = "now";
        }
        else
        {
            const history::Entry& e = h.find(to);
            b                       = e.pose;
            b_name                  = "#" + std::to_string(e.id) + (e.label.empty() ? "" : " (" + e.label + ")");
        }
        const history::Entry& from_entry = *h.findById(a_id);

        json::array joints, missing;
        std::size_t differing = 0;
        double      max_deg   = 0.0;
        for (const history::JointDiff& d : history::diffPoses(from_entry.pose, b))
        {
            if (d.missing)
            {
                missing.push_back(json::string(d.joint));
                continue;
            }
            ++differing;
            max_deg = std::max(max_deg, d.angleDeg);
            const bool shown = d.angleDeg >= min_deg || d.positionM >= 0.005 || d.scale >= 0.01 || d.enabledChanged;
            if (!shown || joints.size() >= (std::size_t)limit)
                continue;
            json::object row;
            row["joint"]     = d.joint;
            row["angle_deg"] = rounded(d.angleDeg, 1);
            if (d.positionM >= 0.0005)
                row["position_cm"] = rounded(d.positionM * 100.0, 1);
            if (d.scale >= 0.001)
                row["scale"] = rounded(d.scale, 3);
            if (d.enabledChanged)
                row["enabled_changed"] = true;
            joints.push_back(std::move(row));
        }

        json::object result;
        result["from"]   = entryJson(h, from_entry, unixNow());
        result["to"]     = b_name;
        result["joints"] = std::move(joints); // largest rotation first
        result["joints_differing"] = differing;
        result["max_deg"]          = rounded(max_deg, 1);
        if (!missing.empty())
            result["only_in_one"] = std::move(missing);
        result["note"] = "angle_deg is the rotation between the two poses' local rotations of that joint (a parent's "
                         "change moves its children without showing on them). Shown: angle >= min_deg, or moved >= 0.5 "
                         "cm, or scaled.";
        return ToolResult::json(result);
    }

    // ---- schema helpers ----

    json::object idOrMarkProp(const std::string& description)
    {
        return json::object{ { "type", json::array{ "string", "integer" } }, { "description", description } };
    }
}

namespace plus::posermcp
{
    // <plus> A drag in the 3D view (IK drag): the pose before it is kept, the drag's own changes are not
    // window edits, and it is recorded once, when the mouse is let go.
    namespace
    {
        history::Pose sUserBefore;
        bool          sUserActive = false;
    }

    void historyIdle()
    {
        // While the mouse is held in a drag, nothing is a window edit, even when the mouse stands still for
        // longer than the hold (the drag is recorded once, by endUserEdit).
        if (sUserActive)
            return;
        tracker().idle();
    }

    bool beginUserEdit()
    {
        if (sUserActive)
            return true;
        Tracker&              t = tracker();
        history::PoseHistory* h = t.history();
        if (!h || !captureSelfPose(sUserBefore))
            return false;
        t.flushEdit(*h, sUserBefore); // a window edit that has not settled yet comes first
        t.holdIdle();
        sUserActive = true;
        return true;
    }

    void holdUserEdit()
    {
        if (sUserActive)
            tracker().holdIdle();
    }

    void endUserEdit(const std::string& source, const std::string& summary)
    {
        if (!sUserActive)
            return;
        sUserActive = false;
        Tracker& t  = tracker();
        t.releaseIdle();
        history::PoseHistory* h = t.history();
        history::Pose         after;
        if (!h || !captureSelfPose(after))
        {
            t.forget();
            return;
        }
        h->record(sUserBefore, after, source, summary, unixNow());
        t.setSeen(after);
    }
    // </plus>

    MainThreadStep withHistory(const std::string& tool, const json::object& args, MainThreadStep step)
    {
        // This runs on the connection thread: whether the call is about the own avatar
        // (targetsSelf reads gAgentID) is asked in the step, on the main thread.

        // A step may run over several frames (waiting for the skeleton, or
        // contact's rounds that change the pose every frame): the pose from
        // before its first frame is kept here and recorded once, when the step
        // is done, as one entry.
        struct Call
        {
            bool          started = false;
            bool          tracked = false;
            history::Pose before;
        };
        auto call = std::make_shared<Call>();

        return [tool, args, step = std::move(step), call](const JobProgress& progress) -> std::optional<ToolResult>
        {
            Tracker& t = tracker();
            if (!call->started)
            {
                call->started           = true;
                history::PoseHistory* h = targetsSelf(args) ? t.history() : nullptr;
                call->tracked           = h && captureSelfPose(call->before);
                if (call->tracked)
                    t.flushEdit(*h, call->before); // a window edit that has not settled yet comes first
            }
            if (call->tracked)
                t.holdIdle(); // the step's own changes are not window edits

            auto finish = [&](ToolResult* result)
            {
                if (!call->tracked)
                    return;
                call->tracked = false;
                t.releaseIdle();
                history::PoseHistory* h = t.history();
                history::Pose         after;
                if (!h || !captureSelfPose(after))
                {
                    t.forget(); // the tool stopped posing
                    return;
                }
                const std::uint64_t id = h->record(call->before, after, "mcp:" + tool, summarize(tool, args), unixNow());
                t.setSeen(after);
                if (id && result)
                    annotate(*result, id);
            };

            std::optional<ToolResult> result;
            try
            {
                result = step(progress);
            }
            catch (...)
            {
                finish(nullptr); // keep what changed before the error
                throw;
            }
            if (result)
                finish(&*result);
            return result;
        };
    }

    void appendHistoryTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        auto handler = [run](ToolResult (*impl)(const json::object&))
        {
            return [run, impl](const json::object& args) -> ToolResult
            {
                return run([impl, args](const JobProgress&) -> std::optional<ToolResult> { return impl(args); }, 15.0);
            };
        };

        {
            Tool t;
            t.name        = "undo";
            t.title       = "Undo pose changes";
            t.description = "Goes back in the pose history of your own avatar (animesh are not recorded): steps=N (default 1) entries back, or to=<id "
                            "or mark name> (from history or mark; may also go forward). Every pose-changing tool saves "
                            "the pose before it runs (its result has history_id), and edits in the poser window are saved "
                            "too, so undo 1 right after a tool call restores the pose from before that call. undo only "
                            "moves in the history; redo goes forward again until the next change. Nothing is thrown "
                            "away: a change after an undo is added at the end. Use it whenever a step made the pose "
                            "worse, instead of repairing by hand.";
            json::object p;
            p["steps"]    = prop("integer", "How many entries back (1-100). Default 1.");
            p["to"]       = idOrMarkProp("An entry id (\"12\" or \"#12\") or a mark name.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = handler(&undoStep);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "redo";
            t.title       = "Redo pose changes";
            t.description = "Goes forward again in the pose history after undo (steps, default 1). Not available after "
                            "a new change (use history and undo to=<id> to reach any saved pose).";
            json::object p;
            p["steps"]    = prop("integer", "How many entries forward (1-100). Default 1.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = handler(&redoStep);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "history";
            t.title       = "Pose history";
            t.description = "Lists the pose history of your own avatar, newest first: id, time, source (mcp:<tool> = the "
                            "pose before that call, manual = before an edit in the poser window, saved = kept by undo or "
                            "mark), summary, mark, changed_joints and the largest changes, steps_back (undo steps=N "
                            "reaches it) and which entry is current. Kept on disk: after a restart of the viewer the "
                            "history is still there (newest 100 entries; marked entries are dropped last).";
            t.readOnly    = true;
            json::object p;
            p["limit"]    = prop("integer", "How many entries (1-100). Default 20.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = handler(&historyStep);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "mark";
            t.title       = "Mark the pose";
            t.description = "Names the current pose in the history (e.g. \"arms ok\") so that undo to=\"arms ok\" or "
                            "compare from=\"arms ok\" can find it later, even after many changes or a restart. Mark a "
                            "good state before trying something risky. A name used before moves to the current pose. "
                            "Names: 1-40 characters, not starting with a digit or '#'.";
            json::object p;
            p["name"]     = prop("string", "The mark's name.");
            t.inputSchema = objectSchema(std::move(p), { "name" });
            t.handler     = handler(&markStep);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "compare";
            t.title       = "Compare two poses";
            t.description = "Which joints differ between an earlier pose in the history and now (or another entry), "
                            "largest rotation first: angle_deg per joint (the angle between the two local rotations), "
                            "position_cm for moved joints. from=<id or mark> or steps=N (default 1: the pose undo 1 would "
                            "restore); to=\"now\" (default) or <id or mark>. Use it to see what a tool call really "
                            "changed, or what differs from a good marked pose.";
            t.readOnly    = true;
            json::object p;
            p["from"]     = idOrMarkProp("An entry id or mark name to compare from.");
            p["steps"]    = prop("integer", "Instead of from: that many entries back (0-100). Default 1.");
            p["to"]       = idOrMarkProp("\"now\" (default) or an entry id or mark name.");
            p["min_deg"]  = prop("number", "Only list joints that turned at least this much (default 1).");
            p["limit"]    = prop("integer", "At most this many joints (default 30).");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = handler(&compareStep);
            tools.push_back(std::move(t));
        }
    }

    std::string historyInstructions()
    {
        return "HISTORY AND UNDO (your own avatar): every tool that changes the pose first saves the whole pose (its "
               "result has history_id), and edits made by hand in the poser window are saved too. When a step made the "
               "pose worse, call undo (steps, or to=<id or mark>) instead of repairing it by hand; redo goes forward "
               "again. Before a risky change, mark the good state with a name (mark name=\"...\"), then undo "
               "to=\"<name>\" brings it back. history lists the saved poses, compare shows which joints differ from an "
               "earlier pose (largest first). The history is kept across restarts of the viewer.\n";
    }
}
