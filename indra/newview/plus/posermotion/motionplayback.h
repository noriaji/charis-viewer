/**
 * @file motionplayback.h
 * @brief "Motion Between Poses" (research/324 section 5.1, C): whether there is
 * a clip, whether it plays, and which frame it shows. The states only (the
 * window writes the poses), so it is tested on its own.
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

// C0 None: no clip. C1 Stopped: a clip, showing frame(). C2 Playing: a clip, frames from the real time (F64,
// LLTimer::getTotalSeconds()) since the start, so any frame rate shows the same frame at the same time
// (research/324 section 5.4). Every way out of Playing goes through stop() or reset().

#ifndef PLUS_POSERMOTION_MOTIONPLAYBACK_H
#define PLUS_POSERMOTION_MOTIONPLAYBACK_H

#include "motionclip.h"

namespace plus::posermotion
{
    enum class ClipState
    {
        None,    // C0
        Stopped, // C1
        Playing, // C2
    };

    class ClipPlayback
    {
    public:
        ClipState state() const { return mState; }
        bool      hasClip() const { return mState != ClipState::None; }
        bool      playing() const { return mState == ClipState::Playing; }
        S32       frameCount() const { return mFrameCount; }
        /// The frame shown (C1), or the last one the playback reached (C2); 0 in C0.
        S32       frame() const { return mFrame; }

        /// C0 (no clip): from any state.
        void reset();
        /// A new clip of frame_count frames (min_frames..max_frames, the model's answer by default; others: C0 and
        /// false): C1 at frame 0.
        bool load(S32 frame_count, S32 min_frames = CLIP_MIN_FRAMES, S32 max_frames = CLIP_MAX_FRAMES);
        /// The clip was made again with frame_count frames (min_frames..max_frames): the position stays (clamped
        /// to the new clip) and a playback stops (C1). Not in C0 (false, nothing changed); outside the range: C0 and false.
        bool reload(S32 frame_count, S32 min_frames = TIMELINE_MIN_FRAMES, S32 max_frames = TIMELINE_MAX_FRAMES);
        /// C1 -> C2 from the frame shown (from 0 when it shows the last frame). False in another state.
        bool play(F64 now_sec);
        /// C2 -> C1 at the frame reached. Nothing in another state.
        void stop();
        /// The needle: C1 or C2 -> C1 at frame (clamped). False in C0.
        bool seek(S32 frame);

        /// C2: where the playback is at now_sec. Once, at or past the last frame: C1 at the last frame (the cursor's
        /// finished is true; the caller writes that frame and stops writing). Other states: the frame shown.
        PlayCursor advance(F64 now_sec, LoopMode mode, F64 loop_hold_sec = LOOP_HOLD_SEC);
        /// The cursor on the frame shown (C1), for writing it.
        PlayCursor shown() const { return cursorAtFrame(mFrameCount, mFrame); }

    private:
        ClipState mState      = ClipState::None;
        S32       mFrameCount = 0;
        S32       mFrame      = 0;
        F64       mStartSec   = 0.0; // real time of frame 0 of this playback (C2)
    };
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONPLAYBACK_H
