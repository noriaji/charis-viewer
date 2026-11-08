/**
 * @file lutfile.h
 * @brief Colour lookup tables (LUTs) from Adobe .cube text and from strip
 *        images (ReShade / GShade / Unreal / Alchemy), as one 3D table
 *        (research/102-community-looks.md in the sl-viewer repository).
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

#ifndef PLUS_LOOK_LUTFILE_H
#define PLUS_LOOK_LUTFILE_H

// Standard C++ only (no viewer headers), so that tools/looks/lut_test.cpp in
// the sl-viewer repository can build and test it on its own.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace plus::look
{
    // A 3D colour table: size^3 RGB entries, red changing fastest, then green,
    // then blue (the .cube order). Entry (r, g, b) is the colour for the input
    // domain_min + (r, g, b) / (size - 1) * (domain_max - domain_min).
    struct LutTable
    {
        unsigned size = 0;
        std::vector<float> rgb;                 // size^3 * 3
        float domain_min[3] = { 0.f, 0.f, 0.f };
        float domain_max[3] = { 1.f, 1.f, 1.f };
        std::string title;                      // a .cube's TITLE, if any
    };

    // Table sizes taken (the GPU holds them at 16, 32 or 64: gpuSize()).
    constexpr unsigned MIN_LUT_SIZE = 2;
    constexpr unsigned MAX_LUT_SIZE = 65;
    // A 1D .cube is expanded into a 3D table of its size, at most this.
    constexpr unsigned MAX_1D_LUT_SIZE = 65536;
    // Larger .cube files are not read (a 65^3 table takes about 8 MB of text).
    constexpr size_t MAX_CUBE_FILE_BYTES = 32u * 1024u * 1024u;
    // Strips stacked in one image (GShade's MultiLUT): at most this many.
    constexpr unsigned MAX_STRIPS = 256;

    // Adobe .cube text: a 3D table (LUT_3D_SIZE), or a 1D one (LUT_1D_SIZE,
    // expanded into a 3D table), with DOMAIN_MIN / DOMAIN_MAX or
    // LUT_*_INPUT_RANGE. False with a message (and the line) for anything it
    // cannot take: a missing size, a line that is not three numbers, a number
    // that is not finite, too few or too many lines.
    bool parseCube(const std::string& text, LutTable& out, std::string& error);

    // A strip image: `count` strips stacked downwards, each `size` tiles of
    // size x size side by side (width size^2, height size * count).
    struct StripLayout
    {
        unsigned size = 0;
        unsigned count = 0;
    };
    // False when width x height is not such an image (or the size is out of range).
    bool stripLayout(unsigned width, unsigned height, StripLayout& out);

    // Strip number `strip` (0: the top one) of an 8-bit image with 3 or 4
    // components a pixel. Rows are in memory bottom first when bottom_up (the
    // viewer's LLImageRaw), top first otherwise. In a tile, x is red and y is
    // green; the tile is blue. Green grows downwards (ReShade, Unreal): when
    // the colours change down the strip the opposite way to along red in a
    // tile (a strip saved upside down), the rows are read the other way and
    // flipped is set.
    bool parseStrip(const std::uint8_t* pixels, unsigned width, unsigned height, unsigned components, bool bottom_up,
                    unsigned strip, LutTable& out, bool& flipped, std::string& error);

    // The neutral table (every colour to itself).
    void makeIdentity(unsigned size, LutTable& out);

    // The table size the GPU holds a table of `size` at: 16, 32 or 64, so the
    // strip (size^2 x size) has power-of-two sides, as LLImageGL wants.
    unsigned gpuSize(unsigned size);

    // The largest strip the GPU holds (64 tiles of 64 x 64: 4096 x 64).
    constexpr unsigned MAX_STRIP_SIZE = 64;

    // The table's colour for colour: trilinear between its entries (clamped to its domain).
    void apply(const LutTable& table, const float colour[3], float out[3]);

    // The table at another size (at most MAX_STRIP_SIZE), each entry the
    // table's colour there (apply). Its domain is narrowed to 0..1 where it
    // is wider, the range of the finished image. A neutral table stays neutral.
    LutTable resample(const LutTable& table, unsigned size);

    // The table as the GPU holds it: 8-bit RGBA, size^2 wide and size high,
    // row green (row 0 first in memory), column blue * size + red, alpha 255.
    std::vector<std::uint8_t> toStrip8(const LutTable& table);

    // The largest difference between an entry and the colour it stands for
    // (0 for a neutral table). For the log.
    float distanceFromNeutral(const LutTable& table);

    // True when the name or title suggests a LUT for log-encoded camera
    // footage (LogC, S-Log, V-Log, ...), which makes the viewer's image look
    // washed out or garish. A warning for the log, not an error.
    bool looksLogEncoded(const std::string& text);
}

#endif // PLUS_LOOK_LUTFILE_H
