/**
 * @file shadowshift.h
 * @brief Moving sun shadows: a cached cascade moved by whole texels instead of
 *        drawn again whole (PlusShadowShift, research/151-shadow-shift-reuse.md
 *        in the sl-viewer repository).
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

#ifndef PLUS_SHADOWSHIFT_SHADOWSHIFT_H
#define PLUS_SHADOWSHIFT_SHADOWSHIFT_H

#include "stdtypes.h"
#include "v3math.h"

#include "glm/mat4x4.hpp"

#include <vector>

class LLCamera;
class LLDrawInfo;
class LLRenderTarget;

// The static sun shadow cache (LLRHI::sCacheSunShadows) keeps each cascade's
// static casters' depth while the cascade's projection still holds what the
// camera sees. A stable cascade (plus/shadowdist) has its texel grid fixed in
// the world, so when the camera moves out of it, the same grid one whole
// number of texels further on holds the same depth where the two overlap:
// the cached depth is copied moved by that much, only the static casters of
// the strips that came into view are drawn, and the result is copied back as
// the new cached depth. Every texel ends as a whole redraw with the same
// projection would make it (research/151 sections 2 and 3).
//
// The <plus> hooks in pipeline.cpp (main thread, the main view's sun
// cascades only; probes and mirrors never come here). Each does nothing
// while PlusShadowShift is off.
namespace plus::shadowshift
{
    // Why a cascade's static depth was drawn whole (the VKPerf line).
    enum Why : U8
    {
        WHY_FIRST,          // no cached depth to move (first frame, invalidated)
        WHY_SUN,            // the sun moved more than the cache allows (0.05 degrees)
        WHY_WATER,          // the water clip plane changed
        WHY_TURN,           // the stable cascade turned a step (camera turned)
        WHY_SIZE,           // its size changed a step, or the receivers do not fit
        WHY_DEPTH,          // the receivers left its depth range
        WHY_LARGE,          // moved more than half the map: cheaper drawn whole
        WHY_CONTENT,        // what the cached depth holds was not recorded for this grid
        WHY_SORT,           // the sort did not run on the cull threads
        WHY_STABLE,         // not a stable cascade
        WHY_MISMATCH,       // the casters both windows share differ from the cached depth's
        WHY_LIST,           // a GPU shadow list group in the cached depth changed
        WHY_CLAMP,          // the casters' bias clamp changed
        WHY_AGE,            // the 2 s refresh (the safety net)
        WHY_CONTENTS,       // not moved: a cached group changed
        WHY_CULL,           // not moved: the cull found other static casters
        WHY_TARGET,         // the cache's target was made again
        WHY_DIRTY,          // a partial redraw (PlusShadowShiftPartial) would cover too much of the map
        WHY_PROJECTION,     // for beginContent: the reason noted this frame (or WHY_FIRST)
        WHY_COUNT
    };

    // LLPipeline::reuseShadowCascade(), first thing for cascade j each frame:
    // forgets the last frame's move, and on the frame's first cascade checks
    // the last frame's verified cascade (PlusShadowShiftVerify), writes the
    // VKPerf line and lets go of what PlusShadowShift off no longer needs.
    void beginReuse(S32 j);

    // LLPipeline::generateSunShadow(), each main view cascade, before
    // reuseShadowCascade (which runs only with the sun shadow cache): the
    // frame's PlusShadowShift (ditherWidth) and letting go once it is off,
    // with the cache off too. Once a frame does anything.
    void beginFrame();

    // LLPipeline::reuseShadowCascade(): the cached projection was refused for
    // a reason before the receivers were tested (WHY_SUN, WHY_WATER).
    void noteReject(S32 j, Why why);

    // LLPipeline::reuseShadowCascade(), when a receiver (points, agent space)
    // is outside the cached cascade: true when the same grid moved by whole
    // texels holds them all at this frame's size and turn (the current stable
    // cascade's view and proj tell), with view, proj and shadow_cam (its cull
    // planes; the near plane kept) set to it. False: drawn whole.
    bool accept(S32 j, const glm::mat4& cached_view, const glm::mat4& cached_proj, glm::mat4& view, glm::mat4& proj,
                LLCamera& shadow_cam, const std::vector<LLVector3>& points, U32 map_width, U32 map_height);

    // LLPipeline::renderCachedShadowCascade(), first thing: sun cascade
    // `index` (-1: not one), position its bit in the sort's entries (-1: the
    // sort did not run in chunks). True when accept() moved it this frame and
    // it can be drawn moved.
    bool beginDraw(S32 index, S32 position);

    // A moved cascade, when nothing else has it drawn whole: beginStrips, then
    // noteCached for every cached caster the sort gave it (with its hash),
    // then drawShift. drawShift draws into shadow (the cascade's map, left
    // holding the static depth) and cache, or returns false (nothing drawn:
    // draw it whole). list_serial: the GPU shadow list's rows in the cached depth.
    // With PlusShadowShiftPartial, casters gone or come where both windows look
    // have their rects cleared and drawn again instead (research/158 section 2).
    void beginStrips(S32 index, const glm::mat4& view, const glm::mat4& proj);
    void noteCached(U32 type, LLDrawInfo* info, U64 hash);
    bool drawShift(S32 index, LLRenderTarget& shadow, LLRenderTarget& cache, const glm::mat4& view, const glm::mat4& proj,
                   LLCamera& camera, U32 list_serial);

    // A cascade's static depth drawn whole: when recording() (PlusShadowShift
    // on), beginContent, noteContent for every static caster drawn into it
    // (cached and joining), endContent; the next move compares against that.
    bool recording(S32 index, S32 position);
    void beginContent(S32 index, const glm::mat4& view, const glm::mat4& proj, U32 width, U32 height, Why why);
    void noteContent(LLDrawInfo* info, U64 hash);
    void endContent();

    // LLPipeline::releaseShadowCaches(): the caches' depth is gone.
    void release();

    // The shadow_target_width the alpha shadow shaders get (and the native
    // shadow packets): width as it is, or width + 1 while PlusShadowShift is on
    // for a main view sun cascade. The shaders' dither takes the column
    // 0.5 * (shadow_target_width - 1) * x_ndc: with width + 1 it is the grid's
    // texel column counted from the window's centre (half way between whole
    // numbers at texel centres), so a window moved by an even number of
    // texels (accept) keeps every caster's dither where a whole redraw puts it.
    U32 ditherWidth(U32 width);

    // LLPipeline::renderSunShadowCascades(), before the sort of the cascades'
    // groups (main thread): takes the shadowshift.toggle cue (LLRHI_CUE_DIR),
    // and says whether the sort may count a group it sees for the first time
    // as settled since its last build (PlusShadowShiftFirstSeen, research/158
    // section 3; only while a cascade moved or left its cached projection).
    bool beginSort();
    // The groups the sort counted so (the VKPerf line).
    void noteFirstSeen(U32 groups);
}

#endif // PLUS_SHADOWSHIFT_SHADOWSHIFT_H
