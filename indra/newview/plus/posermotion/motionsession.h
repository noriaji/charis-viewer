/**
 * @file motionsession.h
 * @brief "Motion Between Poses" (research/324 section 5): the window's rules
 * that need no viewer: what to drop when the poser or the avatar goes away, the
 * pose from before the first "generate", and one change of the pose made from
 * the window (its history entry and the poser's easing). Tested on its own
 * (motionui_test.cpp).
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

#ifndef PLUS_POSERMOTION_MOTIONSESSION_H
#define PLUS_POSERMOTION_MOTIONSESSION_H

#include "stdtypes.h"

#include <optional>
#include <utility>

namespace plus::posermotion
{
    /// After an edit of the pose ended, the poser keeps drawing the targets at once this long (so that the last
    /// frame written is drawn as written too).
    inline constexpr F64 SNAP_HOLD_SEC = 0.25;

    /// Whether the own avatar can be posed through the window (worst first).
    enum class SelfPoseState
    {
        NotLoggedIn,   // no own avatar (before login, logging out)
        NoPoser,       // the poser window is not open
        NotPosing,     // the poser is not posing the own avatar (its posing motion is gone or stopped)
        OtherSelected, // the poser has an animesh (or someone else) selected: this window works on the own avatar only
        Posing,
    };

    /// What the window's tick sees.
    struct FollowInput
    {
        SelfPoseState state        = SelfPoseState::NotLoggedIn;
        bool          has_clip     = false;
        bool          joints_alive = false; // the clip's resolved joints still belong to the running motion
        bool          has_before   = false;
        bool          has_keys     = false; // a key is taken
        bool          request_out  = false; // motions are queued or being made (one is out at the helper, or waits to be sent)
    };

    /// What it must do about it (research/324 section 5.3, "the other side is gone").
    struct FollowActions
    {
        bool stop_helper    = false; // logged out: the helper ends (a download goes on)
        bool cancel_request = false; // nothing to put the answer on any more
        bool drop_all       = false; // the clip (C0), its playback and the pose from before; the keys stay (the window cancels the generate queue itself, on cancel_request)
        bool drop_keys      = false; // the key list and what was made for it: logout or the own avatar gone only
        bool note_gone      = false; // say why the clip went
    };

    inline bool poserGone(SelfPoseState state)
    {
        return state == SelfPoseState::NotLoggedIn || state == SelfPoseState::NoPoser || state == SelfPoseState::NotPosing;
    }

    inline FollowActions followActions(const FollowInput& in)
    {
        FollowActions out;
        const bool gone    = poserGone(in.state);
        out.stop_helper    = in.state == SelfPoseState::NotLoggedIn;
        out.cancel_request = gone && in.request_out;
        if (in.has_clip && (gone || !in.joints_alive))
        {
            out.drop_all  = true;
            out.note_gone = true;
        }
        else if (gone && in.has_before)
        {
            out.drop_all = true;
        }
        out.drop_keys = in.state == SelfPoseState::NotLoggedIn && in.has_keys; // the poser closing or stopping keeps them
        return out;
    }

    /// The pose from before the window first wrote to the own avatar (research/357 section 4.2 mBefore): taken just before
    /// the first write (a key put on the avatar, the playback, a frame), offered with commit() and kept only when that write
    /// went on. A write that did not leave nothing to restore.
    template <typename Pose>
    class BeforeKeeper
    {
    public:
        bool        has() const { return mBefore.has_value(); }
        const Pose& get() const { return *mBefore; }
        /// Before a write to the avatar: true when the pose now is to be taken and offered (none is kept yet).
        bool        wanted() const { return !mBefore; }
        /// The write went on: the offered pose becomes the one to restore if there is none yet.
        void        commit(std::optional<Pose>& offered)
        {
            if (!mBefore && offered)
            {
                mBefore = std::move(*offered);
            }
            offered.reset();
        }
        void        drop() { mBefore.reset(); }

    private:
        std::optional<Pose> mBefore;
    };

    /// How an answer was dealt with (MotionFloater::takeAnswer).
    enum class AnswerOutcome
    {
        NotForSelf, // the own avatar was not the one selected (or posed) when it came: not put on anyone
        NotAClip,   // it could not be made a clip
        Gone,       // the joints went before its first frame was written (or while writing it)
        Applied,    // its first frame is on the avatar
    };

    /// The pose kept with a request, once its answer was dealt with: the pose to restore only when the clip went on
    /// (Applied); for any other outcome it goes with the request. offered is empty afterwards in every case.
    template <typename Pose>
    void settleBefore(AnswerOutcome outcome, BeforeKeeper<Pose>& keeper, std::optional<Pose>& offered)
    {
        if (outcome == AnswerOutcome::Applied)
        {
            keeper.commit(offered);
        }
        offered.reset();
    }

    /// One change of the pose made from the window (the first frame, the playback, the needle): one entry of the pose
    /// history and the poser drawing the targets at once, until SNAP_HOLD_SEC after it ended. Backend:
    ///   bool beginRecord(); void holdRecord(); void endRecord(const char* summary);  (the pose history)
    ///   bool startSnap();   void stopSnap();                                         (the poser's easing)
    template <typename Backend>
    class PoseEditT
    {
    public:
        PoseEditT() = default;
        ~PoseEditT()
        {
            end("Motion Between Poses", 0.0);
            releaseSnap();
        }
        PoseEditT(const PoseEditT&)            = delete;
        PoseEditT& operator=(const PoseEditT&) = delete;

        /// Opens it (nothing when it is open), and starts the snapping if it is not on.
        void begin()
        {
            if (!mOpen)
            {
                mRecording = mBackend.beginRecord();
                mOpen      = true;
            }
            if (!mSnapping)
            {
                mSnapping = mBackend.startSnap();
            }
        }
        void hold()
        {
            if (mOpen && mRecording)
            {
                mBackend.holdRecord();
            }
        }
        void end(const char* summary, F64 now_sec)
        {
            if (!mOpen)
            {
                return;
            }
            mOpen = false;
            if (mRecording)
            {
                mRecording = false;
                mBackend.endRecord(summary);
            }
            mSnapUntil = now_sec + SNAP_HOLD_SEC;
        }
        /// The window's tick: the snapping ends SNAP_HOLD_SEC after the edit did.
        void releaseSnapWhenDue(F64 now_sec)
        {
            if (!mOpen && mSnapping && now_sec >= mSnapUntil)
            {
                releaseSnap();
            }
        }
        void releaseSnap()
        {
            if (mSnapping)
            {
                mSnapping = false;
                mBackend.stopSnap();
            }
        }
        bool open() const { return mOpen; }
        bool snapping() const { return mSnapping; }
        Backend&       backend() { return mBackend; }
        const Backend& backend() const { return mBackend; }

    private:
        Backend mBackend;
        bool    mOpen      = false;
        bool    mRecording = false;
        bool    mSnapping  = false;
        F64     mSnapUntil = 0.0; // real time
    };
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONSESSION_H
