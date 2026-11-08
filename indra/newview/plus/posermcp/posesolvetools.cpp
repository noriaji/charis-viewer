/**
 * @file posesolvetools.cpp
 * @brief The poser MCP tools solve_pose and nudge. See posesolvetools.h.
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

// Threads: the handlers parse their arguments on the connection thread (no viewer
// state), the job steps run on the main thread (MainThreadRunner), the solver runs on
// a worker thread that only sees a Task (a copy of the skeleton, the work function and
// its result). The job owns the Task through a shared_ptr and the worker keeps its own
// reference, so an abandoned job (timeout, server stop) only raises the cancel flag;
// the worker stops within one LM iteration and frees the Task itself.

#include "llviewerprecompiledheaders.h"

#include "posesolvetools.h"

#include "posehistorytools.h"
#include "posenudge.h"
#include "posesolverjson.h"
#include "posesolvertables.h"

#include "llevents.h"
#include "llsd.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace json = boost::json;
using plus::mcp::Tool;
using plus::mcp::ToolResult;

namespace plus::posermcp
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr double SETTLE_SECONDS  = 4.0;  // as waitForSettle: a higher-priority animation can hold a joint
        constexpr double CORRECT_MM      = 10.0; // drawn joints further than this from the prediction: solve again
        constexpr int    MAX_CORRECT     = 3;
        constexpr double SOLVE_TIMEOUT   = 60.0;
        constexpr double NUDGE_TIMEOUT   = 30.0;
        constexpr double CHANGED_DEG     = 0.005; // a joint whose rotation changes less is left alone
        constexpr F32    MAX_PELVIS_SHIFT = 1.5f; // the poser window's position range

        struct ArgError : public std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        double rounded(double v, int decimals)
        {
            const double f = std::pow(10.0, decimals);
            const double r = std::round(v * f) / f;
            return r == 0.0 ? 0.0 : r;
        }

        bool toNumber(const json::value& v, double& out)
        {
            if (v.is_double())
                out = v.get_double();
            else if (v.is_int64())
                out = (double)v.get_int64();
            else if (v.is_uint64())
                out = (double)v.get_uint64();
            else
                return false;
            // Finite and within F32: the solver works in F32, where 1e39 would become inf.
            return std::isfinite(out) && std::fabs(out) <= std::numeric_limits<F32>::max();
        }

        std::optional<double> argNumber(const json::object& args, const char* key)
        {
            const json::value* v = args.if_contains(key);
            if (!v || v->is_null())
                return std::nullopt;
            double d = 0.0;
            if (!toNumber(*v, d))
                throw ArgError(std::string("'") + key + "' must be a finite number");
            return d;
        }

        bool argBool(const json::object& args, const char* key, bool fallback)
        {
            const json::value* v = args.if_contains(key);
            if (!v || v->is_null())
                return fallback;
            if (!v->is_bool())
                throw ArgError(std::string("'") + key + "' must be true or false");
            return v->get_bool();
        }

        std::string argString(const json::object& args, const char* key, const std::string& fallback)
        {
            const json::value* v = args.if_contains(key);
            if (!v || v->is_null())
                return fallback;
            if (!v->is_string())
                throw ArgError(std::string("'") + key + "' must be a string");
            return std::string(v->get_string());
        }

        // ------------------------------------------------------------ the job

        // What a solve hands back from the worker thread.
        struct Outcome
        {
            std::vector<std::pair<std::string, LLQuaternion>> rotations; // whole local rotations to set
            std::optional<LLVector3>                          pelvis;    // the new mPelvis position offset
            std::vector<std::pair<std::string, solver::Vec3>> predicted; // body frame, compared after the redraw
            json::object                                      report;
            bool                                              apply       = true;
            bool                                              correctable = false; // may be measured and solved again
        };

        // round 0: the first solve; round n: again from the skeleton drawn after applying round n-1.
        using Work = std::function<Outcome(const solver::Skeleton&, const std::atomic<bool>& cancel, int round)>;

        // Shared with the worker thread: plain data only.
        struct Task
        {
            solver::Skeleton  skeleton;
            int               round = 0;
            Work              work;
            std::atomic<bool> done{ false };
            std::atomic<bool> cancel{ false };
            Outcome           outcome;
            std::string       error;
            double            seconds = 0.0;
        };

        void runTask(const std::shared_ptr<Task>& task)
        {
            const auto t0 = Clock::now();
            try
            {
                task->outcome = task->work(task->skeleton, task->cancel, task->round);
            }
            catch (const solver::SolveCancelled&)
            {
                task->error = "cancelled";
            }
            catch (const std::exception& e)
            {
                task->error = e.what();
            }
            catch (...)
            {
                task->error = "the solver failed";
            }
            task->seconds = std::chrono::duration<double>(Clock::now() - t0).count();
            task->done.store(true, std::memory_order_release);
        }

        double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

        struct SolveJob
        {
            json::object             args;
            std::vector<std::string> wanted;
            Work                     work;
            int                      maxCorrect = 0;

            enum class Phase
            {
                Settle,  // waiting for the pose to settle, then read the skeleton
                Solving, // the worker thread solves
                Redraw   // applied: waiting for the skeleton to be drawn, then measure
            };
            Phase                 phase      = Phase::Settle;
            unsigned              frames     = 0;
            Clock::time_point     phaseStart = Clock::now();
            std::shared_ptr<Task> task;
            std::uint64_t         fingerprint      = 0;
            bool                  settledAtCapture = true;
            int                   round            = 0;
            int                   restarts         = 0;
            double                seconds          = 0.0;
            json::array           verify;
            Outcome               last;
            bool                  applied = false;

            ~SolveJob()
            {
                if (task)
                    task->cancel.store(true);
            }

            void enter(Phase p)
            {
                phase      = p;
                frames     = 0;
                phaseStart = Clock::now();
            }
        };

        // fix-threads4: the solver threads (detached) still running. At the app's
        // "quitting" their tasks are cancelled and waited for, before exit()
        // destroys what they read (posesolver.cpp's function-local tables). Made
        // once and never destroyed: a thread that outlives the wait still finds it.
        struct Running
        {
            std::mutex                         mutex;
            std::condition_variable            done;
            std::vector<std::shared_ptr<Task>> tasks;
            bool                               quitting = false;
        };

        Running& running()
        {
            static Running* const r = new Running();
            return *r;
        }

        constexpr double QUIT_WAIT_SECONDS = 5.0; // a cancelled solve stops within one LM iteration

        // The LLApp event pump's "quitting" (main thread, before cleanup()).
        void onQuitting()
        {
            Running& r = running();
            std::unique_lock<std::mutex> lock(r.mutex);
            r.quitting = true;
            for (const std::shared_ptr<Task>& task : r.tasks)
                task->cancel.store(true);
            if (!r.done.wait_for(lock, std::chrono::duration<double>(QUIT_WAIT_SECONDS), [&r] { return r.tasks.empty(); }))
            {
                LL_WARNS("PlusPoserMcp") << r.tasks.size() << " pose solve(s) still running at exit" << LL_ENDL;
            }
        }

        // Main thread (launch): once.
        void listenForQuitting()
        {
            static bool listening = false;
            if (listening)
                return;
            listening = true;
            LLEventPumps::instance().obtain("LLApp").listen("PlusPoseSolve", [](const LLSD& event)
            {
                if (event["status"].asString() == "quitting")
                    onQuitting();
                return false;
            });
        }

        // runTask on a solver thread, counted in running() while it runs.
        void runCounted(const std::shared_ptr<Task>& task)
        {
            runTask(task);
            Running& r = running();
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                r.tasks.erase(std::remove(r.tasks.begin(), r.tasks.end(), task), r.tasks.end());
            }
            r.done.notify_all();
        }

        void launch(SolveJob& job, solver::Skeleton skeleton)
        {
            auto task      = std::make_shared<Task>();
            task->skeleton = std::move(skeleton);
            task->round    = job.round;
            task->work     = job.work;
            job.task       = task;
            job.enter(SolveJob::Phase::Solving);
            listenForQuitting();
            Running& r = running();
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                if (r.quitting)
                {
                    task->error = "cancelled";  // the app is quitting: no new solve
                    task->done.store(true, std::memory_order_release);
                    return;
                }
                r.tasks.push_back(task);
            }
            try
            {
                std::thread([task] { runCounted(task); }).detach();
            }
            catch (...)
            {
                runCounted(task); // no thread to be had (system_error, bad_alloc): solve here (this frame takes longer)
            }
        }

        void captureAndLaunch(SolveJob& job)
        {
            SolveCapture cap;
            captureSolveSkeleton(job.args, job.wanted, cap);
            job.fingerprint      = cap.fingerprint;
            job.settledAtCapture = cap.settled;
            launch(job, std::move(cap.skeleton));
        }

        bool footBone(const std::string& name) { return name.rfind("mFoot", 0) == 0 || name.rfind("mToe", 0) == 0; }

        // The drawn joints against the prediction (the foot bones are left out: a mesh body
        // may override them, posemodel.py does the same).
        json::object verifyRound(const SolveJob& job, double& maxMm)
        {
            std::vector<std::string> names;
            for (const auto& [name, p] : job.last.predicted)
            {
                if (!footBone(name))
                    names.push_back(name);
            }
            const auto drawn = drawnPositions(job.args, names);
            std::vector<std::pair<double, std::string>> diffs;
            for (const auto& [name, d] : drawn)
            {
                for (const auto& [pname, p] : job.last.predicted)
                {
                    if (pname == name)
                    {
                        const solver::Vec3 dv = solver::fromLL(d) - p;
                        diffs.push_back({ solver::norm(dv) * 1000.0, name });
                    }
                }
            }
            std::sort(diffs.begin(), diffs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            double mean = 0.0;
            for (const auto& d : diffs)
                mean += d.first;
            mean /= std::max<size_t>(1, diffs.size());
            maxMm = diffs.empty() ? 0.0 : diffs.front().first;
            json::array worst;
            for (size_t i = 0; i < diffs.size() && i < 3; ++i)
                worst.push_back(json::array{ json::string(diffs[i].second), rounded(diffs[i].first, 1) });
            json::object v;
            v["round"]                   = job.round;
            v["drawn_vs_predicted_max_mm"] = rounded(maxMm, 1);
            v["mean_mm"]                 = rounded(mean, 1);
            v["joints"]                  = (int)diffs.size();
            v["worst"]                   = std::move(worst);
            return v;
        }

        ToolResult finish(SolveJob& job)
        {
            json::object result = job.last.report;
            result["applied"]   = job.applied;
            if (!job.verify.empty())
                result["verify"] = job.verify;
            result["rounds"]        = job.round + 1;
            result["solve_seconds"] = rounded(job.seconds, 3);
            if (!job.settledAtCapture)
                result["note"] = "Some joints had not reached their targets when the skeleton was read (another animation may "
                                 "hold them); check with joint_positions or snapshot views.";
            return ToolResult::json(result);
        }

        // One frame of a solve_pose / nudge call (main thread).
        std::optional<ToolResult> step(SolveJob& job)
        {
            switch (job.phase)
            {
                case SolveJob::Phase::Settle:
                {
                    if (++job.frames == 1)
                        job.phaseStart = Clock::now(); // the job may have waited in the queue
                    const bool settled = posesSettledNow();
                    if (job.frames < 2 || (!settled && since(job.phaseStart) < SETTLE_SECONDS))
                        return std::nullopt;
                    captureAndLaunch(job);
                    return std::nullopt;
                }
                case SolveJob::Phase::Solving:
                {
                    if (!job.task || !job.task->done.load(std::memory_order_acquire))
                        return std::nullopt;
                    std::shared_ptr<Task> task = std::move(job.task);
                    job.task.reset();
                    if (!task->error.empty())
                        throw std::runtime_error(task->error);
                    job.seconds += task->seconds;
                    job.last = std::move(task->outcome);
                    if (!job.last.apply)
                        return finish(job);
                    // the pose must still be the one that was solved
                    if (poseFingerprint(job.args) != job.fingerprint)
                    {
                        if (job.restarts++ < 1)
                        {
                            job.enter(SolveJob::Phase::Settle);
                            return std::nullopt;
                        }
                        throw std::runtime_error("The pose changed (in the poser window or by another call) while it was being "
                                                 "solved, twice; this call's last solve was not applied. Call it again.");
                    }
                    if (job.last.rotations.empty() && !job.last.pelvis)
                        return finish(job); // nothing to change
                    applySolvedPose(job.args, job.last.rotations, job.last.pelvis);
                    job.applied = true;
                    job.enter(SolveJob::Phase::Redraw);
                    return std::nullopt;
                }
                case SolveJob::Phase::Redraw:
                {
                    ++job.frames;
                    const bool settled = posesSettledNow();
                    if (job.frames < 2 || (!settled && since(job.phaseStart) < SETTLE_SECONDS))
                        return std::nullopt;
                    double       maxMm = 0.0;
                    json::object v     = verifyRound(job, maxMm);
                    v["settled"]       = settled;
                    job.verify.push_back(std::move(v));
                    if (job.last.correctable && maxMm > CORRECT_MM && job.round < job.maxCorrect)
                    {
                        // something else moves the joints, or the model is off: measure again, solve from there
                        ++job.round;
                        captureAndLaunch(job);
                        return std::nullopt;
                    }
                    return finish(job);
                }
            }
            return std::nullopt;
        }

        // The changed joints of a solve, as rotations to apply.
        void rotationsOf(const solver::Result& res, Outcome& out)
        {
            for (const solver::Result::Joint& j : res.joints)
            {
                if (j.changeDeg >= CHANGED_DEG)
                    out.rotations.push_back({ j.name, solver::toLLQuat(j.local) });
            }
            if (res.pelvisFree && solver::norm(res.pelvisMovedCm) > 0.001)
            {
                LLVector3 p = solver::toLL(res.pelvisPosition);
                p.clamp(-MAX_PELVIS_SHIFT, MAX_PELVIS_SHIFT);
                out.pelvis = p;
            }
            out.predicted = res.predicted;
        }

        // ------------------------------------------------------------ solve_pose

        struct SolveShared
        {
            solver::Spec                                      spec;
            std::optional<solver::Spec>                       frozen; // written by round 0's worker, read by later rounds
            std::vector<std::pair<std::string, LLQuaternion>> fingers;
            bool                                              dryRun = false;
        };

        ToolResult solvePose(const MainThreadRunner& run, const json::object& args)
        {
            auto shared = std::make_shared<SolveShared>();
            auto job    = std::make_shared<SolveJob>();
            try
            {
                shared->spec   = solver::parseSpec(args);
                shared->dryRun = argBool(args, "dry_run", false);
                const double correct = argNumber(args, "correct").value_or(1.0);
                if (correct < 0 || correct > MAX_CORRECT || correct != std::floor(correct))
                    throw ArgError("'correct' must be 0-" + std::to_string(MAX_CORRECT));
                job->maxCorrect = (int)correct;
                if (const json::value* f = args.if_contains("fingers"); f && !f->is_null())
                {
                    if (!f->is_object())
                        throw ArgError("'fingers' must be an object {joint: [x, y, z] degrees}");
                    for (const auto& kv : f->get_object())
                    {
                        const solver::Vec3 e = solver::parseVec3(kv.value(), "fingers " + std::string(kv.key()));
                        shared->fingers.push_back({ std::string(kv.key()), solver::toLLQuat(solver::eulerToRot(e)) });
                    }
                }
                if (shared->spec.goals.empty())
                    throw ArgError("'goals' must list at least one goal (see the description)");
            }
            catch (const std::exception& e)
            {
                return ToolResult::error(e.what());
            }
            job->args   = args;
            job->wanted = solver::neededJoints(shared->spec);
            for (const auto& f : shared->fingers)
                job->wanted.push_back(f.first);
            job->work = [shared](const solver::Skeleton& sk, const std::atomic<bool>& cancel, int round) -> Outcome
            {
                solver::Spec spec = shared->spec;
                if (round > 0 && shared->frozen)
                {
                    spec       = *shared->frozen; // the same targets for the anchors and the floor
                    spec.start = "current";
                }
                const solver::Result res = solver::solve(sk, spec, &cancel);
                if (round == 0)
                    shared->frozen = res.frozen;
                Outcome out;
                out.report = solver::resultToJson(res, false);
                rotationsOf(res, out);
                for (const auto& f : shared->fingers)
                    out.rotations.push_back(f);
                out.report["changed_joints"] = (int)out.rotations.size();
                out.apply                    = !shared->dryRun;
                out.correctable              = true;
                if (shared->dryRun)
                    out.report["next"] = "dry_run: nothing was applied. Call again without dry_run to apply.";
                else
                    out.report["next"] = "Check with snapshot views (front, a side, and top for contacts). Red warnings name goals "
                                         "that could not be met (the pose was applied anyway). Fine-tune with nudge.";
                return out;
            };
            return run(withHistory("solve_pose", args, [job](const JobProgress&) { return step(*job); }), SOLVE_TIMEOUT);
        }

        // ------------------------------------------------------------ nudge

        // The exact spelling of a joint nudge has words for (case-insensitive), or the input.
        std::string canonicalJoint(const std::string& name)
        {
            auto same = [&](const std::string& c)
            {
                if (c.size() != name.size())
                    return false;
                for (size_t i = 0; i < c.size(); ++i)
                {
                    if (std::tolower((unsigned char)c[i]) != std::tolower((unsigned char)name[i]))
                        return false;
                }
                return true;
            };
            for (const tables::FamilyRow& f : tables::FAMILIES)
            {
                const std::string fam = f.family;
                if (same(fam))
                    return fam;
                for (const char* side : { "Left", "Right" })
                {
                    if (same(fam + side))
                        return fam + side;
                    for (const char* seg : { "1", "2", "3" })
                    {
                        if (same(fam + seg + side))
                            return fam + seg + side;
                    }
                }
            }
            return name;
        }

        json::object limitJson(const nudge::Turn& t)
        {
            json::object l;
            l["motion"]   = t.limitMotion;
            l["value_at_asked"] = rounded(t.limitValue, 1);
            l["limit"]    = t.limit ? json::value(*t.limit) : json::value(nullptr);
            return l;
        }

        ToolResult nudgeTool(const MainThreadRunner& run, const json::object& args)
        {
            auto job = std::make_shared<SolveJob>();
            try
            {
                const bool        rotation    = args.if_contains("joint") != nullptr;
                const bool        move        = args.if_contains("at") != nullptr;
                const bool        limits      = argBool(args, "limits", true);
                const bool        dryRun      = argBool(args, "dry_run", false);
                const std::string flexibility = argString(args, "flexibility", "normal");
                const std::string keep        = argString(args, "keep", "none");
                if (flexibility != "normal" && flexibility != "athletic" && flexibility != "dancer")
                    throw ArgError("flexibility must be normal, athletic or dancer");
                if (keep != "none")
                    throw ArgError("keep: only \"none\" for now (pins come later)");
                if (rotation == move)
                    throw ArgError("give either joint + motion (+ deg) to turn a joint, or at + move (+ cm) to move a point");
                if (rotation)
                {
                    const std::string joint  = canonicalJoint(argString(args, "joint", ""));
                    const std::string motion = argString(args, "motion", "");
                    if (motion.empty())
                        throw ArgError("motion is required: " + nudge::wordsOf(joint));
                    for (const char* k : { "move", "cm" })
                    {
                        if (args.if_contains(k))
                            throw ArgError(std::string("'") + k + "' goes with at (moving a point), not with joint");
                    }
                    const double deg = argNumber(args, "deg").value_or(nudge::DEFAULT_DEG);
                    if (!(deg > 0.0) || deg > nudge::MAX_DEG)
                        throw ArgError("deg must be more than 0 and at most " + std::to_string((int)nudge::MAX_DEG) +
                                       " (use the opposite word to go the other way; for more, aim or solve_pose)");
                    const std::vector<std::string> joints = nudge::expandJoints(joint, motion);
                    solver::Spec                   s;
                    s.freeAuto = false;
                    s.free     = joints;
                    job->wanted = solver::neededJoints(s);
                    for (const std::string& j : joints)
                    {
                        const std::string end = solver::boneEnd(j);
                        if (!end.empty())
                            job->wanted.push_back(end);
                    }
                    job->work = [joints, motion, deg, limits, flexibility, dryRun](const solver::Skeleton& sk, const std::atomic<bool>&,
                                                                                  int) -> Outcome
                    {
                        solver::State st{ sk.local, sk.rootPos };
                        Outcome       out;
                        json::array   turned, warnings;
                        std::string   canonical;
                        for (const std::string& name : joints)
                        {
                            const int j = sk.find(name);
                            if (j < 0)
                            {
                                warnings.push_back(json::string(name + " is not in this avatar's skeleton: skipped"));
                                continue;
                            }
                            const nudge::Motion& m = nudge::findMotion(name, motion);
                            canonical              = m.name;
                            const nudge::Turn t    = nudge::turnJoint(sk, st, j, m, deg, limits, flexibility);
                            st.local[j]            = t.local;
                            json::object e;
                            e["joint"]     = sk.names[j];
                            e["moved_deg"] = rounded(t.movedDeg, 2);
                            e["rotation"]  = solver::vecJson(solver::eulerDegrees(t.local), 2);
                            if (t.reached)
                                e["reached"] = true;
                            if (t.limited)
                            {
                                e["limited_by"] = limitJson(t);
                                warnings.push_back(json::string(sk.names[j] + " stopped at its " + t.limitMotion + " limit: moved " +
                                                                std::to_string(ll_round((F32)t.movedDeg)) + " of " +
                                                                std::to_string(ll_round((F32)deg)) + " degrees"));
                            }
                            else if (t.reached)
                            {
                                warnings.push_back(json::string(sk.names[j] + " now points " + m.name + " and stops there: moved " +
                                                                std::to_string(ll_round((F32)t.movedDeg)) + " of " +
                                                                std::to_string(ll_round((F32)deg)) + " degrees"));
                            }
                            if (!t.note.empty())
                                e["note"] = t.note;
                            if (t.movedDeg > 1e-4)
                                out.rotations.push_back({ sk.names[j], solver::toLLQuat(t.local) });
                            turned.push_back(std::move(e));
                        }
                        std::vector<solver::Mat3> W;
                        std::vector<solver::Vec3> P;
                        solver::forward(sk, st, W, P);
                        for (const std::string& name : joints)
                        {
                            const int e = sk.find(solver::boneEnd(name));
                            if (e >= 0)
                                out.predicted.push_back({ sk.names[e], P[e] });
                        }
                        out.report["motion"]    = canonical;
                        out.report["asked_deg"] = deg;
                        out.report["joints"]    = std::move(turned);
                        if (out.rotations.empty())
                            warnings.push_back(json::string("nothing moved"));
                        out.report["warnings"] = std::move(warnings);
                        out.report["next"] = dryRun ? "dry_run: nothing was applied." : "Check with snapshot views; undo goes back.";
                        out.apply          = !dryRun;
                        return out;
                    };
                }
                else
                {
                    const solver::PointSpec at = solver::parsePoint(*args.if_contains("at"), "at");
                    if (!at.mid.empty())
                        throw ArgError("at must be one point (a joint, a site or an offset), not a mid");
                    for (const char* k : { "motion", "deg" })
                    {
                        if (args.if_contains(k))
                            throw ArgError(std::string("'") + k + "' goes with joint (turning a joint), not with at");
                    }
                    const json::value* mv = args.if_contains("move");
                    if (!mv || mv->is_null())
                        throw ArgError("move is required: forward, back, up, down, left, right, out, in or [x, y, z] (body frame)");
                    std::optional<solver::Vec3> vector;
                    std::string                 word;
                    if (mv->is_string())
                    {
                        word = std::string(mv->get_string());
                        solver::Vec3 probe;
                        if (!nudge::directionOf(word, "Left", probe))
                            throw ArgError("move must be forward, back, up, down, left, right, out, in or [x, y, z]; not '" + word + "'");
                    }
                    else
                    {
                        solver::Vec3 v = solver::parseVec3(*mv, "move");
                        if (solver::norm(v) < 1e-9)
                            throw ArgError("move must not be zero");
                        vector = solver::unit(v);
                    }
                    const double cm = argNumber(args, "cm").value_or(nudge::DEFAULT_CM);
                    if (!(cm > 0.0) || cm > nudge::MAX_CM)
                        throw ArgError("cm must be more than 0 and at most " + std::to_string((int)nudge::MAX_CM) +
                                       " (for more, reach, contact or solve_pose)");
                    solver::Spec s;
                    solver::Goal g;
                    g.type = solver::GoalType::Anchor;
                    g.at   = at;
                    s.goals.push_back(g);
                    job->wanted = solver::neededJoints(s);
                    job->work   = [at, word, vector, cm, flexibility, dryRun](const solver::Skeleton& sk, const std::atomic<bool>& cancel,
                                                                            int) -> Outcome
                    {
                        const auto  parts = solver::resolvePoint(sk, at);
                        std::string root, middle, limb;
                        if (!solver::limbOfPoint(sk, parts, root, middle, limb))
                            throw ArgError("move works on a point of a hand or foot (the wrist / ankle or below); for other points "
                                           "use solve_pose");
                        solver::Vec3 dir;
                        if (vector)
                            dir = *vector;
                        else
                            nudge::directionOf(word, limb.rfind("left", 0) == 0 ? "Left" : "Right", dir);
                        std::vector<solver::Mat3> W;
                        std::vector<solver::Vec3> P;
                        solver::forward(sk, solver::State{ sk.local, sk.rootPos }, W, P);
                        const solver::Vec3   from   = solver::pointPosition(W, P, parts);
                        const solver::Vec3   target = from + dir * (cm / 100.0);
                        const solver::Result res    = solver::solve(sk, nudge::moveSpec(at, target, root, middle, flexibility), &cancel);
                        solver::forward(sk, res.state, W, P);
                        const solver::Vec3 to = solver::pointPosition(W, P, parts);
                        Outcome            out;
                        rotationsOf(res, out);
                        out.pelvis.reset();
                        json::object joints;
                        for (const solver::Result::Joint& j : res.joints)
                            joints[j.name] = json::object{ { "rotation", solver::vecJson(solver::eulerDegrees(j.local), 2) }, { "change_deg", j.changeDeg } };
                        json::array warnings;
                        for (const std::string& w : res.warnings)
                            warnings.push_back(json::string(w));
                        const double off = solver::norm(to - target) * 1000.0;
                        if (res.status != "converged")
                            warnings.push_back(json::string("the point can not get all the way (the " + limb +
                                                            "'s reach or its joint ranges): " + std::to_string(ll_round((F32)off)) +
                                                            " mm short"));
                        out.report["point"]         = at.joint + (at.site.empty() ? "" : "." + at.site);
                        out.report["limb"]          = limb;
                        out.report["move"]          = vector ? json::value(solver::vecJson(*vector, 3)) : json::value(word);
                        out.report["direction"]     = solver::vecJson(dir, 3);
                        out.report["asked_cm"]      = cm;
                        out.report["moved_cm"]      = rounded(solver::norm(to - from) * 100.0, 2);
                        out.report["off_target_mm"] = rounded(off, 1);
                        out.report["from"]          = solver::vecJson(from, 4);
                        out.report["to"]            = solver::vecJson(to, 4);
                        out.report["status"]        = res.status;
                        out.report["joints"]        = std::move(joints);
                        out.report["warnings"]      = std::move(warnings);
                        out.report["next"]          = dryRun ? "dry_run: nothing was applied." : "Check with snapshot views; undo goes back.";
                        out.apply                   = !dryRun;
                        return out;
                    };
                }
            }
            catch (const std::exception& e)
            {
                return ToolResult::error(e.what());
            }
            job->args = args;
            return run(withHistory("nudge", args, [job](const JobProgress&) { return step(*job); }), NUDGE_TIMEOUT);
        }

        // ------------------------------------------------------------ documentation

        // The nudge words, from the table (the same source as the maths).
        std::string vocabulary()
        {
            std::string text;
            std::string pendingNames, pendingWords;
            auto flush = [&]()
            {
                if (!pendingNames.empty())
                    text += pendingNames + ": " + pendingWords + ". ";
                pendingNames.clear();
                pendingWords.clear();
            };
            for (const tables::FamilyRow& f : tables::FAMILIES)
            {
                std::string words;
                for (const tables::MotionRow& m : tables::MOTIONS)
                {
                    if (std::string(m.family) == f.family)
                        words += (words.empty() ? "" : ", ") + std::string(m.name) + (m.segment ? " (segment 1)" : "");
                }
                std::string aliases;
                for (const tables::AliasRow& a : tables::ALIASES)
                {
                    if (std::string(a.family) == f.family)
                        aliases += (aliases.empty() ? "" : ", ") + std::string(a.alias) + "=" + a.motion;
                }
                if (!aliases.empty())
                    words += " [also " + aliases + "]";
                const std::string name = std::string(f.family) + (f.sided ? "Left/Right" : "") + " (" + f.what + ")";
                if (words == pendingWords)
                {
                    pendingNames += ", " + name;
                    continue;
                }
                flush();
                pendingNames = name;
                pendingWords = words;
            }
            flush();
            return text;
        }
    }

    std::string solveInstructions()
    {
        return "SOLVE MANY GOALS AT ONCE WITH solve_pose: give what must be true (bone directions, hands and feet touching, "
               "feet flat on the floor, the knee bent 90-110, the trunk leaning 25-35 ...) and every body joint is solved "
               "together within human joint ranges, so fixing one thing does not break another. It applies the result "
               "(one undo step) and reports each goal's residual; red warnings name hard goals it could not meet. FINE-TUNE "
               "WITH nudge in words (\"left knee bend 10\", \"right wrist palm_side 5\", the left toes 3 cm out): only what "
               "was asked moves, stopped at the joint's range. TO COPY A PHOTO: photo_pose.py gives bone directions -> "
               "solve_pose with them as weak bone_dir goals (weight 0.3) plus the contacts you see (hard), the standing "
               "foot on_floor flat, ranges you can read from the photo (joint_range, torso_lean, bone_dir within_deg) -> "
               "snapshot views from the photo's side and from above -> add the goals the views show missing and solve again, "
               "or nudge the details -> undo if a step made it worse.\n";
    }

    void appendSolveTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        auto prop = [](const char* type, const std::string& description)
        { return json::object{ { "type", type }, { "description", description } }; };
        auto enumOf = [](std::initializer_list<const char*> values, const std::string& description)
        {
            json::array list;
            for (const char* v : values)
                list.push_back(json::string(v));
            return json::object{ { "type", "string" }, { "enum", std::move(list) }, { "description", description } };
        };
        const json::object avatar = prop("string", "\"self\" (default) for your own avatar, or the UUID of an animesh you own.");

        {
            Tool t;
            t.name        = "solve_pose";
            t.title       = "Solve a pose from goals";
            t.description =
                "Solves EVERY BODY JOINT AT ONCE from goals, within human joint ranges, and applies it (one undo step). Use it "
                "when several things must hold together (hands on feet AND a foot flat AND a lean ...); set_joints / aim / reach "
                "change one thing at a time and can undo another. Arguments = the goals file of tools/poser-mcp (goals.json): "
                "start (current = from this pose, default; tpose), goals [...], free (\"auto\" = the body; or joint names), lock "
                "(joint names, \"mPelvis.position\"), floor_z (rest = where the soles are in the T-pose, default; lowest; or a "
                "height in m), options {flexibility normal|athletic|dancer, starts (default 6), stay (how strongly joints stay "
                "as they are, default 1), stay_weight {joint or arm/leg/trunk/pelvis/head: w}, max_iter, comfort}, camera "
                "(for image_dir: {azimuth, elevation}), sites_override, fingers {joint: [x,y,z]} (set after the solve), "
                "dry_run, correct (re-measure and solve again when the drawn joints miss the prediction by more than 1 cm; "
                "default 1).\n"
                "GOALS (points are a joint name, {\"joint\", \"site\"?, \"offset\"? [m, the joint's axes]} or {\"mid\": [points]}; "
                "sites: palm, back_of_hand, fingertips, wrist, instep, sole, heel, ball, toe_tip, ankle, knee_front, elbow_tip, "
                "hip_side, seat, chin, forehead, top_of_head, nose, cheek_left/right): bone_dir {joint, direction [body frame], "
                "weight?, roll_toward?, within_deg? (hard: within this angle)}; contact {a, b, distance? m, tolerance? m (0.005), "
                "hard? (true), facing?}; on_floor {at, flat? (sole level), height? m (a seat)}; above_floor {at: [points]}; "
                "anchor {at, position? (default where it is now), orient? (keep its turn)}; joint_range {joint, motion, min, max} "
                "(motions: shoulder back/across/twist, elbow/knee flex, wrist flex/deviate/twist, hip flex/abduct/twist, ankle "
                "flex/invert/twist, trunk forward/side/twist, collar up/forward/twist, rotation); bone_elevation {joint, min, max} "
                "(degrees above horizontal); torso_lean {min, max (hard, degrees from vertical) | forward_deg, side_deg (weak)}; "
                "balance {support: [joints], weight?}; look {toward [body frame]}; image_dir {from, to | axis {joint, local}, "
                "photo_vec [dx, dy] (pixels in the photo, y down), camera?}; image_point {at, px [x, y], camera {azimuth, "
                "elevation, fov, distance, focus, size}}. Hard goals (contact, on_floor, anchor, ranges, within_deg, min/max) are "
                "met to their tolerance or reported unmet (the pose is applied anyway, with a RED warning); weak ones are traded "
                "off by weight (1 cm = 1 degree = 1).\n"
                "DEFAULT RANGES (human average; flexibility athletic x1.15, dancer x1.3 for hips, spine and shoulders): every "
                "joint turns at most 120 degrees, wrists and ankles twist at most 10, elbows and knees only bend (-5..150), hip "
                "flex -20..125, trunk forward -15..40 per joint. A standing (supporting) foot is right when it is FLAT on the "
                "floor: on_floor {at: {joint: mAnkle.., site: sole}, flat: true}. A forward-leaning upper body: torso_lean min 25 "
                "max 45 (when matching a photo, max at most 45); the spine's own ranges allow that.\n"
                "Returns status (converged | unmet), every goal's value and met, unmet, warnings, range_hits, joints (rotation, "
                "change_deg, angle_deg, twist_deg), starts, alternatives (clearly different poses of similar cost), verify (drawn "
                "vs predicted joints), history_id. Example: {\"goals\":[{\"type\":\"contact\",\"a\":{\"joint\":\"mWristLeft\","
                "\"site\":\"palm\"},\"b\":{\"joint\":\"mAnkleLeft\",\"site\":\"instep\"}},{\"type\":\"on_floor\",\"at\":{\"joint\":"
                "\"mAnkleRight\",\"site\":\"sole\"},\"flat\":true},{\"type\":\"torso_lean\",\"min\":25,\"max\":35}],\"options\":"
                "{\"flexibility\":\"dancer\"}}";
            json::object goal{ { "type", "object" },
                               { "properties",
                                 json::object{ { "type",
                                                 enumOf({ "bone_dir", "contact", "on_floor", "above_floor", "anchor", "joint_range",
                                                          "bone_elevation", "torso_lean", "balance", "look", "image_dir", "image_point" },
                                                        "Goal type (see the description for its fields).") } } },
                               { "required", json::array{ "type" } } };
            json::object p;
            p["avatar"] = avatar;
            p["goals"]  = json::object{ { "type", "array" }, { "items", std::move(goal) }, { "minItems", 1 }, { "maxItems", 200 } };
            p["start"]  = enumOf({ "current", "tpose" }, "Solve from this pose (default) or from the T-pose.");
            p["free"]   = json::object{ { "description", "\"auto\" (the body joints) or joint names to solve." } };
            p["lock"]   = json::object{ { "type", "array" }, { "items", json::object{ { "type", "string" } } },
                                      { "description", "Joints that do not move (\"mPelvis.position\": the pelvis stays in place)." } };
            p["lock_position"] = prop("boolean", "Keep the pelvis in place. Default false.");
            p["floor_z"] = json::object{ { "description", "\"rest\" (default), \"lowest\" or a height in m (body frame)." } };
            p["options"] = prop("object", "flexibility, starts, stay, stay_weight, max_iter, max_outer, comfort, seed.");
            p["camera"]  = prop("object", "The photo's camera for image_dir / image_point: {azimuth, elevation, roll?, fov?, distance?}.");
            p["sites_override"] = prop("object", "Per-avatar skin points {site: {Left|Right|*: {anchor?, frame?, offset?, normal?}}}.");
            p["fingers"] = prop("object", "Finger rotations set after the solve {joint: [x, y, z] degrees} (as set_joints).");
            p["dry_run"] = prop("boolean", "Solve and report without changing the pose. Default false.");
            p["correct"] = prop("integer", "Times to re-measure and solve again if the drawn joints miss by > 1 cm (0-3). Default 1.");
            p["note"]       = prop("string", "Free text (ignored).");
            p["bones_skip"] = json::object{ { "description", "Ignored (solve_pose_cli.py's option)." } };
            json::object schema;
            schema["type"]                 = "object";
            schema["properties"]           = std::move(p);
            schema["required"]             = json::array{ "goals" };
            schema["additionalProperties"] = false;
            t.inputSchema                  = std::move(schema);
            t.handler = [run](const json::object& args) { return solvePose(run, args); };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "nudge";
            t.title       = "Nudge a joint or a hand / foot";
            t.description =
                "Small changes IN WORDS, only what is asked moves (one undo step). TURN A JOINT: {\"joint\", \"motion\", \"deg\"} "
                "(deg default " + std::to_string((int)nudge::DEFAULT_DEG) + ", at most " + std::to_string((int)nudge::MAX_DEG) +
                "; use the opposite word for the other way). The words work in the joint's own sense whatever the pose: "
                "upper arms / thighs turn toward forward, back, up, down, out (away from the body's middle), in, of the chest "
                "/ pelvis and stop there; rotate_in / rotate_out twist them about their bone; elbows and knees bend / "
                "straighten about their hinge; hands, feet, collars, spine, neck, head and fingers turn about their own axes. "
                "Left and right are handled for you (the right side is the mirror image). A turn stops at the joint's human "
                "range (limits false to go past it; flexibility athletic / dancer widens it) and the result says so. Joints: "
                "a single joint, a whole finger (mHandIndexLeft = its 3 segments) or fingers_left / fingers_right (all but the "
                "thumbs). WORDS: " + vocabulary() +
                "MOVE A HAND OR FOOT POINT: {\"at\": joint or {\"joint\", \"site\"?, \"offset\"?}, \"move\": forward|back|up|down|"
                "left|right|out|in or [x,y,z] (body frame), \"cm\"} (cm default " + std::to_string((int)nudge::DEFAULT_CM) +
                ", at most " + std::to_string((int)nudge::MAX_CM) + "): only the shoulder + elbow / hip + knee move (the hand / "
                "foot keeps its angle to the forearm / shin); out of reach it gets as close as it can and says how far short. "
                "Sites as in solve_pose (palm, fingertips, sole, toe_tip, heel, instep ...). Returns what actually moved "
                "(moved_deg / moved_cm), limited_by, warnings, verify (drawn vs predicted), history_id.";
            json::object p;
            p["avatar"]      = avatar;
            p["joint"]       = prop("string", "Joint to turn (mKneeLeft, mWristRight, mHandIndexLeft, fingers_left ...).");
            p["motion"]      = prop("string", "A word for that joint (see the description).");
            p["deg"]         = prop("number", "Degrees, more than 0. Default 10, at most 45.");
            p["at"]          = json::object{ { "description", "Point to move: a joint name or {\"joint\", \"site\"?, \"offset\"?}." } };
            p["move"]        = json::object{ { "description", "forward, back, up, down, left, right, out, in, or [x, y, z] (body frame)." } };
            p["cm"]          = prop("number", "Centimetres, more than 0. Default 3, at most 20.");
            p["limits"]      = prop("boolean", "Stop at the joint's human range. Default true.");
            p["flexibility"] = enumOf({ "normal", "athletic", "dancer" }, "Range widths. Default normal.");
            p["keep"]        = enumOf({ "none" }, "Reserved for pins (later): what else is kept while nudging. Default none.");
            p["dry_run"]     = prop("boolean", "Work it out without changing the pose. Default false.");
            json::object schema;
            schema["type"]                 = "object";
            schema["properties"]           = std::move(p);
            schema["additionalProperties"] = false;
            t.inputSchema                  = std::move(schema);
            t.handler = [run](const json::object& args) { return nudgeTool(run, args); };
            tools.push_back(std::move(t));
        }
    }
}
