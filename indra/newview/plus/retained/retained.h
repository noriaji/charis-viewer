/**
 * @file retained.h
 * @brief Stage 0 of the retained, GPU driven main view: counts only
 *        (LLRHI_RETAINED_CENSUS, research/195 3.1 in the sl-viewer repository).
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

#ifndef PLUS_RETAINED_RETAINED_H
#define PLUS_RETAINED_RETAINED_H

#include "stdtypes.h"

#include <atomic>

class LLCamera;
class LLCullResult;

// Stage 1 of research/195 draws the main view's static opaque batches from
// blocks (a group's batches of one render type, registered once, the GPU
// probe list's G5 widened to the main camera). What it saves depends on
// numbers nobody has counted on the main view: how many batches a block
// would have, how many of the visible groups stay the same frame after frame,
// how many buckets (program, vertex format) there are, whether the GPU list's
// tables take the main view's size. This census counts them, on the main
// camera's postSort, after the render map is built, and writes one VKPerf
// line every 5 seconds with the stage 1 estimate from them
// ("retained census ... S1 est >= 0.5ms: yes|no").
//
// LLRHI_RETAINED_CENSUS=1 turns it on (unset or 0: off, the default). Off:
// one flag test per postSort and per LLDrawInfo made or freed, nothing else.
// It only reads: the groups, their draw maps and batches, as the render map
// loop has just read them.
namespace plus::retained
{
    namespace detail
    {
        extern const bool sOn;                  // LLRHI_RETAINED_CENSUS, read once at start
        extern std::atomic<U32> sDrawInfoMade;  // since the last census
        extern std::atomic<U32> sDrawInfoFreed;
        extern std::atomic<U32> sGeomBuilds;
        void censusSlow(LLCamera& camera, LLCullResult& cull);
    }

    inline bool on()
    {
        return detail::sOn;
    }

    // The end of LLPipeline::postSort's render map: the main camera's census.
    inline void census(LLCamera& camera, LLCullResult* cull)
    {
        if (detail::sOn && cull)
        {
            detail::censusSlow(camera, *cull);
        }
    }

    // LLDrawInfo's constructor and destructor, LLSpatialGroup::rebuildGeom
    // of a dirty group (any thread: relaxed counts).
    inline void noteDrawInfoMade()
    {
        if (detail::sOn)
        {
            detail::sDrawInfoMade.fetch_add(1, std::memory_order_relaxed);
        }
    }
    inline void noteDrawInfoFreed()
    {
        if (detail::sOn)
        {
            detail::sDrawInfoFreed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    inline void noteGeomBuild()
    {
        if (detail::sOn)
        {
            detail::sGeomBuilds.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

#endif // PLUS_RETAINED_RETAINED_H
