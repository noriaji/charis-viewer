/**
 * @file skinfill.h
 * @brief Softer shadows on faces: a fill light and weaker ambient occlusion
 *        on the skin only (research/110-skin-fill.md).
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

#ifndef PLUS_SKINFILL_H
#define PLUS_SKINFILL_H

// Both hooks return at once while the feature is off (PlusSkinFill 0, the
// default, and no snapshot boost), with GL or LLRHI_SKIN_FILL=0, and on frames
// whose G-buffer pass marked no skin (plus/skinsss). Main thread.
namespace plus::skinfill
{
    // The <plus> hook in LLPipeline::renderDeferredLighting(), after the sun
    // program painted the light map and GTAO drew into it, before the official
    // blur: the ambient occlusion (the light map's g) is weakened on the skin
    // pixels. The blur then softens the edge of the skin with the rest.
    void renderLightMap();

    // The <plus> hook in LLPipeline::renderDeferredLighting(), after the
    // opaque lighting (sun, local lights) and before the skin's scattering and
    // the alpha geometry, with mRT->screen bound: a fill light from the sun's
    // (or moon's) colour on the skin pixels it does not reach, where what
    // shades them is near (brows, nose, hair, a hat).
    void render();
}

#endif // PLUS_SKINFILL_H
