/**
 * @file realstarart.h
 * @brief Readers of the constellation pictures (research/288-constellation-art.md): where each
 *        picture's plate lies in the real sky (app_settings/plus/starart.bin) and the pictures
 *        themselves (app_settings/plus/starart-tex.bin), both made by
 *        tools/starcat/make_starart.py. Plain C++, header only.
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

#ifndef PLUS_REALSTARS_REALSTARART_H
#define PLUS_REALSTARS_REALSTARART_H

// The plates (little-endian):
//   header 44 B: "SART", version u16 (1), reserved u16, count u32, SHA-256 of the source (32 B)
//   per plate 52 B: layer u16 (the picture's layer in starart-tex.bin), pad u16,
//                   4 corners x (x, y, z) float32, J2000 (x to the vernal equinox, z to the north pole)
// The corners are the picture's (0,0), (W,0), (W,H), (0,H) (top left, top right, bottom right,
// bottom left) on the plane through the tips (on the unit sphere) of the picture's three
// reference stars, as Stellarium draws them: points of that plane, not directions (not of length
// 1: inside the sphere between the stars, outside it beyond them). The plate is drawn flat
// between them (times the sky's radius), so the picture is not bent and its reference stars are
// where they belong (research/288 section 3.1). A few plates are refit on the drawn figure (any
// flat parallelogram is read the same way; their reference stars up to 0.24 degrees off, research/291).
//
// The pictures:
//   header 16 B: "ATEX", version u16 (2), reserved u16, layers u32, size u16 (512), mips u16 (8)
//   then for each mip 0 .. 7, layers 0 .. layers-1 of it one after another: BC4_UNORM blocks
//   (8 B a 4x4 block), the rows from the picture's top. One grey channel: the drawing on black.
//   (Version 1 had each layer's mips together: a copy a layer and a mip. Not read.)
//
// This reads bytes already in memory (the viewer reads the files with llifstream: UTF-8 paths).

#include "realstarcat.h"    // detail::readU16 / readU32

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace plus
{
namespace realstars
{
    // ---- the plates ----------------------------------------------------------------------
    constexpr std::uint16_t ART_VERSION = 1;
    constexpr std::size_t ART_HEADER_BYTES = 44;
    constexpr std::size_t ART_PLATE_BYTES = 52;
    constexpr std::uint32_t ART_MAX_PLATES = 128;   // 88 constellations, 85 with a picture
    constexpr std::uint32_t ART_MAX_LAYERS = 128;   // a layer number at most ART_MAX_LAYERS - 1 (the
                                                    // vertex colour's alpha carries it: under 256)

    // A corner's length: 0.5 .. 4 (the plane of three stars at most 120 degrees apart passes at
    // 0.5 or more from the centre; 4 is about 75 degrees from the plane's nearest point). Two
    // corners at most 6 apart. The pictures of make_starart.py (2026-11-06): lengths 0.87 .. 1.32,
    // corners at most 1.54 apart (Hydra, Eridanus the largest).
    constexpr float ART_MIN_CORNER_LENGTH = 0.5f;
    constexpr float ART_MAX_CORNER_LENGTH = 4.f;
    constexpr float ART_MAX_PLATE_SPAN = 6.f;
    constexpr float ART_MIN_PLATE_SPAN = 1e-4f;     // a plate not crushed to a line or a point

    // The corners' texture coordinates, in the file's order of the corners.
    constexpr float ART_CORNER_UV[4][2] = { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } };
    // The two triangles of a plate, by corner (culling is off for the sky: either winding).
    constexpr int ART_TRIANGLE_CORNERS[6] = { 0, 1, 2, 0, 2, 3 };

    struct ArtPlate
    {
        std::uint16_t layer = 0;
        float corner[4][3] = {};
    };

    struct Art
    {
        std::vector<ArtPlate> plates;
        std::uint8_t sourceSha256[32] = {};
    };

    enum class ArtResult
    {
        Ok,
        TooShort,
        BadMagic,
        BadVersion,
        BadCount,   // more plates than ART_MAX_PLATES, or the size does not match the count
        BadPlate,   // a layer past ART_MAX_LAYERS, a corner not finite or out of the sizes above
    };

    namespace detail
    {
        inline float readF32(const std::uint8_t* p)
        {
            const std::uint32_t bits = readU32(p);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        inline float distance(const float a[3], const float b[3])
        {
            const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
            return std::sqrt(x * x + y * y + z * z);
        }

        inline bool plateFits(const ArtPlate& plate)
        {
            if (plate.layer >= ART_MAX_LAYERS)
            {
                return false;
            }
            for (const float* c : plate.corner)
            {
                if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]))
                {
                    return false;
                }
                const float length = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                if (!(length >= ART_MIN_CORNER_LENGTH && length <= ART_MAX_CORNER_LENGTH))
                {
                    return false;
                }
            }
            for (int i = 0; i < 4; ++i)
            {
                for (int j = i + 1; j < 4; ++j)
                {
                    if (distance(plate.corner[i], plate.corner[j]) > ART_MAX_PLATE_SPAN)
                    {
                        return false;
                    }
                }
            }
            // its diagonals: neither a line nor a point
            return distance(plate.corner[0], plate.corner[2]) >= ART_MIN_PLATE_SPAN
                && distance(plate.corner[1], plate.corner[3]) >= ART_MIN_PLATE_SPAN;
        }
    }

    // Fills out from starart.bin's bytes. On any error out.plates is left empty.
    inline ArtResult parseArt(const std::uint8_t* data, std::size_t size, Art& out)
    {
        out.plates.clear();
        if (!data || size < ART_HEADER_BYTES)
            return ArtResult::TooShort;
        if (std::memcmp(data, "SART", 4) != 0)
            return ArtResult::BadMagic;
        if (detail::readU16(data + 4) != ART_VERSION)
            return ArtResult::BadVersion;
        const std::uint32_t count = detail::readU32(data + 8);
        if (count > ART_MAX_PLATES || size != ART_HEADER_BYTES + (std::size_t)count * ART_PLATE_BYTES)
            return ArtResult::BadCount;

        std::vector<ArtPlate> plates(count);
        const std::uint8_t* p = data + ART_HEADER_BYTES;
        for (std::uint32_t i = 0; i < count; ++i, p += ART_PLATE_BYTES)
        {
            ArtPlate& plate = plates[i];
            plate.layer = detail::readU16(p);
            for (int c = 0; c < 4; ++c)
            {
                for (int k = 0; k < 3; ++k)
                {
                    plate.corner[c][k] = detail::readF32(p + 4 + (c * 3 + k) * 4);
                }
            }
            if (!detail::plateFits(plate))
                return ArtResult::BadPlate;
        }
        std::memcpy(out.sourceSha256, data + 12, sizeof(out.sourceSha256));
        out.plates.swap(plates);
        return ArtResult::Ok;
    }

    // The layers the plates need (the highest layer and one): the pictures must have as many.
    inline std::uint32_t artLayersNeeded(const Art& art)
    {
        std::uint32_t needed = 0;
        for (const ArtPlate& plate : art.plates)
        {
            needed = plate.layer + 1u > needed ? plate.layer + 1u : needed;
        }
        return needed;
    }

    // ---- the pictures --------------------------------------------------------------------
    constexpr std::uint16_t ART_TEX_VERSION = 2;    // 2: mip first (1, layer first, is not read)
    constexpr std::size_t ART_TEX_HEADER_BYTES = 16;
    constexpr std::uint32_t ART_TEX_SIZE = 512;
    constexpr std::uint32_t ART_TEX_MIPS = 8;       // 512 down to 4: a level is whole blocks
    constexpr std::size_t ART_TEX_BLOCK_BYTES = 8;  // BC4: a 4x4 block of one channel

    // The side of a mip (pixels) and its bytes.
    constexpr std::uint32_t artTexSide(std::uint32_t mip)
    {
        return ART_TEX_SIZE >> mip;
    }

    constexpr std::size_t artTexMipBytes(std::uint32_t mip)
    {
        return (std::size_t)(artTexSide(mip) / 4) * (artTexSide(mip) / 4) * ART_TEX_BLOCK_BYTES;
    }

    // The bytes of one layer, all its mips.
    constexpr std::size_t artTexLayerBytes()
    {
        std::size_t bytes = 0;
        for (std::uint32_t mip = 0; mip < ART_TEX_MIPS; ++mip)
        {
            bytes += artTexMipBytes(mip);
        }
        return bytes;
    }

    static_assert(artTexSide(ART_TEX_MIPS - 1) >= 4, "the smallest mip is a whole block");

    // Where a mip's blocks start, from the start of the file, for a file of layers layers: the mip
    // of layer 0, then of layer 1, ... (all the layers of a mip in one run: uploaded in one copy).
    constexpr std::size_t artTexMipOffset(std::uint32_t layers, std::uint32_t mip)
    {
        std::size_t offset = ART_TEX_HEADER_BYTES;
        for (std::uint32_t m = 0; m < mip; ++m)
        {
            offset += (std::size_t)layers * artTexMipBytes(m);
        }
        return offset;
    }

    enum class ArtTexResult
    {
        Ok,
        TooShort,
        BadMagic,
        BadVersion,
        BadShape,   // not 512 x 512 with 8 mips
        BadCount,   // no layer, more than ART_MAX_LAYERS, or the size does not match them
    };

    // Checks starart-tex.bin's bytes (they are uploaded as they are: nothing is decoded) and gives
    // the number of layers (0 on any error).
    inline ArtTexResult checkArtTexture(const std::uint8_t* data, std::size_t size, std::uint32_t& layers)
    {
        layers = 0;
        if (!data || size < ART_TEX_HEADER_BYTES)
            return ArtTexResult::TooShort;
        if (std::memcmp(data, "ATEX", 4) != 0)
            return ArtTexResult::BadMagic;
        if (detail::readU16(data + 4) != ART_TEX_VERSION)
            return ArtTexResult::BadVersion;
        if (detail::readU16(data + 12) != ART_TEX_SIZE || detail::readU16(data + 14) != ART_TEX_MIPS)
            return ArtTexResult::BadShape;
        const std::uint32_t count = detail::readU32(data + 8);
        if (count == 0 || count > ART_MAX_LAYERS || size != ART_TEX_HEADER_BYTES + (std::size_t)count * artTexLayerBytes())
            return ArtTexResult::BadCount;
        layers = count;
        return ArtTexResult::Ok;
    }
} // namespace realstars
} // namespace plus

#endif // PLUS_REALSTARS_REALSTARART_H
