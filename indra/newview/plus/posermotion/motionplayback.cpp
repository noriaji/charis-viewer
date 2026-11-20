/**
 * @file motionplayback.cpp
 * @brief "Motion Between Poses": the clip's playback states (C0 .. C2).
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

#include "motionplayback.h"

#include <algorithm>
#include <cmath>

namespace plus::posermotion
{
    namespace
    {
        constexpr F64 START_BIAS_FRAMES = 1e-3;
    }

    void ClipPlayback::reset()
    {
        mState      = ClipState::None;
        mFrameCount = 0;
        mFrame      = 0;
        mStartSec   = 0.0;
    }

    bool ClipPlayback::load(S32 frame_count, S32 min_frames, S32 max_frames)
    {
        reset();
        if (frame_count < std::max(min_frames, 1) || frame_count > max_frames)
        {
            return false;
        }
        mFrameCount = frame_count;
        mState      = ClipState::Stopped;
        return true;
    }

    bool ClipPlayback::reload(S32 frame_count, S32 min_frames, S32 max_frames)
    {
        if (mState == ClipState::None)
        {
            return false;
        }
        const S32 keep = mFrame;
        if (!load(frame_count, min_frames, max_frames))
        {
            return false;
        }
        mFrame = std::clamp(keep, 0, mFrameCount - 1);
        return true;
    }

    bool ClipPlayback::play(F64 now_sec)
    {
        if (mState != ClipState::Stopped || !std::isfinite(now_sec))
        {
            return false;
        }
        if (mFrame >= mFrameCount - 1)
        {
            mFrame = 0; // from the end: again from the start
        }
        // a thousandth of a frame early, so that the first advance() lands on the frame shown, not on the one before
        // it by a rounding of the clock's large value
        mStartSec = now_sec - ((F64)mFrame + START_BIAS_FRAMES) / CLIP_FPS;
        mState    = ClipState::Playing;
        return true;
    }

    void ClipPlayback::stop()
    {
        if (mState == ClipState::Playing)
        {
            mState = ClipState::Stopped;
        }
    }

    bool ClipPlayback::seek(S32 frame)
    {
        if (mState == ClipState::None)
        {
            return false;
        }
        mFrame = std::clamp(frame, 0, mFrameCount - 1);
        mState = ClipState::Stopped;
        return true;
    }

    PlayCursor ClipPlayback::advance(F64 now_sec, LoopMode mode, F64 loop_hold_sec)
    {
        if (mState != ClipState::Playing)
        {
            return shown();
        }
        const PlayCursor c = cursorAt(mFrameCount, now_sec - mStartSec, mode, loop_hold_sec);
        mFrame = c.frame;
        if (c.finished)
        {
            mFrame = mFrameCount - 1;
            mState = ClipState::Stopped;
        }
        return c;
    }
} // namespace plus::posermotion
