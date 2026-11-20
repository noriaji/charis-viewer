/**
 * @file motiontracks.h
 * @brief "Motion Between Poses" (research/324): the two poses the poser holds,
 * as the clip's key poses, and which poser joint each value of a frame goes to.
 * The values only (no viewer state), so it is tested on its own.
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

// A poser joint holds a target rotation (the whole local rotation as drawn), a position offset and a scale offset
// (from where the joint was when posing started), and whether the poser drives it (plus/posermcp/posehistory.h).
// The clip (motionclip.h) carries:
//   - the rest skeleton's joints' rotations (KeyPose::sl.local, by RestSkeleton index): every bone the poses have,
//     so that SL to SOMA sees the whole chain;
//   - the pelvis (KeyPose::sl.pelvis, in the body frame): the rest pelvis plus mPelvis' position offset;
//   - extra tracks: the rotations of written joints that are not bones of the rest skeleton (none in today's
//     poser; kept so that such a joint is written, not dropped), and the position and scale offsets that are not
//     zero in at least one of the poses.
// The written joints are those every pose (two for 324's window, one per key for the timeline) has and the poser
// drives in at least one of them; the others are left as they are while the clip plays.

#ifndef PLUS_POSERMOTION_MOTIONTRACKS_H
#define PLUS_POSERMOTION_MOTIONTRACKS_H

#include "motionbvh.h"
#include "motionclip.h"

#include "plus/posermcp/posehistory.h"

#include <string>
#include <vector>

namespace plus::posermotion
{
    inline constexpr const char* PELVIS_JOINT = "mPelvis";
    /// An offset (m, or of the scale) at most this far from 0 counts as none (the poser's float noise).
    inline constexpr F32 TRACK_ZERO_OFFSET = 1e-6f;

    /// One poser joint the clip writes, and where its values are in a KeyPose (and a MotionClip frame).
    struct TrackJoint
    {
        std::string name;
        S32  rest      = -1;    // its rotation: KeyPose::sl.local[rest]; -1: not a bone of the rest skeleton
        S32  extra_rot = -1;    // its rotation: KeyPose::extra_rot[extra_rot] (a rotating joint the skeleton lacks)
        S32  position  = -1;    // its position offset: KeyPose::extra_vec[position]; -1: none (or the pelvis track)
        S32  scale     = -1;    // its scale offset: KeyPose::extra_vec[scale]; -1: none
        bool pelvis    = false; // mPelvis: its position offset is KeyPose::sl.pelvis - TrackLayout::pelvis_rest
        bool enabled   = false; // the poser drives it in the start pose (the writer turns it on if not)
    };

    struct TrackLayout
    {
        std::vector<TrackJoint> joints;
        LLVector3               pelvis_rest; // SomaRetarget::pelvisRest() of the clip

        void clear() { joints.clear(); pelvis_rest.clear(); }
    };

    enum class TrackResult
    {
        Ok,
        NotReady, // the SomaRetarget is not initialised
        NoJoints, // the two poses have no joint in common that the poser drives
        BadPose,  // a rotation is degenerate or not finite, or an offset is out of range
    };

    const char* describe(TrackResult result);

    /// The layout and the key poses of N poses (the timeline's keys, at least 2): the written joints are those
    /// every pose has and the poser drives in at least one of them (in the first pose's order; the rotation,
    /// enabled and pelvis rules are the first pose's), a position or scale track exists when it is not zero in at
    /// least one pose. Called when a key changes (it allocates). The outputs are changed only when Ok.
    TrackResult makeKeyPoses(const SomaRetarget& retarget, const std::vector<const posermcp::history::Pose*>& poses,
                             TrackLayout& layout, std::vector<KeyPose>& keys);

    /// The same for two poses (captureSelfPose; this is the form 324's window uses): the layout and the two key poses. The outputs are changed only
    /// when Ok.
    TrackResult makeKeyPoses(const SomaRetarget& retarget, const posermcp::history::Pose& start,
                             const posermcp::history::Pose& end, TrackLayout& layout, KeyPose& start_key,
                             KeyPose& end_key);

    /// One joint's values in a key pose (or a sampled frame) of the layout.
    struct TrackValues
    {
        bool         has_rotation = false;
        LLQuaternion rotation;              // the target (whole local) rotation
        bool         has_position = false;
        LLVector3    position;              // the position offset
        bool         has_scale    = false;
        LLVector3    scale;                 // the scale offset
    };

    /// The values of layout.joints[index] in key (sized as the clip's frames). Every frame of the playback: no
    /// allocation. False when index or key do not fit the layout.
    bool trackValues(const TrackLayout& layout, S32 index, const KeyPose& key, TrackValues& out);

    /// The BVH tracks of a clip of this layout: for each joint of the hierarchy, the clip's rotation track of the joint
    /// when the layout writes its rotation (TrackJoint::rest, or TrackJoint::extra_rot for a rotating joint the rest
    /// skeleton lacks), else none (written 0: a joint the playback does not write either, left to the other
    /// animations). The root's position is the clip's pelvis less the rest pelvis when the layout writes mPelvis'
    /// position (TrackJoint::pelvis), else 0. The BVH holds no other position or scale offsets, though the playback
    /// writes them: SL's loader reads positions of any joint with 6 channels (llbvhloader.cpp, SL-318), but the
    /// poser's hierarchy (FSFloaterPoser::writeBvhFragment) gives 6 channels to the root only.
    BvhClipTracks bvhTracksOf(const std::vector<BvhJoint>& joints, const TrackLayout& layout);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONTRACKS_H
