/**
 * @file slj2k_output.cpp
 *
 * slj2k is Charis Viewer's own JPEG 2000 decoder, written from ITU-T T.800; it
 * holds no OpenJPEG source code. Where it follows OpenJPEG (constants and order
 * of operations, so that the pixels agree), the comments say so
 * (licenses-charis.txt, section 1).
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

// Output stage: inverse component transform, level shift, clamp to 8 bit and interleave (AVX2).
//
// Each output row is built in two steps that stay in L1: the planes are turned into clamped byte
// rows (applying the colour transform to the first three components), then the byte rows are
// interleaved into the viewer's pixel layout.  The image is written bottom row first, as
// llimagej2coj does.
//
// Built with /arch:AVX2: follow the rules in slj2k_avx2.h.  The rows are split into tasks by
// slj2k.cpp.
#include "slj2k_avx2.h"
#include <immintrin.h>

namespace slj2k
{
namespace avx2
{
namespace
{

// 16 int32 values (already level shifted) -> 16 bytes with unsigned saturation.
inline __m128i packBytes(__m256i a, __m256i b)
{
    __m256i w = _mm256_packs_epi32(a, b);                  // 16-bit, lanes interleaved
    w = _mm256_permute4x64_epi64(w, _MM_SHUFFLE(3, 1, 2, 0));  // restore pixel order
    __m256i bytes = _mm256_packus_epi16(w, _mm256_setzero_si256());
    return _mm_unpacklo_epi64(_mm256_castsi256_si128(bytes), _mm256_extracti128_si256(bytes, 1));
}

inline __m256i roundShift(__m256 v)
{
    // clamping first keeps huge values from wrapping in the conversion; the final clamp is unchanged
    v = _mm256_min_ps(_mm256_max_ps(v, _mm256_set1_ps(-1024.0f)), _mm256_set1_ps(1023.0f));
    return _mm256_add_epi32(_mm256_cvtps_epi32(v), _mm256_set1_epi32(128));
}

inline __m128i floatBytes(const float* p) { return packBytes(roundShift(_mm256_load_ps(p)), roundShift(_mm256_load_ps(p + 8))); }
inline __m128i intBytes(const int32_t* p)
{
    const __m256i k = _mm256_set1_epi32(128);
    return packBytes(_mm256_add_epi32(_mm256_load_si256((const __m256i*)p), k),
                     _mm256_add_epi32(_mm256_load_si256((const __m256i*)(p + 8)), k));
}

// ICT: R = Y + 1.402 Cr, G = Y - 0.34413 Cb - 0.71414 Cr, B = Y + 1.772 Cb (OpenJPEG's operation order)
inline void ictBytes(const float* y, const float* u, const float* v, __m128i& r, __m128i& g, __m128i& b)
{
    const __m256 c1 = _mm256_set1_ps(1.402f), c2 = _mm256_set1_ps(0.34413f), c3 = _mm256_set1_ps(0.71414f), c4 = _mm256_set1_ps(1.772f);
    __m256i rr[2], gg[2], bb[2];
    for (int i = 0; i < 2; i++)
    {
        __m256 yv = _mm256_load_ps(y + 8 * i), uv = _mm256_load_ps(u + 8 * i), vv = _mm256_load_ps(v + 8 * i);
        rr[i] = roundShift(_mm256_add_ps(yv, _mm256_mul_ps(vv, c1)));
        gg[i] = roundShift(_mm256_sub_ps(_mm256_sub_ps(yv, _mm256_mul_ps(uv, c2)), _mm256_mul_ps(vv, c3)));
        bb[i] = roundShift(_mm256_add_ps(yv, _mm256_mul_ps(uv, c4)));
    }
    r = packBytes(rr[0], rr[1]);
    g = packBytes(gg[0], gg[1]);
    b = packBytes(bb[0], bb[1]);
}

// RCT: G = Y - ((Cb + Cr) >> 2), R = Cr + G, B = Cb + G
inline void rctBytes(const int32_t* y, const int32_t* u, const int32_t* v, __m128i& r, __m128i& g, __m128i& b)
{
    const __m256i k = _mm256_set1_epi32(128);
    __m256i rr[2], gg[2], bb[2];
    for (int i = 0; i < 2; i++)
    {
        __m256i yv = _mm256_load_si256((const __m256i*)(y + 8 * i)), uv = _mm256_load_si256((const __m256i*)(u + 8 * i)),
                vv = _mm256_load_si256((const __m256i*)(v + 8 * i));
        __m256i gv = _mm256_sub_epi32(yv, _mm256_srai_epi32(_mm256_add_epi32(uv, vv), 2));
        rr[i] = _mm256_add_epi32(_mm256_add_epi32(vv, gv), k);
        gg[i] = _mm256_add_epi32(gv, k);
        bb[i] = _mm256_add_epi32(_mm256_add_epi32(uv, gv), k);
    }
    r = packBytes(rr[0], rr[1]);
    g = packBytes(gg[0], gg[1]);
    b = packBytes(bb[0], bb[1]);
}

inline void transformBytes(const float* y, const float* u, const float* v, __m128i& r, __m128i& g, __m128i& b)
{
    ictBytes(y, u, v, r, g, b);
}
inline void transformBytes(const int32_t* y, const int32_t* u, const int32_t* v, __m128i& r, __m128i& g, __m128i& b)
{
    rctBytes(y, u, v, r, g, b);
}
inline __m128i plainBytes(const float* p) { return floatBytes(p); }
inline __m128i plainBytes(const int32_t* p) { return intBytes(p); }

// RGB24 interleave of 16 pixels (48 bytes).
inline void interleave3(const __m128i& r, const __m128i& g, const __m128i& b, uint8_t* o)
{
#define Z (char)0x80
    const __m128i r0 = _mm_setr_epi8(0, Z, Z, 1, Z, Z, 2, Z, Z, 3, Z, Z, 4, Z, Z, 5);
    const __m128i g0 = _mm_setr_epi8(Z, 0, Z, Z, 1, Z, Z, 2, Z, Z, 3, Z, Z, 4, Z, Z);
    const __m128i b0 = _mm_setr_epi8(Z, Z, 0, Z, Z, 1, Z, Z, 2, Z, Z, 3, Z, Z, 4, Z);
    const __m128i r1 = _mm_setr_epi8(Z, Z, 6, Z, Z, 7, Z, Z, 8, Z, Z, 9, Z, Z, 10, Z);
    const __m128i g1 = _mm_setr_epi8(5, Z, Z, 6, Z, Z, 7, Z, Z, 8, Z, Z, 9, Z, Z, 10);
    const __m128i b1 = _mm_setr_epi8(Z, 5, Z, Z, 6, Z, Z, 7, Z, Z, 8, Z, Z, 9, Z, Z);
    const __m128i r2 = _mm_setr_epi8(Z, 11, Z, Z, 12, Z, Z, 13, Z, Z, 14, Z, Z, 15, Z, Z);
    const __m128i g2 = _mm_setr_epi8(Z, Z, 11, Z, Z, 12, Z, Z, 13, Z, Z, 14, Z, Z, 15, Z);
    const __m128i b2 = _mm_setr_epi8(10, Z, Z, 11, Z, Z, 12, Z, Z, 13, Z, Z, 14, Z, Z, 15);
#undef Z
    _mm_storeu_si128((__m128i*)o, _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r, r0), _mm_shuffle_epi8(g, g0)), _mm_shuffle_epi8(b, b0)));
    _mm_storeu_si128((__m128i*)(o + 16),
                     _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r, r1), _mm_shuffle_epi8(g, g1)), _mm_shuffle_epi8(b, b1)));
    _mm_storeu_si128((__m128i*)(o + 32),
                     _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r, r2), _mm_shuffle_epi8(g, g2)), _mm_shuffle_epi8(b, b2)));
}

inline void interleave4(const __m128i& r, const __m128i& g, const __m128i& b, const __m128i& a, uint8_t* o)
{
    __m128i rgLo = _mm_unpacklo_epi8(r, g), rgHi = _mm_unpackhi_epi8(r, g);
    __m128i baLo = _mm_unpacklo_epi8(b, a), baHi = _mm_unpackhi_epi8(b, a);
    _mm_storeu_si128((__m128i*)o, _mm_unpacklo_epi16(rgLo, baLo));
    _mm_storeu_si128((__m128i*)(o + 16), _mm_unpackhi_epi16(rgLo, baLo));
    _mm_storeu_si128((__m128i*)(o + 32), _mm_unpacklo_epi16(rgHi, baHi));
    _mm_storeu_si128((__m128i*)(o + 48), _mm_unpackhi_epi16(rgHi, baHi));
}

template <class T>
void writeRowsImpl(const OutputJob<T>& job, int r0, int r1)
{
    const int nc = job.nc, w = job.w, h = job.h;
    const int wpad = (w + 15) & ~15;
    alignas(32) uint8_t tmp[8][16384 + 16];
    for (int r = r0; r < r1; r++)
    {
        const int y = h - 1 - r;
        const T* row[8];
        for (int c = 0; c < nc; c++) row[c] = job.planes[c] + (size_t)y * job.stride;
        uint8_t* o = job.out + (size_t)r * w * nc;

        // planes -> byte rows
        for (int x = 0; x < wpad; x += 16)
        {
            int c0 = 0;
            if (job.mct)
            {
                __m128i rr, gg, bb;
                transformBytes(row[0] + x, row[1] + x, row[2] + x, rr, gg, bb);
                _mm_store_si128((__m128i*)(tmp[0] + x), rr);
                _mm_store_si128((__m128i*)(tmp[1] + x), gg);
                _mm_store_si128((__m128i*)(tmp[2] + x), bb);
                c0 = 3;
            }
            for (int c = c0; c < nc; c++) _mm_store_si128((__m128i*)(tmp[c] + x), plainBytes(row[c] + x));
        }

        // byte rows -> interleaved pixels
        int x = 0;
        if (nc == 3)
            for (; x + 16 <= w; x += 16)
                interleave3(_mm_load_si128((const __m128i*)(tmp[0] + x)), _mm_load_si128((const __m128i*)(tmp[1] + x)),
                            _mm_load_si128((const __m128i*)(tmp[2] + x)), o + 3 * x);
        else if (nc == 4)
            for (; x + 16 <= w; x += 16)
                interleave4(_mm_load_si128((const __m128i*)(tmp[0] + x)), _mm_load_si128((const __m128i*)(tmp[1] + x)),
                            _mm_load_si128((const __m128i*)(tmp[2] + x)), _mm_load_si128((const __m128i*)(tmp[3] + x)), o + 4 * x);
        for (; x < w; x++)
            for (int c = 0; c < nc; c++) o[(size_t)x * nc + c] = tmp[c][x];
    }
}

}  // namespace

void writeRows(const OutputJob<float>& job, int r0, int r1) { writeRowsImpl(job, r0, r1); }
void writeRows(const OutputJob<int32_t>& job, int r0, int r1) { writeRowsImpl(job, r0, r1); }

}  // namespace avx2
}  // namespace slj2k
