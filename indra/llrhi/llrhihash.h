/**
 * @file llrhihash.h
 * @brief FNV-1a (64 bits): its constants and loops in one place, for
 *        llvulkan, llrhi and the viewer (research/273 #9).
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

#ifndef LL_LLRHIHASH_H
#define LL_LLRHIHASH_H

#include "stdtypes.h"

#include <cstddef>
#include <cstring>

namespace LLRHI
{
    // FNV-1a, 64 bits: from a start, hash = (hash ^ input) * FNV1A_PRIME
    // for each input. Every function takes the hash to go on from (one of
    // the two starts below, or an earlier result) and returns the new one.
    // The values are kept on disk in places (the shader cache's key, the
    // pipeline cache's format 1): a start, an input's width or the order of
    // the inputs changed there changes every key made before.

    // The offset basis.
    constexpr U64 FNV1A_BASIS = 14695981039346656037ull;
    // The basis with its last digit missing, where most of the per-value
    // hashes start (stamps, cache keys, fingerprints). Kept as it is: a
    // different start would give every one of them a different value.
    constexpr U64 FNV1A_BASIS_SHORT = 1469598103934665603ull;
    constexpr U64 FNV1A_PRIME = 1099511628211ull;

    // One step with a whole value (a word, a pointer, a packed key), not
    // its bytes. A narrower value is widened to 64 bits as `hash ^ value`
    // would widen it.
    constexpr U64 fnv1aMix(U64 hash, U64 value)
    {
        return (hash ^ value) * FNV1A_PRIME;
    }

    // A byte at a time.
    inline U64 fnv1a(U64 hash, const void* data, size_t size)
    {
        const unsigned char* p = (const unsigned char*)data;
        for (size_t i = 0; i < size; ++i)
        {
            hash = fnv1aMix(hash, p[i]);
        }
        return hash;
    }

    // A 32-bit word at a time.
    inline U64 fnv1aWords(U64 hash, const U32* words, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            hash = fnv1aMix(hash, words[i]);
        }
        return hash;
    }

    // A 64-bit word at a time.
    inline U64 fnv1aWords(U64 hash, const U64* words, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            hash = fnv1aMix(hash, words[i]);
        }
        return hash;
    }

    // Floats as their bits, a 32-bit word each (bitwise: -0 and 0 differ, a NaN is its bits).
    inline U64 fnv1aFloats(U64 hash, const F32* values, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            U32 bits;
            memcpy(&bits, &values[i], 4);
            hash = fnv1aMix(hash, bits);
        }
        return hash;
    }

    // FNV-1a over values, one at a time (no padding bytes), from the basis.
    struct Digest
    {
        U64 h = FNV1A_BASIS;
        void bytes(const void* data, size_t size)
        {
            h = fnv1a(h, data, size);
        }
        template <typename T>
        Digest& operator<<(const T& value)
        {
            bytes(&value, sizeof(T));
            return *this;
        }
    };
}

#endif // LL_LLRHIHASH_H
