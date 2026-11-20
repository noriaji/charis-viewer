// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License is distributed
// on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for
// the specific language governing permissions and limitations under the License.
//
// Modified for Charis Viewer (2026): the pose encoding and decoding of the SOMA model of NVIDIA MotionBricks
// (motionbricks/model/soma/kernels.py, https://github.com/NVlabs/motionbricks commit 54fc563), rewritten from its
// Warp kernels into standard C++ for one request at a time on the CPU. The endpoint correction is left out (the
// viewer corrects the ends after retargeting).

// The model's input and output layout (soma_n1.json): an 8-frame window of 4 context frames and 1 to 4 target
// frames goes in as root values and local poses, "facing +X" aligned on the last context frame; out come up to 64
// frames of hips position and joint rotations (6D), of which the first and last BOUNDARY_FRAMES are dropped.
// Coordinates: the SOMA aligned frame, +X forward, +Y up, +Z right, meters; quaternions are column-vector,
// Hamilton, {w, x, y, z}.

#pragma once

#include "soma_tables.h"

#include <cstdint>
#include <vector>

namespace soma
{
    constexpr int    CONTEXT_FRAMES = 4;
    constexpr int    MAX_TARGET_FRAMES = 4;
    constexpr int    WINDOW_FRAMES = CONTEXT_FRAMES + MAX_TARGET_FRAMES;   // 8
    constexpr int    MIN_TOKENS = 6;
    constexpr int    MAX_TOKENS = 16;
    constexpr int    TOKEN_CHOICES = MAX_TOKENS - MIN_TOKENS + 1;            // 11
    constexpr int    FRAMES_PER_TOKEN = 4;
    constexpr int    BOUNDARY_FRAMES = 3;
    constexpr int    MIN_FRAMES = MIN_TOKENS * FRAMES_PER_TOKEN - 2 * BOUNDARY_FRAMES;   // 18
    constexpr int    MAX_FRAMES = MAX_TOKENS * FRAMES_PER_TOKEN - 2 * BOUNDARY_FRAMES;   // 58
    constexpr double SAMPLE_RATE_HZ = 30.0;

    constexpr int GLOBAL_ROOT_DIM = 5;                               // x, y, z, cos(heading), sin(heading)
    constexpr int LOCAL_ROOT_DIM = 4;                                // heading speed, x speed, z speed, height
    constexpr int POSE_DIM = (JOINTS - 1) * 3 + JOINTS * 6;          // 267: positions from the root, 6D rotations
    constexpr int OUT_ROWS = MAX_TOKENS * FRAMES_PER_TOKEN;          // 64
    constexpr int OUT_DIM = 366;
    constexpr int OUT_ROT_OFFSET = 5 + (JOINTS - 1) * 3;             // 92: the 6D rotations of a row
    constexpr int CONTACTS = 4;                                      // the last CONTACTS values of a row
    constexpr double MAX_HIPS_M = 1000.0;                            // an answer's hips further away is refused

    struct Vec3
    {
        double x = 0.0, y = 0.0, z = 0.0;
    };

    struct Quat
    {
        double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
    };

    // One frame in: every joint's world position and world rotation (unit quaternions).
    struct Pose
    {
        Vec3 pos[JOINTS];
        Quat rot[JOINTS];
    };

    // The seven ONNX inputs, in the shapes of soma_n1.json (batch 1).
    struct ModelInputs
    {
        float   globalRoot[WINDOW_FRAMES * GLOBAL_ROOT_DIM];
        float   localRoot[WINDOW_FRAMES * LOCAL_ROOT_DIM];
        float   localPoses[WINDOW_FRAMES * POSE_DIM];
        int32_t hasGlobalRoot[WINDOW_FRAMES];
        int32_t hasLocalRoot[WINDOW_FRAMES];
        int32_t hasLocalPoses[WINDOW_FRAMES];
        int32_t allowedTokens[TOKEN_CHOICES];
    };

    // Where the window was moved to face +X: decode() puts the answer back with it.
    struct Alignment
    {
        Vec3 offset;
        Quat rotation;
    };

    // window: CONTEXT_FRAMES context frames, then targetFrames (1..MAX_TARGET_FRAMES) targets, then anything up
    // to WINDOW_FRAMES (the caller repeats the last target; those frames are marked absent). tokens: 0 lets the
    // model choose any length from MIN_TOKENS to MAX_TOKENS, otherwise exactly that many.
    void encode(const Pose (&window)[WINDOW_FRAMES], int targetFrames, int tokens, ModelInputs& in, Alignment& align);

    // One frame out: the hips' world position, every joint's rotation relative to its parent (the root's is its
    // world rotation; w >= 0) and the four foot contacts (0..1).
    struct OutFrame
    {
        Vec3  hips;
        Quat  local[JOINTS];
        float contacts[CONTACTS];
    };

    enum class DecodeResult
    {
        Ok,
        BadTokens,    // the model's token count is outside MIN_TOKENS..MAX_TOKENS
        WrongLength,  // a length was asked for (wantTokens) and the model answered another
        NotFinite,    // a NaN or an infinity in the frames used, or hips further than MAX_HIPS_M from the origin
        Degenerate    // a 6D rotation whose two vectors are zero or parallel
    };

    // rows: OUT_ROWS x OUT_DIM floats, the model's first output; tokens: its second; wantTokens: the length the
    // request asked for (0: any). frames gets tokens * FRAMES_PER_TOKEN - 2 * BOUNDARY_FRAMES frames
    // (MIN_FRAMES..MAX_FRAMES) when the result is Ok, and is left empty otherwise.
    DecodeResult decode(const float* rows, int tokens, int wantTokens, const Alignment& align, std::vector<OutFrame>& frames);
}
