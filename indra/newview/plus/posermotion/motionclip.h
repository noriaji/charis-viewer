/**
 * @file motionclip.h
 * @brief "Motion Between Poses" (research/324): the clip made from the model's
 * answer (SOMA to SL, the ends fitted to the two poses, the joints the model
 * does not move), and the time of the playback. The maths only, no viewer state.
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

// Rotations follow the viewer (row vectors, world = local * parent), as in somaretarget.h.
// The Python twin is tools/motionbricks-eval/sl_roundtrip.py (endpoint_fix, base_locals) in the sl-viewer
// repository; somaretarget_test.cpp checks the two agree on golden/clips.json.

#ifndef PLUS_POSERMOTION_MOTIONCLIP_H
#define PLUS_POSERMOTION_MOTIONCLIP_H

#include "somaretarget.h"

#include <vector>

namespace plus::posermotion
{
    inline constexpr S32 CLIP_FPS        = 30; // the model's frames per second
    inline constexpr S32 CLIP_MIN_FRAMES = 18; // 6 tokens * 4 frames - 6 (the model's shortest answer)
    inline constexpr S32 CLIP_MAX_FRAMES = 58; // 16 tokens * 4 frames - 6 (its longest)
    /// The joined clip of the timeline (research/357): its own limits, apart from the model's answers above.
    inline constexpr S32 TIMELINE_MIN_FRAMES = 2;
    inline constexpr S32 TIMELINE_MAX_FRAMES = 901; // 30 s and the frame 0
    /// The frames over which the start and the end are fitted to the two poses (at most half of the clip).
    inline constexpr S32 EDGE_FRAMES     = 12;
    /// With looping, the last frame is held this long before the clip starts again (to show the end pose).
    inline constexpr F64 LOOP_HOLD_SEC   = 0.5;

    /// A pose the clip starts or ends at: the skeleton (SlPose, RestSkeleton order) and the tracks the model
    /// knows nothing of, given by the caller in an order of its own (rotations: e.g. collision volumes;
    /// vectors: positions and scales). The extra tracks must have the same sizes in the start and the end.
    struct KeyPose
    {
        SlPose                    sl;
        std::vector<LLQuaternion> extra_rot;
        std::vector<LLVector3>    extra_vec;
    };

    /// The clip at CLIP_FPS: frame f's rotation of joint j is local[f * joint_count + j], and so on.
    struct MotionClip
    {
        S32                       frame_count     = 0;
        S32                       joint_count     = 0;
        S32                       extra_rot_count = 0;
        S32                       extra_vec_count = 0;
        std::vector<LLQuaternion> local;
        std::vector<LLVector3>    pelvis;
        std::vector<LLQuaternion> extra_rot;
        std::vector<LLVector3>    extra_vec;

        bool empty() const { return frame_count <= 0; }
        /// From the first to the last frame, in seconds.
        F64  durationSec() const { return frame_count > 1 ? (F64)(frame_count - 1) / CLIP_FPS : 0.0; }
    };

    /// How far the model's ends were from the two poses before they were fitted (for the diagnostic log).
    struct ClipStats
    {
        F32 start_max_deg     = 0.f; // the driven joints' largest difference at the first frame
        F32 end_max_deg       = 0.f; // ... at the last frame
        F32 start_pelvis_m    = 0.f;
        F32 end_pelvis_m      = 0.f;
        S32 edge_frames       = 0;   // the frames the fitting used (EDGE_FRAMES, or half of a short clip)
    };

    /// Makes the clip: each frame of the answer SOMA to SL (the joints the model does not move go from the start
    /// to the end on an ease-in-out curve and are given to somaToSl), then the driven joints and the pelvis are
    /// fitted so that the first frame is the start and the last the end (over the edge frames, a cosine to 0),
    /// the extra tracks go on the same curve as the undriven joints, and the signs of each track's rotations are
    /// kept on one side frame to frame (no long way round when playing). Called once per answer (it allocates).
    /// out is changed only when Ok.
    RetargetResult buildClip(const SomaRetarget& retarget, const KeyPose& start, const KeyPose& end,
                             const std::vector<SomaFrame>& answer, MotionClip& out, ClipStats* stats = nullptr);

    /// How a key pose goes into an interpolated segment.
    enum class InterpCurve
    {
        EaseInOut, // slow at both ends (the curve the undriven joints of a model clip use)
        Linear,    // straight through
    };

    /// A segment without the model: every joint, the pelvis and the extra tracks go from start to end over
    /// `frames` frames (TIMELINE_MIN_FRAMES..TIMELINE_MAX_FRAMES; the first frame is start, the last end),
    /// spherical for rotations, one side of the sphere frame to frame. start and end must be sized alike.
    /// out is changed only when Ok.
    RetargetResult makeInterpClip(const KeyPose& start, const KeyPose& end, S32 frames, InterpCurve curve,
                                  MotionClip& out);

    /// One pose t of the way from a to b (t may pass 1: the same curve goes on): every rotation spherical, the short way
    /// round and on a's side of the sphere, the pelvis and the extra vectors linear. The same blend as every frame of
    /// makeInterpClip. a and b must be sized alike (BadInput otherwise, or for a degenerate rotation / t not a number).
    /// out is changed only when Ok.
    RetargetResult blendKeyPose(const KeyPose& a, const KeyPose& b, F64 t, KeyPose& out);

    /// The last `count` frames of the clip as SL poses, in time order (the last one is the clip's last frame). False (out
    /// untouched) when count < 1, the clip is empty or inconsistent, or it has fewer frames.
    bool tailPoses(const MotionClip& clip, S32 count, std::vector<SlPose>& out);

    /// Whether the speed at the end of the clip is not the one in `asked` (two SL poses in time order, the last two frames of
    /// the clip it was taken from): the step of a joint over the last two frames (the turn from the frame before the last
    /// to the last) and the step between the two poses differ by more than max_deg at some joint, or the pelvis's step by more
    /// than max_pelvis_m. True too when the sizes do not match or the clip has no two frames (it differs).
    bool tailStepDiffers(const std::vector<SlPose>& asked, const MotionClip& clip, F32 max_deg, F32 max_pelvis_m);

    enum class ConcatResult
    {
        Ok,
        NoClips,
        Mismatch, // the clips do not have the same joint or extra track counts, or one is empty or inconsistent
        BadHold,  // holds is not one more than the clips, or one is negative
        TooLong,  // more than TIMELINE_MAX_FRAMES
    };

    /// Joins the clips in a row: clip i ends at the pose clip i + 1 starts at, so that frame is taken once (the
    /// first frame of every clip but the first is dropped). holds[k] (clips.size() + 1 values, in frames) repeats
    /// the frame of key k that many times after it (key 0: the first frame of clips[0]; key k > 0: the last of
    /// clips[k-1]). The signs of each track are kept on one side over the whole. out is changed only when Ok.
    ConcatResult concatClips(const std::vector<const MotionClip*>& clips, const std::vector<S32>& holds, MotionClip& out);

    enum class LoopMode
    {
        Once,
        Loop,
    };

    /// Where the playback is: between frame and next, blend from 0 (frame) to 1 (next).
    struct PlayCursor
    {
        S32  frame    = 0;
        S32  next     = 0;
        F32  blend    = 0.f;
        bool finished = false; // LoopMode::Once only: at or past the last frame (an empty clip: at once)
    };

    /// The cursor elapsed_sec after the start of the playback (real time, F64; negative or NaN as 0, +infinity as
    /// the last frame). LoopMode::Once is finished at or past the last frame, and at once for an empty clip.
    /// loop_hold_sec: how long the last frame is held before a loop starts again (0 for a clip whose last frame is its
    /// first; negative or not finite: 0).
    PlayCursor cursorAt(S32 frame_count, F64 elapsed_sec, LoopMode mode, F64 loop_hold_sec = LOOP_HOLD_SEC);
    /// The cursor on a frame (the needle), clamped to the clip.
    PlayCursor cursorAtFrame(S32 frame_count, S32 frame);

    /// Gives out the sizes of the clip's pose once (it allocates if they differ), so that samplePose does not.
    void sizePoseFor(const MotionClip& clip, KeyPose& out);
    /// The pose at the cursor (spherical between the two frames). Every frame of the playback: no allocation
    /// when out was sized by sizePoseFor; false (out untouched) if the clip is empty or inconsistent, or out has
    /// other sizes.
    bool samplePose(const MotionClip& clip, const PlayCursor& cursor, KeyPose& out);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONCLIP_H
