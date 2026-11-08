/**
 * @file slj2k.cpp
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

// slj2k top level: ties parsing, tier-1, wavelet and output stages together.
//
// Built for plain x64 (no /arch:AVX2): this file checks the CPU and schedules the AVX2 kernels of
// slj2k_t1.cpp, slj2k_dwt.cpp and slj2k_output.cpp (slj2k_avx2.h) only when it can run them.
#include "slj2k.h"
#include "slj2k_codestream.h"
#include "threadpool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <new>
#include <type_traits>

namespace slj2k
{

bool cpuSupported()
{
    // The kernels are built for AVX2; the compiler may also use BMI1/BMI2, LZCNT and FMA there.
    static const bool ok = []()
    {
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7) return false;
        __cpuid(r, 1);
        const bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0, fma = (r[2] & (1 << 12)) != 0;
        if (!osxsave || !avx || !fma) return false;
        if ((_xgetbv(0) & 6) != 6) return false;  // the OS saves XMM and YMM state
        __cpuidex(r, 7, 0);
        const bool avx2 = (r[1] & (1 << 5)) != 0, bmi1 = (r[1] & (1 << 3)) != 0, bmi2 = (r[1] & (1 << 8)) != 0;
        __cpuid(r, 0x80000000);
        if ((unsigned)r[0] < 0x80000001u) return false;
        __cpuid(r, 0x80000001);
        const bool lzcnt = (r[2] & (1 << 5)) != 0;
        return avx2 && bmi1 && bmi2 && lzcnt;
    }();
    return ok;
}

Workspace::~Workspace() { release(); }

void Workspace::release()
{
    for (Block& b : mBlocks)
    {
        _aligned_free(b.p);
        b = Block();
    }
}

size_t Workspace::bytes() const
{
    size_t total = 0;
    for (const Block& b : mBlocks) total += b.bytes;
    return total;
}

void* Workspace::get(int slot, size_t bytes)
{
    Block& b = mBlocks[slot];
    if (b.bytes < bytes)
    {
        _aligned_free(b.p);
        b.p = _aligned_malloc(bytes, 64);
        if (!b.p)
        {
            b.bytes = 0;
            return nullptr;
        }
        // Not cleared: see get() in slj2k.h (a 1024x1024 image's planes are some 30 MB)
        b.bytes = bytes;
    }
    return b.p;
}

namespace
{

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

template <class F>
void forEach(ThreadPool* pool, int n, F&& fn)
{
    if (pool) pool->parallelFor(n, fn);
    else
        for (int i = 0; i < n; i++) fn(i);
}

struct BlockJob
{
    uint16_t comp, res, band;
    uint32_t block;
    uint32_t cost;
};

// Decode every code block into its place in the component planes.  Blocks without data are
// zero-filled, so the planes need no clearing beforehand.  Expensive blocks go first so that the
// worker threads finish together.
// False when a worker could not get its tier-1 scratch memory.
template <class T>
bool decodeBlocks(const Params& p, std::vector<Component>& comps, int maxRes, const uint8_t* tile, T* const* planes, int stride,
                  ThreadPool* pool)
{
    std::vector<BlockJob> jobs;
    for (int c = 0; c < p.ncomp; c++)
        for (int r = 0; r <= maxRes; r++)
            for (size_t b = 0; b < comps[c].res[r].bands.size(); b++)
            {
                const Band& bd = comps[c].res[r].bands[b];
                for (size_t k = 0; k < bd.blocks.size(); k++)
                {
                    const CodeBlock& cb = bd.blocks[k];
                    jobs.push_back({(uint16_t)c, (uint16_t)r, (uint16_t)b, (uint32_t)k, cb.passes ? cb.bytes + 1 : 0});
                }
            }
    std::stable_sort(jobs.begin(), jobs.end(), [](const BlockJob& a, const BlockJob& b) { return a.cost > b.cost; });

    std::atomic<bool> failed{false};
    forEach(pool, (int)jobs.size(), [&](int j) {
        // nothing in here may throw: it also runs on the pool's worker threads
        thread_local std::unique_ptr<avx2::T1Scratch, void (*)(avx2::T1Scratch*)> scratch(nullptr, avx2::t1Destroy);
        thread_local int32_t buf[64 * 64];
        const BlockJob& job = jobs[j];
        const Band& bd = comps[job.comp].res[job.res].bands[job.band];
        const CodeBlock& cb = bd.blocks[job.block];
        T* dst = planes[job.comp] + (size_t)(bd.offY + cb.y0) * stride + bd.offX + cb.x0;
        if (!cb.passes)
        {
            for (int y = 0; y < cb.h; y++, dst += stride) memset(dst, 0, sizeof(T) * cb.w);
            return;
        }
        if (!scratch) scratch.reset(avx2::t1Create());
        if (!scratch)
        {
            failed = true;
            return;
        }
        const avx2::T1Block blk{cb.chunks.data(), (uint32_t)cb.chunks.size(), cb.w, cb.h, cb.numbps, cb.passes, bd.orient};
        avx2::t1Decode(scratch.get(), tile, blk, buf);
        const int32_t* src = buf;
        for (int y = 0; y < cb.h; y++, dst += stride, src += cb.w)
        {
            for (int x = 0; x < cb.w; x++)
            {
                if constexpr (std::is_same_v<T, float>) dst[x] = (float)src[x] * (bd.step * 0.5f);
                else dst[x] = src[x] / 2;
            }
        }
    });
    return !failed;
}

// Inverse wavelet for all components at once.  A[c] holds the Mallat layout of component c;
// B[c] and C[c] are scratch planes (levels alternate between them: odd levels write B, even C; the
// buffer that receives the last level needs the full image size, the other only a quarter).
// strides = {A, B, C} row strides in elements.  result[c] / resultStride describe the plane that
// holds the synthesised image (A, B or C).  False when a band could not get its window buffers.
template <class T>
bool idwt(const Component* comps, int nc, int levels, T* const* A, T* const* B, T* const* C, const int strides[3], ThreadPool* pool,
          const T** result, int* resultStride)
{
    for (int c = 0; c < nc; c++) result[c] = A[c];
    int lowStride = strides[0];
    for (int r = 1; r <= levels; r++)
    {
        const bool toB = (r & 1) != 0;
        const int dstStride = toB ? strides[1] : strides[2];
        struct Task
        {
            avx2::DwtLevel<T> lv;
            int p0, p1;
        };
        std::vector<Task> tasks;
        for (int c = 0; c < nc; c++)
        {
            const Resolution& rs = comps[c].res[r];
            const Resolution& lo = comps[c].res[r - 1];
            avx2::DwtLevel<T> lv;
            lv.lowBase = result[c];
            lv.lowStride = lowStride;
            lv.hiBase = A[c];
            lv.hiStride = strides[0];
            lv.dst = toB ? B[c] : C[c];
            lv.dstStride = dstStride;
            lv.nw = rs.w;
            lv.nh = rs.h;
            lv.lw = lo.w;
            lv.lh = lo.h;
            const int pairs = std::max(1, std::min(avx2::kDwtMaxPairs, rs.w > 1024 ? 8 : 16));
            for (int p0 = 0; p0 < lo.h; p0 += pairs) tasks.push_back({lv, p0, std::min(lo.h, p0 + pairs)});
            result[c] = lv.dst;
        }
        lowStride = dstStride;
        std::atomic<bool> failed{false};
        forEach(pool, (int)tasks.size(), [&](int t) {
            if (!avx2::dwtBand(tasks[t].lv, tasks[t].p0, tasks[t].p1)) failed = true;
        });
        if (failed) return false;
    }
    *resultStride = lowStride;
    return true;
}

// Final stage: inverse component transform, level shift, clamp, interleave, flip vertically.
// planes[c] points at component c's samples (common row stride).
template <class T>
void writeOutput(const Params& p, const T* const* planes, int stride, int w, int h, uint8_t* out, ThreadPool* pool)
{
    avx2::OutputJob<T> job;
    for (int c = 0; c < 8; c++) job.planes[c] = c < p.ncomp ? planes[c] : nullptr;
    job.nc = p.ncomp;
    job.mct = p.mct && p.ncomp >= 3;
    job.stride = stride;
    job.w = w;
    job.h = h;
    job.out = out;
    constexpr int rowsPerTask = 16;
    const int tasks = (h + rowsPerTask - 1) / rowsPerTask;
    forEach(pool, tasks, [&](int t) { avx2::writeRows(job, t * rowsPerTask, std::min(h, (t + 1) * rowsPerTask)); });
}

template <class T>
bool reconstruct(const Params& p, std::vector<Component>& comps, int levels, const uint8_t* tile, const Options& opt, Image& out,
                 StageTimes* times)
{
    const int nc = p.ncomp;
    const int ow = comps[0].res[levels].w, oh = comps[0].res[levels].h;
    const int stride = ((ow + 15) & ~15) + 16;
    const size_t planeBytes = (size_t)stride * oh * sizeof(T) + 64;
    // Odd wavelet levels write plane B, even levels plane C.  The one that receives the last level
    // is image sized; the other only ever holds a half-size level (a quarter of the samples).
    const bool lastIsB = (levels & 1) != 0;
    const int innerW = levels >= 2 ? comps[0].res[levels - 1].w : 0, innerH = levels >= 2 ? comps[0].res[levels - 1].h : 0;
    const int innerStride = ((innerW + 15) & ~15) + 16;
    const size_t innerBytes = (size_t)innerStride * innerH * sizeof(T) + 64;
    const int strides[3] = {stride, lastIsB ? stride : innerStride, lastIsB ? innerStride : stride};
    // the planes are bounded before any of them is made (kMaxPlaneBytes, kMaxPlanesBytes)
    const size_t planesBytes =
        (size_t)nc * (planeBytes + (levels >= 1 ? (lastIsB ? planeBytes : innerBytes) : 0) + (levels >= 2 ? (lastIsB ? innerBytes : planeBytes) : 0));
    if (planeBytes > kMaxPlaneBytes || planesBytes > kMaxPlanesBytes) return false;

    Workspace local;
    Workspace& ws = opt.workspace ? *opt.workspace : local;
    T* A[8];
    T* B[8];
    T* C[8];
    // The vector code reads whole vectors past the right edge of a row: keep that padding at zero.
    // (The planes are not cleared otherwise: every sample left of it is written before it is read.)
    const auto clearPadding = [](T* plane, int width, int rowStride, int rows) {
        for (int y = 0; y < rows; y++) memset(plane + (size_t)y * rowStride + width, 0, sizeof(T) * (rowStride - width));
    };
    for (int c = 0; c < nc; c++)
    {
        A[c] = (T*)ws.get(c, planeBytes);
        B[c] = levels >= 1 ? (T*)ws.get(8 + c, lastIsB ? planeBytes : innerBytes) : A[c];
        C[c] = levels >= 2 ? (T*)ws.get(16 + c, lastIsB ? innerBytes : planeBytes) : B[c];
        if (!A[c] || !B[c] || !C[c]) return false;
        clearPadding(A[c], ow, stride, oh);
        if (levels >= 1)
        {
            if (lastIsB) clearPadding(B[c], ow, stride, oh);
            else clearPadding(B[c], innerW, innerStride, innerH);
        }
        if (levels >= 2)
        {
            if (lastIsB) clearPadding(C[c], innerW, innerStride, innerH);
            else clearPadding(C[c], ow, stride, oh);
        }
    }

    double t0 = nowMs();
    if (!decodeBlocks<T>(p, comps, levels, tile, A, stride, opt.pool)) return false;
    double t1 = nowMs();
    const T* result[8];
    int resultStride = stride;
    if (!idwt<T>(comps.data(), nc, levels, A, B, C, strides, opt.pool, result, &resultStride)) return false;
    double t2 = nowMs();
    out.width = ow;
    out.height = oh;
    out.channels = nc;
    const size_t bytes = (size_t)ow * oh * nc;
    uint8_t* dst = nullptr;
    if (opt.allocOutput) dst = opt.allocOutput(opt.allocContext, ow, oh, nc);
    out.inExternalBuffer = dst != nullptr;
    if (!dst && opt.outputBuffer && opt.outputCapacity >= bytes)
    {
        dst = opt.outputBuffer;
        out.inExternalBuffer = true;
    }
    if (!out.inExternalBuffer)
    {
        out.pixels.resize(bytes);
        dst = out.pixels.data();
    }
    writeOutput<T>(p, result, resultStride, ow, oh, dst, opt.pool);
    double t3 = nowMs();
    if (times)
    {
        times->tier1 += t1 - t0;
        times->dwt += t2 - t1;
        times->output += t3 - t2;
    }
    return true;
}

}  // namespace

Status decode(const uint8_t* data, size_t size, const Options& opt, Image& out, StageTimes* times)
{
    if (!cpuSupported()) return unsupported("CPU without AVX2");
    double t0 = nowMs();
    Params p;
    TileData tile;
    Status st = parseMainHeader(data, size, p, tile);
    if (st != Status::Ok) return st;
    if (opt.discard < 0 || opt.discard > p.levels) return Status::Unsupported;
    std::vector<Component> comps;
    st = buildStructure(p, comps);
    if (st != Status::Ok) return st;
    double t1 = nowMs();
    parsePackets(p, comps, tile.data, tile.size);
    double t2 = nowMs();
    if (times)
    {
        times->parse += t1 - t0;
        times->tier2 += t2 - t1;
    }

    const int levels = p.levels - opt.discard;
    try
    {
        const bool ok = p.reversible ? reconstruct<int32_t>(p, comps, levels, tile.data, opt, out, times)
                                     : reconstruct<float>(p, comps, levels, tile.data, opt, out, times);
        return ok ? Status::Ok : Status::NoMemory;
    }
    catch (const std::bad_alloc&)
    {
        return Status::NoMemory;
    }
}

}  // namespace slj2k
