/**
 * @file motionclient.cpp
 * @brief "Motion Between Poses": the helper client and the machine H (see motionclient.h).
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

#include "llviewerprecompiledheaders.h"

#include "motionclient.h"

#include "plus/subtitle/lineclient.h"

#include "llerror.h"
#include "llstring.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace plus::posermotion
{
    namespace
    {
        constexpr char     LOAD_LINE[]      = "{\"cmd\":\"load\"}";
        constexpr S32      ANSWER_CONTACTS  = 4;
        constexpr uint64_t MAX_JOB_ID       = (1ull << 53) - 1;   // the helper takes ids 0..2^53-1
        // The most a request takes: MOTION_CONTEXT_FRAMES + MOTION_MAX_TARGETS frames of numbers at their longest.
        // Under the helper's limit on a request (MAX_REQUEST_BYTES in charis_motion.cpp, research/361 section 4).
        constexpr size_t   REQUEST_RESERVE  = 32 * 1024;
        constexpr double   MAX_HELPER_MS    = 1e7;                // an answer's compute_ms beyond this (about 3 hours) is not one

        // "%.9g": a float exactly, and never more than the helper's 40 characters for a number
        void appendNumber(std::string& out, F32 value)
        {
            char buf[32];
            const int n = std::snprintf(buf, sizeof(buf), "%.9g", value);
            out.append(buf, n > 0 ? size_t(n) : 0);
        }

        // One frame of the request: {"pos":[[x,y,z] x 30],"rot":[[w,x,y,z] x 30]} (the pose was checked)
        void appendFrame(std::string& out, const SomaPose& pose)
        {
            out += "{\"pos\":[";
            for (S32 j = 0; j < SOMA_JOINT_COUNT; ++j)
            {
                const LLVector3& p = pose.pos[j];
                out += j ? ",[" : "[";
                appendNumber(out, p.mV[VX]);
                out += ',';
                appendNumber(out, p.mV[VY]);
                out += ',';
                appendNumber(out, p.mV[VZ]);
                out += ']';
            }
            out += "],\"rot\":[";
            for (S32 j = 0; j < SOMA_JOINT_COUNT; ++j)
            {
                const LLQuaternion& q = pose.rot[j];   // LLQuaternion keeps x, y, z, w; the helper reads w, x, y, z
                out += j ? ",[" : "[";
                appendNumber(out, q.mQ[VW]);
                out += ',';
                appendNumber(out, q.mQ[VX]);
                out += ',';
                appendNumber(out, q.mQ[VY]);
                out += ',';
                appendNumber(out, q.mQ[VZ]);
                out += ']';
            }
            out += "]}";
        }

        // The pose with its rotations at unit length, false when a number is not finite, a position is beyond
        // MAX_POSITION_M or a rotation is degenerate (the helper would refuse it the same way)
        bool checkedPose(const SomaPose& in, SomaPose& out)
        {
            for (S32 j = 0; j < SOMA_JOINT_COUNT; ++j)
            {
                if (!validPosition(in.pos[j]) || !normalizedRotation(in.rot[j], out.rot[j]))
                {
                    return false;
                }
                out.pos[j] = in.pos[j];
            }
            return true;
        }

        bool validTokens(S32 tokens)
        {
            return tokens == MOTION_TOKENS_AUTO || motionFramesOfTokens(tokens) > 0;
        }

        // The request with its poses checked (rotations at unit length), false when tokens or ntargets (1..MOTION_MAX_TARGETS)
        // is not one the helper takes or a pose that goes out (the context, the first ntargets targets) does not pass checkedPose
        bool checkedRequest(const GenerateRequest& in, GenerateRequest& out)
        {
            if (!validTokens(in.tokens) || in.ntargets < 1 || in.ntargets > MOTION_MAX_TARGETS)
            {
                return false;
            }
            for (S32 i = 0; i < MOTION_CONTEXT_FRAMES; ++i)
            {
                if (!checkedPose(in.context[i], out.context[i]))
                {
                    return false;
                }
            }
            for (S32 i = 0; i < in.ntargets; ++i)
            {
                if (!checkedPose(in.targets[i], out.targets[i]))
                {
                    return false;
                }
            }
            out.ntargets = in.ntargets;
            out.tokens = in.tokens;
            return true;
        }

        // "frames": [frame, frame, ...] of the first n poses
        void appendFrames(std::string& out, const SomaPose* poses, S32 n)
        {
            out += '[';
            for (S32 i = 0; i < n; ++i)
            {
                if (i)
                {
                    out += ',';
                }
                appendFrame(out, poses[i]);
            }
            out += ']';
        }

        // The request after its id: "op", the context, the targets, the length (the request was checked)
        void appendRequest(std::string& out, const GenerateRequest& request)
        {
            out += "\"op\":\"inbetween\",\"context\":";
            appendFrames(out, request.context, MOTION_CONTEXT_FRAMES);
            out += ",\"targets\":";
            appendFrames(out, request.targets, request.ntargets);
            out += ",\"tokens\":" + std::to_string(request.tokens) + "}";
        }

        std::string stringOf(const boost::json::object& o, const char* key)
        {
            const boost::json::value* v = o.if_contains(key);
            return (v && v->is_string()) ? std::string(v->as_string().c_str()) : std::string();
        }

        // A JSON number as a finite double (an integer or not)
        bool finiteNumber(const boost::json::value& v, double& out)
        {
            if (v.is_double())
            {
                out = v.get_double();
            }
            else if (v.is_int64())
            {
                out = double(v.get_int64());
            }
            else if (v.is_uint64())
            {
                out = double(v.get_uint64());
            }
            else
            {
                return false;
            }
            return std::isfinite(out);
        }

        // A JSON array of exactly n finite numbers into out[0..n)
        bool numberRow(const boost::json::value* v, size_t n, double* out)
        {
            if (!v || !v->is_array() || v->as_array().size() != n)
            {
                return false;
            }
            const boost::json::array& a = v->as_array();
            for (size_t i = 0; i < n; ++i)
            {
                if (!finiteNumber(a[i], out[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // The "id" of a line: >= 0 the helper's id of a request, -1 "none" (the helper's id of a line it could not
        // tie to a request), -2 absent or not a whole number we sent
        constexpr int64_t ID_NONE = -1;
        constexpr int64_t ID_BAD  = -2;
        int64_t idOf(const boost::json::object& o)
        {
            const boost::json::value* v = o.if_contains("id");
            if (!v)
            {
                return ID_BAD;
            }
            if (v->is_int64())
            {
                const int64_t id = v->get_int64();
                return (id == ID_NONE || (id >= 0 && uint64_t(id) <= MAX_JOB_ID)) ? id : ID_BAD;
            }
            if (v->is_uint64())
            {
                const uint64_t id = v->get_uint64();
                return id <= MAX_JOB_ID ? int64_t(id) : ID_BAD;
            }
            return ID_BAD;
        }

        // A whole number in [lo, hi] (boost::json keeps every whole number that fits as int64; one beyond is out anyway)
        bool intIn(const boost::json::value* v, int64_t lo, int64_t hi, int64_t& out)
        {
            if (!v || !v->is_int64())
            {
                return false;
            }
            out = v->get_int64();
            return out >= lo && out <= hi;
        }

        // The answer {"type":"frames",...}: research/324 section 4.4. out is written only when everything passed.
        bool readAnswer(const boost::json::object& o, S32 tokens_asked, const LLVector3& start_hips, MotionAnswer& out)
        {
            int64_t tokens = 0;
            if (!intIn(o.if_contains("tokens"), MOTION_TOKENS_MIN, MOTION_TOKENS_MAX, tokens) ||
                (tokens_asked != MOTION_TOKENS_AUTO && tokens != tokens_asked))
            {
                return false;
            }
            double helper_ms = 0.0;
            const boost::json::value* ms = o.if_contains("compute_ms");
            if (!ms || !finiteNumber(*ms, helper_ms) || helper_ms < 0.0 || helper_ms > MAX_HELPER_MS)
            {
                return false;
            }
            const boost::json::value* frames = o.if_contains("frames");
            const size_t count = size_t(motionFramesOfTokens(S32(tokens)));
            if (!frames || !frames->is_array() || frames->as_array().size() != count)
            {
                return false;
            }
            std::vector<SomaFrame> result(count);
            for (size_t f = 0; f < count; ++f)
            {
                const boost::json::value& fv = frames->as_array()[f];
                if (!fv.is_object())
                {
                    return false;
                }
                const boost::json::object& fo = fv.as_object();
                double hips[3];
                if (!numberRow(fo.if_contains("hips"), 3, hips))
                {
                    return false;
                }
                SomaFrame& frame = result[f];
                frame.hips.set(F32(hips[0]), F32(hips[1]), F32(hips[2]));
                if (!validPosition(frame.hips))
                {
                    return false;
                }
                for (S32 i = 0; i < 3; ++i)
                {
                    if (std::fabs(frame.hips.mV[i] - start_hips.mV[i]) > MOTION_MAX_HIPS_FROM_START_M)
                    {
                        return false;
                    }
                }
                const boost::json::value* local = fo.if_contains("local");
                if (!local || !local->is_array() || local->as_array().size() != size_t(SOMA_JOINT_COUNT))
                {
                    return false;
                }
                for (S32 j = 0; j < SOMA_JOINT_COUNT; ++j)
                {
                    double q[4];   // w, x, y, z
                    if (!numberRow(&local->as_array()[j], 4, q) ||
                        !normalizedRotation(LLQuaternion(F32(q[1]), F32(q[2]), F32(q[3]), F32(q[0])), frame.local[j]))
                    {
                        return false;
                    }
                }
                double contacts[ANSWER_CONTACTS];
                if (!numberRow(fo.if_contains("contacts"), ANSWER_CONTACTS, contacts))
                {
                    return false;
                }
                for (const double c : contacts)
                {
                    if (c < 0.0 || c > 1.0)
                    {
                        return false;
                    }
                }
            }
            out.frames = std::move(result);
            out.tokens = S32(tokens);
            out.helper_ms = F32(helper_ms);
            return true;
        }

        // The helper's "reason" of a failure of kind "model"; one we do not know is taken as unreadable (the file
        // is then offered to be fetched again: a way out, not the same failure at every press)
        ModelLoadFailure loadFailureOf(const std::string& reason)
        {
            if (reason == "missing")
            {
                return ModelLoadFailure::Missing;
            }
            if (reason == "size")
            {
                return ModelLoadFailure::Size;
            }
            return ModelLoadFailure::Unreadable;
        }

        // std::filesystem, not LLFile: LLFile's warnings put the path (the user's name) in the log
        bool regularFile(const std::string& utf8)
        {
            std::error_code ec;
            return !utf8.empty() && std::filesystem::is_regular_file(std::filesystem::path(ll_convert_string_to_wide(utf8)), ec);
        }
    }

    const char* describe(MotionFailure failure)
    {
        switch (failure)
        {
        case MotionFailure::None:            return "none";
        case MotionFailure::LowMemory:       return "not enough free memory";
        case MotionFailure::NoHelper:        return "the helper is missing or cannot start";
        case MotionFailure::BadFolder:       return "the model folder cannot be given to the helper";
        case MotionFailure::BadInput:        return "the request was refused";
        case MotionFailure::ModelMissing:    return "the model file is missing or not its size";
        case MotionFailure::ModelUnreadable: return "the model file cannot be read";
        case MotionFailure::Timeout:         return "the helper did not answer in time";
        case MotionFailure::HelperEnded:     return "the helper ended or said what it may not";
        case MotionFailure::RunFailed:       return "the model failed";
        case MotionFailure::BadAnswer:       return "the answer did not pass the checks";
        }
        return "unknown";
    }

    struct MotionClient::Lines
    {
        // In this order: the client (and its reader thread, which pushes into the inbox) goes first.
        plus::subtitle::HelperLineInbox inbox;
        plus::subtitle::LineClient      client;
    };

    MotionClient::MotionClient(std::function<MotionClientConfig()> make_config)
        : mMakeConfig(std::move(make_config)), mLines(new Lines)
    {
    }

    MotionClient::~MotionClient()
    {
        stop();
    }

    uint32_t MotionClient::failureCount(MotionFailure failure) const
    {
        const size_t i = size_t(failure);
        return i < mFailureCounts.size() ? mFailureCounts[i] : 0;
    }

    void MotionClient::setState(HelperState state, F64 now_sec)
    {
        mState = state;
        mStateSince = now_sec;
    }

    GenerateResult MotionClient::refuse(MotionFailure failure)
    {
        mPending.clear();
        mFailure = failure;
        const uint32_t n = ++mFailureCounts[size_t(failure)];
        LL_WARNS("PoserMotion") << "not generating: " << describe(failure) << " (" << n << " this session)" << LL_ENDL;
        return GenerateResult::Refused;
    }

    MotionEvent MotionClient::fail(MotionFailure failure, HelperAfter after, F64 now_sec)
    {
        // A request (or the start for one) was out: the window is told, and lastFailure() says why. Otherwise
        // (Idle: the helper went away or said what it may not while nothing was asked) it is only counted and
        // logged, and lastFailure() keeps what the window was last told.
        const bool asked = busy();
        if (asked)
        {
            mFailure = failure;
        }
        const uint32_t n = ++mFailureCounts[size_t(failure)];
        LL_WARNS("PoserMotion") << "motion helper: " << describe(failure) << " (" << n << " this session)" << LL_ENDL;
        if (after == HelperAfter::Ended)
        {
            stop();
        }
        else
        {
            mJobId = 0;
            mPending.clear();
            setState(HelperState::Idle, now_sec);
        }
        return asked ? MotionEvent::Failed : MotionEvent::None;
    }

    void MotionClient::stop()
    {
        // H5: quit, the grace, the joins (LineClient::stop), then what the reader left in the inbox (its End too)
        mLines->client.stop();
        mLines->inbox.clear();
        mJobId = 0;
        mPending.clear();
        mReady = false;
        mState = HelperState::Stopped;
        mStateSince = 0.0;
    }

    bool MotionClient::startHelper(F64 now_sec)
    {
        mConfig = mMakeConfig ? mMakeConfig() : MotionClientConfig();
        if (mConfig.enough_memory && !mConfig.enough_memory())
        {
            refuse(MotionFailure::LowMemory);
            return false;
        }
        if (!helperModelFolderOk(mConfig.model_folder, mConfig.model_file))
        {
            refuse(MotionFailure::BadFolder);
            return false;
        }
        if (!regularFile(mConfig.helper_exe))
        {
            refuse(MotionFailure::NoHelper);
            return false;
        }
        const S32 threads = std::clamp(mConfig.threads, 0, MOTION_MAX_THREADS);
        // the folder was checked: a drive path without a quote and without a separator at its end
        const std::wstring args = L"--models \"" + ll_convert_string_to_wide(mConfig.model_folder) + L"\" -t " + std::to_wstring(threads);
        // a run before this one may have left its End in the inbox (LineClient::start's own stop() puts it in)
        mLines->client.stop();
        mLines->inbox.clear();
        std::string error;   // names the exe's path: not for the log
        if (!mLines->client.start(mConfig.helper_exe, args, mLines->inbox.handlers(), error))
        {
            refuse(MotionFailure::NoHelper);
            return false;
        }
        mReady = false;
        setState(HelperState::Loading, now_sec);
        return true;
    }

    bool MotionClient::sendPending(F64 now_sec)
    {
        // the id first: a line the helper cannot read to its end still comes back with it (research/324, branch B)
        const uint64_t id = mNextJobId++;
        std::string line;
        line.reserve(mPending.size() + 32);
        line = "{\"id\":" + std::to_string(id) + ",";
        line += mPending;
        mPending.clear();
        if (!mLines->client.send(std::move(line)))
        {
            return false;
        }
        mJobId = id;
        setState(HelperState::Running, now_sec);
        return true;
    }

    GenerateResult MotionClient::generate(const SomaPose& start, const SomaPose& end, S32 tokens, F64 now_sec)
    {
        // standing still at the start (the context is the start, MOTION_CONTEXT_FRAMES times), the end once
        GenerateRequest request;
        for (SomaPose& frame : request.context)
        {
            frame = start;
        }
        request.targets[0] = end;
        request.ntargets = 1;
        request.tokens = tokens;
        return generate(request, now_sec);
    }

    GenerateResult MotionClient::generate(const GenerateRequest& request, F64 now_sec)
    {
        if (busy())
        {
            return GenerateResult::Busy;
        }
        GenerateRequest ok;
        if (!checkedRequest(request, ok) || mNextJobId > MAX_JOB_ID)
        {
            return refuse(MotionFailure::BadInput);
        }
        mPending.clear();
        mPending.reserve(REQUEST_RESERVE);
        appendRequest(mPending, ok);
        // the helper measures the hips from the context's last frame (soma_codec.cpp), so does the answer's check
        mStartHips = ok.context[MOTION_CONTEXT_FRAMES - 1].pos[0];
        mTokensAsked = ok.tokens;
        mGenerateAt = now_sec;
        mFailure = MotionFailure::None;

        if (mState == HelperState::Stopped)
        {
            return startHelper(now_sec) ? GenerateResult::Started : GenerateResult::Refused;
        }
        // Idle: the model is loaded
        if (!sendPending(now_sec))
        {
            // the helper went away while Idle and its end was not read yet: fail() counts it quietly (nothing was
            // asked of it yet), the press is refused and says why
            fail(MotionFailure::HelperEnded, HelperAfter::Ended, now_sec);
            mFailure = MotionFailure::HelperEnded;
            return GenerateResult::Refused;
        }
        return GenerateResult::Started;
    }

    void MotionClient::cancel(F64 now_sec)
    {
        if (mState == HelperState::Loading)
        {
            stop();   // H1 -> H5 -> H0: the model is not wanted now
        }
        else if (mState == HelperState::Running)
        {
            mJobId = 0;   // H2 -> H3: the answer comes in about 30 ms and is dropped
            setState(HelperState::Idle, now_sec);
        }
        else
        {
            return;
        }
        mFailure = MotionFailure::None;
    }

    MotionEvent MotionClient::readLine(const std::string& text, F64 now_sec, MotionAnswer& out)
    {
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(text, ec);
        if (ec || !v.is_object())
        {
            // a line cut by the reader's limit, or not ours: the helper cannot be trusted any more
            return fail(MotionFailure::HelperEnded, HelperAfter::Ended, now_sec);
        }
        const boost::json::object& o = v.as_object();
        const std::string type = stringOf(o, "type");
        const int64_t id = idOf(o);
        const bool for_job = mState == HelperState::Running && id >= 0 && uint64_t(id) == mJobId;

        if (type == "ready")
        {
            if (mState == HelperState::Loading && !mReady)
            {
                mReady = true;
                if (!mLines->client.send(LOAD_LINE))
                {
                    return fail(MotionFailure::HelperEnded, HelperAfter::Ended, now_sec);
                }
            }
            return MotionEvent::None;
        }
        if (type == "loaded")
        {
            if (mState == HelperState::Loading && mReady && !sendPending(now_sec))
            {
                return fail(MotionFailure::HelperEnded, HelperAfter::Ended, now_sec);
            }
            return MotionEvent::None;
        }
        if (type == "frames")
        {
            if (!for_job)
            {
                return MotionEvent::None;   // an answer to a request that was cancelled
            }
            if (!readAnswer(o, mTokensAsked, mStartHips, out))
            {
                return fail(MotionFailure::BadAnswer, HelperAfter::LivesOn, now_sec);
            }
            out.wait_sec = now_sec - mGenerateAt;
            mJobId = 0;
            mFailure = MotionFailure::None;
            setState(HelperState::Idle, now_sec);
            return MotionEvent::Answer;
        }
        if (type == "error")
        {
            const std::string kind = stringOf(o, "kind");
            // the helper's failure without an id belongs to the request out: no other line of ours can have one
            // (research/324, branch B)
            const bool ours = for_job || (mState == HelperState::Running && id == ID_NONE);
            if (kind == "model" && (mState == HelperState::Loading || ours))
            {
                // the helper keeps saying the same: end it first, then the model's state (research/324, branch B)
                const ModelLoadFailure why = loadFailureOf(stringOf(o, "reason"));
                const std::function<void(ModelLoadFailure)> tell = mConfig.on_model_failure;
                const MotionEvent event = fail(why == ModelLoadFailure::Unreadable ? MotionFailure::ModelUnreadable
                                                                                   : MotionFailure::ModelMissing,
                                               HelperAfter::Ended, now_sec);
                if (tell)
                {
                    tell(why);
                }
                return event;
            }
            if (mState == HelperState::Loading)
            {
                return fail(MotionFailure::HelperEnded, HelperAfter::Ended, now_sec);   // only "load" went out
            }
            if (ours)
            {
                return fail(kind == "input" ? MotionFailure::BadInput : MotionFailure::RunFailed, HelperAfter::LivesOn, now_sec);
            }
            return MotionEvent::None;   // about a request that was cancelled
        }
        return MotionEvent::None;   // "loading", and what a later helper may say that we do not know
    }

    MotionEvent MotionClient::update(F64 now_sec, MotionAnswer& out)
    {
        if (mState == HelperState::Stopped)
        {
            return MotionEvent::None;
        }
        plus::subtitle::HelperLine line;
        while (mLines->inbox.pop(line))
        {
            switch (line.kind)
            {
            case plus::subtitle::HelperLine::Kind::Text:
            {
                const MotionEvent event = readLine(line.text, now_sec, out);
                if (event != MotionEvent::None || mState == HelperState::Stopped)
                {
                    return event;
                }
                break;
            }
            case plus::subtitle::HelperLine::Kind::Overflow:
                return fail(MotionFailure::HelperEnded, HelperAfter::Ended, now_sec);
            case plus::subtitle::HelperLine::Kind::End:
                return fail(mReady ? MotionFailure::HelperEnded : MotionFailure::NoHelper, HelperAfter::Ended, now_sec);
            }
        }
        const F64 in_state_sec = now_sec - mStateSince;
        switch (mState)
        {
        case HelperState::Loading:
            if (in_state_sec > mConfig.load_timeout_sec)
            {
                return fail(MotionFailure::Timeout, HelperAfter::Ended, now_sec);
            }
            break;
        case HelperState::Running:
            if (in_state_sec > mConfig.run_timeout_sec)
            {
                return fail(MotionFailure::Timeout, HelperAfter::Ended, now_sec);
            }
            break;
        case HelperState::Idle:
            if (in_state_sec > mConfig.idle_sec)
            {
                stop();   // unused for a while: its memory goes back (the next press loads it again)
            }
            break;
        case HelperState::Stopped:
            break;
        }
        return MotionEvent::None;
    }
} // namespace plus::posermotion
