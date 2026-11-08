/**
 * @file llrhitexanim.h
 * @brief A texture animation's description in a face's texture matrix, and
 *        its matrix at a time (research/194 §11, plus/texanim).
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

#ifndef LL_LLRHITEXANIM_H
#define LL_LLRHITEXANIM_H

#include <cmath>

// Upstream steps every texture animation every frame (LLViewerTextureAnim::
// updateClass) and writes each face's texture matrix (LLFace::mTextureMatrix,
// 16 floats). plus/texanim writes, once, a description of the animation into
// those 16 floats instead; whoever turns the 16 floats into what the GPU or
// the CPU uses (the backend: apply_uniform_matrix, matrix_row,
// write_direct_matrices, the native shadow's texture rows; llface.cpp: the
// small faces' bake and surfaceToTexture) calls matrixAt() first, at the
// frame's animation time (LLRHI::sTexAnimTime). Everything between (gGL's
// matrix stack, the draw infos, the draw lists' copies) moves the 16 floats
// as they are.
//
// Stage S1: the smooth scroll only (ON | LOOP | SMOOTH, no PING_PONG,
// REVERSE, ROTATE or SCALE). The arithmetic is upstream's, in its order and
// in F32 (LLViewerTextureAnim::animateTextures, then the matrix of
// LLVOVolume::animateTextures: T(-0.5) R S T(off + 0.5) built by LLMatrix4's
// translate, rotate, *= and translate), so the matrix is upstream's for the
// same frame counter. Plain types only: the unit test (plus/texanim/
// texanim_test.cpp) compiles this header on its own.
//
// The 16 floats (LLMatrix4::mMatrix[r][c] is f[r * 4 + c]; column-major for GL):
//   0 rate, 1 anchor time (high word), 2 anchor time (low word), 3 the frame
//   counter at the anchor (mLastTime), 4 full length, 5 start, 6 size x,
//   7 size y (0: no grid), 8..11 the face's rotation (R00 R01 R10 R11, from
//   its texture entry's rotation the way upstream builds it), 12..13 the
//   face's texture entry scale (used without a grid), 14 the kind's magic,
//   15 -1 (a real texture matrix is affine: 1).
// A reader that forgot matrixAt() would multiply the texcoords by these
// values: the face's pattern breaks up (visible), it does not stand still.
namespace LLRHITexAnim
{
    enum Slot
    {
        RATE = 0, TIME_HI, TIME_LO, LAST,
        FULL, START, SIZE_X, SIZE_Y,
        R00, R01, R10, R11,
        SCALE_S, SCALE_T, KIND, MARK
    };

    constexpr float MARK_VALUE = -1.f;
    // The kinds' magic numbers: normal floats no matrix has in this place.
    constexpr float KIND_SCROLL = 1.2345678e-30f;

    inline bool isDescription(const float* m)
    {
        return m[MARK] == MARK_VALUE && m[KIND] == KIND_SCROLL;
    }

    // LLQuaternion::setQuat(rot, 0, 0, -1) then LLQuaternion::getMatrix3, the
    // four elements that are not 0 or 1 (llvovolume.cpp: quat.setQuat(rot, 0, 0, -1.f)).
    inline void rotationOf(float rot, float out[4])
    {
        float angle = rot;
        const float mag = sqrtf(0.f * 0.f + 0.f * 0.f + -1.f * -1.f);
        angle *= 0.5f;     // upstream: *= 0.5 (a double; halving a float is exact either way)
        const float c = cosf(angle);
        const float s = sinf(angle) / mag;
        const float qx = 0.f * s;
        const float qy = 0.f * s;
        const float qz = -1.f * s;
        const float qw = c;
        const float xx = qx * qx;
        const float xy = qx * qy;
        const float yy = qy * qy;
        const float zz = qz * qz;
        const float zw = qz * qw;
        out[0] = 1.f - 2.f * (yy + zz);     // R00
        out[1] = 2.f * (xy + zw);           // R01
        out[2] = 2.f * (xy - zw);           // R10
        out[3] = 1.f - 2.f * (xx + zz);     // R11
    }

    // The anchor time (F64 seconds of LLFrameTimer's frame time) in two floats.
    inline void splitTime(double t, float& hi, float& lo)
    {
        hi = (float)t;
        lo = (float)(t - (double)hi);
    }

    inline double anchorTime(const float* d)
    {
        return (double)d[TIME_HI] + (double)d[TIME_LO];
    }

    // A smooth scroll's description. full_length: upstream's (mLength, else
    // max(1, size_x * size_y); no ping-pong). te_rot, te_scale_*: the face's
    // texture entry.
    inline void describeScroll(float* d, float rate, double anchor_time, float last, float full_length, float start,
                               float size_x, float size_y, float te_rot, float te_scale_s, float te_scale_t)
    {
        d[RATE] = rate;
        splitTime(anchor_time, d[TIME_HI], d[TIME_LO]);
        d[LAST] = last;
        d[FULL] = full_length;
        d[START] = start;
        d[SIZE_X] = size_x;
        d[SIZE_Y] = size_y;
        rotationOf(te_rot, d + R00);
        d[SCALE_S] = te_scale_s;
        d[SCALE_T] = te_scale_t;
        d[KIND] = KIND_SCROLL;
        d[MARK] = MARK_VALUE;
    }

    // Upstream's SMOOTH step from the anchor: the frame counter before the
    // loop (what it keeps in mLastTime). The timer's F32 elapsed time, times
    // the rate, plus the counter at the anchor.
    inline float rawCounter(const float* d, double now)
    {
        const float elapsed = (float)(now - anchorTime(d));
        return elapsed * d[RATE] + d[LAST];
    }

    // The matrix for a raw frame counter (rawCounter()): the loop, the start,
    // the grid or the plain scroll, then the matrix.
    inline void matrixForCounter(const float* d, float frame_counter, float* out)
    {
        frame_counter = fmodf(frame_counter, d[FULL]);  // LOOP (C's fmod: the sign of the counter)
        frame_counter += d[START];

        const float size_x = d[SIZE_X];
        const float size_y = d[SIZE_Y];
        float scale_s;
        float scale_t;
        float off_s;
        float off_t;
        if (size_x != 0.f && size_y != 0.f)
        {
            scale_s = 1.f / size_x;
            scale_t = 1.f / size_y;
            const float x_frame = fmodf(frame_counter, size_x);
            const int y_frame = (int)(frame_counter / size_x);
            const float x_pos = x_frame * scale_s;
            const float y_pos = y_frame * scale_t;
            off_s = (-0.5f + 0.5f * scale_s) + x_pos;
            off_t = (0.5f - 0.5f * scale_t) - y_pos;
        }
        else
        {
            const float anim_scale = 1.f;
            const float x_pos = frame_counter * anim_scale;
            off_s = (-0.5f + 0.5f * anim_scale) + x_pos;
            off_t = 0.f;
            scale_s = d[SCALE_S];   // TRANSLATE only: the scale is the face's
            scale_t = d[SCALE_T];
        }

        // setIdentity; translate(-0.5, -0.5, 0); rotate(R); *= S(scale);
        // translate(off + 0.5): the products' terms in LLMatrix4's order (the
        // ones with a 0 factor left out: adding 0 changes nothing).
        const float r00 = d[R00];
        const float r01 = d[R01];
        const float r10 = d[R10];
        const float r11 = d[R11];
        const float row3_0 = -0.5f * r00 + -0.5f * r10;
        const float row3_1 = -0.5f * r01 + -0.5f * r11;
        out[0] = r00 * scale_s;
        out[1] = r01 * scale_t;
        out[2] = 0.f;
        out[3] = 0.f;
        out[4] = r10 * scale_s;
        out[5] = r11 * scale_t;
        out[6] = 0.f;
        out[7] = 0.f;
        out[8] = 0.f;
        out[9] = 0.f;
        out[10] = 1.f;
        out[11] = 0.f;
        out[12] = row3_0 * scale_s + (off_s + 0.5f);
        out[13] = row3_1 * scale_t + (off_t + 0.5f);
        out[14] = 0.f;
        out[15] = 1.f;
    }

    // The description's matrix at `now` (LLRHI::sTexAnimTime). `out` may not be `d`.
    inline void matrixAt(const float* d, double now, float* out)
    {
        matrixForCounter(d, rawCounter(d, now), out);
    }
}

#endif // LL_LLRHITEXANIM_H
