/**
 * @file slj2k_internal.h
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

// Internal structures shared by the slj2k source files built for plain x64 (the AVX2 kernels see
// only slj2k_avx2.h).
#pragma once
#include "slj2k.h"
#include "slj2k_avx2.h"
#include <memory>

namespace slj2k
{

struct CodeBlock
{
    uint16_t x0, y0, w, h;  // position inside the band, size
    uint8_t numbps = 0;     // magnitude bit planes present (Mb - zero bit planes)
    uint8_t lblock = 3;
    bool included = false;
    uint16_t passes = 0;
    uint32_t bytes = 0;
    std::vector<Chunk> chunks;
};

struct TagTree
{
    struct Node
    {
        int value = 0x7fffffff, low = 0;
        int parent = -1;
    };
    std::vector<Node> nodes;
    int leavesW = 0, leavesH = 0;
    void init(int w, int h);
};

struct Band
{
    int orient = 0;  // 0 LL, 1 HL, 2 LH, 3 HH
    int w = 0, h = 0;
    int offX = 0, offY = 0;  // position inside the component's Mallat layout
    int mb = 0;              // magnitude bits (guard + exponent - 1)
    float step = 1.0f;       // irreversible quantiser step
    int ncbx = 0, ncby = 0;
    std::vector<CodeBlock> blocks;
    TagTree incl, zbp;
};

struct Resolution
{
    int w = 0, h = 0;  // size of this resolution
    std::vector<Band> bands;
};

struct Component
{
    std::vector<Resolution> res;
};

struct Params
{
    int width = 0, height = 0, ncomp = 0;
    int progression = 0, layers = 1, mct = 0, levels = 5;
    int cbw = 6, cbh = 6;  // log2 of code-block size
    bool reversible = false;
    int qstyle = 0, guard = 0;
    std::vector<uint16_t> qexp, qmant;
};

}  // namespace slj2k
