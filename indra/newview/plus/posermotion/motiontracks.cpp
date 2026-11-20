/**
 * @file motiontracks.cpp
 * @brief "Motion Between Poses": the poser's two poses as the clip's key poses.
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

#include "motiontracks.h"

#include <cmath>
#include <unordered_map>
#include <utility>

namespace plus::posermotion
{
    namespace
    {
        namespace history = posermcp::history;

        LLQuaternion viewerQuat(const history::Quat& q) { return LLQuaternion(q.x, q.y, q.z, q.w); }
        LLVector3    viewerVec(const history::Vec3& v) { return LLVector3(v.x, v.y, v.z); }

        bool isZero(const LLVector3& v)
        {
            return std::fabs(v.mV[VX]) <= TRACK_ZERO_OFFSET && std::fabs(v.mV[VY]) <= TRACK_ZERO_OFFSET &&
                   std::fabs(v.mV[VZ]) <= TRACK_ZERO_OFFSET;
        }

        /// Appends a vector track (the joints' member `field`, one per key, all validated) to every key; its index, or
        /// -1 when it is zero in every key (or field is null: the pelvis position has its own place).
        bool addVectorTrack(const std::vector<const history::JointState*>& joint_in, history::Vec3 history::JointState::*field,
                            std::vector<KeyPose>& keys, S32& index)
        {
            index = -1;
            if (!field)
            {
                return true;
            }
            std::vector<LLVector3> values;
            values.reserve(joint_in.size());
            bool all_zero = true;
            for (const history::JointState* joint : joint_in)
            {
                const LLVector3 v = viewerVec(joint->*field);
                if (!validPosition(v))
                {
                    return false;
                }
                all_zero = all_zero && isZero(v);
                values.push_back(v);
            }
            if (all_zero)
            {
                return true;
            }
            index = (S32)keys[0].extra_vec.size();
            for (size_t k = 0; k < keys.size(); ++k)
            {
                keys[k].extra_vec.push_back(values[k]);
            }
            return true;
        }

        /// The rest skeleton's rotations of a pose: the target of each bone the pose has, identity for the others.
        bool fillSkeleton(const SomaRetarget& retarget, const std::unordered_map<std::string, const history::JointState*>& by_name,
                          SlPose& out)
        {
            out.local.assign((size_t)retarget.jointCount(), LLQuaternion::DEFAULT);
            for (S32 j = 0; j < retarget.jointCount(); ++j)
            {
                const auto it = by_name.find(retarget.joint(j).name);
                if (it == by_name.end() || !it->second->rotation)
                {
                    continue;
                }
                if (!normalizedRotation(viewerQuat(it->second->target), out.local[(size_t)j]))
                {
                    return false;
                }
            }
            return true;
        }

        std::unordered_map<std::string, const history::JointState*> indexByName(const history::Pose& pose)
        {
            std::unordered_map<std::string, const history::JointState*> by_name;
            by_name.reserve(pose.joints.size());
            for (const history::JointState& joint : pose.joints)
            {
                by_name.emplace(joint.name, &joint);
            }
            return by_name;
        }
    } // namespace

    const char* describe(TrackResult result)
    {
        switch (result)
        {
        case TrackResult::Ok:       return "ok";
        case TrackResult::NotReady: return "the rest skeleton is not ready";
        case TrackResult::NoJoints: return "no joint in common";
        case TrackResult::BadPose:  return "a pose value is degenerate or out of range";
        }
        return "unknown";
    }

    TrackResult makeKeyPoses(const SomaRetarget& retarget, const std::vector<const history::Pose*>& poses,
                             TrackLayout& layout, std::vector<KeyPose>& keys)
    {
        if (!retarget.ready())
        {
            return TrackResult::NotReady;
        }
        if (poses.size() < 2)
        {
            return TrackResult::NoJoints;
        }
        const size_t n = poses.size();
        std::vector<std::unordered_map<std::string, const history::JointState*>> by_name;
        by_name.reserve(n);
        for (const history::Pose* pose : poses)
        {
            if (!pose)
            {
                return TrackResult::BadPose;
            }
            by_name.push_back(indexByName(*pose));
        }

        TrackLayout new_layout;
        new_layout.pelvis_rest = retarget.pelvisRest();
        std::vector<KeyPose> new_keys(n);
        for (size_t k = 0; k < n; ++k)
        {
            if (!fillSkeleton(retarget, by_name[k], new_keys[k].sl))
            {
                return TrackResult::BadPose;
            }
            new_keys[k].sl.pelvis = new_layout.pelvis_rest;
        }

        // the written joints, in the first pose's order (the poser's)
        std::vector<const history::JointState*> joint_in(n);
        for (const history::JointState& first : poses[0]->joints)
        {
            bool everywhere = true;
            bool driven     = false;
            for (size_t k = 0; k < n && everywhere; ++k)
            {
                const auto it = by_name[k].find(first.name);
                everywhere    = it != by_name[k].end();
                if (everywhere)
                {
                    joint_in[k] = it->second;
                    driven      = driven || it->second->enabled;
                }
            }
            if (!everywhere || !driven)
            {
                continue;
            }

            TrackJoint joint;
            joint.name    = first.name;
            joint.enabled = first.enabled;
            if (first.rotation)
            {
                joint.rest = retarget.find(first.name);
                if (joint.rest < 0)
                {
                    // a rotating joint the rest skeleton lacks: its own track
                    joint.extra_rot = (S32)new_keys[0].extra_rot.size();
                    for (size_t k = 0; k < n; ++k)
                    {
                        LLQuaternion q;
                        if (!normalizedRotation(viewerQuat(joint_in[k]->target), q))
                        {
                            return TrackResult::BadPose;
                        }
                        new_keys[k].extra_rot.push_back(q);
                    }
                }
            }

            const bool is_pelvis = first.name == PELVIS_JOINT;
            if (is_pelvis)
            {
                joint.pelvis = true;
                for (size_t k = 0; k < n; ++k)
                {
                    const LLVector3 pos = viewerVec(joint_in[k]->position);
                    if (!validPosition(pos))
                    {
                        return TrackResult::BadPose;
                    }
                    new_keys[k].sl.pelvis = new_layout.pelvis_rest + pos;
                }
            }
            if (!addVectorTrack(joint_in, is_pelvis ? nullptr : &history::JointState::position, new_keys, joint.position) ||
                !addVectorTrack(joint_in, &history::JointState::scale, new_keys, joint.scale))
            {
                return TrackResult::BadPose;
            }
            new_layout.joints.push_back(std::move(joint));
        }
        if (new_layout.joints.empty())
        {
            return TrackResult::NoJoints;
        }
        for (const KeyPose& key : new_keys)
        {
            if (!validPosition(key.sl.pelvis))
            {
                return TrackResult::BadPose;
            }
        }

        layout = std::move(new_layout);
        keys   = std::move(new_keys);
        return TrackResult::Ok;
    }

    TrackResult makeKeyPoses(const SomaRetarget& retarget, const history::Pose& start, const history::Pose& end,
                             TrackLayout& layout, KeyPose& start_key, KeyPose& end_key)
    {
        std::vector<KeyPose> keys;
        const TrackResult    r = makeKeyPoses(retarget, std::vector<const history::Pose*>{ &start, &end }, layout, keys);
        if (r == TrackResult::Ok)
        {
            start_key = std::move(keys[0]);
            end_key   = std::move(keys[1]);
        }
        return r;
    }

    bool trackValues(const TrackLayout& layout, S32 index, const KeyPose& key, TrackValues& out)
    {
        if (index < 0 || index >= (S32)layout.joints.size())
        {
            return false;
        }
        const TrackJoint& joint = layout.joints[(size_t)index];
        out.has_rotation = false;
        out.has_position = false;
        out.has_scale    = false;
        if (joint.rest >= 0)
        {
            if (joint.rest >= (S32)key.sl.local.size())
            {
                return false;
            }
            out.has_rotation = true;
            out.rotation     = key.sl.local[(size_t)joint.rest];
        }
        else if (joint.extra_rot >= 0)
        {
            if (joint.extra_rot >= (S32)key.extra_rot.size())
            {
                return false;
            }
            out.has_rotation = true;
            out.rotation     = key.extra_rot[(size_t)joint.extra_rot];
        }
        if (joint.pelvis)
        {
            out.has_position = true;
            out.position     = key.sl.pelvis - layout.pelvis_rest;
        }
        else if (joint.position >= 0)
        {
            if (joint.position >= (S32)key.extra_vec.size())
            {
                return false;
            }
            out.has_position = true;
            out.position     = key.extra_vec[(size_t)joint.position];
        }
        if (joint.scale >= 0)
        {
            if (joint.scale >= (S32)key.extra_vec.size())
            {
                return false;
            }
            out.has_scale = true;
            out.scale     = key.extra_vec[(size_t)joint.scale];
        }
        return true;
    }

    BvhClipTracks bvhTracksOf(const std::vector<BvhJoint>& joints, const TrackLayout& layout)
    {
        std::unordered_map<std::string, const TrackJoint*> by_name;
        by_name.reserve(layout.joints.size());
        BvhClipTracks tracks;
        tracks.pelvis_rest = layout.pelvis_rest;
        for (const TrackJoint& joint : layout.joints)
        {
            by_name.emplace(joint.name, &joint);
            tracks.root_position = tracks.root_position || joint.pelvis;
        }
        tracks.rest_of_joint.reserve(joints.size());
        tracks.extra_rot_of_joint.reserve(joints.size());
        for (const BvhJoint& joint : joints)
        {
            const auto it = by_name.find(joint.name);
            const TrackJoint* track = it != by_name.end() ? it->second : nullptr;
            const S32 rest = track ? track->rest : -1;
            tracks.rest_of_joint.push_back(rest);
            tracks.extra_rot_of_joint.push_back(track && rest < 0 ? track->extra_rot : -1);
        }
        return tracks;
    }
} // namespace plus::posermotion
