/**
 * @file shadowfar.h
 * @brief The sun shadow cascades leave out small moving casters far from the
 *        camera (PlusShadowSkipFarSmall, research/178 C2 in the sl-viewer
 *        repository).
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

#ifndef PLUS_SHADOWFAR_H
#define PLUS_SHADOWFAR_H

#include "stdtypes.h"

class LLSpatialGroup;

// The casters drawn every frame (the groups of spatial bridges: active
// objects, rotating and scripted moving objects) go to the sun cascades as the
// cull found them. With PlusShadowSkipFarSmall, renderSunShadowCascades' sort
// leaves out a bridge group whose bridge is small (bounding radius at most
// PlusShadowSkipFarSmallRadius) and far (its nearest point beyond
// PlusShadowSkipFarSmallDistance from the camera). Never avatars, their
// attachments or animated objects (their shadows always stay). Static casters
// are not touched (the shadow cache holds them).
//
// Main view only: not the reflection probes' shadows, not snapshots.
// LLRHI_SHADOW_FAR_SMALL=0 keeps it off whatever the setting; with
// LLRHI_CUE_DIR, shadowfarsmall.toggle flips the setting (not saved) for an
// A/B in one start. Counts go to a VKPerf line every 5 seconds while on.
namespace plus::shadowfar
{
    // One sort's counts, kept by the caller (one per chunk on the cull threads).
    struct Count
    {
        U32 tested = 0;             // bridge groups looked at
        U32 left_out = 0;           // of them, left out
        U32 left_out_cascades = 0;  // the cascades they were left out of
    };

    namespace detail
    {
        extern bool sActive;
        bool leaveOutSlow(LLSpatialGroup* group, U32 mask, Count& count);
    }

    // Main thread, once per renderSunShadowCascades before its sort. main_view:
    // the main view's sun cascades (not the probes' shared maps, not a cube
    // snapshot). Reads the settings, the switch and the cue, and the camera.
    void beginSort(bool main_view);

    // Any thread, at the end of one chunk of the sort (or of the whole sort):
    // its counts, added once (atomics, not per group).
    void addCount(const Count& count);

    // Any thread (the sort's chunks run on the cull threads), between beginSort
    // and the end of the sort: true when the group, of a spatial bridge, is to
    // be left out of every cascade (mask: the cascades the cull found it in).
    // Reads only the group, its bridge and the bridge's object.
    inline bool leaveOut(LLSpatialGroup* group, U32 mask, Count& count)
    {
        return detail::sActive && detail::leaveOutSlow(group, mask, count);
    }
}

#endif // PLUS_SHADOWFAR_H
