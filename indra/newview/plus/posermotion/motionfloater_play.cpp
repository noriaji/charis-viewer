/**
 * @file motionfloater_play.cpp
 * @brief "Motion Between Poses": machine C (the playback of the joined clip), the writes to the avatar, the ways out (dropClip, dropAll).
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

// The clip played is the timeline's joined clip (mTimeline.clip(), one for all segments and holds). It is loaded into the
// playback only when the user plays, seeks or drags the strip's needle (preparePlayback), and follows every change of the keys
// (syncPlayback, after the playback was stopped: research/357 section 4.2). While motions are generated nothing plays.

#include "llviewerprecompiledheaders.h"

#include "motionfloater_impl.h"

#include "lltimer.h"

namespace plus::posermotion
{
    namespace floaterui
    {
        // ---------------------------------------------------------------- loading the clip into the playback
        /// The playback is ready for the joined clip as it is now (the keys' joints resolved, the clip loaded), or false.
        bool MotionFloater::preparePlayback()
        {
            if (selfPoseState() != SelfPoseState::Posing || mTimeline.keyCount() < 2 || mTimeline.busy())
            {
                return false;
            }
            if (!mPlayback.hasClip() && !refreshTimeline(true))
            {
                return false; // a playback begins from the skeleton as it is now
            }
            if (mTimeline.stale() && !refreshTimeline(false))
            {
                return false;
            }
            if (mTimeline.stale())
            {
                return false;
            }
            if (mPlayback.hasClip() && mLoadedClip == mTimeline.clipVersion() && mLoadedLayout == mTimeline.layoutVersion()
                && !mJoints.empty())
            {
                return true;
            }
            return loadPlayback();
        }

        bool MotionFloater::loadPlayback()
        {
            const MotionClip& clip = mTimeline.clip();
            if (clip.frame_count < TIMELINE_MIN_FRAMES)
            {
                return false;
            }
            if (mJoints.empty() || mLoadedLayout != mTimeline.layoutVersion())
            {
                mJoints.clear();
                if (!resolveSelfJoints(mTimeline.layout(), mJoints))
                {
                    return false;
                }
                mLoadedLayout = mTimeline.layoutVersion();
            }
            sizePoseFor(clip, mFrameKey);
            const bool loaded = mPlayback.hasClip() ? mPlayback.reload(clip.frame_count)
                                                    : mPlayback.load(clip.frame_count, TIMELINE_MIN_FRAMES, TIMELINE_MAX_FRAMES);
            if (!loaded)
            {
                return false;
            }
            mFrameShown = -1;
            mLoadedClip = mTimeline.clipVersion();
            return true;
        }

        /// The joined clip changed (the playback is stopped): a loaded playback follows it, at the same frame.
        void MotionFloater::syncPlayback()
        {
            if (mPlayback.hasClip() && (mTimeline.stale() || !loadPlayback()))
            {
                dropClip();
            }
        }

        // ---------------------------------------------------------------- machine C
        WriteResult MotionFloater::writeShown(WriteCheck check)
        {
            if (!samplePose(mTimeline.clip(), mPlayback.shown(), mFrameKey))
            {
                return WriteResult::Gone;
            }
            return writeSelfFrame(mJoints, mTimeline.layout(), mFrameKey, check);
        }

        void MotionFloater::onPlay()
        {
            if (mPlayback.playing())
            {
                stopPlayback();
            }
            else if (preparePlayback())
            {
                endSeekEdit();
                std::optional<history::Pose> offered = offerBefore(); // before the first frame is written
                if (mPlayback.play(LLTimer::getTotalSeconds()))
                {
                    // what the joints hold now is the playback's start: changes made in C1 are not "touched"
                    for (ResolvedJoint& joint : mJoints.joints)
                    {
                        joint.has_last = false;
                    }
                    mBefore.commit(offered);
                    mEdit.begin();
                    mFrameHook.start(&MotionFloater::onIdleFrame, this);
                }
                offered.reset();
            }
            refresh(selfPoseState());
        }

        /// C2, every frame: the frame of the real time, written unless somebody else moved a joint meanwhile.
        void MotionFloater::playFrame()
        {
            const LoopMode mode = mLoopOn ? LoopMode::Loop : LoopMode::Once;
            const PlayCursor cursor = mPlayback.advance(LLTimer::getTotalSeconds(), mode, mTimeline.loopHoldSec());
            WriteResult result = WriteResult::Gone;
            if (samplePose(mTimeline.clip(), cursor, mFrameKey))
            {
                result = writeSelfFrame(mJoints, mTimeline.layout(), mFrameKey, WriteCheck::IfUntouched);
            }
            switch (result)
            {
            case WriteResult::Written:
                mEdit.hold(); // the frame's label follows on the window's tick (no words made here)
                mTimelineView->setPlayheadSec((F64)mPlayback.frame() / (F64)CLIP_FPS);
                if (!mPlayback.playing())
                {
                    stopPlayback(); // the end (not looping): C1 at the last frame
                }
                break;
            case WriteResult::Touched:
                stopPlayback();
                setNote(Note::Touched);
                break;
            case WriteResult::Gone:
                dropAll();
                setNote(Note::Gone);
                break;
            }
        }

        void MotionFloater::stopPlayback()
        {
            if (!mFrameHook.active())
            {
                mPlayback.stop();
                return; // not playing (C0, C1): nothing to end
            }
            mPlayback.stop();
            mFrameHook.stop();
            mEdit.end("Motion Between Poses: played", LLTimer::getTotalSeconds());
            refreshPoserWindow(); // once, now that the joints rest
            refreshFrame();
        }

        /// One frame of the joined clip on the avatar (the strip's needle); one entry of the pose history for a whole drag
        /// (endSeekEdit).
        void MotionFloater::seekTo(S32 frame)
        {
            if (mPlayback.playing())
            {
                stopPlayback();
            }
            if (frame == mPlayback.frame() && mSeekEdit)
            {
                return;
            }
            std::optional<history::Pose> offered = offerBefore(); // before the first frame is written
            mEdit.begin();
            mSeekEdit = true;
            mPlayback.seek(frame);
            if (writeShown(WriteCheck::Always) == WriteResult::Gone)
            {
                offered.reset();
                dropAll();
                setNote(Note::Gone);
                return;
            }
            mBefore.commit(offered);
            refreshFrame();
            mTimelineView->setPlayheadSec((F64)mPlayback.frame() / (F64)CLIP_FPS);
        }

        void MotionFloater::endSeekEdit()
        {
            if (mSeekEdit)
            {
                mSeekEdit = false;
                mEdit.end("Motion Between Poses: frame", LLTimer::getTotalSeconds());
                refreshPoserWindow();
            }
        }

        /// C0, keeping the pose from before (the keys and the timeline's clip stay: they are not the playback's).
        void MotionFloater::dropClip()
        {
            stopPlayback();
            endSeekEdit();
            mEdit.releaseSnap(); // no clip, nothing to draw at once
            mPlayback.reset();
            mJoints.clear();
            mFrameShown = -1;
        }

        /// C0, and the pose from before is forgotten (discarded, the window closes, posing stopped, the avatar gone).
        void MotionFloater::dropAll()
        {
            dropClip();
            mBefore.drop();
        }
    } // namespace floaterui
} // namespace plus::posermotion
