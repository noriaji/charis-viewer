/**
 * @file slj2k_t1.cpp
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

// Tier-1: MQ arithmetic decoder and the three bit-plane coding passes (ITU-T T.800 Annex C/D).
// Built with /arch:AVX2 (BMI, LZCNT): follow the rules in slj2k_avx2.h.
#include "slj2k_avx2.h"
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#if defined(SLJ2K_COUNT_DECISIONS) || defined(SLJ2K_REPLAY)
#include <vector>  // diagnostics builds of the bench tools only
#endif

namespace slj2k
{
namespace avx2
{
namespace
{

// ---- MQ coder tables ---------------------------------------------------------------------

struct MqState
{
    uint16_t qe;
    uint8_t nmps, nlps, sw;
};

const MqState kMq[47] = {
    {0x5601, 1, 1, 1},   {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},   {0x0AC1, 4, 12, 0},  {0x0521, 5, 29, 0},
    {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1},   {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},  {0x3801, 10, 14, 0},
    {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0}, {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0}, {0x1C01, 25, 22, 0},
    {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0},
    {0x02A1, 36, 33, 0}, {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}};

// Combined (state, mps) entries: index = state * 2 + mps.
struct MqEntry
{
    uint16_t qe;
    uint8_t nmps, nlps;  // next combined index
};

// The tables are filled by buildTables() on the first t1Create(), not by constructors: a dynamic
// initializer of this file would run AVX2 code at program start.
struct MqTable
{
    MqEntry e[94];
    void build()
    {
        for (int s = 0; s < 47; s++)
            for (int m = 0; m < 2; m++)
            {
                MqEntry& x = e[s * 2 + m];
                x.qe = kMq[s].qe;
                x.nmps = (uint8_t)(kMq[s].nmps * 2 + m);
                x.nlps = (uint8_t)(kMq[s].nlps * 2 + (m ^ kMq[s].sw));
            }
    }
};
MqTable kTable;

enum { CTX_SC = 9, CTX_MR = 14, CTX_AGG = 17, CTX_UNI = 18, CTX_COUNT = 19 };

#ifdef SLJ2K_COUNT_DECISIONS
unsigned long long gDecisionCount = 0;
std::vector<uint8_t> gCtxTrace;  // context of every decision (diagnostics for bench_block)
#define COUNT_DECISION() (++gDecisionCount, gCtxTrace.size() < (1u << 22) ? (void)gCtxTrace.push_back((uint8_t)ctx) : (void)0)
#else
#define COUNT_DECISION() ((void)0)
#endif

#ifdef SLJ2K_REPLAY
uint8_t* gReplayBits = nullptr;
size_t gReplayPos = 0;
std::vector<uint8_t> gRecordBits;
#endif

struct Mq
{
    const uint8_t* bp;
    uint32_t c, a;
    int ct;
    uint8_t cx[CTX_COUNT];

    void resetContexts()
    {
        memset(cx, 0, sizeof(cx));
        cx[0] = 4 * 2;
        cx[CTX_AGG] = 3 * 2;
        cx[CTX_UNI] = 46 * 2;
    }

    // `data` must be followed by two 0xFF bytes.
    void init(const uint8_t* data)
    {
        bp = data;
        c = (uint32_t)*bp << 16;
        byteIn();
        c <<= 7;
        ct -= 7;
        a = 0x8000;
    }

    __forceinline void byteIn()
    {
        if (*bp == 0xFF)
        {
            if (bp[1] > 0x8F)
            {
                c += 0xFF00;
                ct = 8;
            }
            else
            {
                bp++;
                c += (uint32_t)*bp << 9;
                ct = 7;
            }
        }
        else
        {
            bp++;
            c += (uint32_t)*bp << 8;
            ct = 8;
        }
    }

    // Branch-free formulation of the T.800 DECODE procedure (the interval/exchange cases are
    // folded into selects, so a mispredicted branch is only paid when a byte has to be fetched).
    //   region1: the code value lies in the LPS sub-interval;  exch: the conditional exchange applies.
    //   Decoded symbol = MPS xor (region1 xor exch); the context moves to its LPS successor on that
    //   flip, to its MPS successor when only renormalisation happens, and stays put otherwise.
    // The coder state lives in locals while the context byte is updated: a byte store may alias
    // the member fields, which would otherwise force them to be reloaded.
    __forceinline int decode(int ctx)
    {
#ifdef SLJ2K_REPLAY
        if (gReplayBits) return gReplayBits[gReplayPos++];
#endif
        const uint32_t idx = cx[ctx];
        const MqEntry& e = kTable.e[idx];
        const uint32_t qe = e.qe;
        uint32_t a1 = a, c1 = c;
        int ct1 = ct;
        const uint32_t t = a1 - qe;
        const uint32_t region1 = (c1 >> 16) < qe;
        const uint32_t exch = t < qe;
        const uint32_t flip = region1 ^ exch;
        const uint32_t renormNeeded = region1 | (t < 0x8000);
        const uint32_t m1 = 0u - region1;  // all ones in region 1
        a1 = (qe & m1) | (t & ~m1);
        c1 -= (qe << 16) & ~m1;
        const uint32_t next = flip ? e.nlps : e.nmps;
        const uint32_t newIdx = renormNeeded ? next : idx;
        const int shift = (int)_lzcnt_u32(a1) - 16;  // 0 when A is already normalised
        if (shift <= ct1)
        {
            a1 <<= shift;
            c1 <<= shift;
            ct1 -= shift;
        }
        else  // rare: the bit counter runs out, fetch bytes
        {
            int left = shift - ct1;
            a1 <<= ct1;
            c1 <<= ct1;
            ct = 0;
            c = c1;
            do
            {
                byteIn();
                const int k = left < ct ? left : ct;
                a1 <<= k;
                c <<= k;
                ct -= k;
                left -= k;
            } while (left > 0);
            c1 = c;
            ct1 = ct;
        }
        a = a1;
        c = c1;
        ct = ct1;
        cx[ctx] = (uint8_t)newIdx;
        const int dbit = (int)((idx & 1) ^ flip);
#ifdef SLJ2K_REPLAY
        gRecordBits.push_back((uint8_t)dbit);
#endif
        return dbit;
    }
};

// ---- Context tables ------------------------------------------------------------------------

// Per-sample flags: significance of the 8 neighbours, signs of the 4 edge neighbours, own state.
enum : uint16_t
{
    F_NW = 1 << 0, F_N = 1 << 1, F_NE = 1 << 2, F_W = 1 << 3, F_E = 1 << 4, F_SW = 1 << 5, F_S = 1 << 6, F_SE = 1 << 7,
    F_SGN_N = 1 << 8, F_SGN_W = 1 << 9, F_SGN_E = 1 << 10, F_SGN_S = 1 << 11,
    F_SIG = 1 << 12, F_VISIT = 1 << 13, F_REFINE = 1 << 14,
    F_NEIGH = 0xFF
};

struct Luts
{
    uint8_t zc[4][256];
    uint8_t sc[4096];  // ctx | (xor << 7), indexed by flags & 0xFFF

    void build()
    {
        for (int o = 0; o < 4; o++)
            for (int f = 0; f < 256; f++)
            {
                int h = ((f & F_W) != 0) + ((f & F_E) != 0);
                int v = ((f & F_N) != 0) + ((f & F_S) != 0);
                int d = ((f & F_NW) != 0) + ((f & F_NE) != 0) + ((f & F_SW) != 0) + ((f & F_SE) != 0);
                if (o == 1)  // HL: horizontal high-pass
                {
                    const int t = h;
                    h = v;
                    v = t;
                }
                int ctx;
                if (o == 3)
                {
                    int hv = h + v;
                    if (d >= 3) ctx = 8;
                    else if (d == 2) ctx = hv >= 1 ? 7 : 6;
                    else if (d == 1) ctx = hv >= 2 ? 5 : (hv == 1 ? 4 : 3);
                    else ctx = hv >= 2 ? 2 : hv;
                }
                else
                {
                    if (h == 2) ctx = 8;
                    else if (h == 1) ctx = v >= 1 ? 7 : (d >= 1 ? 6 : 5);
                    else if (v == 2) ctx = 4;
                    else if (v == 1) ctx = 3;
                    else ctx = d >= 2 ? 2 : d;
                }
                zc[o][f] = (uint8_t)ctx;
            }
        for (int f = 0; f < 4096; f++)
        {
            auto contrib = [&](int sigBit, int sgnBit) {
                if (!(f & sigBit)) return 0;
                return (f & sgnBit) ? -1 : 1;
            };
            int hc = contrib(F_W, F_SGN_W) + contrib(F_E, F_SGN_E);
            int vc = contrib(F_N, F_SGN_N) + contrib(F_S, F_SGN_S);
            hc = hc > 1 ? 1 : (hc < -1 ? -1 : hc);
            vc = vc > 1 ? 1 : (vc < -1 ? -1 : vc);
            int ctx, x = 0;
            if (hc == 1) ctx = vc == 1 ? 13 : (vc == 0 ? 12 : 11);
            else if (hc == 0) { ctx = vc == 0 ? 9 : 10; x = vc == -1; }
            else { ctx = vc == 1 ? 11 : (vc == 0 ? 12 : 13); x = 1; }
            sc[f] = (uint8_t)(ctx | (x << 7));
        }
    }
};
Luts kLuts;

void buildTables()
{
    kTable.build();
    kLuts.build();
}

struct T1Impl
{
    Mq mq;
    uint16_t flags[(64 + 2) * (64 + 2)];
    // Row bit masks (bit x = column x), indexed by row + 1 so that rows -1 and h are valid:
    // sig = significant, nb = has a significant neighbour, vis = already coded in this bit plane.
    uint64_t sigM[66], nbM[66], visM[66];
    uint8_t padded[65536 + 8];
    int w, h, fstride;
    uint64_t wmask;
    const uint8_t* zc;
    int32_t* data;

    void setSignificant(uint16_t* f, int x, int y, int sign)
    {
        const int s = fstride;
        f[-s - 1] |= F_SE;
        f[-s] |= F_S | (sign ? F_SGN_S : 0);
        f[-s + 1] |= F_SW;
        f[-1] |= F_E | (sign ? F_SGN_E : 0);
        f[1] |= F_W | (sign ? F_SGN_W : 0);
        f[s - 1] |= F_NE;
        f[s] |= F_N | (sign ? F_SGN_N : 0);
        f[s + 1] |= F_NW;
        *f |= F_SIG;
        const uint64_t bit = 1ull << x;
        const uint64_t three = (bit >> 1) | bit | (bit << 1);  // columns x-1 .. x+1
        sigM[y + 1] |= bit;
        nbM[y] |= three;
        nbM[y + 1] |= three & ~bit;
        nbM[y + 2] |= three;
    }

    int decodeSign(uint16_t f)
    {
        int e = kLuts.sc[f & 0xFFF];
        return mq.decode(e & 0x7F) ^ (e >> 7);
    }

    // Decode the significance of a sample that is not significant yet; returns true if it became so.
    __forceinline bool codeSignificance(int x, int y, int32_t oneplushalf)
    {
        uint16_t* f = &flags[(y + 1) * fstride + x + 1];
        if (!mq.decode(zc[*f & 0xFF])) return false;
        int sign = decodeSign(*f);
        data[y * w + x] = sign ? -oneplushalf : oneplushalf;
        setSignificant(f, x, y, sign);
        return true;
    }

    uint64_t sigCandidates(int y) const { return nbM[y + 1] & ~sigM[y + 1] & ~visM[y + 1] & wmask; }

    template <int J>
    static uint64_t& pick(uint64_t& c0, uint64_t& c1, uint64_t& c2, uint64_t& c3)
    {
        if constexpr (J == 0) return c0;
        else if constexpr (J == 1) return c1;
        else if constexpr (J == 2) return c2;
        else return c3;
    }

    // Row J of a significance-propagation column: code the sample if it is a candidate.  The four
    // candidate masks are passed as separate locals so that they stay in registers.
    template <int J>
    __forceinline void sigRow(int y0, int nr, int x, uint64_t bit, int32_t oneplushalf, uint64_t& c0, uint64_t& c1, uint64_t& c2,
                              uint64_t& c3)
    {
        uint64_t& cj = pick<J>(c0, c1, c2, c3);
        if (!(cj & bit)) return;
        const int y = y0 + J;
        const bool became = codeSignificance(x, y, oneplushalf);
        visM[y + 1] |= bit;
        cj &= ~bit;
        if (became)
        {
            // neighbours inside this stripe that are not decided yet become candidates
            const uint64_t three = (bit >> 1) | bit | (bit << 1);
            if constexpr (J > 0) pick<(J > 0 ? J - 1 : 0)>(c0, c1, c2, c3) |= three & ~(sigM[y] | visM[y]) & wmask;
            pick<J>(c0, c1, c2, c3) |= three & ~(sigM[y + 1] | visM[y + 1]) & wmask;
            if (J + 1 < nr) pick<(J < 3 ? J + 1 : 3)>(c0, c1, c2, c3) |= three & ~(sigM[y + 2] | visM[y + 2]) & wmask;
        }
    }

    void sigPass(int bp)
    {
        const int32_t oneplushalf = (1 << (bp + 1)) | (1 << bp);
        for (int y0 = 0; y0 < h; y0 += 4)
        {
            const int nr = h - y0 < 4 ? h - y0 : 4;
            // c0..c3: samples of the stripe rows that the pass would code now (insignificant, not yet
            // coded, with a significant neighbour).  Kept up to date as samples become significant.
            uint64_t c0 = sigCandidates(y0), c1 = nr > 1 ? sigCandidates(y0 + 1) : 0, c2 = nr > 2 ? sigCandidates(y0 + 2) : 0,
                     c3 = nr > 3 ? sigCandidates(y0 + 3) : 0;
            int x0 = 0;
            while (x0 < w)
            {
                const uint64_t any = (c0 | c1 | c2 | c3) & (~0ull << x0);
                if (!any) break;
                const int x = (int)_tzcnt_u64(any);
                const uint64_t bit = 1ull << x;
                sigRow<0>(y0, nr, x, bit, oneplushalf, c0, c1, c2, c3);
                sigRow<1>(y0, nr, x, bit, oneplushalf, c0, c1, c2, c3);
                sigRow<2>(y0, nr, x, bit, oneplushalf, c0, c1, c2, c3);
                sigRow<3>(y0, nr, x, bit, oneplushalf, c0, c1, c2, c3);
                x0 = x + 1;
            }
        }
    }

    void refPass(int bp)
    {
        const int32_t poshalf = 1 << bp;
        for (int y0 = 0; y0 < h; y0 += 4)
        {
            const int nr = h - y0 < 4 ? h - y0 : 4;
            uint64_t m[4] = {0, 0, 0, 0};
            uint64_t any = 0;
            for (int j = 0; j < nr; j++)
            {
                m[j] = sigM[y0 + j + 1] & ~visM[y0 + j + 1] & wmask;
                any |= m[j];
            }
            while (any)
            {
                const int x = (int)_tzcnt_u64(any);
                any &= any - 1;
                for (int j = 0; j < nr; j++)
                {
                    if (!((m[j] >> x) & 1)) continue;
                    const int y = y0 + j;
                    uint16_t* f = &flags[(y + 1) * fstride + x + 1];
                    // context 16 once refined, otherwise 15 with a significant neighbour and 14 without
                    const uint32_t refined = (*f >> 14) & 1;
                    const uint32_t neighbour = (*f & F_NEIGH) != 0;
                    const int ctx = CTX_MR + (int)((refined << 1) | (neighbour & ~refined));
                    const int bit = mq.decode(ctx);
                    int32_t& v = data[y * w + x];
                    const int32_t m = bit - 1;           // 0 for a 1 bit, -1 for a 0 bit
                    const int32_t t = (poshalf ^ m) - m;  // +poshalf or -poshalf
                    const int32_t neg = v >> 31;          // -1 for negative values
                    v += (t ^ neg) - neg;
                    *f |= F_REFINE;
                }
            }
        }
    }

    // Columns whose four samples are all insignificant, unvisited and without significant neighbours.
    uint64_t runLengthColumns(int y0) const
    {
        uint64_t busy = 0;
        for (int j = 0; j < 4; j++) busy |= sigM[y0 + j + 1] | nbM[y0 + j + 1] | visM[y0 + j + 1];
        return ~busy & wmask;
    }

    // Row J of a clean-up column, for rows from `first` on: code the sample if it is undecided.
    // Returns true when it became significant.
    template <int J>
    __forceinline bool cleanRow(int first, int nr, int x, uint64_t bit, int32_t oneplushalf, int y0, uint64_t undecided)
    {
        if (J < first || J >= nr || !(undecided & bit)) return false;
        return codeSignificance(x, y0 + J, oneplushalf);
    }

    void cleanupPass(int bp)
    {
        const int32_t oneplushalf = (1 << (bp + 1)) | (1 << bp);
        for (int y0 = 0; y0 < h; y0 += 4)
        {
            const int nr = h - y0 < 4 ? h - y0 : 4;
            // u0..u3: samples of the stripe rows that are not significant and not coded in this bit
            // plane.  Neither property changes for other columns while the stripe is processed.
            const uint64_t u0 = ~(sigM[y0 + 1] | visM[y0 + 1]) & wmask;
            const uint64_t u1 = nr > 1 ? ~(sigM[y0 + 2] | visM[y0 + 2]) & wmask : 0;
            const uint64_t u2 = nr > 2 ? ~(sigM[y0 + 3] | visM[y0 + 3]) & wmask : 0;
            const uint64_t u3 = nr > 3 ? ~(sigM[y0 + 4] | visM[y0 + 4]) & wmask : 0;
            uint64_t rl = nr == 4 ? runLengthColumns(y0) : 0;
            for (int x = 0; x < w; x++)
            {
                const uint64_t bit = 1ull << x;
                int first = 0;
                if (rl & bit)
                {
                    if (!mq.decode(CTX_AGG)) continue;
                    int j = mq.decode(CTX_UNI) << 1;
                    j |= mq.decode(CTX_UNI);
                    const int y = y0 + j;
                    uint16_t* f = &flags[(y + 1) * fstride + x + 1];
                    int sign = decodeSign(*f);
                    data[y * w + x] = sign ? -oneplushalf : oneplushalf;
                    setSignificant(f, x, y, sign);
                    rl = runLengthColumns(y0);
                    first = j + 1;
                }
                bool any = false;
                any |= cleanRow<0>(first, nr, x, bit, oneplushalf, y0, u0);
                any |= cleanRow<1>(first, nr, x, bit, oneplushalf, y0, u1);
                any |= cleanRow<2>(first, nr, x, bit, oneplushalf, y0, u2);
                any |= cleanRow<3>(first, nr, x, bit, oneplushalf, y0, u3);
                if (any && nr == 4) rl = runLengthColumns(y0);
            }
        }
        memset(visM, 0, sizeof(visM));
    }
};

}  // namespace

// The exported handle wraps the implementation, which stays in the anonymous namespace.
struct T1Scratch
{
    T1Impl impl;
};

T1Scratch* t1Create()
{
    static const bool tablesBuilt = (buildTables(), true);  // once, thread safe, after the CPU check
    (void)tablesBuilt;
    // calloc rather than new: operator new and its exception path are templates and inline
    // functions shared with the rest of the viewer (see slj2k_avx2.h). T1Scratch is plain data.
    return static_cast<T1Scratch*>(calloc(1, sizeof(T1Scratch)));
}

void t1Destroy(T1Scratch* s) { free(s); }

void t1Decode(T1Scratch* scratch, const uint8_t* tile, const T1Block& cb, int32_t* out)
{
    T1Impl* s = &scratch->impl;
    s->w = cb.w;
    s->h = cb.h;
    s->fstride = cb.w + 2;
    s->wmask = cb.w == 64 ? ~0ull : ((1ull << cb.w) - 1);
    s->zc = kLuts.zc[cb.orient];
    s->data = out;
    memset(out, 0, sizeof(int32_t) * cb.w * cb.h);
    memset(s->flags, 0, sizeof(uint16_t) * (cb.h + 2) * s->fstride);
    memset(s->sigM, 0, sizeof(s->sigM));
    memset(s->nbM, 0, sizeof(s->nbM));
    memset(s->visM, 0, sizeof(s->visM));

    // The MQ decoder reads past the end: terminate the segment with two 0xFF bytes.
    size_t len = 0;
    for (uint32_t k = 0; k < cb.nchunks; k++)
    {
        const Chunk& c = cb.chunks[k];
        if (len + c.length > sizeof(s->padded) - 2) break;
        memcpy(s->padded + len, tile + c.offset, c.length);
        len += c.length;
    }
    s->padded[len] = s->padded[len + 1] = 0xFF;
    s->mq.init(s->padded);
    s->mq.resetContexts();

    int bp = cb.numbps - 1;
    int type = 2;  // the first pass is a clean-up pass
    for (int pass = 0; pass < cb.passes && bp >= 0; pass++)
    {
        if (type == 0) s->sigPass(bp);
        else if (type == 1) s->refPass(bp);
        else s->cleanupPass(bp);
        if (++type == 3)
        {
            type = 0;
            bp--;
        }
    }
}

}  // namespace avx2
}  // namespace slj2k
