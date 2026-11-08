/**
 * @file slj2k_codestream.cpp
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

// Codestream parsing (markers) and tier-2 (packet headers) for slj2k.
#include "slj2k_codestream.h"
#include <cmath>
#include <cstring>

namespace slj2k
{

static thread_local const char* gReason = "";
const char* lastUnsupportedReason() { return gReason; }

Status unsupported(const char* why)
{
    gReason = why;
    return Status::Unsupported;
}

#define UNSUPPORTED(why) (gReason = (why), Status::Unsupported)

void TagTree::init(int w, int h)
{
    leavesW = w;
    leavesH = h;
    std::vector<int> lw, lh, off;
    int cw = w, ch = h, total = 0;
    for (;;)
    {
        lw.push_back(cw);
        lh.push_back(ch);
        off.push_back(total);
        total += cw * ch;
        if (cw <= 1 && ch <= 1) break;
        cw = (cw + 1) / 2;
        ch = (ch + 1) / 2;
    }
    nodes.assign(total, Node());
    for (size_t k = 0; k < lw.size(); k++)
        for (int y = 0; y < lh[k]; y++)
            for (int x = 0; x < lw[k]; x++)
            {
                Node& n = nodes[off[k] + y * lw[k] + x];
                n.parent = k + 1 < lw.size() ? off[k + 1] + (y / 2) * lw[k + 1] + (x / 2) : -1;
            }
}

namespace
{

inline int ceilDivPow2(int a, int b) { return (int)(((int64_t)a + ((int64_t)1 << b) - 1) >> b); }

inline uint32_t rd16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Packet-header bit reader with the 0xFF bit-stuffing rule.  Reading past the end of the data
// yields zero bits (as OpenJPEG does), so a header cut short still parses; `overrun` records it.
struct BitReader
{
    const uint8_t* p;
    const uint8_t* end;
    uint32_t cur = 0;
    int left = 0;
    bool overrun = false;

    int bit()
    {
        if (left == 0)
        {
            bool stuffed = cur == 0xFF;
            if (p < end) cur = *p++;
            else
            {
                cur = 0;
                overrun = true;
            }
            left = stuffed ? 7 : 8;
        }
        left--;
        return (cur >> left) & 1;
    }
    uint32_t bits(int n)
    {
        uint32_t v = 0;
        while (n--) v = (v << 1) | bit();
        return v;
    }
    // Finish the header: drop the partial byte, and skip the stuffing byte after a trailing 0xFF.
    void align()
    {
        left = 0;
        if (cur == 0xFF)
        {
            if (p < end) p++;
            else overrun = true;
        }
        cur = 0;
    }
};

bool tagDecode(TagTree& t, BitReader& br, int x, int y, int threshold)
{
    int stack[32];
    int n = 0;
    for (int i = y * t.leavesW + x; i >= 0; i = t.nodes[i].parent) stack[n++] = i;
    int low = 0;
    for (int k = n - 1; k >= 0; k--)
    {
        TagTree::Node& nd = t.nodes[stack[k]];
        if (low > nd.low) nd.low = low;
        else low = nd.low;
        while (low < threshold && low < nd.value)
        {
            if (br.bit()) nd.value = low;
            else ++low;
        }
        nd.low = low;
    }
    return t.nodes[stack[0]].value < threshold;
}

int floorLog2(unsigned v)
{
    int r = 0;
    while (v >>= 1) r++;
    return r;
}

}  // namespace

namespace
{

struct HeaderState
{
    bool haveSiz = false, haveCod = false, haveQcd = false;
    std::vector<uint8_t> cod, qcd;  // raw bodies, to check per-component overrides against
};

// One marker segment of the main or tile-part header. `s` points after the length field,
// `len` is the Lxxx value (it counts the two length bytes).
Status applySegment(uint32_t marker, const uint8_t* s, uint32_t len, Params& p, HeaderState& hs)
{
    switch (marker)
    {
    case 0xFF51:  // SIZ
    {
        if (len < 38 + 3) return Status::Corrupt;
        uint32_t xs = rd32(s + 2), ys = rd32(s + 6), xo = rd32(s + 10), yo = rd32(s + 14);
        uint32_t xt = rd32(s + 18), yt = rd32(s + 22), xto = rd32(s + 26), yto = rd32(s + 30);
        int nc = (int)rd16(s + 34);
        if (xo || yo || xto || yto || xs == 0 || ys == 0 || xs > kMaxSide || ys > kMaxSide) return UNSUPPORTED("image offset or size");
        if ((uint64_t)xs * ys * (uint64_t)rd16(s + 34) > (1u << 26)) return UNSUPPORTED("image too large");
        if (xt < xs || yt < ys) return UNSUPPORTED("more than one tile");
        if (nc < 1 || nc > 8 || len < (uint32_t)(38 + 3 * nc)) return UNSUPPORTED("component count");
        for (int c = 0; c < nc; c++)
            if (s[36 + 3 * c] != 7 || s[37 + 3 * c] != 1 || s[38 + 3 * c] != 1) return UNSUPPORTED("component depth or subsampling");
        p.width = (int)xs;
        p.height = (int)ys;
        p.ncomp = nc;
        hs.haveSiz = true;
        return Status::Ok;
    }
    case 0xFF52:  // COD
    {
        if (len < 12) return Status::Corrupt;
        int scod = s[0];
        p.progression = s[1];
        p.layers = (int)rd16(s + 2);
        p.mct = s[4];
        p.levels = s[5];
        p.cbw = s[6] + 2;
        p.cbh = s[7] + 2;
        int style = s[8];
        p.reversible = s[9] == 1;
        if (scod != 0) return UNSUPPORTED("precincts, SOP or EPH");
        if (style != 0 || s[9] > 1) return UNSUPPORTED("code-block style");
        if (p.cbw > 6 || p.cbh > 6 || p.layers < 1 || p.levels > 12 || p.progression > 4) return UNSUPPORTED("coding parameters");
        if (p.layers > kMaxLayers) return UNSUPPORTED("too many quality layers");
        hs.cod.assign(s + 5, s + len - 2);  // levels .. transform
        hs.haveCod = true;
        return Status::Ok;
    }
    case 0xFF5C:  // QCD
    {
        if (len < 3) return Status::Corrupt;  // Sqcd itself is missing
        p.qstyle = s[0] & 0x1F;
        p.guard = s[0] >> 5;
        p.qexp.clear();
        p.qmant.clear();
        if (p.qstyle == 0)
            for (uint32_t i = 1; i < len - 2; i++) p.qexp.push_back(s[i] >> 3), p.qmant.push_back(0);
        else if (p.qstyle == 1 || p.qstyle == 2)
            for (uint32_t i = 1; i + 1 < len - 2; i += 2)
            {
                uint32_t v = rd16(s + i);
                p.qexp.push_back((uint16_t)(v >> 11));
                p.qmant.push_back((uint16_t)(v & 0x7FF));
            }
        else
            return UNSUPPORTED("quantisation style");
        hs.qcd.assign(s, s + len - 2);
        hs.haveQcd = true;
        return Status::Ok;
    }
    case 0xFF53:  // COC: accepted only when it repeats the default coding style (Kakadu writes these)
    {
        uint32_t i = p.ncomp < 257 ? 1 : 2;
        if (!hs.haveCod || len != 2 + i + 1 + 5 || hs.cod.size() != 5) return UNSUPPORTED("COC before COD, or with precincts");
        if (s[i] != 0) return UNSUPPORTED("COC with precincts");
        // levels, cbw, cbh, style and transform must equal the defaults
        if (memcmp(s + i + 1, hs.cod.data(), 5) != 0) return UNSUPPORTED("component-specific coding style");
        return Status::Ok;
    }
    case 0xFF5D:  // QCC: accepted only when identical to QCD
    {
        uint32_t i = p.ncomp < 257 ? 1 : 2;
        if (!hs.haveQcd) return UNSUPPORTED("QCC before QCD");
        if (len - 2 - i != hs.qcd.size() || memcmp(s + i, hs.qcd.data(), hs.qcd.size()) != 0)
            return UNSUPPORTED("component-specific quantisation");
        return Status::Ok;
    }
    case 0xFF5E:  // RGN
    case 0xFF5F:  // POC
    case 0xFF60:  // PPM
    case 0xFF61:  // PPT
        return UNSUPPORTED("region of interest, progression change or packed headers");
    default:  // TLM, PLM, PLT, CRG, COM, ...
        return Status::Ok;
    }
}

}  // namespace

Status parseMainHeader(const uint8_t* d, size_t size, Params& p, TileData& tile)
{
    if (size < 4 || d[0] != 0xFF || d[1] != 0x4F) return Status::Corrupt;
    size_t pos = 2;
    HeaderState hs;
    std::vector<std::pair<size_t, size_t>> parts;  // tile-part payload ranges

    while (pos + 4 <= size)
    {
        uint32_t marker = rd16(d + pos);
        if (marker == 0xFFD9) break;  // EOC
        if ((marker & 0xFF00) != 0xFF00) return Status::Corrupt;
        uint32_t len = rd16(d + pos + 2);
        if (len < 2) return Status::Corrupt;

        if (marker == 0xFF90)  // SOT
        {
            if (!hs.haveSiz || !hs.haveCod || !hs.haveQcd) return Status::Corrupt;
            if (pos + 12 > size) break;
            uint32_t isot = rd16(d + pos + 4), psot = rd32(d + pos + 6);
            if (isot != 0) return UNSUPPORTED("tile index");
            size_t partEnd = psot ? pos + psot : size;
            if (partEnd > size || partEnd < pos + 12) partEnd = size;
            // tile-part header: markers up to SOD
            size_t q = pos + 12;
            bool sod = false;
            while (q + 2 <= partEnd)
            {
                uint32_t m = rd16(d + q);
                if (m == 0xFF93)
                {
                    sod = true;
                    q += 2;
                    break;
                }
                if (q + 4 > partEnd) break;
                uint32_t l = rd16(d + q + 2);
                if (l < 2 || q + 2 + l > partEnd) return Status::Corrupt;
                Status st = applySegment(m, d + q + 4, l, p, hs);
                if (st != Status::Ok) return st;
                q += 2 + l;
            }
            if (!sod) break;
            if (q >= partEnd) break;  // no tile data yet
            parts.push_back({q, partEnd});
            pos = partEnd;
            continue;
        }

        if (pos + 2 + len > size) return Status::Corrupt;
        Status st = applySegment(marker, d + pos + 4, len, p, hs);
        if (st != Status::Ok) return st;
        pos += 2 + len;
    }

    if (!hs.haveSiz || !hs.haveCod || !hs.haveQcd) return Status::Corrupt;
    if (p.reversible != (p.qstyle == 0)) return UNSUPPORTED("transform and quantisation style disagree");
    if (p.qstyle == 1 && p.qexp.empty()) return Status::Corrupt;
    if (p.mct && p.ncomp < 3) return UNSUPPORTED("component transform needs three components");

    if (parts.empty()) return Status::Corrupt;  // the tile-part header is not complete yet
    if (parts.size() == 1)
    {
        tile.data = d + parts[0].first;
        tile.size = parts[0].second - parts[0].first;
    }
    else
    {
        for (auto& r : parts) tile.owned.insert(tile.owned.end(), d + r.first, d + r.second);
        tile.data = tile.owned.data();
        tile.size = tile.owned.size();
    }
    return Status::Ok;
}

Status buildStructure(const Params& p, std::vector<Component>& comps)
{
    comps.assign(p.ncomp, Component());
    const int L = p.levels;
    size_t totalBlocks = 0;  // counted before each band's blocks are made (kMaxBlocks)
    // Tier-1 runs each pass a packet header gives over all of a block's samples, data or not.
    uint64_t passSamples = 0;
    for (auto& comp : comps)
    {
        comp.res.resize(L + 1);
        for (int r = 0; r <= L; r++)
        {
            Resolution& rs = comp.res[r];
            rs.w = ceilDivPow2(p.width, L - r);
            rs.h = ceilDivPow2(p.height, L - r);
            int lowW = r ? comp.res[r - 1].w : 0, lowH = r ? comp.res[r - 1].h : 0;
            int nb = r == 0 ? 1 : 3;
            rs.bands.resize(nb);
            for (int b = 0; b < nb; b++)
            {
                Band& bd = rs.bands[b];
                bd.orient = r == 0 ? 0 : b + 1;
                int hiW = rs.w - lowW, hiH = rs.h - lowH;
                switch (bd.orient)
                {
                case 0: bd.w = rs.w; bd.h = rs.h; bd.offX = 0; bd.offY = 0; break;
                case 1: bd.w = hiW; bd.h = lowH; bd.offX = lowW; bd.offY = 0; break;
                case 2: bd.w = lowW; bd.h = hiH; bd.offX = 0; bd.offY = lowH; break;
                default: bd.w = hiW; bd.h = hiH; bd.offX = lowW; bd.offY = lowH; break;
                }
                // quantisation parameters
                int qi = r == 0 ? 0 : 3 * (r - 1) + b + 1;
                int expn, mant;
                if (p.qstyle == 1)
                {
                    expn = p.qexp[0] - (r == 0 ? 0 : r - 1);
                    mant = p.qmant[0];
                }
                else
                {
                    if (qi >= (int)p.qexp.size()) return Status::Corrupt;
                    expn = p.qexp[qi];
                    mant = p.qmant[qi];
                }
                bd.mb = p.guard + expn - 1;
                if (bd.mb < 1 || bd.mb > 30) return UNSUPPORTED("line295");
                passSamples += (uint64_t)bd.w * bd.h * (3 * bd.mb - 2);
                if (passSamples > kMaxPassSamples) return UNSUPPORTED("too many coding passes for its size");
                if (!p.reversible)
                {
                    bd.step = (float)((1.0 + mant / 2048.0) * std::pow(2.0, 8 - expn));  // subband gain is folded into the wavelet scaling
                }
                // code blocks
                const int cw = 1 << p.cbw, ch = 1 << p.cbh;
                bd.ncbx = (bd.w + cw - 1) / cw;
                bd.ncby = (bd.h + ch - 1) / ch;
                if (bd.w > 0 && bd.h > 0)
                {
                    totalBlocks += (size_t)bd.ncbx * bd.ncby;
                    if (totalBlocks > kMaxBlocks) return UNSUPPORTED("too many code blocks");
                    bd.blocks.resize((size_t)bd.ncbx * bd.ncby);
                    for (int by = 0; by < bd.ncby; by++)
                        for (int bx = 0; bx < bd.ncbx; bx++)
                        {
                            CodeBlock& cb = bd.blocks[by * bd.ncbx + bx];
                            cb.x0 = (uint16_t)(bx * cw);
                            cb.y0 = (uint16_t)(by * ch);
                            cb.w = (uint16_t)std::min(cw, bd.w - bx * cw);
                            cb.h = (uint16_t)std::min(ch, bd.h - by * ch);
                        }
                    bd.incl.init(bd.ncbx, bd.ncby);
                    bd.zbp.init(bd.ncbx, bd.ncby);
                }
            }
        }
    }
    return Status::Ok;
}

namespace
{

// Parse one packet and record its code-block contributions.  Returns false when the data ends
// inside the packet.  A truncated packet is treated like OpenJPEG's non-strict mode does: header
// bits past the end read as zero, and a block whose bytes do not fit (with every block after it
// in the packet) still receives its pass count but no data.
bool readPacket(const Params& p, Resolution& rs, int layer, const uint8_t* base, size_t size, size_t& pos)
{
    BitReader br;
    br.p = base + pos;
    br.end = base + size;
    struct Contribution
    {
        CodeBlock* cb;
        uint32_t length;
        int np;
    };
    std::vector<Contribution> contrib;

    if (br.bit())
    {
        for (Band& bd : rs.bands)
        {
            for (int by = 0; by < bd.ncby; by++)
                for (int bx = 0; bx < bd.ncbx; bx++)
                {
                    CodeBlock& cb = bd.blocks[by * bd.ncbx + bx];
                    bool inc;
                    if (!cb.included) inc = tagDecode(bd.incl, br, bx, by, layer + 1);
                    else inc = br.bit() != 0;
                    if (!inc) continue;

                    if (!cb.included)
                    {
                        int i = 1;
                        while (!tagDecode(bd.zbp, br, bx, by, i))
                            if (++i > 64) return false;
                        cb.included = true;
                        cb.numbps = (uint8_t)std::max(0, bd.mb + 1 - i);
                        cb.lblock = 3;
                    }
                    int np;
                    if (!br.bit()) np = 1;
                    else if (!br.bit()) np = 2;
                    else
                    {
                        int n = br.bits(2);
                        if (n != 3) np = 3 + n;
                        else
                        {
                            n = br.bits(5);
                            np = n != 31 ? 6 + n : 37 + (int)br.bits(7);
                        }
                    }
                    while (br.bit())
                        if (++cb.lblock > 32) return false;
                    int nbits = cb.lblock + floorLog2(np);
                    if (nbits > 24) return false;
                    cb.passes = (uint16_t)(cb.passes + np);
                    contrib.push_back({&cb, br.bits(nbits), np});
                }
        }
    }
    br.align();

    size_t body = br.p - base;
    bool overflow = false;
    for (const Contribution& c : contrib)
    {
        if (body + c.length > size) overflow = true;
        if (overflow)
        {
            // OpenJPEG discards the whole block, including what earlier layers delivered.
            c.cb->passes = 0;
            c.cb->bytes = 0;
            c.cb->chunks.clear();
            continue;
        }
        if (c.length)
        {
            c.cb->chunks.push_back({(uint32_t)body, c.length});
            c.cb->bytes += c.length;
        }
        body += c.length;
    }
    pos = body;
    return !overflow && !br.overrun;
}

}  // namespace

static thread_local std::vector<size_t>* gPacketEnds = nullptr;

std::vector<size_t> debugPacketEnds(const uint8_t* data, size_t size)
{
    std::vector<size_t> ends;
    Params p;
    TileData tile;
    std::vector<Component> comps;
    if (parseMainHeader(data, size, p, tile) != Status::Ok || buildStructure(p, comps) != Status::Ok) return ends;
    gPacketEnds = &ends;
    parsePackets(p, comps, tile.data, tile.size);
    gPacketEnds = nullptr;
    for (auto& e : ends) e += tile.data - data;
    return ends;
}

void parsePackets(const Params& p, std::vector<Component>& comps, const uint8_t* data, size_t size)
{
    const int L = p.levels, nc = p.ncomp, nl = p.layers;
    size_t pos = 0;
    // A packet header looks at every code block of its resolution, even those it skips with a
    // bit or two: count them, and stop as at the end of the data past kMaxBlockVisits.
    uint64_t visits = 0;
    auto one = [&](int l, int r, int c) {
        for (const Band& bd : comps[c].res[r].bands) visits += bd.blocks.size();
        if (visits > kMaxBlockVisits) return false;
        bool ok = readPacket(p, comps[c].res[r], l, data, size, pos);
        if (ok && gPacketEnds) gPacketEnds->push_back(pos);
        return ok;
    };
    // With one precinct per resolution the position loops of RPCL/PCRL/CPRL collapse.
    switch (p.progression)
    {
    case 0:  // LRCP
        for (int l = 0; l < nl; l++)
            for (int r = 0; r <= L; r++)
                for (int c = 0; c < nc; c++)
                    if (!one(l, r, c)) return;
        break;
    case 1:  // RLCP
        for (int r = 0; r <= L; r++)
            for (int l = 0; l < nl; l++)
                for (int c = 0; c < nc; c++)
                    if (!one(l, r, c)) return;
        break;
    case 2:  // RPCL
        for (int r = 0; r <= L; r++)
            for (int c = 0; c < nc; c++)
                for (int l = 0; l < nl; l++)
                    if (!one(l, r, c)) return;
        break;
    default:  // PCRL, CPRL
        for (int c = 0; c < nc; c++)
            for (int r = 0; r <= L; r++)
                for (int l = 0; l < nl; l++)
                    if (!one(l, r, c)) return;
        break;
    }
}

}  // namespace slj2k
