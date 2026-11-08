/**
 * @file slj2k_dwt.cpp
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

// Inverse discrete wavelet transform: 9/7 irreversible (float) and 5/3 reversible (int32), AVX2.
//
// Each resolution level is synthesised in independent bands of row pairs.  A band
//   1. synthesises the horizontal direction of the low/high row pairs it needs (plus a halo of two
//      pairs on each side) into small cache-resident window buffers,
//   2. runs the vertical lifting steps inside the window, and
//   3. writes the interleaved output rows straight into the level's destination plane.
// Source data is therefore read once and the result written once per level; nothing is lifted
// in place in the big planes.  Levels alternate between two destination planes (B, C) so that a
// level never overwrites the previous level's LL rows it reads.
//
// The 9/7 constants and operation order follow OpenJPEG so that results agree to the last bit
// (separate multiply and add, no fused multiply-add).
//
// Built with /arch:AVX2: follow the rules in slj2k_avx2.h.  The levels are split into bands and
// scheduled by slj2k.cpp.
#include "slj2k_avx2.h"
#include <immintrin.h>
#include <cstdlib>
#include <cstring>

namespace slj2k
{
namespace avx2
{
namespace
{

constexpr float K = 1.230174105f;
constexpr float K_HIGH = 1.625732422f;  // OpenJPEG's high-pass gain (13318 / 8192, close to 2 / K), folded into the scaling
constexpr float ALPHA = 1.586134342f;
constexpr float BETA = 0.052980118f;
constexpr float GAMMA = -0.882911075f;
constexpr float DELTA = -0.443506852f;

struct F8
{
    using T = float;
    using V = __m256;
    static constexpr bool kFourStep = true;
    static V load(const T* p) { return _mm256_loadu_ps(p); }
    static void store(T* p, V v) { _mm256_storeu_ps(p, v); }
    static V scaleLow(V x) { return _mm256_mul_ps(x, _mm256_set1_ps(K)); }
    static V scaleHigh(V x) { return _mm256_mul_ps(x, _mm256_set1_ps(K_HIGH)); }
    static V lift(V x, V a, V b, float c) { return _mm256_add_ps(x, _mm256_mul_ps(_mm256_add_ps(a, b), _mm256_set1_ps(c))); }
    static V e1(V x, V a, V b) { return lift(x, a, b, DELTA); }
    static V o1(V x, V a, V b) { return lift(x, a, b, GAMMA); }
    static V e2(V x, V a, V b) { return lift(x, a, b, BETA); }
    static V o2(V x, V a, V b) { return lift(x, a, b, ALPHA); }
};

struct I8
{
    using T = int32_t;
    using V = __m256i;
    static constexpr bool kFourStep = false;
    static V load(const T* p) { return _mm256_loadu_si256((const __m256i*)p); }
    static void store(T* p, V v) { _mm256_storeu_si256((__m256i*)p, v); }
    static V scaleLow(V x) { return x; }
    static V scaleHigh(V x) { return x; }
    static V e1(V x, V, V) { return x; }
    static V o1(V x, V, V) { return x; }
    // even: x - ((a + b + 2) >> 2)
    static V e2(V x, V a, V b)
    {
        V s = _mm256_add_epi32(_mm256_add_epi32(a, b), _mm256_set1_epi32(2));
        return _mm256_sub_epi32(x, _mm256_srai_epi32(s, 2));
    }
    // odd: x + ((a + b) >> 1)
    static V o2(V x, V a, V b) { return _mm256_add_epi32(x, _mm256_srai_epi32(_mm256_add_epi32(a, b), 1)); }
};

template <class T>
struct AlignedArray
{
    T* p = nullptr;
    size_t n = 0;
    AlignedArray() = default;
    AlignedArray(const AlignedArray&) = delete;
    ~AlignedArray() { _aligned_free(p); }
    // False (and empty) when the memory cannot be had.
    bool resize(size_t count)
    {
        if (count <= n) return true;
        _aligned_free(p);
        n = 0;
        p = (T*)_aligned_malloc(count * sizeof(T), 64);
        if (!p) return false;
        memset(p, 0, count * sizeof(T));
        n = count;
        return true;
    }
};

// Source rows of a window come from big, cold planes in strides of whole pages, so the hardware
// prefetcher restarts for every row; ask for the rows a few steps ahead.
template <class T>
inline void prefetchRange(const T* p, int count)
{
    const char* c = (const char*)p;
    for (int off = 0; off < count * (int)sizeof(T); off += 64) _mm_prefetch(c + off, _MM_HINT_T0);
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

// How the vertical scaling of a row is folded into the horizontal pass's final store.
enum class VScale { None, Low, High };

// Synthesise one row horizontally.  The low half comes from `low` (nl samples), the high half from
// `high` (nh samples); the interleaved result (nl + nh samples, rounded up to whole vectors) goes
// to dst.  lbuf / hbuf are scratch rows; hbuf[-1] must be addressable.
template <class O>
void synthRow(const typename O::T* low, int nl, const typename O::T* high, int nh, typename O::T* dst, typename O::T* lbuf,
              typename O::T* hbuf, VScale vs)
{
    using T = typename O::T;
    const int n = nl + nh;
    if (n == 1)  // a single column: no horizontal filtering, only the vertical scaling applies
    {
        T v = low[0];
        if constexpr (O::kFourStep) v = vs == VScale::Low ? v * K : (vs == VScale::High ? v * K_HIGH : v);
        dst[0] = v;
        return;
    }
    for (int i = 0; i < nl; i += 8) O::store(lbuf + i, O::scaleLow(O::load(low + i)));
    for (int i = 0; i < nh; i += 8) O::store(hbuf + i, O::scaleHigh(O::load(high + i)));

    auto evenStep = [&](auto fn) {
        hbuf[-1] = hbuf[0];
        if (nl > nh) hbuf[nh] = hbuf[nh - 1];
        for (int i = 0; i < nl; i += 8) O::store(lbuf + i, fn(O::load(lbuf + i), O::load(hbuf + i - 1), O::load(hbuf + i)));
    };
    auto oddStep = [&](auto fn) {
        if (nl == nh) lbuf[nl] = lbuf[nl - 1];
        for (int i = 0; i < nh; i += 8) O::store(hbuf + i, fn(O::load(hbuf + i), O::load(lbuf + i), O::load(lbuf + i + 1)));
    };
    if constexpr (O::kFourStep)
    {
        evenStep([](auto x, auto a, auto b) { return O::e1(x, a, b); });
        oddStep([](auto x, auto a, auto b) { return O::o1(x, a, b); });
    }
    evenStep([](auto x, auto a, auto b) { return O::e2(x, a, b); });
    oddStep([](auto x, auto a, auto b) { return O::o2(x, a, b); });

    // vertical scaling (9/7 only) is applied while interleaving
    if constexpr (O::kFourStep)
    {
        if (vs != VScale::None)
        {
            const __m256 s = _mm256_set1_ps(vs == VScale::Low ? K : K_HIGH);
            for (int i = 0; i < nl; i += 8) _mm256_storeu_ps(lbuf + i, _mm256_mul_ps(_mm256_loadu_ps(lbuf + i), s));
            for (int i = 0; i < nh; i += 8) _mm256_storeu_ps(hbuf + i, _mm256_mul_ps(_mm256_loadu_ps(hbuf + i), s));
        }
    }

    int i = 0;
    for (; i + 8 <= nh; i += 8)
    {
        __m256i l = _mm256_loadu_si256((const __m256i*)(lbuf + i));
        __m256i h = _mm256_loadu_si256((const __m256i*)(hbuf + i));
        __m256i lo = _mm256_unpacklo_epi32(l, h);
        __m256i hi = _mm256_unpackhi_epi32(l, h);
        _mm256_storeu_si256((__m256i*)(dst + 2 * i), _mm256_permute2x128_si256(lo, hi, 0x20));
        _mm256_storeu_si256((__m256i*)(dst + 2 * i + 8), _mm256_permute2x128_si256(lo, hi, 0x31));
    }
    for (; i < nh; i++)
    {
        dst[2 * i] = lbuf[i];
        dst[2 * i + 1] = hbuf[i];
    }
    if (nl > nh) dst[2 * nh] = lbuf[nh];
}

constexpr int kMaxWindowPairs = kDwtMaxPairs + 4;  // band + halo

template <class O>
bool bandTask(const DwtLevel<typename O::T>& lv, int p0, int p1)
{
    using T = typename O::T;
    const int hh = lv.nh - lv.lh, hw = lv.nw - lv.lw;
    const int wstride = ((lv.nw + 7) & ~7) + 16;
    thread_local AlignedArray<T> wl, wh, lb, hb, tmp;
    if (p1 - p0 > kDwtMaxPairs || !wl.resize((size_t)kMaxWindowPairs * wstride) || !wh.resize((size_t)kMaxWindowPairs * wstride) ||
        !lb.resize(lv.nw / 2 + 64) || !hb.resize(lv.nw / 2 + 64) || !tmp.resize(wstride))
    {
        return false;
    }
    T* lbuf = lb.p;
    T* hbuf = hb.p + 16;

    const int q0 = imax(0, p0 - 2);
    const int qL = imin(lv.lh, p1 + 2);  // exclusive window ends
    const int qH = imin(hh, p1 + 2);
    auto WL = [&](int i) { return wl.p + (size_t)(i - q0) * wstride; };
    auto WH = [&](int i) { return wh.p + (size_t)(i - q0) * wstride; };

    const bool single = lv.nh == 1;  // one row: no vertical filtering, no scaling
    constexpr int kAhead = 3;
    auto prefetchL = [&](int i) {
        prefetchRange(lv.lowBase + (size_t)i * lv.lowStride, lv.lw);
        prefetchRange(lv.hiBase + (size_t)i * lv.hiStride + lv.lw, hw);
    };
    auto prefetchH = [&](int i) { prefetchRange(lv.hiBase + (size_t)(lv.lh + i) * lv.hiStride, lv.nw); };
    for (int k = 0; k < kAhead; k++)
    {
        if (q0 + k < qL) prefetchL(q0 + k);
        if (q0 + k < qH) prefetchH(q0 + k);
    }
    for (int i = q0; i < qL; i++)
    {
        if (i + kAhead < qL) prefetchL(i + kAhead);
        synthRow<O>(lv.lowBase + (size_t)i * lv.lowStride, lv.lw, lv.hiBase + (size_t)i * lv.hiStride + lv.lw, hw, WL(i), lbuf, hbuf,
                    single ? VScale::None : VScale::Low);
    }
    for (int i = q0; i < qH; i++)
    {
        if (i + kAhead < qH) prefetchH(i + kAhead);
        const T* row = lv.hiBase + (size_t)(lv.lh + i) * lv.hiStride;
        synthRow<O>(row, lv.lw, row + lv.lw, hw, WH(i), lbuf, hbuf, VScale::High);
    }
    const int nv = (lv.nw + 7) & ~7;
    if (single)
    {
        for (int x = 0; x < nv; x += 8) O::store(lv.dst + x, O::load(WL(0) + x));
        return true;
    }

    // neighbours with the symmetric extension of the image edges; indices that fall outside the
    // window can only feed halo rows whose results are never used
    auto Hat = [&](int i) { return WH(clampi(clampi(i, 0, hh - 1), q0, qH - 1)); };
    auto Lat = [&](int i) { return WL(clampi(clampi(i, 0, lv.lh - 1), q0, qL - 1)); };

    if constexpr (O::kFourStep)
    {
        for (int i = q0; i < qL; i++)
        {
            T* l = WL(i);
            const T* h0 = Hat(i - 1);
            const T* h1 = Hat(i);
            for (int x = 0; x < nv; x += 8) O::store(l + x, O::e1(O::load(l + x), O::load(h0 + x), O::load(h1 + x)));
        }
        for (int i = q0; i < qH; i++)
        {
            T* h = WH(i);
            const T* l0 = Lat(i);
            const T* l1 = Lat(i + 1);
            for (int x = 0; x < nv; x += 8) O::store(h + x, O::o1(O::load(h + x), O::load(l0 + x), O::load(l1 + x)));
        }
    }

    const int e1 = imin(p1, lv.lh);  // even output rows of this band: pairs [p0, e1)
    const int o1 = imin(p1, hh);
    for (int i = p0; i < e1; i++)
    {
        T* d = lv.dst + (size_t)(2 * i) * lv.dstStride;
        const T* l = WL(i);
        const T* h0 = Hat(i - 1);
        const T* h1 = Hat(i);
        for (int x = 0; x < nv; x += 8) O::store(d + x, O::e2(O::load(l + x), O::load(h0 + x), O::load(h1 + x)));
    }
    // the odd row of pair p1-1 needs the even row of pair p1, which belongs to the next band
    const bool needExtra = o1 > p0 && p1 < lv.lh && (p1 - 1) < hh;
    if (needExtra)
    {
        const T* l = Lat(p1);
        const T* h0 = Hat(p1 - 1);
        const T* h1 = Hat(p1);
        for (int x = 0; x < nv; x += 8) O::store(tmp.p + x, O::e2(O::load(l + x), O::load(h0 + x), O::load(h1 + x)));
    }
    for (int i = p0; i < o1; i++)
    {
        T* d = lv.dst + (size_t)(2 * i + 1) * lv.dstStride;
        const T* h = WH(i);
        const T* a0 = lv.dst + (size_t)(2 * i) * lv.dstStride;
        const int j = imin(i + 1, lv.lh - 1);  // mirrored at the bottom edge
        const T* a1 = j < p1 ? lv.dst + (size_t)(2 * j) * lv.dstStride : tmp.p;
        for (int x = 0; x < nv; x += 8) O::store(d + x, O::o2(O::load(h + x), O::load(a0 + x), O::load(a1 + x)));
    }
    return true;
}

}  // namespace

bool dwtBand(const DwtLevel<float>& lv, int p0, int p1) { return bandTask<F8>(lv, p0, p1); }
bool dwtBand(const DwtLevel<int32_t>& lv, int p0, int p1) { return bandTask<I8>(lv, p0, p1); }

}  // namespace avx2
}  // namespace slj2k
