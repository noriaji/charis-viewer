/**
 * @file soma_tables.h
 * @brief The SOMA skeleton's joint names and parents, in the order the MotionBricks SOMA model uses.
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

// The joint names and their order are the model's (joint_names of motionbricks/assets/onnx/soma/soma_n1.json), the
// parents those of the skeleton the model was trained on (motionbricks/assets/human/soma_base_skel_minimal_no_root.usd,
// with the finger joints between each Hand and its *End collapsed). Both come from NVIDIA MotionBricks,
// https://github.com/NVlabs/motionbricks, commit 54fc563 (v0.2.0):
//   SOMA skeleton from NVIDIA MotionBricks (c) 2026 NVIDIA CORPORATION & AFFILIATES, CC-BY-4.0.
//   (Creative Commons Attribution 4.0 International, https://creativecommons.org/licenses/by/4.0/; changed: only
//   the names and the parent of each joint are taken, the fingers collapsed as above.)
// Only the names and the hierarchy are here (no bone lengths, no rest rotations: the helper does not need them).
// The developers' test checks this table against the skeleton table the viewer's retargeting is generated from.

#pragma once

namespace soma
{
    constexpr int JOINTS = 30;

    constexpr const char* JOINT_NAMES[JOINTS] = {
        "Hips",              //  0
        "Spine1",            //  1
        "Spine2",            //  2
        "Chest",             //  3
        "Neck1",             //  4
        "Neck2",             //  5
        "Head",              //  6
        "Jaw",               //  7
        "LeftEye",           //  8
        "RightEye",          //  9
        "LeftShoulder",      // 10
        "LeftArm",           // 11
        "LeftForeArm",       // 12
        "LeftHand",          // 13
        "LeftHandThumbEnd",  // 14
        "LeftHandMiddleEnd", // 15
        "RightShoulder",     // 16
        "RightArm",          // 17
        "RightForeArm",      // 18
        "RightHand",         // 19
        "RightHandThumbEnd", // 20
        "RightHandMiddleEnd",// 21
        "LeftLeg",           // 22
        "LeftShin",          // 23
        "LeftFoot",          // 24
        "LeftToeBase",       // 25
        "RightLeg",          // 26
        "RightShin",         // 27
        "RightFoot",         // 28
        "RightToeBase",      // 29
    };

    // -1 for the root. Every parent comes before its child.
    constexpr int JOINT_PARENTS[JOINTS] = {
        -1, 0, 1, 2, 3, 4, 5, 6, 6, 6,
        3, 10, 11, 12, 13, 13,
        3, 16, 17, 18, 19, 19,
        0, 22, 23, 24,
        0, 26, 27, 28,
    };

    // The hips joints the heading is taken from (the model's compute_heading_method "hips_pos").
    constexpr int LEFT_LEG = 22;
    constexpr int RIGHT_LEG = 26;
}
