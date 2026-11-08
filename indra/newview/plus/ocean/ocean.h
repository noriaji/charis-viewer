/**
 * @file ocean.h
 * @brief The ocean: which water is sea, how far each part of it is from the
 *        shore and where the shore lies, as a small texture the water shader
 *        reads to draw surf foam and incoming waves on the sea only
 *        (research/112-ocean-shader-plan.md in the sl-viewer repository).
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

#ifndef PLUS_OCEAN_OCEAN_H
#define PLUS_OCEAN_OCEAN_H

#include "stdtypes.h"

class LLGLSLShader;

namespace plus::ocean
{
    // The <plus> hook in LLDrawPoolWater::renderPostDeferred(), after the
    // official uniforms and before the water planes are drawn: binds the shore
    // field and sets the ocean's uniforms on the water shader. With the
    // setting off (PlusOcean) or no field yet it only tells the shader the
    // ocean is off. Main thread, inside a frame; makes no texture of its own
    // (the field is kept up to date at idle).
    void bind(LLGLSLShader* shader);

    // Pure helpers (no viewer state), kept apart for the tests.
    namespace field
    {
        constexpr S32 GRID           = 256;     // cells per side
        constexpr F32 CELL_METERS    = 4.f;     // one cell
        constexpr F32 SPAN_METERS    = GRID * CELL_METERS;
        constexpr F32 MAX_SHORE_DIST = 64.f;    // the distance the field's red channel stops at (m)

        enum Kind : U8
        {
            KIND_UNKNOWN = 0,   // no terrain loaded here
            KIND_LAND    = 1,   // at or above the water
            KIND_WATER   = 2,   // below the water
        };

        enum Body : U8
        {
            BODY_NONE = 0,
            BODY_POND = 1,
            BODY_SEA  = 2,
        };

        // Sorts the water cells into sea and ponds. 4-connected bodies of
        // water: a body is sea when it covers at least min_area (m2), or a
        // quarter of it while it touches the edge of the field or of the
        // loaded terrain (it may go on past it). body: GRID x GRID, set for
        // every cell (BODY_NONE for land and unknown cells).
        void classify(const U8* kind, F32 min_area, U8* body);

        // Distance in meters from each cell to the nearest land cell (exact
        // distance transform, smoothed), stopped at MAX_SHORE_DIST. Cells with no land near
        // (unknown ones too) get MAX_SHORE_DIST. dist: GRID x GRID.
        void shoreDistance(const U8* kind, F32* dist);

        // The field's pixels (RGBA8, GRID x GRID, row j = cells with y index j):
        //   R  distance to the shore / MAX_SHORE_DIST
        //   G,B  unit vector to the shore (x, y of the agent space) * 0.5 + 0.5
        //   A  255 sea, 128 pond, 0 neither
        // Land next to water repeats its neighbour's values (two cells deep),
        // so the shader's bilinear sampling stays smooth at the waterline.
        void pack(const U8* kind, const U8* body, const F32* dist, U8* rgba);
    }
}

#endif // PLUS_OCEAN_OCEAN_H
