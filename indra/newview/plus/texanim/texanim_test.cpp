// Stand-alone test of llrhitexanim.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX texanim_test.cpp && texanim_test.exe
// research/194 11: a smooth scroll's description, turned into a matrix at a
// frame's time, gives the matrix upstream's step builds that frame
// (LLViewerTextureAnim::animateTextures, then LLVOVolume::animateTextures'
// face matrix with LLMatrix4's arithmetic, transcribed below), for random
// rates (negative too), grids, starts, lengths, texture entry rotations and
// scales, frame steps and clock values; and that the comparison finds a
// decoder with one thing wrong (the floor mod of GLSL, the rotation's sign,
// the scale before the rotation, the start left out, the face's scale used
// with a grid, the anchor's low word dropped).
#include "../../../llrhi/llrhitexanim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace
{
    int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

    // ---- upstream, as llviewertextureanim.cpp / llvovolume.cpp / m4math have it ----
    enum { ON = 0x01, LOOP = 0x02, REVERSE = 0x04, PING_PONG = 0x08, SMOOTH = 0x10, ROTATE = 0x20, SCALE = 0x40 };
    enum { TRANSLATE = 0x01 };

    struct Mat4
    {
        float m[4][4];
        void setIdentity()
        {
            std::memset(m, 0, sizeof(m));
            m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.f;
        }
        void translate(float x, float y, float z)
        {
            m[3][0] += x;
            m[3][1] += y;
            m[3][2] += z;
        }
        void mul(const Mat4& b)     // operator*=(LLMatrix4&, const LLMatrix4&)
        {
            Mat4 r;
            for (int i = 0; i < 4; i++)
            {
                for (int j = 0; j < 4; j++)
                {
                    r.m[j][i] = m[j][0] * b.m[0][i] + m[j][1] * b.m[1][i] + m[j][2] * b.m[2][i] + m[j][3] * b.m[3][i];
                }
            }
            *this = r;
        }
    };

    struct Quat { float x, y, z, w; };

    Quat set_quat(float angle, float x, float y, float z)     // LLQuaternion::setQuat(F32, F32, F32, F32)
    {
        Quat q{ 0.f, 0.f, 0.f, 1.f };
        float mag = sqrtf(x * x + y * y + z * z);
        if (mag > 0.0000001f)
        {
            angle = (float)(angle * 0.5);
            float c = cosf(angle);
            float s = sinf(angle) / mag;
            q = { x * s, y * s, z * s, c };
        }
        return q;
    }

    Mat4 rotation(const Quat& q)    // LLMatrix4(q): LLMatrix3(q) (getMatrix3) then initMatrix
    {
        float xx = q.x * q.x, xy = q.x * q.y, xz = q.x * q.z, xw = q.x * q.w;
        float yy = q.y * q.y, yz = q.y * q.z, yw = q.y * q.w;
        float zz = q.z * q.z, zw = q.z * q.w;
        Mat4 r;
        r.m[0][0] = 1.f - 2.f * (yy + zz); r.m[0][1] = 2.f * (xy + zw); r.m[0][2] = 2.f * (xz - yw); r.m[0][3] = 0.f;
        r.m[1][0] = 2.f * (xy - zw); r.m[1][1] = 1.f - 2.f * (xx + zz); r.m[1][2] = 2.f * (yz + xw); r.m[1][3] = 0.f;
        r.m[2][0] = 2.f * (xz + yw); r.m[2][1] = 2.f * (yz - xw); r.m[2][2] = 1.f - 2.f * (xx + yy); r.m[2][3] = 0.f;
        r.m[3][0] = 0.f; r.m[3][1] = 0.f; r.m[3][2] = 0.f; r.m[3][3] = 1.f;
        return r;
    }

    Mat4 init_all_scale(float sx, float sy, float sz)  // initAll(scale, LLQuaternion(), LLVector3())
    {
        Quat q{ 0.f, 0.f, 0.f, 1.f };
        float xx = q.x * q.x, xy = q.x * q.y, xz = q.x * q.z, xw = q.x * q.w;
        float yy = q.y * q.y, yz = q.y * q.z, yw = q.y * q.w;
        float zz = q.z * q.z, zw = q.z * q.w;
        Mat4 r;
        r.m[0][0] = (1.f - 2.f * (yy + zz)) * sx; r.m[0][1] = (2.f * (xy + zw)) * sx; r.m[0][2] = (2.f * (xz - yw)) * sx; r.m[0][3] = 0.f;
        r.m[1][0] = (2.f * (xy - zw)) * sy; r.m[1][1] = (1.f - 2.f * (xx + zz)) * sy; r.m[1][2] = (2.f * (yz + xw)) * sy; r.m[1][3] = 0.f;
        r.m[2][0] = (2.f * (xz + yw)) * sz; r.m[2][1] = (2.f * (yz - xw)) * sz; r.m[2][2] = (1.f - 2.f * (xx + yy)) * sz; r.m[2][3] = 0.f;
        r.m[3][0] = 0.f; r.m[3][1] = 0.f; r.m[3][2] = 0.f; r.m[3][3] = 1.0;
        return r;
    }

    struct Anim
    {
        unsigned char mode = ON | LOOP | SMOOTH;
        unsigned char size_x = 1, size_y = 1;
        float start = 0.f, length = 1.f, rate = 1.f;
        double timer_start = 0.0;       // LLFrameTimer::mStartTime
        double last_time = 0.0;         // mLastTime
        float last_frame = -1.f;        // mLastFrame
    };

    // LLViewerTextureAnim::animateTextures for ON | LOOP | SMOOTH (the other
    // modes' branches are not taken); sFrameTime = now.
    int upstream_step(Anim& a, double now, float& off_s, float& off_t, float& scale_s, float& scale_t)
    {
        int result = 0;
        float num_frames = a.length ? a.length : std::max(1.f, (float)(a.size_x * a.size_y));
        float full_length = num_frames;
        float elapsed = float(now - a.timer_start);
        a.timer_start = now;
        float frame_counter = elapsed * a.rate + (float)a.last_time;
        a.last_time = frame_counter;
        frame_counter = std::fmod(frame_counter, full_length);
        frame_counter += a.start;
        if (a.last_frame != frame_counter)
        {
            a.last_frame = frame_counter;
            result |= TRANSLATE;
            if (a.size_x && a.size_y)
            {
                result |= SCALE;
                scale_s = 1.f / a.size_x;
                scale_t = 1.f / a.size_y;
                float x_frame = fmodf(frame_counter, (float)a.size_x);
                int y_frame = (int)(frame_counter / (float)a.size_x);
                float x_pos = x_frame * scale_s;
                float y_pos = y_frame * scale_t;
                off_s = (-0.5f + 0.5f * scale_s) + x_pos;
                off_t = (0.5f - 0.5f * scale_t) - y_pos;
            }
            else
            {
                scale_s = 1.f;
                scale_t = 1.f;
                float x_pos = frame_counter * scale_s;
                off_s = (-0.5f + 0.5f * scale_s) + x_pos;
                off_t = 0.f;
            }
        }
        return result;
    }

    // LLVOVolume::animateTextures: one face's matrix.
    bool upstream_face(Anim& a, double now, float te_rot, float te_s, float te_t, float out[16])
    {
        float off_s = 0.f, off_t = 0.f, scale_s = 1.f, scale_t = 1.f, rot = 0.f;
        int result = upstream_step(a, now, off_s, off_t, scale_s, scale_t);
        if (!result)
        {
            return false;
        }
        if (!(result & ROTATE)) rot = te_rot;
        if (!(result & TRANSLATE)) { off_s = 0.f; off_t = 0.f; }
        if (!(result & SCALE)) { scale_s = te_s; scale_t = te_t; }
        Mat4 tex_mat;
        tex_mat.setIdentity();
        float trans[3] = { off_s + 0.5f, off_t + 0.5f, 0.f };
        tex_mat.translate(-0.5f, -0.5f, 0.f);
        Quat quat = set_quat(rot, 0, 0, -1.f);
        tex_mat.mul(rotation(quat));
        tex_mat.mul(init_all_scale(scale_s, scale_t, 1.f));
        tex_mat.translate(trans[0], trans[1], trans[2]);
        std::memcpy(out, tex_mat.m, sizeof(tex_mat.m));
        return true;
    }

    // ---- decoders with one thing wrong ----
    enum Mutant { NONE, GLSL_MOD, ROT_SIGN, SCALE_FIRST, NO_START, TE_SCALE_GRID, NO_LOW_WORD, MUTANTS };
    const char* const MUTANT_NAMES[MUTANTS] = { "none", "glsl mod", "rotation sign", "scale first", "no start",
                                                "face scale with a grid", "no low word" };

    void decode(const float* d, double now, float out[16], Mutant mutant)
    {
        using namespace LLRHITexAnim;
        if (mutant == NONE)
        {
            matrixAt(d, now, out);
            return;
        }
        float e[16];
        std::memcpy(e, d, sizeof(e));
        if (mutant == NO_LOW_WORD)
        {
            e[TIME_LO] = 0.f;
            matrixAt(e, now, out);
            return;
        }
        if (mutant == ROT_SIGN)
        {
            std::swap(e[R01], e[R10]);
            matrixAt(e, now, out);
            return;
        }
        if (mutant == NO_START)
        {
            e[START] = 0.f;
            matrixAt(e, now, out);
            return;
        }
        float fc = rawCounter(e, now);
        if (mutant == GLSL_MOD)
        {
            fc = fc - e[FULL] * floorf(fc / e[FULL]);   // GLSL mod(): the sign of the divisor
            e[FULL] = 1e30f;                            // matrixForCounter's own fmodf leaves it
        }
        matrixForCounter(e, fc, out);
        if (mutant == TE_SCALE_GRID && e[SIZE_X] != 0.f && e[SIZE_Y] != 0.f)
        {
            out[0] *= e[SCALE_S] * e[SIZE_X];
            out[5] *= e[SCALE_T] * e[SIZE_Y];
        }
        if (mutant == SCALE_FIRST)
        {
            // S then R: the off-diagonal terms take the other axis' scale.
            const float sx = out[0] / (e[R00] != 0.f ? e[R00] : 1.f);
            const float sy = out[5] / (e[R11] != 0.f ? e[R11] : 1.f);
            out[1] = e[R01] * sx;
            out[4] = e[R10] * sy;
        }
    }

    bool same(const float a[16], const float b[16], float& worst)
    {
        bool ok = true;
        for (int k = 0; k < 16; ++k)
        {
            const float diff = fabsf(a[k] - b[k]);
            worst = std::max(worst, diff);
            ok &= diff <= 1e-4f * (1.f + fabsf(b[k]));    // texanim.cpp's TOLERANCE
        }
        return ok;
    }

    struct Run
    {
        long compared = 0;
        long bitwise = 0;
        long mismatches = 0;
        float worst = 0.f;
    };

    // One animation stepped frame by frame as upstream; at each step the
    // description made from the step before's anchor (what the verify does,
    // and what release/describe_faces keep) is turned into the matrix.
    void run_anim(std::mt19937& rng, Mutant mutant, Run& run)
    {
        std::uniform_real_distribution<float> u01(0.f, 1.f);
        Anim a;
        const int grid = (int)(u01(rng) * 4.f);
        if (grid == 0) { a.size_x = 1; a.size_y = 1; }
        else if (grid == 1) { a.size_x = 4; a.size_y = 4; }
        else if (grid == 2) { a.size_x = 0; a.size_y = 0; }
        else { a.size_x = (unsigned char)(1 + u01(rng) * 7.f); a.size_y = (unsigned char)(1 + u01(rng) * 7.f); }
        a.length = u01(rng) < 0.5f ? 0.f : (u01(rng) < 0.5f ? 1.f : 0.5f + u01(rng) * 20.f);
        a.start = u01(rng) < 0.5f ? 0.f : u01(rng) * 3.f;
        a.rate = (u01(rng) - 0.5f) * 6.f;
        if (a.rate == 0.f)
        {
            a.rate = 0.25f;
        }
        const float te_rot = u01(rng) < 0.4f ? 0.f : (u01(rng) - 0.5f) * 12.f;
        const float te_s = u01(rng) < 0.5f ? 1.f : (u01(rng) - 0.3f) * 8.f;
        const float te_t = u01(rng) < 0.5f ? 1.f : (u01(rng) - 0.3f) * 8.f;
        double now = (double)(u01(rng) * 2e5f) + (double)u01(rng);     // the frame clock (s since start)
        a.timer_start = now;
        a.last_time = u01(rng) < 0.5f ? 0.0 : (double)((u01(rng) - 0.5f) * 1000.f);

        const float full = a.length ? a.length : std::max(1.f, (float)(a.size_x * a.size_y));
        for (int frame = 0; frame < 200; ++frame)
        {
            // the anchor: the timer's state the step before left
            float d[16];
            LLRHITexAnim::describeScroll(d, a.rate, a.timer_start, (float)a.last_time, full, a.start, (float)a.size_x,
                                         (float)a.size_y, te_rot, te_s, te_t);
            CHECK(LLRHITexAnim::isDescription(d));
            now += 0.002 + (double)u01(rng) * 0.1;
            float up[16];
            if (!upstream_face(a, now, te_rot, te_s, te_t, up))
            {
                continue;
            }
            float mine[16];
            decode(d, now, mine, mutant);
            ++run.compared;
            if (!std::memcmp(mine, up, sizeof(up)))
            {
                ++run.bitwise;
            }
            if (!same(mine, up, run.worst))
            {
                ++run.mismatches;
            }
        }
    }
}

int main()
{
    // The two words keep a frame clock to well under a microsecond.
    for (double t : { 0.0, 1.0 / 3.0, 12345.678901234, 86400.123456789, 999999.999999 })
    {
        float hi, lo;
        LLRHITexAnim::splitTime(t, hi, lo);
        CHECK(fabs(((double)hi + (double)lo) - t) < 1e-9 * (1.0 + t));
    }

    // Matrices are no descriptions.
    {
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float projection[16] = { 1.2f, 0, 0, 0, 0, 2.1f, 0, 0, 0, 0, -1.0002f, -1, 0, 0, -0.20002f, 0 };
        CHECK(!LLRHITexAnim::isDescription(identity));
        CHECK(!LLRHITexAnim::isDescription(projection));
    }

    std::mt19937 rng(1940);
    Run right;
    for (int i = 0; i < 4000; ++i)
    {
        run_anim(rng, NONE, right);
    }
    std::printf("right: %ld compared, %ld bit for bit, %ld mismatches, largest difference %g\n", right.compared,
                right.bitwise, right.mismatches, right.worst);
    CHECK(right.compared > 100000);
    CHECK(right.mismatches == 0);
    // upstream's arithmetic: mostly the same floats (the rest: the anchor time's two words
    // round the F32 elapsed time the other way, one step of the counter's last bit)
    CHECK(right.bitwise * 100 >= right.compared * 90);
    CHECK(right.worst < 1e-3f);

    for (int mutant = GLSL_MOD; mutant < MUTANTS; ++mutant)
    {
        std::mt19937 mrng(77 + mutant);
        Run run;
        for (int i = 0; i < 1000; ++i)
        {
            run_anim(mrng, (Mutant)mutant, run);
        }
        std::printf("mutant %-24s %ld compared, %ld mismatches\n", MUTANT_NAMES[mutant], run.compared, run.mismatches);
        CHECK(run.mismatches > 0);
    }

    std::printf(failures ? "texanim_test: %d FAILED\n" : "texanim_test: OK\n", failures);
    return failures ? 1 : 0;
}
