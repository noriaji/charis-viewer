/**
 * @file volumetric.h
 * @brief "Volumetric light": sun shafts, the light the air scatters towards
 *        the eye where the sun reaches it (research/84).
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

#ifndef PLUS_VOLUMETRIC_H
#define PLUS_VOLUMETRIC_H

namespace plus::volumetric
{
    // The <plus> hook at the start of LLPipeline::renderFinalize(): adds the
    // scattered light to the scene's HDR target (mRT->screen) before
    // exposure and tone mapping read it, unless renderBeforeAlpha() did this
    // frame (the default). Returns at once when the feature is
    // off (PlusVolumetric, the default), the backend cannot draw it (GL, or
    // LLRHI_VOLUMETRIC=0), or there is nothing to draw it with (no sun
    // shadows, sun and moon down, camera under water). Main thread.
    void render();

    // The <plus> hook at the start of LLDrawPoolAlpha::renderPostDeferred()
    // for the main view's above-water alpha, with mRT->screen bound: draws
    // this frame's shafts there (PlusVolumetricAlpha 3, the default), so the
    // alpha blends over them as over the rest of the scene behind it; render()
    // then draws none this frame. Otherwise returns at once. Main thread.
    void renderBeforeAlpha();

    // With the shafts drawn after the alpha (PlusVolumetricAlpha 1 or 2, to compare), the
    // alpha depth they want this frame, asked by the same hook further on: the
    // shafts stop each view ray at the scene depth, which the alpha leaves out.
    // 0: none (drawn before the alpha, off, or nothing to draw); 1: the rigged
    // alpha's, written after the OIT resolve as upstream's rigged pass does
    // without OIT; 2: all the alpha's, as depth of field writes it. Main thread.
    U32 alphaDepth();

    // Whether this frame's light shafts will be drawn (the setting is on, the
    // backend and the sun's shadows are there, the sun or moon gives light, the
    // march's targets are not waiting after VRAM ran out): the ground fog (plus/atmosphere)
    // then leaves the shadowed part of its sun glow to them. Main thread.
    bool drawsShafts();
}

#endif // PLUS_VOLUMETRIC_H
