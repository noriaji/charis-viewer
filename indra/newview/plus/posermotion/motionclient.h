/**
 * @file motionclient.h
 * @brief "Motion Between Poses" (research/324 section 2.5, 5.1 H): the viewer's side of charis-motion.exe. The
 * machine H on plus/subtitle's LineClient and HelperLineInbox.
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

// The helper (tools/motion-helper in the sl-viewer repository; its protocol is at the head of charis_motion.cpp)
// runs the model in a process of its own: the viewer starts it the first time "generate" is pressed (not when the
// window opens), keeps it MOTION_HELPER_IDLE_SEC after the last request, and ends it when the window closes, on
// logout, on quit, before the model is removed, and when anything goes wrong. All of it is on the main thread but
// LineClient's writer and reader; the reader only moves each line into the inbox, and update() (the window's tick)
// takes them out and reads them here. Nothing runs while the helper is stopped: update() returns at its first line.
//
// States (research/324 section 5.1): Stopped H0, Loading H1 (started; "ready" -> {"cmd":"load"} -> "loaded", then
// the request that waited is sent), Running H2 (one request out), Idle H3 (loaded, nothing out). H5 (ending) is
// stop() itself: it ends within the call (LineClient::stop(), HELPER_QUIT_GRACE_MS), so it is never seen.

#ifndef PLUS_POSERMOTION_MOTIONCLIENT_H
#define PLUS_POSERMOTION_MOTIONCLIENT_H

#include "motionmodel.h"
#include "somaretarget.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plus::posermotion
{
    inline constexpr const char* MOTION_HELPER_EXE       = "charis-motion.exe";
    inline constexpr F64 MOTION_LOAD_TIMEOUT_SEC         = 120.0; // H1: from the start to "loaded"
    inline constexpr F64 MOTION_RUN_TIMEOUT_SEC          = 20.0;  // H2: from the request to its answer
    inline constexpr F64 MOTION_HELPER_IDLE_SEC          = 120.0; // H3: the helper is ended after this long unused
    inline constexpr uint64_t MOTION_MIN_FREE_BYTES      = 1536ull << 20; // free physical memory to start it (it takes about 0.8 GB)
    inline constexpr S32 MOTION_MAX_THREADS              = 16;    // PlusPoserMotionThreads is clamped to 0..this (0: the helper's own choice)
    inline constexpr S32 MOTION_TOKENS_AUTO              = 0;     // the model chooses the length (6..16 tokens)
    inline constexpr S32 MOTION_TOKENS_MIN               = 6;     // a token is 4 frames: 6..16 tokens are 18..58 frames
    inline constexpr S32 MOTION_TOKENS_MAX               = 16;
    inline constexpr S32 MOTION_TOKENS_DEFAULT           = 10;    // "automatic" in the window: 1.1 s (research/324, decided 2026-11-13)
    inline constexpr S32 MOTION_FRAMES_PER_TOKEN         = 4;
    inline constexpr S32 MOTION_EDGE_FRAMES_DROPPED      = 6;     // frames = tokens * 4 - 6
    inline constexpr S32 MOTION_CONTEXT_FRAMES           = 4;     // the helper takes exactly this many context frames
    inline constexpr S32 MOTION_MAX_TARGETS              = 4;     // and 1..this many targets (research/361 section 1.1)
    inline constexpr F32 MOTION_MAX_HIPS_FROM_START_M    = 5.f;   // an answer whose hips go farther from the context's last frame's is refused

    /// The frames of an answer of `tokens` tokens (6..16), or 0 for another number.
    constexpr S32 motionFramesOfTokens(S32 tokens)
    {
        return (tokens >= MOTION_TOKENS_MIN && tokens <= MOTION_TOKENS_MAX)
                   ? tokens * MOTION_FRAMES_PER_TOKEN - MOTION_EDGE_FRAMES_DROPPED : 0;
    }

    /// H0 .. H3 (research/324 section 5.1).
    enum class HelperState : uint8_t
    {
        Stopped, // H0
        Loading, // H1  started, the model is being read; the request waits
        Running, // H2  the request is out
        Idle,    // H3  loaded, nothing out
    };

    /// Why the last generate() was refused, or why the request failed (the window has its own words for each).
    enum class MotionFailure : uint8_t
    {
        None,
        LowMemory,       // less than MOTION_MIN_FREE_BYTES free, or memguard says the memory is low: not started
        NoHelper,        // charis-motion.exe is not next to the viewer, cannot be started, or ended before "ready"
                         // (charis-onnxruntime.dll missing or not its version)
        BadFolder,       // the model folder is not one the helper takes (helperModelFolderOk)
        BadInput,        // a pose is not finite / out of range, a bad length, or the helper refused the request
        ModelMissing,    // the helper did not find the file, or not its size: the model is Missing (M0)
        ModelUnreadable, // the helper could not read it: the model is Broken (M5)
        Timeout,         // no "loaded" in MOTION_LOAD_TIMEOUT_SEC, or no answer in MOTION_RUN_TIMEOUT_SEC
        HelperEnded,     // the helper went away, said more than it may, or a line could not be read
        RunFailed,       // the model or its post-processing failed (the helper's kind "run")
        BadAnswer,       // the answer did not pass the checks (frames, numbers, ranges)
    };
    inline constexpr size_t MOTION_FAILURE_KINDS = size_t(MotionFailure::BadAnswer) + 1;

    /// English, for the log.
    const char* describe(MotionFailure failure);

    /// generate()'s answer.
    enum class GenerateResult : uint8_t
    {
        Started, // Loading or Running now
        Busy,    // Loading or Running already (the window shows "cancel" then): nothing changed
        Refused, // not started: lastFailure() says why; the state is as it was (Stopped when the helper was found
                 // gone as it was pressed)
    };

    /// What update() hands over.
    enum class MotionEvent : uint8_t
    {
        None,   // nothing to say
        Answer, // the answer is in update()'s out (Idle now)
        Failed, // the request failed: lastFailure() (Idle when the helper lives on, else Stopped)
    };

    /// The model's answer, checked: frames for buildClip() (motionclip.h), and numbers for the diagnostic log.
    struct MotionAnswer
    {
        std::vector<SomaFrame> frames;      // motionFramesOfTokens(tokens) frames, rotations at unit length
        S32                    tokens = 0;
        F32                    helper_ms = 0.f;   // the helper's compute_ms (the request without the loading)
        F64                    wait_sec  = 0.0;   // from generate() to the answer (with the start and the loading)
    };

    /// What one start of the helper needs. The viewer's is made in posermotion.cpp (helperPath, the model folder,
    /// PlusPoserMotionThreads, memguard, motionModel()); a test makes its own.
    struct MotionClientConfig
    {
        std::string helper_exe;             // exactly that file (subtitle::helperPath: next to the viewer only)
        std::string model_folder;           // UTF-8; checked with helperModelFolderOk
        std::string model_file;             // the name the helper is to find (for the check of the folder)
        S32         threads          = 0;   // 0..MOTION_MAX_THREADS (clamped here too)
        F64         load_timeout_sec = MOTION_LOAD_TIMEOUT_SEC;
        F64         run_timeout_sec  = MOTION_RUN_TIMEOUT_SEC;
        F64         idle_sec         = MOTION_HELPER_IDLE_SEC;
        /// True when there is room to start the helper; empty: not asked.
        std::function<bool()> enough_memory;
        /// Told after the helper was stopped for a failure of kind "model" (the viewer's: motionModel().onHelperLoadFailure).
        std::function<void(ModelLoadFailure)> on_model_failure;
    };

    /// One request in full (research/361): the context (the last frames before the motion, in time order, the
    /// last one being where it starts) and the targets (the first one is where the motion ends; the rest are the
    /// frames after it, so the model reads the speed it arrives with). Every pose is a SomaPose of
    /// SomaRetarget::slToSoma.
    struct GenerateRequest
    {
        SomaPose context[MOTION_CONTEXT_FRAMES];
        SomaPose targets[MOTION_MAX_TARGETS];
        S32      ntargets = 1;                   // 1..MOTION_MAX_TARGETS
        S32      tokens   = MOTION_TOKENS_AUTO;  // MOTION_TOKENS_AUTO or 6..16
    };

    class MotionClient
    {
    public:
        /// make_config is asked each time the helper is to be started (so a changed setting counts from the next start).
        explicit MotionClient(std::function<MotionClientConfig()> make_config);
        ~MotionClient();   // stops
        MotionClient(const MotionClient&) = delete;
        MotionClient& operator=(const MotionClient&) = delete;

        HelperState   state() const { return mState; }
        bool          busy() const { return mState == HelperState::Loading || mState == HelperState::Running; }
        /// Why the last generate() was refused or the last request failed (what the window was told; None after an
        /// answer, a cancel or a new generate()). A helper that goes away while Idle does not change it.
        MotionFailure lastFailure() const { return mFailure; }
        /// How many times each failure came since the start of the viewer (0 = it did not come; counted for the log).
        uint32_t      failureCount(MotionFailure failure) const;

        /// "Generate": the motion from start to end (both SomaPose of SomaRetarget::slToSoma, the body's origin near
        /// 0: every coordinate within MAX_POSITION_M), tokens MOTION_TOKENS_AUTO or 6..16. The context is the start
        /// MOTION_CONTEXT_FRAMES times (standing still), the target the end once (research/324 section 2.4): the
        /// request form below with those. now_sec: real time (LLTimer::getTotalSeconds()). Stopped: starts the
        /// helper (Loading, the request waits). Idle: sends it (Running). Loading, Running: Busy. The caller makes
        /// sure the model is Installed.
        GenerateResult generate(const SomaPose& start, const SomaPose& end, S32 tokens, F64 now_sec);
        /// The same with the context and the targets given (see GenerateRequest). The answer's hips must stay
        /// within MOTION_MAX_HIPS_FROM_START_M of the context's last frame (where the helper measures them from).
        /// Refused (BadInput): a pose that is not finite or out of range, ntargets outside 1..MOTION_MAX_TARGETS,
        /// or a bad length.
        GenerateResult generate(const GenerateRequest& request, F64 now_sec);
        /// "Cancel": Loading ends the helper (Stopped); Running forgets the request (Idle; its answer is dropped when
        /// it comes). Nothing else. Leaves lastFailure() at None.
        void           cancel(F64 now_sec);
        /// Ends the helper (H5 -> H0) and forgets the request: the window closes, logout, quit, before the model
        /// is removed. Waits at most HELPER_QUIT_GRACE_MS plus the joins. Safe in any state, and twice.
        void           stop();

        /// The window's tick (every 0.1 s while it is open). Reads what the helper said, moves the state on, and
        /// ends the helper on its timeouts. At most one event a call (the lines after it wait for the next call).
        /// Stopped: returns None at once (no lock). On Answer, out gets the answer (its old content is replaced).
        /// Idle: the helper's end (or a line it may not say) stops it quietly (None; counted and logged).
        MotionEvent    update(F64 now_sec, MotionAnswer& out);

    private:
        enum class HelperAfter : uint8_t { LivesOn, Ended };   // a failure leaves the helper (Idle) or ends it (Stopped)

        MotionEvent    readLine(const std::string& line, F64 now_sec, MotionAnswer& out);
        bool           startHelper(F64 now_sec);
        bool           sendPending(F64 now_sec);
        void           setState(HelperState state, F64 now_sec);
        MotionEvent    fail(MotionFailure failure, HelperAfter after, F64 now_sec);
        GenerateResult refuse(MotionFailure failure);

        std::function<MotionClientConfig()> mMakeConfig;
        MotionClientConfig mConfig;             // of the run that goes on
        HelperState   mState     = HelperState::Stopped;
        MotionFailure mFailure   = MotionFailure::None;
        F64           mStateSince = 0.0;        // real time, when mState was entered (the timeouts and the idle end)
        F64           mGenerateAt = 0.0;        // real time of the generate() that the job belongs to
        uint64_t      mJobId     = 0;           // the request out (Running); 0: none
        uint64_t      mNextJobId = 1;           // never goes back (an old answer can never match)
        bool          mReady     = false;       // "ready" came in this run: an end before it is NoHelper (set by startHelper(), reset by stop())
        std::string   mPending;                 // the request line that waits for "loaded" (Loading only), else empty
        LLVector3     mStartHips;               // the job's context's last frame's hips: the answer's stay within MOTION_MAX_HIPS_FROM_START_M
        S32           mTokensAsked = 0;         // the job's tokens (MOTION_TOKENS_AUTO or 6..16)
        std::array<uint32_t, MOTION_FAILURE_KINDS> mFailureCounts{};

        // The inbox and the LineClient, in that order (the client's reader pushes into the inbox, so it goes first).
        struct Lines;
        std::unique_ptr<Lines> mLines;
    };
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONCLIENT_H
