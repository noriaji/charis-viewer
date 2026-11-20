/**
 * @file motionbvh.h
 * @brief "Motion Between Poses" (research/324 section 2.8): the MOTION part of a
 * BVH file with every frame of a clip, for the joints and channels of a
 * HIERARCHY the poser wrote. The values only (no viewer state), so it is tested
 * on its own (motionbvh_test.cpp).
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

// The hierarchy is the poser's own (FSFloaterPoser::writeBvhFragment: its joints, offsets and each joint's
// channel order), read back from what it wrote (readBvhHierarchy), so the motion always follows the channels the
// file says. The values are what SL's BVH loader (indra/llcharacter/llbvhloader.cpp) turns back into the clip:
//   - it reads a joint's angles by the names of its channels, and builds the rotation with mayaQ() in the reversed
//     channel order (bvhStringToOrder), i.e. the last channel's turn first;
//   - it turns that from the BVH frame into SL's with the frame matrix of makeTranslation() (BVH x, y, z are SL's
//     y, z, x): SL = frame^-1 * BVH * frame;
//   - frame 0 is the reference (all 0) and is not played; the pelvis' rotation and position are relative to it.
// So each frame holds the clip's local rotation (the whole target, as the poser draws it) in degrees per channel,
// and the root's position is the pelvis' offset from the rest pose in inches, axes as the poser writes them
// (FSFloaterPoser::positionToString). Frame Time is 1 / CLIP_FPS: Frames N + 1 for a clip of N frames, so the
// upload plays (N - 1) / CLIP_FPS seconds, the clip's durationSec().

#ifndef PLUS_POSERMOTION_MOTIONBVH_H
#define PLUS_POSERMOTION_MOTIONBVH_H

#include "motionclip.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace plus::posermotion
{
    inline constexpr F32 BVH_INCHES_PER_M = 39.37008f; // as FSFloaterPoser::positionToString
    inline constexpr S32 BVH_MAX_JOINTS   = 512;       // a hierarchy with more is refused

    struct BvhChannel
    {
        enum class Kind : U8
        {
            Position,
            Rotation,
        };
        Kind kind = Kind::Rotation;
        U8   axis = 0; // 0 X, 1 Y, 2 Z (the BVH's axes)
    };

    /// A joint of the hierarchy (ROOT or JOINT; End Site is not one) and its channels, in the file's order.
    struct BvhJoint
    {
        std::string             name;
        std::vector<BvhChannel> channels;
    };

    /// The joints of a HIERARCHY text, in the order its MOTION lines list them. False (out cleared) when a joint has
    /// no CHANNELS line, a channel is not one of the six names, there are more than BVH_MAX_JOINTS joints, or the
    /// text has no ROOT.
    bool readBvhHierarchy(std::istream& in, std::vector<BvhJoint>& out);

    /// The angles (degrees) about the BVH's X, Y and Z (by axis, not by channel) that SL's loader, with the
    /// rotation channels in this order (three distinct axes), turns back into the SL local rotation q.
    LLVector3 bvhAnglesDegrees(const LLQuaternion& q, const BvhChannel rotation_order[3]);

    /// The joints' rotations in the clip: for each joint of the hierarchy, the clip's track: a RestSkeleton index
    /// (MotionClip::local[frame * joint_count + index]), or else an extra rotation track
    /// (MotionClip::extra_rot[frame * extra_rot_count + index]), or neither (-1 and -1: written 0).
    struct BvhClipTracks
    {
        std::vector<S32> rest_of_joint;      // the same size as the hierarchy's joints; -1: not a rest-skeleton track
        std::vector<S32> extra_rot_of_joint; // the same size; -1: no extra track (used when rest_of_joint is -1)
        bool             root_position = false; // the root's position channels hold the pelvis' offset (else 0)
        LLVector3        pelvis_rest;           // the root's position is MotionClip::pelvis - this
    };

    /// Writes "MOTION", "Frames", "Frame Time", the reference frame and the clip's frames. False when the tracks do
    /// not fit the hierarchy or the clip, or a value is not finite (nothing is written then).
    bool writeBvhMotion(std::ostream& out, const std::vector<BvhJoint>& joints, const BvhClipTracks& tracks,
                        const MotionClip& clip);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONBVH_H
