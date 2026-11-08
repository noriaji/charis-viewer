/**
 * @file gtao.h
 * @brief Ground-truth ambient occlusion (GTAO) and contact shadows, written
 *        into the deferred light map (research/92).
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

#ifndef PLUS_GTAO_H
#define PLUS_GTAO_H

namespace plus::gtao
{
    // The <plus> hook in LLPipeline::renderDeferredLighting(), after the sun
    // program has painted the light map (mRT->deferredLight) and before the
    // official blur softens it: GTAO replaces the light map's g (ambient
    // occlusion), contact shadows lower its r (the sun's shadow). Returns at
    // once when both are off (PlusGtao, PlusContactShadows: the default), the
    // backend cannot draw them (GL, or LLRHI_GTAO=0), or this is not the main
    // view (reflection probes, mirrors). Main thread.
    void render();

    // The <plus> hook where renderDeferredLighting() picks the sun program for
    // the main view: true when this frame's GTAO will replace the light map's g
    // on every pixel, so the official SSAO the sun program would put there is
    // thrown away. The pipeline then draws the sun program without SSAO
    // (gDeferredSunProbeProgram: sunLightF.glsl, the same r, b and a, g = 1)
    // and the picture is the same, without the official SSAO's cost
    // (research/125 #6). Makes the GTAO targets first: false when they cannot
    // be made, so that frame keeps the official SSAO. Called once a frame, just
    // before render(), which checks GTAO was in fact drawn; a frame where it
    // could not be leaves g at 1, and the official SSAO is kept for a while
    // (300 frames, doubling while it keeps failing) before trying again. Main thread.
    bool replacesAmbientOcclusion();

    // Why GTAO (or SSGI, plus/ssgi) cannot be used now, whatever its own
    // settings say: the name of a text of panel_plus_gtao_reasons.xml (en, ja),
    // shown on the greyed-out rows (gtao_ui.h). Inline, so each has one
    // address: a row compares the pointer to rewrite its tool tip only when
    // the reason changes.
    namespace reason
    {
        inline constexpr char SAFE_MODE[]          = "reason_safe_mode";          // LLRHI_GTAO / LLRHI_SSGI=0 from safe mode
        inline constexpr char GTAO_SWITCHED_OFF[]  = "reason_gtao_switched_off";  // LLRHI_GTAO=0 (a switch for tests)
        inline constexpr char SSGI_SWITCHED_OFF[]  = "reason_ssgi_switched_off";  // LLRHI_SSGI=0
        inline constexpr char NO_BACKEND[]         = "reason_no_backend";         // GL, or the backend did not install its passes
        inline constexpr char SSAO_OFF[]           = "reason_ssao_off";           // RenderDeferredSSAO
        inline constexpr char SSAO_UNSUPPORTED[]   = "reason_ssao_unsupported";   // the feature table has no SSAO for this GPU
        inline constexpr char GTAO_OFF[]           = "reason_gtao_off";           // PlusGtao (SSGI's)
        inline constexpr char GTAO_STRENGTH_ZERO[] = "reason_gtao_strength_zero"; // PlusGtaoStrength 0 (SSGI's)
        inline constexpr char HDR_OFF[]            = "reason_hdr_off";            // RenderHDREnabled (SSGI's)
        inline constexpr char PROBES_OFF[]         = "reason_probes_off";         // RenderReflectionsEnabled (SSGI's)
        inline constexpr char PROBES_UNSUPPORTED[] = "reason_probes_unsupported"; // the feature table has no probes for this GPU
    }

    // What GTAO needs besides its own settings (PlusGtao and the rest): the
    // backend (Vulkan, LLRHI_GTAO not 0: safe mode sets it to 0) and the
    // official SSAO (RenderDeferredSSAO), as render() checks them.
    // unavailableReason(): nullptr when available(), else one of reason::.
    // GTAO's strength at 0 is not one: it is a row of GTAO's own, which must
    // stay usable to be raised again (SSGI's rows are greyed out for it).
    // Cheap (no setting looked up by name; the feature table once a session,
    // set at start). Main thread.
    bool available();
    const char* unavailableReason();
}

#endif // PLUS_GTAO_H
