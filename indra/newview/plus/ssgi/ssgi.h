/**
 * @file ssgi.h
 * @brief Screen space indirect light (SSGI): GTAO's occlusion widened into a
 *        visibility bitmask that also gathers the light near occluders send
 *        back (research/164).
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

#ifndef PLUS_SSGI_H
#define PLUS_SSGI_H

#include "stdtypes.h"

class LLGLSLShader;

namespace LLRHI
{
    struct GtaoOcclusion;
}

// Everything returns at once while the feature is off (PlusSsgi 0, the
// default), while available() is false (GL or LLRHI_SSGI=0, without HDR, the
// official SSAO, GTAO (PlusGtao) or reflection probes), and for reflection
// probe faces and mirrors. Main thread.
namespace plus::ssgi
{
    // What SSGI needs besides its own settings (PlusSsgi, its strength): the
    // backend (Vulkan, LLRHI_SSGI not 0: safe mode sets it to 0), what GTAO
    // needs (plus::gtao::unavailableReason()), GTAO itself (PlusGtao, its
    // strength above 0), HDR and the reflection probes; the same checks as the
    // passes make. unavailableReason(): nullptr when available(), else one of
    // plus::gtao::reason:: (the first missing, in the order the user would
    // fix them), for the greyed-out rows' tool tips (plus/gtao/gtao_ui.h).
    // Cheap (the feature table is looked up by name once a session). Main thread.
    bool available();
    const char* unavailableReason();

    // plus/gtao, for the main view: whether this frame's occlusion pass is
    // SSGI's (in place of GTAO's own occlusion and denoise passes).
    bool wanted();

    // plus/gtao: SSGI's occlusion targets at width x height (GTAO's reduced
    // grid); false when they cannot be made (GTAO's own are used instead).
    bool sizeTargets(U32 width, U32 height);

    // plus/gtao, after GtaoDepth, in place of GtaoOcclusion and GtaoDenoise:
    // the last frame's light moved into this frame, the bitmask's occlusion
    // and indirect light, their 4x4 average. Returns the GL name of the
    // averaged visibility (RG16F, as GtaoDenoise's target) for GtaoApply,
    // 0 when a pass could not be drawn (SSGI then waits a while and GTAO
    // draws as before).
    U32 renderOcclusion(const LLRHI::GtaoOcclusion& occlusion);

    // The <plus> hook in LLPipeline::renderDeferredLighting() after the
    // opaque lighting (the skin hooks), with the lit screen bound: the lit
    // opaque pixels kept at half size for the next frame's light, then
    // PlusSsgiDebug's views. Releases everything while the feature is off.
    void capture();

    // The <plus> hook at the end of LLPipeline::bindReflectionProbes(): on
    // the main view's lighting program (softenLightF.glsl), plusSsgiMap is
    // this frame's indirect light (plusSsgiParams.x its strength); every
    // other program gets plusSsgiParams.x 0 (nothing is added).
    void bind(LLGLSLShader& shader);
}

#endif // PLUS_SSGI_H
