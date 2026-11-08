/**
 * @file llvkframetable.h
 * @brief A U64 -> U32 table emptied every frame (or pass) without freeing anything.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#ifndef LL_LLVKFRAMETABLE_H
#define LL_LLVKFRAMETABLE_H

#include "stdtypes.h"

#include <vector>

// fix-pipeleak: for lookups that start empty every frame or pass. An
// unordered_map emptied with clear() frees every entry and allocates each
// again in the next frame; here the slots stay allocated and clear() only
// starts a new generation: a slot holds an entry only while its generation
// is the current one. Open addressing with linear probing, at most half full
// (a probe always ends at a free slot). Not thread safe.
class LLVKFrameTable
{
public:
    void clear()
    {
        mUsed = 0;
        if (++mGeneration == 0)
        {
            // Wrapped around: stamps of 4 billion clears ago would read as current.
            for (Slot& slot : mSlots)
            {
                slot.generation = 0;
            }
            mGeneration = 1;
        }
    }

    // The value stored for key, or null. Valid until the next set() or clear().
    U32* find(U64 key)
    {
        if (mSlots.empty())
        {
            return nullptr;
        }
        const size_t mask = mSlots.size() - 1;
        for (size_t i = hash(key) & mask;; i = (i + 1) & mask)
        {
            Slot& slot = mSlots[i];
            if (slot.generation != mGeneration)
            {
                return nullptr;
            }
            if (slot.key == key)
            {
                return &slot.value;
            }
        }
    }

    // Stores value for key, replacing what it had.
    void set(U64 key, U32 value)
    {
        if ((size_t)(mUsed + 1) * 2 > mSlots.size())
        {
            grow();
        }
        insert(key, value);
    }

    U32 size() const { return mUsed; }

private:
    struct Slot
    {
        U64 key = 0;
        U32 value = 0;
        U32 generation = 0;     // 0: never used (mGeneration is never 0)
    };

    static size_t hash(U64 key)
    {
        // The finalizer of MurmurHash3: pointers and FNV hashes alike spread over the low bits.
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ull;
        key ^= key >> 33;
        return (size_t)key;
    }

    void insert(U64 key, U32 value)
    {
        const size_t mask = mSlots.size() - 1;
        for (size_t i = hash(key) & mask;; i = (i + 1) & mask)
        {
            Slot& slot = mSlots[i];
            if (slot.generation != mGeneration)
            {
                slot.key = key;
                slot.value = value;
                slot.generation = mGeneration;
                ++mUsed;
                return;
            }
            if (slot.key == key)
            {
                slot.value = value;
                return;
            }
        }
    }

    // Twice the slots (a power of two), with the current entries moved over.
    void grow()
    {
        std::vector<Slot> old;
        old.swap(mSlots);
        mSlots.assign(old.empty() ? 64 : old.size() * 2, Slot());
        mUsed = 0;
        for (const Slot& slot : old)
        {
            if (slot.generation == mGeneration)
            {
                insert(slot.key, slot.value);
            }
        }
    }

    std::vector<Slot> mSlots;
    U32 mGeneration = 1;
    U32 mUsed = 0;
};

#endif // LL_LLVKFRAMETABLE_H
