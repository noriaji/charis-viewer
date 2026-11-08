/**
 * @file shadowpenumbra.h
 * @brief A penumbra that grows with the distance from the caster in the
 *        sun's shadows (research/187 in the sl-viewer repository).
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

#ifndef PLUS_SHADOWPENUMBRA_SHADOWPENUMBRA_H
#define PLUS_SHADOWPENUMBRA_SHADOWPENUMBRA_H

class LLGLSLShader;

// PlusShadowPenumbra (0 off, the default): how many times the penumbra the
// sun's disc (about 0.53 degrees) makes, 1 being the physical one. In the two
// nearest sun cascades of the main view (the sun light pass of
// renderDeferredLighting), a pixel's shadow is read with a filter as wide as
// that penumbra at the distance of what shadows it, found with a few taps of
// the shadow map read as depth: sharp at a caster's foot, softer the farther
// it falls. Needs the sun shadows and the stable orthographic cascades
// (PlusShadowStabilize), whose depth is a length along the light. Probes,
// mirrors, translucent surfaces and water read the shadows as before.
// Snapshots use more taps. The shader part is shadowUtil.glsl (PLUS_PENUMBRA).
namespace plus::shadowpenumbra
{
    // In the sun light pass, once the shader is bound: sets its uniforms (the
    // penumbra, or 0 for the official read) and binds the two nearest cascades
    // as depth. The sun shader is also the probe's, so it is called for every
    // sun pass.
    void bind(LLGLSLShader& shader);

    // After that pass: lets go of the units the depth was bound to.
    void unbind(LLGLSLShader& shader);
}

#endif // PLUS_SHADOWPENUMBRA_SHADOWPENUMBRA_H
