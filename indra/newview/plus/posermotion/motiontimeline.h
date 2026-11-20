/**
 * @file motiontimeline.h
 * @brief "Motion Between Poses" with three or more poses (research/357): the key list, the table of what each
 * pair of neighbouring keys holds (method, length, the model's answers), the state of each segment, the queue
 * of generate requests, the one layout of joints for all keys, and the one clip that joins every segment and
 * every hold. The data and the rules only: no window, no viewer state, no helper (the window asks for the next
 * job and hands the answer back), so it is tested on its own (motiontimeline_test.cpp / build_timeline_test.bat).
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

// Segment states (research/357 section 4.1; the numbers are the memo's, S4 does not exist):
//   S0 Unmade   a Generate segment without an answer (the clip holds a Smooth stand-in of the same length)
//   S1 Queued   waiting for its turn
//   S2 Running  one request is out at the helper
//   S3 Made     answers (1..3) and a clip; Smooth and Linear segments are always Made
//   S5 Failed   the segment's own failure, with its reason; the others go on
// Whole-run failures (JobError, isWholeFailure) put the queue and the running segment back to S0 / S3.
// Passing through (research/361): a key B between two Generate segments can be "passed", so the motion does not stop
// at it. The segment into B is asked with targets that go on past B (the model reads the speed it arrives with),
// the segment out of B with the last frames of the one into B as its context. So a segment whose from key is passed
// waits for the segment before it (nextJob's table), and every answer remembers what it was asked with (SentRecord),
// so a later change of the neighbours is shown as "regenerating is advised" (ctx_stale, worked out again by every
// successful refresh, never a state of its own).
// cancelGeneration() is the user's "cancel" only; it never comes from an edit. An answer that arrives for a pair
// that is gone is dropped; one for a pair that is there is fitted to the keys as they are NOW (the answer carries
// the poses it was made for, so a later refit is always possible).

#ifndef PLUS_POSERMOTION_MOTIONTIMELINE_H
#define PLUS_POSERMOTION_MOTIONTIMELINE_H

#include "motionclip.h"
#include "motiontracks.h"

#include "plus/posermcp/posehistory.h"

#include <map>
#include <optional>
#include <vector>

namespace plus::posermotion
{
    // ---- limits ---------------------------------------------------------------------------------------------
    inline constexpr S32 TIMELINE_MAX_KEYS   = 16;
    // The joined clip's limits, TIMELINE_MIN_FRAMES (2) and TIMELINE_MAX_FRAMES (901: 30 s and frame 0), are in motionclip.h.
    inline constexpr F32 HOLD_MAX_SEC        = 5.f;
    inline constexpr F32 SEGMENT_MIN_SEC     = 0.2f;  // Smooth and Linear segments
    inline constexpr F32 SEGMENT_MAX_SEC     = 10.f;
    inline constexpr F32 SEGMENT_DEFAULT_SEC = 1.f;
    /// The model's tokens (4 frames each, less 6 for the whole answer): the same values as motionclient.h.
    inline constexpr S32 TIMELINE_TOKENS_AUTO    = 0;
    inline constexpr S32 TIMELINE_TOKENS_MIN     = 6;
    inline constexpr S32 TIMELINE_TOKENS_MAX     = 16;
    inline constexpr S32 TIMELINE_TOKENS_DEFAULT = 10; // what "automatic" means
    inline constexpr S32 TIMELINE_MAX_ANSWERS    = 3;  // candidates of one segment: L-2, L, L+2 tokens
    /// A refit that ends this much farther from the key than the answer did when it was made, or moves the
    /// pelvis this much farther, is marked "regenerating is advised" (the numbers are provisional).
    inline constexpr F32 REGEN_DEG = 15.f;
    inline constexpr F32 REGEN_M   = 0.05f;
    /// A refit that moves the pelvis this far at an end (over EDGE_FRAMES, 0.4 s, or half of a short clip: the feet
    /// slide) is marked too.
    inline constexpr F32 REGEN_PELVIS_JUMP_M = 0.15f;
    /// With looping, the end hold is 0 when the last key equals the first within these.
    inline constexpr F32 LOOP_SAME_DEG = 0.5f;
    inline constexpr F32 LOOP_SAME_M   = 0.001f;

    // ---- passing through (research/361) ---------------------------------------------------------------------
    /// What the model takes: exactly this many context frames and 1..this many targets (the same as motionclient.h).
    inline constexpr S32 TIMELINE_CONTEXT_FRAMES = 4;
    inline constexpr S32 TIMELINE_MAX_TARGETS    = 4;
    /// How far past the key the extra targets lie, as a part of the way to the next key (1, 2 and 3 times this).
    inline constexpr F32 PASS_D_WEAK   = 0.03f;
    inline constexpr F32 PASS_D_NORMAL = 0.06f;
    /// "Automatic": a key whose coming and going directions (12 body points, all counted alike) have a
    /// normalized cosine under this is a turning point and is not passed; nor is a key that moves less than this (the
    /// mean over the points, m) on either side (the numbers are provisional; the golden values are research/359).
    inline constexpr F32 PASS_AUTO_COS      = -0.4f;
    inline constexpr F32 PASS_AUTO_MIN_MOVE = 0.03f;
    inline constexpr S32 PASS_POINT_COUNT   = 12;
    /// The context an answer was made with is "stale" when the speed at the end of the segment before it (the step of
    /// every joint between the last two frames) is this much different now.
    inline constexpr F32 TAIL_VEL_DEG = 3.f;
    /// ... and the pelvis moves this much (per frame, m) otherwise over the last two frames.
    inline constexpr F32 TAIL_VEL_PELVIS_M = 0.01f;

    /// The frames of an answer of this many tokens (6..16), 0 for another number (as motionFramesOfTokens).
    constexpr S32 timelineFramesOfTokens(S32 tokens)
    {
        return (tokens >= TIMELINE_TOKENS_MIN && tokens <= TIMELINE_TOKENS_MAX) ? tokens * 4 - 6 : 0;
    }
    /// 1 / 30 s steps of a Smooth or Linear segment: its frames (the first frame is the key's).
    S32 framesOfSeconds(F32 seconds);

    // ---- ids ------------------------------------------------------------------------------------------------
    /// A key's id: unique, never reused (a deleted key's id stays dead). 0 is none.
    using KeyId = U32;
    inline constexpr KeyId NO_KEY = 0;

    /// A segment is the pair of keys it joins: moving, inserting or deleting keys keeps the pairs that stay.
    struct PairId
    {
        KeyId from = NO_KEY;
        KeyId to   = NO_KEY;
        auto operator<=>(const PairId&) const = default;
        bool valid() const { return from != NO_KEY && to != NO_KEY && from != to; }
    };

    enum class TimelineResult
    {
        Ok,
        TooManyKeys, // more than TIMELINE_MAX_KEYS
        TooLong,     // the joined clip would pass TIMELINE_MAX_FRAMES (an operation that adds time is refused)
        NoSuchKey,
        NoSuchPair,
        BadValue,    // a length, a hold, a token count or an index out of range, or not a number; or nothing to do
        NeedsKeys,   // fewer than two keys
        SamePoses,   // "Generate" needs two different poses (a Smooth or Linear segment may join equal ones)
        NotReady,    // the rest skeleton is not ready
        Busy,        // a segment that is queued or running cannot be changed that way
        BuildFailed, // the layout or a clip could not be made (describe(TrackResult) / the segment says why)
        NeedPrev,    // the segment starts at a passed key and the segment before it has no answer (to be made first)
    };
    const char* describe(TimelineResult result);

    // ---- passing through ------------------------------------------------------------------------------------
    enum class PassMode : U8
    {
        Auto,    // the timeline judges (judgeAutoPass): passed unless it is a turning point
        Through, // always passed (when the key is eligible)
    };
    enum class PassStrength : U8
    {
        Weak,   // PASS_D_WEAK
        Normal, // PASS_D_NORMAL
    };
    constexpr F32 passStrengthD(PassStrength s) { return s == PassStrength::Weak ? PASS_D_WEAK : PASS_D_NORMAL; }

    /// Why a key cannot be passed (research/361 section 1.2). To stop at a key, put the same pose twice (the pair
    /// between is not Generate: both keys lose their eligibility) or give it a hold.
    enum class PassEligibility : U8
    {
        Ok,
        EndKey,               // the first or the last key
        NeighbourNotGenerate, // a segment next to it is Smooth or Linear
        Hold,                 // it has a hold (hold_sec > 0)
        SamePose,             // a neighbour has the same pose (its pair cannot be generated)
    };
    const char* describe(PassEligibility e);

    /// The 12 body points of the automatic judgement (head, elbows, wrists, hands, knees, ankles, pelvis) as positions
    /// of a SOMA pose.
    void passPoints(const SomaPose& pose, LLVector3 (&out)[PASS_POINT_COUNT]);
    struct AutoPassResult
    {
        F32  cos     = 0.f; // normalized cosine of the coming and the going directions (0 when it cannot be worked out)
        F32  move1_m = 0.f; // the mean move of the points from A to B
        F32  move2_m = 0.f; // ... from B to C
        bool through = false;
    };
    /// research/361 section 1.3: through unless cos < PASS_AUTO_COS or either move is under PASS_AUTO_MIN_MOVE.
    AutoPassResult judgeAutoPass(const LLVector3 (&a)[PASS_POINT_COUNT], const LLVector3 (&b)[PASS_POINT_COUNT],
                                 const LLVector3 (&c)[PASS_POINT_COUNT]);

    // ---- keys -----------------------------------------------------------------------------------------------
    struct TimelineKey
    {
        KeyId           id       = NO_KEY;
        posermcp::history::Pose pose;      // the poser's pose as taken (plain data)
        F32             hold_sec = 0.f;    // the pose is kept this long (0..HOLD_MAX_SEC; the frame is repeated)
        U32             pose_ver = 0;      // grows every time the pose is replaced
        PassMode        pass     = PassMode::Auto;
        PassStrength    strength = PassStrength::Normal;
        // written by refresh() only (what the keys are now): the key is passed / the automatic judgement says "pass" (only
        // meaningful for an eligible key whose mode is Auto)
        bool            pass_effective = false;
        bool            pass_auto      = false;
        // what pass_auto was worked out from (the keys around it, their pose versions, the layout and the skeleton)
        struct AutoMemo
        {
            bool  known = false;
            KeyId prev = NO_KEY, next = NO_KEY;
            U32   ver_prev = 0, ver_self = 0, ver_next = 0, layout = 0, skeleton = 0;
            bool  through = false;
        } auto_memo;
    };

    // ---- segments -------------------------------------------------------------------------------------------
    enum class SegMethod : U8
    {
        Generate, // the model (motion helper)
        Smooth,   // spherical interpolation, ease in and out
        Linear,   // spherical interpolation, straight through
    };
    enum class SegState : U8
    {
        Unmade,  // S0
        Queued,  // S1
        Running, // S2
        Made,    // S3
        Failed,  // S5
    };

    /// What went wrong with one job. The first group ends the whole run (the helper cannot be used now), the
    /// second group only the segment (the window maps motionclient.h's MotionFailure onto these).
    enum class JobError : U8
    {
        None,
        // whole run
        LowMemory,
        NoHelper,
        BadFolder,
        ModelMissing,
        ModelUnreadable,
        Timeout,
        HelperEnded,
        // this segment only
        BadInput,
        RunFailed,
        BadAnswer,
        CannotBuild, // the answer could not be turned into a clip (fitting failed)
        PrevMissing, // (a note on a segment that went back to S0 / S3) the segment before it, passed into, has no answer
    };
    bool        isWholeFailure(JobError error);
    const char* describe(JobError error);

    /// What a job was asked with, as far as passing through goes (kept with the answer: SegmentAnswer::used). SL-side
    /// data, so a change of the rest skeleton cannot break it. Nothing is kept for the poses of a plain segment (the ends
    /// are sent_start / sent_end and suggest_regen watches them).
    struct SentRecord
    {
        bool                     made          = false; // set by nextJob (an answer without one is never "stale")
        // the context: the last frames of the answer of the segment before (from key passed), else the from key four times
        bool                     ctx_moving    = false;
        U32                      ctx_serial    = 0;     // the serial of the answer it came from
        std::vector<SlPose>      ctx_last2;             // its last two frames (the speed it had)
        // the targets: the to key, and past it (to key passed) the way to the key after it
        bool                     to_through    = false;
        PassStrength             to_strength   = PassStrength::Normal;
        U32                      to_ver        = 0;
        U32                      next_ver      = 0;
        KeyId                    next_id       = NO_KEY; // the key after the to key (versions count from 1 in every key: the id tells which)
        KeyPose                  to_pose;               // (only when to_through)
        KeyPose                  next_pose;
    };

    /// Why "regenerating is advised" (the bits of PairCache::regen_reasons; the window shows one mark and the reasons).
    inline constexpr U8 REGEN_ENDS    = 1; // the refit moved the ends too far (suggest_regen)
    inline constexpr U8 REGEN_TARGETS = 2; // the to key (or the one after it) moved since it was asked
    inline constexpr U8 REGEN_CONTEXT = 4; // the answer before it is another one, or ends with another speed
    inline constexpr U8 REGEN_PASS    = 8; // passed / stopped at a key, or the strength, changed since it was asked

    /// One answer of the model for a segment, with what is needed to refit it later.
    struct SegmentAnswer
    {
        S32                    tokens = 0;          // 6..16 (the answer's frames are timelineFramesOfTokens(tokens))
        std::vector<SomaFrame> frames;
        posermcp::history::Pose sent_start;          // the poses it was asked for
        posermcp::history::Pose sent_end;
        ClipStats              made_stats;           // how far the model's ends were from those poses
        U32                    serial = 0;           // unique: another answer is another serial (a longer one replacing it too)
        SentRecord             used;                 // what it was asked with (passing through)
    };

    /// What the timeline keeps per pair of keys (the memo's PairCache). The clip belongs to the selected answer
    /// (or is the stand-in of a segment without one); fit_* say what it was made from, so refresh() redoes only
    /// what changed.
    struct PairCache
    {
        SegMethod method  = SegMethod::Generate;
        S32       tokens  = TIMELINE_TOKENS_AUTO; // Generate: asked length (a selected answer's own length wins)
        F32       seconds = SEGMENT_DEFAULT_SEC;  // Smooth, Linear
        SegState  state   = SegState::Unmade;
        JobError  fail    = JobError::None;       // Failed: why
        std::vector<SegmentAnswer> answers;       // up to TIMELINE_MAX_ANSWERS
        S32       selected = -1;                  // index in answers; -1 none
        bool      suggest_regen = false;          // the refit moved the ends too far from what the answer was made for
        bool      ctx_stale     = false;          // what it was asked with differs from now (REGEN_TARGETS/CONTEXT/PASS); worked out by refresh
        U8        regen_reasons = 0;              // REGEN_* bits (including REGEN_ENDS for suggest_regen)
        // whether the two keys are equal (computed when a key's pose_ver changed; see samePair)
        bool      same_known    = false;
        bool      same          = false;
        U32       same_from_ver = 0;
        U32       same_to_ver   = 0;
        ClipStats fit_stats;                      // the selected answer refitted to the keys as they are
        MotionClip clip;                          // the segment's frames (first = its from key, last = its to key)
        // what the clip was made from (stale when any differs from now)
        bool      fitted        = false;
        U32       fit_from_ver  = 0;
        U32       fit_to_ver    = 0;
        U32       fit_layout    = 0;
        SegMethod fit_method    = SegMethod::Generate;
        S32       fit_answer    = -1;
        S32       fit_frames    = 0;
    };

    /// A segment as the window and the timeline strip show it (computed from the keys and the table; cheap).
    struct SegmentInfo
    {
        PairId    pair;
        S32       from_index    = 0;      // the key it starts at
        SegMethod method        = SegMethod::Generate;
        SegState  state         = SegState::Unmade;
        JobError  fail          = JobError::None;
        S32       start_frame   = 0;      // in the joined clip: where its motion starts (its from key's frame + that key's hold)
        S32       frames        = 0;      // its length in frame steps: its motion ends at start_frame + frames = the to key's frame
        S32       answer_count  = 0;
        S32       selected      = -1;
        S32       tokens        = TIMELINE_TOKENS_AUTO;
        F32       seconds       = 0.f;    // its length in seconds (frames / CLIP_FPS), whatever the method
        bool      suggest_regen = false;
        bool      ctx_stale     = false;
        U8        regen_reasons = 0;      // REGEN_* bits: when not 0, "regenerating is advised" (one mark, these are the reasons)
        bool      same_poses    = false;  // the two keys are equal (Generate is refused)
    };

    /// One request for the helper (nextJob). The poses are in the model's form already.
    struct GenerateJob
    {
        PairId    pair;
        S32       tokens = TIMELINE_TOKENS_DEFAULT; // 6..16 ("automatic" is already TIMELINE_TOKENS_DEFAULT here)
        bool      extra  = false;                // one more answer for a segment that has one ("more candidates")
        /// The context (TIMELINE_CONTEXT_FRAMES frames in time order, the last one is where the motion starts) and the
        /// targets (the first is where it ends; with a passed end key the rest are 1, 2 and 3 steps of that key's
        /// strength on the way to the next key). The same as motionclient.h's GenerateRequest.
        SomaPose  context[TIMELINE_CONTEXT_FRAMES];
        SomaPose  targets[TIMELINE_MAX_TARGETS];
        S32       ntargets = 1;
    };

    /// What became of an answer (onAnswer).
    enum class AnswerFate
    {
        Used,        // the segment is Made with it
        Dropped,     // its pair is gone, or nothing was out: not used
        CannotBuild, // it could not be used (wrong length, fitting failed): the segment is Failed
    };

    class MotionTimeline
    {
    public:
        // ---- keys (every call that changes keys, lengths, holds or methods makes the joined clip stale) ----
        // Indices are checked: out of range gives an empty key / segment / pose / frame 0, never a read outside.
        // A pointer from find(), cache() or findSegment() and a reference from key() / segment() / keyPose() are valid
        // until the next call that changes keys, pairs or answers (the table is made again then).
        S32               keyCount() const { return (S32)mKeys.size(); }
        const TimelineKey& key(S32 index) const;
        S32               indexOf(KeyId id) const;               // -1 if none
        const TimelineKey* find(KeyId id) const;

        /// A new key at the end (or at index, 0..keyCount()). Its pose is copied. A new pair takes the method of
        /// the pair before it (the one after when it is first; Generate when there is none).
        TimelineResult addKey(const posermcp::history::Pose& pose, KeyId* out_id = nullptr);
        TimelineResult insertKey(S32 index, const posermcp::history::Pose& pose, KeyId* out_id = nullptr);
        /// A copy of the key right after it (a new id; the pair between is Smooth, the poses being equal).
        TimelineResult duplicateKey(KeyId id, KeyId* out_id = nullptr);
        TimelineResult removeKey(KeyId id);
        /// The key goes to index (0..keyCount()-1) in the list.
        TimelineResult moveKey(KeyId id, S32 to_index);
        /// Replaces the pose of a key (pose_ver grows); the answers of its pairs stay and are refitted.
        TimelineResult updateKey(KeyId id, const posermcp::history::Pose& pose);
        TimelineResult setHold(KeyId id, F32 seconds);
        /// Passing through (research/361). Accepted whatever the state of the segments (a queued or running one is not
        /// touched: its answer will be marked when it differs from the keys). Nothing changes when the value is the same.
        TimelineResult setPass(KeyId id, PassMode mode);
        TimelineResult setStrength(KeyId id, PassStrength strength);
        /// Passing through is off until the owner turns it on: off, no key is passed (pass_effective false, requests as before
        /// research/361). The window turns it on as it is made (it has the controls: the key's mode and strength, the marks
        /// and their reasons); the tests of research/357 keep it off. Changing it makes the joined clip stale (the flags and
        /// marks are worked out again).
        void           setPassingEnabled(bool enabled);
        bool           passingEnabled() const { return mPassEnabled; }
        /// Whether the key can be passed (cheap; from the keys and the methods as they are now).
        PassEligibility passEligibility(KeyId id) const;
        /// "Loop": a copy of the first key is appended (a new id); not when the last key is it already (equal).
        TimelineResult closeLoop(KeyId* out_id = nullptr);
        /// Everything gone (keys, table, queue, clip); ids keep counting up.
        /// True when a request was out at the helper (the window cancels it: MotionClient::cancel).
        bool           clearAll();

        // ---- segments ------------------------------------------------------------------------------------
        S32            segmentCount() const { return (S32)mSegments.size(); }
        const SegmentInfo& segment(S32 index) const;
        const SegmentInfo* findSegment(PairId pair) const;
        const PairCache*   cache(PairId pair) const;   // null if the pair is not neighbours

        TimelineResult setMethod(PairId pair, SegMethod method);        // Generate refused for equal poses
        /// Smooth and Linear: seconds clamped to SEGMENT_MIN_SEC..SEGMENT_MAX_SEC (the drag of the right edge).
        TimelineResult setSeconds(PairId pair, F32 seconds);
        /// Generate: TIMELINE_TOKENS_AUTO or 6..16. An answer of that length is selected if there is one, else the
        /// segment is S0 (its old answers stay as candidates).
        TimelineResult setTokens(PairId pair, S32 tokens);
        /// Which of the answers is used (changes the length: the keys after it move in time). Busy while the segment is
        /// queued or running.
        TimelineResult selectAnswer(PairId pair, S32 index);

        // ---- generation: the queue the window works through, one job at a time ------------------------------
        /// "Generate this segment": S0, S3 or S5 Generate segments -> S1. Equal poses refused (SamePoses). A segment that
        /// starts at a passed key needs the segment before it to have an answer, or to be queued or running: else NeedPrev.
        TimelineResult requestGenerate(PairId pair);
        /// "Generate all": every S0 and S5 Generate segment (not S3) -> S1, in order. Returns how many. One that would be
        /// refused (NeedPrev, SamePoses) is skipped without a word: the count says how many went in.
        S32            requestAll();
        /// "More candidates": up to two more jobs, of the selected answer's tokens - 2 and + 2 (inside 6..16 and
        /// not one it has); the segment goes to S1 (S3 again when they are done). Returns how many jobs. None when the
        /// segment starts at a passed key and the segment before it has no answer (the rule of requestGenerate).
        S32            requestMoreAnswers(PairId pair);
        /// "Generate the ones after it too": this segment and the segments after it up to the next key that is not passed
        /// (a segment whose to key is not passed is the last), in order; the segments that already have answers too.
        /// Returns how many pairs; empty when there is no such pair.
        std::vector<PairId> chainFrom(PairId pair) const;
        /// Queues the whole chainFrom(pair), or nothing: every segment is looked at first with the rules of requestGenerate
        /// (Busy, SamePoses, BadValue for one that is not Generate, NeedPrev for the first when the segment before it has no
        /// answer), the ones after the first being asked with the one queued before them. *out_count: how many were queued.
        TimelineResult requestChain(PairId pair, S32* out_count = nullptr);
        /// What requestGenerate(pair) / requestChain(pair) would say now, without queueing (the window greys its buttons and
        /// says why by these: one rule for the button and the call).
        TimelineResult canRequestGenerate(PairId pair) const { return canQueue(pair, false); }
        TimelineResult canRequestChain(PairId pair) const;
        /// Whether requestMoreAnswers(pair) would queue a job (the window greys its button by this: one rule for both).
        bool           canRequestMoreAnswers(PairId pair) const;
        bool           busy() const { return mRunning.active || !mQueue.empty(); }
        bool           running() const { return mRunning.active; }
        S32            queuedCount() const { return (S32)mQueue.size(); }
        /// The next request, when nothing is running: the first queued segment whose pair is still there, still
        /// Generate, and not waiting; it is S2 now. A segment that starts at a passed key waits for the segment before it
        /// (skipped, stays queued) while that one is queued; when that one has no answer to go on, the segment and the ones
        /// passed after it go back to S0 / S3 (fail == PrevMissing). Never returns nothing while it only skipped: the
        /// queue gets shorter or a job comes out. Refreshes the layout first when the keys changed.
        std::optional<GenerateJob> nextJob(const SomaRetarget& retarget);
        /// The answer to the running job. Used: S3 (a refit for the keys as they are now).
        AnswerFate    onAnswer(const SomaRetarget& retarget, std::vector<SomaFrame>&& frames);
        /// The running job failed (or the window could not send it). A whole-run error puts the queue and the
        /// running segment back (S0 / S3, to be asked again by the user); a segment error makes only that one S5.
        /// Returns whether it was a whole-run error.
        bool           onFailure(JobError error);
        /// The user's "cancel": the queue empties, the running segment goes back to S0 / S3. True when a job was
        /// running (the window then cancels the helper's request). Edits never call this.
        bool           cancelGeneration();

        // ---- the joined clip ------------------------------------------------------------------------------
        /// Brings everything up to date: the layout over all keys (when a key changed), each segment's clip (only
        /// the stale ones), the joined clip. Called by the window after edits, while nothing plays.
        TimelineResult refresh(const SomaRetarget& retarget);
        bool           stale() const { return mStale; }
        const MotionClip&  clip() const { return mClip; }
        const TrackLayout& layout() const { return mLayout; }
        /// Why the last layout could not be made (TrackResult::Ok when it could).
        TrackResult    lastTrackResult() const { return mTrackResult; }
        /// The rest skeleton changed: every clip is made again at the next refresh().
        void           invalidateClips();
        /// Grows when the layout's joints change (the writer resizes its poses then).
        U32            layoutVersion() const { return mLayoutVersion; }
        /// Grows every time clip() is made again.
        U32            clipVersion() const { return mClipVersion; }
        /// The frame count of the joined clip as the lengths say now (valid when stale(), too).
        S32            totalFrames() const { return mTotalFrames; }
        F64            totalSeconds() const { return mTotalFrames > 1 ? (F64)(mTotalFrames - 1) / CLIP_FPS : 0.0; }
        /// The frame of key `index` in the joined clip (the first frame of its hold).
        S32            keyFrame(S32 index) const;
        S32            holdFrames(S32 index) const;
        /// The key poses in the layout. Valid when !stale() (they, layout() and clip() change together, only when refresh()
        /// succeeds; a refresh that fails leaves all three as they were).
        const KeyPose& keyPose(S32 index) const;
        /// 0 when the last key equals the first (a looped timeline), else LOOP_HOLD_SEC.
        F64            loopHoldSec() const { return mLoopHold; }

    private:
        struct Running
        {
            bool                     active = false;
            PairId                   pair;
            S32                      tokens = 0;
            bool                     extra  = false;
            U32                      from_ver = 0; // the keys' pose_ver when it was sent
            U32                      to_ver   = 0;
            posermcp::history::Pose  sent_start;
            posermcp::history::Pose  sent_end;
            SentRecord               sent;  // what the job was asked with; onAnswer puts it into the answer as it is
        };
        struct Queued
        {
            PairId pair;
            S32    tokens = 0;
            bool   extra  = false;
        };

        TimelineKey*     findMutable(KeyId id);
        PairCache*       cacheMutable(PairId pair);
        TimelineResult   insertAt(S32 index, const posermcp::history::Pose& pose, KeyId* out_id, bool check_length = true);
        void             structureChanged();            // pairs synced, view, stale
        void             syncPairs();
        void             updateView();                  // mSegments, mKeyFrames, mTotalFrames, mLoopHold
        void             markStale() { mStale = true; }
        /// After a change of a length or a hold: the view is made again; when the whole is now longer than the limit, undo()
        /// puts the change back and the call is TooLong, else the joined clip is stale.
        template <typename Undo> TimelineResult commitOrUndo(Undo&& undo);
        static S32       segmentFrames(const PairCache& c);
        /// The lengths "more candidates" would ask for: the selected answer's tokens - 2 and + 2 that are inside 6..16, not
        /// held already and fit in the three a segment keeps. Returns how many (0..2).
        static S32       moreTokens(const PairCache& c, S32 (&out)[2]);
        S32              totalFor(const std::vector<KeyId>& order) const;
        static void      restoreAfterStop(PairCache& c);
        bool             hasQueued(PairId pair) const;
        void             eraseQueued(PairId pair);
        void             restoreQueued();
        void             settleJob(PairId pair, PairCache& c, JobError error);
        bool             samePair(S32 index);          // keys index and index + 1 are equal (cached by pose_ver)
        TimelineResult   queueOne(PairId pair);        // requestGenerate without the view
        bool             fitPair(const SomaRetarget& retarget, const std::vector<KeyPose>& keys, U32 layout_version, S32 index,
                                 PairCache& c);
        // ---- passing through ----
        enum class Wait : U8 { Ready, Waits, Missing };
        /// Whether the segment that starts at key `from_index` can be asked now (see nextJob).
        Wait             waitFor(S32 from_index) const;
        /// The segment before key `index` has an answer in use (selected, and the clip is made of it).
        bool             prevUsable(S32 index) const;
        /// The segment that starts at key `from_index` has what it is asked with: the key is not passed, or the segment
        /// before it has an answer in use or is queued or running (the one rule of requestGenerate, requestMoreAnswers and
        /// canRequestMoreAnswers).
        bool             prevAvailable(S32 from_index) const;
        /// What requestGenerate would say for the pair (Ok when it queues it). prev_queued: the segment before it is
        /// queued in the same call (requestChain).
        TimelineResult   canQueue(PairId pair, bool prev_queued) const;
        /// The last segment (its from key's index) of the run that starts at `from_index`: each segment after the first
        /// starts at a passed key (the one set of chainFrom and dropChain).
        S32              chainLast(S32 from_index) const;
        /// The queue entries of the segment starting at `from_index`, and of the ones passed after it, are taken out and
        /// the segments go back to S0 / S3 with the note PrevMissing.
        void             dropChain(S32 from_index);
        /// The job: context and targets from the key poses and the previous segment's clip; false when a pose cannot be sent.
        bool             composeJob(const SomaRetarget& retarget, S32 from_index, GenerateJob& job, SentRecord& sent) const;
        /// After a successful refresh: pass_effective / pass_auto of every key, then the regeneration marks.
        void             updatePassFlags(const SomaRetarget& retarget);
        void             updateRegenMarks();
        PassEligibility  eligibilityAt(S32 index) const;

        std::vector<TimelineKey>  mKeys;
        KeyId                     mNextId = 1;
        std::map<PairId, PairCache> mPairs;
        std::vector<SegmentInfo>  mSegments;
        std::vector<Queued>       mQueue;
        Running                   mRunning;

        TrackLayout               mLayout;
        std::vector<KeyPose>      mKeyPoses;
        std::vector<S32>          mKeyFrames;
        U32                       mLayoutVersion = 0;
        U32                       mClipVersion   = 0;
        bool                      mPassEnabled   = false;  // see setPassingEnabled
        U32                       mNextSerial    = 1;  // SegmentAnswer::serial
        U32                       mSkeletonGen   = 0;  // grows when the rest skeleton changes (invalidateClips)
        MotionClip                mClip;
        S32                       mTotalFrames   = 0;
        F64                       mLoopHold      = LOOP_HOLD_SEC;
        bool                      mStale         = true;
        TrackResult               mTrackResult   = TrackResult::Ok;
        bool                      mKeysChanged   = true;  // a key was added, removed, moved or its pose replaced
        KeyId                     mLoopFirstId = NO_KEY, mLoopLastId = NO_KEY;  // what mLoopHold was worked out for
        U32                       mLoopFirstVer = 0, mLoopLastVer = 0;
    };

    // ---- what the window shows of a key's passing (research/361 sections 1.2 and 1.3; the rule is tested here) --------
    /// The one line under the controls.
    enum class PassNote : U8
    {
        None,                 // no key
        StopHint,             // passed (or Auto without a judgement now): "to stop at it, duplicate it"
        PassHint,             // Auto judged "stop": "to go through it, choose Pass Through"
        EndKey,               // why it cannot be passed (PassEligibility)
        NeighbourNotGenerate,
        Hold,
        SamePose,
    };
    struct PassView
    {
        KeyId        key      = NO_KEY;
        PassMode     mode     = PassMode::Auto;
        PassStrength strength = PassStrength::Normal;
        S8           judged   = -1;    // the automatic judgement: 1 passes, 0 stops, -1 none (no key, not Auto, not eligible, stale)
        bool         mode_enabled     = false;  // eligible
        bool         strength_enabled = false;  // eligible and passed (Through, or Auto judged "passes")
        PassNote     note     = PassNote::None;
        bool operator==(const PassView&) const = default;
    };
    /// The view of key `id` (a key that is not there, or none: the default view). The judgement is the last successful refresh's
    /// and is not shown while the joined clip is stale.
    PassView passViewOf(const MotionTimeline& timeline, KeyId id);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONTIMELINE_H
