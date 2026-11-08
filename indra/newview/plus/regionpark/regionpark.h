/**
 * @file regionpark.h
 * @brief Teleport and revisit speed: the textures of the regions just left
 *        stay while VRAM allows, and after an arrival the cached objects in
 *        reach are made before they come into view.
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

#ifndef PLUS_REGIONPARK_H
#define PLUS_REGIONPARK_H

#include "stdtypes.h"

class LLViewerFetchedTexture;
class LLViewerRegion;

// research/242. Upstream already keeps a region left by a teleport, with all
// its objects, until the simulator closes it (DisableSimulator, about 50 s):
// LLVOCacheEntry::isAnyVisible() always says yes, so killInvisibleObjects()
// removes nothing, and a teleport back within that time finds the region and
// its objects as they were. What a teleport back loses is the textures, which
// plus/tprelease sends down after the teleport so VRAM does not run over
// (research/97). And after the region has closed, upstream makes the cached
// objects again only as they come into view.
//
// PlusRegionPark (off by default): a texture last on screen during one of the
// last PlusRegionParkMax visits before this one (a visit: from a teleport or
// the login to the next teleport) is parked while VRAM has room (no texture
// pressure, discard bias 1 or under, VRAM in use under 80 % of the budget,
// system memory not short): tprelease
// does not send it down, and with nothing using it (its region has closed) it
// stays in the list PlusRegionParkTextureHold seconds instead of upstream's 30
// (or tprelease's 5). Under pressure tprelease works as before, from the next
// visit of the texture (about a second). Only textures of the kind tprelease
// sends down (LOD, not boosted) are parked.
//
// PlusRegionParkEager (off by default): for 30 s after a teleport or the login,
// in the region arrived in, the valid cached objects within the draw distance
// are queued for creation even when out of view (upstream: only those in view,
// the rest when the camera turns), with upstream's own measure (in the object
// cache tree, large enough on screen), PlusRegionParkEagerPerFrame objects a
// frame counting a linkset's children (twice that under the teleport screen).
// The window closes early (the teleport screen gone, EAGER_MIN_SECONDS after the
// arrival) after two sweeps in a row that made nothing (queued
// nothing, or the region's live cached objects did not grow). Not on a walking
// region crossing, nor on a teleport within the region.
//
// Log (tag RegionPark): the region left (objects alive), the arrival (back to a
// region kept, or one new or closed since), then at +1, +3, +10 and +30 s the
// region's objects alive, the texture fetches waiting, when the teleport screen
// went, what was parked and made early, VRAM and memory; a parked region closed
// by the simulator. Main thread only.
namespace plus::regionpark
{
    // The hook at the top of plus::tprelease::onFaceScan(): true when the
    // texture is parked and tprelease is to leave it alone.
    bool skipScaleDown(const LLViewerFetchedTexture* imagep);

    // The hook at the top of plus::tprelease::lazyFlushTimeout(): true when the
    // texture is parked; timeout is then raised to PlusRegionParkTextureHold.
    bool holdInList(const LLViewerFetchedTexture* imagep, F32& timeout);

    // The <plus> hook in LLViewerRegion::idleUpdate(): how many objects (a
    // linkset counts with its children) out of view the region may queue this
    // frame (0: none: off, not the region arrived in, the window closed,
    // pressure). arrival: the arrival it is for (a new one starts a new sweep).
    U32 eagerBudget(const LLViewerRegion* regionp, U32& arrival);

    // After that hook's walk: pushed this frame; sweep_done when the walk came
    // to the end of the cache, with what the whole sweep pushed.
    void eagerDone(const LLViewerRegion* regionp, U32 pushed, bool sweep_done, U32 sweep_pushed);
}

#endif // PLUS_REGIONPARK_H
