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
// Modified for Charis Viewer (2026): see soma_codec.h. The arithmetic follows the Warp kernels step by step (in
// double precision, the inputs stored as float32 like the kernels do) so the answers match the reference to
// float32 rounding.

#include "soma_codec.h"

#include <algorithm>
#include <cmath>

namespace soma
{
    namespace
    {
        Vec3 sub(const Vec3& a, const Vec3& b)
        {
            return { a.x - b.x, a.y - b.y, a.z - b.z };
        }

        Vec3 add(const Vec3& a, const Vec3& b)
        {
            return { a.x + b.x, a.y + b.y, a.z + b.z };
        }

        Vec3 cross(const Vec3& a, const Vec3& b)
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        }

        double length(const Vec3& a)
        {
            return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        }

        Quat mul(const Quat& a, const Quat& b)
        {
            return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                     a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
        }

        Quat conj(const Quat& q)
        {
            return { q.w, -q.x, -q.y, -q.z };
        }

        Vec3 rotate(const Quat& q, const Vec3& v)
        {
            // v + 2w (u x v) + 2 u x (u x v), u = the vector part
            const Vec3 u{ q.x, q.y, q.z };
            const Vec3 t = cross(u, v);
            const Vec3 t2{ 2.0 * t.x, 2.0 * t.y, 2.0 * t.z };
            const Vec3 c = cross(u, t2);
            return { v.x + q.w * t2.x + c.x, v.y + q.w * t2.y + c.y, v.z + q.w * t2.z + c.z };
        }

        // A turn of `angle` radians about +Y.
        Quat yaw(double angle)
        {
            return { std::cos(angle * 0.5), 0.0, std::sin(angle * 0.5), 0.0 };
        }

        Quat normalized(const Quat& q)
        {
            const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
            Quat r{ q.w / n, q.x / n, q.y / n, q.z / n };
            if (r.w < 0.0)
            {
                r = { -r.w, -r.x, -r.y, -r.z };
            }
            return r;
        }

        // The first two columns of the rotation matrix, column after column.
        void to6d(const Quat& q, float* out)
        {
            const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
            const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
            const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
            out[0] = float(1.0 - 2.0 * (yy + zz));
            out[1] = float(2.0 * (xy + wz));
            out[2] = float(2.0 * (xz - wy));
            out[3] = float(2.0 * (xy - wz));
            out[4] = float(1.0 - 2.0 * (xx + zz));
            out[5] = float(2.0 * (yz + wx));
        }

        // The rotation whose first column is a and whose second is b made orthogonal to it (Gram-Schmidt through
        // the cross product). false when a is zero or b is parallel to it.
        bool from6d(const float* v, Quat& q)
        {
            const Vec3   a{ v[0], v[1], v[2] };
            const Vec3   b{ v[3], v[4], v[5] };
            const double na = length(a);
            if (!(na > 1e-12))
            {
                return false;
            }
            const Vec3   x{ a.x / na, a.y / na, a.z / na };
            Vec3         z = cross(x, b);
            const double nz = length(z);
            if (!(nz > 1e-12))
            {
                return false;
            }
            z = { z.x / nz, z.y / nz, z.z / nz };
            const Vec3 y = cross(z, x);
            // the matrix [x y z] (columns) to a quaternion
            const double m00 = x.x, m10 = x.y, m20 = x.z;
            const double m01 = y.x, m11 = y.y, m21 = y.z;
            const double m02 = z.x, m12 = z.y, m22 = z.z;
            const double trace = m00 + m11 + m22;
            if (trace > 0.0)
            {
                const double s = std::sqrt(trace + 1.0) * 2.0;
                q = { 0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s };
            }
            else if (m00 > m11 && m00 > m22)
            {
                const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
                q = { (m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s };
            }
            else if (m11 > m22)
            {
                const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
                q = { (m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s };
            }
            else
            {
                const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
                q = { (m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s };
            }
            q = normalized(q);
            return true;
        }

        // The direction the hips face, from the two leg joints (the kernels' heading from hip positions):
        // forward = up x (right hip - left hip) on the ground, as atan2(forward.x, forward.z). 0 when the hips are
        // one above the other.
        double heading(const Vec3& rightHip, const Vec3& leftHip)
        {
            Vec3         across{ rightHip.x - leftHip.x, 0.0, rightHip.z - leftHip.z };
            const double n = length(across);
            if (n <= 1e-6)
            {
                return 0.0;
            }
            across = { across.x / n, 0.0, across.z / n };
            const Vec3 forward = cross(Vec3{ 0.0, 1.0, 0.0 }, across);
            return std::atan2(forward.x, forward.z);
        }

        // The heading of "facing +X" (atan2(1, 0)).
        const double FACING_X = std::atan2(1.0, 0.0);
    }

    void encode(const Pose (&window)[WINDOW_FRAMES], int targetFrames, int tokens, ModelInputs& in, Alignment& align)
    {
        const int   last = CONTEXT_FRAMES - 1;
        const Pose& anchor = window[last];
        align.offset = { anchor.pos[0].x, 0.0, anchor.pos[0].z };
        align.rotation = yaw(heading(anchor.pos[RIGHT_LEG], anchor.pos[LEFT_LEG]) - FACING_X);
        const Quat inv = conj(align.rotation);

        Vec3 aligned[WINDOW_FRAMES][JOINTS];
        for (int f = 0; f < WINDOW_FRAMES; ++f)
        {
            for (int j = 0; j < JOINTS; ++j)
            {
                aligned[f][j] = rotate(inv, sub(window[f].pos[j], align.offset));
            }
        }
        for (int f = 0; f < WINDOW_FRAMES; ++f)
        {
            // the speeds look one frame ahead, except on the last context frame (the window's two halves)
            const int    next = f == last ? f : std::min(f + 1, WINDOW_FRAMES - 1);
            const Vec3&  cur = aligned[f][0];
            const Vec3&  nxt = aligned[next][0];
            const double h = heading(aligned[f][RIGHT_LEG], aligned[f][LEFT_LEG]);
            const double nh = heading(aligned[next][RIGHT_LEG], aligned[next][LEFT_LEG]);
            const double turn = std::atan2(std::sin(nh - h), std::cos(nh - h));
            float* g = in.globalRoot + f * GLOBAL_ROOT_DIM;
            g[0] = float(cur.x);
            g[1] = float(cur.y);
            g[2] = float(cur.z);
            g[3] = float(std::cos(h));
            g[4] = float(std::sin(h));
            float* l = in.localRoot + f * LOCAL_ROOT_DIM;
            l[0] = float(turn * SAMPLE_RATE_HZ);
            l[1] = float((nxt.x - cur.x) * SAMPLE_RATE_HZ);
            l[2] = float((nxt.z - cur.z) * SAMPLE_RATE_HZ);
            l[3] = float(cur.y);
            float*     pose = in.localPoses + f * POSE_DIM;
            const Vec3 root{ cur.x, 0.0, cur.z };
            for (int j = 1; j < JOINTS; ++j)
            {
                const Vec3 d = sub(aligned[f][j], root);
                pose[(j - 1) * 3 + 0] = float(d.x);
                pose[(j - 1) * 3 + 1] = float(d.y);
                pose[(j - 1) * 3 + 2] = float(d.z);
            }
            for (int j = 0; j < JOINTS; ++j)
            {
                to6d(mul(inv, window[f].rot[j]), pose + (JOINTS - 1) * 3 + j * 6);
            }
        }
        const int present = CONTEXT_FRAMES + targetFrames;
        for (int f = 0; f < WINDOW_FRAMES; ++f)
        {
            const int32_t has = f < present ? 1 : 0;
            in.hasGlobalRoot[f] = has;
            in.hasLocalPoses[f] = has;
            // no speed where the next frame is not the following one (the last context and the last target)
            in.hasLocalRoot[f] = (f == last || f == present - 1) ? 0 : has;
        }
        for (int k = 0; k < TOKEN_CHOICES; ++k)
        {
            in.allowedTokens[k] = (tokens == 0 || tokens == MIN_TOKENS + k) ? 1 : 0;
        }
    }

    DecodeResult decode(const float* rows, int tokens, int wantTokens, const Alignment& align, std::vector<OutFrame>& frames)
    {
        frames.clear();
        if (tokens < MIN_TOKENS || tokens > MAX_TOKENS)
        {
            return DecodeResult::BadTokens;
        }
        if (wantTokens != 0 && tokens != wantTokens)
        {
            return DecodeResult::WrongLength;
        }
        const int count = tokens * FRAMES_PER_TOKEN - 2 * BOUNDARY_FRAMES;
        for (int f = 0; f < count; ++f)
        {
            const float* r = rows + (BOUNDARY_FRAMES + f) * OUT_DIM;
            for (int k = 0; k < OUT_DIM; ++k)
            {
                if (!std::isfinite(r[k]))
                {
                    return DecodeResult::NotFinite;
                }
            }
        }
        frames.resize(count);
        Quat world[JOINTS];
        for (int f = 0; f < count; ++f)
        {
            const float* r = rows + (BOUNDARY_FRAMES + f) * OUT_DIM;
            OutFrame&    out = frames[f];
            for (int j = 0; j < JOINTS; ++j)
            {
                if (!from6d(r + OUT_ROT_OFFSET + j * 6, world[j]))
                {
                    frames.clear();
                    return DecodeResult::Degenerate;
                }
            }
            out.hips = add(align.offset, rotate(align.rotation, Vec3{ r[0], r[1], r[2] }));
            if (!(length(out.hips) <= MAX_HIPS_M))   // the answer is written as float32: keep it far inside
            {
                frames.clear();
                return DecodeResult::NotFinite;
            }
            out.local[0] = normalized(mul(align.rotation, world[0]));
            for (int j = 1; j < JOINTS; ++j)
            {
                out.local[j] = normalized(mul(conj(world[JOINT_PARENTS[j]]), world[j]));
            }
            for (int c = 0; c < CONTACTS; ++c)
            {
                out.contacts[c] = std::clamp(r[OUT_DIM - CONTACTS + c], 0.0f, 1.0f);
            }
        }
        return DecodeResult::Ok;
    }
}
