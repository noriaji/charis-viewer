/**
* @file llrigginginfo.h
* @brief  Functions for tracking rigged box extents
*
* $LicenseInfo:firstyear=2018&license=viewerlgpl$
* Second Life Viewer Source Code
* Copyright (C) 2018, Linden Research, Inc.
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
*
* Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
* $/LicenseInfo$
*/

// Stores information related to associated rigged mesh vertices
// This lives in llmath because llvolume lives in llmath.

#ifndef LL_LLRIGGINGINFO_H
#define LL_LLRIGGINGINFO_H

#include "llvector4a.h"

// Extents are in joint space
// isRiggedTo is based on the state of all currently associated rigged meshes
class alignas(16) LLJointRiggingInfo
{
    LL_ALIGN_NEW
public:
    LLJointRiggingInfo();
    // <plus> research/348 1-a (rigged-joint-list): defined here so the walks over a whole table (a face's area, the
    // avatar's box) do not call a function for each of its entries (was: declared only, defined in llrigginginfo.cpp)
    bool isRiggedTo() const { return mIsRiggedTo; }
    void setIsRiggedTo(bool val) { mIsRiggedTo = val; }
    LLVector4a *getRiggedExtents() { return mRiggedExtents; }
    const LLVector4a *getRiggedExtents() const { return mRiggedExtents; }
    // </plus>
    void merge(const LLJointRiggingInfo& other);

private:
    LLVector4a mRiggedExtents[2];
    bool mIsRiggedTo;
};

// <plus> research/348 1-a (rigged-joint-list): the entries of a built table that are rigged to their joint, as joint
// numbers in rising order (the order of a walk over the whole table), so a walk visits those alone. Made only for the
// tables a face's area walk uses (LLFace::calcPixelArea, main thread), on the first walk after the table is built; gone
// with the table (clear, resize, merge, setNeedsUpdate(true), destruction). generation tells a list from any made
// before it (never 0).
struct PlusRigJointList
{
    static constexpr S32 CAPACITY = 256;    // joint numbers fit a U8; a table longer than this has no list
    U32 generation;
    U16 count;
    U8 joints[CAPACITY];
};
// </plus>

// For storing all the rigging info associated with a given avatar or
// object, keyed by joint_num.
// Using direct memory management instead of std::vector<> to avoid alignment issues.
class LLJointRiggingInfoTab
{
public:
    LLJointRiggingInfoTab();
    ~LLJointRiggingInfoTab();
    void resize(S32 size);
    void clear();
    S32 size() const { return mSize; }
    void merge(const LLJointRiggingInfoTab& src);
    LLJointRiggingInfo& operator[](S32 i) { return mRigInfoPtr[i]; }
    const LLJointRiggingInfo& operator[](S32 i) const { return mRigInfoPtr[i]; };
    bool needsUpdate() { return mNeedsUpdate; }
    // <plus> research/348 1-a: a table to be built again has no list
    void setNeedsUpdate(bool val)
    {
        mNeedsUpdate = val;
        if (val)
        {
            plusFreeJointList();
        }
    }
    // The list of the entries rigged to their joint (null: none made, the walk goes over every entry).
    const PlusRigJointList* plusJointList() const { return mPlusJoints; }
    // Makes the list from the table as it is now (replacing any) and returns it; null when it could not be made (the
    // table is empty or longer than the list can hold, or out of memory). The caller makes it only for a built table.
    const PlusRigJointList* plusBuildJointList();
    void plusFreeJointList()
    {
        delete mPlusJoints;
        mPlusJoints = nullptr;
    }
    // </plus>

    LLJointRiggingInfo* begin() { return mRigInfoPtr; }
    LLJointRiggingInfo* end() { return mRigInfoPtr + mSize; }

    LLJointRiggingInfoTab& operator=(const LLJointRiggingInfoTab& src) = delete;
    LLJointRiggingInfoTab(const LLJointRiggingInfoTab& src) = delete;

private:
    LLJointRiggingInfo *mRigInfoPtr;
    S32 mSize;
    bool mNeedsUpdate;
    PlusRigJointList* mPlusJoints = nullptr;  // <plus> research/348 1-a: see plusJointList </plus>
};

#endif
