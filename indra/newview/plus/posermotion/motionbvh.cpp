/**
 * @file motionbvh.cpp
 * @brief "Motion Between Poses": a clip as the MOTION part of a BVH file.
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

#include "motionbvh.h"

#include "m3math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <istream>
#include <ostream>
#include <sstream>

namespace plus::posermotion
{
    namespace
    {
        constexpr F64 RAD_TO_DEG_64    = 57.295779513082320876798;
        constexpr F64 GIMBAL_COS       = 1e-6; // cos(pitch) under this: the first and last axes line up
        constexpr size_t MAX_LINE_CHARS = 4096;

        /// llbvhloader.cpp's makeTranslation(): the rows x, y, z of the BVH frame are SL's y, z, x.
        const LLQuaternion& bvhFrame()
        {
            static const LLQuaternion frame = []
            {
                LLMatrix3 m;
                m.setRows(LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, 0.f, 1.f), LLVector3(1.f, 0.f, 0.f));
                return LLQuaternion(m);
            }();
            return frame;
        }

        /// The rotation matrix of q acting on column vectors (row r, column c), in double: v' = M v is what the
        /// viewer writes v * q.
        void columnMatrix(const LLQuaternion& q, F64 m[3][3])
        {
            F64 x = q.mQ[VX], y = q.mQ[VY], z = q.mQ[VZ], w = q.mQ[VW];
            const F64 n = std::sqrt(x * x + y * y + z * z + w * w);
            if (n > 0.0)
            {
                x /= n; y /= n; z /= n; w /= n;
            }
            m[0][0] = 1.0 - 2.0 * (y * y + z * z); m[0][1] = 2.0 * (x * y - z * w);       m[0][2] = 2.0 * (x * z + y * w);
            m[1][0] = 2.0 * (x * y + z * w);       m[1][1] = 1.0 - 2.0 * (x * x + z * z); m[1][2] = 2.0 * (y * z - x * w);
            m[2][0] = 2.0 * (x * z - y * w);       m[2][1] = 2.0 * (y * z + x * w);       m[2][2] = 1.0 - 2.0 * (x * x + y * y);
        }

        bool parseChannel(const std::string& word, BvhChannel& out)
        {
            if (word.size() != 9) // "Xposition", "Yrotation", ...
            {
                return false;
            }
            const char axis = word[0];
            if (axis < 'X' || axis > 'Z')
            {
                return false;
            }
            const std::string kind = word.substr(1);
            if (kind == "position")
            {
                out.kind = BvhChannel::Kind::Position;
            }
            else if (kind == "rotation")
            {
                out.kind = BvhChannel::Kind::Rotation;
            }
            else
            {
                return false;
            }
            out.axis = (U8)(axis - 'X');
            return true;
        }

        /// As SL's loader reads a joint's channels: 3 rotations, or 3 positions X, Y, Z then 3 rotations, each
        /// rotation axis once.
        bool channelsLoadable(const std::vector<BvhChannel>& ch)
        {
            size_t first_rot = 0;
            if (ch.size() == 6)
            {
                for (U8 a = 0; a < 3; ++a)
                {
                    if (ch[a].kind != BvhChannel::Kind::Position || ch[a].axis != a)
                    {
                        return false;
                    }
                }
                first_rot = 3;
            }
            else if (ch.size() != 3)
            {
                return false;
            }
            bool seen[3] = { false, false, false };
            for (size_t i = first_rot; i < ch.size(); ++i)
            {
                if (ch[i].kind != BvhChannel::Kind::Rotation || seen[ch[i].axis])
                {
                    return false;
                }
                seen[ch[i].axis] = true;
            }
            return true;
        }

        void appendNumber(std::string& line, F64 value)
        {
            char buf[32];
            const int n = std::snprintf(buf, sizeof(buf), " %.6f", value);
            if (n > 0)
            {
                line.append(buf, (size_t)std::min(n, (int)sizeof(buf) - 1));
            }
        }
    } // namespace

    bool readBvhHierarchy(std::istream& in, std::vector<BvhJoint>& out)
    {
        out.clear();
        std::vector<BvhJoint> joints;
        std::string line;
        bool seen_root = false;
        while (std::getline(in, line))
        {
            if (line.size() > MAX_LINE_CHARS)
            {
                return false;
            }
            std::istringstream words(line);
            std::string first;
            if (!(words >> first))
            {
                continue;
            }
            if (first == "MOTION")
            {
                break;
            }
            if (first == "ROOT" || first == "JOINT")
            {
                if ((first == "ROOT") == seen_root || joints.size() >= (size_t)BVH_MAX_JOINTS)
                {
                    return false; // ROOT first and once, a JOINT only after it
                }
                seen_root = true;
                BvhJoint joint;
                if (!(words >> joint.name))
                {
                    return false;
                }
                joints.push_back(std::move(joint));
            }
            else if (first == "CHANNELS")
            {
                int count = 0;
                if (joints.empty() || !joints.back().channels.empty() || !(words >> count) || (count != 3 && count != 6))
                {
                    return false;
                }
                std::vector<BvhChannel>& ch = joints.back().channels;
                for (int i = 0; i < count; ++i)
                {
                    std::string word;
                    BvhChannel channel;
                    if (!(words >> word) || !parseChannel(word, channel))
                    {
                        return false;
                    }
                    ch.push_back(channel);
                }
                if (!channelsLoadable(ch))
                {
                    return false;
                }
            }
        }
        if (joints.empty())
        {
            return false;
        }
        for (const BvhJoint& joint : joints)
        {
            if (joint.channels.empty())
            {
                return false;
            }
        }
        out = std::move(joints);
        return true;
    }

    LLVector3 bvhAnglesDegrees(const LLQuaternion& q, const BvhChannel rotation_order[3])
    {
        // The loader: SL = frame^-1 * mayaQ(reversed order) * frame (the viewer's products: left first), and
        // mayaQ of the reversed order turns about the last channel's axis first. With column vectors that is
        // R = R(a0) R(a1) R(a2) for the channels a0 a1 a2 as the file lists them.
        const LLQuaternion inBvh = bvhFrame() * q * ~bvhFrame();
        F64 m[3][3];
        columnMatrix(inBvh, m);

        const int i = rotation_order[0].axis;
        const int j = rotation_order[1].axis;
        const int k = rotation_order[2].axis;
        // +1 when (i, j, k) is an even permutation of (0, 1, 2)
        const F64 e = ((j - i + 3) % 3 == 1) ? 1.0 : -1.0;

        F64 angle[3] = { 0.0, 0.0, 0.0 }; // by axis
        const F64 sin_b = e * m[i][k];
        const F64 cos_b = std::sqrt(m[i][i] * m[i][i] + m[i][j] * m[i][j]);
        angle[j] = std::atan2(sin_b, cos_b);
        if (cos_b > GIMBAL_COS)
        {
            angle[i] = std::atan2(-e * m[j][k], m[k][k]);
            angle[k] = std::atan2(-e * m[i][j], m[i][i]);
        }
        else
        {
            // the first and last axes line up: all of their turn on the first
            angle[i] = std::atan2(e * m[k][j], m[j][j]);
            angle[k] = 0.0;
        }
        return LLVector3((F32)(angle[0] * RAD_TO_DEG_64), (F32)(angle[1] * RAD_TO_DEG_64), (F32)(angle[2] * RAD_TO_DEG_64));
    }

    bool writeBvhMotion(std::ostream& out, const std::vector<BvhJoint>& joints, const BvhClipTracks& tracks,
                        const MotionClip& clip)
    {
        if (joints.empty() || tracks.rest_of_joint.size() != joints.size() ||
            tracks.extra_rot_of_joint.size() != joints.size() || clip.frame_count <= 0 ||
            clip.joint_count <= 0 || clip.local.size() != (size_t)clip.frame_count * (size_t)clip.joint_count ||
            clip.pelvis.size() != (size_t)clip.frame_count || !validPosition(tracks.pelvis_rest))
        {
            return false;
        }
        for (size_t n = 0; n < joints.size(); ++n)
        {
            const S32 rest  = tracks.rest_of_joint[n];
            const S32 extra = tracks.extra_rot_of_joint[n];
            if (rest < -1 || rest >= clip.joint_count || extra < -1 || extra >= clip.extra_rot_count ||
                !channelsLoadable(joints[n].channels))
            {
                return false;
            }
            if (extra >= 0 && clip.extra_rot.size() != (size_t)clip.frame_count * (size_t)clip.extra_rot_count)
            {
                return false;
            }
        }

        std::string text = "MOTION\n";
        text += "Frames: " + std::to_string(clip.frame_count + 1) + "\n";
        char frame_time[48];
        std::snprintf(frame_time, sizeof(frame_time), "Frame Time: %.7f\n", 1.0 / CLIP_FPS);
        text += frame_time;

        // frame 0: the reference, all 0 (as FSFloaterPoser::writeFirstFrameOfBvhMotion)
        std::string line;
        for (const BvhJoint& joint : joints)
        {
            for (size_t c = 0; c < joint.channels.size(); ++c)
            {
                appendNumber(line, 0.0);
            }
        }
        text.append(line, 1, std::string::npos);
        text += "\n";

        for (S32 f = 0; f < clip.frame_count; ++f)
        {
            line.clear();
            for (size_t n = 0; n < joints.size(); ++n)
            {
                const BvhJoint& joint = joints[n];
                const S32 rest  = tracks.rest_of_joint[n];
                const S32 extra = tracks.extra_rot_of_joint[n];
                size_t first_rot = 0;
                if (joint.channels.size() == 6)
                {
                    // the pelvis' offset in inches, BVH x, y, z = SL y, z, x (FSFloaterPoser::positionToString)
                    // (only for the root, and only when the clip moves the pelvis: else 0, as the playback leaves it)
                    const LLVector3 offset = (n == 0 && tracks.root_position) ? clip.pelvis[(size_t)f] - tracks.pelvis_rest
                                                                              : LLVector3::zero;
                    if (!validPosition(offset))
                    {
                        return false;
                    }
                    appendNumber(line, (F64)offset.mV[VY] * BVH_INCHES_PER_M);
                    appendNumber(line, (F64)offset.mV[VZ] * BVH_INCHES_PER_M);
                    appendNumber(line, (F64)offset.mV[VX] * BVH_INCHES_PER_M);
                    first_rot = 3;
                }
                LLVector3 angles;
                if (rest >= 0 || extra >= 0)
                {
                    const LLQuaternion& raw = rest >= 0 ? clip.local[(size_t)f * (size_t)clip.joint_count + (size_t)rest]
                                                        : clip.extra_rot[(size_t)f * (size_t)clip.extra_rot_count + (size_t)extra];
                    LLQuaternion q;
                    if (!normalizedRotation(raw, q))
                    {
                        return false;
                    }
                    angles = bvhAnglesDegrees(q, &joint.channels[first_rot]);
                    if (!angles.isFinite())
                    {
                        return false;
                    }
                }
                for (size_t c = first_rot; c < joint.channels.size(); ++c)
                {
                    appendNumber(line, angles.mV[joint.channels[c].axis]);
                }
            }
            text.append(line, 1, std::string::npos);
            text += "\n";
        }

        out << text;
        return (bool)out;
    }
} // namespace plus::posermotion
