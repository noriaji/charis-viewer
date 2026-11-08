/**
 * @file slj2k_codestream.h
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

// Codestream parsing and tier-2 interface (internal).
#pragma once
#include "slj2k_internal.h"
#include <algorithm>

namespace slj2k
{

struct TileData
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::vector<uint8_t> owned;  // used when the tile spans several tile-parts
};

// What one codestream may ask of slj2k. A texture is at most 4096 on a side (the viewer's
// MAX_IMAGE_SIZE) and has a handful of quality layers; a stream of a few hundred bytes could
// otherwise ask for hours of packet headers or gigabytes of planes. LLImageJ2COJ::decodeImpl
// screens every stream with the same limits before either decoder (plus_j2c::screen); these
// hold inside slj2k as well.
constexpr uint32_t kMaxSide = 4096;
constexpr int kMaxLayers = 64;
constexpr size_t kMaxBlocks = size_t(1) << 19;              // code blocks, all components together
constexpr uint64_t kMaxBlockVisits = uint64_t(1) << 24;     // code blocks looked at by all packet headers
constexpr uint64_t kMaxPassSamples = uint64_t(3) << 30;    // band samples times the passes their bit planes allow (3 Mb - 2)
constexpr size_t kMaxPlaneBytes = size_t(128) << 20;        // one plane (4096 x 4096 floats: 68 MB)
constexpr size_t kMaxPlanesBytes = size_t(768) << 20;       // the planes of one decode

// Records `why` for lastUnsupportedReason() and returns Status::Unsupported.
Status unsupported(const char* why);

// Parse markers up to and including the (only) tile's data.
Status parseMainHeader(const uint8_t* data, size_t size, Params& p, TileData& tile);

// Allocate resolutions / bands / code blocks and quantiser parameters.
Status buildStructure(const Params& p, std::vector<Component>& comps);

// Read packet headers and record each code block's data ranges (offsets into `data`). Stops,
// as at the end of the data, once the headers have looked at kMaxBlockVisits code blocks.
void parsePackets(const Params& p, std::vector<Component>& comps, const uint8_t* data, size_t size);

// Diagnostics: end offset (in the file) of every packet of a complete stream.
std::vector<size_t> debugPacketEnds(const uint8_t* data, size_t size);

}  // namespace slj2k
