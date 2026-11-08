/**
 * @file dofalpha.h
 * @brief Depth of field behind alpha: the depth of field's depth pass writes
 *        an alpha surface's depth only where it is mostly opaque.
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

#ifndef PLUS_DOFALPHA_DOFALPHA_H
#define PLUS_DOFALPHA_DOFALPHA_H

#include "stdtypes.h"

class LLGLSLShader;

// The <plus> hooks in LLDrawPoolAlpha (lldrawpoolalpha.cpp). Main thread.
//
// Depth of field (LLPipeline::renderDoF) works out each pixel's blur from the
// one depth in the depth buffer. Blended alpha writes none while it is drawn,
// so the alpha pool draws it once more at its end, depth only, for depth of
// field: upstream meant "only where the surface is at least a third opaque"
// (setMinimumAlpha(0.33f)), but only the plain alpha program discards below
// that. Legacy materials (normal or specular map) never discard when blended,
// and PBR blend materials neither (minimum alpha -1): every pixel of their
// triangles, a hair's clear card edges too, took the surface's depth, so the
// background seen through them was not blurred. With PlusDoFAlpha on, those
// programs discard below the same cutoff in that pass (the uniform
// plus_dof_alpha_cutoff in materialF.glsl and pbralphaF.glsl; 0, the colour
// passes, changes nothing), and skip their lighting there (the pass writes no
// colour).
namespace plus::dofalpha
{
    // The opacity below which the depth pass writes no depth: PlusDoFAlphaCutoff
    // with PlusDoFAlpha on, upstream's 0.33 with it off.
    F32 depthCutoff();

    // Around the depth pass (both after the OIT resolve, if any). Begin sets
    // the cutoff on the legacy material and PBR blend programs (and their
    // rigged variants) while PlusDoFAlpha is on; end sets it back to 0. The
    // bound program is the same after each as before.
    // dof: the pass is depth of field's (not only volumetric light's, which
    // wants the alpha's depth as it was). With depth of field on, the pass
    // is shared: the glass lets the shafts through too, also in the volumetric
    // light's comparison mode (PlusVolumetricAlpha 2), which reads this depth.
    void beginDepthPass(bool dof);
    void endDepthPass();

    // Between the two: the pass is drawn with the cutoff. Its draws then keep
    // the cutoff whatever their blend function (upstream drops the minimum
    // alpha to 0 for draws with a custom one, and back to 1/255 after them).
    bool depthPassActive();

    // Focus through glass (plus::focuslock::throughGlass(), research/218): in
    // depth of field's depth pass, the world's alpha (not an avatar's, nor an
    // attachment's) writes no depth where it is less opaque than
    // plus::focuslock::SEE_THROUGH_OPACITY, the texture's alpha times the
    // colour's (a window pane's transparency is its colour's alpha, which the
    // plain alpha program's minimum alpha does not look at). Then a face seen
    // through glass is blurred by its own distance, not the glass's.
    // glassActive(): between begin and end, the pass does this.
    // batchCutoff(): before each of the pass's draws, with the bound program
    // and whether the batch is the world's; sets that program's cutoff
    // uniform (plus_dof_alpha_cutoff, also in fullbrightF.glsl's alpha mask
    // variants) when it changed. Programs other than the plain alpha mask,
    // the legacy material blend and the PBR blend ones are left as they are.
    bool glassActive();
    void batchCutoff(LLGLSLShader* program, bool world);
}

#endif // PLUS_DOFALPHA_DOFALPHA_H
