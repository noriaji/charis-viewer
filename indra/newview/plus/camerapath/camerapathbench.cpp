/**
 * @file camerapathbench.cpp
 * @brief Measuring runs of a camera path: the same moves played the same way every time, started and stopped by
 *        cue files, with the frame times of every lap in the log (research/153-moving-measure.md).
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

// Only with LLRHI_CUE_DIR (tools/launch.ps1 and daily.ps1 set it; the downloadable viewer does not): then, once a
// second, <LLRHI_CUE_DIR>/camerapath.run is looked for. It is read, removed, and its runs are played one after the
// other; <LLRHI_CUE_DIR>/camerapath.stop ends them. The file (text, UTF-8; '#' starts a comment):
//
//   path    = C:/path/to/leeaf-walk.xml                                 a full file name (.xml or a .json script),
//                                         a file name in the cue folder, or the name of a saved path
//   laps    = 3                           laps measured (1 to 100)
//   warmup  = 1                           laps played first and not measured (0 to 10)
//   speed   = walk                        walk (3.2 m/s), run (5.1), fly (16), a number of m/s, or path (the path's
//                                         own times). With a speed every move's time is made from its length
//   turn    = 90                          degrees a second for the turns (with a speed; 5 to 720)
//   label   = shift-on                    the run's name in the log and the tables (no spaces: they become _)
//   set PlusShadowStabilize = 1           a setting for this run only (bool, S32, U32 or F32; not saved; put back after)
//   toggle  = drawlistsopaque             <name>.toggle is made at the start and again at the end (flipped and back)
//   scene   = heavy-leeaf                 a test scene (plus/testscene; needs PlusTestScene on, "set PlusTestScene = 1"
//                                         will do) built before the run, after its settings; carries on like path, kept
//                                         from run to run while the name stays, removed when the runs end
//   ---                                   the next run: path, laps, warmup, speed and turn carry on, the rest does not
//   rounds  = 2                           the whole list played this many times (A B A B ...; 1 to 20)
//
// Every run plays the path from its first point with no countdown, round and round (the open path is replaced by
// the file's; the camera stays where the run ends). A run ends early, and the rest are dropped, when the path is
// stopped some other way (Esc, the window, the flycam, another camera tool), when the avatar changes region, or by
// camerapath.stop. Settings and toggles are put back in every case.
//
// The log ("PerfMark" lines, one line each; tools/perf-summary.py reads them):
//   begin   run=N label=... round=R path=... laps=L warmup=W speed=... turn=... lap_s=... region=... set=... toggle=...
//           scene=<name>:<hash> (or -)
//   measure run=N label=...                     the warm-up laps are over (with warmup=0: right after begin)
//   lap     run=N label=... lap=K warmup=0|1 frames=... avg=... p50=... p95=... p99=... max=... over50=...   (ms)
//   end     run=N label=... frames=... avg=... p50=... p95=... p99=... max=... over50=... fps=... hist=bin:count,...
//   abort   run=N label=... reason=...
//   done    runs=N
// hist: the measured frames in 0.5 ms bins (bin k holds k/2 to (k+1)/2 ms), for joining runs exactly.
//
// The cost: nothing without LLRHI_CUE_DIR. With it, one clock comparison a frame while no run plays; while one plays,
// one float a frame into a vector reserved once (the numbers are worked out at the end of each lap).

#include "llviewerprecompiledheaders.h"

#include "camerapath.h"

#include "testscene/testscene.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "llagent.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llrhi.h"
#include "llsdutil.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

namespace plus::camerapath
{
    namespace
    {
        constexpr char   RUN_CUE[]        = "camerapath.run";
        constexpr char   STOP_CUE[]       = "camerapath.stop";
        constexpr S64    MAX_CUE_BYTES    = 64 * 1024;
        constexpr F64    POLL_SECONDS     = 1.0;
        constexpr F64    TOGGLE_WAIT      = 10.0;   // seconds the next run waits for the last run's toggles to be taken
        constexpr F64    GAP_SECONDS      = 2.0;    // between runs
        constexpr int    MAX_LAPS         = 100;
        constexpr int    MAX_WARMUP       = 10;
        constexpr int    MAX_ROUNDS       = 20;
        constexpr size_t MAX_RUNS         = 64;     // after the rounds
        constexpr double WALK_SPEED       = 3.2;    // m/s, an avatar walking
        constexpr double RUN_SPEED        = 5.1;
        constexpr double FLY_SPEED        = 16.0;
        constexpr double MIN_SPEED        = 0.1;
        constexpr double MAX_SPEED        = 100.0;
        constexpr double DEFAULT_TURN     = 90.0;   // degrees a second
        constexpr double MIN_TURN         = 5.0;
        constexpr double MAX_TURN         = 720.0;
        constexpr double MIN_MOVE_SECONDS = 0.2;
        constexpr double MAX_MOVE_SECONDS = 600.0;  // as camerapath.cpp
        constexpr double TWO_PI           = 6.283185307179586;
        constexpr double DEGREES          = 57.29577951308232;
        constexpr float  HITCH_MS         = 50.f;
        constexpr float  BINS_PER_MS      = 2.f;
        constexpr int    MAX_BIN          = 2000;   // 1 s; longer frames go in the last bin
        constexpr size_t LAP_RESERVE      = 8192;
        constexpr size_t RUN_RESERVE      = 65536;

        struct Setting
        {
            std::string name;
            LLSD        value;
        };

        struct Spec
        {
            std::string              path;
            std::string              label;
            std::string              scene;          // a test scene's name or file; empty: none
            int                      laps   = 3;
            int                      warmup = 1;
            double                   speed  = 0.0;   // m/s; 0: the path's own times
            double                   turn   = DEFAULT_TURN;
            int                      round  = 1;
            std::vector<Setting>     settings;
            std::vector<std::string> toggles;
        };

        struct Stats
        {
            size_t frames = 0;
            double avg = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0;
            size_t over50 = 0;
        };

        std::string sCueDir;
        std::string sRunCue;
        std::string sStopCue;
        F64         sNextPoll = 0.0;

        std::vector<Spec> sQueue;
        size_t            sNext = 0;
        F64               sResumeAt = 0.0;     // the next run starts at this time (frame timer seconds) at the earliest
        F64               sToggleDeadline = 0.0;
        std::vector<std::string> sPendingToggles;   // files the last run made, to be taken before the next starts

        bool               sActive = false;
        Spec               sRun;
        int                sRunNumber = 0;     // counts every run since the viewer started
        double             sStart = 0.0;
        double             sLapSeconds = 0.0;
        int                sLap = 0;
        U64                sRegion = 0;
        std::vector<float> sLapFrames;
        std::vector<float> sMeasured;
        std::vector<Setting> sSaved;           // the values the run's settings had before it
        std::string        sBenchScene;        // the test scene the runs built (cleared with the queue)

        std::string clean(std::string text)
        {
            for (char& c : text)
            {
                if (c == ' ' || c == '\t' || c == '=' || c == ',' || (unsigned char)c < 32)
                {
                    c = '_';
                }
            }
            return text.empty() ? std::string("-") : text;
        }

        bool toggleName(const std::string& name)
        {
            return !name.empty() && name.size() <= 64
                   && std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; });
        }

        std::string head()
        {
            return llformat("run=%d label=%s", sRunNumber, sRun.label.c_str());
        }

        void mark(const std::string& text)
        {
            LL_INFOS("PerfMark") << text << LL_ENDL;
        }

        // 'frames' sorted.
        Stats statsOf(const std::vector<float>& frames)
        {
            Stats s;
            s.frames = frames.size();
            if (frames.empty())
            {
                return s;
            }
            double sum = 0.0;
            for (float ms : frames)
            {
                sum += ms;
                s.over50 += ms > HITCH_MS ? 1 : 0;
            }
            const auto rank = [&frames](double q)
            {
                const size_t n = frames.size();
                const size_t k = (size_t)std::ceil(q * (double)n);
                return (double)frames[std::min(n - 1, k > 0 ? k - 1 : 0)];
            };
            s.avg = sum / (double)frames.size();
            s.p50 = rank(0.50);
            s.p95 = rank(0.95);
            s.p99 = rank(0.99);
            s.max = frames.back();
            return s;
        }

        std::string statsText(const Stats& s)
        {
            return llformat("frames=%u avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f over50=%u", (unsigned)s.frames, s.avg, s.p50,
                            s.p95, s.p99, s.max, (unsigned)s.over50);
        }

        // 'frames' sorted: "bin:count,..." of the bins that have frames.
        std::string histogram(const std::vector<float>& frames)
        {
            std::ostringstream out;
            int    bin = -1;
            size_t count = 0;
            const auto flush = [&]()
            {
                if (count)
                {
                    out << (out.tellp() > 0 ? "," : "") << bin << ":" << count;
                }
            };
            for (float ms : frames)
            {
                const int b = std::min(MAX_BIN, std::max(0, (int)(ms * BINS_PER_MS)));
                if (b != bin)
                {
                    flush();
                    bin = b;
                    count = 0;
                }
                ++count;
            }
            flush();
            return out.str();
        }

        std::string toggleFile(const std::string& name)
        {
            return gDirUtilp->add(sCueDir, name + ".toggle");
        }

        void writeToggle(const std::string& name)
        {
            const std::string file = toggleFile(name);
            if (LLFILE* f = LLFile::fopen(file, LLFILE_MODE("wb")))
            {
                LLFile::close(f);
                sPendingToggles.push_back(file);
            }
            else
            {
                LL_WARNS("PerfMark") << "cannot make " << file << LL_ENDL;
            }
        }

        // The run's toggles back: a file the viewer has not taken yet is removed (it never flipped), else made again.
        void undoToggles()
        {
            sPendingToggles.clear();
            for (const std::string& name : sRun.toggles)
            {
                const std::string file = toggleFile(name);
                if (LLFile::isfile(file))
                {
                    LLFile::remove(file);
                }
                else
                {
                    writeToggle(name);
                }
            }
        }

        // --- settings ------------------------------------------------------------

        bool settingValue(LLControlVariable* control, const std::string& text, LLSD& value)
        {
            const char* begin = text.c_str();
            char*       end   = nullptr;
            switch (control->type())
            {
            case TYPE_BOOLEAN:
            {
                std::string lower = text;
                LLStringUtil::toLower(lower);
                if (lower == "1" || lower == "true" || lower == "on")
                {
                    value = LLSD(true);
                    return true;
                }
                if (lower == "0" || lower == "false" || lower == "off")
                {
                    value = LLSD(false);
                    return true;
                }
                return false;
            }
            case TYPE_S32:
            case TYPE_U32:
            {
                const long long number = std::strtoll(begin, &end, 10);
                if (end == begin || *end || number < (control->type() == TYPE_U32 ? 0 : INT_MIN) || number > (long long)UINT_MAX
                    || (control->type() == TYPE_S32 && number > INT_MAX))
                {
                    return false;
                }
                value = control->type() == TYPE_U32 ? LLSD((LLSD::Integer)(U32)number) : LLSD((LLSD::Integer)number);
                return true;
            }
            case TYPE_F32:
            {
                const double number = std::strtod(begin, &end);
                if (end == begin || *end || !std::isfinite(number))
                {
                    return false;
                }
                value = LLSD(number);
                return true;
            }
            default:
                return false;
            }
        }

        // Every setting of the run as it asks, the old values kept (not saved: setValue(..., false) leaves the saved value
        // as it was, so a viewer closed in the middle of a run keeps the user's).
        void applySettings()
        {
            sSaved.clear();
            for (const Setting& setting : sRun.settings)
            {
                if (LLControlVariable* control = gSavedSettings.getControl(setting.name))
                {
                    sSaved.push_back({ setting.name, control->getValue() });
                    control->setValue(setting.value, false);
                }
            }
        }

        void restoreSettings()
        {
            for (auto it = sSaved.rbegin(); it != sSaved.rend(); ++it)
            {
                if (LLControlVariable* control = gSavedSettings.getControl(it->name))
                {
                    // The saved value: put back as it was saved (no value of this run left over the saved one).
                    const bool was_saved = llsd_equals(it->value, control->getSaveValue());
                    control->setValue(it->value, was_saved);
                }
            }
            sSaved.clear();
        }

        // --- the cue file ----------------------------------------------------------

        bool readText(const std::string& file, std::string& text)
        {
            const S64 bytes = LLFile::size(file);
            if (bytes < 0 || bytes > MAX_CUE_BYTES)
            {
                return false;
            }
            llifstream stream(file.c_str(), std::ios::binary);
            if (!stream.is_open())
            {
                return false;
            }
            std::ostringstream all;
            all << stream.rdbuf();
            text = all.str();
            return true;
        }

        bool numberIn(const std::string& text, double low, double high, double& out)
        {
            const char* begin = text.c_str();
            char*       end   = nullptr;
            const double number = std::strtod(begin, &end);
            if (end == begin || *end || !std::isfinite(number) || number < low || number > high)
            {
                return false;
            }
            out = number;
            return true;
        }

        // The runs of the file into 'runs'; false with a reason in 'error'.
        bool parse(const std::string& text, std::vector<Spec>& runs, std::string& error)
        {
            runs.clear();
            Spec spec;
            int  rounds = 1;
            int  line_number = 0;
            std::istringstream lines(text);
            std::string line;
            const auto finishSpec = [&]()
            {
                if (spec.path.empty())
                {
                    error = "no path";
                    return false;
                }
                if (spec.label.empty())
                {
                    spec.label = llformat("run%u", (unsigned)runs.size() + 1);
                }
                runs.push_back(spec);
                // What carries on into the next run.
                spec.label.clear();
                spec.settings.clear();
                spec.toggles.clear();
                return true;
            };
            while (std::getline(lines, line))
            {
                ++line_number;
                const size_t comment = line.find('#');
                if (comment != std::string::npos)
                {
                    line.erase(comment);
                }
                LLStringUtil::trim(line);
                if (line_number == 1 && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                {
                    line.erase(0, 3);   // a byte order mark (Notepad, PowerShell's Out-File)
                }
                if (line.empty())
                {
                    continue;
                }
                if (line == "---")
                {
                    if (!finishSpec())
                    {
                        return false;
                    }
                    continue;
                }
                const size_t equals = line.find('=');
                if (equals == std::string::npos)
                {
                    error = llformat("line %d: no '='", line_number);
                    return false;
                }
                std::string key   = line.substr(0, equals);
                std::string value = line.substr(equals + 1);
                LLStringUtil::trim(key);
                LLStringUtil::trim(value);
                double number = 0.0;
                if (key.size() > 4 && key.compare(0, 4, "set ") == 0)
                {
                    std::string name = key.substr(4);
                    LLStringUtil::trim(name);
                    LLControlVariable* control = gSavedSettings.getControl(name);
                    Setting setting{ name, LLSD() };
                    if (!control || !settingValue(control, value, setting.value))
                    {
                        error = llformat("line %d: setting %s %s", line_number, name.c_str(),
                                         control ? "takes no such value (bool, S32, U32 or F32 only)" : "does not exist");
                        return false;
                    }
                    spec.settings.push_back(setting);
                }
                else if (key == "path")
                {
                    spec.path = value;
                }
                else if (key == "label")
                {
                    spec.label = clean(value);
                }
                else if (key == "laps" && numberIn(value, 1, MAX_LAPS, number))
                {
                    spec.laps = (int)number;
                }
                else if (key == "warmup" && numberIn(value, 0, MAX_WARMUP, number))
                {
                    spec.warmup = (int)number;
                }
                else if (key == "rounds" && numberIn(value, 1, MAX_ROUNDS, number))
                {
                    rounds = (int)number;
                }
                else if (key == "turn" && numberIn(value, MIN_TURN, MAX_TURN, number))
                {
                    spec.turn = number;
                }
                else if (key == "speed")
                {
                    if (value == "path")
                    {
                        spec.speed = 0.0;
                    }
                    else if (value == "walk")
                    {
                        spec.speed = WALK_SPEED;
                    }
                    else if (value == "run")
                    {
                        spec.speed = RUN_SPEED;
                    }
                    else if (value == "fly")
                    {
                        spec.speed = FLY_SPEED;
                    }
                    else if (numberIn(value, MIN_SPEED, MAX_SPEED, number))
                    {
                        spec.speed = number;
                    }
                    else
                    {
                        error = llformat("line %d: speed %s", line_number, value.c_str());
                        return false;
                    }
                }
                else if (key == "toggle" && toggleName(value))
                {
                    spec.toggles.push_back(value);
                }
                else if (key == "scene" && !value.empty() && value.size() <= 1024)
                {
                    spec.scene = value;
                }
                else
                {
                    error = llformat("line %d: %s = %s", line_number, key.c_str(), value.c_str());
                    return false;
                }
            }
            if (!finishSpec())
            {
                return false;
            }
            const size_t one_round = runs.size();
            if (one_round * (size_t)rounds > MAX_RUNS)
            {
                error = llformat("%u runs, at most %u", (unsigned)(one_round * rounds), (unsigned)MAX_RUNS);
                return false;
            }
            for (int r = 2; r <= rounds; ++r)
            {
                for (size_t i = 0; i < one_round; ++i)
                {
                    runs.push_back(runs[i]);
                    runs.back().round = r;
                }
            }
            return true;
        }

        // --- the path at a speed ---------------------------------------------------

        double angleDegrees(const Point& a, const Point& b)
        {
            LLVector3d da = a.focus - a.position;
            LLVector3d db = b.focus - b.position;
            if (da.normVec() <= 0.0 || db.normVec() <= 0.0)
            {
                return 0.0;
            }
            return std::acos(std::clamp(da * db, -1.0, 1.0)) * DEGREES;
        }

        // Every move's time from its length at 'speed' m/s and its turn at 'turn' degrees a second (the longer of the two);
        // cuts and holds as they are.
        void retime(Path& path, double speed, double turn)
        {
            for (size_t i = 1; i < path.points.size(); ++i)
            {
                Point&       point = path.points[i];
                const Point& before = path.points[i - 1];
                if (point.seconds <= 0.0)
                {
                    continue;   // a cut
                }
                double metres  = (point.position - before.position).length();
                double degrees = angleDegrees(before, point) + std::fabs(point.spin) * 360.0;
                if (point.motion == Motion::Orbit)
                {
                    metres += std::fabs(point.orbit) * TWO_PI * (point.position - point.focus).length();
                    degrees += std::fabs(point.orbit) * 360.0;
                }
                point.seconds = std::clamp(std::max(metres / speed, degrees / turn), MIN_MOVE_SECONDS, MAX_MOVE_SECONDS);
            }
        }

        // --- runs ------------------------------------------------------------------

        bool openPath(const std::string& name, std::string& error)
        {
            if (LLFile::isfile(name))
            {
                return loadFile(name, error);
            }
            const std::string in_cue = gDirUtilp->add(sCueDir, name);
            if (LLFile::isfile(in_cue))
            {
                return loadFile(in_cue, error);
            }
            return load(name, error);
        }

        void clearQueue()
        {
            sQueue.clear();
            sNext = 0;
            if (!sBenchScene.empty())
            {
                plus::testscene::clear("camerapath runs ended");
                sBenchScene.clear();
            }
        }

        // The run's test scene, built unless the last run built the same one and it is still there.
        bool ensureScene(std::string& error)
        {
            if (sRun.scene.empty() || (sRun.scene == sBenchScene && plus::testscene::isBuilt()))
            {
                return true;
            }
            sBenchScene.clear();
            if (!plus::testscene::build(sRun.scene, LLSD(), error))
            {
                return false;
            }
            sBenchScene = sRun.scene;
            return true;
        }

        // The end of a run, whichever way: settings and toggles back.
        void endRun()
        {
            sActive = false;
            restoreSettings();
            undoToggles();
            sToggleDeadline = LLFrameTimer::getElapsedSeconds() + TOGGLE_WAIT;
            sResumeAt = LLFrameTimer::getElapsedSeconds() + GAP_SECONDS;
            if (playing())
            {
                stop();
            }
        }

        void abortRun(const std::string& reason)
        {
            mark(llformat("abort %s reason=%s", head().c_str(), clean(reason).c_str()));
            endRun();
            if (sNext < sQueue.size())
            {
                LL_INFOS("PerfMark") << "the remaining " << (sQueue.size() - sNext) << " runs are dropped" << LL_ENDL;
            }
            clearQueue();
        }

        void startRun(const Spec& spec)
        {
            sRun = spec;   // 'spec' may be in sQueue, which clearQueue() empties: only sRun from here on
            ++sRunNumber;
            std::string error;
            if (!openPath(sRun.path, error))
            {
                mark(llformat("abort %s reason=path_%s(%s)", head().c_str(), clean(error).c_str(), clean(sRun.path).c_str()));
                clearQueue();
                return;
            }
            Path& open = path();
            open.loop      = true;
            open.countdown = 0;
            if (sRun.speed > 0.0)
            {
                retime(open, sRun.speed, sRun.turn);
            }
            changed();
            sLapSeconds = totalSeconds();
            if (open.points.size() < 2 || sLapSeconds <= 0.0)
            {
                mark(llformat("abort %s reason=path_too_short", head().c_str()));
                clearQueue();
                return;
            }
            applySettings();
            if (!ensureScene(error))
            {
                mark(llformat("abort %s reason=scene_%s", head().c_str(), clean(error).c_str()));
                restoreSettings();
                clearQueue();
                return;
            }
            sPendingToggles.clear();
            for (const std::string& name : sRun.toggles)
            {
                writeToggle(name);
            }
            if (!play(error))
            {
                mark(llformat("abort %s reason=play_%s", head().c_str(), clean(error).c_str()));
                restoreSettings();
                undoToggles();
                clearQueue();
                return;
            }
            sActive = true;
            sStart  = LLTimer::getTotalSeconds();
            sLap    = 0;
            const LLViewerRegion* region = gAgent.getRegion();
            sRegion = region ? region->getHandle() : 0;
            sLapFrames.clear();
            sMeasured.clear();
            if (sLapFrames.capacity() < LAP_RESERVE)
            {
                sLapFrames.reserve(LAP_RESERVE);
            }
            if (sMeasured.capacity() < RUN_RESERVE)
            {
                sMeasured.reserve(RUN_RESERVE);
            }

            std::string settings;
            for (const Setting& setting : sRun.settings)
            {
                settings += (settings.empty() ? "" : ";") + setting.name + ":" + setting.value.asString();
            }
            std::string toggles;
            for (const std::string& name : sRun.toggles)
            {
                toggles += (toggles.empty() ? "" : ";") + name;
            }
            // Not llformat: its buffer is 1 KB, and the names and settings may be long.
            std::ostringstream begin;
            begin << "begin " << head() << " round=" << sRun.round << " path=" << clean(open.name) << " laps=" << sRun.laps
                  << " warmup=" << sRun.warmup << " speed=" << (sRun.speed > 0.0 ? llformat("%.2f", sRun.speed) : std::string("path"))
                  << " turn=" << llformat("%.0f", sRun.turn) << " lap_s=" << llformat("%.2f", sLapSeconds)
                  << " points=" << open.points.size() << " region=" << clean(region ? region->getName() : std::string())
                  << " set=" << clean(settings) << " toggle=" << clean(toggles) << " scene=" << clean(plus::testscene::tag());
            mark(begin.str());
            if (sRun.warmup == 0)
            {
                mark("measure " + head());
            }
        }

        void finishRun()
        {
            std::sort(sMeasured.begin(), sMeasured.end());
            const Stats s = statsOf(sMeasured);
            double seconds = 0.0;
            for (float ms : sMeasured)
            {
                seconds += ms * 0.001;
            }
            mark(llformat("end %s %s fps=%.2f hist=", head().c_str(), statsText(s).c_str(),
                          seconds > 0.0 ? (double)s.frames / seconds : 0.0) + histogram(sMeasured));
            endRun();
            if (sNext >= sQueue.size())
            {
                mark(llformat("done runs=%u", (unsigned)sQueue.size()));
                clearQueue();
            }
        }

        void endLap()
        {
            const bool warm = sLap < sRun.warmup;
            std::sort(sLapFrames.begin(), sLapFrames.end());
            mark(llformat("lap %s lap=%d warmup=%d %s", head().c_str(), sLap + 1, warm ? 1 : 0, statsText(statsOf(sLapFrames)).c_str()));
            if (!warm)
            {
                sMeasured.insert(sMeasured.end(), sLapFrames.begin(), sLapFrames.end());
            }
            else if (sLap + 1 == sRun.warmup)
            {
                mark("measure " + head());
            }
            sLapFrames.clear();
        }

        // Every frame of a run.
        void sampleFrame()
        {
            if (!playing())
            {
                abortRun("stopped");   // Esc, the window, the flycam, another camera tool
                return;
            }
            const LLViewerRegion* region = gAgent.getRegion();
            if (!region || region->getHandle() != sRegion)
            {
                abortRun("region_changed");
                return;
            }
            sLapFrames.push_back(LLFrameTimer::getFrameDeltaTimeF32() * 1000.f);
            const int lap = (int)((LLTimer::getTotalSeconds() - sStart) / sLapSeconds);
            if (lap != sLap)
            {
                endLap();
                sLap = lap;
                if (sLap >= sRun.warmup + sRun.laps)
                {
                    finishRun();
                }
            }
        }

        bool togglesTaken()
        {
            for (const std::string& file : sPendingToggles)
            {
                if (LLFile::isfile(file))
                {
                    return false;
                }
            }
            return true;
        }

        void startNextWhenReady()
        {
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now < sResumeAt)
            {
                return;
            }
            if (!togglesTaken())
            {
                if (now < sToggleDeadline)
                {
                    return;
                }
                LL_WARNS("PerfMark") << "a toggle was not taken in " << TOGGLE_WAIT << " s (a name the viewer does not know?)" << LL_ENDL;
            }
            sPendingToggles.clear();
            startRun(sQueue[sNext++]);
        }

        // Once a second: the cue files.
        void pollCues()
        {
            if (LLFile::isfile(sStopCue))
            {
                LLFile::remove(sStopCue);
                if (sActive)
                {
                    abortRun("stop_cue");
                }
                else if (!sQueue.empty())
                {
                    mark(llformat("abort run=%d label=- reason=stop_cue", sRunNumber));
                    clearQueue();
                }
            }
            if (!LLFile::isfile(sRunCue))
            {
                return;
            }
            std::string text;
            const bool read = readText(sRunCue, text);
            LLFile::remove(sRunCue);
            if (sActive || !sQueue.empty())
            {
                LL_WARNS("PerfMark") << RUN_CUE << " ignored: runs are going (camerapath.stop ends them)" << LL_ENDL;
                return;
            }
            std::vector<Spec> runs;
            std::string error;
            if (!read)
            {
                error = "cannot read the file";
            }
            if (!read || !parse(text, runs, error))
            {
                LL_WARNS("PerfMark") << RUN_CUE << ": " << error << "; nothing runs" << LL_ENDL;
                return;
            }
            sQueue = std::move(runs);
            sNext = 0;
            sResumeAt = 0.0;
            sPendingToggles.clear();
            LL_INFOS("PerfMark") << RUN_CUE << ": " << sQueue.size() << " runs" << LL_ENDL;
        }

        void onFrame(void*)
        {
            if (sActive)
            {
                sampleFrame();
            }
            else if (sNext < sQueue.size())
            {
                startNextWhenReady();
            }
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now >= sNextPoll)
            {
                sNextPoll = now + POLL_SECONDS;
                pollCues();
            }
        }
    }

    void attachBench()
    {
        static bool attached = false;
        if (attached)
        {
            return;
        }
        attached = true;
        sCueDir = LLRHI::cueDir();     // UTF-8, as LLFile takes it
        if (sCueDir.empty())
        {
            return;
        }
        sRunCue  = gDirUtilp->add(sCueDir, RUN_CUE);
        sStopCue = gDirUtilp->add(sCueDir, STOP_CUE);
        gIdleCallbacks.addFunction(&onFrame, nullptr);
        LL_INFOS("PerfMark") << "measuring runs: waiting for " << sRunCue << LL_ENDL;
    }
}
