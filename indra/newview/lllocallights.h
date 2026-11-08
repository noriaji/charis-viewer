/**
 * @file lllocallights.h
 * @brief The local light table (research/197 stage 3c, research/203): what the
 *        light loops read of each point light, kept and read again only when
 *        the light changed.
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

#ifndef LL_LLLOCALLIGHTS_H
#define LL_LLLOCALLIGHTS_H

#include "stdtypes.h"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

class LLDrawable;

// Every frame LLPipeline::calcNearbyLights reads, of every light in the draw
// distance, its intensity, radius, position, attachment and avatar, and the
// light loop of renderDeferredLighting reads its center, radius, color,
// falloff and kind (research/187 N1: 505 lights in heavy-leeaf-2, lights
// 0.65 ms, most of it cache misses on the objects and their light
// parameters). Of a light that does not move, none of it changes until its
// parameters or its state do. The table keeps it per light (a record), marks
// a record when one of those changes, and the loops read a light from its
// record unless the record is marked (read again then), the light moves (an
// active drawable) or belongs to an avatar (both read from the object every
// frame, as before).
//
// What marks a record (research/203 2.2):
// - LLVOVolume::parameterChanged for PARAMS_LIGHT / PARAMS_LIGHT_IMAGE (every
//   setter and every update from the server goes through it);
// - LLDrawable::makeActive / makeStatic (a static drawable becomes active
//   before it moves: LLDrawable::updateMove; attaching makes it active,
//   detaching static);
// - LLPipeline::shiftObjects (the agent frame moved: every record);
// - LLViewerObject::setSelected (calc_light_dist puts a selected light first).
// A record lives while its drawable is in LLPipeline::mLights (setLight adds
// it; setLight, unlinkDrawable and removeMutedAVsLights remove it); mLights
// holds the drawable with an LLPointer meanwhile.
//
// The switch LLRHI_LIGHT_TABLE (llrhi.h): off, verify (the loops read the
// objects as before and compare every record they would have read, bit for
// bit), auto (verify, then on after AUTO_FRAMES frames with comparisons and
// no difference; off at the first), on.

// ---- research/203 local light rule
// (compiled on its own by tools/selftest/sim/sim_lighttable.cpp: no viewer
// type here besides F32 and U32; the drawable type is a template parameter
// with a public U32 mLightTableSlot)
namespace LLLocalLights
{
    // What the light loops read of a light. Floats and a flag word only (no
    // padding): compared bit for bit.
    struct Values
    {
        F32 render_position[3];     // LLVOVolume::getRenderPosition() (calc_light_dist)
        F32 intensity;              // getLightIntensity()
        F32 radius;                 // getLightRadius()
        F32 center[3];              // LLDrawable::getPositionAgent() (the light's box)
        F32 size;                   // getLightRadius() * 1.5f
        F32 color[3];               // getLightLinearColor()
        F32 falloff;                // getLightFalloff(DEFERRED_LIGHT_FALLOFF)
        U32 flags;                  // FLAG_*
    };
    static_assert(sizeof(Values) == 56, "14 words, no padding");

    enum : U32
    {
        FLAG_ATTACHMENT = 1,        // isAttachment()
        FLAG_HUD = 2,               // isHUDAttachment()
        FLAG_AVATAR = 4,            // getAvatar() != null
        FLAG_SPOTLIGHT = 8,         // isLightSpotlight()
        FLAG_SELECTED = 16,         // isSelected()
    };

    // The name of the first member in which a and b differ bit for bit (null: none).
    inline const char* firstDifference(const Values& a, const Values& b)
    {
        struct Member
        {
            size_t offset;
            size_t bytes;
            const char* name;
        };
        static const Member members[] = {
            { offsetof(Values, render_position), sizeof(Values::render_position), "render position" },
            { offsetof(Values, intensity), sizeof(Values::intensity), "intensity" },
            { offsetof(Values, radius), sizeof(Values::radius), "radius" },
            { offsetof(Values, center), sizeof(Values::center), "center" },
            { offsetof(Values, size), sizeof(Values::size), "size" },
            { offsetof(Values, color), sizeof(Values::color), "color" },
            { offsetof(Values, falloff), sizeof(Values::falloff), "falloff" },
            { offsetof(Values, flags), sizeof(Values::flags), "kind (attachment, HUD, avatar, spot, selected)" },
        };
        const char* pa = reinterpret_cast<const char*>(&a);
        const char* pb = reinterpret_cast<const char*>(&b);
        for (const Member& m : members)
        {
            if (memcmp(pa + m.offset, pb + m.offset, m.bytes))
            {
                return m.name;
            }
        }
        return nullptr;
    }

    // A frame's counts.
    struct Counts
    {
        U32 table = 0;          // lights read from their record
        U32 refreshed = 0;      // records read again from their object (marked)
        U32 object = 0;         // lights read from the object (moving, an avatar's, no record, verify)
        U32 checked = 0;        // records compared with their object (verify)
        U32 mismatches = 0;     // ... that differed

        void add(const Counts& c)
        {
            table += c.table;
            refreshed += c.refreshed;
            object += c.object;
            checked += c.checked;
            mismatches += c.mismatches;
        }
    };

    // The records, by the drawable's mLightTableSlot. A slot is checked
    // against its owner on every look-up (a drawable whose slot was given to
    // another reads as having none).
    template <class Drawable>
    class Slots
    {
    public:
        static constexpr U32 NONE = 0xffffffffu;

        struct Record
        {
            Drawable* owner = nullptr;
            Values values = {};
            bool clean = false;     // false: read the object again at the next use
        };

        // The drawable joined the lights (again: its record is marked).
        void add(Drawable* drawable)
        {
            if (!drawable)
            {
                return;
            }
            if (Record* record = find(drawable))
            {
                record->clean = false;
                return;
            }
            U32 slot;
            if (!mFree.empty())
            {
                slot = mFree.back();
                mFree.pop_back();
            }
            else
            {
                slot = (U32)mRecords.size();
                mRecords.emplace_back();
            }
            mRecords[slot] = Record();
            mRecords[slot].owner = drawable;
            drawable->mLightTableSlot = slot;
            ++mLive;
        }

        // The drawable left the lights.
        void remove(Drawable* drawable)
        {
            Record* record = find(drawable);
            if (!record)
            {
                return;
            }
            const U32 slot = drawable->mLightTableSlot;
            *record = Record();
            mFree.push_back(slot);
            drawable->mLightTableSlot = NONE;
            --mLive;
        }

        Record* find(const Drawable* drawable)
        {
            if (!drawable)
            {
                return nullptr;
            }
            const U32 slot = drawable->mLightTableSlot;
            if (slot >= (U32)mRecords.size() || mRecords[slot].owner != drawable)
            {
                return nullptr;
            }
            return &mRecords[slot];
        }

        void markDirty(const Drawable* drawable)
        {
            if (Record* record = find(drawable))
            {
                record->clean = false;
            }
        }

        void markAllDirty()
        {
            for (Record& record : mRecords)
            {
                record.clean = false;
            }
        }

        U32 live() const { return mLive; }
        U32 span() const { return (U32)mRecords.size(); }

    private:
        std::vector<Record> mRecords;
        std::vector<U32> mFree;
        U32 mLive = 0;
    };

    // The values a loop reads a light from: its record (refreshed first when
    // marked), or null for "read the object" (the light moves, it is an
    // avatar's, it has no record, or the table only verifies).
    // moving: its drawable is active. reads: the table is on. verifies: the
    // table verifies. check: this call compares a clean record with the
    // object (once per light and frame: calcNearbyLights). read(Values&):
    // reads the light from its object now. report(field, was, now): a
    // record that differed from its object (it takes the object's values).
    template <class Drawable, class Read, class Report>
    const Values* serve(Slots<Drawable>& slots, const Drawable* drawable, bool moving, bool reads, bool verifies, bool check,
                        Counts& counts, Read&& read, Report&& report)
    {
        typename Slots<Drawable>::Record* record = (reads || verifies) ? slots.find(drawable) : nullptr;
        if (!record || moving)
        {
            ++counts.object;
            return nullptr;
        }
        if (!record->clean)
        {
            read(record->values);
            record->clean = true;
            ++counts.refreshed;
        }
        else if (verifies && check)
        {
            Values now;
            read(now);
            ++counts.checked;
            if (const char* field = firstDifference(record->values, now))
            {
                ++counts.mismatches;
                report(field, record->values, now);
                record->values = now;
            }
        }
        if (!reads || (record->values.flags & FLAG_AVATAR))
        {
            ++counts.object;
            return nullptr;
        }
        ++counts.table;
        return &record->values;
    }

    // ---- calcNearbyLights' first loop, applied to the set (research/203 2.3)
    // One entry of the nearby set: its node, and whether it stays with its new key.
    template <class Iterator>
    struct NearbyUpdate
    {
        Iterator it;
        F32 dist;
        F32 fade;
        bool keep;
    };

    // LLPipeline::Light::compare on (dist, drawable).
    template <class P>
    inline bool keyLess(F32 da, const P* a, F32 db, const P* b)
    {
        if (da < db)
        {
            return true;
        }
        if (da > db)
        {
            return false;
        }
        return a < b;
    }

    // Whether the kept entries' new keys come in strictly increasing order in
    // the set's present order: then they can take them where they are.
    template <class Update>
    bool keysStayOrdered(const std::vector<Update>& updates)
    {
        const Update* prev = nullptr;
        for (const Update& u : updates)
        {
            if (!u.keep)
            {
                continue;
            }
            if (prev && !keyLess(prev->dist, prev->it->drawable.get(), u.dist, u.it->drawable.get()))
            {
                return false;
            }
            prev = &u;
        }
        return true;
    }

    // Only when keysStayOrdered: the others leave, the kept ones take their
    // new keys in their nodes (the tree's order is kept, so the set is the
    // one the nodes moved over would make).
    template <class Set, class Update>
    void applyInPlace(Set& set, const std::vector<Update>& updates)
    {
        for (const Update& u : updates)
        {
            if (!u.keep)
            {
                set.erase(u.it);
            }
        }
        for (const Update& u : updates)
        {
            if (u.keep)
            {
                typename Set::value_type& light = const_cast<typename Set::value_type&>(*u.it);
                light.dist = u.dist;
                light.fade = u.fade;
            }
        }
    }

    // research/187 N1a: the kept entries' nodes move over to `scratch` (empty)
    // with their new keys, the set takes them, the others leave.
    template <class Set, class Update>
    void applyByNodes(Set& set, Set& scratch, const std::vector<Update>& updates)
    {
        for (const Update& u : updates)
        {
            if (u.keep)
            {
                typename Set::node_type node = set.extract(u.it);
                node.value().dist = u.dist;
                node.value().fade = u.fade;
                scratch.insert(std::move(node));
            }
        }
        set.swap(scratch);
        scratch.clear();
    }
}
// ---- end of the research/203 local light rule ----

// The table: the records, the switch's state and the LightTable line.
// Main thread only.
class LLLocalLightTable
{
public:
    using Slots = LLLocalLights::Slots<LLDrawable>;
    static constexpr U32 AUTO_FRAMES = 600;

    // mLights' membership (LLPipeline::setLight, unlinkDrawable, removeMutedAVsLights).
    void add(LLDrawable* drawable) { mSlots.add(drawable); }
    void remove(LLDrawable* drawable) { mSlots.remove(drawable); }

    // LLVOVolume::parameterChanged.
    void paramsChanged(LLDrawable* drawable, U16 param_type);
    // LLDrawable::makeActive / makeStatic, LLViewerObject::setSelected.
    void stateChanged(const LLDrawable* drawable) { mSlots.markDirty(drawable); }
    // LLPipeline::shiftObjects.
    void shifted() { mSlots.markAllDirty(); }

    // The switch, as auto has moved it.
    static bool reads();        // the loops read from the records (on)
    static bool verifies();     // the loops read the objects and compare (verify, auto's first part)

    Slots& slots() { return mSlots; }
    LLLocalLights::Counts& counts() { return mFrame; }

    // calcNearbyLights, once per main view frame before its loops: the frame
    // before's counts move auto on, and go into the LightTable line (every
    // 5 s while verifying or while the frame cost accounting is on).
    void beginFrame();
    // serve()'s report: the first few go to the log.
    void reportMismatch(const char* field, const LLLocalLights::Values& was, const LLLocalLights::Values& now);

private:
    Slots mSlots;
    LLLocalLights::Counts mFrame;
    LLLocalLights::Counts mPeriod;
    U32 mPeriodFrames = 0;
    U32 mAutoCleanFrames = 0;
    U32 mLoggedMismatches = 0;
    F32 mLoggedAt = -1.f;
};

extern LLLocalLightTable gLocalLightTable;

#endif // LL_LLLOCALLIGHTS_H
