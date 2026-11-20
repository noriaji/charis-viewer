/**
 * @file motionposer.h
 * @brief "Motion Between Poses" (research/324 sections 2.7, 2.8): the own
 * avatar's poser joints, resolved once and written every frame of a played
 * clip, and the clip saved as a BVH file next to the poser's poses.
 * Implemented in plus/posermcp/posertools.cpp (PlusPoserTools is the poser
 * window's friend, as for captureSelfPose / applySelfPose).
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

// applySelfPose() looks every joint up by name, builds strings, redraws the poser window and throws when nobody
// is posing: right for "discard and restore", wrong for every frame (research/287 P06). Here the joints are looked
// up once (resolveSelfJoints) and kept as the poser's own FSJointPose pointers, which live in the posing motion's
// list of joint poses. What keeps them from being written through after that list went (research/149 (E)):
//   - before every write: the poser window by its handle, then the own avatar's posing motion looked up again and
//     compared with the one resolved (pointer and id) and not stopped (resolvedMotion). Stopping the posing stops
//     the motion first (it eases out for 0.5 s before it may be removed), so a stopped motion is caught here;
//   - the window's 0.1 s tick also looks the first joint up by name again and compares the pointer
//     (resolvedJointsHold), which catches a list of joint poses made again inside the same motion;
//   - the window drops the joints on its tick when either fails.
// Pointer and id alone would not prove a rebuilt motion: they only add to the checks above.
// The own avatar's motion is looked up without the poser's "same region as the agent" test, so a teleport or a
// region crossing does not stop the playback (research/324 section 5.3).
// The writes go into each joint's target (whole local rotation, position and scale offsets) as applySelf does, and
// the poser window is redrawn only when asked (refreshPoserWindow, once when the playback stops).
// Main thread only.

#ifndef PLUS_POSERMOTION_MOTIONPOSER_H
#define PLUS_POSERMOTION_MOTIONPOSER_H

#include "motionsession.h"
#include "motiontracks.h"

#include "llhandle.h"
#include "lluuid.h"

#include <string>
#include <vector>

class FSJointPose;
class FSPosingMotion;
class LLFloater;

namespace plus::posermotion
{
    /// A joint that somebody else turned or moved by more than this since the last write stops the playback.
    inline constexpr F32 TOUCHED_ANGLE_DEG = 0.01f;
    inline constexpr F32 TOUCHED_OFFSET_M  = 1e-5f;

    /// The poser and the own avatar now (motionsession.h). Cheap: no walk over the avatars, the selected row's id
    /// compared with the agent's.
    SelfPoseState selfPoseState();

    struct ResolvedJoint
    {
        FSJointPose* pose = nullptr;        // null: a joint this skeleton does not have (skipped)
        bool         has_last = false;      // a write happened (last_* hold what the joint read back after it)
        LLQuaternion last_target;
        LLVector3    last_position;
        LLVector3    last_scale;
    };

    /// The joints of a TrackLayout (same order) in the own avatar's posing motion.
    struct ResolvedJoints
    {
        LLHandle<LLFloater>        poser;           // the poser window whose animator holds the motion
        FSPosingMotion*            motion = nullptr; // compared, never used without the checks above
        LLUUID                     motion_id;
        std::vector<ResolvedJoint> joints;

        bool empty() const { return motion == nullptr; }
        void clear()
        {
            poser.markDead();
            motion = nullptr;
            motion_id.setNull();
            joints.clear();
        }
    };

    /// Resolves the layout's joints (once per clip; it allocates and looks the names up). The joints the poser
    /// does not drive yet are turned on (the poser window is redrawn then). False (out cleared) when the own avatar
    /// is not being posed.
    bool resolveSelfJoints(const TrackLayout& layout, ResolvedJoints& out);

    /// The motion the joints belong to, when it is still the own avatar's running posing motion; null otherwise
    /// (logged out, posing stopped or started again, the window closed). Cheap: a few lookups by id. Every write.
    FSPosingMotion* resolvedMotion(const ResolvedJoints& joints);

    /// resolvedMotion(), and the first resolved joint is still the one its name finds in that motion (the window's
    /// tick, not every frame: it looks a name up).
    bool resolvedJointsHold(const ResolvedJoints& joints, const TrackLayout& layout);

    enum class WriteCheck
    {
        Always,      // a frame chosen by the user (the needle, the first frame): written over what is there
        IfUntouched, // the playback: not written when a joint changed since the last write
    };

    enum class WriteResult
    {
        Written,
        Gone,    // resolvedMotion() is null, or frame does not fit the layout: nothing written
        Touched, // IfUntouched, and something else changed a written joint: nothing written
    };

    /// Writes frame (sized for the clip, e.g. by samplePose) into the resolved joints: no name lookup, no
    /// allocation, no exception, no redraw of the poser window. Every frame of the playback.
    WriteResult writeSelfFrame(ResolvedJoints& joints, const TrackLayout& layout, const KeyPose& frame, WriteCheck check);

    /// Redraws the poser window's joint lists and sliders (after writes; not every frame).
    void refreshPoserWindow();

    enum class BvhSaveResult
    {
        Saved,
        BadName,     // not a plain file name (plus::isSafeFileStem)
        NoPoser,     // not logged in, or no poser window (its hierarchy is written by the poser)
        WriteFailed, // the file could not be written or moved into place (the old one, if any, is left)
    };

    /// True when a BVH file of that name is in the poser's folder.
    bool clipBvhExists(const std::string& name);

    /// Saves the clip as <name>.bvh in the poser's folder (<user settings>/poses), written next to it, checked,
    /// then moved over it. The hierarchy is the poser's (FSFloaterPoser::writeBvhFragment), the motion every frame
    /// of the clip (motionbvh.h) for the joints the clip writes (layout; the others stay 0, as in the playback the
    /// poser leaves them to the other animations).
    BvhSaveResult saveClipBvh(const std::string& name, const MotionClip& clip, const TrackLayout& layout);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONPOSER_H
