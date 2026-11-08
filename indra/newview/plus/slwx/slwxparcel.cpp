/**
 * @file slwxparcel.cpp
 * @brief Watches the agent's parcel for a [slwx ...] tag (research/229).
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

#include "llviewerprecompiledheaders.h"

#include "slwxparcel.h"

#include "slwxsync.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llparcel.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "roles_constants.h"

#include <cstring>

namespace plus::slwx::parcel
{
    namespace
    {
        // The seconds the last parcel's weather is kept after a region crossing
        // while the new region's parcel is not known.
        constexpr F64 HOLD_SECONDS = 10.0;

        Target sTarget;
        LLUUID sRegion;             // the region the tag below was read in
        S32 sLocalId = -1;
        bool sHaveKey = false;      // sRegion / sLocalId / sTag describe what was read
        std::string sTag;           // the tag's text, "" with none
        LLUUID sSeenRegion;         // the agent's region on the last frame
        F64 sRegionChangedAt = 0.0;
        bool sStale = false;        // the agent's region changed and its parcel is not known yet
        bool sTeleported = false;   // a teleport since the last change of the target
        bool sImmediateNext = false;// reset(): the first tag read after it is taken at once

        void change(bool active, const tag::Params& params, uint32_t seed, int bad_items)
        {
            sTarget.active = active;
            sTarget.params = params;
            sTarget.seed = seed;
            sTarget.bad_items = bad_items;
            sTarget.immediate = sTeleported || sImmediateNext;
            sTeleported = false;
            ++sTarget.generation;
        }

        void drop()
        {
            if (sTarget.active)
            {
                change(false, tag::Params(), 0, 0);
            }
            else
            {
                sTeleported = false;
            }
        }

        // Whether the agent may write the parcel's description: the test the About Land window's
        // General tab enables its name and description by (a group's parcel to one with the power;
        // not a parcel of the agent's own that is not leased, one being sold).
        bool mayEdit(const LLParcel* parcel)
        {
            return parcel && LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_IDENTITY);
        }

        // The agent's parcel, when it is the one of the agent's region.
        LLParcel* freshParcel(const LLViewerRegion* region)
        {
            LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
            if (!parcel || !region)
            {
                return nullptr;
            }
            const LLUUID& id = parcel->getRegionID();
            if (id.notNull() && id != region->getRegionID())
            {
                return nullptr;     // the parcel of the region we came from
            }
            return parcel;
        }

        bool editDescription(const std::string& tag_text)
        {
            LLViewerRegion* region = gAgent.getRegion();
            LLParcel* current = freshParcel(region);
            if (!mayEdit(current))
            {
                return false;
            }
            std::string description = current->getDesc();
            if (!tag::replaceIn(description, tag_text))
            {
                return false;
            }
            // The update carries the whole parcel (there is no update of the description alone), as
            // the About Land window's does; the agent's parcel is what the simulator sent last, so a
            // change made elsewhere since then would be written back (research/275 L084: fetching the
            // parcel again first needs a wait for its answer, not done).
            LLParcel update(*current);
            update.setRegionID(region->getRegionID());
            update.setDesc(description);
            LLViewerParcelMgr::getInstance()->sendParcelPropertiesUpdate(&update);
            return true;
        }
    }

    // Reads the parcel's tag when the place or the tag is not the one read last.
    static void readParcel(LLParcel* parcel, const LLUUID& region_id)
    {
        const S32 local_id = parcel->getLocalID();
        const std::string& description = parcel->getDesc();
        size_t begin = 0;
        size_t end = 0;
        const bool found = tag::find(description, begin, end);
        // The same place and the same tag: nothing to do (an edit of the rest of
        // the description changes nothing).
        if (sHaveKey && sRegion == region_id && sLocalId == local_id &&
            (found ? description.compare(begin, end - begin, sTag) == 0 : sTag.empty()))
        {
            return;
        }
        sHaveKey = true;
        sRegion = region_id;
        sLocalId = local_id;
        sTag = found ? description.substr(begin, end - begin) : std::string();

        const tag::ParseResult read = tag::parse(description);
        if (!read.found || read.off)
        {
            drop();
            return;
        }
        uint32_t seed = read.params.seed;
        if (!read.params.has_seed)
        {
            uint32_t words[4];
            std::memcpy(words, region_id.mData, sizeof(words));
            seed = sync::seedFor(words, local_id);
        }
        if (read.bad_items > 0)
        {
            LL_WARNS("SLWX") << "The parcel's [slwx] tag has " << read.bad_items << " broken item(s); they are left out." << LL_ENDL;
        }
        change(true, read.params, seed, read.bad_items);
    }

    const Target& target()
    {
        return sTarget;
    }

    void reset()
    {
        const uint32_t generation = sTarget.generation + 1;
        sTarget = Target();
        sTarget.generation = generation;
        sTarget.immediate = true;
        sRegion.setNull();
        sLocalId = -1;
        sHaveKey = false;
        sTag.clear();
        sSeenRegion.setNull();
        sStale = false;
        sTeleported = false;
        sImmediateNext = true;
    }

    void update()
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            return;
        }
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        {
            sTeleported = true;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        const LLUUID& region_id = region->getRegionID();
        if (region_id != sSeenRegion)
        {
            sSeenRegion = region_id;
            sRegionChangedAt = now;
            sStale = true;
            if (sTeleported)
            {
                // A teleport: the old place's weather does not go on over here.
                drop();
                sHaveKey = false;
            }
        }

        LLParcel* parcel = freshParcel(region);
        if (!parcel)
        {
            if (sStale && now - sRegionChangedAt > HOLD_SECONDS)
            {
                drop();
                sHaveKey = false;
                sStale = false;
            }
            return;
        }
        sStale = false;
        readParcel(parcel, region_id);
        sImmediateNext = false;
        // Once the parcel of a teleport's goal has been read, the next change is an ordinary one.
        if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
        {
            sTeleported = false;
        }
    }

    bool canEdit()
    {
        LLParcel* current = freshParcel(gAgent.getRegion());
        return mayEdit(current);
    }

    bool setTag(const std::string& tag_text)
    {
        return !tag_text.empty() && editDescription(tag_text);
    }

    bool clearTag()
    {
        return editDescription(std::string());
    }
}
