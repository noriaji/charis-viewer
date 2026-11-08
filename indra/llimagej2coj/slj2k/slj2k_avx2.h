/**
 * @file slj2k_avx2.h
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

// Entry points of the AVX2 kernels: tier-1 (slj2k_t1.cpp), wavelet (slj2k_dwt.cpp) and output
// (slj2k_output.cpp).  Only those three files are compiled with /arch:AVX2; everything else in
// slj2k (parsing, scheduling, the thread pool, the CPU check) is built for plain x64.
//
// Rules for the AVX2 files, so that no AVX2 instruction can run on a CPU without it:
//  - include only this header and system headers whose inline functions are not used
//    (<immintrin.h>, <cstring>, <cstdlib>, <cstdint>); no std containers, std::min/max,
//    std::function, mutexes, operator new: an inline function or template used there is emitted
//    as an AVX2 COMDAT, and the linker may pick that copy for every caller in the viewer;
//  - keep every helper in an anonymous namespace, and the exported functions in slj2k::avx2;
//  - no namespace-scope object with a dynamic initializer (it would run at program start);
//  - call the functions below only after slj2k::cpuSupported() returned true (slj2k::decode()
//    checks it).
#pragma once
#include <cstddef>
#include <cstdint>

namespace slj2k
{

struct Chunk
{
    uint32_t offset;  // into the tile data
    uint32_t length;
};

namespace avx2
{

// ---- tier-1 (slj2k_t1.cpp) ----

struct T1Scratch;
// nullptr when the memory cannot be had
T1Scratch* t1Create();
void t1Destroy(T1Scratch*);

struct T1Block
{
    const Chunk* chunks;  // the code block's data ranges in the tile
    uint32_t nchunks;
    int w, h;
    int numbps;  // magnitude bit planes present
    int passes;
    int orient;  // 0 LL, 1 HL, 2 LH, 3 HH
};
// Decode one block into signed values with one fractional bit (value * 2), w * h samples.
void t1Decode(T1Scratch*, const uint8_t* tile, const T1Block& cb, int32_t* out);

// ---- inverse wavelet (slj2k_dwt.cpp) ----

// One resolution level of one component.
template <class T>
struct DwtLevel
{
    const T* lowBase;  // LL rows (lw wide)
    int lowStride;
    const T* hiBase;  // Mallat plane holding HL / LH / HH
    int hiStride;
    T* dst;
    int dstStride;
    int nw, nh, lw, lh;
};
// Synthesise the output row pairs [p0, p1) of the level (at most kDwtMaxPairs of them).  False
// when the window buffers cannot be allocated.
constexpr int kDwtMaxPairs = 20;
bool dwtBand(const DwtLevel<float>& lv, int p0, int p1);
bool dwtBand(const DwtLevel<int32_t>& lv, int p0, int p1);

// ---- output (slj2k_output.cpp) ----

template <class T>
struct OutputJob
{
    const T* planes[8];  // component c's samples (common row stride)
    int nc;
    bool mct;  // inverse colour transform on the first three components
    int stride, w, h;
    uint8_t* out;  // w * h * nc bytes, interleaved, bottom row first
};
// Inverse component transform, level shift, clamp, interleave and vertical flip of output rows
// [r0, r1) (counted from the start of `out`).
void writeRows(const OutputJob<float>& job, int r0, int r1);
void writeRows(const OutputJob<int32_t>& job, int r0, int r1);

}  // namespace avx2
}  // namespace slj2k
