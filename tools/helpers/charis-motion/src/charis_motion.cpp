/**
 * @file charis_motion.cpp
 * @brief charis-motion.exe, the motion-between-poses helper of Charis Viewer (the NVIDIA MotionBricks SOMA model
 *        on ONNX Runtime, on the CPU).
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

// charis-motion: makes the motion between two poses (0.6 to 1.9 seconds at 30 frames a second).
//
// Reads one JSON object per line on stdin (at most MAX_REQUEST_BYTES), writes one JSON object per line on stdout
// (UTF-8). A closed stdin ends the process.
//   in : {"cmd":"load"}                                   load the model now (otherwise on the first request)
//        {"cmd":"quit"}                                   exit at once
//        {"id":N,"op":"inbetween","context":[4 frames],"targets":[1..4 frames],"tokens":T}
//        {"id":N,"op":"encode",...the same...}            the model's inputs only, no model needed (for tests)
//          a frame: {"pos":[[x,y,z] x 30],"rot":[[w,x,y,z] x 30]}: the 30 SOMA joints' world positions (meters,
//          each coordinate within MAX_POSITION_M) and world rotations (length 0.5..2, normalized here), in the
//          SOMA aligned frame (+X forward, +Y up, +Z right; column-vector quaternions). Where the frames face does
//          not matter: they are turned to face +X on the last context frame.
//          T: 0 lets the model choose the length (6..16 tokens), or exactly 6..16 tokens (4 frames each).
//          N: a whole number 0..2^53-1.
//   out: {"type":"ready","version":"1"}                   once, at start (no model is loaded yet)
//        {"type":"loading"} / {"type":"loaded","load_ms":N}
//        {"type":"frames","id":N,"tokens":T,"compute_ms":X,"frames":[{"hips":[x,y,z],"local":[[w,x,y,z] x 30],
//                 "contacts":[4]} x (4T-6)]}             hips: world position; local: each joint's rotation
//                                                         relative to its parent (the root's: world), w >= 0;
//                                                         contacts: left heel, left toe, right heel, right toe
//                                                         (the model's order, docs/soma_model.md), 0..1
//        {"type":"inputs","id":N,...}                     the answer to "encode"
//        {"type":"error","id":N,"kind":"model|input|run","message":"..."}   id -1 when there was none
//          kind model also has "reason": "missing" (no model file), "size" (not the expected size) or
//          "unreadable" (the right size but ONNX Runtime cannot read it, or it is not the expected model).
// The model is <models>/MODEL_FILE: no other file there is read (the model has no external data). Its size is
// checked before it is read; its hash is checked when it is downloaded, not here.
// stderr gets no paths and no names: versions, counts, milliseconds and kinds of failure only.
//
// Options: --models DIR (a folder on a drive)  -t THREADS (0 = the physical cores, at most 4; at most 16)  --normalprio  -v
// Exit status: 0 (stdin closed or quit), 2 (bad options), 3 (charis-onnxruntime.dll is missing or not 1.23.2).

#include "soma_codec.h"
#include "soma_tables.h"

#include "helper_json.h"
#include "helper_lineio.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
    const char* const    VERSION = "1";
    const char* const    ORT_VERSION = "1.23.2";
    const wchar_t* const ORT_DLL = L"charis-onnxruntime.dll";
    const wchar_t* const MODEL_FILE = L"mmm_v6_soma_fp32.onnx";
    constexpr long long  MODEL_BYTES = 757864273;          // the released file (its SHA-256 is checked by the viewer)

    constexpr size_t     MAX_REQUEST_BYTES = 64 * 1024;    // a request is about 20 KB (8 frames)
    constexpr double     MAX_POSITION_M = 10.0;
    constexpr double     MIN_QUAT_LENGTH = 0.5;
    constexpr double     MAX_QUAT_LENGTH = 2.0;
    constexpr long long  MAX_ID = 9007199254740991LL;       // 2^53 - 1: an id survives a double on the other side
    constexpr int        MAX_THREADS = 16;
    constexpr int        AUTO_MAX_THREADS = 4;

    using charis_helper::Reader;
    using charis_helper::emitLine;
    using charis_helper::quote;

    struct Options
    {
        std::wstring models;
        int          threads = 0;   // 0: auto
        bool         normalPriority = false;
        bool         verbose = false;
    };

    double msSince(const std::chrono::steady_clock::time_point& t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    std::string msText(double ms)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", ms);
        return buf;
    }

    void emitError(long long id, const char* kind, const std::string& message, const char* reason = nullptr)
    {
        std::string line = "{\"type\":\"error\",\"id\":" + std::to_string(id) + ",\"kind\":\"" + kind + "\"";
        if (reason)
        {
            line += std::string(",\"reason\":\"") + reason + "\"";
        }
        emitLine(line + ",\"message\":" + quote(message) + "}");
    }

    // ---- requests ----

    struct Request
    {
        long long               id = -1;
        std::string             cmd;
        std::string             op;
        long long               tokens = -1;   // -1: not given
        std::vector<soma::Pose> context;
        std::vector<soma::Pose> targets;
    };

    // One of a frame's two tables, [[...] x 30] with `width` numbers a row, into scratch (once per frame).
    bool readJointRows(Reader& r, const std::string& key, size_t width, bool& seen, std::vector<double>& scratch,
                       std::string& error)
    {
        size_t rows = 0;
        scratch.clear();
        if (seen)
        {
            error = key + " twice";
            return false;
        }
        seen = true;
        if (!r.numberRows(scratch, width, soma::JOINTS, rows) || rows != soma::JOINTS)
        {
            error = key + " must be 30 rows of " + std::to_string(width) + " numbers";
            return false;
        }
        return true;
    }

    // {"pos":[[x,y,z] x 30],"rot":[[w,x,y,z] x 30]}: checked and normalized into pose.
    bool readPose(Reader& r, soma::Pose& pose, std::vector<double>& scratch, std::string& error)
    {
        bool       havePos = false, haveRot = false;
        const bool ok = r.object([&](const std::string& key) {
            if (key == "pos")
            {
                if (!readJointRows(r, key, 3, havePos, scratch, error))
                {
                    return false;
                }
                for (int j = 0; j < soma::JOINTS; ++j)
                {
                    const double* v = scratch.data() + j * 3;
                    if (!(std::abs(v[0]) <= MAX_POSITION_M && std::abs(v[1]) <= MAX_POSITION_M && std::abs(v[2]) <= MAX_POSITION_M))
                    {
                        error = "a position is out of range";
                        return false;
                    }
                    pose.pos[j] = { v[0], v[1], v[2] };
                }
                return true;
            }
            if (key == "rot")
            {
                if (!readJointRows(r, key, 4, haveRot, scratch, error))
                {
                    return false;
                }
                for (int j = 0; j < soma::JOINTS; ++j)
                {
                    const double* q = scratch.data() + j * 4;
                    const double  n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    if (!(n >= MIN_QUAT_LENGTH && n <= MAX_QUAT_LENGTH))
                    {
                        error = "a rotation is not a unit quaternion";
                        return false;
                    }
                    pose.rot[j] = { q[0] / n, q[1] / n, q[2] / n, q[3] / n };
                }
                return true;
            }
            return r.skipStrict();   // an unknown key: any well-formed value
        });
        if (!ok)
        {
            if (error.empty())
            {
                error = "bad frame";
            }
            return false;
        }
        if (!havePos || !haveRot)
        {
            error = "a frame needs pos and rot";
            return false;
        }
        return true;
    }

    // [frame, ...] with at most maxFrames.
    bool readPoses(Reader& r, std::vector<soma::Pose>& poses, size_t maxFrames, std::vector<double>& scratch,
                   const char* what, std::string& error)
    {
        poses.clear();
        size_t     count = 0;
        const bool ok = r.array(
            [&](size_t i) {
                poses.resize(i + 1);
                return readPose(r, poses[i], scratch, error);
            },
            maxFrames, count);
        if (!ok && error.empty())
        {
            error = std::string("bad ") + what;
        }
        return ok;
    }

    bool parseRequest(const std::string& line, Request& req, std::vector<double>& scratch, std::string& error)
    {
        Reader r(line);
        r.ws();
        if (r.p >= line.size() || line[r.p] != '{')
        {
            error = "not a JSON object";
            return false;
        }
        bool seenId = false, seenCmd = false, seenOp = false, seenTokens = false, seenContext = false, seenTargets = false;
        auto once = [&](bool& seen, const std::string& key) {
            if (seen)
            {
                error = "\"" + key + "\" twice";
                return false;
            }
            seen = true;
            return true;
        };
        const bool ok = r.object([&](const std::string& key) {
            if (key == "id")
            {
                long long id = -1;
                if (!once(seenId, key) || !r.integer(id, 0, MAX_ID))
                {
                    if (error.empty())
                    {
                        error = "id must be a whole number 0..2^53-1";
                    }
                    return false;
                }
                req.id = id;
                return true;
            }
            if (key == "cmd" || key == "op")
            {
                std::string& value = key == "cmd" ? req.cmd : req.op;
                if (!once(key == "cmd" ? seenCmd : seenOp, key) || !r.string(value) || value.size() > 32)
                {
                    if (error.empty())
                    {
                        error = "\"" + key + "\" must be a short string";
                    }
                    return false;
                }
                return true;
            }
            if (key == "tokens")
            {
                if (!once(seenTokens, key) || !r.integer(req.tokens, 0, soma::MAX_TOKENS) ||
                    (req.tokens != 0 && req.tokens < soma::MIN_TOKENS))
                {
                    if (error.empty())
                    {
                        error = "tokens must be 0 or 6..16";
                    }
                    return false;
                }
                return true;
            }
            if (key == "context")
            {
                return once(seenContext, key) &&
                       readPoses(r, req.context, soma::CONTEXT_FRAMES, scratch, "context (4 frames)", error);
            }
            if (key == "targets")
            {
                return once(seenTargets, key) &&
                       readPoses(r, req.targets, soma::MAX_TARGET_FRAMES, scratch, "targets (1 to 4 frames)", error);
            }
            return r.skipStrict();   // an unknown key: any well-formed value
        });
        if (!ok)
        {
            if (error.empty())
            {
                error = "bad JSON";
            }
            return false;
        }
        if (!r.atEnd())
        {
            error = "text after the object";
            return false;
        }
        return true;
    }

    // What a parsed request is still missing, or nullptr.
    const char* requestProblem(const Request& req)
    {
        if (req.id < 0)
        {
            return "no id";
        }
        if (req.op != "inbetween" && req.op != "encode")
        {
            return req.op.empty() ? "no op" : "unknown op";
        }
        if (req.context.size() != soma::CONTEXT_FRAMES)
        {
            return "context must have 4 frames";
        }
        if (req.targets.empty() || req.targets.size() > soma::MAX_TARGET_FRAMES)
        {
            return "targets must have 1 to 4 frames";
        }
        if (req.tokens < 0)
        {
            return "no tokens";
        }
        return nullptr;
    }

    // The model's window: the context, the targets, the last target repeated.
    void buildWindow(const Request& req, soma::Pose (&window)[soma::WINDOW_FRAMES])
    {
        for (int f = 0; f < soma::WINDOW_FRAMES; ++f)
        {
            if (f < soma::CONTEXT_FRAMES)
            {
                window[f] = req.context[f];
            }
            else
            {
                const size_t t = std::min(size_t(f - soma::CONTEXT_FRAMES), req.targets.size() - 1);
                window[f] = req.targets[t];
            }
        }
    }

    // ---- answers ----

    void appendFloats(std::string& out, const float* v, int count)
    {
        out += '[';
        for (int i = 0; i < count; ++i)
        {
            if (i)
            {
                out += ',';
            }
            charis_helper::appendFloat(out, v[i]);
        }
        out += ']';
    }

    void appendInts(std::string& out, const int32_t* v, int count)
    {
        out += '[';
        for (int i = 0; i < count; ++i)
        {
            out += (i ? "," : "") + std::to_string(v[i]);
        }
        out += ']';
    }

    void appendQuat(std::string& out, const soma::Quat& q)
    {
        const float v[4] = { float(q.w), float(q.x), float(q.y), float(q.z) };
        appendFloats(out, v, 4);
    }

    std::string inputsAnswer(long long id, const soma::ModelInputs& in)
    {
        std::string out = "{\"type\":\"inputs\",\"id\":" + std::to_string(id);
        out += ",\"global_root_values\":";
        appendFloats(out, in.globalRoot, soma::WINDOW_FRAMES * soma::GLOBAL_ROOT_DIM);
        out += ",\"local_root_values\":";
        appendFloats(out, in.localRoot, soma::WINDOW_FRAMES * soma::LOCAL_ROOT_DIM);
        out += ",\"local_poses\":";
        appendFloats(out, in.localPoses, soma::WINDOW_FRAMES * soma::POSE_DIM);
        out += ",\"has_global_root_values\":";
        appendInts(out, in.hasGlobalRoot, soma::WINDOW_FRAMES);
        out += ",\"has_local_root_values\":";
        appendInts(out, in.hasLocalRoot, soma::WINDOW_FRAMES);
        out += ",\"has_local_poses\":";
        appendInts(out, in.hasLocalPoses, soma::WINDOW_FRAMES);
        out += ",\"allowed_pred_num_tokens\":";
        appendInts(out, in.allowedTokens, soma::TOKEN_CHOICES);
        return out + "}";
    }

    std::string framesAnswer(long long id, int tokens, double ms, const std::vector<soma::OutFrame>& frames)
    {
        std::string out;
        out.reserve(frames.size() * 2048 + 128);
        out += "{\"type\":\"frames\",\"id\":" + std::to_string(id) + ",\"tokens\":" + std::to_string(tokens) +
              ",\"compute_ms\":" + msText(ms) + ",\"frames\":[";
        for (size_t f = 0; f < frames.size(); ++f)
        {
            const soma::OutFrame& fr = frames[f];
            out += f ? ",{\"hips\":" : "{\"hips\":";
            const float hips[3] = { float(fr.hips.x), float(fr.hips.y), float(fr.hips.z) };
            appendFloats(out, hips, 3);
            out += ",\"local\":[";
            for (int j = 0; j < soma::JOINTS; ++j)
            {
                if (j)
                {
                    out += ',';
                }
                appendQuat(out, fr.local[j]);
            }
            out += "],\"contacts\":";
            appendFloats(out, fr.contacts, soma::CONTACTS);
            out += '}';
        }
        return out + "]}";
    }

    // ---- the model ----

    // ONNX Runtime's log lines can name the model's path: none of them are printed.
    void ORT_API_CALL quietLogger(void*, OrtLoggingLevel, const char*, const char*, const char*, const char*)
    {
    }

    struct TensorSpec
    {
        const char*                 name;
        ONNXTensorElementDataType   type;
        std::vector<int64_t>        shape;
    };

    // soma_n1.json's tensor_specs.
    const TensorSpec INPUT_SPECS[] = {
        { "global_root_values", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, { 1, soma::WINDOW_FRAMES, soma::GLOBAL_ROOT_DIM } },
        { "has_global_root_values", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, { 1, soma::WINDOW_FRAMES } },
        { "local_root_values", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, { 1, soma::WINDOW_FRAMES, soma::LOCAL_ROOT_DIM } },
        { "has_local_root_values", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, { 1, soma::WINDOW_FRAMES } },
        { "local_poses", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, { 1, soma::WINDOW_FRAMES, soma::POSE_DIM } },
        { "has_local_poses", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, { 1, soma::WINDOW_FRAMES } },
        { "allowed_pred_num_tokens", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, { 1, soma::TOKEN_CHOICES } },
    };
    const TensorSpec OUTPUT_SPECS[] = {
        { "pred_global_motions", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, { 1, soma::OUT_ROWS, soma::OUT_DIM } },
        { "pred_num_tokens", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, { 1 } },
    };
    constexpr size_t INPUTS = sizeof(INPUT_SPECS) / sizeof(INPUT_SPECS[0]);
    constexpr size_t OUTPUTS = sizeof(OUTPUT_SPECS) / sizeof(OUTPUT_SPECS[0]);

    // A tensor that is there: the type and the shape exactly.
    template <class Info>
    bool sameSpec(const Info& info, const TensorSpec& spec)
    {
        return info.GetElementType() == spec.type && info.GetShape() == spec.shape;
    }

    // A tensor the model declares: the type, and the shape with any named (dynamic, -1) dimension allowed.
    template <class Info>
    bool fitsSpec(const Info& info, const TensorSpec& spec)
    {
        const std::vector<int64_t> shape = info.GetShape();
        if (info.GetElementType() != spec.type || shape.size() != spec.shape.size())
        {
            return false;
        }
        for (size_t i = 0; i < shape.size(); ++i)
        {
            if (shape[i] >= 0 && shape[i] != spec.shape[i])
            {
                return false;
            }
        }
        return true;
    }

    class Model
    {
    public:
        Model(const Options& o, int threads) : mOpt(o), mThreads(threads) {}

        // Loads the model if it is not loaded yet. On a failure: false, the reason ("missing", "size",
        // "unreadable") and a message; the failure is kept (asking again answers the same at once).
        bool load(const char*& reason, std::string& error)
        {
            if (mSession)
            {
                return true;
            }
            if (mFailReason)
            {
                reason = mFailReason;
                error = mFailMessage;
                return false;
            }
            emitLine("{\"type\":\"loading\"}");
            const auto t0 = std::chrono::steady_clock::now();
            if (!open())
            {
                reason = mFailReason;
                error = mFailMessage;
                return false;
            }
            emitLine("{\"type\":\"loaded\",\"load_ms\":" + std::to_string(static_cast<long long>(msSince(t0))) + "}");
            if (mOpt.verbose)
            {
                std::fprintf(stderr, "loaded %.0fms, %d threads\n", msSince(t0), mThreads);
            }
            return true;
        }

        // One run. rows gets the first output (OUT_ROWS x OUT_DIM), tokens the second.
        bool run(soma::ModelInputs& in, std::vector<float>& rows, int& tokens, std::string& error)
        {
            try
            {
                const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                Ort::Value            values[INPUTS] = {
                    tensor(mem, in.globalRoot, INPUT_SPECS[0]),  tensor(mem, in.hasGlobalRoot, INPUT_SPECS[1]),
                    tensor(mem, in.localRoot, INPUT_SPECS[2]),   tensor(mem, in.hasLocalRoot, INPUT_SPECS[3]),
                    tensor(mem, in.localPoses, INPUT_SPECS[4]),  tensor(mem, in.hasLocalPoses, INPUT_SPECS[5]),
                    tensor(mem, in.allowedTokens, INPUT_SPECS[6]),
                };
                const char* inNames[INPUTS];
                for (size_t i = 0; i < INPUTS; ++i)
                {
                    inNames[i] = INPUT_SPECS[i].name;
                }
                const char* outNames[OUTPUTS] = { OUTPUT_SPECS[0].name, OUTPUT_SPECS[1].name };
                std::vector<Ort::Value> out =
                    mSession->Run(Ort::RunOptions{ nullptr }, inNames, values, INPUTS, outNames, OUTPUTS);
                if (out.size() != OUTPUTS || !out[0].IsTensor() || !out[1].IsTensor() ||
                    !sameSpec(out[0].GetTensorTypeAndShapeInfo(), OUTPUT_SPECS[0]) ||
                    !sameSpec(out[1].GetTensorTypeAndShapeInfo(), OUTPUT_SPECS[1]))
                {
                    error = "the model's answer is not the expected shape";
                    return false;
                }
                const float* data = out[0].GetTensorData<float>();
                rows.assign(data, data + size_t(soma::OUT_ROWS) * soma::OUT_DIM);
                tokens = out[1].GetTensorData<int32_t>()[0];
                return true;
            }
            catch (const Ort::Exception& e)
            {
                error = "inference failed (onnxruntime error " + std::to_string(int(e.GetOrtErrorCode())) + ")";
            }
            catch (const std::exception&)
            {
                error = "inference failed";
            }
            return false;
        }

    private:
        template <class T>
        static Ort::Value tensor(const Ort::MemoryInfo& mem, T* data, const TensorSpec& spec)
        {
            size_t count = 1;
            for (const int64_t d : spec.shape)
            {
                count *= size_t(d);
            }
            return Ort::Value::CreateTensor<T>(mem, data, count, spec.shape.data(), spec.shape.size());
        }

        bool fail(const char* reason, std::string message)
        {
            mFailReason = reason;
            mFailMessage = std::move(message);
            mSession.reset();
            return false;
        }

        bool open()
        {
            const std::wstring path = mOpt.models + L"\\" + MODEL_FILE;
            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) || (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                return fail("missing", "the model file is not there");
            }
            const long long bytes = (static_cast<long long>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            if (bytes != MODEL_BYTES)
            {
                return fail("size", "the model file is not the expected size");
            }
            try
            {
                mEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "charis-motion", quietLogger, nullptr);
                mEnv->DisableTelemetryEvents();
                Ort::SessionOptions so;
                so.SetIntraOpNumThreads(mThreads);
                so.SetInterOpNumThreads(1);
                so.AddConfigEntry("session.intra_op.allow_spinning", "0");   // no busy threads after a run
                mSession = std::make_unique<Ort::Session>(*mEnv, path.c_str(), so);
            }
            catch (const Ort::Exception& e)
            {
                return fail("unreadable", "the model cannot be read (onnxruntime error " + std::to_string(int(e.GetOrtErrorCode())) + ")");
            }
            catch (const std::exception&)
            {
                return fail("unreadable", "the model cannot be read");
            }
            if (!expectedSignature())
            {
                return fail("unreadable", "the model is not the expected one (inputs and outputs)");
            }
            return true;
        }

        // The model declares exactly the inputs and outputs of INPUT_SPECS and OUTPUT_SPECS (in any order).
        bool expectedSignature()
        {
            try
            {
                Ort::AllocatorWithDefaultOptions alloc;
                // one declared tensor (index k of the inputs or the outputs) against the spec of the same name
                auto declaredFits = [&](bool input, size_t k, const TensorSpec* specs, size_t count) {
                    const Ort::AllocatedStringPtr name = input ? mSession->GetInputNameAllocated(k, alloc)
                                                               : mSession->GetOutputNameAllocated(k, alloc);
                    const Ort::TypeInfo info = input ? mSession->GetInputTypeInfo(k) : mSession->GetOutputTypeInfo(k);
                    for (size_t i = 0; i < count; ++i)
                    {
                        if (std::string(name.get()) == specs[i].name)
                        {
                            return info.GetONNXType() == ONNX_TYPE_TENSOR && fitsSpec(info.GetTensorTypeAndShapeInfo(), specs[i]);
                        }
                    }
                    return false;
                };
                if (mSession->GetInputCount() != INPUTS || mSession->GetOutputCount() != OUTPUTS)
                {
                    return false;
                }
                // as many as the specs, each named by one, no name twice (ONNX names are unique): all of them
                for (size_t k = 0; k < INPUTS; ++k)
                {
                    if (!declaredFits(true, k, INPUT_SPECS, INPUTS))
                    {
                        return false;
                    }
                }
                for (size_t k = 0; k < OUTPUTS; ++k)
                {
                    if (!declaredFits(false, k, OUTPUT_SPECS, OUTPUTS))
                    {
                        return false;
                    }
                }
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        }

        const Options                 mOpt;
        const int                     mThreads;
        std::unique_ptr<Ort::Env>     mEnv;
        std::unique_ptr<Ort::Session> mSession;
        const char*                   mFailReason = nullptr;   // set once a load failed
        std::string                   mFailMessage;
    };

    // ---- start-up ----

    int physicalCores()
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformation(nullptr, &bytes);
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
        bytes = DWORD(info.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (!GetLogicalProcessorInformation(info.data(), &bytes))
        {
            return 1;
        }
        int cores = 0;
        for (size_t i = 0; i < bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i)
        {
            cores += info[i].Relationship == RelationProcessorCore ? 1 : 0;
        }
        return cores > 0 ? cores : 1;
    }

    // This exe's folder (no trailing backslash), or empty.
    std::wstring exeFolder()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
            if (n == 0)
            {
                return std::wstring();
            }
            if (n < path.size())
            {
                path.resize(n);
                break;
            }
            if (path.size() >= 32768)
            {
                return std::wstring();
            }
            path.resize(path.size() * 2);
        }
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    // charis-onnxruntime.dll from this exe's folder only (never the current folder, PATH or an older copy in System32),
    // its own imports from that folder and System32. Not in the import table, so a missing or wrong one is a
    // message here and not a loader dialog.
    bool loadOnnxRuntime()
    {
        const std::wstring folder = exeFolder();
        if (folder.empty())
        {
            std::fprintf(stderr, "charis-motion: cannot find its own folder\n");
            return false;
        }
        const HMODULE dll = LoadLibraryExW((folder + L"\\" + ORT_DLL).c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
        {
            std::fprintf(stderr, "charis-motion: charis-onnxruntime.dll could not be loaded (error %lu)\n", GetLastError());
            return false;
        }
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        const GetApiBase getBase = reinterpret_cast<GetApiBase>(GetProcAddress(dll, "OrtGetApiBase"));
        const OrtApiBase* base = getBase ? getBase() : nullptr;
        const char*       version = base ? base->GetVersionString() : nullptr;
        if (!version || std::string(version) != ORT_VERSION)
        {
            std::fprintf(stderr, "charis-motion: charis-onnxruntime.dll is not %s\n", ORT_VERSION);
            return false;
        }
        const OrtApi* api = base->GetApi(ORT_API_VERSION);
        if (!api)
        {
            std::fprintf(stderr, "charis-motion: charis-onnxruntime.dll has no API %d\n", ORT_API_VERSION);
            return false;
        }
        Ort::InitApi(api);
        return true;
    }

    bool parseArgs(int argc, wchar_t** argv, Options& o)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring a = argv[i];
            auto need = [&](const char* name) -> const wchar_t* {
                if (i + 1 >= argc)
                {
                    std::fprintf(stderr, "%s needs a value\n", name);
                    return nullptr;
                }
                return argv[++i];
            };
            const wchar_t* v = nullptr;
            if (a == L"--models")
            {
                if (!(v = need("--models")))
                {
                    return false;
                }
                o.models = v;
            }
            else if (a == L"-t")
            {
                if (!(v = need("-t")))
                {
                    return false;
                }
                wchar_t*   end = nullptr;
                const long n = std::wcstol(v, &end, 10);
                if (!*v || *end || n < 0 || n > MAX_THREADS)
                {
                    std::fprintf(stderr, "-t takes 0..%d\n", MAX_THREADS);
                    return false;
                }
                o.threads = int(n);
            }
            else if (a == L"--normalprio")
            {
                o.normalPriority = true;
            }
            else if (a == L"-v")
            {
                o.verbose = true;
            }
            else
            {
                std::fprintf(stderr, "unknown option\n");
                return false;
            }
        }
        // a folder on a drive ("C:\..."), nothing else: no share ("\\server\"), no device or long-path prefix
        // ("\\.\", "\\?\"), no relative path. The exe has no longPathAware manifest, so a model path longer than
        // MAX_PATH (260) is not found: a load answers "missing" (the viewer's folder is far shorter).
        const std::wstring& m = o.models;
        const bool onDrive = m.size() >= 3 && ((m[0] >= L'A' && m[0] <= L'Z') || (m[0] >= L'a' && m[0] <= L'z')) &&
                             m[1] == L':' && (m[2] == L'\\' || m[2] == L'/');
        if (!onDrive || m.size() > 30000)
        {
            std::fprintf(stderr, "usage: charis-motion --models <folder on a drive> [-t 0..%d] [--normalprio] [-v]\n", MAX_THREADS);
            return false;
        }
        while (o.models.size() > 3 && (o.models.back() == L'\\' || o.models.back() == L'/'))
        {
            o.models.pop_back();
        }
        return true;
    }

    // Buffers kept from one request to the next (no new allocation per request once they have grown).
    struct Scratch
    {
        std::vector<double>         numbers;   // the rows of one table while a request is read
        std::vector<float>          rows;      // the model's output
        std::vector<soma::OutFrame> frames;    // the decoded answer
    };

    // One "inbetween" or "encode" request (already checked).
    void answer(const Request& req, Model& model, const Options& opt, Scratch& scratch)
    {
        const bool  infer = req.op == "inbetween";
        const char* reason = nullptr;
        std::string error;
        if (infer && !model.load(reason, error))
        {
            emitError(req.id, "model", error, reason);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();   // compute_ms: the request without the loading
        soma::Pose window[soma::WINDOW_FRAMES];
        buildWindow(req, window);
        soma::ModelInputs in;
        soma::Alignment   align;
        encode(window, int(req.targets.size()), int(req.tokens), in, align);
        if (!infer)
        {
            emitLine(inputsAnswer(req.id, in));
            return;
        }
        std::vector<float>&          rows = scratch.rows;
        std::vector<soma::OutFrame>& frames = scratch.frames;
        int                          tokens = 0;
        if (!model.run(in, rows, tokens, error))
        {
            emitError(req.id, "run", error);
            return;
        }
        switch (soma::decode(rows.data(), tokens, int(req.tokens), align, frames))
        {
        case soma::DecodeResult::Ok:
            break;
        case soma::DecodeResult::BadTokens:
            emitError(req.id, "run", "the model answered " + std::to_string(tokens) + " tokens (not 6..16)");
            return;
        case soma::DecodeResult::WrongLength:
            emitError(req.id, "run", "the model answered another length (" + std::to_string(tokens) + " tokens)");
            return;
        case soma::DecodeResult::NotFinite:
            emitError(req.id, "run", "the model answered a number that is not finite, or hips out of range");
            return;
        case soma::DecodeResult::Degenerate:
            emitError(req.id, "run", "the model answered a rotation that is not one");
            return;
        }
        const double ms = msSince(t0);
        emitLine(framesAnswer(req.id, tokens, ms, frames));
        if (opt.verbose)
        {
            std::fprintf(stderr, "inbetween %d tokens, %zu frames, %.1fms\n", tokens, frames.size(), ms);
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    // DLLs only from System32 unless a load names its folder (LoadLibraryExW below); no error dialogs.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Options opt;
    if (!parseArgs(argc, argv, opt))
    {
        return 2;
    }
    if (!loadOnnxRuntime())
    {
        return 3;
    }
    charis_helper::setBinaryStdio();
    if (!opt.normalPriority)
    {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    }
    const int threads = opt.threads > 0 ? opt.threads : std::min(physicalCores(), AUTO_MAX_THREADS);
    Model     model(opt, threads);
    emitLine(std::string("{\"type\":\"ready\",\"version\":\"") + VERSION + "\"}");

    std::string line;
    Scratch     scratch;
    for (;;)
    {
        const charis_helper::LineRead got = charis_helper::readLine(stdin, line, MAX_REQUEST_BYTES);
        if (got == charis_helper::LineRead::End)
        {
            break;   // stdin closed: nothing is queued, so there is nothing to finish
        }
        if (got == charis_helper::LineRead::TooLong)
        {
            emitError(-1, "input", "the line is too long");
            continue;
        }
        if (line.empty() || line == "\r")
        {
            continue;
        }
        Request     req;
        std::string error;
        if (!parseRequest(line, req, scratch.numbers, error))
        {
            emitError(req.id, "input", error);
            continue;
        }
        if (!req.cmd.empty())
        {
            if (req.cmd == "quit")
            {
                break;
            }
            if (req.cmd != "load")
            {
                emitError(req.id, "input", "unknown cmd");
                continue;
            }
            const char* reason = nullptr;
            if (!model.load(reason, error))
            {
                emitError(-1, "model", error, reason);
            }
            continue;
        }
        if (const char* problem = requestProblem(req))
        {
            emitError(req.id, "input", problem);
            continue;
        }
        answer(req, model, opt, scratch);
    }
    // ONNX Runtime's thread pools and the session take long to tear down; every answer is already flushed and
    // nothing else needs closing, so leave without running destructors.
    std::fflush(stdout);
    std::_Exit(0);
}
