/**
 * @file realstarcat.h
 * @brief Reader of the real star catalogue (app_settings/plus/starcat-bsc5.bin, made by
 *        tools/starcat/make_starcat.py from the Yale Bright Star Catalogue; research/177 §3.2).
 *        Plain C++, header only.
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

#ifndef PLUS_REALSTARS_REALSTARCAT_H
#define PLUS_REALSTARS_REALSTARCAT_H

// The file (little-endian):
//   header 44 B: "CSTR", version u16 (1), reserved u16, count u32, SHA-256 of the source (32 B)
//   per star 10 B: J2000 unit vector int16 x 3 (x 32767), V magnitude int16 (x 100),
//                  B-V int16 (x 1000; 600 where the catalogue has none)
// This reads bytes already in memory: the viewer reads the file with LLFile / llifstream
// (UTF-8 paths; a plain std::ifstream fails on a Japanese user name on Windows).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace plus
{
namespace realstars
{
    constexpr std::uint16_t CATALOG_VERSION = 1;
    constexpr std::size_t CATALOG_HEADER_BYTES = 44;
    constexpr std::size_t CATALOG_STAR_BYTES = 10;
    constexpr std::uint32_t CATALOG_MAX_STARS = 20000; // BSC5 has 9,096 with positions

    struct CatalogStar
    {
        float dir[3]; // J2000 unit vector (x to the vernal equinox, z to the north pole)
        float vmag;   // V magnitude
        float bv;     // B-V colour index
    };

    struct Catalog
    {
        std::vector<CatalogStar> stars;
        std::uint8_t sourceSha256[32] = {};
    };

    enum class CatalogResult
    {
        Ok,
        TooShort,
        BadMagic,
        BadVersion,
        BadCount, // more stars than CATALOG_MAX_STARS, or the size does not match the count
        BadStar,  // a direction far from unit length
    };

    namespace detail
    {
        inline std::uint16_t readU16(const std::uint8_t* p)
        {
            return (std::uint16_t)(p[0] | (p[1] << 8));
        }

        inline std::uint32_t readU32(const std::uint8_t* p)
        {
            return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
        }

        inline std::int16_t readS16(const std::uint8_t* p)
        {
            return (std::int16_t)readU16(p);
        }
    }

    // Fills out from the file's bytes. On any error out.stars is left empty.
    inline CatalogResult parseCatalog(const std::uint8_t* data, std::size_t size, Catalog& out)
    {
        out.stars.clear();
        if (!data || size < CATALOG_HEADER_BYTES)
            return CatalogResult::TooShort;
        if (std::memcmp(data, "CSTR", 4) != 0)
            return CatalogResult::BadMagic;
        if (detail::readU16(data + 4) != CATALOG_VERSION)
            return CatalogResult::BadVersion;
        const std::uint32_t count = detail::readU32(data + 8);
        if (count > CATALOG_MAX_STARS || size != CATALOG_HEADER_BYTES + (std::size_t)count * CATALOG_STAR_BYTES)
            return CatalogResult::BadCount;
        std::memcpy(out.sourceSha256, data + 12, sizeof(out.sourceSha256));

        std::vector<CatalogStar> stars(count);
        const std::uint8_t* p = data + CATALOG_HEADER_BYTES;
        for (std::uint32_t i = 0; i < count; ++i, p += CATALOG_STAR_BYTES)
        {
            const double x = detail::readS16(p) / 32767.0;
            const double y = detail::readS16(p + 2) / 32767.0;
            const double z = detail::readS16(p + 4) / 32767.0;
            const double len = std::sqrt(x * x + y * y + z * z);
            if (len < 0.999 || len > 1.001) // int16 rounding moves it by 5e-5 at most
                return CatalogResult::BadStar;
            CatalogStar& s = stars[i];
            s.dir[0] = (float)(x / len);
            s.dir[1] = (float)(y / len);
            s.dir[2] = (float)(z / len);
            s.vmag = detail::readS16(p + 6) / 100.f;
            s.bv = detail::readS16(p + 8) / 1000.f;
        }
        out.stars.swap(stars);
        return CatalogResult::Ok;
    }

    // ---- the constellation lines (research/177 §2.6, stage 4) ------------------------------
    // app_settings/plus/starcat-lines.bin, made by tools/starcat/make_starlines.py from
    // d3-celestial's constellations.lines.json with each corner taken to its catalogue star:
    //   header 76 B: "CLIN", version u16 (1), reserved u16, count u32 (segments),
    //                SHA-256 of the lines' source (32 B), SHA-256 of the star catalogue's source
    //                (32 B: the star numbers below are that catalogue's)
    //   per segment 4 B: star number u16 x 2 (the order of starcat-bsc5.bin)
    // A file of its own (not the stars' .bin) so the star catalogue and its checks stay as they are.

    constexpr std::uint16_t LINES_VERSION = 1;
    constexpr std::size_t LINES_HEADER_BYTES = 76;
    constexpr std::size_t LINES_SEGMENT_BYTES = 4;
    constexpr std::uint32_t LINES_MAX_SEGMENTS = 4000; // d3-celestial has about 700

    struct Lines
    {
        std::vector<std::uint16_t> ends; // two star numbers per segment
        std::uint8_t sourceSha256[32] = {};
    };

    enum class LinesResult
    {
        Ok,
        TooShort,
        BadMagic,
        BadVersion,
        BadCount,       // more segments than LINES_MAX_SEGMENTS, or the size does not match the count
        OtherCatalogue, // made for another star catalogue (its SHA-256 is not this one's)
        BadStar,        // a star number past the catalogue, or a segment from a star to itself
    };

    // Fills out from the file's bytes, for the catalogue cat (already read). On any error
    // out.ends is left empty.
    inline LinesResult parseLines(const std::uint8_t* data, std::size_t size, const Catalog& cat, Lines& out)
    {
        out.ends.clear();
        if (!data || size < LINES_HEADER_BYTES)
            return LinesResult::TooShort;
        if (std::memcmp(data, "CLIN", 4) != 0)
            return LinesResult::BadMagic;
        if (detail::readU16(data + 4) != LINES_VERSION)
            return LinesResult::BadVersion;
        const std::uint32_t count = detail::readU32(data + 8);
        if (count > LINES_MAX_SEGMENTS || size != LINES_HEADER_BYTES + (std::size_t)count * LINES_SEGMENT_BYTES)
            return LinesResult::BadCount;
        if (std::memcmp(data + 44, cat.sourceSha256, sizeof(cat.sourceSha256)) != 0)
            return LinesResult::OtherCatalogue;

        std::vector<std::uint16_t> ends((std::size_t)count * 2);
        const std::uint8_t* p = data + LINES_HEADER_BYTES;
        for (std::uint32_t i = 0; i < count; ++i, p += LINES_SEGMENT_BYTES)
        {
            const std::uint16_t a = detail::readU16(p);
            const std::uint16_t b = detail::readU16(p + 2);
            if (a >= cat.stars.size() || b >= cat.stars.size() || a == b)
                return LinesResult::BadStar;
            ends[2 * i] = a;
            ends[2 * i + 1] = b;
        }
        std::memcpy(out.sourceSha256, data + 12, sizeof(out.sourceSha256));
        out.ends.swap(ends);
        return LinesResult::Ok;
    }

    // The segments of ends that join two stars an earlier segment joins already, either way round
    // (each would be drawn twice; make_starlines.py writes a segment drawn twice once).
    inline std::size_t duplicateSegments(const std::vector<std::uint16_t>& ends)
    {
        std::vector<std::uint32_t> keys;
        keys.reserve(ends.size() / 2);
        for (std::size_t i = 0; i + 1 < ends.size(); i += 2)
        {
            const std::uint16_t lo = ends[i] < ends[i + 1] ? ends[i] : ends[i + 1];
            const std::uint16_t hi = ends[i] < ends[i + 1] ? ends[i + 1] : ends[i];
            keys.push_back(((std::uint32_t)lo << 16) | hi);
        }
        std::sort(keys.begin(), keys.end());
        return keys.size() - (std::size_t)(std::unique(keys.begin(), keys.end()) - keys.begin());
    }

    // ---- a line's quad (llvowlsky.cpp fills it, starsV.glsl spreads it to its width) ---------
    // Six corners, two triangles: a-, b+, a+ and a-, b-, b+ (the sides as seen from a; they turn
    // counter-clockwise on the screen, as the stars' quads do). A vertex sits at its own end and
    // knows only the other end, so it steps across the line as it sees the line from its end:
    // seen from b the sides change places. The distance across the line that the fragment shader
    // is given must still be the same on a side at both ends. Taking b's own side for it made it
    // run from +w at a to -w at b along each long edge, 0 on a slanted line inside each triangle:
    // the line came out as two thin ones side by side (2026-10-25).
    // The vertex's alpha carries both: bit 0 its side as seen from its own end, bit 1 the end
    // (0 a, 1 b), as the code times 85 (0, 85, 170, 255; starsV.glsl decodes it).
    struct LineCorner
    {
        bool atB;       // the vertex sits at b
        bool plusSide;  // its side as seen from a
    };

    constexpr LineCorner LINE_CORNERS[6] = {
        { false, false }, { true, true }, { false, true },
        { false, false }, { true, false }, { true, true },
    };

    // The side the vertex steps to as seen from its own end.
    constexpr bool lineOwnSide(const LineCorner& c)
    {
        return c.atB ? !c.plusSide : c.plusSide;
    }

    // The vertex's alpha.
    constexpr std::uint8_t lineCornerAlpha(const LineCorner& c)
    {
        return (std::uint8_t)(((c.atB ? 2 : 0) + (lineOwnSide(c) ? 1 : 0)) * 85);
    }
} // namespace realstars
} // namespace plus

#endif // PLUS_REALSTARS_REALSTARCAT_H
